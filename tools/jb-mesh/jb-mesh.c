/*
 * jb-mesh: OBJ or glTF 2.0 to a .jbm mesh (docs/formats.md). docs/tools.md,
 * "`jb-mesh`", says what it does; obj.c and gltf.c read, this file quantises and
 * writes.
 *
 *   jb-mesh [options] INPUT.obj|.gltf|.glb OUTPUT.jbm
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jitterbug.h>

#include "api/formats.h"
#include "mesh.h"
#include "stb_image.h"

/* sqrt, the one C library maths function used: correctly rounded in IEEE
   arithmetic, so the same everywhere. */
#include <math.h>

static const char usage[] =
    "usage: jb-mesh [options] INPUT.obj|INPUT.gltf|INPUT.glb OUTPUT.jbm\n"
    "  --texture MATERIAL=FILE  give MATERIAL the texture FILE (.jbt or .png), whose\n"
    "                           size its u, v are mapped to; each different FILE is a\n"
    "                           texture slot, numbered from 0 in the order given\n"
    "  --scale S                multiply positions by S (default 1) ...\n"
    "  --fit N                  ... or scale so the largest coordinate is N\n"
    "  --no-normals             leave normals out (faces are lit flat)\n"
    "  --no-colors              leave vertex colours out\n"
    "  --blend MATERIAL=MODE    none, half, add, sub or quarter\n"
    "  --unlit MATERIAL         colours as they are, without the lights\n"
    "  --lit MATERIAL           with the lights (the default)\n"
    "  --raw MATERIAL           textured faces: texels not multiplied by the colour\n"
    "  --double-sided MATERIAL  draw faces seen from behind too\n"
    "  --single-sided MATERIAL  cull them (the default)\n"
    "MATERIAL is a material's name as the file gives it (\"default\" for faces\n"
    "without one; \"#k\" for glTF material k when it has no name of its own), or *\n"
    "for every material. Options apply in order, after what the file says.\n";

