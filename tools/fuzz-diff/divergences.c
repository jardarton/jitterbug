/*
 * The exploratory labels (docs/testing.md, "Three modes"). A difference at
 * a command matching one is counted under it and then ignored: the
 * reference is brought back to jitterbug's pixels and the stream goes on.
 *
 * These are triage labels, never a pass criterion: a label is a hypothesis
 * about the cause of a difference, and most predicates are wider than the
 * rule difference they are named after. The
 * checks are strict and bounded mode, which use none of this.
 *
 * Two kinds:
 *   - psxstack differs from the console, by a real-console dump in
 *     third_party/ps1-tests (what `fuzz-diff conformance` shows psxstack
 *     getting wrong there) or by psx-spx;
 *   - open questions: neither settles them. jitterbug keeps its rule;
 *     docs/conformance.md ("Open questions awaiting hardware") has
 *     each, with a reproducer in tests/conformance/open/.
 * The order matters: the first match wins.
 */
#include "fd.h"

#define NEEDS(d, f) (((d)->needs & (f)) == (f))
#define ONLY(d, f) (((d)->needs & ~(unsigned)(f)) == 0)

static int is_poly(const fd_desc *d)
{
    return d->kind == FD_K_TRI || d->kind == FD_K_QUAD;
}

static int is_line(const fd_desc *d)
{
    return d->kind == FD_K_LINE || d->kind == FD_K_POLYLINE;
}

/* Narrowed: jitterbug given psxstack's wrapped vertices matches it
   (main.c, wrap_explains). */
static int offset_wraps(const fd_desc *d)
{
    return d->kind >= FD_K_TRI && d->kind <= FD_K_RECT && d->offset_wraps && d->wrap_explains;
}

static int flips(const fd_desc *d)
{
    return d->kind == FD_K_RECT && d->textured && (d->flip_x || d->flip_y);
}

/* Its bounds within the drawing area meet its CLUT (classify.c); still
   wider than the rule: the entries sampled are not traced. */
static int own_clut(const fd_desc *d)
{
    return d->self_clut && (d->needs & FD_F_TEXTURE);
}

/* Not the same rectangle: there every pixel is read before it is written
   whatever the order, so ordering cannot explain a difference. */
static int copy_overlap(const fd_desc *d)
{
    return d->kind == FD_K_COPY && d->copy_overlap && !d->copy_same;
}

/* A line covering other pixels, or a difference that needs nothing beyond
   the line, its colours and dithering, or one that needs dithering or the
   colours to vary (what psxstack gets wrong), whatever else (a blend, the
   mask bits) it takes to show. A line difference that needs only the
   blend or the mask settings is not in it. */
static int lines(const fd_desc *d)
{
    return is_line(d) && d->isolated > 0 &&
           (d->coverage || ONLY(d, FD_F_DITHER | FD_F_GOURAUD | FD_F_COLOUR) ||
            (d->needs & (FD_F_DITHER | FD_F_COLOUR)) != 0);
}

static int colour_interp(const fd_desc *d)
{
    return is_poly(d) && d->isolated > 0 && NEEDS(d, FD_F_COLOUR);
}

static int uv_interp(const fd_desc *d)
{
    return is_poly(d) && d->isolated > 0 && NEEDS(d, FD_F_UV);
}

/* Narrowed: jitterbug drawing the two triangles in psxstack's order
   matches it (main.c, order_explains). */
static int quad_order(const fd_desc *d)
{
    return d->kind == FD_K_QUAD && d->order_explains;
}

static int flat_texture_dither(const fd_desc *d)
{
    return is_poly(d) && d->isolated > 0 && !d->gouraud && !d->raw &&
           NEEDS(d, FD_F_DITHER | FD_F_MODULATE) && !(d->needs & FD_F_SEMI);
}

static int raw_gouraud_dither(const fd_desc *d)
{
    return is_poly(d) && d->isolated > 0 && d->raw && d->gouraud && NEEDS(d, FD_F_DITHER) &&
           !(d->needs & FD_F_SEMI);
}

