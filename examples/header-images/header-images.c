/*
 * header-images: the README's header images, drawn by jitterbug.
 *
 *   header-images DIR
 *       writes DIR/logo.png, DIR/beetle.png and DIR/banner.png
 *   header-images --check-favicon PAGE PNG
 *       status 0 when the text file PAGE holds PNG as a data URI
 *       (data:image/png;base64,...), 1 when not
 *   header-images --favicon-uri PNG
 *       prints that data URI
 *
 * logo.png: an animated PNG (APNG), a low-poly beetle walking on the spot
 *   as it turns once round, LOGO_FRAMES frames a loop, beside the wordmark
 *   "jitterbug" in the library's own 8 x 8 font. Both are drawn at the
 *   console's 1x, with integer vertices, affine textures and dithering,
 *   and shown LOGO_ZOOM times larger with hard, nearest-neighbour pixels;
 *   the background is transparent.
 * beetle.png: the beetle alone, the logo's first frame, square, for a
 *   favicon and the repository's social image.
 * banner.png: the demo's lighthouse at sunset (examples/demo, its own scene
 *   code and assets, from a fixed camera of the banner's own) drawn twice
 *   and cut diagonally through the middle of the tower: on the left the
 *   console (1x, every artefact on) shown 4 times larger with hard pixels,
 *   on the right the same frame drawn at 4x with every switch off.
 *
 * Everything drawn goes through jitterbug's public API; this program only
 * composes the frames it gives (transparency, upscaling, the diagonal cut,
 * labels from the library's font) and writes them. All of it is integer
 * arithmetic or the library's deterministic float maths (jb_sinf, jb_cosf,
 * + - * / in float with no contraction), so the files are the same bytes
 * on every platform: the test header_images regenerates them and compares
 * them with docs/images byte for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jitterbug.h>

#include "encode.h"
#include "scene.h"

#define PI 3.14159265f

/* ------------------------------------------------------------------------ */
/* Images */

/* An RGBA image, 4 bytes a pixel, rows with no padding. */
typedef struct image {
    int w, h;
    uint8_t *px;
} image;

static int image_new(image *im, int w, int h)
{
    im->w = w, im->h = h;
    im->px = calloc((size_t)w * (size_t)h, 4);
    if (im->px == NULL)
        fprintf(stderr, "header-images: out of memory\n");
    return im->px != NULL;
}

static uint8_t *at(const image *im, int x, int y)
{
    return im->px + ((size_t)y * (size_t)im->w + (size_t)x) * 4;
}

/* src over dst at (x, y): source pixels with alpha 0 left out, others
   copied (every alpha here is 0 or 255). */
static void paste(image *dst, const image *src, int x0, int y0)
{
    int x, y;

    for (y = 0; y < src->h; ++y)
        for (x = 0; x < src->w; ++x) {
            const uint8_t *s = at(src, x, y);
            if (s[3] != 0 && x0 + x >= 0 && y0 + y >= 0 && x0 + x < dst->w && y0 + y < dst->h)
                memcpy(at(dst, x0 + x, y0 + y), s, 4);
        }
}

/* src (x, y, w, h) shown k times larger, nearest neighbour. */
static int zoom(image *dst, const image *src, int x0, int y0, int w, int h, int k)
{
    int x, y;

    if (!image_new(dst, w * k, h * k))
        return 0;
    for (y = 0; y < h * k; ++y)
        for (x = 0; x < w * k; ++x)
            memcpy(at(dst, x, y), at(src, x0 + x / k, y0 + y / k), 4);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Files */

static int write_file(const char *path, const unsigned char *data, size_t n)
{
    FILE *fp = fopen(path, "wb");
    int ok = fp != NULL && fwrite(data, 1, n, fp) == n;

    if (fp != NULL && fclose(fp) != 0)
        ok = 0;
    if (!ok)
        fprintf(stderr, "header-images: cannot write %s\n", path);
    return ok;
}

static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *buf = NULL;
    long len;

    if (fp == NULL || fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0 || (buf = malloc((size_t)len + 1)) == NULL ||
        fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        fprintf(stderr, "header-images: cannot read %s\n", path);
        free(buf);
        if (fp != NULL)
            fclose(fp);
        return NULL;
    }
    fclose(fp);
    buf[len] = 0;
    *n = (size_t)len;
    return buf;
}

/* A growing byte buffer. */
typedef struct bytes {
    unsigned char *p;
    size_t n, cap;
    int failed;
} bytes;

static void put(bytes *b, const void *data, size_t n)
{
    if (b->failed)
        return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 65536;
        unsigned char *p;
        while (cap < b->n + n)
            cap *= 2;
        if ((p = realloc(b->p, cap)) == NULL) {
            b->failed = 1;
            return;
        }
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
}

static void put32(bytes *b, uint32_t v)
{
    unsigned char c[4];

    c[0] = (unsigned char)(v >> 24), c[1] = (unsigned char)(v >> 16);
    c[2] = (unsigned char)(v >> 8), c[3] = (unsigned char)v;
    put(b, c, 4);
}

