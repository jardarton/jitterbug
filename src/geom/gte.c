/*
 * GTE (COP2) emulation; see gte.h for the register semantics.
 *
 * Arithmetic model:
 *   - MAC1..3 accumulate in 44 bits. After every addition step a result
 *     outside -2^43..2^43-1 sets the MAC overflow flag and wraps to 44 bits.
 *     The 44-bit sum, shifted right by 12 when sf is set, is truncated to
 *     32 bits into MAC1..3.
 *   - IR1..3 saturate MAC1..3 to -0x8000..0x7fff, or 0..0x7fff with lm.
 *   - MAC0 is a 32-bit result checked for overflow (flags 15, 16) on the
 *     full-precision value; the full value feeds SX2/SY2, IR0 and OTZ.
 *   - Colours pushed to the RGB FIFO are MAC1..3 >> 4 saturated to 0..255,
 *     with the CODE byte from RGBC.
 *   - RTPS/RTPT divide H by SZ3 with the hardware's Newton-Raphson
 *     reciprocal (unr_table) and saturate the quotient to 0x1ffff.
 *
 * With a precise table that is enabled (jbi_gte_set_precise), RTPS/RTPT
 * also record each projected vertex's unrounded screen position for
 * upscaled rendering (precise.h, record_precise below). Recording only
 * reads values the projection computed anyway; no register or flag changes. RTPS/RTPT are
 * the only commands that make new screen positions: SXYP and SXY0..2
 * writes move values the program read out (records are keyed by value, so a
 * value keeps its record wherever it goes), and NCLIP and AVSZ only read.
 */
#include "geom/gte.h"

#include <stddef.h>

typedef int64_t s64;
typedef uint64_t u64;
typedef int32_t s32;
typedef uint32_t u32;

/* Every function below that touches registers takes the GTE as g. */
#define D (g->data)
#define C (g->ctrl)
/* FLAG bits raised by the current command. */
#define flag (g->flag)

/* The reciprocal seed table: entry i is max(0, (0x40000 / (i + 0x100) + 1)
   / 2 - 0x101), for i = 0..0x100, expanded at compile time. */
#define UNR_V(i) ((0x40000 / ((i) + 0x100) + 1) / 2 - 0x101)
#define UNR(i) (UNR_V(i) > 0 ? UNR_V(i) : 0)
#define UNR4(i) UNR(i), UNR((i) + 1), UNR((i) + 2), UNR((i) + 3)
#define UNR16(i) UNR4(i), UNR4((i) + 4), UNR4((i) + 8), UNR4((i) + 12)
#define UNR64(i) UNR16(i), UNR16((i) + 16), UNR16((i) + 32), UNR16((i) + 48)
static const unsigned char unr_table[0x101] = {
    UNR64(0), UNR64(64), UNR64(128), UNR64(192), UNR(256),
};

static inline s32 lo16(u32 v) { return (short)v; }
static inline s32 hi16(u32 v) { return (short)(v >> 16); }
static inline u32 sext16(u32 v) { return (u32)(s32)(short)v; }

static u32 count_leading(u32 v)
{
    if (v & 0x80000000u)
        v = ~v;
    return v != 0 ? (u32)__builtin_clz(v) : 32;
}

/* ------------------------------------------------------------------------ */
/* Saturation and overflow checks. */

/* 44-bit accumulator step for MAC i (1..3). */
static s64 A(jbi_gte *g, int i, s64 v)
{
    if (v > 0x7FFFFFFFFFFLL)
        flag |= JBI_GTE_FLAG_MAC_POS(i);
    else if (v < -0x80000000000LL)
        flag |= JBI_GTE_FLAG_MAC_NEG(i);
    return (s64)((u64)v << 20) >> 20;
}

