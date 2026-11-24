/*
 * api_gte: the context's GTE (jb_gte_write, jb_gte_read, jb_gte_command)
 * against the internal one (jbi_gte, src/geom/gte.h), which tests/gte
 * checks against hand-worked cases: the one place this test uses the
 * internals.
 *
 *   - A seeded run: all 64 registers written with the same values to a
 *     context's GTE and to a jbi_gte (small values, and now and then any
 *     32 bits), then every command, with sf and lm on and off, MVMVA with
 *     every matrix (the reserved one too), vector and translation, and
 *     random bits 20-24; after each command all 64 registers and FLAG read
 *     the same. Again with precise_vertices and perspective_correct on,
 *     whose recording must change no register.
 *   - The numbering: each JB_GTE_* register constant against the internal
 *     data and control numbers, and each JB_GTE_* command word against the
 *     internal one.
 *   - Hand-checked cases: an identity rotation and a known projection
 *     through RTPS and RTPT, NCLIP and AVSZ3 on their results, and a FLAG
 *     from a saturated screen position.
 *   - Refused commands: above 25 bits (JB_ERROR_INVALID_ARGUMENT) and each
 *     undocumented command number (JB_ERROR_UNSUPPORTED), changing nothing.
 *   - Two contexts' GTEs are independent.
 */
#include "api_test.h"

#include "geom/gte.h"

static jb_context *ctx(int options)
{
    jb_config c;

    memset(&c, 0, sizeof c);
    c.precise_vertices = options;
    c.perspective_correct = options;
    return must_create(&c);
}

static uint32_t internal_read(const jbi_gte *g, int reg)
{
    return reg < 32 ? jbi_gte_read_data(g, reg) : jbi_gte_read_ctrl(g, reg - 32);
}

static void write_both(jb_context *jb, jbi_gte *g, int reg, uint32_t v)
{
    check("jb_gte_write", jb_gte_write(jb, reg, v), JB_OK);
    if (reg < 32)
        jbi_gte_write_data(g, reg, v);
    else
        jbi_gte_write_ctrl(g, reg - 32, v);
}

static int compare_all(jb_context *jb, const jbi_gte *g, const char *what)
{
    int reg, same = 1;
    char msg[96];

    for (reg = 0; reg < 64; ++reg) {
        uint32_t a = jb_gte_read(jb, reg), b = internal_read(g, reg);

        if (a != b) {
            snprintf(msg, sizeof msg, "%s: register %d", what, reg);
            check(msg, a, b);
            same = 0;
        }
    }
    ++api_t.checks;
    return same;
}

/* A register value: mostly small (so that results are not all saturated),
   sometimes any 32 bits. */
static uint32_t gen_value(gen *g, int reg)
{
    int k = rnd_below(g, 8);

    if (k == 0)
        return rnd(g);
    if (reg == JB_GTE_H)
        return 100 + (uint32_t)rnd_below(g, 600);
    if (reg == JB_GTE_OFX || reg == JB_GTE_OFY)
        return (uint32_t)(rnd_below(g, 400) << 16);
    if (reg == JB_GTE_TRZ)
        return 200 + (uint32_t)rnd_below(g, 4000);
    if (reg == JB_GTE_RGBC)
        return rnd(g);
    return JB_GTE_PAIR(rnd_below(g, 8192) - 4096, rnd_below(g, 8192) - 4096);
}

static const uint32_t commands[] = {
    JB_GTE_RTPS, JB_GTE_RTPT, JB_GTE_NCLIP, JB_GTE_OP,   JB_GTE_DPCS,  JB_GTE_INTPL,
    JB_GTE_MVMVA, JB_GTE_NCDS, JB_GTE_CDP,  JB_GTE_NCDT, JB_GTE_NCCS,  JB_GTE_CC,
    JB_GTE_NCS,  JB_GTE_NCT,  JB_GTE_SQR,   JB_GTE_DCPL, JB_GTE_DPCT,  JB_GTE_AVSZ3,
    JB_GTE_AVSZ4, JB_GTE_GPF, JB_GTE_GPL,   JB_GTE_NCCT,
};
enum { COMMANDS = sizeof commands / sizeof commands[0] };

