/*
 * Tests for the precise-vertex records (src/geom/precise.c and the
 * recording in gte.c; interface in src/geom/precise.h):
 *   - recording on and off give identical GTE results, over random states;
 *   - lookups hit what the GTE projected this frame and miss anything else;
 *   - the same key twice keeps the last at the same depth, both at
 *     different depths; a crowded probe chain evicts its oldest record;
 *   - a polygon's lookup takes the records projected together, and drops
 *     the depth of a vertex projected far from the others;
 *   - begin_frame forgets everything;
 *   - derived and offset records;
 *   - accuracy: precise positions against the exact projection and the
 *     integer SXY, and a slowly moving vertex across frames.
 * Run with -v for the measurements.
 */
#include <stdio.h>
#include <string.h>

#include "geom/gte.h"
#include "geom/precise.h"

/* The GTE under test, and the short forms of translated assembly. */
static jbi_gte gte;
#define gte_mtc2(value, reg) jbi_gte_write_data(&gte, (reg), (uint32_t)(value))
#define gte_mfc2(reg) jbi_gte_read_data(&gte, (reg))
#define gte_ctc2(value, reg) jbi_gte_write_ctrl(&gte, (reg), (uint32_t)(value))
#define gte_cfc2(reg) jbi_gte_read_ctrl(&gte, (reg))
#define gte_cmd(cmd) jbi_gte_command(&gte, (cmd))
/* The precise table the GTE records into. */
static jbi_precise *table;

static int failures, checks, verbose;

#define CHECK(cond, ...)                                                          \
    do {                                                                          \
        checks++;                                                                 \
        if (!(cond)) {                                                            \
            failures++;                                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                           \
            printf(__VA_ARGS__);                                                  \
            printf("\n");                                                         \
        }                                                                         \
    } while (0)

static unsigned xy(int x, int y) { return ((unsigned)x & 0xffff) | (unsigned)y << 16; }
static int sx(unsigned v) { return (short)(v & 0xffff); }
static int sy(unsigned v) { return (short)(v >> 16); }