/* IR i (1..3) from a 32-bit MAC value. */
static s32 lm_b(jbi_gte *g, int i, s32 v, int lm)
{
    s32 low = lm ? 0 : -0x8000;

    if (v < low) {
        flag |= JBI_GTE_FLAG_IR(i);
        return low;
    }
    if (v > 0x7fff) {
        flag |= JBI_GTE_FLAG_IR(i);
        return 0x7fff;
    }
    return v;
}

/* Colour component i (1..3). */
static u32 lm_c(jbi_gte *g, int i, s32 v)
{
    if (v < 0) {
        flag |= JBI_GTE_FLAG_COLOR(i);
        return 0;
    }
    if (v > 0xff) {
        flag |= JBI_GTE_FLAG_COLOR(i);
        return 0xff;
    }
    return (u32)v;
}

/* SZ3 or OTZ. */
static u32 lm_d(jbi_gte *g, s64 v)
{
    if (v < 0) {
        flag |= JBI_GTE_FLAG_SZ_OTZ;
        return 0;
    }
    if (v > 0xffff) {
        flag |= JBI_GTE_FLAG_SZ_OTZ;
        return 0xffff;
    }
    return (u32)v;
}

/* MAC0 overflow check. */
static s64 F(jbi_gte *g, s64 v)
{
    if (v > 0x7fffffffLL)
        flag |= JBI_GTE_FLAG_MAC0_POS;
    else if (v < -0x80000000LL)
        flag |= JBI_GTE_FLAG_MAC0_NEG;
    return v;
}

/* Screen coordinate: SX2 (bit JBI_GTE_FLAG_SX) or SY2. */
static s32 lm_g(jbi_gte *g, u32 bit, s64 v)
{
    if (v < -0x400) {
        flag |= bit;
        return -0x400;
    }
    if (v > 0x3ff) {
        flag |= bit;
        return 0x3ff;
    }
    return (s32)v;
}

/* IR0. */
static s32 lm_h(jbi_gte *g, s64 v)
{
    if (v < 0) {
        flag |= JBI_GTE_FLAG_IR0;
        return 0;
    }
    if (v > 0x1000) {
        flag |= JBI_GTE_FLAG_IR0;
        return 0x1000;
    }
    return (s32)v;
}

/* ------------------------------------------------------------------------ */
/* Register helpers. */

static inline s32 ir(const jbi_gte *g, int i) { return lo16(D[JBI_GTE_IR0 + i]); }

/* MAC i = sum >> sh (truncated); IR i saturated from it. */
static void set_mac_ir(jbi_gte *g, int i, s64 sum, int sh, int lm)
{
    s32 mac = (s32)(sum >> sh);

    D[JBI_GTE_MAC0 + i] = (u32)mac;
    D[JBI_GTE_IR0 + i] = (u32)lm_b(g, i, mac, lm);
}

static void push_color(jbi_gte *g)
{
    D[JBI_GTE_RGB0] = D[JBI_GTE_RGB1];
    D[JBI_GTE_RGB1] = D[JBI_GTE_RGB2];
    D[JBI_GTE_RGB2] = lm_c(g, 1, (s32)D[JBI_GTE_MAC1] >> 4) |
                      lm_c(g, 2, (s32)D[JBI_GTE_MAC2] >> 4) << 8 |
                      lm_c(g, 3, (s32)D[JBI_GTE_MAC3] >> 4) << 16 |
                      (D[JBI_GTE_RGBC] & 0xff000000u);
}

static void push_sz(jbi_gte *g, u32 z)
{
    D[JBI_GTE_SZ0] = D[JBI_GTE_SZ1];
    D[JBI_GTE_SZ1] = D[JBI_GTE_SZ2];
    D[JBI_GTE_SZ2] = D[JBI_GTE_SZ3];
    D[JBI_GTE_SZ3] = z;
}

static void push_sxy(jbi_gte *g, s32 x, s32 y)
{
    D[JBI_GTE_SXY0] = D[JBI_GTE_SXY1];
    D[JBI_GTE_SXY1] = D[JBI_GTE_SXY2];
    D[JBI_GTE_SXY2] = ((u32)x & 0xffff) | (u32)y << 16;
}