static int fail(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* Prints "jb-mesh: " and the message; returns 1, the exit status. */
static int fail(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "jb-mesh: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    return 1;
}

/* A per-material option, applied in command-line order. */
enum { OPT_TEXTURE, OPT_BLEND, OPT_UNLIT, OPT_LIT, OPT_RAW, OPT_DOUBLE, OPT_SINGLE };
typedef struct option {
    int kind;
    const char *material; /* or "*" */
    int value;            /* OPT_TEXTURE: the slot; OPT_BLEND: the mode */
    int used;
} option;

typedef struct slot {
    const char *file;
    int width, height, repeat;
} slot;

enum { MAX_OPTIONS = 1024, MAX_SLOTS = 1024 };

/* What a material becomes. */
typedef struct material_out {
    int slot; /* -1 for none */
    int blend, unlit, raw, double_sided;
    int index; /* in the output, -1 until a face uses it */
} material_out;

/* ------------------------------------------------------------------------ */
/* Texture sizes */

static int read_whole(const char *path, unsigned char **data, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    size_t n = 0, cap = 0;

    if (f == NULL)
        return -1;
    for (;;) {
        size_t got;
        if (n == cap) {
            unsigned char *g;
            cap = cap ? cap * 2 : 65536;
            g = realloc(buf, cap);
            if (g == NULL) {
                free(buf);
                fclose(f);
                return -1;
            }
            buf = g;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *data = buf;
    *size = n;
    return 0;
}

/* The size (and repeat flag) of a texture file: a .jbt, read with the
   library's jb_texture_parse, or a PNG's header. */
static int texture_size(slot *s)
{
    unsigned char *data;
    size_t size;
    int n;

    if (read_whole(s->file, &data, &size) != 0)
        return fail("%s: cannot read it", s->file);
    if (size >= 4 && memcmp(data, JBI_JBT_MAGIC, 4) == 0) {
        jb_result r = JB_ERROR_INVALID_ARGUMENT;
        jb_context *jb = jb_create(NULL, &r);
        jb_texture_desc d;
        const char *why = "";
        if (jb == NULL) {
            free(data);
            return fail("%s: jb_create failed: %s", s->file, jb_result_string(r));
        }
        r = jb_texture_parse(jb, data, size, &d, NULL);
        if (r != JB_OK) {
            jb_check(jb, &why);
            fail("%s: not a texture jitterbug reads: %s", s->file, why);
            jb_destroy(jb);
            free(data);
            return 1;
        }
        s->width = d.width;
        s->height = d.height;
        s->repeat = d.repeat;
        jb_destroy(jb);
        free(data);
        return 0;
    }
    if (!stbi_info_from_memory(data, (int)(size < 0x7fffffff ? size : 0x7fffffff), &s->width,
                               &s->height, &n)) {
        free(data);
        return fail("%s: neither a .jbt nor a PNG (%s)", s->file, stbi_failure_reason());
    }
    free(data);
    if (s->width < 1 || s->width > 256 || s->height < 1 || s->height > 256)
        return fail("%s: %d x %d; a texture is 1..256 texels each way", s->file, s->width,
                    s->height);
    s->repeat = 0;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Quantising */

/* x rounded to the nearest integer, halves away from 0, into *out when it
   lies in lo..hi; 0, or -1 outside (or not a number). */
static int round_in(double x, long lo, long hi, long *out)
{
    long r;

    if (!(x > (double)lo - 1.0 && x < (double)hi + 1.0))
        return -1;
    r = (long)x; /* towards 0, exact for these magnitudes */
    if (x - (double)r >= 0.5)
        r++;
    else if ((double)r - x >= 0.5)
        r--;
    if (r < lo || r > hi)
        return -1;
    *out = r;
    return 0;
}

/* The largest whole number not above x, for x within +-2^30. */
static long floor_l(double x)
{
    long r = (long)x;

    return (double)r > x ? r - 1 : r;
}

/* A colour factor as a channel 0..255: f times k, rounded, saturated. */
static unsigned channel(double f, double k)
{
    double v = f * k;
    long r;

    if (!(v > 0.0))
        return 0;
    if (v >= 255.0)
        return 255;
    r = (long)v;
    if (v - (double)r >= 0.5)
        r++;
    return (unsigned)r;
}

/* A face's own normal, not normalised: the cross product of its first two
   edges, or for a quad of its diagonals (as jb_draw_mesh lights a face
   without normals). */
static void face_normal(const jbm_face *f, double *n)
{
    double a[3], b[3];
    int i;

    for (i = 0; i < 3; i++) {
        if (f->vertices == 4) {
            a[i] = f->c[2].p[i] - f->c[0].p[i];
            b[i] = f->c[3].p[i] - f->c[1].p[i];
        } else {
            a[i] = f->c[1].p[i] - f->c[0].p[i];
            b[i] = f->c[2].p[i] - f->c[0].p[i];
        }
    }
    n[0] = a[1] * b[2] - a[2] * b[1];
    n[1] = a[2] * b[0] - a[0] * b[2];
    n[2] = a[0] * b[1] - a[1] * b[0];
}

/* An output vertex, as the file stores it. */
typedef struct vertex {
    int16_t p[3], n[3];
    uint32_t color;
    uint8_t uv[2];
} vertex;

typedef struct vtable {
    vertex *v;
    int count;
    int *hash; /* open addressing: index + 1, 0 empty */
    size_t hash_size;
} vtable;

static uint32_t vhash(const vertex *v)
{
    uint32_t h = 2166136261u;
    int k;
    const uint32_t parts[9] = {
        (uint16_t)v->p[0], (uint16_t)v->p[1], (uint16_t)v->p[2], (uint16_t)v->n[0],
        (uint16_t)v->n[1], (uint16_t)v->n[2], v->color, v->uv[0], v->uv[1]};

    for (k = 0; k < 9; k++) {
        h ^= parts[k];
        h *= 16777619u;
    }
    return h;
}

static int same(const vertex *a, const vertex *b)
{
    return memcmp(a->p, b->p, sizeof a->p) == 0 && memcmp(a->n, b->n, sizeof a->n) == 0 &&
           a->color == b->color && a->uv[0] == b->uv[0] && a->uv[1] == b->uv[1];
}

/* The index of v in t, added at the end if new; -1 past 65536 vertices or
   without memory. */
static int vindex(vtable *t, const vertex *v)
{
    size_t i = vhash(v) & (t->hash_size - 1);

    while (t->hash[i] != 0) {
        if (same(&t->v[t->hash[i] - 1], v))
            return t->hash[i] - 1;
        i = (i + 1) & (t->hash_size - 1);
    }
    if (t->count == JBI_JBM_MAX_VERTICES)
        return -1;
    t->v[t->count] = *v;
    t->hash[i] = ++t->count;
    return t->count - 1;
}

/* ------------------------------------------------------------------------ */
/* Writing */

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)(v >> 8 & 0xff);
}

static void put32(unsigned char *p, uint32_t v)
{
    put16(p, (unsigned)(v & 0xffff));
    put16(p + 2, (unsigned)(v >> 16 & 0xffff));
}

static const char *ext(const char *path)
{
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');

    return dot != NULL && (slash == NULL || dot > slash) ? dot : "";
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static int blend_mode(const char *m)
{
    static const char *const names[] = {"none", "half", "add", "sub", "quarter"};
    int i;

    for (i = 0; i < 5; i++)
        if (strcmp(m, names[i]) == 0)
            return i;
    return -1;
}

int main(int argc, char **argv)
{
    static option opts[MAX_OPTIONS];
    static slot slots[MAX_SLOTS];
    const char *in = NULL, *out = NULL;
    int nopts = 0, nslots = 0, no_normals = 0, no_colors = 0, i, k, rc;
    double scale = 1.0, fit = 0.0;
    jbm_scene scene;
    material_out *mo = NULL;
    vtable vt;
    int *out_mat = NULL, nmat = 0, has_n = 0, has_c = 0, has_uv = 0, tris = 0, quads = 0;
    uint16_t *faces = NULL;
    size_t f;
    unsigned char *file = NULL;
    size_t off[JBI_JBM_SECTIONS], bytes[JBI_JBM_SECTIONS], size;
    FILE *fp;

    memset(&scene, 0, sizeof scene);
    memset(&vt, 0, sizeof vt);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int kind = -1;
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            fputs(usage, stdout);
            return 0;
        } else if (strcmp(a, "--no-normals") == 0) {
            no_normals = 1;
            continue;
        } else if (strcmp(a, "--no-colors") == 0) {
            no_colors = 1;
            continue;
        } else if ((strcmp(a, "--scale") == 0 || strcmp(a, "--fit") == 0) && i + 1 < argc) {
            const char *end;
            double v;
            if (jbm_parse_number(argv[i + 1], &end, &v) != 0 || *end != '\0' || !(v > 0.0))
                return fail("%s %s: not a number above 0", a, argv[i + 1]);
            if (a[2] == 's')
                scale = v, fit = 0.0;
            else
                fit = v;
            i++;
            continue;
        } else if (strcmp(a, "--texture") == 0) {
            kind = OPT_TEXTURE;
        } else if (strcmp(a, "--blend") == 0) {
            kind = OPT_BLEND;
        } else if (strcmp(a, "--unlit") == 0) {
            kind = OPT_UNLIT;
        } else if (strcmp(a, "--lit") == 0) {
            kind = OPT_LIT;
        } else if (strcmp(a, "--raw") == 0) {
            kind = OPT_RAW;
        } else if (strcmp(a, "--double-sided") == 0) {
            kind = OPT_DOUBLE;
        } else if (strcmp(a, "--single-sided") == 0) {
            kind = OPT_SINGLE;
        } else if (a[0] == '-' && a[1] != '\0') {
            fputs(usage, stderr);
            return fail("unknown option %s", a);
        } else if (in == NULL) {
            in = a;
            continue;
        } else if (out == NULL) {
            out = a;
            continue;
        } else {
            fputs(usage, stderr);
            return 1;
        }
        if (i + 1 >= argc)
            return fail("%s needs an argument", a);
        if (nopts == MAX_OPTIONS)
            return fail("more than %d material options", MAX_OPTIONS);
        {
            option *o = &opts[nopts++];
            char *arg = argv[++i], *eq = strchr(arg, '=');
            o->kind = kind;
            o->material = arg;
            o->used = 0;
            if (kind == OPT_TEXTURE || kind == OPT_BLEND) {
                if (eq == NULL || eq == arg || eq[1] == '\0')
                    return fail("%s %s: needs MATERIAL=%s", a, arg,
                                kind == OPT_TEXTURE ? "FILE" : "MODE");
                *eq = '\0';
                if (kind == OPT_BLEND) {
                    o->value = blend_mode(eq + 1);
                    if (o->value < 0)
                        return fail("%s %s=%s: the mode is none, half, add, sub or quarter", a,
                                    arg, eq + 1);
                } else {
                    for (k = 0; k < nslots && strcmp(slots[k].file, eq + 1) != 0; k++)
                        ;
                    if (k == nslots) {
                        if (nslots == MAX_SLOTS)
                            return fail("more than %d textures", MAX_SLOTS);
                        slots[nslots].file = eq + 1;
                        if (texture_size(&slots[nslots]) != 0)
                            return 1;
                        nslots++;
                    }
                    o->value = k;
                }
            }
        }
    }
    if (in == NULL || out == NULL) {
        fputs(usage, stderr);
        return 1;
    }

    /* From here every exit frees what was taken (done). */
#define FAIL(...)                                                                             \
    do {                                                                                      \
        rc = fail(__VA_ARGS__);                                                               \
        goto done;                                                                            \
    } while (0)
    /* Read. */
    if (ieq(ext(in), ".obj"))
        rc = jbm_read_obj(in, &scene);
    else if (ieq(ext(in), ".gltf") || ieq(ext(in), ".glb"))
        rc = jbm_read_gltf(in, &scene);
    else
        FAIL("%s: the input must be .obj, .gltf or .glb", in);
    if (rc != 0) {
        rc = 1;
        goto done;
    }
    if (scene.face_count == 0)
        FAIL("%s: no faces", in);
    if (scene.face_count > JBI_JBM_MAX_FACES)
        FAIL("%s: %lu faces; a mesh has at most %d", in, (unsigned long)scene.face_count,
                    JBI_JBM_MAX_FACES);

    /* The materials, with the options over what the file says. */
    mo = calloc((size_t)scene.material_count, sizeof *mo);
    out_mat = calloc((size_t)scene.material_count, sizeof *out_mat);
    if (mo == NULL || out_mat == NULL)
        FAIL("out of memory");
    for (i = 0; i < scene.material_count; i++) {
        const jbm_material *m = &scene.materials[i];
        mo[i].slot = -1;
        mo[i].blend = m->blend;
        mo[i].unlit = m->unlit;
        mo[i].double_sided = m->double_sided;
        mo[i].index = -1;
        for (k = 0; k < nopts; k++) {
            option *o = &opts[k];
            if (strcmp(o->material, "*") != 0 && strcmp(o->material, m->name) != 0)
                continue;
            o->used = 1;
            switch (o->kind) {
            case OPT_TEXTURE: mo[i].slot = o->value; break;
            case OPT_BLEND: mo[i].blend = o->value; break;
            case OPT_UNLIT: mo[i].unlit = 1; break;
            case OPT_LIT: mo[i].unlit = 0; break;
            case OPT_RAW: mo[i].raw = 1; break;
            case OPT_DOUBLE: mo[i].double_sided = 1; break;
            case OPT_SINGLE: mo[i].double_sided = 0; break;
            }
        }
    }
    for (k = 0; k < nopts; k++)
        if (!opts[k].used) {
            fprintf(stderr, "jb-mesh: no material is named \"%s\"; the materials are:",
                    opts[k].material);
            for (i = 0; i < scene.material_count; i++)
                fprintf(stderr, " \"%s\"", scene.materials[i].name);
            fprintf(stderr, "\n");
            rc = 1;
            goto done;
        }
    for (i = 0; i < scene.material_count; i++)
        if (scene.materials[i].had_texture && mo[i].slot < 0)
            fprintf(stderr,
                    "jb-mesh: warning: material \"%s\" has a texture in %s, but no --texture; "
                    "it is untextured\n",
                    scene.materials[i].name, in);

    /* What every vertex has. */
    for (f = 0; f < scene.face_count; f++) {
        const jbm_face *fc = &scene.faces[f];
        for (k = 0; k < fc->vertices; k++) {
            has_n |= fc->c[k].has_n;
            has_c |= fc->c[k].has_c;
        }
        has_uv |= mo[fc->material].slot >= 0;
    }
    has_n = has_n && !no_normals;
    has_c = has_c && !no_colors;
    if (fit > 0.0) {
        double most = 0.0;
        for (f = 0; f < scene.face_count; f++)
            for (k = 0; k < scene.faces[f].vertices; k++)
                for (i = 0; i < 3; i++) {
                    double v = scene.faces[f].c[k].p[i];
                    v = v < 0.0 ? -v : v;
                    most = v > most ? v : most;
                }
        scale = most > 0.0 ? fit / most : 1.0;
    }

    /* Quantise, face by face. */
    vt.count = 0;
    vt.hash_size = 1;
    while (vt.hash_size < 2u * JBI_JBM_MAX_VERTICES)
        vt.hash_size *= 2;
    vt.v = calloc(JBI_JBM_MAX_VERTICES, sizeof *vt.v);
    vt.hash = calloc(vt.hash_size, sizeof *vt.hash);
    faces = calloc(scene.face_count * 6, sizeof *faces);
    if (vt.v == NULL || vt.hash == NULL || faces == NULL)
        FAIL("out of memory");
    for (f = 0; f < scene.face_count; f++) {
        const jbm_face *fc = &scene.faces[f];
        const jbm_material *m = &scene.materials[fc->material];
        const material_out *o = &mo[fc->material];
        const slot *s = o->slot >= 0 ? &slots[o->slot] : NULL;
        double ck = s != NULL ? 128.0 : 255.0, shift[2] = {0.0, 0.0};
        /* A repeat texture wraps u, v within it, so the face's may move by
           whole repeats: by those that bring its smallest into the first
           (its corners are vertices of their own where they differ), up to
           half a texel before it, as the edge, the same tolerance as
           without repeat. */
        if (s != NULL && s->repeat) {
            int tsize[2], a, any = 0;
            double lo[2] = {0.0, 0.0};
            tsize[0] = s->width, tsize[1] = s->height;
            for (k = 0; k < fc->vertices; k++)
                if (fc->c[k].has_uv) {
                    for (a = 0; a < 2; a++) {
                        double t = fc->c[k].uv[a] * tsize[a];
                        lo[a] = !any || t < lo[a] ? t : lo[a];
                    }
                    any = 1;
                }
            for (a = 0; any && a < 2; a++) {
                double r = (lo[a] + 0.5) / tsize[a];
                if (!(r > -1e9 && r < 1e9))
                    FAIL("%s: face %lu: %c = %g is too far from the texture", in,
                                (unsigned long)f, "uv"[a], lo[a] / tsize[a]);
                shift[a] = (double)floor_l(r) * tsize[a];
            }
        }
        for (k = 0; k < fc->vertices; k++) {
            const jbm_corner *c = &fc->c[k];
            vertex v;
            long q;
            int a, idx;
            memset(&v, 0, sizeof v);
            for (a = 0; a < 3; a++) {
                if (round_in(c->p[a] * scale, -32768, 32767, &q) != 0)
                    FAIL("%s: face %lu, corner %d: %c = %g times the scale %g is outside "
                                "the int16 positions -32768..32767; give a smaller --scale or "
                                "--fit",
                                in, (unsigned long)f, k, "xyz"[a], c->p[a], scale);
                v.p[a] = (int16_t)q;
            }
            if (has_n) {
                /* The corner's normal; for a corner without one (when
                   others have), or with one of no length, its face's. */
                double nn[3], len = 0.0;
                if (c->has_n) {
                    memcpy(nn, c->n, sizeof nn);
                    len = sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                }
                if (!(len > 0.0) || len - len != 0.0) {
                    face_normal(fc, nn);
                    len = sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                }
                if (!(len > 0.0) || len - len != 0.0) {
                    /* A face with no area: up, as good as any. */
                    nn[0] = 0.0, nn[1] = 1.0, nn[2] = 0.0;
                    len = 1.0;
                }
                for (a = 0; a < 3; a++) {
                    if (round_in(nn[a] / len * 4096.0, -4096, 4096, &q) != 0)
                        q = nn[a] < 0.0 ? -4096 : 4096;
                    v.n[a] = (int16_t)q;
                }
            }
            if (has_c) {
                unsigned ch[3];
                for (a = 0; a < 3; a++)
                    ch[a] = channel((c->has_c ? c->c[a] : 1.0) * m->color[a], ck);
                v.color = JB_RGB(ch[0], ch[1], ch[2]);
            }
            if (s != NULL) {
                int tsize[2], lim[2];
                tsize[0] = s->width, tsize[1] = s->height;
                if (!c->has_uv)
                    FAIL("%s: face %lu, corner %d has no texture coordinates, but its "
                                "material \"%s\" is textured",
                                in, (unsigned long)f, k, m->name);
                for (a = 0; a < 2; a++) {
                    double t = c->uv[a] * tsize[a] - shift[a];
                    lim[a] = s->repeat ? 256 : tsize[a];
                    if (!(t >= -0.5 && t <= lim[a] + 0.5))
                        FAIL("%s: face %lu, corner %d: %c = %g is outside the %s%d x %d "
                                    "texture %s%s",
                                    in, (unsigned long)f, k, "uv"[a], c->uv[a],
                                    s->repeat ? "repeat " : "", s->width, s->height, s->file,
                                    s->repeat ? " (a face's u, v may span 256 texels, its "
                                                "page)"
                                              : " (0..1; a .jbt made with --repeat may tile)");
                    q = floor_l(t);
                    q = q < 0 ? 0 : q > lim[a] - 1 ? lim[a] - 1 : q;
                    v.uv[a] = (uint8_t)q;
                }
            }
            idx = vindex(&vt, &v);
            if (idx < 0)
                FAIL("%s: more than %d different vertices", in, JBI_JBM_MAX_VERTICES);
            faces[f * 6 + (size_t)k] = (uint16_t)idx;
        }
        faces[f * 6 + 4] = (uint16_t)fc->vertices;
        if (o->index < 0) {
            if (nmat == JBI_JBM_MAX_MATERIALS)
                FAIL("%s: more than %d materials", in, JBI_JBM_MAX_MATERIALS);
            mo[fc->material].index = nmat;
            out_mat[nmat++] = fc->material;
        }
        faces[f * 6 + 5] = (uint16_t)mo[fc->material].index;
        if (fc->vertices == 3)
            tris++;
        else
            quads++;
    }

    /* Lay out and write. */
    bytes[JBI_JBM_S_POSITIONS] = (size_t)vt.count * JBI_JBM_POSITION;
    bytes[JBI_JBM_S_NORMALS] = has_n ? (size_t)vt.count * JBI_JBM_NORMAL : 0;
    bytes[JBI_JBM_S_COLORS] = has_c ? (size_t)vt.count * JBI_JBM_COLOR : 0;
    bytes[JBI_JBM_S_UV] = has_uv ? (size_t)vt.count * JBI_JBM_UVREC : 0;
    bytes[JBI_JBM_S_FACES] = scene.face_count * JBI_JBM_FACE;
    bytes[JBI_JBM_S_MATERIALS] = (size_t)nmat * JBI_JBM_MATERIAL;
    bytes[JBI_JBM_S_SLOTS] = (size_t)nslots * JBI_JBM_SLOT;
    size = JBI_JBM_HEADER;
    for (k = 0; k < JBI_JBM_SECTIONS; k++) {
        size = (size + 3) / 4 * 4;
        off[k] = bytes[k] > 0 ? size : 0;
        size += bytes[k];
    }
    if (size > 0xffffffffu)
        FAIL("%s: the mesh would be more than 4 GB", in);
    file = calloc(size, 1);
    if (file == NULL)
        FAIL("out of memory");
    memcpy(file + JBI_JBM_O_MAGIC, JBI_JBM_MAGIC, 4);
    put32(file + JBI_JBM_O_VERSION, JBI_JBM_VERSION);
    put32(file + JBI_JBM_O_SIZE, (uint32_t)size);
    put32(file + JBI_JBM_O_FLAGS, (uint32_t)((has_n ? JBI_JBM_NORMALS : 0) |
                                             (has_c ? JBI_JBM_COLORS : 0) |
                                             (has_uv ? JBI_JBM_UV : 0)));
    put32(file + JBI_JBM_O_VERTICES, (uint32_t)vt.count);
    put32(file + JBI_JBM_O_FACES, (uint32_t)scene.face_count);
    put32(file + JBI_JBM_O_MATERIALS, (uint32_t)nmat);
    put32(file + JBI_JBM_O_SLOTS, (uint32_t)nslots);
    for (k = 0; k < JBI_JBM_SECTIONS; k++)
        put32(file + JBI_JBM_O_SECTIONS + 4 * k, (uint32_t)off[k]);
    for (i = 0; i < vt.count; i++) {
        const vertex *v = &vt.v[i];
        for (k = 0; k < 3; k++) {
            put16(file + off[JBI_JBM_S_POSITIONS] + (size_t)i * 6 + 2 * k, (uint16_t)v->p[k]);
            if (has_n)
                put16(file + off[JBI_JBM_S_NORMALS] + (size_t)i * 6 + 2 * k, (uint16_t)v->n[k]);
        }
        if (has_c)
            put32(file + off[JBI_JBM_S_COLORS] + (size_t)i * 4, v->color);
        if (has_uv) {
            file[off[JBI_JBM_S_UV] + (size_t)i * 2] = v->uv[0];
            file[off[JBI_JBM_S_UV] + (size_t)i * 2 + 1] = v->uv[1];
        }
    }
    for (f = 0; f < scene.face_count; f++)
        for (k = 0; k < 6; k++)
            put16(file + off[JBI_JBM_S_FACES] + f * 12 + 2 * (size_t)k, faces[f * 6 + (size_t)k]);
    for (i = 0; i < nmat; i++) {
        const jbm_material *m = &scene.materials[out_mat[i]];
        const material_out *o = &mo[out_mat[i]];
        unsigned char *p = file + off[JBI_JBM_S_MATERIALS] + (size_t)i * 8;
        double ck = o->slot >= 0 ? 128.0 : 255.0;
        uint32_t color = JB_RGB(channel(m->color[0], ck), channel(m->color[1], ck),
                                channel(m->color[2], ck));
        put32(p, color == 0 ? JB_TINT_BLACK : color);
        p[4] = (unsigned char)o->blend;
        p[5] = (unsigned char)((o->unlit ? JBI_JBM_UNLIT : 0) | (o->raw ? JBI_JBM_RAW : 0) |
                               (o->double_sided ? JBI_JBM_DOUBLE_SIDED : 0));
        put16(p + 6, o->slot >= 0 ? (unsigned)o->slot : JBI_JBM_NO_SLOT);
    }
    for (i = 0; i < nslots; i++) {
        put16(file + off[JBI_JBM_S_SLOTS] + (size_t)i * 4, (unsigned)slots[i].width);
        put16(file + off[JBI_JBM_S_SLOTS] + (size_t)i * 4 + 2, (unsigned)slots[i].height);
    }
    fp = fopen(out, "wb");
    if (fp == NULL)
        FAIL("%s: cannot open it for writing", out);
    if (fwrite(file, 1, size, fp) != size) {
        fclose(fp);
        FAIL("%s: cannot write it", out);
    }
    if (fclose(fp) != 0)
        FAIL("%s: cannot write it", out);

    printf("%s: %d vertices, %d triangles, %d quads, %d materials, %d texture slots%s\n", out,
           vt.count, tris, quads, nmat, nslots, nslots > 0 ? ":" : "");
    for (i = 0; i < nslots; i++)
        printf("  slot %d: %s (%d x %d%s)\n", i, slots[i].file, slots[i].width,
               slots[i].height, slots[i].repeat ? ", repeat" : "");
    rc = 0;
#undef FAIL
done:
    free(file);
    free(faces);
    free(vt.v);
    free(vt.hash);
    free(mo);
    free(out_mat);
    jbm_scene_free(&scene);
    return rc;
}
