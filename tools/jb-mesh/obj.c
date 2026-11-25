/*
 * jb-mesh's OBJ reader (mesh.h), our own: Wavefront OBJ with its MTL
 * materials, the parts a mesh for jb_mesh needs.
 *
 *   v x y z [r g b]   a position, with an optional vertex colour (0..1)
 *   vt u [v [w]]      texture coordinates; v is turned over (1 - v), since
 *                     OBJ's (0, 0) is the image's bottom-left
 *   vn x y z          a normal
 *   f a b c ...       a face of 3 or more corners, each v, v/vt, v//vn or
 *                     v/vt/vn, indices from 1 or negative (from the end);
 *                     3 or 4 corners are kept as a triangle or quad, more
 *                     (up to MAX_POLYGON) are cut into triangles by ear
 *                     clipping (ear_clip)
 *   usemtl name       the material of the faces after it ("default" before
 *                     any)
 *   mtllib file...    MTL files beside the OBJ: newmtl, Kd (the colour),
 *                     d (dissolve: below 1 blends, JB_BLEND_HALF), Tr (1 - d),
 *                     map_Kd (noted: jb-mesh needs --texture for it)
 * Comments (#), blank lines and other statements (o, g, s, l, p, ...) are
 * ignored; a backslash at the end of a line joins the next.
 */
#include "mesh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct vec_array {
    double *v; /* stride doubles each */
    size_t n, cap;
    int stride;
} vec_array;

static int push(vec_array *a, const double *x)
{
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 1024;
        double *g = realloc(a->v, cap * (size_t)a->stride * sizeof *g);
        if (g == NULL)
            return -1;
        a->v = g;
        a->cap = cap;
    }
    memcpy(a->v + a->n * (size_t)a->stride, x, (size_t)a->stride * sizeof *x);
    a->n++;
    return 0;
}

/* A whole line, with backslash continuations joined, into a buffer that
   grows; NULL at the end of the file (or with no memory). *line counts
   the lines begun, so it is the number of the line's last part. */
static char *read_line(FILE *f, char **buf, size_t *cap, long *line)
{
    size_t n = 0;
    int c, any = 0;

    for (;;) {
        c = getc(f);
        if (c == EOF)
            break;
        if (!any)
            ++*line;
        any = 1;
        if (c == '\n') {
            /* A backslash before the end, LF or CRLF, joins the next. */
            size_t e = n > 0 && (*buf)[n - 1] == '\r' ? n - 1 : n;
            if (e > 0 && (*buf)[e - 1] == '\\') {
                n = e - 1;
                ++*line;
                continue;
            }
            break;
        }
        if (n + 2 > *cap) {
            size_t cap2 = *cap ? *cap * 2 : 256;
            char *g = realloc(*buf, cap2);
            if (g == NULL)
                return NULL;
            *buf = g;
            *cap = cap2;
        }
        (*buf)[n++] = (char)c;
    }
    if (!any)
        return NULL;
    if (*cap == 0) {
        *buf = malloc(1);
        if (*buf == NULL)
            return NULL;
        *cap = 1;
    }
    (*buf)[n] = '\0';
    if (n > 0 && (*buf)[n - 1] == '\r')
        (*buf)[n - 1] = '\0';
    return *buf;
}

static const char *skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* The keyword at s: 1 if it is kw followed by space or the end. */
static int is(const char *s, const char *kw)
{
    size_t n = strlen(kw);
    return strncmp(s, kw, n) == 0 && (s[n] == ' ' || s[n] == '\t' || s[n] == '\0');
}

/* Up to max numbers after the keyword; returns how many, or -1 for text
   that is not a number. */
static int numbers(const char *s, double *out, int max)
{
    int n = 0;

    for (;;) {
        const char *end;
        s = skip_space(s);
        if (*s == '\0' || *s == '#')
            return n;
        if (n == max || jbm_parse_number(s, &end, &out[n]) != 0 ||
            (*end != ' ' && *end != '\t' && *end != '\0'))
            return -1;
        n++;
        s = end;
    }
}