/* Element (row, col) of the matrix whose first control register is base
   (0 RT, 8 LLM, 16 LCM). */
static s32 mat(const jbi_gte *g, int base, int row, int col)
{
    int k = row * 3 + col;
    u32 w = C[base + (k >> 1)];

    return (k & 1) ? hi16(w) : lo16(w);
}

static void vertex(const jbi_gte *g, int n, s32 v[3])
{
    v[0] = lo16(D[n * 2]);
    v[1] = hi16(D[n * 2]);
    v[2] = lo16(D[n * 2 + 1]);
}

/* sum = t * 4096 + m[row] . v, with a 44-bit check after each step. */
static s64 dot(jbi_gte *g, int i, s64 t, const s32 m[3], const s32 v[3])
{
    s64 sum = A(g, i, t * 4096 + (s64)m[0] * v[0]);

    sum = A(g, i, sum + (s64)m[1] * v[1]);
    return A(g, i, sum + (s64)m[2] * v[2]);
}

static void mat_row(const jbi_gte *g, int base, int row, s32 m[3])
{
    m[0] = mat(g, base, row, 0);
    m[1] = mat(g, base, row, 1);
    m[2] = mat(g, base, row, 2);
}

/* MAC/IR = (t << 12 + matrix * v) >> sh, for all three rows. */
static void transform(jbi_gte *g, int base, const s32 *t, const s32 v[3], int sh, int lm)
{
    int i;

    for (i = 0; i < 3; i++) {
        s32 m[3];

        mat_row(g, base, i, m);
        set_mac_ir(g, i + 1, dot(g, i + 1, t ? t[i] : 0, m, v), sh, lm);
    }
}

/* ------------------------------------------------------------------------ */
/* Perspective transformation. */

u32 jbi_gte_divide(u32 h, u32 sz, int *overflow)
{
    u32 z, n, d, u;
    u64 q;

    h &= 0xffff;
    sz &= 0xffff;
    if (!(h < sz * 2)) {
        *overflow = 1;
        return 0x1ffff;
    }
    *overflow = 0;
    z = (u32)__builtin_clz(sz) - 16;
    n = h << z;
    d = sz << z;
    u = unr_table[(d - 0x7fc0) >> 7] + 0x101u;
    d = (0x2000080u - d * u) >> 8;
    d = (0x0000080u + d * u) >> 8;
    q = ((u64)n * d + 0x8000u) >> 16;
    return q > 0x1ffff ? 0x1ffff : (u32)q;
}

static u32 divide(jbi_gte *g, u32 h, u32 sz)
{
    int overflow;
    u32 q = jbi_gte_divide(h, sz, &overflow);

    if (overflow)
        flag |= JBI_GTE_FLAG_DIVIDE;
    return q;
}

static int round_nearest(double v)
{
    int i = (int)(v + 0.5);

    return (double)i > v + 0.5 ? i - 1 : i;
}

/*
 * Records the vertex just pushed to SXY2 into the GTE's precise table
 * (precise.h). x and y are the GTE's 16.16 screen positions, IR * quotient
 * + OF, before the truncation to integers and the saturation; d1, d2 and z
 * are the 44-bit sums behind MAC1..3 (view space in 1/4096 units with sf),
 * q the quotient.
 *
 * The GTE's own fraction (x >> 8) is exact to the hardware's arithmetic,
 * but its inputs are coarse: IR1/IR2 are whole view-space units and SZ a
 * whole depth unit, which near the camera is several pixels per unit. So
 * the record projects the full-precision view-space position instead,
 * OF + (d1 / 2^sh) * H / (z / 4096), and falls back to the GTE's fraction
 * where that strays more than a pixel from it (a saturated IR, a divide
 * overflow, or a vertex the hardware projects differently). Vertices whose
 * SX or SY saturated are not recorded: many share each saturated key.
 */
