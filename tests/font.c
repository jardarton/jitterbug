/*
 * font: the library's text font (src/api/font.c, src/api/texture.h).
 *
 *   glyphs   every glyph of 32..126 has a texel set but space, which has
 *            none; every glyph leaves column 7 empty (it fits its cell with
 *            a column between characters); and the bitmap's FNV-1a hash is
 *            the one stored here, so no change to a glyph goes unnoticed
 *            (a deliberate one updates FONT_HASH in the same commit)
 *   drawn    the font texture placed through jbi_font, drawn whole with a
 *            raw jb_prim_rect and read back: exactly the glyph table, in
 *            the texel colour 0x4210, nothing where a glyph is empty; and
 *            modulated by a colour c it shows c >> 3, as a fill of c does
 *   texture  its fields (4-bit, 128 x 48, both CLUTs), placed once, not
 *            destroyable, named by jb_vram_reserve; no room for it is
 *            JB_ERROR_OUT_OF_MEMORY with its detail
 *   lookup   jbi_font_glyph's cells, and '?' for what has no glyph
 *   window   jbi_texture_window (for the friendly draws) gives the
 *            header's window for repeat textures, and none otherwise
 *
 * With --print it prints the glyph table, '#' a set texel.
 */
#include <stdio.h>
#include <string.h>

#include <jitterbug.h>

#include "api/api.h"
#include "api/texture.h"

/* FNV-1a, 32 bits, over the 95 x 8 rows (bit 7 the leftmost texel). */
#define FONT_HASH 0x5ba5f981u

static int checks, failures;

static void check(const char *what, long got, long want)
{
    ++checks;
    if (got == want)
        return;
    if (++failures <= 30)
        printf("FAIL %s: got %ld (0x%lx), want %ld (0x%lx)\n", what, got, (unsigned long)got,
               want, (unsigned long)want);
}

static uint16_t vram[1024 * 512];

static void print_table(void)
{
    int row, ch, r, c;

    for (row = 0; row < JBI_FONT_ROWS; ++row)
        for (r = 0; r < 8; ++r) {
            for (ch = JBI_FONT_FIRST + row * 16; ch < JBI_FONT_FIRST + row * 16 + 16; ++ch)
                for (c = 0; c < 8; ++c)
                    putchar(jbi_font_row(ch, r) >> (7 - c) & 1 ? '#' : '.');
            putchar('\n');
        }
}

static void glyphs(void)
{
    uint32_t h = 2166136261u;
    int ch, r;
    char m[64];

    for (ch = JBI_FONT_FIRST; ch <= JBI_FONT_LAST; ++ch) {
        unsigned any = 0, col7 = 0;
        for (r = 0; r < 8; ++r) {
            unsigned bits = jbi_font_row(ch, r);
            any |= bits;
            col7 |= bits & 1;
            h = (h ^ bits) * 16777619u;
        }
        snprintf(m, sizeof m, "glyph %d '%c' %s", ch, ch, ch == ' ' ? "empty" : "not empty");
        check(m, any != 0, ch != ' ');
        snprintf(m, sizeof m, "glyph %d '%c' leaves column 7 empty", ch, ch);
        check(m, col7, 0);
    }
    check("no glyph below 32", jbi_font_row(31, 0) | jbi_font_row(31, 3), 0);
    check("no glyph above 126", jbi_font_row(127, 0) | jbi_font_row(127, 3), 0);
    check("no row 8", jbi_font_row('A', 8), 0);
    check("the font's hash", (long)h, (long)FONT_HASH);
    if (h != FONT_HASH)
        printf("  (the bitmap's hash is 0x%08lx)\n", (unsigned long)h);
}

