/*
 * api_texture: textures placed in VRAM (include/jitterbug.h, Textures),
 * through the public API alone.
 *
 *   basics      the first textures of each depth land where the documented
 *               first fit puts them; their texels read back from VRAM with
 *               the GPU's packing (4-bit low nibble first, 8-bit low byte
 *               first, a row's last pixel padded with 0), their palettes;
 *               drawn with jb_prim_rect they show, at 1x and 2x
 *   repeat      repeat textures are aligned to their size, and the texture
 *               window the header gives tiles them
 *   palettes    jb_palette_write shows its colours where a bare
 *               jb_vram_load of the CLUT does not (the CLUT cache)
 *   writes      jb_texture_write of sub-rectangles keeps the texels beside
 *   destroy     space comes back; a texture is known by its fields
 *   reserve     jb_vram_reserve keeps textures out; overlaps refused
 *   layouts     the display buffers of each layout rule and widescreen
 *   full        VRAM full of textures, palettes with no room, 1024
 *               textures: JB_ERROR_OUT_OF_MEMORY with its detail, nothing
 *               changed
 *   errors      every documented error and its detail
 *   allocation  no call allocates after jb_create
 *   determinism two contexts given the same calls place the same
 *   random      a seeded run of creates, destroys, writes and reservations,
 *               checked after every step from the jb_texture fields alone
 *               (rules, no overlap, nothing over the buffers or the
 *               reservations) and every live texture's texels and palette
 *               read back intact now and then
 *   rgba        jb_palette_from_rgba's alpha and black rules
 */
#include "api_test.h"

static uint16_t vram[VRAM_PIXELS], before[VRAM_PIXELS];

static void read_all(jb_context *jb, uint16_t *dst)
{
    check("jb_vram_read", jb_vram_read(jb, 0, 0, VRAM_W, VRAM_H, dst), JB_OK);
}

static int per_pixel(int depth)
{
    return depth == JB_TEX_4BIT ? 4 : depth == JB_TEX_8BIT ? 2 : 1;
}

typedef struct box {
    int x, y, w, h;
} box;

/* The VRAM rectangles of a texture's texels and palette (w 0 for none),
   from its fields as the header defines them. */
static box texel_box(const jb_texture *t)
{
    int n = per_pixel(t->depth);
    box b;

    b.x = t->page_x + t->u / n;
    b.y = t->page_y + t->v;
    b.w = (t->width + n - 1) / n;
    b.h = t->height;
    return b;
}

static box clut_box(const jb_texture *t)
{
    box b;

    b.x = t->clut_x;
    b.y = t->clut_y;
    b.w = t->depth == JB_TEX_4BIT ? 16 : t->depth == JB_TEX_8BIT ? 256 : 0;
    b.h = 1;
    return b;
}

static int overlap(box a, box b)
{
    return a.w > 0 && b.w > 0 && a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h &&
           b.y < a.y + a.h;
}

/* Texel (x, y) of a texture, decoded from VRAM as the GPU reads a page. */
static unsigned texel_at(const uint16_t *v, const jb_texture *t, int x, int y)
{
    int n = per_pixel(t->depth), tu = t->u + x;
    unsigned p = v[(t->page_y + t->v + y) * VRAM_W + t->page_x + tu / n];

    if (t->depth == JB_TEX_4BIT)
        return p >> (tu % 4 * 4) & 15;
    if (t->depth == JB_TEX_8BIT)
        return p >> (tu % 2 * 8) & 255;
    return p;
}

/* The display buffers of a w x h display, by the header's rule. */
static int buffers(int w, int h, box out[2])
{
    out[0].x = 0, out[0].y = 0, out[0].w = w, out[0].h = h;
    out[1] = out[0];
    if (2 * h <= 512) {
        out[1].y = h;
        return 2;
    }
    if (2 * w <= 1024) {
        out[1].x = w;
        return 2;
    }
    return 1;
}

/* The placement rules of the header, for one texture. */
static void check_rules(const char *what, const jb_texture *t)
{
    char m[128];
    int n = per_pixel(t->depth);
    box b = texel_box(t), c = clut_box(t);
    int span = 256 / n;

    snprintf(m, sizeof m, "%s: page_x a multiple of 64 in 0..960", what);
    check(m, t->page_x % 64 == 0 && t->page_x >= 0 && t->page_x <= 960, 1);
    snprintf(m, sizeof m, "%s: page_y 0 or 256", what);
    check(m, t->page_y == 0 || t->page_y == 256, 1);
    snprintf(m, sizeof m, "%s: u a whole pixel", what);
    check(m, t->u % n, 0);
    snprintf(m, sizeof m, "%s: within the page span", what);
    check(m, b.x >= t->page_x && b.x + b.w <= t->page_x + span && b.x + b.w <= VRAM_W, 1);
    snprintf(m, sizeof m, "%s: within the page rows", what);
    check(m, t->v >= 0 && t->v + t->height <= 256, 1);
    snprintf(m, sizeof m, "%s: u + width within the page's 256 texels", what);
    check(m, t->u + t->width <= 256, 1);
    if (c.w != 0) {
        snprintf(m, sizeof m, "%s: CLUT x a multiple of 16", what);
        check(m, t->clut_x % 16 == 0 && t->clut_x + c.w <= VRAM_W && t->clut_y >= 0 &&
                     t->clut_y < VRAM_H,
              1);
        snprintf(m, sizeof m, "%s: palette clear of the texels", what);
        check(m, overlap(b, c), 0);
    } else {
        snprintf(m, sizeof m, "%s: no CLUT at 15 bits", what);
        check(m, t->clut_x == 0 && t->clut_y == 0, 1);
    }
    if (t->repeat) {
        snprintf(m, sizeof m, "%s: repeat aligned to its size", what);
        check(m, t->u % t->width == 0 && t->v % t->height == 0, 1);
    }
}

/* The frame's buffers, by the header's rule: those of the reserved width,
   the larger of w and, with widescreen, the 16:9 width. */
static int frame_buffers(int w, int h, int ws, box out[2])
{
    int wide = (h * 16 + 4) / 9;

    return buffers(ws && wide > w ? wide : w, h, out);
}

static void check_clear_of_buffers(const char *what, const jb_texture *t, int w, int h, int ws)
{
    box buf[2];
    int n = frame_buffers(w, h, ws, buf), i;
    char m[128];
    for (i = 0; i < n; ++i) {
        snprintf(m, sizeof m, "%s: texels clear of buffer %d", what, i);
        check(m, overlap(texel_box(t), buf[i]), 0);
        snprintf(m, sizeof m, "%s: palette clear of buffer %d", what, i);
        check(m, overlap(clut_box(t), buf[i]), 0);
    }
}

static jb_texture_desc desc(int w, int h, int depth, const void *texels, const uint16_t *pal,
                            int repeat)
{
    jb_texture_desc d;

    memset(&d, 0, sizeof d);
    d.width = w;
    d.height = h;
    d.depth = depth;
    d.texels = texels;
    d.palette = pal;
    d.repeat = repeat;
    return d;
}

static jb_texture make(jb_context *jb, const char *what, int w, int h, int depth,
                       const void *texels, const uint16_t *pal, int repeat)
{
    jb_texture_desc d = desc(w, h, depth, texels, pal, repeat);
    jb_texture t;
    jb_result r;

    memset(&t, 0, sizeof t);
    r = jb_texture_create(jb, &d, &t);
    check(what, r, JB_OK);
    if (r != JB_OK)
        printf("  (%s)\n", jb_error_string(jb));
    return t;
}

static void check_detail(const char *what, jb_context *jb, const char *want)
{
    check_str(what, jb_error_string(jb), want);
}

static int has(const char *s, const char *part)
{
    return strstr(s, part) != NULL;
}

static uint8_t idx[256 * 256];
static uint16_t pix[256 * 256], pal[256];

/* Seeded texels and palettes. */
static uint32_t hash(uint32_t a, uint32_t b)
{
    uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h;
}

static void fill_texels(uint32_t seed, int w, int h, int depth)
{
    int i;

    for (i = 0; i < w * h; ++i) {
        uint32_t v = hash(seed, (uint32_t)i);
        if (depth == JB_TEX_15BIT)
            pix[i] = (uint16_t)v;
        else
            idx[i] = (uint8_t)(depth == JB_TEX_4BIT ? v & 15 : v & 255);
    }
}

static void fill_palette(uint32_t seed, int n)
{
    int i;

    for (i = 0; i < n; ++i)
        pal[i] = (uint16_t)(1 + hash(seed ^ 0xabcdu, (uint32_t)i) % 0x7fffu); /* not 0, no bit 15 */
}

static const void *texels_of(int depth)
{
    return depth == JB_TEX_15BIT ? (const void *)pix : (const void *)idx;
}

static unsigned want_texel(int depth, int i)
{
    return depth == JB_TEX_15BIT ? pix[i] : idx[i];
}

/* Texels and palette of t in VRAM against those in idx/pix and pal. */
static void check_contents(const char *what, const uint16_t *v, const jb_texture *t)
{
    int x, y, bad = 0, entries = clut_box(t).w;
    char m[128];

    for (y = 0; y < t->height; ++y)
        for (x = 0; x < t->width; ++x)
            if (texel_at(v, t, x, y) != want_texel(t->depth, y * t->width + x))
                ++bad;
    snprintf(m, sizeof m, "%s: texels", what);
    check(m, bad, 0);
    for (x = 0, bad = 0; x < entries; ++x)
        if (v[t->clut_y * VRAM_W + t->clut_x + x] != pal[x])
            ++bad;
    snprintf(m, sizeof m, "%s: palette", what);
    check(m, bad, 0);
}