static void record_precise(jbi_gte *g, s64 x, s64 y, s64 d1, s64 d2, s64 z, int sh, u32 q)
{
    jbi_precise_stats *counters = jbi_precise_counters(g->precise);
    jbi_precise_vertex pv;
    s64 ax = x >> 8, ay = y >> 8;
    s32 mac1 = (s32)(d1 >> sh), mac2 = (s32)(d2 >> sh);

    if ((x >> 16) < -0x400 || (x >> 16) > 0x3ff || (y >> 16) < -0x400 ||
        (y >> 16) > 0x3ff) {
        counters->saturated++;
        return;
    }
    pv.x = (int)ax;
    pv.y = (int)ay;
    pv.z = (int)D[JBI_GTE_SZ3];
    if (q < 0x1ffff && z > 0 && mac1 == ir(g, 1) && mac2 == ir(g, 2)) {
        double k = (double)(C[JBI_GTE_H] & 0xffff) * 4096.0 * JBI_PRECISE_ONE /
                   ((double)z * (double)(1 << sh));
        double bx = (double)(s32)C[JBI_GTE_OFX] / (65536 / JBI_PRECISE_ONE) + (double)d1 * k;
        double by = (double)(s32)C[JBI_GTE_OFY] / (65536 / JBI_PRECISE_ONE) + (double)d2 * k;

        if (bx >= (double)(ax - JBI_PRECISE_ONE) && bx <= (double)(ax + JBI_PRECISE_ONE) &&
            by >= (double)(ay - JBI_PRECISE_ONE) && by <= (double)(ay + JBI_PRECISE_ONE)) {
            pv.x = round_nearest(bx);
            pv.y = round_nearest(by);
            counters->refined++;
        } else {
            counters->coarse++;
        }
    } else {
        counters->coarse++;
    }
    jbi_precise_record(g->precise, D[JBI_GTE_SXY2], &pv);
}

static void rtp(jbi_gte *g, int n, int sh, int lm, int last)
{
    s32 v[3], m[3], tr[3];
    s64 z, d1, d2;
    s32 mac3, low;
    u32 q;
    s64 x, y;

    vertex(g, n, v);
    tr[0] = (s32)C[JBI_GTE_TRX];
    tr[1] = (s32)C[JBI_GTE_TRY];
    tr[2] = (s32)C[JBI_GTE_TRZ];

    mat_row(g, JBI_GTE_R11R12, 0, m);
    d1 = dot(g, 1, tr[0], m, v);
    set_mac_ir(g, 1, d1, sh, lm);
    mat_row(g, JBI_GTE_R11R12, 1, m);
    d2 = dot(g, 2, tr[1], m, v);
    set_mac_ir(g, 2, d2, sh, lm);

    /* IR3 saturates MAC3, but its flag tests the sum >> 12 whatever sf. */
    mat_row(g, JBI_GTE_R11R12, 2, m);
    z = dot(g, 3, tr[2], m, v);
    mac3 = (s32)(z >> sh);
    D[JBI_GTE_MAC3] = (u32)mac3;
    low = lm ? 0 : -0x8000;
    D[JBI_GTE_IR3] = (u32)(mac3 < low ? low : mac3 > 0x7fff ? 0x7fff : mac3);
    if ((z >> 12) < -0x8000 || (z >> 12) > 0x7fff)
        flag |= JBI_GTE_FLAG_IR(3);

    push_sz(g, lm_d(g, z >> 12));

    q = divide(g, C[JBI_GTE_H] & 0xffff, D[JBI_GTE_SZ3]);
    x = (s64)ir(g, 1) * q + (s32)C[JBI_GTE_OFX];
    x = F(g, x);
    D[JBI_GTE_MAC0] = (u32)x;
    y = F(g, (s64)ir(g, 2) * q + (s32)C[JBI_GTE_OFY]);
    D[JBI_GTE_MAC0] = (u32)y;
    push_sxy(g, lm_g(g, JBI_GTE_FLAG_SX, x >> 16), lm_g(g, JBI_GTE_FLAG_SY, y >> 16));
    if (g->precise != NULL && jbi_precise_enabled(g->precise))
        record_precise(g, x, y, d1, d2, z, sh, q);

    if (last) {
        s64 dq = F(g, (s64)lo16(C[JBI_GTE_DQA]) * q + (s32)C[JBI_GTE_DQB]);

        D[JBI_GTE_MAC0] = (u32)dq;
        D[JBI_GTE_IR0] = (u32)lm_h(g, dq >> 12);
    }
}