static uint32_t crc32(uint32_t crc, const unsigned char *p, size_t n)
{
    size_t i;
    int k;

    crc = ~crc;
    for (i = 0; i < n; ++i) {
        crc ^= p[i];
        for (k = 0; k < 8; ++k)
            crc = crc >> 1 ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* A PNG chunk: length, type, data, CRC of type and data. */
static void chunk(bytes *b, const char *type, const unsigned char *data, size_t n)
{
    uint32_t crc = crc32(0, (const unsigned char *)type, 4);

    put32(b, (uint32_t)n);
    put(b, type, 4);
    if (n > 0)
        put(b, data, n);
    put32(b, n > 0 ? crc32(crc, data, n) : crc);
}

/* The image data of an encoded PNG: its IDAT chunks' data, one after
   another (one zlib stream), into *out; and its IHDR's 13 bytes. */
static int png_parts(const unsigned char *png, int len, bytes *out, unsigned char ihdr[13])
{
    int pos = 8;

    while (pos + 12 <= len) {
        uint32_t n = (uint32_t)png[pos] << 24 | (uint32_t)png[pos + 1] << 16 |
                     (uint32_t)png[pos + 2] << 8 | png[pos + 3];
        const unsigned char *type = png + pos + 4;
        if (n > (uint32_t)(len - pos - 12))
            return 0;
        if (memcmp(type, "IHDR", 4) == 0 && n == 13)
            memcpy(ihdr, type + 4, 13);
        else if (memcmp(type, "IDAT", 4) == 0)
            put(out, type + 4, n);
        pos += 12 + (int)n;
    }
    return !out->failed && out->n > 0;
}

/* The pixels (x, y, w, h) of im, encoded as a PNG's image data. */
static int encode_part(const image *im, int x0, int y0, int w, int h, bytes *data, unsigned char ihdr[13])
{
    image part;
    unsigned char *png;
    int len, y, ok;

    if (!image_new(&part, w, h))
        return 0;
    for (y = 0; y < h; ++y)
        memcpy(at(&part, 0, y), at(im, x0, y0 + y), (size_t)w * 4);
    png = hi_png(part.px, w, h, 4, &len);
    free(part.px);
    if (png == NULL)
        return 0;
    ok = png_parts(png, len, data, ihdr);
    hi_png_free(png);
    return ok;
}

/*
 * An animated PNG of n frames, each shown delay_num / delay_den seconds,
 * looping for ever. The first frame is the PNG's own image (what a viewer
 * without APNG shows); every later one replaces only the rectangle (bx,
 * by, bw, bh), the part of the frames that changes, alpha included
 * (APNG_BLEND_OP_SOURCE, APNG_DISPOSE_OP_NONE).
 */
static int write_apng(const char *path, const image *frames, int n, int bx, int by, int bw, int bh,
                      int delay_num, int delay_den)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    bytes out = {0}, ctl = {0};
    unsigned char ihdr[13], other[13];
    uint32_t seq = 0;
    int i, ok;

    put(&out, signature, 8);
    for (i = 0; i < n; ++i) {
        bytes data = {0};
        int x = i == 0 ? 0 : bx, y = i == 0 ? 0 : by;
        int w = i == 0 ? frames[0].w : bw, h = i == 0 ? frames[0].h : bh;
        unsigned char tail[6];

        if (!encode_part(&frames[i], x, y, w, h, &data, i == 0 ? ihdr : other)) {
            free(data.p);
            free(out.p);
            free(ctl.p);
            fprintf(stderr, "header-images: cannot encode frame %d\n", i);
            return 0;
        }
        if (i == 0) {
            chunk(&out, "IHDR", ihdr, 13);
            ctl.n = 0;
            put32(&ctl, (uint32_t)n);
            put32(&ctl, 0); /* plays: 0, for ever */
            chunk(&out, "acTL", ctl.p, ctl.n);
        }
        ctl.n = 0;
        put32(&ctl, seq++);
        put32(&ctl, (uint32_t)w);
        put32(&ctl, (uint32_t)h);
        put32(&ctl, (uint32_t)x);
        put32(&ctl, (uint32_t)y);
        tail[0] = (unsigned char)(delay_num >> 8), tail[1] = (unsigned char)delay_num;
        tail[2] = (unsigned char)(delay_den >> 8), tail[3] = (unsigned char)delay_den;
        tail[4] = 0; /* APNG_DISPOSE_OP_NONE */
        tail[5] = 0; /* APNG_BLEND_OP_SOURCE */
        put(&ctl, tail, 6);
        chunk(&out, "fcTL", ctl.p, ctl.n);
        if (i == 0) {
            chunk(&out, "IDAT", data.p, data.n);
        } else {
            ctl.n = 0;
            put32(&ctl, seq++);
            put(&ctl, data.p, data.n);
            chunk(&out, "fdAT", ctl.p, ctl.n);
        }
        free(data.p);
    }
    chunk(&out, "IEND", NULL, 0);
    ok = !out.failed && !ctl.failed && write_file(path, out.p, out.n);
    if (ok)
        printf("wrote %s: %d x %d, %d frames (%d x %d at %d, %d after the first), %lu bytes\n", path,
               frames[0].w, frames[0].h, n, bw, bh, bx, by, (unsigned long)out.n);
    free(out.p);
    free(ctl.p);
    return ok;
}

static int write_png(const char *path, const image *im, int comp)
{
    unsigned char *png, *rgb = NULL;
    int len, ok, i;

    if (comp == 3) {
        if ((rgb = malloc((size_t)im->w * (size_t)im->h * 3)) == NULL)
            return 0;
        for (i = 0; i < im->w * im->h; ++i)
            memcpy(rgb + (size_t)i * 3, im->px + (size_t)i * 4, 3);
    }
    png = hi_png(rgb != NULL ? rgb : im->px, im->w, im->h, comp, &len);
    free(rgb);
    if (png == NULL)
        return 0;
    ok = write_file(path, png, (size_t)len);
    if (ok)
        printf("wrote %s: %d x %d, %d bytes\n", path, im->w, im->h, len);
    hi_png_free(png);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Drawing with jitterbug */

static jb_context *context(int w, int h, int scale)
{
    jb_config c;
    jb_result r;
    jb_context *jb;

    memset(&c, 0, sizeof c);
    c.width = w, c.height = h, c.scale = scale, c.threads = 1;
    if ((jb = jb_create(&c, &r)) == NULL)
        fprintf(stderr, "header-images: no context: %s\n", jb_result_string(r));
    return jb;
}

/* After a frame: 1, or 0 with why. */
static int frame_ok(jb_context *jb, const char *what)
{
    if (jb_end_frame(jb) != JB_OK) {
        fprintf(stderr, "header-images: %s: %s\n", what, jb_error_string(jb));
        return 0;
    }
    return 1;
}

/* The two colours a frame is cleared to, drawn twice: a pixel the same in
   both is the picture's, one that differs is background. */
#define KEY_A JB_RGB(255, 0, 255)
#define KEY_B JB_RGB(0, 255, 0)

/* Two frames of the same drawing over the two keys, a and b, into im (of
   their size): opaque where they agree, transparent where not. */
static void keyed(image *im, const uint8_t *a, const uint8_t *b)
{
    int i;

    for (i = 0; i < im->w * im->h; ++i) {
        uint8_t *p = im->px + (size_t)i * 4;
        if (memcmp(a + (size_t)i * 4, b + (size_t)i * 4, 3) == 0)
            memcpy(p, a + (size_t)i * 4, 3), p[3] = 255;
        else
            memset(p, 0, 4);
    }
}

/* The library's font: text's set pixels, as 1 or 0 in mask (8 x strlen
   wide, 8 high), drawn by jb_draw_text. */
static int text_mask(const char *text, uint8_t *mask)
{
    int w = 8 * (int)strlen(text), fw, fh, i;
    jb_context *jb = context(w, 8, 1);
    const uint8_t *px;

    if (jb == NULL)
        return 0;
    jb_begin_frame(jb, JB_RGB(0, 0, 0));
    jb_draw_text(jb, 0, 0, JB_RGB(255, 255, 255), text);
    if (!frame_ok(jb, "text") || (px = jb_frame(jb, &fw, &fh)) == NULL || fw != w || fh != 8) {
        jb_destroy(jb);
        return 0;
    }
    for (i = 0; i < w * 8; ++i)
        mask[i] = px[(size_t)i * 4] != 0;
    jb_destroy(jb);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* The beetle */

enum { MAX_V = 1024, MAX_F = 1024 };

/* A mesh being made: vertices, faces and materials. */
typedef struct part {
    jb_gte_vec pos[MAX_V], nrm[MAX_V];
    uint32_t col[MAX_V];
    uint8_t uv[2 * MAX_V];
    jb_mesh_face face[MAX_F];
    jb_material mat[2];
    int nv, nf;
    jb_mesh mesh;
} part;

static int16_t round16(float v)
{
    return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

static jb_vec3 v3(float x, float y, float z)
{
    jb_vec3 v;

    v.x = x, v.y = y, v.z = z;
    return v;
}

static jb_vec3 add(jb_vec3 a, jb_vec3 b)
{
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static jb_vec3 sub(jb_vec3 a, jb_vec3 b)
{
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static jb_vec3 mul(jb_vec3 a, float k)
{
    return v3(a.x * k, a.y * k, a.z * k);
}

static int vertex(part *p, jb_vec3 pos, jb_vec3 n, uint32_t color, int u, int v)
{
    int i = p->nv++;

    n = jb_vec3_normalize(n);
    p->pos[i].x = round16(pos.x), p->pos[i].y = round16(pos.y), p->pos[i].z = round16(pos.z);
    p->nrm[i].x = round16(n.x * 4096), p->nrm[i].y = round16(n.y * 4096), p->nrm[i].z = round16(n.z * 4096);
    p->col[i] = color;
    p->uv[2 * i] = (uint8_t)u, p->uv[2 * i + 1] = (uint8_t)v;
    return i;
}

static jb_vec3 vpos(const part *p, int i)
{
    return v3(p->pos[i].x, p->pos[i].y, p->pos[i].z);
}

/* A face of 3 (d < 0) or 4 vertices given round it either way: turned to
   go anticlockwise seen from the side its vertex normals point to (or,
   with out given, from that side). */
static void face(part *p, int a, int b, int c, int d, int mat, const jb_vec3 *out)
{
    jb_mesh_face *f = &p->face[p->nf++];
    jb_vec3 cr, n;

    if (d < 0)
        cr = jb_vec3_cross(sub(vpos(p, b), vpos(p, a)), sub(vpos(p, c), vpos(p, a)));
    else
        cr = jb_vec3_cross(sub(vpos(p, c), vpos(p, a)), sub(vpos(p, d), vpos(p, b)));
    if (out != NULL)
        n = *out;
    else {
        n = v3(p->nrm[a].x + p->nrm[b].x + p->nrm[c].x, p->nrm[a].y + p->nrm[b].y + p->nrm[c].y,
               p->nrm[a].z + p->nrm[b].z + p->nrm[c].z);
        if (d >= 0)
            n = add(n, v3(p->nrm[d].x, p->nrm[d].y, p->nrm[d].z));
    }
    if (jb_vec3_dot(cr, n) < 0) {
        int t = b;
        if (d < 0)
            b = c, c = t;
        else
            b = d, d = t;
    }
    f->v[0] = (uint16_t)a, f->v[1] = (uint16_t)b, f->v[2] = (uint16_t)c;
    f->v[3] = (uint16_t)(d < 0 ? 0 : d);
    f->vertices = (uint16_t)(d < 0 ? 3 : 4);
    f->material = (uint16_t)mat;
}

/* How an ellipsoid's vertices are coloured: from the unit sphere's point. */
typedef uint32_t (*paint_fn)(jb_vec3 unit);

/*
 * An ellipsoid's band from latitude lo to hi (radians, -pi/2 to pi/2),
 * centre c, radii r, seg segments round and rings rings: smooth normals,
 * colours from paint, and with tex_w its texels projected from above
 * (u across x, v along z, tex_w x tex_h). Material mat.
 */
static void ellipsoid(part *p, jb_vec3 c, jb_vec3 r, float lo, float hi, int seg, int rings,
                      paint_fn paint, int tex_w, int tex_h, int mat)
{
    int first = p->nv, i, j, top = -1, bottom = -1, nring, j0 = 0, j1 = rings;
    const float half_pi = PI / 2;

    /* Rings of vertices, but a pole is one vertex. */
    if (hi >= half_pi)
        j1 = rings - 1;
    if (lo <= -half_pi)
        j0 = 1;
    for (j = j0; j <= j1; ++j) {
        float phi = lo + (hi - lo) * (float)j / (float)rings, cp = jb_cosf(phi), sp = jb_sinf(phi);
        for (i = 0; i < seg; ++i) {
            float th = 2 * PI * (float)i / (float)seg;
            jb_vec3 unit = v3(jb_cosf(th) * cp, sp, jb_sinf(th) * cp);
            jb_vec3 pos = add(c, v3(unit.x * r.x, unit.y * r.y, unit.z * r.z));
            jb_vec3 n = v3(unit.x / r.x, unit.y / r.y, unit.z / r.z);
            int u = tex_w ? round16((unit.x + 1) * 0.5f * (float)(tex_w - 1)) : 0;
            int v = tex_h ? round16((unit.z + 1) * 0.5f * (float)(tex_h - 1)) : 0;
            vertex(p, pos, n, paint(unit), u, v);
        }
    }
    nring = j1 - j0 + 1;
    if (hi >= half_pi)
        top = vertex(p, add(c, v3(0, r.y, 0)), v3(0, 1, 0), paint(v3(0, 1, 0)), tex_w / 2, tex_h / 2);
    if (lo <= -half_pi)
        bottom = vertex(p, add(c, v3(0, -r.y, 0)), v3(0, -1, 0), paint(v3(0, -1, 0)), tex_w / 2,
                        tex_h / 2);
    for (j = 0; j + 1 < nring; ++j)
        for (i = 0; i < seg; ++i) {
            int a = first + j * seg + i, b = first + j * seg + (i + 1) % seg;
            face(p, a, b, b + seg, a + seg, mat, NULL);
        }
    for (i = 0; i < seg; ++i) {
        int a = first + i, b = first + (i + 1) % seg;
        int ta = first + (nring - 1) * seg + i, tb = first + (nring - 1) * seg + (i + 1) % seg;
        if (top >= 0)
            face(p, ta, tb, top, -1, mat, NULL);
        if (bottom >= 0)
            face(p, a, b, bottom, -1, mat, NULL);
    }
}

static void finish(part *p, int flat, int materials)
{
    memset(&p->mesh, 0, sizeof p->mesh);
    p->mesh.vertex_count = p->nv;
    p->mesh.positions = p->pos;
    p->mesh.normals = flat ? NULL : p->nrm;
    p->mesh.colors = p->col;
    p->mesh.uv = p->uv;
    p->mesh.face_count = p->nf;
    p->mesh.faces = p->face;
    p->mesh.material_count = materials;
    p->mesh.materials = p->mat;
}

/* The beetle's colours. */
#define C_BODY JB_RGB(44, 30, 58)
#define C_LEG JB_RGB(66, 44, 80)
#define C_EYE JB_RGB(244, 242, 236)
#define C_PUPIL JB_RGB(22, 16, 34)

static uint32_t paint_shell(jb_vec3 u)
{
    (void)u;
    return JB_NEUTRAL;
}

static uint32_t paint_body(jb_vec3 u)
{
    (void)u;
    return C_BODY;
}

static uint32_t paint_eye(jb_vec3 u)
{
    (void)u;
    return C_EYE;
}

static uint32_t paint_pupil(jb_vec3 u)
{
    (void)u;
    return C_PUPIL;
}

/* The shell's texture: 64 x 64 texels, 4-bit, seen from above (u across,
   v from tail to head): a glossy red dome with dark spots, a seam down the
   middle between the two wing cases, a deeper rim and a little speckle. */
enum { SHELL_W = 64, SHELL_H = 64 };

static uint32_t hash(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

static void shell_texels(uint8_t *t, uint16_t *palette)
{
    /* Spots: x, y in texels from the centre (y towards the head), radius. */
    static const int spot[][3] = {{0, 26, 7},  {-14, 12, 6}, {14, 12, 6}, {-19, -6, 6},
                                  {19, -6, 6}, {-11, -21, 5}, {11, -21, 5}};
    static const uint8_t rgba[16][4] = {
        {0, 0, 0, 0},       {112, 14, 30, 255}, {170, 26, 36, 255}, {212, 44, 42, 255},
        {238, 92, 54, 255}, {32, 16, 46, 255},  {58, 18, 40, 255},  {84, 22, 52, 255},
        {196, 36, 40, 255}, {148, 20, 34, 255}, {0, 0, 0, 255},     {0, 0, 0, 255},
        {0, 0, 0, 255},     {0, 0, 0, 255},     {0, 0, 0, 255},     {0, 0, 0, 255}};
    int x, y, k;

    jb_palette_from_rgba(&rgba[0][0], 16, palette);
    for (y = 0; y < SHELL_H; ++y)
        for (x = 0; x < SHELL_W; ++x) {
            /* Twice the distance from the centre, in texels. */
            int dx = 2 * x - (SHELL_W - 1), dy = 2 * y - (SHELL_H - 1);
            int r2 = dx * dx + dy * dy, c = 3;
            uint32_t h = hash((uint32_t)(y * SHELL_W + x) * 2654435761u);
            if (r2 > 56 * 56)
                c = 1;
            else if (r2 > 50 * 50)
                c = 9;
            else if (r2 > 42 * 42)
                c = (h & 3) == 0 ? 9 : 2;
            else if ((h & 7) == 0)
                c = 8;
            else if ((h & 31) == 1)
                c = 4;
            for (k = 0; k < (int)(sizeof spot / sizeof spot[0]); ++k) {
                int sx = dx - 2 * spot[k][0], sy = dy - 2 * spot[k][1], sr = 2 * spot[k][2];
                if (sx * sx + sy * sy <= sr * sr)
                    c = sx * sx + sy * sy > (sr - 3) * (sr - 3) ? 7 : 5;
            }
            if (dx >= -2 && dx <= 2 && r2 <= 56 * 56)
                c = 6;
            t[y * SHELL_W + x] = (uint8_t)c;
        }
}

/* The glint: 7 x 7 texels, 4-bit, a four-pointed sparkle, semi-transparent
   greys to be added. */
static void glint_texels(uint8_t *t, uint16_t *palette)
{
    static const char *const rows[7] = {"...2...", "...5...", "..3a3..", "25afa52",
                                        "..3a3..", "...5...", "...2..."};
    uint8_t rgba[16][4];
    int x, y, k;

    for (k = 0; k < 16; ++k) {
        rgba[k][0] = (uint8_t)(k * 17);
        rgba[k][1] = (uint8_t)(k * 17);
        rgba[k][2] = (uint8_t)(k * 16);
        rgba[k][3] = k == 0 ? 0 : 64; /* bit 15: blended */
    }
    jb_palette_from_rgba(&rgba[0][0], 16, palette);
    for (y = 0; y < 7; ++y)
        for (x = 0; x < 7; ++x) {
            char ch = rows[y][x];
            t[y * 7 + x] = (uint8_t)(ch == '.' ? 0 : ch <= '9' ? ch - '0' : ch - 'a' + 10);
        }
}

/* The beetle's model, in its own units: +z towards its head, y up; its
   shell's centre near the origin, its feet on y = GROUND. */
#define GROUND (-14.0f)

static const jb_vec3 shell_c = {0, 12, 0}, shell_r = {58, 50, 74};

typedef struct beetle {
    part shell, body, limb;
    jb_texture shell_tex, glint_tex;
} beetle;

static int beetle_make(jb_context *jb, beetle *b)
{
    static uint8_t shell_t[SHELL_W * SHELL_H];
    static const float corner[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
    uint8_t glint_t[49];
    uint16_t pal[16];
    jb_texture_desc d;
    jb_vec3 none = {0, 0, 0}, out;
    int sx, k, v[8];

    memset(&d, 0, sizeof d);
    shell_texels(shell_t, pal);
    d.width = SHELL_W, d.height = SHELL_H, d.depth = JB_TEX_4BIT, d.texels = shell_t, d.palette = pal;
    if (jb_texture_create(jb, &d, &b->shell_tex) != JB_OK)
        return 0;
    glint_texels(glint_t, pal);
    d.width = 7, d.height = 7, d.texels = glint_t;
    if (jb_texture_create(jb, &d, &b->glint_tex) != JB_OK)
        return 0;

    /* The shell: the wing cases, a textured dome, and the belly under it. */
    memset(&b->shell.mat, 0, sizeof b->shell.mat);
    b->shell.mat[0].texture = &b->shell_tex;
    b->shell.nv = b->shell.nf = 0;
    ellipsoid(&b->shell, shell_c, shell_r, 0, PI / 2, 14, 4, paint_shell, SHELL_W, SHELL_H, 0);
    ellipsoid(&b->shell, shell_c, v3(52, 16, 68), -PI / 2, 0, 10, 1, paint_body, 0, 0, 1);
    finish(&b->shell, 0, 2);

    /* The pronotum, the head and the eyes, coloured by their vertices. */
    memset(&b->body.mat, 0, sizeof b->body.mat);
    b->body.mat[1].unlit = 1; /* the eyes: cartoon white, unshaded */
    b->body.nv = b->body.nf = 0;
    ellipsoid(&b->body, v3(0, 12, 80), v3(40, 30, 22), 0, PI / 2, 12, 3, paint_body, 0, 0, 0);
    ellipsoid(&b->body, v3(0, 12, 80), v3(38, 10, 20), -PI / 2, 0, 8, 1, paint_body, 0, 0, 0);
    ellipsoid(&b->body, v3(0, 12, 104), v3(24, 19, 16), -PI / 2, PI / 2, 10, 4, paint_body, 0, 0, 0);
    for (sx = -1; sx <= 1; sx += 2) {
        ellipsoid(&b->body, v3(15.0f * (float)sx, 22, 114), v3(11, 12, 10), -PI / 2, PI / 2, 8, 4,
                  paint_eye, 0, 0, 1);
        ellipsoid(&b->body, v3(17.0f * (float)sx, 24, 123), v3(5.5f, 6.5f, 3), -PI / 2, PI / 2, 6, 2,
                  paint_pupil, 0, 0, 1);
    }
    finish(&b->body, 0, 2);

    /* A leg's or antenna's segment: a tapering square tube along +x from 0
       to 256, 32 thick at its root, flat-lit. */
    memset(&b->limb.mat, 0, sizeof b->limb.mat);
    b->limb.nv = b->limb.nf = 0;
    for (k = 0; k < 4; ++k) {
        v[k] = vertex(&b->limb, v3(0, 16 * corner[k][0], 16 * corner[k][1]), none, C_LEG, 0, 0);
        v[4 + k] = vertex(&b->limb, v3(256, 11 * corner[k][0], 11 * corner[k][1]), none, C_LEG, 0, 0);
    }
    for (k = 0; k < 4; ++k) {
        int n = (k + 1) % 4;
        out = v3(0, corner[k][0] + corner[n][0], corner[k][1] + corner[n][1]);
        face(&b->limb, v[k], v[n], v[4 + n], v[4 + k], 0, &out);
    }
    out = v3(-1, 0, 0);
    face(&b->limb, v[0], v[1], v[2], v[3], 0, &out);
    out = v3(1, 0, 0);
    face(&b->limb, v[4], v[5], v[6], v[7], 0, &out);
    finish(&b->limb, 1, 1);
    return 1;
}

/* A limb's segment from p to q (beetle space), thick across, placed by
   the beetle's matrix m. */
static void segment(jb_context *jb, const beetle *b, const jb_mat4 *m, jb_vec3 p, jb_vec3 q, float thick)
{
    jb_vec3 d = sub(q, p), ex = jb_vec3_normalize(d), e2, e3;
    jb_mat4 s, model;
    jb_mesh_draw draw;

    e2 = jb_vec3_normalize(jb_vec3_cross(ex, v3(0, 1, 0)));
    if (e2.x == 0 && e2.y == 0 && e2.z == 0)
        e2 = v3(0, 0, 1);
    e3 = jb_vec3_cross(e2, ex);
    e2 = mul(e2, thick / 32), e3 = mul(e3, thick / 32), d = mul(d, 1.0f / 256);
    memset(&s, 0, sizeof s);
    s.m[0] = d.x, s.m[1] = d.y, s.m[2] = d.z;
    s.m[4] = e3.x, s.m[5] = e3.y, s.m[6] = e3.z;
    s.m[8] = e2.x, s.m[9] = e2.y, s.m[10] = e2.z;
    s.m[12] = p.x, s.m[13] = p.y, s.m[14] = p.z, s.m[15] = 1;
    model = jb_mat4_mul(m, &s);
    memset(&draw, 0, sizeof draw);
    draw.mesh = &b->limb.mesh, draw.model = &model;
    jb_draw_mesh_ex(jb, &draw);
}

/* The loop: frames, and the walk's steps in it. */
enum { LOGO_FRAMES = 60, LOGO_FPS = 20, WALK_CYCLES = 5 };

/* The camera and lights the beetle is drawn with. */
static const jb_vec3 cam_eye = {0, 205, 400}, cam_target = {0, 6, 2};
static const jb_light key_light = {{0.55f, -0.75f, -0.35f}, JB_RGB(255, 238, 214), 1.0f};
static const jb_light rim_light = {{-0.35f, -0.25f, 0.9f}, JB_RGB(80, 120, 230), 0.9f};

static void beetle_scene(jb_context *jb, float fov)
{
    jb_camera cam;

    memset(&cam, 0, sizeof cam);
    cam.eye = cam_eye, cam.target = cam_target, cam.fov_y = fov;
    cam.z_near = 64, cam.z_far = 1024;
    jb_set_camera(jb, &cam);
    jb_set_ambient(jb, JB_RGB(62, 52, 84));
    jb_set_light(jb, 0, &key_light);
    jb_set_light(jb, 1, &rim_light);
    jb_set_dither(jb, 1);
}

/* Frame t of the loop: the beetle turned once round over the loop,
   walking on the spot, WALK_CYCLES steps of each tripod. */
static void beetle_draw(jb_context *jb, const beetle *b, int t)
{
    static const float hip_z[3] = {58, 36, 14}, rest[3] = {0.75f, 0.05f, -0.7f};
    float turn = (float)t / LOGO_FRAMES, walk = 2 * PI * WALK_CYCLES * turn;
    float yaw = 0.75f - 2 * PI * turn, bob = 1.5f * jb_sinf(2 * walk);
    jb_mat4 rot = jb_mat4_rotate_y(yaw), move = jb_mat4_translate(0, bob, -26), m, back;
    jb_mesh_draw draw;
    jb_billboard g;
    jb_vec3 centre, hv, n, q, pos;
    float pulse, len;
    int side, k;

    m = jb_mat4_mul(&rot, &move);
    memset(&draw, 0, sizeof draw);
    draw.model = &m;
    draw.mesh = &b->shell.mesh;
    jb_draw_mesh_ex(jb, &draw);
    draw.mesh = &b->body.mesh;
    jb_draw_mesh_ex(jb, &draw);

    for (side = -1; side <= 1; side += 2) {
        float sx = (float)side, wave = 4 * jb_sinf(2 * PI * 2 * turn + (side > 0 ? 1.3f : 0));
        jb_vec3 base = v3(sx * 8, 26, 114), tip = v3(sx * 26, 50 + wave, 134),
                club = v3(sx * 33, 54 + wave, 140);
        /* The legs, as two tripods: front and back of one side with the
           middle of the other. */
        for (k = 0; k < 3; ++k) {
            float ph = walk + (float)((k + (side > 0)) & 1) * PI;
            float a = rest[k] + 0.34f * jb_sinf(ph), lift = jb_cosf(ph);
            jb_vec3 h, hip = v3(sx * 24, 6, hip_z[k]), knee, foot;
            lift = lift > 0 ? lift : 0;
            h = v3(sx * jb_cosf(a), 0, jb_sinf(a));
            knee = add(add(hip, mul(h, 38)), v3(0, 18 + 8 * lift, 0));
            foot = add(knee, mul(h, 24));
            foot.y = GROUND + 12 * lift;
            segment(jb, b, &m, hip, knee, 11);
            segment(jb, b, &m, knee, foot, 8);
        }
        /* An antenna, waving a little, with a club at its tip. */
        segment(jb, b, &m, base, tip, 5);
        segment(jb, b, &m, tip, club, 10);
    }

    /* The glint: where the shell's normal is halfway between the key
       light and the eye, so it stays on the shell as it turns, twinkling
       twice a loop. The point of the ellipsoid with normal n is
       c + r^2 n / |r n|. */
    centre = jb_mat4_transform_point(&m, shell_c);
    hv = jb_vec3_normalize(add(jb_vec3_normalize(mul(key_light.direction, -1)),
                               jb_vec3_normalize(sub(cam_eye, centre))));
    back = jb_mat4_rotate_y(-yaw);
    n = jb_mat4_transform_point(&back, hv);
    q = v3(n.x * shell_r.x, n.y * shell_r.y, n.z * shell_r.z);
    len = jb_vec3_dot(q, jb_vec3_normalize(q));
    pos = add(shell_c, v3(shell_r.x * q.x / len, shell_r.y * q.y / len, shell_r.z * q.z / len));
    pos = jb_mat4_transform_point(&m, pos);
    pulse = jb_sinf(2 * PI * 2 * turn);
    memset(&g, 0, sizeof g);
    g.texture = &b->glint_tex;
    g.position = pos;
    g.width = 22 + 16 * (pulse > 0 ? pulse : 0);
    g.blend = JB_BLEND_ADD;
    g.depth_bias = -40;
    jb_draw_billboard(jb, &g);
}

/* The beetle's frame t, keyed (transparent background), into im (the
   context's size). */
static int beetle_frame(jb_context *jb, const beetle *b, float fov, int t, image *im)
{
    static uint8_t first[256 * 256 * 4];
    const uint8_t *px = NULL;
    int pass, w, h;

    for (pass = 0; pass < 2; ++pass) {
        jb_begin_frame(jb, pass ? KEY_B : KEY_A);
        beetle_scene(jb, fov);
        beetle_draw(jb, b, t);
        if (!frame_ok(jb, "beetle") || (px = jb_frame(jb, &w, &h)) == NULL || w != im->w || h != im->h)
            return 0;
        if (pass == 0)
            memcpy(first, px, (size_t)w * (size_t)h * 4);
    }
    keyed(im, first, px);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* The logo */

/* The logo at 1x: the beetle's square at the left, the wordmark beside it;
   shown LOGO_ZOOM times larger. */
enum { BEETLE_PX = 64, LOGO_ZOOM = 3, FONT_ZOOM = 2, EXTRUDE = 2, MARGIN = 2, GAP = 7 };
static const char wordmark[] = "jitterbug";
#define BEETLE_FOV 33.0f

/* The wordmark: the library's font at FONT_ZOOM, drawn by jitterbug as a
   texture of its own, its rows shaded from gold to red, over a dark
   extrusion EXTRUDE pixels deep; keyed. */
static int wordmark_image(image *im)
{
    enum { LEN = sizeof wordmark - 1, TW = 8 * LEN * FONT_ZOOM, TH = 8 * FONT_ZOOM };
    static uint8_t mask[8 * LEN * 8], tex[TW * TH], first[(TW + EXTRUDE) * (TH + EXTRUDE) * 4];
    uint16_t pal[16];
    jb_texture_desc d;
    jb_texture t;
    jb_context *jb;
    const uint8_t *px = NULL;
    int x, y, pass, w, h, k;

    if (!text_mask(wordmark, mask))
        return 0;
    for (y = 0; y < TH; ++y)
        for (x = 0; x < TW; ++x)
            tex[y * TW + x] = mask[(y / FONT_ZOOM) * 8 * LEN + x / FONT_ZOOM];
    memset(pal, 0, sizeof pal);
    pal[1] = JB_RGB15(31, 31, 31);
    if ((jb = context(TW + EXTRUDE, TH + EXTRUDE, 1)) == NULL)
        return 0;
    memset(&d, 0, sizeof d);
    d.width = TW, d.height = TH, d.depth = JB_TEX_4BIT, d.texels = tex, d.palette = pal;
    if (jb_texture_create(jb, &d, &t) != JB_OK || !image_new(im, TW + EXTRUDE, TH + EXTRUDE)) {
        jb_destroy(jb);
        return 0;
    }
    for (pass = 0; pass < 2; ++pass) {
        jb_begin_frame(jb, pass ? KEY_B : KEY_A);
        for (k = EXTRUDE; k >= 0; --k)
            for (y = 0; y < TH; ++y) {
                /* Each row a sprite of its own colour: the extrusion dark
                   plum, the face from pale gold at the top to red. */
                jb_sprite s;
                int f = y * 64 / (TH - 1);
                memset(&s, 0, sizeof s);
                s.texture = &t, s.x = k, s.y = y + k, s.v = y, s.src_h = 1;
                s.tint = k ? JB_RGB(46 - 6 * k, 24 - 3 * k, 54 - 6 * k)
                           : JB_RGB(128, 122 - f, 84 - f / 2 - (f > 32 ? f - 32 : 0));
                jb_draw_sprite(jb, &s);
            }
        if (!frame_ok(jb, "wordmark") || (px = jb_frame(jb, &w, &h)) == NULL || w != im->w || h != im->h) {
            jb_destroy(jb);
            return 0;
        }
        if (pass == 0)
            memcpy(first, px, (size_t)w * (size_t)h * 4);
    }
    keyed(im, first, px);
    jb_destroy(jb);
    return 1;
}

/*
 * The edge that keeps the picture readable on a light page and a dark
 * one: every transparent pixel next to an opaque one (8-neighbours) turns
 * dark, and every transparent pixel next to that pale, as a sticker's.
 */
#define OUTLINE_IN JB_RGB(26, 18, 36)
#define OUTLINE_OUT JB_RGB(250, 244, 230)

static void outline(image *im, uint32_t color)
{
    int x, y, dx, dy;

    for (y = 0; y < im->h; ++y)
        for (x = 0; x < im->w; ++x) {
            uint8_t *p = at(im, x, y);
            int near = 0;
            if (p[3] != 0)
                continue;
            for (dy = -1; dy <= 1; ++dy)
                for (dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < im->w && ny < im->h && at(im, nx, ny)[3] == 255)
                        near = 1;
                }
            if (near) {
                p[0] = (uint8_t)(color & 255), p[1] = (uint8_t)(color >> 8 & 255);
                p[2] = (uint8_t)(color >> 16 & 255), p[3] = 1; /* new: not a neighbour yet */
            }
        }
    for (y = 0; y < im->h; ++y)
        for (x = 0; x < im->w; ++x)
            if (at(im, x, y)[3] == 1)
                at(im, x, y)[3] = 255;
}

static void sticker(image *im)
{
    outline(im, OUTLINE_IN);
    outline(im, OUTLINE_OUT);
}

static int make_logo(const char *dir)
{
    static image frames[LOGO_FRAMES];
    static beetle b;
    char path[4096];
    image word, canvas, bug, square = {0, 0, NULL};
    jb_context *jb;
    int t, ok = 1, bx0, by0, bx1, by1, x, y, i, cw, ch, word_x, word_y;

    if (!wordmark_image(&word))
        return 0;
    cw = MARGIN + 2 + BEETLE_PX + GAP + word.w + 2 + MARGIN;
    ch = MARGIN + 2 + BEETLE_PX + 2 + MARGIN;
    word_x = MARGIN + 2 + BEETLE_PX + GAP;
    word_y = (ch - word.h) / 2 + 3;
    if ((jb = context(BEETLE_PX, BEETLE_PX, 1)) == NULL) {
        free(word.px);
        return 0;
    }
    if (!beetle_make(jb, &b) || !image_new(&bug, BEETLE_PX, BEETLE_PX)) {
        fprintf(stderr, "header-images: the beetle: %s\n", jb_error_string(jb));
        jb_destroy(jb);
        free(word.px);
        return 0;
    }
    for (t = 0; t < LOGO_FRAMES && ok; ++t) {
        ok = beetle_frame(jb, &b, BEETLE_FOV, t, &bug) && image_new(&canvas, cw, ch);
        if (!ok)
            break;
        paste(&canvas, &bug, MARGIN + 2, MARGIN + 2);
        paste(&canvas, &word, word_x, word_y);
        sticker(&canvas);
        ok = zoom(&frames[t], &canvas, 0, 0, cw, ch, LOGO_ZOOM);
        free(canvas.px);
        if (ok && t == 0) {
            /* The square: the first frame's beetle alone, with its edge. */
            image sq;
            ok = image_new(&sq, BEETLE_PX + 4, BEETLE_PX + 4);
            if (ok) {
                paste(&sq, &bug, 2, 2);
                sticker(&sq);
                ok = zoom(&square, &sq, 2, 2, BEETLE_PX, BEETLE_PX, 4);
                free(sq.px);
            }
        }
    }
    free(bug.px);
    jb_destroy(jb);
    free(word.px);
    /* The rectangle that changes: what differs from the first frame. */
    bx0 = frames[0].w, by0 = frames[0].h, bx1 = -1, by1 = -1;
    for (i = 1; ok && i < LOGO_FRAMES; ++i)
        for (y = 0; y < frames[0].h; ++y)
            for (x = 0; x < frames[0].w; ++x)
                if (memcmp(at(&frames[i], x, y), at(&frames[0], x, y), 4) != 0) {
                    bx0 = x < bx0 ? x : bx0, by0 = y < by0 ? y : by0;
                    bx1 = x > bx1 ? x : bx1, by1 = y > by1 ? y : by1;
                }
    if (bx1 < 0)
        bx0 = by0 = bx1 = by1 = 0;
    sprintf(path, "%s/logo.png", dir);
    ok = ok && write_apng(path, frames, LOGO_FRAMES, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1, 1, LOGO_FPS);
    sprintf(path, "%s/beetle.png", dir);
    ok = ok && write_png(path, &square, 4);
    for (i = 0; i < LOGO_FRAMES; ++i)
        free(frames[i].px);
    free(square.px);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* The banner */

/*
 * The banner's moment: the demo's frame BANNER_FRAME (its waves, clouds
 * and the lighthouse's beams, here turned to sweep out to both sides)
 * seen from a fixed camera of its own (dm_scene.shot) rather than the
 * flight's: SHOT_DIST across the ground from the lighthouse, at SHOT_TURN
 * of a turn round it from +z towards +x, SHOT_HEIGHT above its foot,
 * looking back at it SHOT_PITCH turns below the horizon, with the low sun
 * to its left and the sea behind. The band shown is 1x rows from BAND_Y,
 * BAND_H high, and the cut runs from x CUT_TOP at the top of the banner
 * to CUT_BOTTOM at its bottom (in its pixels), through the middle of the
 * tower, so that one half of it is the console's and the other half the
 * switches off.
 */
enum { BANNER_FRAME = 150, BAND_Y = 52, BAND_H = 120, ZOOM = 4 };
enum { CUT_TOP = 676, CUT_BOTTOM = 608 };
#define SHOT_TURN 0.79f
#define SHOT_DIST 580.0f
#define SHOT_HEIGHT 250.0f
#define SHOT_PITCH 0.025f

/* A label in the library's font, FONT_ZOOM larger, light over a shadow,
   its corner at (x, y) of im; its right edge at x when right. */
static int label(image *im, const char *text, int x, int y, int right)
{
    static uint8_t mask[8 * 64 * 8];
    int n = (int)strlen(text), w = 8 * n, px, py, pass;

    if (n > 64 || !text_mask(text, mask))
        return 0;
    if (right)
        x -= w * FONT_ZOOM;
    for (pass = 0; pass < 2; ++pass)
        for (py = 0; py < 8 * FONT_ZOOM; ++py)
            for (px = 0; px < w * FONT_ZOOM; ++px) {
                int ox = x + px + (pass ? 0 : FONT_ZOOM / 2), oy = y + py + (pass ? 0 : FONT_ZOOM / 2), k;
                uint8_t *p;
                if (!mask[(py / FONT_ZOOM) * w + px / FONT_ZOOM] || ox < 0 || oy < 0 || ox >= im->w ||
                    oy >= im->h)
                    continue;
                p = at(im, ox, oy);
                for (k = 0; k < 3; ++k)
                    p[k] = (uint8_t)(pass ? (p[k] * 24 + 246 * 232) / 256 : p[k] * 96 / 256);
            }
    return 1;
}

static int make_banner(const char *dir)
{
    char path[4096];
    dm_switches console = DM_SWITCHES_CONSOLE, off;
    dm_scene *s = malloc(sizeof *s);
    dm_shot shot;
    image one = {0, 0, NULL}, four = {0, 0, NULL}, out;
    const uint8_t *px = NULL;
    int w, h, x, y, ok, W = 320 * ZOOM, H = BAND_H * ZOOM;
    float dx = (float)(CUT_BOTTOM - CUT_TOP), len;
    jb_result r;

    if (s == NULL)
        return 0;
    console.panel = 0;
    off = console;
    off.scale = ZOOM, off.precise = 1, off.perspective = 1, off.dither = 0;
    off.geometry = JB_GEOMETRY_FLOAT, off.clut_cache = 0;
    if ((r = dm_scene_init(s, &console, 1)) != JB_OK) {
        fprintf(stderr, "header-images: the demo: %s\n", jb_result_string(r));
        free(s);
        return 0;
    }
    shot.eye = v3(s->lighthouse.x + SHOT_DIST * jb_sinf(SHOT_TURN * 2 * PI), s->lighthouse.y + SHOT_HEIGHT,
                  s->lighthouse.z + SHOT_DIST * jb_cosf(SHOT_TURN * 2 * PI));
    shot.yaw = SHOT_TURN + 0.5f; /* back towards the lighthouse */
    shot.pitch = SHOT_PITCH;
    s->shot = &shot;
    ok = dm_scene_draw(s, BANNER_FRAME, 0) == JB_OK && (px = dm_scene_frame(s, &w, &h)) != NULL &&
         w == 320 && h == 240 && image_new(&one, w, h);
    if (ok)
        memcpy(one.px, px, (size_t)w * (size_t)h * 4);
    ok = ok && dm_scene_set_switches(s, &off) == JB_OK && dm_scene_draw(s, BANNER_FRAME, 0) == JB_OK &&
         (px = dm_scene_frame(s, &w, &h)) != NULL && w == 320 * ZOOM && h == 240 * ZOOM &&
         image_new(&four, w, h);
    if (ok)
        memcpy(four.px, px, (size_t)w * (size_t)h * 4);
    else
        fprintf(stderr, "header-images: the demo's frame: %s\n", jb_error_string(s->jb));
    dm_scene_free(s);
    free(s);
    if (!ok || !image_new(&out, W, H)) {
        free(one.px);
        free(four.px);
        return 0;
    }

    /* The cut, 4 x 4 samples a pixel: left of the line the console, right
       of it the switches off, and on it (within 1.25 pixels) the line. */
    len = jb_vec3_dot(v3(dx, (float)H, 0), jb_vec3_normalize(v3(dx, (float)H, 0)));
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x) {
            const uint8_t *l = at(&one, x / ZOOM, BAND_Y + y / ZOOM), *rr = at(&four, x, BAND_Y * ZOOM + y);
            int sum[3] = {0, 0, 0}, sx, sy, k;
            uint8_t *o = at(&out, x, y);
            for (sy = 0; sy < 4; ++sy)
                for (sx = 0; sx < 4; ++sx) {
                    float fx = (float)x + ((float)sx + 0.5f) / 4, fy = (float)y + ((float)sy + 0.5f) / 4;
                    /* The signed distance from the line, in pixels. */
                    float d = ((fx - (float)CUT_TOP) * (float)H - dx * fy) / len;
                    for (k = 0; k < 3; ++k)
                        sum[k] += d > -1.25f && d < 1.25f ? 248 : d < 0 ? l[k] : rr[k];
                }
            for (k = 0; k < 3; ++k)
                o[k] = (uint8_t)((sum[k] + 8) / 16);
            o[3] = 255;
        }
    ok = label(&out, "1x, console", 14, H - 14 - 8 * FONT_ZOOM, 0) &&
         label(&out, "4x, switches off", W - 14, H - 14 - 8 * FONT_ZOOM, 1);
    sprintf(path, "%s/banner.png", dir);
    ok = ok && write_png(path, &out, 3);
    free(one.px);
    free(four.px);
    free(out.px);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* The favicon */

/* "data:image/png;base64,..." for the file at path (free it), or NULL. */
static char *data_uri(const char *path)
{
    static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char head[] = "data:image/png;base64,";
    size_t n, i, o = sizeof head - 1;
    unsigned char *png = read_file(path, &n);
    char *uri = png != NULL ? malloc(sizeof head + (n + 2) / 3 * 4) : NULL;

    if (uri == NULL) {
        free(png);
        return NULL;
    }
    memcpy(uri, head, sizeof head - 1);
    for (i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)png[i] << 16 | (i + 1 < n ? (uint32_t)png[i + 1] << 8 : 0) |
                     (i + 2 < n ? png[i + 2] : 0);
        uri[o++] = abc[v >> 18 & 63];
        uri[o++] = abc[v >> 12 & 63];
        uri[o++] = i + 1 < n ? abc[v >> 6 & 63] : '=';
        uri[o++] = i + 2 < n ? abc[v & 63] : '=';
    }
    uri[o] = 0;
    free(png);
    return uri;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--check-favicon") == 0) {
        size_t n;
        char *uri = data_uri(argv[3]), *page = (char *)read_file(argv[2], &n);
        int found = uri != NULL && page != NULL && strstr(page, uri) != NULL;
        if (uri != NULL && page != NULL)
            printf("%s %s %s as a data URI\n", argv[2], found ? "holds" : "does not hold", argv[3]);
        free(uri);
        free(page);
        return found ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "--favicon-uri") == 0) {
        char *uri = data_uri(argv[2]);
        if (uri == NULL)
            return 1;
        printf("%s\n", uri);
        free(uri);
        return 0;
    }
    if (argc != 2 || argv[1][0] == '-') {
        fprintf(stderr, "usage: header-images DIR\n"
                        "       header-images --check-favicon PAGE PNG\n"
                        "       header-images --favicon-uri PNG\n");
        return 2;
    }
    return make_logo(argv[1]) && make_banner(argv[1]) ? 0 : 1;
}