static void seeded(int options, uint64_t seed, int cases)
{
    jb_context *jb = ctx(options);
    jbi_gte g;
    gen r = {0, 0, 0};
    int c, k, reg, ran = 0;
    char what[96];

    r.s = seed;
    jbi_gte_init(&g);
    compare_all(jb, &g, "after jb_create");
    for (c = 0; c < cases; ++c) {
        for (reg = 63; reg >= 0; --reg)
            write_both(jb, &g, reg, gen_value(&r, reg));
        snprintf(what, sizeof what, "case %d written", c);
        compare_all(jb, &g, what);
        for (k = 0; k < (int)COMMANDS; ++k) {
            uint32_t cmd = commands[k];

            if (rnd_below(&r, 2))
                cmd ^= JB_GTE_SF;
            if (rnd_below(&r, 2))
                cmd ^= JB_GTE_LM;
            if ((cmd & 0x3f) == (JB_GTE_MVMVA & 0x3f))
                cmd |= (uint32_t)rnd_below(&r, 64) << 13; /* mx, v, cv */
            cmd = (cmd & ~(0x1fu << 20)) | (uint32_t)rnd_below(&r, 32) << 20;
            if (rnd_below(&r, 4) == 0) /* a few inputs again */
                write_both(jb, &g, rnd_below(&r, 64), gen_value(&r, 0));
            check("jb_gte_command", jb_gte_command(jb, cmd), JB_OK);
            jbi_gte_command(&g, cmd);
            snprintf(what, sizeof what, "%s case %d command 0x%07x",
                     options ? "with the options" : "plain", c, (unsigned)cmd);
            if (!compare_all(jb, &g, what))
                break;
            ++ran;
        }
    }
    printf("seeded%s: %d commands compared\n", options ? " (options on)" : "", ran);
    jb_destroy(jb);
}

/* The header's numbers against the internal ones. */
static void numbering(void)
{
    check("VXY0", JB_GTE_VXY0, JBI_GTE_VXY0);
    check("RGBC", JB_GTE_RGBC, JBI_GTE_RGBC);
    check("OTZ", JB_GTE_OTZ, JBI_GTE_OTZ);
    check("IR3", JB_GTE_IR3, JBI_GTE_IR3);
    check("SXYP", JB_GTE_SXYP, JBI_GTE_SXYP);
    check("SZ3", JB_GTE_SZ3, JBI_GTE_SZ3);
    check("RES1", JB_GTE_RES1, JBI_GTE_RES1);
    check("MAC0", JB_GTE_MAC0, JBI_GTE_MAC0);
    check("IRGB", JB_GTE_IRGB, JBI_GTE_IRGB);
    check("LZCR", JB_GTE_LZCR, JBI_GTE_LZCR);
    check("RT11RT12", JB_GTE_RT11RT12, 32 + JBI_GTE_R11R12);
    check("RT33", JB_GTE_RT33, 32 + JBI_GTE_R33);
    check("TRZ", JB_GTE_TRZ, 32 + JBI_GTE_TRZ);
    check("L33", JB_GTE_L33, 32 + JBI_GTE_L33);
    check("BBK", JB_GTE_BBK, 32 + JBI_GTE_BBK);
    check("LB3", JB_GTE_LB3, 32 + JBI_GTE_LB3);
    check("BFC", JB_GTE_BFC, 32 + JBI_GTE_BFC);
    check("OFY", JB_GTE_OFY, 32 + JBI_GTE_OFY);
    check("H", JB_GTE_H, 32 + JBI_GTE_H);
    check("DQB", JB_GTE_DQB, 32 + JBI_GTE_DQB);
    check("ZSF4", JB_GTE_ZSF4, 32 + JBI_GTE_ZSF4);
    check("FLAG", JB_GTE_FLAG, 32 + JBI_GTE_FLAG);
    check("cmd RTPS", JB_GTE_RTPS, JBI_GTE_CMD_RTPS);
    check("cmd RTPT", JB_GTE_RTPT, JBI_GTE_CMD_RTPT);
    check("cmd NCLIP", JB_GTE_NCLIP, JBI_GTE_CMD_NCLIP);
    check("cmd OP", JB_GTE_OP, JBI_GTE_CMD_OP(0));
    check("cmd DPCS", JB_GTE_DPCS, JBI_GTE_CMD_DPCS);
    check("cmd INTPL", JB_GTE_INTPL, JBI_GTE_CMD_INTPL);
    check("cmd MVMVA", JB_GTE_MVMVA | JB_GTE_SF | JB_GTE_MX_LCM | JB_GTE_V_IR | JB_GTE_CV_FC |
                           JB_GTE_LM,
          JBI_GTE_CMD_MVMVA(1, 2, 3, 2, 1));
    check("cmd NCDS", JB_GTE_NCDS, JBI_GTE_CMD_NCDS);
    check("cmd CDP", JB_GTE_CDP, JBI_GTE_CMD_CDP);
    check("cmd NCDT", JB_GTE_NCDT, JBI_GTE_CMD_NCDT);
    check("cmd NCCS", JB_GTE_NCCS, JBI_GTE_CMD_NCCS);
    check("cmd CC", JB_GTE_CC, JBI_GTE_CMD_CC);
    check("cmd NCS", JB_GTE_NCS, JBI_GTE_CMD_NCS);
    check("cmd NCT", JB_GTE_NCT, JBI_GTE_CMD_NCT);
    check("cmd SQR", JB_GTE_SQR, JBI_GTE_CMD_SQR(0));
    check("cmd DCPL", JB_GTE_DCPL, JBI_GTE_CMD_DCPL);
    check("cmd DPCT", JB_GTE_DPCT, JBI_GTE_CMD_DPCT);
    check("cmd AVSZ3", JB_GTE_AVSZ3, JBI_GTE_CMD_AVSZ3);
    check("cmd AVSZ4", JB_GTE_AVSZ4, JBI_GTE_CMD_AVSZ4);
    check("cmd GPF", JB_GTE_GPF, JBI_GTE_CMD_GPF(0));
    check("cmd GPL", JB_GTE_GPL, JBI_GTE_CMD_GPL(0));
    check("cmd NCCT", JB_GTE_NCCT, JBI_GTE_CMD_NCCT);
    check("flag error", JB_GTE_FLAG_ERROR, JBI_GTE_FLAG_ERROR);
    check("flag MAC2+", JB_GTE_FLAG_MAC2_POS, JBI_GTE_FLAG_MAC_POS(2));
    check("flag MAC3-", JB_GTE_FLAG_MAC3_NEG, JBI_GTE_FLAG_MAC_NEG(3));
    check("flag IR1", JB_GTE_FLAG_IR1, JBI_GTE_FLAG_IR(1));
    check("flag colour B", JB_GTE_FLAG_COLOR_B, JBI_GTE_FLAG_COLOR(3));
    check("flag SX2", JB_GTE_FLAG_SX2, JBI_GTE_FLAG_SX);
    check("flag SY2", JB_GTE_FLAG_SY2, JBI_GTE_FLAG_SY);
    check("flag IR0", JB_GTE_FLAG_IR0, JBI_GTE_FLAG_IR0);
    check("JB_GTE_SX", JB_GTE_SX(0x0005fffbu), -5);
    check("JB_GTE_SY", JB_GTE_SY(0x0005fffbu), 5);
    check("JB_GTE_SY negative", JB_GTE_SY(0x8000u << 16), -32768);
    check("JB_GTE_PAIR", JB_GTE_PAIR(-1, 2), 0x0002ffffu);
}