/* ------------------------------------------------------------------------ */
/* Colour pipeline. */

/* MAC = MAC + (FC - MAC) * IR0, then >> sh into MAC/IR and the FIFO. */
static void interpolate(jbi_gte *g, s64 mac[3], int sh, int lm)
{
    s32 ir0 = ir(g, 0);
    int i;

    for (i = 0; i < 3; i++) {
        s64 fc = (s64)(s32)C[JBI_GTE_RFC + i] * 4096;
        s32 t = lm_b(g, i + 1, (s32)(A(g, i + 1, fc - mac[i]) >> sh), 0);

        mac[i] = A(g, i + 1, (s64)t * ir0 + mac[i]);
    }
    for (i = 0; i < 3; i++)
        set_mac_ir(g, i + 1, mac[i], sh, lm);
    push_color(g);
}

/* MAC = [R*IR1, G*IR2, B*IR3] << 4. */
static void color_scale(jbi_gte *g, s64 mac[3])
{
    u32 rgbc = D[JBI_GTE_RGBC];
    int i;

    for (i = 0; i < 3; i++)
        mac[i] = A(g, i + 1, (s64)((rgbc >> (8 * i)) & 0xff) * ir(g, i + 1) * 16);
}

/* IR = (BK << 12 + LCM * IR) >> sh. */
static void color_matrix(jbi_gte *g, int sh, int lm)
{
    s32 v[3] = { ir(g, 1), ir(g, 2), ir(g, 3) };
    s32 bk[3] = { (s32)C[JBI_GTE_RBK], (s32)C[JBI_GTE_GBK], (s32)C[JBI_GTE_BBK] };

    transform(g, JBI_GTE_LR1LR2, bk, v, sh, lm);
}

/* IR = (LLM * Vn) >> sh. */
static void light(jbi_gte *g, int n, int sh, int lm)
{
    s32 v[3];

    vertex(g, n, v);
    transform(g, JBI_GTE_L11L12, 0, v, sh, lm);
}

static void finish_colors(jbi_gte *g, s64 mac[3], int sh, int lm)
{
    int i;

    for (i = 0; i < 3; i++)
        set_mac_ir(g, i + 1, mac[i], sh, lm);
    push_color(g);
}

static void ncs(jbi_gte *g, int n, int sh, int lm)
{
    light(g, n, sh, lm);
    color_matrix(g, sh, lm);
    push_color(g);
}

static void nccs(jbi_gte *g, int n, int sh, int lm)
{
    s64 mac[3];

    light(g, n, sh, lm);
    color_matrix(g, sh, lm);
    color_scale(g, mac);
    finish_colors(g, mac, sh, lm);
}

static void ncds(jbi_gte *g, int n, int sh, int lm)
{
    s64 mac[3];

    light(g, n, sh, lm);
    color_matrix(g, sh, lm);
    color_scale(g, mac);
    interpolate(g, mac, sh, lm);
}

static void dpcs(jbi_gte *g, u32 rgb, int sh, int lm)
{
    s64 mac[3];
    int i;

    for (i = 0; i < 3; i++)
        mac[i] = (s64)((rgb >> (8 * i)) & 0xff) << 16;
    interpolate(g, mac, sh, lm);
}

/* ------------------------------------------------------------------------ */
/* MVMVA. */