/* A textured raw rectangle of the whole texture at (0, 0) of VRAM. */
static void draw(jb_context *jb, const jb_texture *t, int w, int h)
{
    jb_rect r;

    memset(&r, 0, sizeof r);
    r.w = w;
    r.h = h;
    r.texture = t;
    r.raw = 1;
    r.u = t->u;
    r.v = t->v;
    check("jb_prim_rect", jb_prim_rect(jb, &r), JB_OK);
}

/* ------------------------------------------------------------------------ */

static void basics(int scale)
{
    jb_config cfg;
    jb_context *jb;
    jb_texture a, b, c;
    int x, y, bad;

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = scale;
    jb = must_create(&cfg);

    /* 4-bit, 37 x 20: 10 VRAM pixels a row, the last holding texel 36. */
    fill_texels(1, 37, 20, JB_TEX_4BIT);
    fill_palette(1, 16);
    a = make(jb, "basics: create 4-bit", 37, 20, JB_TEX_4BIT, idx, pal, 0);
    check("basics: 4-bit page_x", a.page_x, 320);
    check("basics: 4-bit page_y", a.page_y, 0);
    check("basics: 4-bit u, v", a.u == 0 && a.v == 0, 1);
    check("basics: 4-bit size", a.width == 37 && a.height == 20 && a.depth == 4, 1);
    check("basics: 4-bit CLUT at the bottom left", a.clut_x == 0 && a.clut_y == 511, 1);
    check("basics: repeat 0", a.repeat, 0);
    check_rules("basics 4-bit", &a);
    read_all(jb, vram);
    check_contents("basics 4-bit", vram, &a);
    /* The packing, pixel by pixel: low nibble first, padding 0. */
    for (y = 0, bad = 0; y < 20; ++y)
        for (x = 0; x < 10; ++x) {
            unsigned want = 0, k;
            for (k = 0; k < 4; ++k)
                if (x * 4 + (int)k < 37)
                    want |= (unsigned)idx[y * 37 + x * 4 + (int)k] << (4 * k);
            if (vram[y * VRAM_W + 320 + x] != want)
                ++bad;
        }
    check("basics: 4-bit packing", bad, 0);
    check("basics: nothing written beside the 4-bit texels", vram[330], 0);

    /* Drawn: each pixel its palette entry (raw, none is 0x0000). */
    check("draw area", jb_set_draw_area(jb, 0, 0, 319, 239), JB_OK);
    draw(jb, &a, 37, 20);
    read_all(jb, vram);
    for (y = 0, bad = 0; y < 20; ++y)
        for (x = 0; x < 37; ++x)
            if (vram[y * VRAM_W + x] != pal[idx[y * 37 + x]])
                ++bad;
    check("basics: 4-bit drawn", bad, 0);

    /* 8-bit, 33 x 5: beside the first, in the same page row (x 330,
       page 320, u 20); its palette above the first one, in column 0. */
    fill_texels(2, 33, 5, JB_TEX_8BIT);
    fill_palette(2, 256);
    b = make(jb, "basics: create 8-bit", 33, 5, JB_TEX_8BIT, idx, pal, 0);
    check("basics: 8-bit page_x", b.page_x, 320);
    check("basics: 8-bit u", b.u, 20);
    check("basics: 8-bit v", b.v, 0);
    check("basics: 8-bit CLUT", b.clut_x == 0 && b.clut_y == 510, 1);
    check_rules("basics 8-bit", &b);
    read_all(jb, vram);
    check_contents("basics 8-bit", vram, &b);
    for (y = 0, bad = 0; y < 5; ++y)
        for (x = 0; x < 17; ++x) {
            unsigned want = idx[y * 33 + x * 2];
            if (x * 2 + 1 < 33)
                want |= (unsigned)idx[y * 33 + x * 2 + 1] << 8;
            if (vram[y * VRAM_W + 330 + x] != want)
                ++bad;
        }
    check("basics: 8-bit packing", bad, 0);
    draw(jb, &b, 33, 5);
    read_all(jb, vram);
    for (y = 0, bad = 0; y < 5; ++y)
        for (x = 0; x < 33; ++x)
            if (vram[y * VRAM_W + x] != pal[idx[y * 33 + x]])
                ++bad;
    check("basics: 8-bit drawn", bad, 0);

    /* 15-bit, 5 x 3, after it: x 347, u 27; no CLUT. */
    fill_texels(3, 5, 3, JB_TEX_15BIT);
    for (x = 0; x < 15; ++x)
        pix[x] = (uint16_t)(pix[x] | 1); /* never 0x0000, which would not draw */
    c = make(jb, "basics: create 15-bit", 5, 3, JB_TEX_15BIT, pix, NULL, 0);
    check("basics: 15-bit page_x", c.page_x, 320);
    check("basics: 15-bit u", c.u, 27);
    check("basics: 15-bit CLUT 0", c.clut_x == 0 && c.clut_y == 0, 1);
    check_rules("basics 15-bit", &c);
    read_all(jb, vram);
    for (y = 0, bad = 0; y < 3; ++y)
        for (x = 0; x < 5; ++x)
            if (vram[y * VRAM_W + 347 + x] != pix[y * 5 + x])
                ++bad;
    check("basics: 15-bit texels", bad, 0);
    draw(jb, &c, 5, 3);
    read_all(jb, vram);
    for (y = 0, bad = 0; y < 3; ++y)
        for (x = 0; x < 5; ++x)
            if (vram[y * VRAM_W + x] != pix[y * 5 + x])
                ++bad;
    check("basics: 15-bit drawn", bad, 0);

    /* NULL texels and palette: all 0. */
    {
        jb_texture z = make(jb, "basics: create NULLs", 9, 9, JB_TEX_8BIT, NULL, NULL, 0);
        memset(idx, 0, sizeof idx);
        memset(pal, 0, sizeof pal);
        read_all(jb, vram);
        check_contents("basics NULLs", vram, &z);
    }
    jb_destroy(jb);
}

static void repeat(void)
{
    jb_context *jb = must_create(NULL);
    jb_texture odd, t, big;
    int x, y, bad, mx, my, ox, oy;

    /* Something odd first, so alignment has work to do. */
    odd = make(jb, "repeat: an odd one first", 7, 5, JB_TEX_4BIT, NULL, NULL, 0);
    (void)odd;
    fill_texels(4, 16, 16, JB_TEX_4BIT);
    fill_palette(4, 16);
    t = make(jb, "repeat: create 16 x 16", 16, 16, JB_TEX_4BIT, idx, pal, 1);
    check("repeat: flag", t.repeat, 1);
    check_rules("repeat 16", &t);
    check("repeat: u a multiple of 16, not 0", t.u % 16 == 0 && t.u > 0, 1);
    /* The window the header gives: mask (256 - 16) / 8, offset u / 8. */
    mx = (256 - t.width) / 8, my = (256 - t.height) / 8, ox = t.u / 8, oy = t.v / 8;
    check("repeat: window", jb_set_texture_window(jb, mx, my, ox, oy), JB_OK);
    check("draw area", jb_set_draw_area(jb, 0, 0, 319, 239), JB_OK);
    {
        jb_rect r;
        memset(&r, 0, sizeof r);
        r.w = 48, r.h = 40, r.texture = &t, r.raw = 1, r.u = 0, r.v = 5;
        check("repeat: rect", jb_prim_rect(jb, &r), JB_OK);
    }
    read_all(jb, vram);
    for (y = 0, bad = 0; y < 40; ++y)
        for (x = 0; x < 48; ++x)
            if (vram[y * VRAM_W + x] != pal[idx[((y + 5) % 16) * 16 + x % 16]])
                ++bad;
    check("repeat: tiled", bad, 0);
    check("repeat: window off", jb_set_texture_window(jb, 0, 0, 0, 0), JB_OK);

    /* Every power of two at every depth places aligned. */
    {
        static const int depths[3] = {JB_TEX_4BIT, JB_TEX_8BIT, JB_TEX_15BIT};
        int d, k;
        for (d = 0; d < 3; ++d)
            for (k = 8; k <= 128; k *= 2) {
                char m[64];
                jb_texture q;
                snprintf(m, sizeof m, "repeat %d-bit %d", depths[d], k);
                q = make(jb, m, k, k / 2 < 8 ? 8 : k / 2, depths[d], NULL, NULL, 1);
                check_rules(m, &q);
                check(m, jb_texture_destroy(jb, &q), JB_OK);
            }
    }
    big = make(jb, "repeat: 256 x 256", 256, 256, JB_TEX_4BIT, NULL, NULL, 1);
    check("repeat: 256 at the page's start", big.u == 0 && big.v == 0, 1);
    jb_destroy(jb);
}