static void lookup(void)
{
    int u, v;

    check("' ' found", jbi_font_glyph(' ', &u, &v), 1);
    check("' ' cell", u == 0 && v == 0, 1);
    check("'A' found", jbi_font_glyph('A', &u, &v), 1);
    check("'A' cell", u == 8 && v == 16, 1);
    check("'~' found", jbi_font_glyph('~', &u, &v), 1);
    check("'~' cell", u == 14 * 8 && v == 5 * 8, 1);
    check("'\\n' has none", jbi_font_glyph('\n', &u, &v), 0);
    check("'\\n' gives '?'", u == 15 * 8 && v == 8, 1);
    check("200 has none", jbi_font_glyph(200, &u, &v), 0);
    check("-1 has none", jbi_font_glyph(-1, &u, &v), 0);
}

static void drawn(int scale)
{
    jb_config cfg;
    jb_context *jb;
    const jb_texture *f, *again;
    jb_rect rect;
    int x, y, bad = 0;
    char m[64];

    memset(&cfg, 0, sizeof cfg);
    cfg.scale = scale;
    jb = jb_create(&cfg, NULL);
    if (jb == NULL) {
        check("jb_create", 0, 1);
        return;
    }
    f = jbi_font(jb, "font test");
    check("jbi_font", f != NULL, 1);
    if (f == NULL) {
        jb_destroy(jb);
        return;
    }
    check("font: 4-bit", f->depth, JB_TEX_4BIT);
    check("font: 128 x 48", f->width == 128 && f->height == 48, 1);
    check("font: CLUT on 16", f->clut_x % 16, 0);
    check("font: repeat 0", f->repeat, 0);
    again = jbi_font(jb, "font test");
    check("font: placed once", again == f, 1);
    check("font: found", jbi_texture_find(jb, f) >= 0, 1);
    check("font: not destroyable", jb_texture_destroy(jb, f), JB_ERROR_INVALID_ARGUMENT);
    check("font: destroy detail",
          strcmp(jb_error_string(jb), "jb_texture_destroy: that texture is the library's font"),
          0);
    check("font: reserve over it",
          jb_vram_reserve(jb, f->page_x + f->u / 4, f->page_y + f->v, 1, 1),
          JB_ERROR_INVALID_ARGUMENT);
    check("font: reserve detail names it",
          strstr(jb_error_string(jb), "(the library's font)") != NULL, 1);

    jb_set_draw_area(jb, 0, 0, 319, 239);
    memset(&rect, 0, sizeof rect);
    rect.w = 128, rect.h = 48, rect.texture = f, rect.raw = 1, rect.u = f->u, rect.v = f->v;
    check("font: draw raw", jb_prim_rect(jb, &rect), JB_OK);
    /* Modulated by c, below it: shows c >> 3. */
    rect.y = 60, rect.raw = 0, rect.color = JB_RGB(200, 100, 40);
    check("font: draw modulated", jb_prim_rect(jb, &rect), JB_OK);
    jb_vram_read(jb, 0, 0, 1024, 512, vram);
    for (y = 0; y < 48; ++y)
        for (x = 0; x < 128; ++x) {
            int ch = JBI_FONT_FIRST + y / 8 * 16 + x / 8;
            int set = (int)(jbi_font_row(ch, y % 8) >> (7 - x % 8) & 1);
            unsigned want = set ? 0x4210u : 0;
            unsigned want_mod = set ? (200u >> 3 | (100u >> 3) << 5 | (40u >> 3) << 10) : 0;
            if (vram[y * 1024 + x] != want || vram[(y + 60) * 1024 + x] != want_mod)
                ++bad;
        }
    snprintf(m, sizeof m, "font at %dx: drawn as the table", scale);
    check(m, bad, 0);
    check("font: opaque CLUT", vram[f->clut_y * 1024 + f->clut_x] == 0 &&
                                   vram[f->clut_y * 1024 + f->clut_x + 1] == JBI_FONT_TEXEL,
          1);
    check("font: blending CLUT",
          vram[f->clut_y * 1024 + f->clut_x + 16] == 0 &&
              vram[f->clut_y * 1024 + f->clut_x + 17] == (JBI_FONT_TEXEL | 0x8000),
          1);
    jb_destroy(jb);
}

