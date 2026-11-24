/*
 * A test of compare.c (the audit's comparison) on synthetic captures: each
 * case writes an old and a new 1x VRAM and audit map of one capture into
 * DIR/old and DIR/new, runs compare's main on them, and checks its exit
 * status: 0 for a change the envelope allows, 1 for one it does not, 2 for
 * input that is wrong. The cases include the two that the
 * first version of compare.c let through (a same-primitive blend from
 * 0x0001 to 0xffff, and a change no primitive's value accounts for).
 *
 *   selftest DIR
 *
 * DIR must exist; the test makes DIR/old and DIR/new (tests/CMakeLists.txt,
 * seed_audit_compare).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0777)
#endif

/* compare.c, its main renamed, and its exit on wrong input made a return
   to the case that ran it (status 2). */
#include <setjmp.h>
static jmp_buf input_error;
#define COMPARE_EXIT(status) longjmp(input_error, status)
int compare_main(int argc, char **argv);
#define main compare_main
#include "compare.c"
#undef main

static uint16_t vram_o[N], vram_n[N];
/* The map entries of PIX and PIX + 1, the only pixels a case changes. */
static px map_o[2], map_n[2];
static const px no_events;
static const char *dir;
static int failures;

/* Pixel (10, 10) of the capture: every case changes only that one. */
#define PIX (10 * W + 10)

static void reset(void)
{
    memset(vram_o, 0, sizeof vram_o);
    memset(vram_n, 0, sizeof vram_n);
    memset(map_o, 0, sizeof map_o);
    memset(map_n, 0, sizeof map_n);
}

/* The state recorded with an event: MODE (the semi-transparency mode and
   any state bits) plus what the kind implies unless FLAT_SHADED is given:
   semi-transparency for a blended write, the colour interpolated for a
   Gouraud triangle and a modulated textured one. */
enum { FLAT_SHADED = 0x100 };

static int state(int kind, int mode)
{
    int k = kind & 7, flags = mode & 0xff;

    if (kind & BLENDED)
        flags |= SEMI;
    if (!(mode & FLAT_SHADED) && (k == GOURAUD || (k == TEXTURED && !(mode & RAW))))
        flags |= SHADED;
    return flags;
}

/* An event at PIX: a write gives out, a pass leaves the pixel as it is. */
static ev *add(px *p, uint16_t *vram, uint32_t id, int kind)
{
    ev *e = &p->e[p->n++ % HIST];

    memset(e, 0, sizeof *e);
    e->id = id;
    e->kind = (uint8_t)kind;
    e->bg = vram[PIX];
    e->out = vram[PIX];
    return e;
}

static ev *write_px(px *p, uint16_t *vram, uint32_t id, int kind, uint16_t src, uint16_t texel,
                    int u, int v, int mode)
{
    ev *e = add(p, vram, id, kind);
    int k = kind & 7, textured = k == TEXTURED || k == SPRITE, c, col;
    int16_t *chan[3];

    e->src = src;
    e->texel = texel;
    e->u = (int16_t)u;
    e->v = (int16_t)v;
    e->flags = (uint8_t)state(kind, mode);
    /* The first colour before texturing and dithering that gives src (for
       a raw texture none is needed). */
    chan[0] = &e->r;
    chan[1] = &e->g;
    chan[2] = &e->b;
    if (!(textured && (e->flags & RAW)))
        for (c = 0; c < 3; ++c)
            for (col = 0; col < 256; ++col) {
                *chan[c] = (int16_t)col;
                if (((source(e) ^ src) >> (5 * c) & 31) == 0)
                    break;
            }
    e->out = (uint16_t)(((kind & BLENDED) ? blend(e->bg, src, mode) : src) |
                        (textured ? texel & 0x8000 : 0) | ((e->flags & SET_MASK) ? 0x8000 : 0));
    vram[PIX] = e->out;
    return e;
}

static void pass_mode(px *p, uint16_t *vram, uint32_t id, int kind, int u, int v, int mode)
{
    ev *e = add(p, vram, id, kind);

    e->u = (int16_t)u;
    e->v = (int16_t)v;
    e->flags = (uint8_t)state(kind, mode);
}

