/*
 * The library's text font (texture.h, jbi_font).
 *
 * An original 8 x 8 bitmap font for ASCII 32..126, drawn for jitterbug
 * from scratch, glyph by glyph, in this file; it is not copied, traced or
 * converted from any other font. It is part of jitterbug, under the
 * library's licence (LICENSE).
 *
 * The design: capitals and digits 5 texels wide (columns 1-5) and 6 high
 * (rows 0-5) on a baseline under row 5; lower case 4 high (rows 2-5) with
 * ascenders to row 0 and descenders in rows 6-7. Column 7 is always
 * empty, so neighbouring glyphs never touch at the 8-texel advance, and
 * lines are 8 rows apart.
 *
 * Each glyph is eight rows of eight characters, '#' a set texel and '.' an
 * empty one, so the shapes can be read (and changed) here; tests/font.c
 * keeps a hash of the bitmap so that no change goes unnoticed.
 */
#include <string.h>

#include "api/api.h"
#include "api/texture.h"

#define G(r0, r1, r2, r3, r4, r5, r6, r7) {r0, r1, r2, r3, r4, r5, r6, r7}
#define BLANK "........"

static const char glyphs[JBI_FONT_LAST - JBI_FONT_FIRST + 1][8][9] = {
    /* ' ' */ G(BLANK, BLANK, BLANK, BLANK, BLANK, BLANK, BLANK, BLANK),
    /* '!' */
    G("...#....",
      "...#....",
      "...#....",
      "...#....",
      "........",
      "...#....",
      BLANK, BLANK),
    /* '"' */
    G("..#.#...",
      "..#.#...",
      BLANK, BLANK, BLANK, BLANK, BLANK, BLANK),
    /* '#' */
    G("..#.#...",
      ".#####..",
      "..#.#...",
      ".#####..",
      "..#.#...",
      BLANK, BLANK, BLANK),
    /* '$' */
    G("...#....",
      "..####..",
      ".#.#....",
      "..###...",
      "...#.#..",
      ".####...",
      "...#....",
      BLANK),
    /* '%' */
    G(".##..#..",
      ".##.#...",
      "...#....",
      "..#.##..",
      ".#..##..",
      BLANK, BLANK, BLANK),
    /* '&' */
    G("..##....",
      ".#..#...",
      "..##....",
      ".#.#.#..",
      ".#..#...",
      "..##.#..",
      BLANK, BLANK),
    /* ''' */
    G("...#....",
      "...#....",
      "..#.....",
      BLANK, BLANK, BLANK, BLANK, BLANK),
    /* '(' */
    G("....#...",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      "....#...",
      BLANK, BLANK),
    /* ')' */
    G("..#.....",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      "..#.....",
      BLANK, BLANK),
    /* '*' */
    G(BLANK,
      ".#.#.#..",
      "..###...",
      ".#####..",
      "..###...",
      ".#.#.#..",
      BLANK, BLANK),
    /* '+' */
    G(BLANK,
      "...#....",
      "...#....",
      ".#####..",
      "...#....",
      "...#....",
      BLANK, BLANK),
    /* ',' */
    G(BLANK, BLANK, BLANK, BLANK, BLANK,
      "...##...",
      "....#...",
      "...#...."),
    /* '-' */
    G(BLANK, BLANK, BLANK,
      ".####...",
      BLANK, BLANK, BLANK, BLANK),
    /* '.' */
    G(BLANK, BLANK, BLANK, BLANK,
      "...##...",
      "...##...",
      BLANK, BLANK),
    /* '/' */
    G(".....#..",
      "....#...",
      "....#...",
      "...#....",
      "..#.....",
      ".#......",
      BLANK, BLANK),
    /* '0' */
    G("..###...",
      ".#..##..",
      ".#.#.#..",
      ".##..#..",
      ".#...#..",
      "..###...",
      BLANK, BLANK),
    /* '1' */
    G("...#....",
      "..##....",
      "...#....",
      "...#....",
      "...#....",
      "..###...",
      BLANK, BLANK),
    /* '2' */
    G("..###...",
      ".#...#..",
      "....#...",
      "...#....",
      "..#.....",
      ".#####..",
      BLANK, BLANK),
    /* '3' */
    G(".####...",
      ".....#..",
      "..###...",
      ".....#..",
      ".....#..",
      ".####...",
      BLANK, BLANK),
    /* '4' */
    G("....#...",
      "...##...",
      "..#.#...",
      ".#..#...",
      ".#####..",
      "....#...",
      BLANK, BLANK),
    /* '5' */
    G(".#####..",
      ".#......",
      ".####...",
      ".....#..",
      ".....#..",
      ".####...",
      BLANK, BLANK),
    /* '6' */
    G("..###...",
      ".#......",
      ".####...",
      ".#...#..",
      ".#...#..",
      "..###...",
      BLANK, BLANK),
    /* '7' */
    G(".#####..",
      ".....#..",
      "....#...",
      "...#....",
      "...#....",
      "...#....",
      BLANK, BLANK),
    /* '8' */
    G("..###...",
      ".#...#..",
      "..###...",
      ".#...#..",
      ".#...#..",
      "..###...",
      BLANK, BLANK),
    /* '9' */
    G("..###...",
      ".#...#..",
      ".#...#..",
      "..####..",
      ".....#..",
      "..###...",
      BLANK, BLANK),
    /* ':' */
    G(BLANK,
      "...#....",
      BLANK, BLANK,
      "...#....",
      BLANK, BLANK, BLANK),
    /* ';' */
    G(BLANK,
      "...#....",
      BLANK, BLANK,
      "...#....",
      "..#.....",
      BLANK, BLANK),
    /* '<' */
    G("....#...",
      "...#....",
      "..#.....",
      "...#....",
      "....#...",
      BLANK, BLANK, BLANK),
    /* '=' */
    G(BLANK,
      ".#####..",
      BLANK,
      ".#####..",
      BLANK, BLANK, BLANK, BLANK),
    /* '>' */
    G("..#.....",
      "...#....",
      "....#...",
      "...#....",
      "..#.....",
      BLANK, BLANK, BLANK),
    /* '?' */
    G("..###...",
      ".#...#..",
      "....#...",
      "...#....",
      BLANK,
      "...#....",
      BLANK, BLANK),
    /* '@' */
    G("..###...",
      ".#...#..",
      ".#.###..",
      ".#.###..",
      ".#......",
      "..####..",
      BLANK, BLANK),
    /* 'A' */
    G("..###...",
      ".#...#..",
      ".#...#..",
      ".#####..",
      ".#...#..",
      ".#...#..",
      BLANK, BLANK),
    /* 'B' */
    G(".####...",
      ".#...#..",
      ".####...",
      ".#...#..",
      ".#...#..",
      ".####...",
      BLANK, BLANK),
    /* 'C' */
    G("..####..",
      ".#......",
      ".#......",
      ".#......",
      ".#......",
      "..####..",
      BLANK, BLANK),
    /* 'D' */
    G(".####...",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      ".####...",
      BLANK, BLANK),
    /* 'E' */
    G(".#####..",
      ".#......",
      ".####...",
      ".#......",
      ".#......",
      ".#####..",
      BLANK, BLANK),
    /* 'F' */
    G(".#####..",
      ".#......",
      ".####...",
      ".#......",
      ".#......",
      ".#......",
      BLANK, BLANK),
    /* 'G' */
    G("..####..",
      ".#......",
      ".#..##..",
      ".#...#..",
      ".#...#..",
      "..####..",
      BLANK, BLANK),
    /* 'H' */
    G(".#...#..",
      ".#...#..",
      ".#####..",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      BLANK, BLANK),
    /* 'I' */
    G("..###...",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      "..###...",
      BLANK, BLANK),
    /* 'J' */
    G("....##..",
      ".....#..",
      ".....#..",
      ".....#..",
      ".#...#..",
      "..###...",
      BLANK, BLANK),
    /* 'K' */
    G(".#...#..",
      ".#..#...",
      ".###....",
      ".#..#...",
      ".#...#..",
      ".#...#..",
      BLANK, BLANK),
    /* 'L' */
    G(".#......",
      ".#......",
      ".#......",
      ".#......",
      ".#......",
      ".#####..",
      BLANK, BLANK),
    /* 'M' */
    G(".#...#..",
      ".##.##..",
      ".#.#.#..",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      BLANK, BLANK),
    /* 'N' */
    G(".#...#..",
      ".##..#..",
      ".#.#.#..",
      ".#..##..",
      ".#...#..",
      ".#...#..",
      BLANK, BLANK),
    /* 'O' */
    G("..###...",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      "..###...",
      BLANK, BLANK),
    /* 'P' */
    G(".####...",
      ".#...#..",
      ".#...#..",
      ".####...",
      ".#......",
      ".#......",
      BLANK, BLANK),
    /* 'Q' */
    G("..###...",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      ".#..#...",
      "..##.#..",
      BLANK, BLANK),
    /* 'R' */
    G(".####...",
      ".#...#..",
      ".#...#..",
      ".####...",
      ".#..#...",
      ".#...#..",
      BLANK, BLANK),
    /* 'S' */
    G("..####..",
      ".#......",
      "..###...",
      ".....#..",
      ".....#..",
      ".####...",
      BLANK, BLANK),
    /* 'T' */
    G(".#####..",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      BLANK, BLANK),
    /* 'U' */
    G(".#...#..",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      "..###...",
      BLANK, BLANK),
    /* 'V' */
    G(".#...#..",
      ".#...#..",
      ".#...#..",
      "..#.#...",
      "..#.#...",
      "...#....",
      BLANK, BLANK),
    /* 'W' */
    G(".#...#..",
      ".#...#..",
      ".#...#..",
      ".#.#.#..",
      ".##.##..",
      ".#...#..",
      BLANK, BLANK),
    /* 'X' */
    G(".#...#..",
      "..#.#...",
      "...#....",
      "...#....",
      "..#.#...",
      ".#...#..",
      BLANK, BLANK),
    /* 'Y' */
    G(".#...#..",
      "..#.#...",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      BLANK, BLANK),
    /* 'Z' */
    G(".#####..",
      "....#...",
      "...#....",
      "..#.....",
      ".#......",
      ".#####..",
      BLANK, BLANK),
    /* '[' */
    G("..###...",
      "..#.....",
      "..#.....",
      "..#.....",
      "..#.....",
      "..###...",
      BLANK, BLANK),
    /* '\' */
    G(".#......",
      "..#.....",
      "...#....",
      "...#....",
      "....#...",
      ".....#..",
      BLANK, BLANK),
    /* ']' */
    G("..###...",
      "....#...",
      "....#...",
      "....#...",
      "....#...",
      "..###...",
      BLANK, BLANK),
    /* '^' */
    G("...#....",
      "..#.#...",
      ".#...#..",
      BLANK, BLANK, BLANK, BLANK, BLANK),
    /* '_' */
    G(BLANK, BLANK, BLANK, BLANK, BLANK, BLANK,
      ".#####..",
      BLANK),
    /* '`' */
    G("..#.....",
      "...#....",
      BLANK, BLANK, BLANK, BLANK, BLANK, BLANK),
    /* 'a' */
    G(BLANK, BLANK,
      "..####..",
      ".#...#..",
      ".#..##..",
      "..##.#..",
      BLANK, BLANK),
    /* 'b' */
    G(".#......",
      ".#......",
      ".####...",
      ".#...#..",
      ".#...#..",
      ".####...",
      BLANK, BLANK),
    /* 'c' */
    G(BLANK, BLANK,
      "..####..",
      ".#......",
      ".#......",
      "..####..",
      BLANK, BLANK),
    /* 'd' */
    G(".....#..",
      ".....#..",
      "..####..",
      ".#...#..",
      ".#...#..",
      "..####..",
      BLANK, BLANK),
    /* 'e' */
    G(BLANK, BLANK,
      "..###...",
      ".#####..",
      ".#......",
      "..####..",
      BLANK, BLANK),
    /* 'f' */
    G("...##...",
      "..#.....",
      ".####...",
      "..#.....",
      "..#.....",
      "..#.....",
      BLANK, BLANK),
    /* 'g' */
    G(BLANK, BLANK,
      "..####..",
      ".#...#..",
      ".#...#..",
      "..####..",
      ".....#..",
      "..###..."),
    /* 'h' */
    G(".#......",
      ".#......",
      ".####...",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      BLANK, BLANK),
    /* 'i' */
    G("...#....",
      BLANK,
      "..##....",
      "...#....",
      "...#....",
      "..###...",
      BLANK, BLANK),
    /* 'j' */
    G("....#...",
      BLANK,
      "...##...",
      "....#...",
      "....#...",
      "....#...",
      "....#...",
      ".###...."),
    /* 'k' */
    G(".#......",
      ".#......",
      ".#..#...",
      ".###....",
      ".#..#...",
      ".#...#..",
      BLANK, BLANK),
    /* 'l' */
    G("..##....",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      "...##...",
      BLANK, BLANK),
    /* 'm' */
    G(BLANK, BLANK,
      ".##.#...",
      ".#.#.#..",
      ".#.#.#..",
      ".#...#..",
      BLANK, BLANK),
    /* 'n' */
    G(BLANK, BLANK,
      ".####...",
      ".#...#..",
      ".#...#..",
      ".#...#..",
      BLANK, BLANK),
    /* 'o' */
    G(BLANK, BLANK,
      "..###...",
      ".#...#..",
      ".#...#..",
      "..###...",
      BLANK, BLANK),
    /* 'p' */
    G(BLANK, BLANK,
      ".####...",
      ".#...#..",
      ".#...#..",
      ".####...",
      ".#......",
      ".#......"),
    /* 'q' */
    G(BLANK, BLANK,
      "..####..",
      ".#...#..",
      ".#...#..",
      "..####..",
      ".....#..",
      ".....#.."),
    /* 'r' */
    G(BLANK, BLANK,
      ".#.##...",
      ".##.....",
      ".#......",
      ".#......",
      BLANK, BLANK),
    /* 's' */
    G(BLANK, BLANK,
      "..####..",
      ".##.....",
      "....##..",
      ".####...",
      BLANK, BLANK),
    /* 't' */
    G("..#.....",
      "..#.....",
      ".####...",
      "..#.....",
      "..#.....",
      "...##...",
      BLANK, BLANK),
    /* 'u' */
    G(BLANK, BLANK,
      ".#...#..",
      ".#...#..",
      ".#...#..",
      "..####..",
      BLANK, BLANK),
    /* 'v' */
    G(BLANK, BLANK,
      ".#...#..",
      ".#...#..",
      "..#.#...",
      "...#....",
      BLANK, BLANK),
    /* 'w' */
    G(BLANK, BLANK,
      ".#...#..",
      ".#.#.#..",
      ".#.#.#..",
      "..#.#...",
      BLANK, BLANK),
    /* 'x' */
    G(BLANK, BLANK,
      ".#...#..",
      "..#.#...",
      "..#.#...",
      ".#...#..",
      BLANK, BLANK),
    /* 'y' */
    G(BLANK, BLANK,
      ".#...#..",
      ".#...#..",
      ".#...#..",
      "..####..",
      ".....#..",
      "..###..."),
    /* 'z' */
    G(BLANK, BLANK,
      ".#####..",
      "....#...",
      "..##....",
      ".#####..",
      BLANK, BLANK),
    /* '{' */
    G("....##..",
      "...#....",
      "..#.....",
      "...#....",
      "...#....",
      "....##..",
      BLANK, BLANK),
    /* '|' */
    G("...#....",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      "...#....",
      BLANK),
    /* '}' */
    G(".##.....",
      "...#....",
      "....#...",
      "...#....",
      "...#....",
      ".##.....",
      BLANK, BLANK),
    /* '~' */
    G(BLANK, BLANK,
      "..##.#..",
      ".#..#...",
      BLANK, BLANK, BLANK, BLANK),
};