static void window(void)
{
    static const int sizes[][3] = {{8, 8, 4},    {16, 64, 8},  {128, 32, 15},
                                   {256, 8, 4},  {32, 256, 8}, {256, 256, 15}};
    jb_context *jb = jb_create(NULL, NULL);
    int i, mx, my, ox, oy;

    if (jb == NULL) {
        check("jb_create", 0, 1);
        return;
    }
    for (i = 0; i < 6; ++i) {
        jb_texture_desc d;
        jb_texture t;
        int u, bad = 0;

        memset(&d, 0, sizeof d);
        d.width = sizes[i][0], d.height = sizes[i][1], d.depth = sizes[i][2], d.repeat = 1;
        check("window: create", jb_texture_create(jb, &d, &t), JB_OK);
        jbi_texture_window(&t, &mx, &my, &ox, &oy);
        check("window: mask x", mx, t.width == 256 ? 0 : (256 - t.width) / 8);
        check("window: mask y", my, t.height == 256 ? 0 : (256 - t.height) / 8);
        check("window: offset x", ox, t.width == 256 ? 0 : t.u / 8);
        check("window: offset y", oy, t.height == 256 ? 0 : t.v / 8);
        /* The window maps every u to t.u + u mod width, and v likewise. */
        for (u = 0; u < 256; ++u) {
            if (((u & ~(mx * 8)) | ((ox & mx) * 8)) != t.u + u % t.width)
                ++bad;
            if (((u & ~(my * 8)) | ((oy & my) * 8)) != t.v + u % t.height)
                ++bad;
        }
        check("window: wraps", bad, 0);
        d.repeat = 0;
        check("window: create plain", jb_texture_create(jb, &d, &t), JB_OK);
        jbi_texture_window(&t, &mx, &my, &ox, &oy);
        check("window: none without repeat", mx | my | ox | oy, 0);
    }
    jb_destroy(jb);
}

/* The font placed at first use while the program has the mask
   bits on is written exactly (no bit 15 added, old masked pixels
   overwritten), the settings put back after; and the CLUT cache is
   cleared after its palettes. */
static void masked_first_use(int set, int check_bit)
{
    jb_context *jb = jb_create(NULL, NULL);
    const jb_texture *f;
    uint16_t junk[32];
    uint32_t stat;
    int k, x, y, bad = 0;
    char m[64];

    if (jb == NULL) {
        check("jb_create", 0, 1);
        return;
    }
    /* Old pixels with bit 15 where the font and its palettes will go (the
       first texture's place, and the palettes' at the bottom left). */
    for (k = 0; k < 32; ++k)
        junk[k] = 0xffff;
    for (y = 0; y < 48; ++y)
        jb_vram_load(jb, 320, y, 32, 1, junk);
    jb_vram_load(jb, 0, 511, 32, 1, junk);
    jb_set_mask(jb, set, check_bit);
    stat = jb_gpustat(jb);
    f = jbi_font(jb, "font test");
    check("masked: placed", f != NULL, 1);
    if (f == NULL) {
        jb_destroy(jb);
        return;
    }
    check("masked: where expected", f->page_x + f->u / 4 == 320 && f->clut_x == 0 &&
                                        f->clut_y == 511,
          1);
    jb_vram_read(jb, 0, 0, 1024, 512, vram);
    snprintf(m, sizeof m, "masked %d %d: CLUT entry 0", set, check_bit);
    check(m, vram[511 * 1024 + 0], 0);
    snprintf(m, sizeof m, "masked %d %d: CLUT entry 1", set, check_bit);
    check(m, vram[511 * 1024 + 1], JBI_FONT_TEXEL);
    snprintf(m, sizeof m, "masked %d %d: blending entry 1", set, check_bit);
    check(m, vram[511 * 1024 + 17], JBI_FONT_TEXEL | 0x8000);
    for (y = 0; y < 48; ++y)
        for (x = 0; x < 128; ++x) {
            unsigned word = vram[y * 1024 + 320 + x / 4], idx = word >> (x % 4 * 4) & 15;
            int ch = JBI_FONT_FIRST + y / 8 * 16 + x / 8;
            if (idx != (jbi_font_row(ch, y % 8) >> (7 - x % 8) & 1))
                ++bad;
        }
    snprintf(m, sizeof m, "masked %d %d: the glyphs exactly", set, check_bit);
    check(m, bad, 0);
    snprintf(m, sizeof m, "masked %d %d: space's first word", set, check_bit);
    check(m, vram[320], 0);
    snprintf(m, sizeof m, "masked %d %d: mask settings put back", set, check_bit);
    check(m, (long)jb_gpustat(jb), (long)stat);
    jb_destroy(jb);
}