static void pass_px(px *p, uint16_t *vram, uint32_t id, int kind, int u, int v)
{
    pass_mode(p, vram, id, kind, u, v, 0);
}

static void put(const char *side, const uint16_t *vram, const px *map, int skip_map,
                uint32_t extra)
{
    char path[4096];
    FILE *f;
    uint32_t i, n = 0, size = (uint32_t)sizeof(px);

    snprintf(path, sizeof path, "%s/%s/00-case.op1.1x.raw", dir, side);
    f = fopen(path, "wb");
    if (f == NULL || fwrite(vram, 2, N, f) != N || fclose(f) != 0) {
        fprintf(stderr, "selftest: cannot write %s\n", path);
        exit(2);
    }
    for (i = 0; i < N; ++i)
        n += (vram_o[i] != vram_n[i] && !skip_map) || i == extra;
    snprintf(path, sizeof path, "%s/%s/00-case.op1.audit", dir, side);
    f = fopen(path, "wb");
    if (f == NULL || fwrite("JBAUDIT3", 1, 8, f) != 8 || fwrite(&n, 4, 1, f) != 1 ||
        fwrite(&size, 4, 1, f) != 1) {
        fprintf(stderr, "selftest: cannot write %s\n", path);
        exit(2);
    }
    for (i = 0; i < N; ++i)
        if ((vram_o[i] != vram_n[i] && !skip_map) || i == extra)
            if (fwrite(&i, 4, 1, f) != 1 ||
                fwrite(i == PIX || i == PIX + 1 ? &map[i - PIX] : &no_events, size, 1, f) != 1) {
                fprintf(stderr, "selftest: cannot write %s\n", path);
                exit(2);
            }
    if (fclose(f) != 0) {
        fprintf(stderr, "selftest: cannot write %s\n", path);
        exit(2);
    }
}

/* Writes the case (with the map options) and runs compare on it. */
static void run_opts(const char *name, int want, int skip_map, uint32_t extra)
{
    char o[4096], n[4096], l[4096];
    char *args[4];
    FILE *f;
    int got;

    put("old", vram_o, map_o, skip_map, extra);
    put("new", vram_n, map_n, 0, extra);
    snprintf(o, sizeof o, "%s/old", dir);
    snprintf(n, sizeof n, "%s/new", dir);
    snprintf(l, sizeof l, "%s/list.txt", dir);
    f = fopen(l, "w");
    if (f == NULL || fputs("00-case.op1\n", f) == EOF || fclose(f) != 0) {
        fprintf(stderr, "selftest: cannot write %s\n", l);
        exit(2);
    }
    args[0] = (char *)"compare";
    args[1] = o;
    args[2] = n;
    args[3] = l;
    printf("== %s (expect exit status %d)\n", name, want);
    fflush(stdout);
    got = setjmp(input_error);
    if (got == 0)
        got = compare_main(4, args);
    fflush(stdout);
    printf("%s %s: exit status %d\n", got == want ? "ok  " : "FAIL", name, got);
    failures += got != want;
}

static void run(const char *name, int want)
{
    run_opts(name, want, 0, N);
}

/* compare with a list naming no capture. */
static void run_empty(void)
{
    char o[4096], n[4096], l[4096];
    char *args[4];
    FILE *f;
    int got;

    snprintf(o, sizeof o, "%s/old", dir);
    snprintf(n, sizeof n, "%s/new", dir);
    snprintf(l, sizeof l, "%s/list.txt", dir);
    f = fopen(l, "w");
    if (f == NULL || fclose(f) != 0) {
        fprintf(stderr, "selftest: cannot write %s\n", l);
        exit(2);
    }
    args[0] = (char *)"compare";
    args[1] = o;
    args[2] = n;
    args[3] = l;
    printf("== no capture (expect exit status 2)\n");
    fflush(stdout);
    got = setjmp(input_error);
    if (got == 0)
        got = compare_main(4, args);
    fflush(stdout);
    printf("%s no capture: exit status %d\n", got == 2 ? "ok  " : "FAIL", got);
    failures += got != 2;
}

