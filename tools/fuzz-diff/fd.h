/*
 * tools/fuzz-diff: seeded GP0 streams through jitterbug and a reference GPU
 * (psxstack's, ref.h), compared (docs/testing.md, "Differential fuzzing").
 * Shared types. C11.
 */
#ifndef FUZZ_DIFF_FD_H
#define FUZZ_DIFF_FD_H

#include <stddef.h>
#include <stdint.h>

enum { FD_VRAM_W = 1024, FD_VRAM_H = 512, FD_VRAM_PIXELS = FD_VRAM_W * FD_VRAM_H };

/* ------------------------------------------------------------------------ */
/* The random numbers: one draw per statement, so that the stream does not
   depend on the order in which a compiler evaluates arguments. */

typedef struct fd_rng {
    uint64_t s;
} fd_rng;

uint32_t fd_r32(fd_rng *r);
int fd_below(fd_rng *r, int n);         /* 0 .. n - 1 */
int fd_range(fd_rng *r, int lo, int hi); /* lo .. hi */
/* The generator of stream (seed, index). */
void fd_rng_stream(fd_rng *r, uint64_t seed, uint64_t index);

/* ------------------------------------------------------------------------ */
/* A stream: GP0 commands, each complete, in order. A VRAM-to-CPU command
   (C0h) is followed by reading all of its words through GPUREAD. */

typedef struct fd_cmd {
    uint32_t *w;
    int n;
    int reads; /* C0h: the GPUREAD words read after it; else 0 */
    int background; /* one of the loads that set every pixel at the start */
    /* Strict and bounded streams (gen_check.c): the group the command
       belongs to (a primitive and the state words, loads and GP0(01h) sent
       right before it, which make it self-contained: shrinking drops whole
       groups), and whether the command is checked against the bounded
       residual (main.c, "Bounded residual") rather than exactly. */
    int group;
    int bounded;
} fd_cmd;

typedef struct fd_stream {
    fd_cmd *c;
    int n, cap;
} fd_stream;

void fd_stream_free(fd_stream *s);
void fd_stream_copy(fd_stream *dst, const fd_stream *src);
/* Appends a command (copies the words). */
fd_cmd *fd_stream_add(fd_stream *s, const uint32_t *w, int n);
void fd_stream_remove(fd_stream *s, int at, int count);
long fd_stream_words(const fd_stream *s);

/* What the generator aims at: --focus narrows the exploratory streams to
   one rule. */
typedef enum {
    FD_FOCUS_ALL,      /* everything both sides implement */
    FD_FOCUS_XFLIP,    /* x-flipped textured rectangles, u0 anywhere */
    FD_FOCUS_LINETIES, /* lines with exact halves, in all eight octants */
    FD_FOCUS_LINEG,    /* Gouraud lines, steps up and down */
    FD_FOCUS_QUAD,     /* Gouraud and textured quads */
    FD_FOCUS_V,        /* textured triangles varying v */
    FD_FOCUS_COUNT
} fd_focus;

extern const char *const fd_focus_names[FD_FOCUS_COUNT];

typedef struct fd_gen_options {
    int commands; /* after the background and the first drawing state */
    fd_focus focus;
    /* Leave out what psxstack does not model at all: the rectangle flips
       (GP0(E1h) bits 12-13) and the CLUT cache (a GP0(01h) before every 4-
       and 8-bit primitive, which makes jitterbug reload the palette every
       time). On by default; off finds those divergences again. */
    int avoid_unmodelled;
} fd_gen_options;

void fd_generate(uint64_t seed, uint64_t index, const fd_gen_options *o, fd_stream *s);
/* The 32 loads of 128 x 128 that set every pixel of VRAM (gen.c). */
void fd_gen_background(fd_rng *r, fd_stream *s);

/* The checked subsets (docs/testing.md, "Three modes";
   gen_check.c): streams of self-contained groups inside the strict subset,
   or of bounded primitives with their setup. commands is the number of
   groups after the background. */
void fd_generate_strict(uint64_t seed, uint64_t index, int commands, fd_stream *s);
void fd_generate_bounded(uint64_t seed, uint64_t index, int commands, fd_stream *s);

/* ------------------------------------------------------------------------ */
/* The drawing state the commands so far have set (the last E1h-E6h words,
   with a textured polygon's page in E1h's bits 0-8). */

typedef struct fd_state {
    uint32_t e1, e2, e3, e4, e5, e6;
} fd_state;

void fd_state_reset(fd_state *st);
void fd_state_apply(fd_state *st, const fd_cmd *c);

/* ------------------------------------------------------------------------ */
/* A command described, for classes and for the divergence filter. */