/*
 * The identity rotation (4096 is 1.0), the camera 1000 units from the
 * plane z = 0 (TRZ), H 1000 and the screen centre at (160, 120): a vertex
 * (x, y, 0) is at depth 1000, the divide gives H / SZ = 1.0 exactly
 * (0x10000), so it lands at (160 + x, 120 + y).
 */
static void hand_cases(void)
{
    jb_context *jb = ctx(0);

    jb_gte_write(jb, JB_GTE_RT11RT12, JB_GTE_PAIR(4096, 0));
    jb_gte_write(jb, JB_GTE_RT13RT21, JB_GTE_PAIR(0, 0));
    jb_gte_write(jb, JB_GTE_RT22RT23, JB_GTE_PAIR(4096, 0));
    jb_gte_write(jb, JB_GTE_RT31RT32, JB_GTE_PAIR(0, 0));
    jb_gte_write(jb, JB_GTE_RT33, 4096);
    jb_gte_write(jb, JB_GTE_TRX, 0);
    jb_gte_write(jb, JB_GTE_TRY, 0);
    jb_gte_write(jb, JB_GTE_TRZ, 1000);
    jb_gte_write(jb, JB_GTE_H, 1000);
    jb_gte_write(jb, JB_GTE_OFX, 160u << 16);
    jb_gte_write(jb, JB_GTE_OFY, 120u << 16);

    jb_gte_write(jb, JB_GTE_VXY0, JB_GTE_PAIR(100, 50));
    jb_gte_write(jb, JB_GTE_VZ0, 0);
    check("RTPS", jb_gte_command(jb, JB_GTE_RTPS), JB_OK);
    check("RTPS: IR1", jb_gte_read(jb, JB_GTE_IR1), 100);
    check("RTPS: IR2", jb_gte_read(jb, JB_GTE_IR2), 50);
    check("RTPS: IR3", jb_gte_read(jb, JB_GTE_IR3), 1000);
    check("RTPS: SZ3", jb_gte_read(jb, JB_GTE_SZ3), 1000);
    check("RTPS: SX2", JB_GTE_SX(jb_gte_read(jb, JB_GTE_SXY2)), 260);
    check("RTPS: SY2", JB_GTE_SY(jb_gte_read(jb, JB_GTE_SXY2)), 170);
    check("RTPS: SXYP reads SXY2", jb_gte_read(jb, JB_GTE_SXYP),
          jb_gte_read(jb, JB_GTE_SXY2));
    check("RTPS: FLAG", jb_gte_read(jb, JB_GTE_FLAG), 0);

    /* A triangle: (100, 50), (-100, 0), (0, -50) at z 0. */
    jb_gte_write(jb, JB_GTE_VXY1, JB_GTE_PAIR(-100, 0));
    jb_gte_write(jb, JB_GTE_VZ1, 0);
    jb_gte_write(jb, JB_GTE_VXY2, JB_GTE_PAIR(0, -50));
    jb_gte_write(jb, JB_GTE_VZ2, 0);
    check("RTPT", jb_gte_command(jb, JB_GTE_RTPT), JB_OK);
    check("RTPT: SXY0", jb_gte_read(jb, JB_GTE_SXY0), JB_GTE_PAIR(260, 170));
    check("RTPT: SXY1", jb_gte_read(jb, JB_GTE_SXY1), JB_GTE_PAIR(60, 120));
    check("RTPT: SXY2", jb_gte_read(jb, JB_GTE_SXY2), JB_GTE_PAIR(160, 70));
    check("RTPT: SZ1-3", jb_gte_read(jb, JB_GTE_SZ1) + jb_gte_read(jb, JB_GTE_SZ2) +
                             jb_gte_read(jb, JB_GTE_SZ3),
          3000);
    /* x0 y1 + x1 y2 + x2 y0 - x0 y2 - x1 y0 - x2 y1
       = 260*120 + 60*70 + 160*170 - 260*70 - 60*170 - 160*120 = 15000 */
    check("NCLIP", jb_gte_command(jb, JB_GTE_NCLIP), JB_OK);
    check("NCLIP: MAC0", (int32_t)jb_gte_read(jb, JB_GTE_MAC0), 15000);
    /* The same triangle the other way round faces away. */
    jb_gte_write(jb, JB_GTE_SXY0, JB_GTE_PAIR(160, 70));
    jb_gte_write(jb, JB_GTE_SXY2, JB_GTE_PAIR(260, 170));
    jb_gte_command(jb, JB_GTE_NCLIP);
    check("NCLIP reversed: MAC0", (int32_t)jb_gte_read(jb, JB_GTE_MAC0), -15000);
    /* ZSF3 1365 (about 4096 / 3): OTZ = 1365 * 3000 >> 12 = 999. */
    jb_gte_write(jb, JB_GTE_ZSF3, 1365);
    check("AVSZ3", jb_gte_command(jb, JB_GTE_AVSZ3), JB_OK);
    check("AVSZ3: MAC0", jb_gte_read(jb, JB_GTE_MAC0), 1365 * 3000);
    check("AVSZ3: OTZ", jb_gte_read(jb, JB_GTE_OTZ), 999);

    /* x 2000 lands at 2160, beyond 0x3FF: SX2 saturates, FLAG says so. */
    jb_gte_write(jb, JB_GTE_VXY0, JB_GTE_PAIR(2000, 0));
    jb_gte_command(jb, JB_GTE_RTPS);
    check("saturated: SX2", JB_GTE_SX(jb_gte_read(jb, JB_GTE_SXY2)), 0x3ff);
    check("saturated: FLAG", jb_gte_read(jb, JB_GTE_FLAG),
          JB_GTE_FLAG_SX2 | JB_GTE_FLAG_ERROR);

    /* Registers keep what the header says. */
    jb_gte_write(jb, JB_GTE_VZ0, 0x12348765u);
    check("VZ0 sign-extends", jb_gte_read(jb, JB_GTE_VZ0), 0xffff8765u);
    jb_gte_write(jb, JB_GTE_SZ2, 0xabcd9999u);
    check("SZ2 zero-extends", jb_gte_read(jb, JB_GTE_SZ2), 0x9999u);
    jb_gte_write(jb, JB_GTE_IRGB, 0x7fffu);
    check("IRGB sets IR1", jb_gte_read(jb, JB_GTE_IR1), 31u << 7);
    check("ORGB reads IR back", jb_gte_read(jb, JB_GTE_ORGB), 0x7fffu);
    jb_gte_write(jb, JB_GTE_LZCS, 0x00ffffffu);
    check("LZCR", jb_gte_read(jb, JB_GTE_LZCR), 8);
    jb_gte_write(jb, JB_GTE_FLAG, 0xffffffffu);
    check("FLAG write", jb_gte_read(jb, JB_GTE_FLAG), 0xfffff000u);
    jb_destroy(jb);
}