unsigned jbi_font_row(int ch, int r)
{
    const char *row;
    unsigned bits = 0;
    int i;

    if (ch < JBI_FONT_FIRST || ch > JBI_FONT_LAST || r < 0 || r > 7)
        return 0;
    row = glyphs[ch - JBI_FONT_FIRST][r];
    for (i = 0; i < 8; ++i)
        bits = bits << 1 | (row[i] == '#');
    return bits;
}

int jbi_font_glyph(int ch, int *u, int *v)
{
    int ok = ch >= JBI_FONT_FIRST && ch <= JBI_FONT_LAST, k = (ok ? ch : '?') - JBI_FONT_FIRST;

    *u = k % JBI_FONT_COLUMNS * JBI_FONT_CELL;
    *v = k / JBI_FONT_COLUMNS * JBI_FONT_CELL;
    return ok;
}

enum {
    FONT_W = JBI_FONT_COLUMNS * JBI_FONT_CELL, /* 128 texels */
    FONT_H = JBI_FONT_ROWS * JBI_FONT_CELL     /* 48 */
};

const jb_texture *jbi_font(jb_context *jb, const char *fn)
{
    static const uint32_t clear_cache = 0x01000000u;
    struct jbi_textures *s = jb->api->textures;
    /* One row of glyph cells at a time keeps the stack small: the texture
       is written in six bands through jb_texture_write's path (which the
       public call refuses for the font). */
    uint8_t texels[FONT_W * JBI_FONT_CELL];
    uint16_t palette[16], blend[16];
    jb_texture_desc d;
    jb_texture t;
    int band, i, r, ch;
    uint32_t mask;

    if (s->font >= 0)
        return &s->slot[s->font].tex;
    /* The library's own atlas is written exactly, whatever the program's
       mask settings: with them off, put back after. */
    mask = jbi_mask_off(jb);
    memset(&d, 0, sizeof d);
    d.width = FONT_W;
    d.height = FONT_H;
    d.depth = JB_TEX_4BIT;
    memset(palette, 0, sizeof palette);
    palette[1] = JBI_FONT_TEXEL;
    d.palette = palette;
    if (jbi_texture_create(jb, fn, &d, 1, &t) != JB_OK) {
        jbi_mask_restore(jb, mask);
        return NULL;
    }
    s->font = jbi_texture_find(jb, &t);
    for (band = 0; band < JBI_FONT_ROWS; ++band) {
        memset(texels, 0, sizeof texels);
        for (i = 0; i < JBI_FONT_COLUMNS; ++i) {
            ch = JBI_FONT_FIRST + band * JBI_FONT_COLUMNS + i;
            for (r = 0; r < JBI_FONT_CELL; ++r) {
                unsigned bits = jbi_font_row(ch, r);
                int c;
                for (c = 0; c < JBI_FONT_CELL; ++c)
                    texels[r * FONT_W + i * JBI_FONT_CELL + c] = (uint8_t)(bits >> (7 - c) & 1);
            }
        }
        jbi_texture_put(jb, s->font, 0, band * JBI_FONT_CELL, FONT_W, JBI_FONT_CELL, texels);
    }
    memcpy(blend, palette, sizeof blend);
    blend[1] = (uint16_t)(JBI_FONT_TEXEL | 0x8000u);
    jbi_load(jb, s->font_blend.x, s->font_blend.y, 16, 1, blend);
    /* Neither CLUT may be stood in for by one the cache holds from these
       pixels. */
    jbi_api_command(jb, &clear_cache, 1);
    jbi_mask_restore(jb, mask);
    return &s->slot[s->font].tex;
}

jb_texture jbi_font_blend(const jb_context *jb)
{
    const struct jbi_textures *s = jb->api->textures;
    jb_texture t = s->slot[s->font].tex;

    t.clut_x = s->font_blend.x;
    t.clut_y = s->font_blend.y;
    return t;
}