/* For the font: a CLUT drawn from the font's palette pixels
   before the font is placed there does not stand in for its palette. */
static void font_after_stale_clut(void)
{
    jb_context *jb = jb_create(NULL, NULL);
    jb_texture t;
    jb_rect r;
    uint16_t red[16] = {0x001f, 0x001f}, one = 0x1111;

    if (jb == NULL) {
        check("jb_create", 0, 1);
        return;
    }
    /* A 4-bit sprite of the program's own with its CLUT at (0, 511), where
       the font's palette goes, drawn so the cache holds it. */
    jb_vram_load(jb, 0, 511, 16, 1, red);
    jb_vram_load(jb, 960, 0, 1, 1, &one);
    memset(&t, 0, sizeof t);
    t.page_x = 960, t.depth = JB_TEX_4BIT, t.clut_x = 0, t.clut_y = 511;
    memset(&r, 0, sizeof r);
    r.w = r.h = 1, r.texture = &t, r.raw = 1;
    jb_set_draw_area(jb, 0, 0, 319, 239);
    jb_prim_rect(jb, &r);
    check("stale: the font placed", jbi_font(jb, "font test") != NULL, 1);
    /* A glyph texel (index 1) drawn raw with the font's CLUT now. */
    {
        const jb_texture *f = jbi_font(jb, "font test");
        int u, v, x, y, found = 0;
        jbi_font_glyph('H', &u, &v);
        r.texture = f;
        r.w = 8, r.h = 8, r.u = f->u + u, r.v = f->v + v, r.x = 20, r.y = 20;
        jb_prim_rect(jb, &r);
        jb_vram_read(jb, 0, 0, 1024, 512, vram);
        for (y = 20; y < 28; ++y)
            for (x = 20; x < 28; ++x) {
                if (vram[y * 1024 + x] == 0x001f)
                    found = -1000;
                if (vram[y * 1024 + x] == JBI_FONT_TEXEL)
                    ++found;
            }
        check("stale: the font's colour, not the cached red", found > 0, 1);
    }
    jb_destroy(jb);
}

/* Ids never repeat in a context: when the count reaches its end,
   jb_texture_create refuses, rather than wrap round to an id a stale
   handle may hold. */
static void ids_run_out(void)
{
    jb_context *jb = jb_create(NULL, NULL);
    jb_texture_desc d;
    jb_texture a, b, c;

    if (jb == NULL) {
        check("jb_create", 0, 1);
        return;
    }
    memset(&d, 0, sizeof d);
    d.width = d.height = 4, d.depth = JB_TEX_15BIT;
    check("ids: first", jb_texture_create(jb, &d, &a), JB_OK);
    check("ids: first is 1", (long)a.id, 1);
    check("ids: destroy it", jb_texture_destroy(jb, &a), JB_OK);
    jb->api->textures->serial = 0xfffffffeu; /* as after that many textures */
    check("ids: the last", jb_texture_create(jb, &d, &b), JB_OK);
    check("ids: the last is 0xffffffff", (long)b.id, (long)0xffffffffu);
    check("ids: then none", jb_texture_create(jb, &d, &c), JB_ERROR_OUT_OF_MEMORY);
    check("ids: detail", strstr(jb_error_string(jb), "every id it has") != NULL, 1);
    check("ids: the old handle is still refused", jb_texture_destroy(jb, &a),
          JB_ERROR_INVALID_ARGUMENT);
    jb_destroy(jb);
}