static void mvmva(jbi_gte *g, u32 cmd, int sh, int lm)
{
    int mx = (cmd >> 17) & 3, vsel = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
    s32 v[3], t[3] = { 0, 0, 0 }, m[3][3];
    int i, j;

    if (vsel == 3) {
        v[0] = ir(g, 1);
        v[1] = ir(g, 2);
        v[2] = ir(g, 3);
    } else {
        vertex(g, vsel, v);
    }
    if (cv != 3) {
        static const int base[3] = { JBI_GTE_TRX, JBI_GTE_RBK, JBI_GTE_RFC };

        for (i = 0; i < 3; i++)
            t[i] = (s32)C[base[cv] + i];
    }
    if (mx == 3) {
        /* Reserved: the hardware reads a garbage matrix. */
        s32 r = (s32)(D[JBI_GTE_RGBC] & 0xff) << 4;

        m[0][0] = -r;
        m[0][1] = r;
        m[0][2] = ir(g, 0);
        for (j = 0; j < 3; j++) {
            m[1][j] = mat(g, JBI_GTE_R11R12, 0, 2);
            m[2][j] = mat(g, JBI_GTE_R11R12, 1, 1);
        }
    } else {
        for (i = 0; i < 3; i++)
            mat_row(g, mx * 8, i, m[i]);
    }

    for (i = 0; i < 3; i++) {
        s64 sum;

        if (cv == 2) {
            /* Hardware bug: the FC term and the first column only set
               flags; the result keeps the other two columns. */
            s64 partial = A(g, i + 1, (s64)t[i] * 4096 + (s64)m[i][0] * v[0]);

            lm_b(g, i + 1, (s32)(partial >> sh), 0);
            sum = A(g, i + 1, (s64)m[i][1] * v[1]);
            sum = A(g, i + 1, sum + (s64)m[i][2] * v[2]);
        } else {
            sum = dot(g, i + 1, t[i], m[i], v);
        }
        set_mac_ir(g, i + 1, sum, sh, lm);
    }
}

/* ------------------------------------------------------------------------ */