static void refused(void)
{
    jb_context *jb = ctx(0);
    jbi_gte g;
    gen r = {77, 0, 0};
    uint32_t op;
    int reg, n = 0;

    jbi_gte_init(&g);
    for (reg = 0; reg < 64; ++reg)
        write_both(jb, &g, reg, gen_value(&r, reg));
    check("26 bits", jb_gte_command(jb, 0x2000001u), JB_ERROR_INVALID_ARGUMENT);
    check_str("26 bits: error string", jb_error_string(jb),
              "jb_gte_command: 0x2000001 is more than the 25 bits of a command");
    check("a whole cop2 instruction", jb_gte_command(jb, 0x4a180001u), JB_ERROR_INVALID_ARGUMENT);
    for (op = 0; op < 64; ++op) {
        int known = 0;
        unsigned k;

        for (k = 0; k < COMMANDS; ++k)
            known |= (commands[k] & 0x3f) == op;
        if (known)
            continue;
        ++n;
        check("undocumented command", jb_gte_command(jb, 0x0180000u | op), JB_ERROR_UNSUPPORTED);
    }
    check("undocumented command numbers", n, 64 - 22);
    check_str("undocumented: error string", jb_error_string(jb),
              "jb_gte_command: 0x18003c: command number 0x3c is not a GTE command");
    compare_all(jb, &g, "refused commands change nothing");
    check("register 64", jb_gte_write(jb, 64, 1), JB_ERROR_INVALID_ARGUMENT);
    check_str("register 64: error string", jb_error_string(jb),
              "jb_gte_write: register 64 is outside 0..63");
    check("register -1", jb_gte_write(jb, -1, 1), JB_ERROR_INVALID_ARGUMENT);
    check("read register 64", jb_gte_read(jb, 64), 0);
    check_str("read register 64: error string", jb_error_string(jb),
              "jb_gte_read: register 64 is outside 0..63");
    check("read register -1", jb_gte_read(jb, -1), 0);
    compare_all(jb, &g, "refused writes change nothing");
    check("write NULL", jb_gte_write(NULL, 0, 1), JB_ERROR_INVALID_ARGUMENT);
    check("read NULL", jb_gte_read(NULL, 0), 0);
    check("command NULL", jb_gte_command(NULL, JB_GTE_RTPS), JB_ERROR_INVALID_ARGUMENT);
    check("begin frame NULL", jb_gte_begin_frame(NULL), JB_ERROR_INVALID_ARGUMENT);
    check("begin frame", jb_gte_begin_frame(jb), JB_OK);
    compare_all(jb, &g, "jb_gte_begin_frame changes no register");
    jb_destroy(jb);
}