/* When the palette's only places lie under the texels' only
   place, the pair search does not search the texels again for every
   palette: everything is reserved but x 640..703, y 0..127, and a 256 x
   128 4-bit texture (64 x 128 pixels) takes all of it. */
static void pair_search_cost(void)
{
    jb_context *jb = jb_create(NULL, NULL);
    jb_texture_desc d;
    jb_texture t;
    unsigned long before;

    if (jb == NULL) {
        check("jb_create", 0, 1);
        return;
    }
    jb_vram_reserve(jb, 0, 0, 640, 512);
    jb_vram_reserve(jb, 704, 0, 320, 512);
    jb_vram_reserve(jb, 640, 128, 64, 384);
    memset(&d, 0, sizeof d);
    d.width = 256, d.height = 128, d.depth = JB_TEX_4BIT;
    before = jb->api->textures->texel_searches;
    check("pair cost: no room", jb_texture_create(jb, &d, &t), JB_ERROR_OUT_OF_MEMORY);
    check("pair cost: palette named",
          strstr(jb_error_string(jb), "16-entry palette") != NULL, 1);
    check("pair cost: texel searches", jb->api->textures->texel_searches - before <= 3, 1);
    if (jb->api->textures->texel_searches - before > 3)
        printf("  (%lu texel searches)\n", jb->api->textures->texel_searches - before);
    jb_destroy(jb);
}

/* Past what the first pair search kept, the search still runs one
   texel search: everything is reserved but x 640..766 of rows 0..255,
   with rows 112..143 widened to 895. A 128 x 144 8-bit texture has 7,232
   places, and every one overlaps each of the 32 places its 256-entry
   palette has (x 640 of rows 112..143). */
static void pair_search_cost_wide(void)
{
    jb_context *jb = jb_create(NULL, NULL);
    jb_texture_desc d;
    jb_texture t;
    unsigned long before;

    if (jb == NULL) {
        check("jb_create", 0, 1);
        return;
    }
    jb_vram_reserve(jb, 0, 0, 640, 512);
    jb_vram_reserve(jb, 767, 0, 257, 112);
    jb_vram_reserve(jb, 896, 112, 128, 32);
    jb_vram_reserve(jb, 767, 144, 257, 368);
    jb_vram_reserve(jb, 640, 256, 127, 256);
    memset(&d, 0, sizeof d);
    d.width = 128, d.height = 144, d.depth = JB_TEX_8BIT;
    before = jb->api->textures->texel_searches;
    check("wide pair cost: no room", jb_texture_create(jb, &d, &t), JB_ERROR_OUT_OF_MEMORY);
    check("wide pair cost: palette named",
          strstr(jb_error_string(jb), "256-entry palette") != NULL, 1);
    check("wide pair cost: texel searches", jb->api->textures->texel_searches - before <= 3, 1);
    if (jb->api->textures->texel_searches - before > 3)
        printf("  (%lu texel searches)\n", jb->api->textures->texel_searches - before);
    jb_destroy(jb);
}

static void no_room(void)
{
    jb_config cfg;
    jb_context *jb;

    memset(&cfg, 0, sizeof cfg);
    cfg.width = 1024, cfg.height = 512; /* one buffer, all of VRAM */
    jb = jb_create(&cfg, NULL);
    if (jb == NULL) {
        check("jb_create", 0, 1);
        return;
    }
    check("no room: NULL", jbi_font(jb, "jb_draw_text") == NULL, 1);
    check("no room: detail",
          strcmp(jb_error_string(jb),
                 "jb_draw_text: no room in VRAM for a 128 x 48 4-bit texture"),
          0);
    jb_destroy(jb);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--print") == 0) {
        print_table();
        return 0;
    }
    glyphs();
    lookup();
    drawn(1);
    drawn(3);
    window();
    ids_run_out();
    pair_search_cost();
    pair_search_cost_wide();
    masked_first_use(1, 0);
    masked_first_use(0, 1);
    masked_first_use(1, 1);
    font_after_stale_clut();
    no_room();
    printf("font: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