/* xorshift32 */
static unsigned s_rng = 0x12345678u;
static unsigned rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static int rnd_range(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

typedef struct {
    unsigned data[32], ctrl[32];
} State;

static void save(State *s)
{
    int i;

    for (i = 0; i < 32; i++) {
        s->data[i] = gte.data[i];
        s->ctrl[i] = gte.ctrl[i];
    }
}

static void load(const State *s)
{
    int i;

    for (i = 0; i < 32; i++) {
        gte.data[i] = s->data[i];
        gte.ctrl[i] = s->ctrl[i];
    }
}

/* A rotation about y by angle a (4.12), a typical projection (H 341,
   centre 256,128), translation t. */
static void camera(double a, int tx, int ty, int tz)
{
    double c = __builtin_cos(a), s = __builtin_sin(a);
    int c12 = (int)(c * 4096.0 + (c < 0 ? -0.5 : 0.5)), s12 = (int)(s * 4096.0 + (s < 0 ? -0.5 : 0.5));

    gte_ctc2(xy(c12, 0), JBI_GTE_R11R12);
    gte_ctc2(xy(s12, 0), JBI_GTE_R13R21);
    gte_ctc2(xy(4096, 0), JBI_GTE_R22R23);
    gte_ctc2(xy(-s12, 0), JBI_GTE_R31R32);
    gte_ctc2(c12, JBI_GTE_R33);
    gte_ctc2((unsigned)tx, JBI_GTE_TRX);
    gte_ctc2((unsigned)ty, JBI_GTE_TRY);
    gte_ctc2((unsigned)tz, JBI_GTE_TRZ);
    gte_ctc2(256u << 16, JBI_GTE_OFX);
    gte_ctc2(128u << 16, JBI_GTE_OFY);
    gte_ctc2(341, JBI_GTE_H);
    gte_ctc2(0xfffffe00u, JBI_GTE_DQA);
    gte_ctc2(0x01400000u, JBI_GTE_DQB);
    gte_ctc2(0x155, JBI_GTE_ZSF3);
    gte_ctc2(0x100, JBI_GTE_ZSF4);
}

static void set_vertex(int n, int x, int y, int z)
{
    gte_mtc2(xy(x, y), JBI_GTE_VXY0 + 2 * n);
    gte_mtc2((unsigned)z & 0xffff, JBI_GTE_VZ0 + 2 * n);
}

/* The exact projection of (x, y, z) by the matrix and translation in the
   GTE registers, in pixels. */
static void exact(int x, int y, int z, double *px, double *py)
{
    double m[9], t[3], v[3] = { x, y, z }, r[3];
    int i, k;

    for (k = 0; k < 9; k++) {
        unsigned w = gte.ctrl[JBI_GTE_R11R12 + k / 2];
        m[k] = (k & 1 ? (short)(w >> 16) : (short)w) / 4096.0;
    }
    t[0] = (int)gte.ctrl[JBI_GTE_TRX];
    t[1] = (int)gte.ctrl[JBI_GTE_TRY];
    t[2] = (int)gte.ctrl[JBI_GTE_TRZ];
    for (i = 0; i < 3; i++)
        r[i] = t[i] + m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
    *px = 256.0 + r[0] * 341.0 / r[2];
    *py = 128.0 + r[1] * 341.0 / r[2];
}

/* ------------------------------------------------------------------------ */

/* Random register files and commands: every command in the list, with
   recording off and on, must leave every register identical. */
static void test_bit_exact(void)
{
    static const unsigned cmds[] = {
        0x0180001, 0x0280030, 0x0080001, 0x0080030, 0x01a0001, 0x1400006, 0x158002d,
        0x168002e, 0x0480012, 0x049e012, 0x04a6012, 0x0486012, 0x190003d, 0x1a0003e,
        0x0e80413, 0x0f80416, 0x0d80420, 0x108041b, 0x118043f, 0x0780010, 0x0980011,
        0x0a00428, 0x170000c, 0x0c8041e, 0x138041c, 0x1280414, 0x0f8002a, 0x0680029,
    };
    State before, off, on;
    int iter, i, mismatches = 0;

    for (iter = 0; iter < 200000; iter++) {
        unsigned cmd = cmds[rnd() % (sizeof(cmds) / sizeof(cmds[0]))];

        jbi_gte_reset(&gte);
        for (i = 0; i < 32; i++) {
            /* Mostly modest values, sometimes anything: saturation, divide
               overflow and MAC overflow paths. */
            unsigned r = rnd();
            jbi_gte_write_data(&gte, i, (rnd() & 7) ? (r & 0x0fff0fffu) ^ ((r & 0x08000800u) ? 0xf000f000u : 0) : rnd());
            jbi_gte_write_ctrl(&gte, i, (rnd() & 7) ? (rnd() & 0x00ff00ffu) : rnd());
        }
        if (rnd() & 1) {
            camera((rnd() % 6283) / 1000.0, rnd_range(-4000, 4000), rnd_range(-4000, 4000),
                   rnd_range(-200, 12000));
        }
        save(&before);

        jbi_precise_enable(table, 0);
        jbi_precise_begin_frame(table);
        jbi_gte_command(&gte, cmd);
        save(&off);

        load(&before);
        jbi_precise_enable(table, 1);
        jbi_precise_begin_frame(table);
        jbi_gte_command(&gte, cmd);
        save(&on);
        jbi_precise_enable(table, 0);

        if (memcmp(&off, &on, sizeof(off)) != 0)
            mismatches++;
    }
    CHECK(mismatches == 0, "%d of 200000 random commands differ with recording on", mismatches);
}

/* Nothing is recorded while disabled. */
static void test_disabled(void)
{
    jbi_precise_vertex v;

    jbi_gte_reset(&gte);
    camera(0.3, 100, -50, 3000);
    jbi_precise_enable(table, 0);
    jbi_precise_begin_frame(table);
    set_vertex(0, 120, 40, -60);
    gte_cmd(0x0180001);
    CHECK(!jbi_precise_lookup(table, gte_mfc2(JBI_GTE_SXY2), &v), "recorded while disabled");
    CHECK(jbi_precise_count(table) == 0, "count %d while disabled", jbi_precise_count(table));
}

static void test_hit_miss_reset(void)
{
    jbi_precise_vertex v;
    unsigned sxy[3];
    int i;

    jbi_gte_reset(&gte);
    camera(0.3, 100, -50, 3000);
    jbi_precise_enable(table, 1);
    jbi_precise_begin_frame(table);
    set_vertex(0, 120, 40, -60);
    set_vertex(1, -300, 10, 200);
    set_vertex(2, 50, -250, 900);
    gte_cmd(0x0280030);
    for (i = 0; i < 3; i++) {
        double ex, ey;
        int vx = i == 0 ? 120 : i == 1 ? -300 : 50, vy = i == 0 ? 40 : i == 1 ? 10 : -250,
            vz = i == 0 ? -60 : i == 1 ? 200 : 900;

        sxy[i] = gte_mfc2(JBI_GTE_SXY0 + i);
        CHECK(jbi_precise_lookup(table, sxy[i], &v), "vertex %d not recorded", i);
        CHECK(v.z == (int)gte_mfc2(JBI_GTE_SZ1 + i), "vertex %d z %d, SZ %u", i, v.z,
              gte_mfc2(JBI_GTE_SZ1 + i));
        CHECK(v.x >= sx(sxy[i]) * 256 - 256 && v.x < sx(sxy[i]) * 256 + 512 &&
                  v.y >= sy(sxy[i]) * 256 - 256 && v.y < sy(sxy[i]) * 256 + 512,
              "vertex %d at (%d, %d)/256 for key (%d, %d)", i, v.x, v.y, sx(sxy[i]), sy(sxy[i]));
        exact(vx, vy, vz, &ex, &ey);
        CHECK(__builtin_fabs(v.x / 256.0 - ex) < 0.02 && __builtin_fabs(v.y / 256.0 - ey) < 0.02,
              "vertex %d at (%.3f, %.3f), exactly (%.3f, %.3f)", i, v.x / 256.0, v.y / 256.0, ex, ey);
    }
    /* Neighbouring keys and junk miss. */
    CHECK(!jbi_precise_lookup(table, sxy[0] + 1, &v), "x+1 hits");
    CHECK(!jbi_precise_lookup(table, sxy[0] + 0x10000, &v), "y+1 hits");
    CHECK(!jbi_precise_lookup(table, xy(-7, 300), &v), "an unrecorded key hits");
    /* The GPU reads 11-bit coordinates: bits 11..15 of each half are
       ignored by the lookup too. */
    {
        unsigned junk = (sxy[1] & 0x07ff07ffu) | 0xa000a000u;

        CHECK(jbi_precise_lookup(table, junk, &v), "lookup of %08x as %08x misses", sxy[1], junk);
    }

    /* SXYP writes and other FIFO moves record nothing new; a value written
       back keeps its record. */
    gte_mtc2(xy(10, 20), JBI_GTE_SXYP);
    CHECK(!jbi_precise_lookup(table, xy(10, 20), &v), "an SXYP write recorded");
    gte_mtc2(sxy[0], JBI_GTE_SXY2);
    CHECK(jbi_precise_lookup(table, gte_mfc2(JBI_GTE_SXY2), &v), "a written-back SXY lost its record");

    /* The next frame forgets. */
    jbi_precise_begin_frame(table);
    for (i = 0; i < 3; i++)
        CHECK(!jbi_precise_lookup(table, sxy[i], &v), "vertex %d survives begin_frame", i);
    CHECK(jbi_precise_count(table) == 0, "count %d after begin_frame", jbi_precise_count(table));
    jbi_precise_enable(table, 0);
}

/* Saturated projections are not recorded. */
static void test_saturated(void)
{
    jbi_precise_vertex v;

    jbi_gte_reset(&gte);
    camera(0.0, 0, 0, 0);
    jbi_precise_enable(table, 1);
    jbi_precise_begin_frame(table);
    set_vertex(0, 30000, 0, 100); /* far off the right edge */
    gte_cmd(0x0180001);
    CHECK(sx(gte_mfc2(JBI_GTE_SXY2)) == 0x3ff, "SX %d, want saturated", sx(gte_mfc2(JBI_GTE_SXY2)));
    CHECK(!jbi_precise_lookup(table, gte_mfc2(JBI_GTE_SXY2), &v), "a saturated vertex was recorded");
    jbi_precise_enable(table, 0);
}

static void test_collisions(void)
{
    jbi_precise_vertex v, a = { 100 * 256 + 10, 50 * 256 + 20, 300 }, b = { 100 * 256 + 200, 50 * 256 + 100, 400 };
    jbi_precise_stats st0, st1;
    unsigned key = xy(100, 50), keys[20], k;
    int n = 0;

    jbi_precise_enable(table, 1);
    jbi_precise_begin_frame(table);

    /* The same key twice at the same depth (within 1/16): the same vertex
       again, and the last write wins. */
    b.z = 315;
    jbi_precise_record(table, key, &a);
    jbi_precise_record(table, key, &b);
    CHECK(jbi_precise_lookup(table, key, &v) && v.x == b.x && v.y == b.y && v.z == b.z,
          "same key: got (%d, %d, %d)", v.x, v.y, v.z);
    CHECK(jbi_precise_count(table) == 1, "same key twice makes %d entries", jbi_precise_count(table));

    /* At another depth: another vertex on the same pixel, kept beside it;
       a single lookup gets the latest. */
    b.z = 400;
    jbi_precise_record(table, key, &b);
    CHECK(jbi_precise_count(table) == 2, "two depths make %d entries", jbi_precise_count(table));
    CHECK(jbi_precise_lookup(table, key, &v) && v.z == 400, "latest of two: z %d", v.z);
    jbi_precise_record(table, key, &a);
    CHECK(jbi_precise_count(table) == 2, "a third record at a known depth makes %d entries",
          jbi_precise_count(table));
    CHECK(jbi_precise_lookup(table, key, &v) && v.z == 300 && v.x == a.x, "latest again: z %d", v.z);

    /* Records more than a pixel from their key are refused. */
    a.x = 103 * 256;
    jbi_precise_record(table, xy(101, 50), &a);
    CHECK(!jbi_precise_lookup(table, xy(101, 50), &v), "a record 2 px from its key was kept");

    /* Twenty keys on one slot: the probe chain holds sixteen, and the
       seventeenth and later evict the oldest records, so the newest
       sixteen are found. */
    jbi_precise_begin_frame(table);
    for (k = 0; n < 20 && k < 0x40000000u; k++) {
        unsigned key2 = xy((int)(k % 1024) - 512, (int)(k / 1024) % 1024 - 512);

        if (((key2 * 0x9e3779b1u) >> 18) == ((key * 0x9e3779b1u) >> 18))
            keys[n++] = key2;
    }
    CHECK(n == 20, "found %d keys on one slot", n);
    jbi_precise_stats_get(table, &st0, 0);
    for (k = 0; k < 20; k++) {
        jbi_precise_vertex p = { sx(keys[k]) * 256 + (int)k, sy(keys[k]) * 256, (int)k + 1 };
        jbi_precise_record(table, keys[k], &p);
    }
    jbi_precise_stats_get(table, &st1, 0);
    CHECK(st1.evicted - st0.evicted == 4, "%u evictions, want 4", st1.evicted - st0.evicted);
    for (k = 0; k < 4; k++)
        CHECK(!jbi_precise_lookup(table, keys[k], &v), "old key %u kept", k);
    for (k = 4; k < 20; k++)
        CHECK(jbi_precise_lookup(table, keys[k], &v) && v.z == (int)k + 1, "key %u lost", k);
    jbi_precise_enable(table, 0);
}

/* A vertex at (x, y) pixels plus (fx, fy) 256ths, depth z. */
static jbi_precise_vertex pv(int x, int y, int fx, int fy, int z)
{
    jbi_precise_vertex v = { x * 256 + fx, y * 256 + fy, z };

    return v;
}

/* Records n unrelated vertices, to put projections between meshes. */
static void filler(int n)
{
    int i;

    for (i = 0; i < n; i++) {
        jbi_precise_vertex p = pv(-400 + i % 100, -200 - i / 100, 0, 0, 5000);

        jbi_precise_record(table, xy(-400 + i % 100, -200 - i / 100), &p);
    }
}

static void test_poly(void)
{
    /* Mesh A, a triangle near the camera; mesh B, far away, one of whose
       vertices lands on A's second vertex's pixel. */
    unsigned ka[3] = { xy(100, 100), xy(120, 100), xy(100, 120) };
    unsigned kb[3] = { xy(140, 90), xy(120, 100), xy(130, 110) };
    unsigned k4[4] = { xy(100, 100), xy(120, 100), xy(100, 120), xy(-5, -5) };
    jbi_precise_vertex a[3] = { pv(100, 100, 10, 10, 1000), pv(120, 100, 20, 20, 1100),
                              pv(100, 120, 30, 30, 1200) };
    jbi_precise_vertex b[3] = { pv(140, 90, 0, 0, 8000), pv(120, 100, 200, 200, 8100),
                              pv(130, 110, 0, 0, 8200) };
    jbi_precise_vertex v[4];
    unsigned hits;
    int i;

    jbi_precise_enable(table, 1);
    jbi_precise_begin_frame(table);
    for (i = 0; i < 3; i++)
        jbi_precise_record(table, ka[i], &a[i]);
    filler(100);
    for (i = 0; i < 3; i++)
        jbi_precise_record(table, kb[i], &b[i]);

    /* A single lookup of the shared pixel gets B's, the latest. */
    CHECK(jbi_precise_lookup(table, ka[1], &v[0]) && v[0].z == 8100, "shared key: z %d", v[0].z);
    /* A's triangle gets A's vertex there, position and depth. */
    hits = jbi_precise_lookup_poly(table, ka, 3, v);
    CHECK(hits == 7, "A: hits %x", hits);
    for (i = 0; i < 3; i++)
        CHECK(v[i].x == a[i].x && v[i].y == a[i].y && v[i].z == a[i].z,
              "A vertex %d: (%d, %d, %d)", i, v[i].x, v[i].y, v[i].z);
    /* B's triangle gets B's. */
    hits = jbi_precise_lookup_poly(table, kb, 3, v);
    CHECK(hits == 7, "B: hits %x", hits);
    for (i = 0; i < 3; i++)
        CHECK(v[i].x == b[i].x && v[i].y == b[i].y && v[i].z == b[i].z,
              "B vertex %d: (%d, %d, %d)", i, v[i].x, v[i].y, v[i].z);
    /* A quad with a missing vertex: the others as for A. */
    hits = jbi_precise_lookup_poly(table, k4, 4, v);
    CHECK(hits == 7 && v[1].z == a[1].z, "quad: hits %x, z %d", hits, v[1].z);
    /* A vertex projected again (same depth) keeps its place among A's:
       its range of projections still touches A's. */
    jbi_precise_record(table, ka[0], &a[0]);
    hits = jbi_precise_lookup_poly(table, ka, 3, v);
    CHECK(hits == 7 && v[1].z == a[1].z, "A after reprojection: z %d", v[1].z);

    /* A vertex with only another object's record, projected far from the
       polygon's others: its position, but no depth. */
    jbi_precise_begin_frame(table);
    jbi_precise_record(table, kb[1], &b[1]);
    filler(600);
    jbi_precise_record(table, ka[0], &a[0]);
    jbi_precise_record(table, ka[2], &a[2]);
    hits = jbi_precise_lookup_poly(table, ka, 3, v);
    CHECK(hits == 7, "isolated: hits %x", hits);
    CHECK(v[1].x == b[1].x && v[1].y == b[1].y && v[1].z == 0, "isolated vertex: (%d, %d, %d)",
          v[1].x, v[1].y, v[1].z);
    CHECK(v[0].z == a[0].z && v[2].z == a[2].z, "isolated: others' depths %d %d", v[0].z,
          v[2].z);
    /* Within 512 projections it keeps its depth. */
    jbi_precise_begin_frame(table);
    jbi_precise_record(table, kb[1], &b[1]);
    filler(400);
    jbi_precise_record(table, ka[0], &a[0]);
    jbi_precise_record(table, ka[2], &a[2]);
    hits = jbi_precise_lookup_poly(table, ka, 3, v);
    CHECK(hits == 7 && v[1].z == b[1].z, "near: z %d", v[1].z);

    /* set_depth corrects the latest record of its key only. */
    jbi_precise_begin_frame(table);
    jbi_precise_record(table, ka[1], &a[1]);
    jbi_precise_record(table, kb[1], &b[1]);
    jbi_precise_set_depth(table, kb[1], 500);
    hits = jbi_precise_lookup_poly(table, &ka[1], 1, v);
    CHECK(hits == 1 && v[0].z == 500, "set_depth: latest z %d", v[0].z);
    jbi_precise_record(table, ka[0], &a[0]);
    jbi_precise_record(table, ka[2], &a[2]);
    hits = jbi_precise_lookup_poly(table, ka, 3, v);
    CHECK(hits == 7 && v[1].z == 500, "set_depth: chosen z %d", v[1].z);
    jbi_precise_enable(table, 0);
}

static void test_derived(void)
{
    jbi_precise_vertex v, s = { 10 * 256 + 128, 20 * 256 + 64, 1000 }, a = { 30 * 256, 20 * 256, 2000 },
                        b = { 50 * 256 + 255, 60 * 256 + 255, 3000 };
    unsigned in[3] = { xy(10, 20), xy(30, 20), xy(50, 60) };
    int w[3] = { 128, 64, 64 };

    jbi_precise_enable(table, 1);
    jbi_precise_begin_frame(table);
    jbi_precise_record(table, in[0], &s);
    jbi_precise_record(table, in[1], &a);
    jbi_precise_record(table, in[2], &b);

    /* A weighted morph: (128 * S + 64 * (A + B)) >> 8 = (25, 30) in
       integers; precisely (25.5 + 0.25 + 0.25 * 0.996, ...). */
    jbi_precise_derive(table, xy(25, 30), in, w, 3, 8);
    CHECK(jbi_precise_lookup(table, xy(25, 30), &v), "derived vertex missing");
    CHECK(v.x == (128 * s.x + 64 * (a.x + b.x) + 128) >> 8 &&
              v.y == (128 * s.y + 64 * (a.y + b.y) + 128) >> 8 && v.z == 1750,
          "derived (%d, %d, %d)", v.x, v.y, v.z);

    /* A missing parent: nothing recorded. */
    in[2] = xy(-5, -5);
    jbi_precise_derive(table, xy(1, 1), in, w, 3, 8);
    CHECK(!jbi_precise_lookup(table, xy(1, 1), &v), "derived from a missing parent");

    /* Offsets: a rim vertex one pixel down. */
    jbi_precise_offset(table, xy(10, 21), xy(10, 20));
    CHECK(jbi_precise_lookup(table, xy(10, 21), &v) && v.x == s.x && v.y == s.y + 256 && v.z == s.z,
          "offset (%d, %d, %d)", v.x, v.y, v.z);
    jbi_precise_offset(table, xy(-3, 21), xy(-3, 20));
    CHECK(!jbi_precise_lookup(table, xy(-3, 21), &v), "offset from a missing vertex");

    /* A corrected depth replaces SZ; other keys are untouched. */
    jbi_precise_set_depth(table, xy(30, 20), 125);
    CHECK(jbi_precise_lookup(table, xy(30, 20), &v) && v.z == 125 && v.x == a.x, "depth %d", v.z);
    jbi_precise_set_depth(table, xy(31, 20), 5);
    CHECK(!jbi_precise_lookup(table, xy(31, 20), &v), "set_depth made a record");
    jbi_precise_enable(table, 0);
}

/* Random projections: where the record lies against the integer SXY, and
   how far the record and the integer SXY are from the exact projection. */
static void test_accuracy(void)
{
    double err_int = 0, err_prec = 0, sum_int = 0, sum_prec = 0;
    int n = 0, inside = 0, within1 = 0, hw_off = 0, i;
    jbi_precise_vertex v;

    jbi_precise_enable(table, 1);
    for (i = 0; i < 100000; i++) {
        int x = rnd_range(-3000, 3000), y = rnd_range(-3000, 3000), z = rnd_range(-3000, 3000);
        double ex, ey, ei, ep;
        unsigned s;

        jbi_gte_reset(&gte);
        camera((rnd() % 6283) / 1000.0, rnd_range(-2000, 2000), rnd_range(-2000, 2000),
               rnd_range(300, 20000));
        jbi_precise_begin_frame(table);
        set_vertex(0, x, y, z);
        gte_cmd(0x0180001);
        s = gte_mfc2(JBI_GTE_SXY2);
        if (!jbi_precise_lookup(table, s, &v))
            continue; /* behind the camera or saturated */
        exact(x, y, z, &ex, &ey);
        /* Projections the hardware itself gets wrong (a saturated IR, a
           divide overflow near the camera) are no test of the record. */
        if (__builtin_fabs(sx(s) + 0.5 - ex) > 2 || __builtin_fabs(sy(s) + 0.5 - ey) > 2) {
            hw_off++;
            continue;
        }
        n++;
        /* The integer SXY truncates, so its centre is at +0.5. */
        ei = __builtin_fabs(sx(s) + 0.5 - ex);
        ep = __builtin_fabs(v.x / 256.0 - ex);
        sum_int += ei;
        sum_prec += ep;
        if (ei > err_int)
            err_int = ei;
        if (ep > err_prec)
            err_prec = ep;
        inside += v.x >= sx(s) * 256 && v.x < sx(s) * 256 + 256 && v.y >= sy(s) * 256 &&
                  v.y < sy(s) * 256 + 256;
        within1 += __builtin_fabs(v.x / 256.0 - (sx(s) + 0.5)) <= 1.5 &&
                   __builtin_fabs(v.y / 256.0 - (sy(s) + 0.5)) <= 1.5;
    }
    jbi_precise_enable(table, 0);
    CHECK(n > 50000, "only %d usable projections", n);
    CHECK(within1 == n, "%d of %d records further than a pixel from their SXY", n - within1, n);
    CHECK(sum_prec / n < 0.05, "precise mean error %.4f px", sum_prec / n);
    if (verbose)
        printf("accuracy over %d projections (%d more the GTE itself projects off): integer "
               "SXY+0.5 error mean %.3f max %.3f px; precise mean %.4f max %.3f px; %.1f%% "
               "inside the SXY's pixel\n",
               n, hw_off, sum_int / n, err_int, sum_prec / n, err_prec, 100.0 * inside / n);
}

/* A vertex creeping right by 1/40 pixel per frame: the record follows it
   smoothly while the integer SXY steps once every 40 frames. */
static void test_slow_motion(void)
{
    int f, steps_int = 0, last_int = 0, last_prec = 0, max_dev = 0;
    double err_max = 0;
    jbi_precise_vertex v;

    jbi_precise_enable(table, 1);
    jbi_gte_reset(&gte);
    /* At depth 13640 one world unit is 341 / 13640 = 1/40 pixel: the
       vertex moves one unit per frame, from 100.3 pixels right of the
       centre. */
    camera(0.0, 0, 0, 13640);
    for (f = 0; f < 400; f++) {
        unsigned s;
        double ex, ey;

        jbi_precise_begin_frame(table);
        set_vertex(0, 4012 + f, 0, 0);
        gte_cmd(0x0180001);
        s = gte_mfc2(JBI_GTE_SXY2);
        CHECK(jbi_precise_lookup(table, s, &v), "frame %d: no record", f);
        exact(4012 + f, 0, 0, &ex, &ey);
        if (__builtin_fabs(v.x / 256.0 - ex) > err_max)
            err_max = __builtin_fabs(v.x / 256.0 - ex);
        if (f > 0) {
            int d = v.x - last_prec;

            steps_int += sx(s) != last_int;
            /* 1/40 px is 6.4/256 per frame */
            if (d - 6 > max_dev)
                max_dev = d - 6;
            if (6 - d > max_dev)
                max_dev = 6 - d;
        }
        last_int = sx(s);
        last_prec = v.x;
    }
    jbi_precise_enable(table, 0);
    CHECK(max_dev <= 2, "precise step deviates by %d/256 px from 6.4/256", max_dev);
    CHECK(err_max < 0.01, "precise error up to %.4f px", err_max);
    CHECK(steps_int == 10, "integer SXY stepped %d times in 400 frames, want 10", steps_int);
    if (verbose)
        printf("slow motion, 1/40 px per frame over 400 frames: integer SXY steps %d times "
               "(by a whole pixel); precise steps 6.4/256 px +- %d/256, error <= %.4f px\n",
               steps_int, max_dev, err_max);
}

/* The cost of recording, per projected vertex. It counts x86 time-stamp
   cycles, so elsewhere it does nothing. */
static void bench(void)
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned long long t0, t1, t2;
    int i;

    jbi_gte_reset(&gte);
    camera(0.3, 100, -50, 3000);
    set_vertex(0, 120, 40, -60);
    set_vertex(1, -300, 10, 200);
    set_vertex(2, 50, -250, 900);
    jbi_precise_enable(table, 0);
    t0 = __builtin_ia32_rdtsc();
    for (i = 0; i < 1000000; i++) {
        gte_ctc2((unsigned)i & 1023, JBI_GTE_TRX);
        gte_cmd(0x0280030);
    }
    t1 = __builtin_ia32_rdtsc();
    jbi_precise_enable(table, 1);
    for (i = 0; i < 1000000; i++) {
        if ((i & 1023) == 0)
            jbi_precise_begin_frame(table);
        gte_ctc2((unsigned)i & 1023, JBI_GTE_TRX);
        gte_cmd(0x0280030);
    }
    t2 = __builtin_ia32_rdtsc();
    jbi_precise_enable(table, 0);
    printf("RTPT: %.1f cycles off, %.1f cycles with recording (%.1f per vertex)\n",
           (double)(t1 - t0) / 1e6, (double)(t2 - t1) / 1e6, (double)(t2 - t1 - (t1 - t0)) / 3e6);
#else
    printf("--bench needs an x86 time-stamp counter\n");
#endif
}

int main(int argc, char **argv)
{
    int i, do_bench = 0;

    table = jbi_precise_create();
    if (table == NULL)
        return 2;
    jbi_gte_init(&gte);
    jbi_gte_set_precise(&gte, table);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0)
            verbose = 1;
        if (strcmp(argv[i], "--bench") == 0)
            do_bench = 1;
    }
    test_bit_exact();
    test_disabled();
    test_hit_miss_reset();
    test_saturated();
    test_collisions();
    test_poly();
    test_derived();
    test_accuracy();
    test_slow_motion();
    if (do_bench)
        bench();
    printf("precise_test: %d checks, %d failures\n", checks, failures);
    jbi_precise_destroy(table);
    return failures != 0;
}