/* The same in both runs: a flat triangle (id 1) under everything. */
static void base(void)
{
    write_px(&map_o[0], vram_o, 1, FLAT, 0x1084, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 1, FLAT, 0x1084, 0, 0, 0, 0);
}

int main(int argc, char **argv)
{
    char path[4096];

    if (argc != 2) {
        fprintf(stderr, "usage: selftest DIR\n");
        return 2;
    }
    dir = argv[1];
    /* PIX, for the dither value (compare sets the same when it runs). */
    cur_x = PIX % W;
    cur_y = PIX / W;
    snprintf(path, sizeof path, "%s/old", dir);
    make_dir(path);
    snprintf(path, sizeof path, "%s/new", dir);
    make_dir(path);

    /* Inside. */
    reset();
    run("nothing changed", 0);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x0421, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x0842, 6, 5, 0);
    run("textured, the next texel", 0);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, 0);
    run("Gouraud, one step", 0);

    /* A recorded colour outside 0..255: 256 in red would give 32, which
       spills into green and passes for a one-step green change; a negative
       channel likewise is no renderer's. Both outside. */
    reset();
    write_px(&map_o[0], vram_o, 1, GOURAUD, 0, 0, 0, 0, 0);
    {
        ev *bad = write_px(&map_n[0], vram_n, 1, GOURAUD, 0x0020, 0, 0, 0, 0);
        bad->r = 256;
        bad->g = 0;
    }
    run("a recorded colour channel above 255", 1);
    reset();
    write_px(&map_o[0], vram_o, 1, GOURAUD, 0, 0, 0, 0, 0);
    {
        ev *bad = write_px(&map_n[0], vram_n, 1, GOURAUD, 0x0020, 0, 0, 0, 0);
        bad->r = 0;
        bad->g = -8;
    }
    run("a recorded colour channel below 0", 1);

    reset();
    write_px(&map_o[0], vram_o, 1, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 1, GOURAUD, 0x0422, 0, 0, 0, 0);
    write_px(&map_o[0], vram_o, 2, FLAT | BLENDED, 0x0c63, 0, 0, 0, 1);
    write_px(&map_n[0], vram_n, 2, FLAT | BLENDED, 0x0c63, 0, 0, 0, 1);
    run("flat, blended over a Gouraud pixel one step away", 0);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, TEXTURED | SKIP_T, 4, 4);
    run("switch: the next texel transparent", 0);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED | BLENDED, 0x0421, 0x8421, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x0842, 5, 6, SEMI);
    run("textured, the next texel without bit 15", 0);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x2108, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0422, 0x2108, 6, 5, 0);
    run("textured, the same texel, modulated one step", 0);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x0421, 0, 0, RAW);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x0842, 1, 0, RAW);
    run("raw textured, the next texel", 0);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, DITHER);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, DITHER);
    run("Gouraud, dithered, one step", 0);

    /* Outside: the two the first version let through. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 2, FLAT | BLENDED, 0x0001, 0, 0, 0, 1);
    write_px(&map_n[0], vram_n, 2, FLAT | BLENDED, 0x0001, 0, 0, 0, 1);
    map_o[0].e[1].out = vram_o[PIX] = 0x0001;
    map_n[0].e[1].out = vram_n[PIX] = 0xffff;
    run("the same blend, 0x0001 against 0xffff", 1);

    reset();
    vram_o[PIX] = 0x1234;
    vram_n[PIX] = 0x4321;
    run("a change no primitive wrote", 1);

    reset();
    base();
    vram_o[PIX] = 0x1234;
    run("a fill or copy over the last write in one run", 1);

    /* Outside: blends. */
    reset();
    write_px(&map_o[0], vram_o, 1, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 1, GOURAUD, 0x0424, 0, 0, 0, 0);
    write_px(&map_o[0], vram_o, 2, FLAT | BLENDED, 0x0c63, 0, 0, 0, 1);
    write_px(&map_n[0], vram_n, 2, FLAT | BLENDED, 0x0c63, 0, 0, 0, 1);
    run("blended over a Gouraud pixel three steps away", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, FLAT | BLENDED, 0x0c63, 0, 0, 0, 1);
    write_px(&map_n[0], vram_n, 2, FLAT | BLENDED, 0x0c64, 0, 0, 0, 1);
    run("a flat blend whose colour changed", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED | BLENDED, 0x0421, 0x8421, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED | BLENDED, 0x0421, 0x8421, 5, 5, 0);
    map_n[0].e[1].out = vram_n[PIX] = (uint16_t)(vram_n[PIX] + 1);
    run("a blend the recorded inputs do not give", 1);

    /* Outside: the same primitive. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x0421, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x0842, 7, 5, 0);
    run("textured, two texels away", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x0421, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x0842, 5, 5, 0);
    run("textured, the same coordinate, another texel", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x2108, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0423, 0x2108, 6, 5, 0);
    run("textured, the same texel, two steps", 1);

    /* A raw texture's colour is the texel. The texel 0x0421 at
       (0, 0) in both runs, the colour and the value written 0x0421 in one
       and 0x0422 in the other. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x0421, 0, 0, RAW);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0422, 0x0421, 0, 0, RAW);
    run("raw textured, the same texel, another colour", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x0421, 0, 0, RAW | DITHER);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x0842, 1, 0, RAW | DITHER);
    run("raw textured, dithered", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x0421, 0, 0, RAW | SHADED);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x0842, 1, 0, RAW | SHADED);
    run("raw textured, the colour interpolated", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x2108, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0422, 0x2108, 6, 5, 0)->r = 200;
    run("textured, a colour that is not the texel modulated", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x2108, 5, 5, FLAT_SHADED);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0422, 0x2108, 6, 5, FLAT_SHADED);
    run("textured, flat-shaded, the colour before texturing changed", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x8421, 5, 5, SEMI);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x8842, 6, 5, SEMI);
    run("textured, semi-transparent, bit 15, not blended", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0423, 0, 0, 0, 0);
    run("Gouraud, two steps", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, 0)->g += 8;
    run("Gouraud, a colour that is not the colour before dithering / 8", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, DITHER);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, DITHER)->r = 16;
    run("Gouraud, dithered, the colour computed without the dither value", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, DITHER);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, 0);
    run("Gouraud, dithered in one run", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0x0421, 0, 0, 0);
    run("Gouraud, with a texel", 1);

    reset();
    write_px(&map_o[0], vram_o, 1, FLAT, 0x1084, 0, 0, 0, SET_MASK);
    write_px(&map_n[0], vram_n, 1, FLAT, 0x1084, 0, 0, 0, SET_MASK);
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, CHECK_MASK);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, CHECK_MASK);
    run("Gouraud, written under check_mask over bit 15", 1);

    reset();
    write_px(&map_o[0], vram_o, 1, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 1, GOURAUD, 0x0422, 0, 0, 0, 0);
    write_px(&map_o[0], vram_o, 2, FLAT | BLENDED, 0x0c63, 0, 0, 0, 1);
    write_px(&map_n[0], vram_n, 2, FLAT | BLENDED, 0x0c63, 0, 0, 0, 1)->r += 1;
    run("flat, blended, the colour before dividing by 8 changed", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, LINE, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, LINE, 0x0422, 0, 0, 0, 0);
    run("a line's colour", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, 0);
    map_n[0].e[1].flags |= SET_MASK;
    map_n[0].e[1].out = vram_n[PIX] = 0x8422;
    run("Gouraud, set_mask in one run", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, SPRITE, 0x0421, 0x0421, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, SPRITE, 0x0842, 0x0842, 6, 5, 0);
    run("a sprite, the next texel", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TILE, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, TILE, 0x0422, 0, 0, 0, 0);
    run("a tile's colour", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, FLAT, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, FLAT, 0x0422, 0, 0, 0, 0);
    run("a flat triangle's colour", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD | BLENDED, 0x0421, 0, 0, 0, 1);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, 1);
    run("Gouraud, blended in one run", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED | BLENDED, 0x0421, 0x8421, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0c63, 0x8c63, 6, 5, 0);
    run("textured, blended in one run with the same bit 15", 1);

    /* Outside: switches. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, TEXTURED | SKIP_T, 6, 4);
    run("switch: a transparent texel three away", 1);

    /* Below primitive 3, primitive 2 left bit 15 in the new run only (the
       next texel has it). */
    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x0421, 0x0421, 5, 5, 0);
    write_px(&map_n[0], vram_n, 2, TEXTURED, 0x0842, 0x8842, 6, 5, 0);
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, CHECK_MASK);
    pass_mode(&map_n[0], vram_n, 3, TEXTURED | SKIP_M, 4, 4, CHECK_MASK);
    run("switch: through the mask bit", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, CHECK_MASK);
    pass_mode(&map_n[0], vram_n, 3, TEXTURED | SKIP_M, 4, 4, CHECK_MASK);
    run("switch: a mask pass over a pixel without bit 15", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    pass_mode(&map_n[0], vram_n, 3, TEXTURED | SKIP_T, 4, 4, SET_MASK);
    run("switch: the pass with another state than the write", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, GOURAUD | SKIP_T, 4, 4);
    run("switch: a transparent pass by an untextured primitive", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    run("switch: not covered in the other run", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, TEXTURED | SKIP_T, 3, 4);
    run("switch: the same coordinate transparent", 1);

    /* The allowed switch above with a sprite as the later primitive:
       a sprite's texel coordinate does not move under the fix. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_o[0], vram_o, 3, SPRITE, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, SPRITE | SKIP_T, 4, 4);
    run("switch: a sprite, the next texel transparent", 1);

    /* The same with the kinds recorded differently in the two runs. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, SPRITE | SKIP_T, 4, 4);
    run("switch: a textured write, a sprite's pass", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 3, SPRITE, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, TEXTURED | SKIP_T, 4, 4);
    run("switch: a sprite's write, a textured pass", 1);

    reset();
    write_px(&map_o[0], vram_o, 1, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 1, GOURAUD, 0x0425, 0, 0, 0, 0);
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, TEXTURED | SKIP_T, 4, 4);
    run("switch: revealing a Gouraud pixel four steps away", 1);

    reset();
    base();
    write_px(&map_o[0], vram_o, 3, TEXTURED, 0x7c00, 0x7c00, 3, 4, 0);
    pass_px(&map_n[0], vram_n, 3, TEXTURED | SKIP_T, 4, 4);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    run("switch: a write in the other run after the pass", 1);

    /* Outside: history. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 2, TEXTURED, 0x7c00, 0x7c00, 0, 1, 0);
    pass_px(&map_n[0], vram_n, 2, TEXTURED | SKIP_T, 1, 1);
    {
        uint32_t id;

        /* Later primitives pass over the pixel in both runs, until the new
           run's history no longer holds the pass of 2. */
        for (id = 3; id < 3 + HIST; ++id) {
            pass_px(&map_o[0], vram_o, id, TEXTURED | SKIP_T, 1, 1);
            pass_px(&map_n[0], vram_n, id, TEXTURED | SKIP_T, 1, 1);
        }
    }
    run("switch: the pass older than the history", 1);

    /* Wrong input. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, 0);
    run_opts("a map without the changed pixel", 2, 1, N);
    run_opts("a map with an unchanged pixel", 2, 0, 0);
    run_empty();

    /* Two pixels, one inside and one outside. */
    reset();
    base();
    write_px(&map_o[0], vram_o, 2, GOURAUD, 0x0421, 0, 0, 0, 0);
    write_px(&map_n[0], vram_n, 2, GOURAUD, 0x0422, 0, 0, 0, 0);
    vram_o[PIX + 1] = 1;
    run("one pixel inside, one no primitive wrote", 1);

    printf("%s\n", failures ? "FAILED" : "all cases as expected");
    return failures != 0;
}