static void palettes(void)
{
    jb_context *jb = must_create(NULL);
    uint16_t newpal[16];
    jb_texture t, c15;
    int x, bad;

    fill_texels(5, 16, 1, JB_TEX_4BIT);
    for (x = 0; x < 16; ++x)
        idx[x] = (uint8_t)x;
    fill_palette(5, 16);
    t = make(jb, "palettes: create", 16, 1, JB_TEX_4BIT, idx, pal, 0);
    check("draw area", jb_set_draw_area(jb, 0, 0, 319, 239), JB_OK);
    draw(jb, &t, 16, 1);
    for (x = 0; x < 16; ++x)
        newpal[x] = (uint16_t)(0x1234 + 0x111 * x);

    /* A bare jb_vram_load of the CLUT: the cache keeps the old colours. */
    check("palettes: bare load", jb_vram_load(jb, t.clut_x, t.clut_y, 16, 1, newpal), JB_OK);
    draw(jb, &t, 16, 1);
    read_all(jb, vram);
    for (x = 0, bad = 0; x < 16; ++x)
        if (vram[x] != pal[x])
            ++bad;
    check("palettes: the CLUT cache keeps the old colours", bad, 0);

    /* jb_palette_write: the new ones show. Entries 4..11 only. */
    check("palettes: restore", jb_palette_write(jb, &t, 0, 16, pal), JB_OK);
    check("palettes: write 4..11", jb_palette_write(jb, &t, 4, 8, newpal + 4), JB_OK);
    draw(jb, &t, 16, 1);
    read_all(jb, vram);
    for (x = 0, bad = 0; x < 16; ++x)
        if (vram[x] != (x >= 4 && x < 12 ? newpal[x] : pal[x]))
            ++bad;
    check("palettes: the new colours show", bad, 0);
    for (x = 0, bad = 0; x < 16; ++x)
        if (vram[t.clut_y * VRAM_W + t.clut_x + x] != (x >= 4 && x < 12 ? newpal[x] : pal[x]))
            ++bad;
    check("palettes: written in VRAM", bad, 0);

    /* Errors. */
    check("palettes: count 0", jb_palette_write(jb, &t, 3, 0, NULL), JB_OK);
    check("palettes: first -1", jb_palette_write(jb, &t, -1, 1, pal), JB_ERROR_INVALID_ARGUMENT);
    check_detail("palettes: first -1 detail", jb, "jb_palette_write: first -1 is outside 0..15");
    check("palettes: first 16", jb_palette_write(jb, &t, 16, 0, pal), JB_ERROR_INVALID_ARGUMENT);
    check("palettes: count 17", jb_palette_write(jb, &t, 0, 17, pal), JB_ERROR_INVALID_ARGUMENT);
    check("palettes: count past the end", jb_palette_write(jb, &t, 15, 2, pal),
          JB_ERROR_INVALID_ARGUMENT);
    check_detail("palettes: count detail", jb, "jb_palette_write: count 2 is outside 0..1");
    check("palettes: count -1", jb_palette_write(jb, &t, 0, -1, pal), JB_ERROR_INVALID_ARGUMENT);
    check("palettes: colors NULL", jb_palette_write(jb, &t, 0, 1, NULL),
          JB_ERROR_INVALID_ARGUMENT);
    check_detail("palettes: colors NULL detail", jb,
                 "jb_palette_write: colors is NULL but count is 1");
    check("palettes: texture NULL", jb_palette_write(jb, NULL, 0, 1, pal),
          JB_ERROR_INVALID_ARGUMENT);
    check("palettes: jb NULL", jb_palette_write(NULL, &t, 0, 1, pal), JB_ERROR_INVALID_ARGUMENT);
    c15 = make(jb, "palettes: 15-bit", 4, 4, JB_TEX_15BIT, NULL, NULL, 0);
    check("palettes: 15-bit", jb_palette_write(jb, &c15, 0, 1, pal), JB_ERROR_INVALID_ARGUMENT);
    check_detail("palettes: 15-bit detail", jb,
                 "jb_palette_write: a 15-bit texture has no palette");
    {
        jb_texture hand = t;
        hand.clut_x += 16;
        check("palettes: not one of the context's", jb_palette_write(jb, &hand, 0, 1, pal),
              JB_ERROR_INVALID_ARGUMENT);
        check("palettes: detail names it", has(jb_error_string(jb), "is not one"), 1);
    }
    /* 8-bit: 256 entries. */
    {
        jb_texture e = make(jb, "palettes: 8-bit", 4, 4, JB_TEX_8BIT, NULL, NULL, 0);
        check("palettes: 8-bit 255", jb_palette_write(jb, &e, 255, 1, pal), JB_OK);
        check("palettes: 8-bit 256", jb_palette_write(jb, &e, 0, 257, pal),
              JB_ERROR_INVALID_ARGUMENT);
    }
    jb_destroy(jb);
}

static void writes(void)
{
    static const int depths[3] = {JB_TEX_4BIT, JB_TEX_8BIT, JB_TEX_15BIT};
    jb_context *jb = must_create(NULL);
    uint8_t small8[64];
    uint16_t small16[64];
    int d, i, x, y;

    for (d = 0; d < 3; ++d) {
        int depth = depths[d];
        jb_texture t;
        char m[64];

        fill_texels(10 + (uint32_t)d, 13, 7, depth);
        fill_palette(10 + (uint32_t)d, depth == 4 ? 16 : 256);
        snprintf(m, sizeof m, "writes %d-bit: create", depth);
        t = make(jb, m, 13, 7, depth, texels_of(depth), pal, 0);
        /* Rectangles at every alignment, including the last, padded,
           pixel of a row. */
        for (i = 0; i < 12; ++i) {
            int wx = i % 5, wy = i % 3, ww = 1 + (i * 7) % (13 - wx), wh = 1 + i % (7 - wy), k;
            for (k = 0; k < ww * wh; ++k) {
                uint32_t v = hash(77 + (uint32_t)i, (uint32_t)k);
                small8[k] = (uint8_t)(depth == 4 ? v & 15 : v & 255);
                small16[k] = (uint16_t)v;
            }
            snprintf(m, sizeof m, "writes %d-bit: write %d", depth, i);
            check(m,
                  jb_texture_write(jb, &t, wx, wy, ww, wh,
                                   depth == 15 ? (const void *)small16 : (const void *)small8),
                  JB_OK);
            for (y = 0; y < wh; ++y)
                for (x = 0; x < ww; ++x) {
                    int at = (wy + y) * 13 + wx + x;
                    if (depth == 15)
                        pix[at] = small16[y * ww + x];
                    else
                        idx[at] = small8[y * ww + x];
                }
            read_all(jb, vram);
            check_contents(m, vram, &t);
        }
        /* The padding of a 4-bit row's last pixel stays 0. */
        if (depth == 4)
            for (y = 0; y < 7; ++y) {
                unsigned p = vram[(t.page_y + t.v + y) * VRAM_W + t.page_x + t.u / 4 + 3];
                check("writes: padding stays 0", p >> 4, 0);
            }
        snprintf(m, sizeof m, "writes %d-bit: x 13", depth);
        check(m, jb_texture_write(jb, &t, 13, 0, 1, 1, small8), JB_ERROR_INVALID_ARGUMENT);
        check(m, jb_texture_write(jb, &t, -1, 0, 1, 1, small8), JB_ERROR_INVALID_ARGUMENT);
        check(m, jb_texture_write(jb, &t, 0, 7, 1, 1, small8), JB_ERROR_INVALID_ARGUMENT);
        check(m, jb_texture_write(jb, &t, 3, 0, 11, 1, small8), JB_ERROR_INVALID_ARGUMENT);
        check(m, jb_texture_write(jb, &t, 0, 3, 1, 5, small8), JB_ERROR_INVALID_ARGUMENT);
        check(m, jb_texture_write(jb, &t, 0, 0, 0, 1, small8), JB_ERROR_INVALID_ARGUMENT);
        check(m, jb_texture_write(jb, &t, 0, 0, 1, 1, NULL), JB_ERROR_INVALID_ARGUMENT);
        check(m, jb_texture_write(jb, NULL, 0, 0, 1, 1, small8), JB_ERROR_INVALID_ARGUMENT);
        check(m, jb_texture_write(NULL, &t, 0, 0, 1, 1, small8), JB_ERROR_INVALID_ARGUMENT);
        if (depth == 4) {
            small8[0] = 1, small8[1] = 2, small8[2] = 16;
            check(m, jb_texture_write(jb, &t, 0, 0, 3, 1, small8), JB_ERROR_INVALID_ARGUMENT);
            check_detail("writes: 4-bit detail", jb,
                         "jb_texture_write: 4-bit texel (2, 0) is 16, above 15");
            read_all(jb, before);
            check_contents("writes: nothing written", before, &t);
        }
    }
    jb_destroy(jb);
}