static int dither_semi(const fd_desc *d)
{
    return is_poly(d) && d->isolated > 0 && NEEDS(d, FD_F_DITHER | FD_F_SEMI);
}

/* A modulated texel can pass 31 only with a colour channel above 132
   (31 * 133 / 128 > 31); mode 3 without the shading bit is where psxstack
   quarters before modulating. Either, or the label does not apply. */
static int modulated_semi(const fd_desc *d)
{
    return (is_poly(d) || d->kind == FD_K_RECT) && d->isolated > 0 &&
           NEEDS(d, FD_F_SEMI | FD_F_MODULATE) && !(d->needs & FD_F_DITHER) &&
           (d->max_channel > 132 || (d->semi_mode == 3 && !d->gouraud));
}

const fd_divergence fd_divergences[] = {
    {"rect-flip", "rectangle x/y-flip (psxstack has none)", 0,
     "texture-flip dump: psxstack differs in 197632 pixels, jitterbug in none", flips},
    {"offset-wrap", "vertex + drawing offset outside -1024..1023 (psxstack wraps it to 11 bits)", 1,
     "psx-spx gives the vertex and the offset as signed 11-bit values and says nothing "
     "of their sum; no ps1-tests program reaches it",
     offset_wraps},
    {"own-clut", "a 4- or 8-bit primitive drawn over its own CLUT (psxstack has no CLUT cache)", 0,
     "clut-cache dump, rows 52 and 68: psxstack differs, jitterbug matches", own_clut},
    {"copy-overlap", "overlapping VRAM-to-VRAM copy (psxstack copies pixel by pixel)", 0,
     "vram-to-vram-overlap dump: psxstack differs in 3335 pixels, jitterbug in none",
     copy_overlap},
    {"lines", "lines: stepping, area edges, colours, dithering", 0,
     "lines dump: psxstack differs in 1330 pixels (36 placed differently, 129 on undithered "
     "Gouraud lines), jitterbug in none; psx-spx: lines are dithered, mono or shaded, and "
     "clipped only by the drawing area",
     lines},
    {"colour-interp", "polygon colour interpolation", 0,
     "triangle dump: psxstack differs in 2345 pixels, jitterbug in none", colour_interp},
    {"uv-interp", "polygon texture-coordinate interpolation", 0,
     "uv-interpolation dump: psxstack differs in 609 pixels, jitterbug in none", uv_interp},
    {"quad-order", "a quad whose triangles overlap or read each other's pixels (which is drawn first)", 1,
     "psx-spx names v1 v2 v3 the first triangle and v2 v3 v4 the second; psxstack draws "
     "the second first; no dump has such a quad",
     quad_order},
    {"flat-texture-dither", "flat modulated textured polygon with dithering (psxstack does not dither it)", 0,
     "psx-spx: polygons are dithered if they use Gouraud shading or modulation",
     flat_texture_dither},
    {"raw-gouraud-dither", "raw-textured Gouraud polygon with dithering", 1,
     "psx-spx: dithered if they use Gouraud shading or modulation; a raw texture ignores "
     "the colours, so whether the shading bit alone dithers is not said; no dump has one",
     raw_gouraud_dither},
    {"dither-semi", "dithering with semi-transparency (before or after the blend)", 1,
     "psx-spx gives the dither step and the blend but not their order; no dump has a "
     "dithered semi-transparent polygon",
     dither_semi},
    {"modulated-semi", "modulated semi-transparent texels above 31 (saturated before the blend or not)", 1,
     "psx-spx: modulation can exceed 31 and what is written is saturated; it does not say "
     "whether the blend sees the saturated value, nor where mode 3 takes its quarter; "
     "the dumps (rectangles, transparency) have no pixel that tells",
     modulated_semi},
};
const int fd_divergence_count = (int)(sizeof fd_divergences / sizeof fd_divergences[0]);

int fd_divergence_of(const fd_desc *d)
{
    int i;

    for (i = 0; i < fd_divergence_count; ++i)
        if (fd_divergences[i].match(d))
            return i;
    return -1;
}