static void independent(void)
{
    jb_context *a = ctx(0), *b = ctx(1);
    int reg, zero = 1;

    jb_gte_write(a, JB_GTE_VXY0, JB_GTE_PAIR(10, 20));
    jb_gte_write(a, JB_GTE_RT11RT12, JB_GTE_PAIR(4096, 0));
    jb_gte_write(a, JB_GTE_TRZ, 500);
    jb_gte_write(a, JB_GTE_H, 300);
    jb_gte_command(a, JB_GTE_RTPS);
    for (reg = 0; reg < 64; ++reg)
        zero &= jb_gte_read(b, reg) == 0;
    check("the other context's GTE is untouched", zero, 1);
    jb_gte_write(b, JB_GTE_SXY2, 0x12345678u);
    check("its own write", jb_gte_read(b, JB_GTE_SXY2), 0x12345678u);
    check("not the first's", jb_gte_read(a, JB_GTE_SXY2) != 0x12345678u, 1);
    gp1(a, 0);
    check("GP1(00h) keeps the GTE", jb_gte_read(a, JB_GTE_TRZ), 500);
    jb_destroy(a);
    jb_destroy(b);
}

int main(void)
{
    numbering();
    seeded(0, 1, 300);
    seeded(1, 2, 100);
    hand_cases();
    refused();
    independent();
    return finish("api_gte");
}