static void destroy(void)
{
    jb_context *jb = must_create(NULL);
    jb_texture a, b, copy, again;

    a = make(jb, "destroy: a", 64, 64, JB_TEX_8BIT, NULL, NULL, 0);
    b = make(jb, "destroy: b", 64, 64, JB_TEX_8BIT, NULL, NULL, 0);
    check("destroy: b beside a", b.page_x == a.page_x + 64 || b.page_y != a.page_y ||
                                     b.v != a.v || b.u != a.u,
          1);
    copy = a;
    check("destroy: a copy", jb_texture_destroy(jb, &copy), JB_OK);
    check("destroy: twice", jb_texture_destroy(jb, &a), JB_ERROR_INVALID_ARGUMENT);
    check("destroy: twice detail", has(jb_error_string(jb), "jb_texture_destroy: the texture"), 1);
    again = make(jb, "destroy: again", 64, 64, JB_TEX_8BIT, NULL, NULL, 0);
    check("destroy: the space came back", again.page_x == a.page_x && again.u == a.u &&
                                              again.v == a.v && again.page_y == a.page_y &&
                                              again.clut_x == a.clut_x &&
                                              again.clut_y == a.clut_y,
          1);
    {
        jb_texture hand;
        memset(&hand, 0, sizeof hand);
        hand.page_x = 320;
        check("destroy: made by hand", jb_texture_destroy(jb, &hand), JB_ERROR_INVALID_ARGUMENT);
        hand = b;
        hand.repeat = 1;
        check("destroy: one field different", jb_texture_destroy(jb, &hand),
              JB_ERROR_INVALID_ARGUMENT);
    }
    check("destroy: NULL", jb_texture_destroy(jb, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_detail("destroy: NULL detail", jb, "jb_texture_destroy: texture is NULL");
    check("destroy: jb NULL", jb_texture_destroy(NULL, &b), JB_ERROR_INVALID_ARGUMENT);
    {
        /* Another context's texture is not this one's. */
        jb_context *other = must_create(NULL);
        check("destroy: another context's", jb_texture_destroy(other, &b),
              JB_ERROR_INVALID_ARGUMENT);
        jb_destroy(other);
    }
    check("destroy: b", jb_texture_destroy(jb, &b), JB_OK);
    jb_destroy(jb);
}

static void reserve(void)
{
    jb_context *jb = must_create(NULL);
    jb_texture t;

    check("reserve: a page column", jb_vram_reserve(jb, 320, 0, 64, 256), JB_OK);
    t = make(jb, "reserve: after it", 256, 256, JB_TEX_4BIT, NULL, NULL, 0);
    check("reserve: kept out", t.page_x, 384);
    check("reserve: over the buffers is fine", jb_vram_reserve(jb, 0, 0, 330, 10), JB_OK);
    check("reserve: over another reservation is fine", jb_vram_reserve(jb, 330, 10, 10, 10),
          JB_OK);
    check("reserve: over a texture", jb_vram_reserve(jb, 440, 100, 8, 8),
          JB_ERROR_INVALID_ARGUMENT);
    check_detail("reserve: over a texture detail", jb,
                 "jb_vram_reserve: (440, 100) 8 x 8 overlaps the texels of the texture at page "
                 "(384, 0), u, v (0, 0)");
    check("reserve: over a palette", jb_vram_reserve(jb, t.clut_x + 15, t.clut_y, 1, 1),
          JB_ERROR_INVALID_ARGUMENT);
    check("reserve: over a palette detail", has(jb_error_string(jb), "overlaps the palette"), 1);
    check("reserve: x -1", jb_vram_reserve(jb, -1, 0, 1, 1), JB_ERROR_INVALID_ARGUMENT);
    check_detail("reserve: x -1 detail", jb,
                 "jb_vram_reserve: position (-1, 0) is outside 0..1023 x 0..511");
    check("reserve: x 1024", jb_vram_reserve(jb, 1024, 0, 1, 1), JB_ERROR_INVALID_ARGUMENT);
    check("reserve: y 512", jb_vram_reserve(jb, 0, 512, 1, 1), JB_ERROR_INVALID_ARGUMENT);
    check("reserve: w 0", jb_vram_reserve(jb, 0, 0, 0, 1), JB_ERROR_INVALID_ARGUMENT);
    check("reserve: h 0", jb_vram_reserve(jb, 0, 0, 1, 0), JB_ERROR_INVALID_ARGUMENT);
    check("reserve: past the right", jb_vram_reserve(jb, 1000, 0, 25, 1),
          JB_ERROR_INVALID_ARGUMENT);
    check_detail("reserve: past the right detail", jb,
                 "jb_vram_reserve: size 25 x 1 is outside 1..24 x 1..512");
    check("reserve: past the bottom", jb_vram_reserve(jb, 0, 500, 1, 13),
          JB_ERROR_INVALID_ARGUMENT);
    check("reserve: to the corner", jb_vram_reserve(jb, 1000, 500, 24, 12), JB_OK);
    check("reserve: jb NULL", jb_vram_reserve(NULL, 0, 0, 1, 1), JB_ERROR_INVALID_ARGUMENT);
    /* A reservation writes nothing. */
    read_all(jb, before);
    /* Everything else but the palette's pixels: x 448 on, below the page
       column's first row, and the strip under the buffers. */
    check("reserve: everything else", jb_vram_reserve(jb, 448, 0, 576, 512), JB_OK);
    check("reserve: everything else", jb_vram_reserve(jb, 320, 256, 128, 256), JB_OK);
    check("reserve: everything else", jb_vram_reserve(jb, 16, 480, 304, 32), JB_OK);
    check("reserve: everything else", jb_vram_reserve(jb, 0, 480, 16, 31), JB_OK);
    read_all(jb, vram);
    check("reserve: no pixel written", memcmp(before, vram, sizeof vram) == 0, 1);
    {
        jb_texture_desc d = desc(16, 16, JB_TEX_15BIT, NULL, NULL, 0);
        jb_texture none;
        check("reserve: no room left", jb_texture_create(jb, &d, &none), JB_ERROR_OUT_OF_MEMORY);
    }
    jb_destroy(jb);
}

static void layouts(void)
{
    static const struct {
        int w, h, ws;
        int page_x, page_y; /* the first 64 x 256 (4-bit 256 x 256) texture; -1 none */
    } cases[] = {
        {320, 240, 0, 320, 0},  /* two buffers, one above the other */
        {320, 240, 1, 448, 0},  /* and 427 wide: past 427 to the next page column */
        {256, 240, 1, 448, 0},  /* 16:9 of 240 is 427 whatever the width */
        {640, 480, 0, 640, 0},  /* one buffer */
        {640, 240, 1, 640, 0},  /* 640 is wider than 427 */
        {368, 256, 0, 384, 0},  /* 2h = 512: two, above each other, rows 0..511 */
        {512, 300, 0, -1, -1},  /* side by side: rows 0..299 all taken */
        {1024, 512, 0, -1, -1}, /* one buffer, the whole of VRAM */
        {1, 1, 0, 0, 0},        /* tiny buffers at (0, 0) and (0, 1): the next column */
        /* Widescreen where the 16:9 width changes the rule: the frame
           keeps one buffer of the reserved width, 533 x 300, and nothing
           else (not the 512 x 300 pair the narrow width alone would
           have), so x 576 on is free. */
        {512, 300, 1, 576, 0},
        {400, 300, 1, 576, 0},  /* 400 x 300 alone: two, side by side, to 800 */
        {400, 300, 0, 832, 0},
        {384, 288, 1, -1, -1},  /* 512 x 288 reserved: 2w = 1024, side by side, rows 0..287 */
    };
    int i;

    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); ++i) {
        jb_config cfg;
        jb_context *jb;
        jb_texture_desc d = desc(256, 256, JB_TEX_4BIT, NULL, NULL, 0);
        jb_texture t;
        jb_result r;
        char m[96];

        memset(&cfg, 0, sizeof cfg);
        cfg.width = cases[i].w;
        cfg.height = cases[i].h;
        cfg.widescreen = cases[i].ws;
        jb = must_create(&cfg);
        memset(&t, 0, sizeof t);
        r = jb_texture_create(jb, &d, &t);
        snprintf(m, sizeof m, "layouts %dx%d%s", cases[i].w, cases[i].h, cases[i].ws ? " ws" : "");
        if (cases[i].page_x < 0) {
            check(m, r, JB_ERROR_OUT_OF_MEMORY);
            check_detail(m, jb, "jb_texture_create: no room in VRAM for a 256 x 256 4-bit texture");
        } else {
            check(m, r, JB_OK);
            check(m, t.page_x, cases[i].page_x == 0 && cases[i].w == 1 ? 64 : cases[i].page_x);
            check(m, t.page_y, cases[i].page_y);
            check_clear_of_buffers(m, &t, cases[i].w, cases[i].h, cases[i].ws);
        }
        /* Smaller ones fill what is left; none over a buffer. */
        {
            static jb_texture made[201];
            int n, k, f, bad = 0, x, y;
            k = 0;
            if (r == JB_OK)
                made[k++] = t;
            for (n = 0; n < 200; ++n) {
                jb_texture_desc e = desc(1 + n * 37 % 120, 1 + n * 53 % 200,
                                         n % 3 == 0 ? 4 : n % 3 == 1 ? 8 : 15, NULL, NULL, 0);
                if (jb_texture_create(jb, &e, &t) != JB_OK)
                    continue;
                check_rules(m, &t);
                check_clear_of_buffers(m, &t, cases[i].w, cases[i].h, cases[i].ws);
                made[k++] = t;
            }
            /* The frame's own drawing, at both widths when reserved, leaves
               every texel and palette entry (all 0 here) as it was. The
               creations that found no room are latched: taken first. */
            jb_check(jb, NULL);
            for (f = 0; f < 4; ++f) {
                if (cases[i].ws && f == 2)
                    jb_set_widescreen(jb, 0);
                check("layouts: frame", jb_begin_frame(jb, JB_RGB(255, 0, 255)), JB_OK);
                check("layouts: frame end", jb_end_frame(jb), JB_OK);
            }
            read_all(jb, vram);
            for (n = 0; n < k; ++n) {
                box b = texel_box(&made[n]), c = clut_box(&made[n]);
                for (y = b.y; y < b.y + b.h; ++y)
                    for (x = b.x; x < b.x + b.w; ++x)
                        bad += vram[y * VRAM_W + x] != 0;
                for (x = c.x; x < c.x + c.w; ++x)
                    bad += vram[c.y * VRAM_W + x] != 0;
            }
            snprintf(m, sizeof m, "layouts %dx%d%s: frames leave the textures", cases[i].w,
                     cases[i].h, cases[i].ws ? " ws" : "");
            check(m, bad, 0);
        }
        jb_destroy(jb);
    }
    {
        /* 512 x 300: a texture of up to 212 rows fits below the buffers. */
        jb_config cfg;
        jb_context *jb;
        jb_texture t;
        memset(&cfg, 0, sizeof cfg);
        cfg.width = 512, cfg.height = 300;
        jb = must_create(&cfg);
        t = make(jb, "layouts 512x300: 212 rows", 64, 212, JB_TEX_15BIT, NULL, NULL, 0);
        check("layouts 512x300: at row 300", t.page_y + t.v, 300);
        jb_destroy(jb);
    }
}