typedef enum {
    FD_K_STATE, FD_K_OTHER, FD_K_TRI, FD_K_QUAD, FD_K_LINE, FD_K_POLYLINE, FD_K_RECT,
    FD_K_FILL, FD_K_COPY, FD_K_LOAD, FD_K_READ, FD_K_COUNT
} fd_kind;

extern const char *const fd_kind_names[FD_K_COUNT];

typedef struct fd_desc {
    fd_kind kind;
    int op;
    int gouraud, textured, depth, raw, semi, semi_mode;
    int dither;     /* E1h bit 9 set */
    int mask_set, mask_check;
    int flip_x, flip_y;
    int window;     /* E2h not zero */
    int nv;
    int vx[64], vy[64]; /* vertices as jitterbug places them: sext11 + offset */
    int r[64], g[64], b[64];
    int u[4], v[4], clut_x, clut_y, tpx, tpy;
    int w, h;       /* rectangle or transfer size as sent */
    int offset_wraps; /* a vertex + offset outside -1024..1023 */
    int area_x0, area_y0, area_x1, area_y1;
    int outside;    /* a vertex outside the drawing area */
    int too_big;    /* past the size limit (1023 x 511, any edge) */
    int self_clut;  /* a 4/8-bit primitive whose bounds cover its CLUT */
    int self_tex;   /* a textured primitive whose bounds cover its texels */
    int line_tie;   /* a line segment with an exact half somewhere */
    int line_neg;   /* a Gouraud segment with a channel stepping down */
    int line_sloped;
    int quad_overlap; /* a quad whose two triangles overlap (order matters) */
    int copy_overlap; /* a VRAM-to-VRAM copy whose source and destination overlap */
    int copy_same;    /* ... and are the same rectangle */
    int max_channel;  /* the largest vertex colour channel */
    /* Filled in by the driver at a difference (main.c, "Ablation"). */
    unsigned needs; /* the FD_F_* features without which it goes away */
    int isolated;   /* it comes back from the VRAM and drawing state alone */
    int small;      /* every pixel within one step per channel, mask bits equal */
    int read;       /* in GPUREAD's words, not in VRAM */
    int coverage;   /* a line drawn flat, opaque and undithered on zeros, with no
                       mask settings, covers different pixels */
    int wrap_explains;  /* jitterbug given psxstack's wrapped vertices matches it */
    int order_explains; /* jitterbug drawing the quad's triangles in psxstack's
                           order matches it */
} fd_desc;

/* The features ablation takes away one at a time (main.c). */
enum {
    FD_F_DITHER = 1,     /* E1h bit 9 */
    FD_F_MASKCHECK = 2,  /* E6h bit 1 */
    FD_F_MASKSET = 4,    /* E6h bit 0 */
    FD_F_WINDOW = 8,     /* E2h */
    FD_F_SEMI = 16,      /* the command's semi-transparency bit */
    FD_F_MODULATE = 32,  /* a textured command without the raw bit */
    FD_F_GOURAUD = 64,   /* the command's shading bit */
    FD_F_TEXTURE = 128,  /* the command's texture bit */
    FD_F_UV = 256,       /* a polygon's texture coordinates differing between
                            vertices (taken away: every vertex gets the first's) */
    FD_F_COLOUR = 512,   /* vertex colours differing (taken away: every vertex
                            gets the first's, the shading bit kept) */
    FD_F_COUNT = 10
};
extern const char *const fd_feature_names[FD_F_COUNT];

void fd_describe(const fd_cmd *c, const fd_state *st, fd_desc *d);
/* The class: kind, flags and coordinate class, as a short string. */
void fd_class(const fd_desc *d, char *buf, size_t size);

/* The exploratory labels (divergences.c): triage hypotheses about the
   cause of a difference, used by `run` only, never by strict or bounded
   mode. A difference at a command matching one is counted under it and the
   stream goes on; that says nothing about which side is right. */
typedef struct fd_divergence {
    const char *id; /* short, for file names */
    const char *name;
    /* 0: psxstack differs from the console (by a dump or by psx-spx); 1: an
       open question neither settles, recorded in docs/conformance.md,
       "Open questions awaiting hardware" */
    int open;
    const char *evidence;
    int (*match)(const fd_desc *d);
} fd_divergence;

extern const fd_divergence fd_divergences[];
extern const int fd_divergence_count;
/* The first divergence d matches, or -1. */
int fd_divergence_of(const fd_desc *d);

/* The open-question lists (open_lists.c; tests/conformance/open). */
typedef struct fd_open_list {
    const char *name;
    void (*build)(fd_stream *s);
} fd_open_list;

extern const fd_open_list fd_open_lists[];
extern const int fd_open_list_count;
void fd_open_list_build(int i, fd_stream *s);

#endif