/* At s[i] (after the keyword and its space), a # with space or tab before
   it and space, tab or the end after it: a comment after a name. A #
   within a name ("mat#1", "Material #1") is the name's. */
static int comment_at(const char *s, size_t i)
{
    return s[i] == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') &&
           (s[i + 1] == ' ' || s[i + 1] == '\t' || s[i + 1] == '\0');
}

/* The rest of the line after the keyword, without surrounding space or a
   comment (comment_at). */
static void rest(const char *s, char *out, size_t n)
{
    size_t len;

    s = skip_space(s);
    for (len = 0; s[len] != '\0' && !comment_at(s, len); len++)
        ;
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
        len--;
    if (len >= n)
        len = n - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

static void dir_of(const char *path, char *out, size_t n)
{
    const char *slash = strrchr(path, '/');
    size_t len = slash ? (size_t)(slash - path + 1) : 0;

    if (len >= n)
        len = n - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static int read_mtl(const char *path, jbm_scene *s)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL, name[128];
    size_t cap = 0;
    long line = 0;
    int cur = -1, rc = 0;
    const char *l;

    if (f == NULL) {
        fprintf(stderr, "jb-mesh: warning: %s: cannot open it; its materials are white\n", path);
        return 0;
    }
    while ((l = read_line(f, &buf, &cap, &line)) != NULL) {
        double v[3];
        int n;
        l = skip_space(l);
        if (is(l, "newmtl")) {
            rest(l + 6, name, sizeof name);
            if (name[0] == '\0') {
                fprintf(stderr, "jb-mesh: %s:%ld: newmtl needs a name\n", path, line);
                rc = -1;
                break;
            }
            cur = jbm_add_material(s, name);
            if (cur < 0) {
                fprintf(stderr, "jb-mesh: %s:%ld: a material name too long or no memory\n",
                        path, line);
                rc = -1;
                break;
            }
        } else if (cur >= 0 && is(l, "Kd")) {
            n = numbers(l + 2, v, 3);
            if (n != 3) {
                fprintf(stderr, "jb-mesh: %s:%ld: Kd needs three numbers\n", path, line);
                rc = -1;
                break;
            }
            memcpy(s->materials[cur].color, v, sizeof v);
        } else if (cur >= 0 && (is(l, "d") || is(l, "Tr"))) {
            int tr = l[0] == 'T';
            n = numbers(l + (tr ? 2 : 1), v, 1);
            if (n != 1) {
                fprintf(stderr, "jb-mesh: %s:%ld: %s needs a number\n", path, line,
                        tr ? "Tr" : "d");
                rc = -1;
                break;
            }
            s->materials[cur].blend = (tr ? 1.0 - v[0] : v[0]) < 1.0 ? 1 : 0;
        } else if (cur >= 0 && is(l, "map_Kd")) {
            s->materials[cur].had_texture = 1;
        }
    }
    free(buf);
    fclose(f);
    return rc;
}

/* The most corners of a face that is cut into triangles: ear clipping takes
   time growing with the cube of the corners at worst, so a hostile face
   is refused rather than taking hours. */
enum { MAX_POLYGON = 1024 };

/* What ear_clip works in, MAX_POLYGON corners' worth. */
typedef struct ear_work {
    double u[MAX_POLYGON], v[MAX_POLYGON]; /* the corners in the face's plane */
    int prev[MAX_POLYGON], next[MAX_POLYGON];
    unsigned char reflex[MAX_POLYGON]; /* not a strictly convex corner */
} ear_work;

/* Twice the signed area of the triangle i, j, k in w's plane: positive
   for anticlockwise. */
static double orient(const ear_work *w, int i, int j, int k)
{
    return (w->u[j] - w->u[i]) * (w->v[k] - w->v[i]) -
           (w->v[j] - w->v[i]) * (w->u[k] - w->u[i]);
}

/* The corner at j is an ear: strictly convex, and no other reflex corner
   (in a simple polygon only those can be within it) inside or on the
   edges of the triangle it makes with its neighbours, but for those at the
   triangle's own corners' positions (where a polygon touches itself). */
static int is_ear(const ear_work *w, int j)
{
    int a = w->prev[j], c = w->next[j], k;

    if (w->reflex[j])
        return 0;
    for (k = w->next[c]; k != a; k = w->next[k]) {
        if (!w->reflex[k] || (w->u[k] == w->u[a] && w->v[k] == w->v[a]) ||
            (w->u[k] == w->u[j] && w->v[k] == w->v[j]) ||
            (w->u[k] == w->u[c] && w->v[k] == w->v[c]))
            continue;
        if (orient(w, a, j, k) >= 0.0 && orient(w, j, c, k) >= 0.0 && orient(w, c, a, k) >= 0.0)
            return 0;
    }
    return 1;
}

/*
 * The n corners (5..MAX_POLYGON) of a face as n - 2 triangles, into tri
 * (3 (n - 2) corner indices, each triangle going round as the face does),
 * by ear clipping in the plane the face's Newell normal is most nearly
 * along. The candidates go round the face from corner 1, and after each
 * ear from the corner after it, so a convex face (every candidate an ear)
 * is cut as a fan from corner 0, as jb-mesh cut every face before. Double
 * operations only (-ffp-contract=off), so the same everywhere. Returns 0,
 * -1 for a face with no area, or -2 for one with no ear left (one that
 * crosses itself or doubles back).
 */
static int ear_clip(const jbm_corner *poly, int n, ear_work *w, int *tri)
{
    double nn[3] = {0.0, 0.0, 0.0}, most = 0.0;
    int i, axis = 0, au, av, cur = 1, tries = 0, left = n, t = 0;

    for (i = 0; i < n; i++) {
        const double *p = poly[i].p, *q = poly[(i + 1) % n].p, *o = poly[0].p;
        double px = p[0] - o[0], py = p[1] - o[1], pz = p[2] - o[2];
        double qx = q[0] - o[0], qy = q[1] - o[1], qz = q[2] - o[2];
        nn[0] += (py - qy) * (pz + qz);
        nn[1] += (pz - qz) * (px + qx);
        nn[2] += (px - qx) * (py + qy);
    }
    for (i = 0; i < 3; i++) {
        double m = nn[i] < 0.0 ? -nn[i] : nn[i];
        if (m - m != 0.0) /* infinite or NaN: no plane to cut it in */
            return -1;
        if (m > most)
            most = m, axis = i;
    }
    if (!(most > 0.0))
        return -1;
    /* The other two axes in turn after it (x, y round z), swapped when the
       face looks down the axis, so the face goes anticlockwise. */
    au = (axis + 1) % 3, av = (axis + 2) % 3;
    if (nn[axis] < 0.0)
        i = au, au = av, av = i;
    for (i = 0; i < n; i++) {
        w->u[i] = poly[i].p[au] - poly[0].p[au];
        w->v[i] = poly[i].p[av] - poly[0].p[av];
        w->prev[i] = (i + n - 1) % n;
        w->next[i] = (i + 1) % n;
    }
    for (i = 0; i < n; i++)
        w->reflex[i] = !(orient(w, w->prev[i], i, w->next[i]) > 0.0);
    while (left > 3) {
        if (is_ear(w, cur)) {
            int a = w->prev[cur], c = w->next[cur];
            tri[t++] = a, tri[t++] = cur, tri[t++] = c;
            w->next[a] = c, w->prev[c] = a;
            left--;
            w->reflex[a] = !(orient(w, w->prev[a], a, c) > 0.0);
            w->reflex[c] = !(orient(w, a, c, w->next[c]) > 0.0);
            cur = c, tries = 0;
        } else {
            cur = w->next[cur];
            if (++tries == left)
                return -2;
        }
    }
    if (!(orient(w, w->prev[cur], cur, w->next[cur]) > 0.0))
        return -2;
    tri[t++] = w->prev[cur], tri[t++] = cur, tri[t++] = w->next[cur];
    return 0;
}

/* One corner "v", "v/vt", "v//vn" or "v/vt/vn": the indices (0-based,
   -1 for none) into arrays of the sizes given; returns 0 or -1. */
static int corner(const char *tok, size_t len, size_t nv, size_t nt, size_t nn, long idx[3])
{
    size_t count[3];
    int part = 0;
    size_t i = 0;

    count[0] = nv, count[1] = nt, count[2] = nn;
    idx[0] = idx[1] = idx[2] = -1;
    while (part < 3) {
        long v = 0;
        int neg = 0, digits = 0;
        if (i < len && tok[i] == '-')
            neg = 1, i++;
        while (i < len && tok[i] >= '0' && tok[i] <= '9') {
            if (v < 100000000)
                v = v * 10 + (tok[i] - '0');
            i++, digits++;
        }
        if (digits > 0) {
            if (v == 0 || (size_t)v > count[part])
                return -1;
            idx[part] = neg ? (long)count[part] - v : v - 1;
        } else if (part == 0 || neg) {
            return -1;
        }
        if (i == len)
            break;
        if (tok[i] != '/')
            return -1;
        i++;
        part++;
    }
    return i == len && part < 3 ? 0 : -1;
}

int jbm_read_obj(const char *path, jbm_scene *s)
{
    FILE *f = fopen(path, "rb");
    vec_array pos = {NULL, 0, 0, 6}, tex = {NULL, 0, 0, 2}, nrm = {NULL, 0, 0, 3};
    char *buf = NULL, dir[1024], name[1024];
    size_t cap = 0;
    long line = 0;
    int material = -1, rc = 0;
    const char *l;
    jbm_corner *poly = NULL;
    size_t poly_cap = 0;
    ear_work *ears = NULL;
    int *tri = NULL;

    if (f == NULL) {
        fprintf(stderr, "jb-mesh: %s: cannot open it\n", path);
        return -1;
    }
    dir_of(path, dir, sizeof dir);
#define ERR(...)                                                                              \
    do {                                                                                      \
        fprintf(stderr, "jb-mesh: %s:%ld: ", path, line);                                     \
        fprintf(stderr, __VA_ARGS__);                                                         \
        fprintf(stderr, "\n");                                                                \
        rc = -1;                                                                              \
        goto done;                                                                            \
    } while (0)
    while ((l = read_line(f, &buf, &cap, &line)) != NULL) {
        double v[6];
        int n;
        l = skip_space(l);
        if (*l == '#' || *l == '\0')
            continue;
        if (is(l, "v")) {
            n = numbers(l + 1, v, 6);
            if (n != 3 && n != 4 && n != 6)
                ERR("v needs x y z, x y z w, or x y z r g b");
            if (n == 4)
                n = 3; /* w is for rational curves; ignored */
            if (n == 3)
                v[3] = v[4] = v[5] = -1.0; /* no colour */
            if (push(&pos, v) != 0)
                ERR("no memory");
        } else if (is(l, "vt")) {
            n = numbers(l + 2, v, 3);
            if (n < 1)
                ERR("vt needs u [v [w]]");
            if (n == 1)
                v[1] = 0.0;
            v[1] = 1.0 - v[1];
            if (push(&tex, v) != 0)
                ERR("no memory");
        } else if (is(l, "vn")) {
            if (numbers(l + 2, v, 3) != 3)
                ERR("vn needs x y z");
            if (push(&nrm, v) != 0)
                ERR("no memory");
        } else if (is(l, "f")) {
            const char *t = l + 1;
            size_t count = 0, k;
            if (material < 0) {
                material = jbm_add_material(s, "default");
                if (material < 0)
                    ERR("no memory");
            }
            for (;;) {
                const char *e;
                long idx[3];
                jbm_corner c;
                t = skip_space(t);
                if (*t == '\0' || *t == '#')
                    break;
                for (e = t; *e != '\0' && *e != ' ' && *e != '\t'; e++)
                    ;
                if (count == MAX_POLYGON)
                    ERR("a face of more than %d corners", MAX_POLYGON);
                if (corner(t, (size_t)(e - t), pos.n, tex.n, nrm.n, idx) != 0)
                    ERR("face corner \"%.*s\" is not v, v/vt, v//vn or v/vt/vn with indices "
                        "of vertices there are",
                        (int)(e - t), t);
                memset(&c, 0, sizeof c);
                memcpy(c.p, pos.v + (size_t)idx[0] * 6, 3 * sizeof(double));
                if (pos.v[(size_t)idx[0] * 6 + 3] >= 0.0) {
                    memcpy(c.c, pos.v + (size_t)idx[0] * 6 + 3, 3 * sizeof(double));
                    c.has_c = 1;
                }
                if (idx[1] >= 0) {
                    memcpy(c.uv, tex.v + (size_t)idx[1] * 2, 2 * sizeof(double));
                    c.has_uv = 1;
                }
                if (idx[2] >= 0) {
                    memcpy(c.n, nrm.v + (size_t)idx[2] * 3, 3 * sizeof(double));
                    c.has_n = 1;
                }
                if (count == poly_cap) {
                    size_t pc = poly_cap ? poly_cap * 2 : 16;
                    jbm_corner *g = realloc(poly, pc * sizeof *g);
                    if (g == NULL)
                        ERR("no memory");
                    poly = g;
                    poly_cap = pc;
                }
                poly[count++] = c;
                t = e;
            }
            if (count < 3)
                ERR("a face needs 3 corners or more, not %lu", (unsigned long)count);
            if (count <= 4) {
                jbm_face face;
                face.vertices = (int)count;
                face.material = material;
                memcpy(face.c, poly, count * sizeof *poly);
                if (jbm_add_face(s, &face) != 0)
                    ERR("no memory");
            } else {
                int r;
                if (ears == NULL) {
                    ears = malloc(sizeof *ears);
                    tri = malloc(3 * (MAX_POLYGON - 2) * sizeof *tri);
                    if (ears == NULL || tri == NULL)
                        ERR("no memory");
                }
                r = ear_clip(poly, (int)count, ears, tri);
                if (r == -1)
                    ERR("a face of %lu corners with no area", (unsigned long)count);
                if (r != 0)
                    ERR("a face of %lu corners that cannot be cut into triangles (does it "
                        "cross itself?)",
                        (unsigned long)count);
                for (k = 0; k + 2 < count; k++) {
                    jbm_face face;
                    face.vertices = 3;
                    face.material = material;
                    face.c[0] = poly[tri[3 * k]];
                    face.c[1] = poly[tri[3 * k + 1]];
                    face.c[2] = poly[tri[3 * k + 2]];
                    if (jbm_add_face(s, &face) != 0)
                        ERR("no memory");
                }
            }
        } else if (is(l, "usemtl")) {
            rest(l + 6, name, 128);
            if (name[0] == '\0')
                ERR("usemtl needs a name");
            material = jbm_add_material(s, name);
            if (material < 0)
                ERR("material name too long, or no memory");
        } else if (is(l, "mtllib")) {
            /* One or more file names, separated by spaces. */
            const char *t = skip_space(l + 6);
            while (*t != '\0' && !comment_at(t, 0)) {
                const char *e;
                char file[2048];
                for (e = t; *e != '\0' && *e != ' ' && *e != '\t'; e++)
                    ;
                snprintf(file, sizeof file, "%s%.*s", dir, (int)(e - t), t);
                if (read_mtl(file, s) != 0) {
                    rc = -1;
                    goto done;
                }
                t = skip_space(e);
            }
        }
    }
done:
#undef ERR
    if (rc == 0 && ferror(f)) {
        fprintf(stderr, "jb-mesh: %s: cannot read it\n", path);
        rc = -1;
    }
    fclose(f);
    free(buf);
    free(poly);
    free(ears);
    free(tri);
    free(pos.v);
    free(tex.v);
    free(nrm.v);
    return rc;
}