static void full(void)
{
    jb_context *jb = must_create(NULL);
    jb_texture_desc d = desc(256, 256, JB_TEX_4BIT, NULL, NULL, 0);
    jb_texture t, keep;
    int n = 0;

    /* 320 x 240: x 320..1023 is 11 page columns, in two page rows. */
    while (n < 100 && jb_texture_create(jb, &d, &t) == JB_OK) {
        check_rules("full", &t);
        ++n;
    }
    check("full: 22 textures of 256 x 256", n, 22);
    check_detail("full: detail", jb, "jb_texture_create: no room in VRAM for a 256 x 256 4-bit "
                                     "texture");
    memset(&keep, 0x5a, sizeof keep);
    t = keep;
    read_all(jb, before);
    check("full: again", jb_texture_create(jb, &d, &t), JB_ERROR_OUT_OF_MEMORY);
    read_all(jb, vram);
    check("full: nothing written", memcmp(before, vram, sizeof vram) == 0, 1);
    check("full: out unchanged", memcmp(&t, &keep, sizeof t) == 0, 1);
    /* (64 x 32 would fit in rows 480..511 under the buffers; 64 rows don't.) */
    d.repeat = 1, d.width = 64, d.height = 64, d.depth = JB_TEX_8BIT;
    check("full: repeat", jb_texture_create(jb, &d, &t), JB_ERROR_OUT_OF_MEMORY);
    check_detail("full: repeat detail", jb,
                 "jb_texture_create: no room in VRAM for a 64 x 64 8-bit repeat texture");
    jb_destroy(jb);

    /* Room for the texels but not the palette: the texels' room is given
       back. Everything is reserved but a 4 x 8 hole at (400, 0). */
    jb = must_create(NULL);
    check("full: reserve", jb_vram_reserve(jb, 320, 0, 80, 512), JB_OK);
    check("full: reserve", jb_vram_reserve(jb, 404, 0, 620, 512), JB_OK);
    check("full: reserve", jb_vram_reserve(jb, 400, 8, 4, 504), JB_OK);
    check("full: reserve", jb_vram_reserve(jb, 0, 480, 320, 32), JB_OK);
    d = desc(16, 8, JB_TEX_4BIT, NULL, NULL, 0);
    check("full: no palette room", jb_texture_create(jb, &d, &t), JB_ERROR_OUT_OF_MEMORY);
    check_detail("full: palette detail", jb,
                 "jb_texture_create: no room in VRAM for the 16-entry palette of a 16 x 8 4-bit "
                 "texture");
    t = make(jb, "full: the hole is free again", 4, 8, JB_TEX_15BIT, NULL, NULL, 0);
    check("full: in the hole", t.page_x + t.u == 400 && t.page_y + t.v == 0, 1);
    jb_destroy(jb);

    /* 1024 textures, the most a context holds. */
    jb = must_create(NULL);
    {
        jb_texture first;
        int i, ok = 1;
        d = desc(1, 1, JB_TEX_15BIT, NULL, NULL, 0);
        memset(&first, 0, sizeof first);
        for (i = 0; i < 1024; ++i) {
            if (jb_texture_create(jb, &d, &t) != JB_OK)
                ok = 0;
            if (i == 0)
                first = t;
        }
        check("full: 1024 textures", ok, 1);
        check("full: the 1025th", jb_texture_create(jb, &d, &t), JB_ERROR_OUT_OF_MEMORY);
        check_detail("full: 1025th detail", jb,
                     "jb_texture_create: the context already holds 1024 textures "
                     "(jb_texture_destroy frees one)");
        check("full: free one", jb_texture_destroy(jb, &first), JB_OK);
        check("full: then one more", jb_texture_create(jb, &d, &t), JB_OK);
    }
    jb_destroy(jb);
}