void jbi_gte_command(jbi_gte *g, u32 cmd)
{
    int sh = (cmd & JBI_GTE_SF) ? 12 : 0;
    int lm = (cmd & JBI_GTE_LM) != 0;
    s64 mac[3];
    int i;

    if (g->trace != NULL && g->precise != NULL && jbi_precise_enabled(g->precise))
        g->trace(g->trace_user, cmd, __builtin_return_address(0));
    flag = 0;
    switch (cmd & 0x3f) {
    case 0x01: /* RTPS */
        rtp(g, 0, sh, lm, 1);
        break;
    case 0x30: /* RTPT */
        rtp(g, 0, sh, lm, 0);
        rtp(g, 1, sh, lm, 0);
        rtp(g, 2, sh, lm, 1);
        break;
    case 0x06: { /* NCLIP */
        s64 x0 = lo16(D[JBI_GTE_SXY0]), y0 = hi16(D[JBI_GTE_SXY0]);
        s64 x1 = lo16(D[JBI_GTE_SXY1]), y1 = hi16(D[JBI_GTE_SXY1]);
        s64 x2 = lo16(D[JBI_GTE_SXY2]), y2 = hi16(D[JBI_GTE_SXY2]);

        D[JBI_GTE_MAC0] = (u32)F(g, x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1);
        break;
    }
    case 0x0c: { /* OP: IR x diagonal of RT */
        s64 d1 = mat(g, JBI_GTE_R11R12, 0, 0), d2 = mat(g, JBI_GTE_R11R12, 1, 1),
            d3 = mat(g, JBI_GTE_R11R12, 2, 2);
        s64 i1 = ir(g, 1), i2 = ir(g, 2), i3 = ir(g, 3);

        mac[0] = A(g, 1, i3 * d2 - i2 * d3);
        mac[1] = A(g, 2, i1 * d3 - i3 * d1);
        mac[2] = A(g, 3, i2 * d1 - i1 * d2);
        for (i = 0; i < 3; i++)
            set_mac_ir(g, i + 1, mac[i], sh, lm);
        break;
    }
    case 0x10: /* DPCS */
        dpcs(g, D[JBI_GTE_RGBC], sh, lm);
        break;
    case 0x11: /* INTPL */
        for (i = 0; i < 3; i++)
            mac[i] = (s64)ir(g, i + 1) * 4096;
        interpolate(g, mac, sh, lm);
        break;
    case 0x12: /* MVMVA */
        mvmva(g, cmd, sh, lm);
        break;
    case 0x13: /* NCDS */
        ncds(g, 0, sh, lm);
        break;
    case 0x14: /* CDP */
        color_matrix(g, sh, lm);
        color_scale(g, mac);
        interpolate(g, mac, sh, lm);
        break;
    case 0x16: /* NCDT */
        ncds(g, 0, sh, lm);
        ncds(g, 1, sh, lm);
        ncds(g, 2, sh, lm);
        break;
    case 0x1b: /* NCCS */
        nccs(g, 0, sh, lm);
        break;
    case 0x1c: /* CC */
        color_matrix(g, sh, lm);
        color_scale(g, mac);
        finish_colors(g, mac, sh, lm);
        break;
    case 0x1e: /* NCS */
        ncs(g, 0, sh, lm);
        break;
    case 0x20: /* NCT */
        ncs(g, 0, sh, lm);
        ncs(g, 1, sh, lm);
        ncs(g, 2, sh, lm);
        break;
    case 0x28: /* SQR */
        for (i = 0; i < 3; i++)
            set_mac_ir(g, i + 1, A(g, i + 1, (s64)ir(g, i + 1) * ir(g, i + 1)), sh, lm);
        break;
    case 0x29: /* DCPL */
        color_scale(g, mac);
        interpolate(g, mac, sh, lm);
        break;
    case 0x2a: /* DPCT: RGB0 three times, as the FIFO advances */
        for (i = 0; i < 3; i++)
            dpcs(g, D[JBI_GTE_RGB0], sh, lm);
        break;
    case 0x2d: { /* AVSZ3 */
        s64 sum = (s64)lo16(C[JBI_GTE_ZSF3]) *
                  (s64)(D[JBI_GTE_SZ1] + D[JBI_GTE_SZ2] + D[JBI_GTE_SZ3]);

        D[JBI_GTE_MAC0] = (u32)F(g, sum);
        D[JBI_GTE_OTZ] = lm_d(g, sum >> 12);
        break;
    }
    case 0x2e: { /* AVSZ4 */
        s64 sum = (s64)lo16(C[JBI_GTE_ZSF4]) *
                  (s64)(D[JBI_GTE_SZ0] + D[JBI_GTE_SZ1] + D[JBI_GTE_SZ2] + D[JBI_GTE_SZ3]);

        D[JBI_GTE_MAC0] = (u32)F(g, sum);
        D[JBI_GTE_OTZ] = lm_d(g, sum >> 12);
        break;
    }
    case 0x3d: /* GPF */
        for (i = 0; i < 3; i++)
            set_mac_ir(g, i + 1, A(g, i + 1, (s64)ir(g, 0) * ir(g, i + 1)), sh, lm);
        push_color(g);
        break;
    case 0x3e: /* GPL */
        for (i = 0; i < 3; i++) {
            s64 base = (s64)(s32)D[JBI_GTE_MAC1 + i] * ((s64)1 << sh);

            set_mac_ir(g, i + 1, A(g, i + 1, base + (s64)ir(g, 0) * ir(g, i + 1)), sh, lm);
        }
        push_color(g);
        break;
    case 0x3f: /* NCCT */
        nccs(g, 0, sh, lm);
        nccs(g, 1, sh, lm);
        nccs(g, 2, sh, lm);
        break;
    default:
        /* Unassigned command numbers: not emulated. */
        break;
    }
    if (flag & 0x7f87e000u)
        flag |= JBI_GTE_FLAG_ERROR;
    C[JBI_GTE_FLAG] = flag;
}

/* ------------------------------------------------------------------------ */
/* Register access. */

static u32 orgb(const jbi_gte *g)
{
    u32 out = 0;
    int i;

    for (i = 0; i < 3; i++) {
        s32 c = ir(g, i + 1) >> 7;

        out |= (u32)(c < 0 ? 0 : c > 0x1f ? 0x1f : c) << (5 * i);
    }
    return out;
}

void jbi_gte_init(jbi_gte *g)
{
    jbi_gte_reset(g);
    flag = 0;
    g->precise = NULL;
    g->trace = NULL;
    g->trace_user = NULL;
}

void jbi_gte_set_precise(jbi_gte *g, jbi_precise *t)
{
    g->precise = t;
}

void jbi_gte_reset(jbi_gte *g)
{
    int i;

    for (i = 0; i < 32; i++) {
        D[i] = 0;
        C[i] = 0;
    }
}

void jbi_gte_write_data(jbi_gte *g, int reg, u32 v)
{
    switch (reg & 31) {
    case JBI_GTE_VZ0:
    case JBI_GTE_VZ1:
    case JBI_GTE_VZ2:
    case JBI_GTE_IR0:
    case JBI_GTE_IR1:
    case JBI_GTE_IR2:
    case JBI_GTE_IR3:
        D[reg] = sext16(v);
        break;
    case JBI_GTE_OTZ:
    case JBI_GTE_SZ0:
    case JBI_GTE_SZ1:
    case JBI_GTE_SZ2:
    case JBI_GTE_SZ3:
        D[reg] = v & 0xffff;
        break;
    case JBI_GTE_SXYP:
        D[JBI_GTE_SXY0] = D[JBI_GTE_SXY1];
        D[JBI_GTE_SXY1] = D[JBI_GTE_SXY2];
        D[JBI_GTE_SXY2] = v;
        break;
    case JBI_GTE_IRGB:
        D[JBI_GTE_IR1] = (v & 0x1f) << 7;
        D[JBI_GTE_IR2] = ((v >> 5) & 0x1f) << 7;
        D[JBI_GTE_IR3] = ((v >> 10) & 0x1f) << 7;
        break;
    case JBI_GTE_ORGB:
    case JBI_GTE_LZCR:
        break;
    case JBI_GTE_LZCS:
        D[JBI_GTE_LZCS] = v;
        D[JBI_GTE_LZCR] = count_leading(v);
        break;
    default:
        D[reg & 31] = v;
        break;
    }
}

u32 jbi_gte_read_data(const jbi_gte *g, int reg)
{
    switch (reg & 31) {
    case JBI_GTE_SXYP:
        return D[JBI_GTE_SXY2];
    case JBI_GTE_IRGB:
    case JBI_GTE_ORGB:
        return orgb(g);
    default:
        return D[reg & 31];
    }
}

void jbi_gte_write_ctrl(jbi_gte *g, int reg, u32 v)
{
    switch (reg & 31) {
    case JBI_GTE_R33:
    case JBI_GTE_L33:
    case JBI_GTE_LB3:
    case JBI_GTE_H:
    case JBI_GTE_DQA:
    case JBI_GTE_ZSF3:
    case JBI_GTE_ZSF4:
        C[reg] = sext16(v);
        break;
    case JBI_GTE_FLAG:
        v &= 0x7ffff000u;
        if (v & 0x7f87e000u)
            v |= JBI_GTE_FLAG_ERROR;
        C[JBI_GTE_FLAG] = v;
        break;
    default:
        C[reg & 31] = v;
        break;
    }
}

u32 jbi_gte_read_ctrl(const jbi_gte *g, int reg) { return C[reg & 31]; }