static void errors(void)
{
    jb_context *jb = must_create(NULL);
    jb_texture_desc d = desc(8, 8, JB_TEX_4BIT, NULL, NULL, 0);
    jb_texture t, keep;

    memset(&keep, 0x33, sizeof keep);
    t = keep;
    check("errors: jb NULL", jb_texture_create(NULL, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    check("errors: desc NULL", jb_texture_create(jb, NULL, &t), JB_ERROR_INVALID_ARGUMENT);
    check_detail("errors: desc NULL detail", jb, "jb_texture_create: desc is NULL");
    check("errors: out NULL", jb_texture_create(jb, &d, NULL), JB_ERROR_INVALID_ARGUMENT);
    check_detail("errors: out NULL detail", jb, "jb_texture_create: out is NULL");
    d.width = 0;
    check("errors: width 0", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    check_detail("errors: width 0 detail", jb,
                 "jb_texture_create: size 0 x 8 is outside 1..256 x 1..256");
    d.width = 257;
    check("errors: width 257", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    d.width = 8, d.height = 0;
    check("errors: height 0", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    d.height = 257;
    check("errors: height 257", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    d.height = 8, d.depth = 16;
    check("errors: depth 16", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    check_detail("errors: depth detail", jb, "jb_texture_create: depth 16 is not 4, 8 or 15");
    d.depth = 0;
    check("errors: depth 0", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    d.depth = JB_TEX_8BIT, d.repeat = 1, d.width = 12;
    check("errors: repeat 12", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    check_detail("errors: repeat detail", jb,
                 "jb_texture_create: a repeat texture needs power-of-two sizes 8..256, not 12 x 8");
    d.width = 4;
    check("errors: repeat 4", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    d.width = 8, d.height = 4;
    check("errors: repeat height 4", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    d.height = 512;
    check("errors: repeat 512", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    memset(idx, 3, 64);
    idx[8 * 2 + 5] = 200;
    d = desc(8, 8, JB_TEX_4BIT, idx, NULL, 0);
    check("errors: 4-bit texel 200", jb_texture_create(jb, &d, &t), JB_ERROR_INVALID_ARGUMENT);
    check_detail("errors: texel detail", jb, "jb_texture_create: 4-bit texel (5, 2) is 200, above 15");
    check("errors: out unchanged", memcmp(&t, &keep, sizeof t) == 0, 1);
    d.depth = JB_TEX_8BIT;
    check("errors: 200 is fine at 8 bits", jb_texture_create(jb, &d, &t), JB_OK);
    /* Nothing was placed by the failures: the first 4-bit one is where
       the first texture goes... after the 8-bit one just made. */
    check("errors: the 8-bit one first", t.page_x == 320 && t.u == 0 && t.v == 0, 1);
    jb_destroy(jb);
}

/* Allocations counted. */
static long allocs;

static void *count_alloc(void *user, size_t n)
{
    (void)user;
    ++allocs;
    return malloc(n);
}

static void count_free(void *user, void *p)
{
    (void)user;
    free(p);
}

static void allocation(void)
{
    jb_config cfg;
    jb_context *jb;
    jb_texture t[40];
    long after;
    int i;

    memset(&cfg, 0, sizeof cfg);
    cfg.alloc = count_alloc;
    cfg.free = count_free;
    jb = must_create(&cfg);
    after = allocs;
    for (i = 0; i < 40; ++i) {
        jb_texture_desc d = desc(1 + i * 7 % 200, 1 + i * 13 % 100, i % 2 ? 8 : 4, NULL, NULL, 0);
        jb_texture_create(jb, &d, &t[i]);
    }
    for (i = 0; i < 40; i += 2)
        jb_texture_destroy(jb, &t[i]);
    jb_palette_write(jb, &t[1], 0, 1, pal);
    memset(idx, 1, 16);
    jb_texture_write(jb, &t[1], 1, 0, 3, 1, idx);
    jb_vram_reserve(jb, 0, 0, 1, 1);
    check("allocation: none after jb_create", allocs, after);
    jb_destroy(jb);
}

static void determinism(void)
{
    jb_context *a = must_create(NULL), *b = must_create(NULL);
    int i, same = 1, freed = 0;

    for (i = 0; i < 300; ++i) {
        jb_texture ta, tb, ca, cb;
        int k = (int)(hash(99, (uint32_t)i) % 1000);
        jb_texture_desc d = desc(1 + k % 97, 1 + k % 61, k % 3 == 0 ? 4 : k % 3 == 1 ? 8 : 15,
                                 NULL, NULL, k % 7 == 0);
        if (d.repeat)
            d.width = 8 << (k % 4), d.height = 8 << (k % 5);
        memset(&ta, 0, sizeof ta);
        memset(&tb, 0, sizeof tb);
        if (jb_texture_create(a, &d, &ta) != jb_texture_create(b, &d, &tb))
            same = 0;
        if (ta.owner == tb.owner && ta.owner != NULL)
            same = 0; /* owners differ between contexts (jb_texture_destroy) */
        /* Compared on copies without their owners: ids are the same, as the
           places are. The live handles are the ones destroyed. */
        ca = ta, cb = tb;
        ca.owner = cb.owner = NULL;
        if (memcmp(&ca, &cb, sizeof ca) != 0)
            same = 0;
        if (k % 4 == 0 && ta.id != 0) {
            check("determinism: destroy in a", jb_texture_destroy(a, &ta), JB_OK);
            check("determinism: destroy in b", jb_texture_destroy(b, &tb), JB_OK);
            ++freed;
        }
    }
    check("determinism: the same places, after frees too", same, 1);
    check("determinism: textures freed along the way", freed > 50, 1);
    jb_destroy(a);
    jb_destroy(b);
}

/* ------------------------------------------------------------------------ */
/* A random run checked from the fields */

typedef struct live {
    jb_texture t;
    uint32_t seed; /* texels and palette: fill_texels/fill_palette */
} live;

static live lives[1100];
static box reserved[64];

static void random_run(int layout, uint32_t seed)
{
    static const int sizes[][3] = {{320, 240, 0}, {320, 240, 1}, {640, 480, 0}, {512, 256, 0}};
    jb_config cfg;
    jb_context *jb;
    int n_live = 0, n_res = 0, step, i, j;
    gen g;
    box buf[4];
    int n_buf;

    memset(&cfg, 0, sizeof cfg);
    cfg.width = sizes[layout][0];
    cfg.height = sizes[layout][1];
    cfg.widescreen = sizes[layout][2];
    cfg.scale = layout == 3 ? 2 : 1;
    jb = must_create(&cfg);
    g.s = seed, g.cx = g.cy = 0;
    n_buf = frame_buffers(cfg.width, cfg.height, cfg.widescreen, buf);

    for (step = 0; step < 700; ++step) {
        int k = rnd_below(&g, 100);
        char m[64];
        snprintf(m, sizeof m, "random %d step %d", layout, step);
        if (k < 55) {
            int depth = rnd_below(&g, 3) == 0 ? 4 : rnd_below(&g, 2) ? 8 : 15;
            int rep = rnd_below(&g, 5) == 0, w, h;
            jb_texture_desc d;
            jb_texture t;
            jb_result r;
            uint32_t s = (uint32_t)step * 31u + seed;
            if (rep) {
                w = 8 << rnd_below(&g, 6);
                h = 8 << rnd_below(&g, 6);
            } else {
                w = rnd_below(&g, 6) == 0 ? 1 + rnd_below(&g, 256) : 1 + rnd_below(&g, 48);
                h = rnd_below(&g, 6) == 0 ? 1 + rnd_below(&g, 256) : 1 + rnd_below(&g, 48);
            }
            fill_texels(s, w, h, depth);
            fill_palette(s, depth == 4 ? 16 : 256);
            d = desc(w, h, depth, texels_of(depth), depth == 15 ? NULL : pal, rep);
            r = jb_texture_create(jb, &d, &t);
            if (r == JB_OK) {
                lives[n_live].t = t;
                lives[n_live].seed = s;
                ++n_live;
                check_rules(m, &t);
            } else {
                check(m, r, JB_ERROR_OUT_OF_MEMORY);
                check(m, has(jb_error_string(jb), "no room in VRAM for"), 1);
            }
        } else if (k < 85 && n_live > 0) {
            int at = rnd_below(&g, n_live);
            check(m, jb_texture_destroy(jb, &lives[at].t), JB_OK);
            lives[at] = lives[--n_live];
        } else if (k < 95 && n_live > 0) {
            /* rewrite the whole of one texture with new texels */
            int at = rnd_below(&g, n_live);
            live *l = &lives[at];
            l->seed = l->seed * 7u + 1u;
            fill_texels(l->seed, l->t.width, l->t.height, l->t.depth);
            check(m, jb_texture_write(jb, &l->t, 0, 0, l->t.width, l->t.height,
                                      texels_of(l->t.depth)),
                  JB_OK);
            if (l->t.depth != 15) {
                /* and its palette, from its old seed's entries to the new */
                fill_palette(l->seed, l->t.depth == 4 ? 16 : 256);
                check(m, jb_palette_write(jb, &l->t, 0, l->t.depth == 4 ? 16 : 256, pal), JB_OK);
            } else {
                /* a 15-bit texture's palette seed is unused */
            }
        } else if (n_res < 64) {
            box r;
            jb_result res;
            int over = 0;
            r.x = rnd_below(&g, 1024);
            r.y = rnd_below(&g, 512);
            r.w = 1 + rnd_below(&g, 1024 - r.x < 64 ? 1024 - r.x : 64);
            r.h = 1 + rnd_below(&g, 512 - r.y < 64 ? 512 - r.y : 64);
            for (i = 0; i < n_live; ++i)
                if (overlap(r, texel_box(&lives[i].t)) || overlap(r, clut_box(&lives[i].t)))
                    over = 1;
            res = jb_vram_reserve(jb, r.x, r.y, r.w, r.h);
            check(m, res, over ? JB_ERROR_INVALID_ARGUMENT : JB_OK);
            if (res == JB_OK)
                reserved[n_res++] = r;
        }
        /* The rules, from the fields alone. */
        for (i = 0; i < n_live; ++i) {
            box ti = texel_box(&lives[i].t), ci = clut_box(&lives[i].t);
            for (j = i + 1; j < n_live; ++j) {
                box tj = texel_box(&lives[j].t), cj = clut_box(&lives[j].t);
                if (overlap(ti, tj) || overlap(ti, cj) || overlap(ci, tj) || overlap(ci, cj))
                    check(m, 1, 0);
            }
            for (j = 0; j < n_buf; ++j)
                if (overlap(ti, buf[j]) || overlap(ci, buf[j]))
                    check(m, 2, 0);
            for (j = 0; j < n_res; ++j)
                if (overlap(ti, reserved[j]) || overlap(ci, reserved[j]))
                    check(m, 3, 0);
        }
        if (step % 50 == 49) {
            read_all(jb, vram);
            for (i = 0; i < n_live; ++i) {
                fill_texels(lives[i].seed, lives[i].t.width, lives[i].t.height, lives[i].t.depth);
                fill_palette(lives[i].seed, lives[i].t.depth == 4 ? 16 : 256);
                check_contents(m, vram, &lives[i].t);
            }
        }
    }
    jb_destroy(jb);
}

static void rgba(void)
{
    static const uint8_t in[] = {
        255, 255, 255, 255, /* white, opaque */
        0,   0,   0,   255, /* black, opaque: 0x8000, not transparent */
        7,   7,   7,   200, /* black in 15 bits too */
        0,   0,   0,   0,   /* transparent */
        255, 0,   0,   0,   /* any colour with alpha 0 */
        8,   16,  248, 127, /* semi-transparent */
        8,   16,  248, 128, /* opaque */
        0,   0,   0,   1,   /* black, semi-transparent */
        15,  23,  31,  255, /* each channel's top 5 bits */
    };
    static const uint16_t want[] = {0x7fff, 0x8000, 0x8000, 0x0000, 0x0000,
                                    0x8000 | 1 | 2 << 5 | 31 << 10, 1 | 2 << 5 | 31 << 10,
                                    0x8000, 1 | 2 << 5 | 3 << 10};
    uint16_t out[9];
    int i;

    check("rgba", jb_palette_from_rgba(in, 9, out), JB_OK);
    for (i = 0; i < 9; ++i)
        check("rgba: colour", out[i], want[i]);
    check("rgba: count 0", jb_palette_from_rgba(NULL, 0, NULL), JB_OK);
    check("rgba: rgba NULL", jb_palette_from_rgba(NULL, 1, out), JB_ERROR_INVALID_ARGUMENT);
    check("rgba: out NULL", jb_palette_from_rgba(in, 1, NULL), JB_ERROR_INVALID_ARGUMENT);
}

/* ------------------------------------------------------------------------ */
/* Placement and lifetime cases (each failed before its fix) */

static uint16_t pixel_at(jb_context *jb, int x, int y)
{
    uint16_t p = 0;

    jb_vram_read(jb, x, y, 1, 1, &p);
    return p;
}

/* A texture made where a destroyed one's palette was is drawn with
   its own palette, not the one the CLUT cache kept, with no clear sent by
   the program. */
static void stale_clut(void)
{
    jb_context *jb = must_create(NULL);
    uint8_t one = 1;
    uint16_t red[16] = {0, 0x001f}, green[16] = {0, 0x03e0};
    jb_texture a, b;

    check("draw area", jb_set_draw_area(jb, 0, 0, 319, 239), JB_OK);
    a = make(jb, "red", 1, 1, JB_TEX_4BIT, &one, red, 0);
    draw(jb, &a, 1, 1);
    check("stale clut: red drawn", pixel_at(jb, 0, 0), 0x001f);
    check("stale clut: destroy", jb_texture_destroy(jb, &a), JB_OK);
    b = make(jb, "green", 1, 1, JB_TEX_4BIT, &one, green, 0);
    check("stale clut: the same CLUT place", b.clut_x == a.clut_x && b.clut_y == a.clut_y, 1);
    draw(jb, &b, 1, 1);
    check("stale clut: green drawn, not the cached red", pixel_at(jb, 0, 0), 0x03e0);
    jb_destroy(jb);
}

/* A copy of a destroyed texture's handle does not name the texture
   made in its place, and another context's handle with the same fields
   does not name this context's. */
static void identity(void)
{
    jb_context *jb = must_create(NULL), *other = must_create(NULL);
    jb_texture a, saved, b, oa;
    uint16_t zpal[16] = {0};
    uint8_t texel = 1;

    a = make(jb, "a", 8, 8, JB_TEX_4BIT, NULL, NULL, 0);
    check("identity: id not 0", a.id != 0, 1);
    saved = a;
    check("identity: destroy a", jb_texture_destroy(jb, &a), JB_OK);
    b = make(jb, "b in a's place", 8, 8, JB_TEX_4BIT, NULL, NULL, 0);
    check("identity: same place", b.page_x == a.page_x && b.u == a.u && b.v == a.v &&
                                b.clut_x == a.clut_x && b.clut_y == a.clut_y,
          1);
    check("identity: another id", b.id != a.id, 1);
    check("identity: destroy by the old handle", jb_texture_destroy(jb, &saved),
          JB_ERROR_INVALID_ARGUMENT);
    check("identity: palette by the old handle", jb_palette_write(jb, &saved, 0, 1, zpal),
          JB_ERROR_INVALID_ARGUMENT);
    check("identity: texels by the old handle", jb_texture_write(jb, &saved, 0, 0, 1, 1, &texel),
          JB_ERROR_INVALID_ARGUMENT);
    /* The same calls on another context: the same place, another id. */
    oa = make(other, "other a", 8, 8, JB_TEX_4BIT, NULL, NULL, 0);
    check("identity: other context, same place", oa.page_x == b.page_x && oa.u == b.u &&
                                               oa.v == b.v && oa.clut_x == b.clut_x,
          1);
    check("identity: other context, its own owner", oa.owner == other && b.owner == jb, 1);
    check("identity: destroy another context's", jb_texture_destroy(jb, &oa),
          JB_ERROR_INVALID_ARGUMENT);
    check("identity: palette of another context's", jb_palette_write(jb, &oa, 0, 1, zpal),
          JB_ERROR_INVALID_ARGUMENT);
    check("identity: texels of another context's", jb_texture_write(jb, &oa, 0, 0, 1, 1, &texel),
          JB_ERROR_INVALID_ARGUMENT);
    /* b is untouched and still its own. */
    check("identity: b still live", jb_texture_destroy(jb, &b), JB_OK);
    check("identity: other's still live", jb_texture_destroy(other, &oa), JB_OK);
    jb_destroy(jb);
    jb_destroy(other);
}

/* The texels' first place takes the palette's only place; another
   place for the texels leaves it. Holes at x 640..655 and 673..688 of
   row 2; everything else reserved. A 64 x 1 4-bit texture (16 pixels) fits
   either hole; its palette fits only at 640 (x a multiple of 16). */
static void pair_search(void)
{
    jb_context *jb = must_create(NULL);
    jb_texture t, keep;
    jb_texture_desc d = desc(64, 1, JB_TEX_4BIT, NULL, NULL, 0);

    check("pair search: reserve", jb_vram_reserve(jb, 0, 0, 1024, 2), JB_OK);
    check("pair search: reserve", jb_vram_reserve(jb, 0, 3, 1024, 509), JB_OK);
    check("pair search: reserve", jb_vram_reserve(jb, 0, 2, 640, 1), JB_OK);
    check("pair search: reserve", jb_vram_reserve(jb, 656, 2, 17, 1), JB_OK);
    check("pair search: reserve", jb_vram_reserve(jb, 689, 2, 335, 1), JB_OK);
    t = make(jb, "fits as a pair", 64, 1, JB_TEX_4BIT, NULL, NULL, 0);
    check("pair search: texels in the second hole",
          t.page_x + t.u / 4 == 673 && t.page_y + t.v == 2, 1);
    check("pair search: palette in the first", t.clut_x == 640 && t.clut_y == 2, 1);
    check_rules("pair search", &t);
    /* Now nothing fits: refused, with the palette named, nothing taken. */
    memset(&keep, 0x11, sizeof keep);
    check("pair search: destroy", jb_texture_destroy(jb, &t), JB_OK);
    t = make(jb, "again", 64, 1, JB_TEX_4BIT, NULL, NULL, 0);
    check("pair search: the same pair again", t.clut_x == 640 && t.page_x + t.u / 4 == 673, 1);
    keep = t;
    check("pair search: no room left", jb_texture_create(jb, &d, &t), JB_ERROR_OUT_OF_MEMORY);
    check("pair search: out unchanged", memcmp(&t, &keep, sizeof t) == 0, 1);
    /* With texels room only (a 15-bit texture needs no palette). */
    jb_destroy(jb);
    jb = must_create(NULL);
    check("pair search: reserve all but one row piece", jb_vram_reserve(jb, 0, 0, 1024, 2), JB_OK);
    check("pair search: reserve", jb_vram_reserve(jb, 0, 3, 1024, 509), JB_OK);
    check("pair search: reserve", jb_vram_reserve(jb, 0, 2, 641, 1), JB_OK);
    check("pair search: reserve", jb_vram_reserve(jb, 657, 2, 367, 1), JB_OK);
    check("pair search: texels but no palette", jb_texture_create(jb, &d, &t),
          JB_ERROR_OUT_OF_MEMORY);
    check_detail("palette detail", jb,
                 "jb_texture_create: no room in VRAM for the 16-entry palette of a 64 x 1 4-bit "
                 "texture");
    t = make(jb, "the hole is still free", 16, 1, JB_TEX_15BIT, NULL, NULL, 0);
    check("pair search: in the hole", t.page_x + t.u == 641, 1);
    jb_destroy(jb);
}

/* The pair search at random: VRAM reserved but for a few small holes; a 4-bit texture
   is made exactly when some placement of its texels and palette fits
   together (worked out here by trying every pair). */
static unsigned char holes[VRAM_H][VRAM_W];

static void pair_search_random(void)
{
    gen g;
    int trial, made = 0, refused = 0;

    g.s = 4242;
    g.cx = g.cy = 0;
    for (trial = 0; trial < 150; ++trial) {
        jb_context *jb = must_create(NULL);
        int n = 1 + rnd_below(&g, 5), k, x, y, w = 4 * (1 + rnd_below(&g, 16)), h;
        int tw, exists = 0, tx, ty, px, py;
        jb_texture_desc d;
        jb_texture t;
        jb_result r;
        char m[64];

        h = 1 + rnd_below(&g, 3);
        tw = w / 4;
        memset(holes, 0, sizeof holes);
        for (k = 0; k < n; ++k) {
            int hx = 320 + rnd_below(&g, 600), hy = rnd_below(&g, 500);
            int hw = 1 + rnd_below(&g, 40), hh = 1 + rnd_below(&g, 4);
            for (y = hy; y < hy + hh && y < VRAM_H; ++y)
                for (x = hx; x < hx + hw && x < VRAM_W; ++x)
                    holes[y][x] = 1;
        }
        /* Reserve every run of non-hole pixels, row by row. */
        for (y = 0; y < VRAM_H; ++y)
            for (x = 0; x < VRAM_W;) {
                int x1 = x;
                while (x1 < VRAM_W && !holes[y][x1])
                    ++x1;
                if (x1 > x)
                    jb_vram_reserve(jb, x, y, x1 - x, 1);
                x = x1;
                while (x < VRAM_W && holes[y][x])
                    ++x;
            }
        /* Every pair, by brute force. */
        for (ty = 0; ty + h <= VRAM_H && !exists; ++ty)
            for (tx = 0; tx + tw <= VRAM_W && !exists; ++tx) {
                int ok = ty / 256 == (ty + h - 1) / 256 && tx + tw <= (tx & ~63) + 64;
                for (y = ty; ok && y < ty + h; ++y)
                    for (x = tx; ok && x < tx + tw; ++x)
                        ok = holes[y][x];
                if (!ok)
                    continue;
                for (py = 0; py < VRAM_H && !exists; ++py)
                    for (px = 0; px + 16 <= VRAM_W && !exists; px += 16) {
                        int free_p = 1;
                        for (x = px; free_p && x < px + 16; ++x)
                            free_p = holes[py][x] &&
                                     !(py >= ty && py < ty + h && x >= tx && x < tx + tw);
                        exists = free_p;
                    }
            }
        d = desc(w, h, JB_TEX_4BIT, NULL, NULL, 0);
        r = jb_texture_create(jb, &d, &t);
        snprintf(m, sizeof m, "pair search %d: made exactly when a pair fits", trial);
        check(m, r, exists ? JB_OK : JB_ERROR_OUT_OF_MEMORY);
        if (r == JB_OK) {
            box b = texel_box(&t), c = clut_box(&t);
            int bad = 0;
            ++made;
            for (y = b.y; y < b.y + b.h; ++y)
                for (x = b.x; x < b.x + b.w; ++x)
                    bad += !holes[y][x];
            for (x = c.x; x < c.x + c.w; ++x)
                bad += !holes[c.y][x];
            check(m, bad + overlap(b, c), 0);
        } else {
            ++refused;
        }
        jb_destroy(jb);
    }
    check("pair search random: pair search: some made", made > 10, 1);
    check("pair search random: pair search: some refused", refused > 10, 1);
}

/* With the mask set bit on, a write of one 4-bit texel changes that
   texel alone; and creation writes exactly too. The settings are put
   back. */
static void masked_writes(void)
{
    jb_context *jb = must_create(NULL);
    uint8_t four[4] = {1, 2, 3, 4}, five = 5;
    uint16_t p5[16] = {0, 0x1234};
    jb_texture t;
    uint32_t stat;

    check("masked writes: mask set", jb_set_mask(jb, 1, 0), JB_OK);
    stat = jb_gpustat(jb);
    t = make(jb, "create masked", 4, 1, JB_TEX_4BIT, four, p5, 0);
    check("masked writes: created exactly", pixel_at(jb, t.page_x + t.u / 4, t.page_y + t.v),
          0x4321);
    check("masked writes: palette exactly", pixel_at(jb, t.clut_x + 1, t.clut_y), 0x1234);
    check("masked writes: write one texel", jb_texture_write(jb, &t, 0, 0, 1, 1, &five), JB_OK);
    check("masked writes: only that texel", pixel_at(jb, t.page_x + t.u / 4, t.page_y + t.v),
          0x4325);
    check("masked writes: palette write", jb_palette_write(jb, &t, 2, 1, p5 + 1), JB_OK);
    check("masked writes: palette written exactly", pixel_at(jb, t.clut_x + 2, t.clut_y), 0x1234);
    check("masked writes: mask settings put back", jb_gpustat(jb), stat);
    /* The check bit would keep old masked pixels: a texture made over
       VRAM whose pixels have bit 15 is still written. */
    {
        jb_context *c = must_create(NULL);
        uint16_t old[16];
        jb_texture u;
        int k;
        for (k = 0; k < 16; ++k)
            old[k] = 0xffff;
        check("masked writes: old pixels", jb_vram_load(c, 320, 0, 16, 1, old), JB_OK);
        check("masked writes: mask check", jb_set_mask(c, 0, 1), JB_OK);
        u = make(c, "over masked pixels", 64, 1, JB_TEX_4BIT, NULL, NULL, 0);
        check("masked writes: at 320", u.page_x + u.u / 4, 320);
        check("masked writes: written over them", pixel_at(c, 320, 0), 0);
        jb_destroy(c);
    }
    jb_destroy(jb);
}

/* NULL texels at 4 bits are index 0, which shows palette entry 0. */
static void null_indices(void)
{
    jb_context *jb = must_create(NULL);
    uint16_t p8[16] = {0x001f};
    jb_texture t;

    check("draw area", jb_set_draw_area(jb, 0, 0, 319, 239), JB_OK);
    t = make(jb, "NULL texels, entry 0 red", 1, 1, JB_TEX_4BIT, NULL, p8, 0);
    draw(jb, &t, 1, 1);
    check("null indices: entry 0 shows", pixel_at(jb, 0, 0), 0x001f);
    t = make(jb, "NULL texels and palette", 1, 1, JB_TEX_4BIT, NULL, NULL, 0);
    draw(jb, &t, 1, 1);
    check("null indices: transparent with a NULL palette", pixel_at(jb, 0, 0), 0x001f);
    jb_destroy(jb);
}

/* ------------------------------------------------------------------------ */
/* Texture handles across contexts and the font's handle */

/*
 * Two live contexts placed (by the allocator) at addresses where
 * the ids of the first scheme (a 32-bit mix of the context's address plus
 * a count) were the same: each makes its first texture in the same place,
 * and neither may take the other's handle. The search looks for such a
 * pair among aligned addresses of an arena, by the old mix, which has an
 * expected 32 pairs among 2^19 addresses.
 */
static unsigned char *arena;
static unsigned char *serve_next; /* where the next context goes, then NULL */
static void *served[2];          /* the arena places handed out */
static int nserved;

static void *arena_alloc(void *user, size_t n)
{
    (void)user;
    if (serve_next != NULL && n <= 4096 && nserved < 2) {
        void *p = serve_next;
        serve_next = NULL;
        served[nserved++] = p;
        return p;
    }
    return malloc(n);
}

/* The arena's places are not malloc's: kept, and known by equality alone
   (ordering unrelated pointers is undefined). */
static void arena_free(void *user, void *p)
{
    (void)user;
    if (p != served[0] && p != served[1])
        free(p);
}

static uint32_t old_mix(const void *at)
{
    uint64_t a = (uint64_t)(uintptr_t)at;

    a ^= a >> 29;
    a *= 0xbf58476d1ce4e5b9ull;
    a ^= a >> 32;
    return (uint32_t)a;
}

typedef struct {
    uint32_t mix, k;
} mixed;

static int by_mix(const void *a, const void *b)
{
    const mixed *x = a, *y = b;

    return x->mix < y->mix ? -1 : x->mix > y->mix ? 1 : x->k < y->k ? -1 : x->k > y->k;
}

static void owner_collision(void)
{
    enum { N = 1 << 19 };
    unsigned char *base;
    mixed *m = (mixed *)must_alloc(sizeof(mixed) * N);
    uint32_t k, k1 = 0, k2 = 0;
    int found = 0;

    arena = (unsigned char *)must_alloc((16u << 19) + 8192);
    served[0] = served[1] = NULL;
    nserved = 0;
    base = arena + (16 - ((uintptr_t)arena & 15)) % 16;
    for (k = 0; k < N; ++k) {
        m[k].mix = old_mix(base + (size_t)k * 16);
        m[k].k = k;
    }
    qsort(m, N, sizeof *m, by_mix);
    for (k = 0; k + 1 < N && !found; ++k)
        if (m[k].mix == m[k + 1].mix && m[k + 1].k - m[k].k >= 256)
            k1 = m[k].k, k2 = m[k + 1].k, found = 1;
    check("owner collision: an old-mix collision in the arena", found, 1);
    if (found) {
        jb_config cfg;
        jb_context *a, *b;
        jb_texture ta, tb;
        jb_texture_desc d = desc(1, 1, JB_TEX_15BIT, NULL, NULL, 0);
        uint16_t texel = 0x1234;
        memset(&cfg, 0, sizeof cfg);
        cfg.alloc = arena_alloc;
        cfg.free = arena_free;
        serve_next = base + (size_t)k1 * 16;
        a = must_create(&cfg);
        serve_next = base + (size_t)k2 * 16;
        b = must_create(&cfg);
        check("owner collision: the contexts where they were put",
              (unsigned char *)a == base + (size_t)k1 * 16 &&
                  (unsigned char *)b == base + (size_t)k2 * 16,
              1);
        check("owner collision: a's texture", jb_texture_create(a, &d, &ta), JB_OK);
        check("owner collision: b's texture", jb_texture_create(b, &d, &tb), JB_OK);
        check("owner collision: the same place and id", ta.page_x == tb.page_x && ta.u == tb.u &&
                                                   ta.v == tb.v && ta.id == tb.id,
              1);
        check("owner collision: b refuses a's handle (write)",
              jb_texture_write(b, &ta, 0, 0, 1, 1, &texel),
              JB_ERROR_INVALID_ARGUMENT);
        check("owner collision: b refuses a's handle (destroy)", jb_texture_destroy(b, &ta),
              JB_ERROR_INVALID_ARGUMENT);
        check("owner collision: a refuses b's", jb_texture_destroy(a, &tb),
              JB_ERROR_INVALID_ARGUMENT);
        check("owner collision: each takes its own", jb_texture_destroy(a, &ta) == JB_OK &&
                                               jb_texture_destroy(b, &tb) == JB_OK,
              1);
        jb_destroy(a);
        jb_destroy(b);
    }
    free(m);
    free(arena);
    arena = NULL;
}

/* A handle equal to the library's font is refused by the public
   writes, as by destroy. The first text a fresh context draws places the
   font first: page (320, 0), 128 x 48, CLUT (0, 511), id 1. */
static void font_handle(void)
{
    jb_context *jb = must_create(NULL);
    jb_texture f;
    uint16_t colour = 0x7fff;
    uint8_t texel = 1;

    jb_begin_frame(jb, 0);
    check("font handle: text", jb_draw_text(jb, 0, 0, JB_RGB(255, 255, 255), "A"), JB_OK);
    jb_end_frame(jb);
    memset(&f, 0, sizeof f);
    f.page_x = 320, f.depth = JB_TEX_4BIT, f.clut_x = 0, f.clut_y = 511;
    f.width = 128, f.height = 48, f.id = 1, f.owner = jb;
    check("font handle: destroy refuses the font", jb_texture_destroy(jb, &f),
          JB_ERROR_INVALID_ARGUMENT);
    check_detail("destroy detail", jb, "jb_texture_destroy: that texture is the library's font");
    check("font handle: palette write refuses the font", jb_palette_write(jb, &f, 1, 1, &colour),
          JB_ERROR_INVALID_ARGUMENT);
    check_detail("palette detail", jb, "jb_palette_write: that texture is the library's font");
    check("font handle: texel write refuses the font", jb_texture_write(jb, &f, 0, 0, 1, 1, &texel),
          JB_ERROR_INVALID_ARGUMENT);
    check_detail("texel detail", jb, "jb_texture_write: that texture is the library's font");
    check("font handle: the font's palette as it was",
          (long)(read_all(jb, vram), vram[511 * VRAM_W + 1]),
          0x4210);
    jb_destroy(jb);
}

int main(void)
{
    int layout;

    owner_collision();
    font_handle();

    stale_clut();
    identity();
    pair_search();
    pair_search_random();
    masked_writes();
    null_indices();

    basics(1);
    basics(2);
    repeat();
    palettes();
    writes();
    destroy();
    reserve();
    layouts();
    full();
    errors();
    allocation();
    determinism();
    for (layout = 0; layout < 4; ++layout)
        random_run(layout, 1000u + (uint32_t)layout);
    rgba();
    return finish("api_texture");
}
