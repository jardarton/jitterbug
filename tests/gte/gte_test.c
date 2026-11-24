/*
 * Unit tests for the GTE emulation (src/geom/gte.c). Each expected
 * value is worked out by hand from the hardware's documented arithmetic;
 * the comments show the working.
 */
#include <stdio.h>

#include "geom/gte.h"

/* The GTE under test, and the short forms of translated assembly. */
static jbi_gte gte;
#define gte_mtc2(value, reg) jbi_gte_write_data(&gte, (reg), (uint32_t)(value))
#define gte_mfc2(reg) jbi_gte_read_data(&gte, (reg))
#define gte_ctc2(value, reg) jbi_gte_write_ctrl(&gte, (reg), (uint32_t)(value))
#define gte_cfc2(reg) jbi_gte_read_ctrl(&gte, (reg))
#define gte_cmd(cmd) jbi_gte_command(&gte, (cmd))

static int failures, checks;

static void check(const char *what, unsigned got, unsigned want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got 0x%08x, want 0x%08x\n", what, got, want);
    }
}

#define CD(reg, want) check(#reg, gte_mfc2(reg), (unsigned)(want))
#define CC(reg, want) check(#reg, gte_cfc2(reg), (unsigned)(want))

static unsigned xy(int x, int y) { return ((unsigned)x & 0xffff) | (unsigned)y << 16; }

static void identity(int base, int one)
{
    gte_ctc2(xy(one, 0), base + 0);
    gte_ctc2(xy(0, 0), base + 1);
    gte_ctc2(xy(one, 0), base + 2);
    gte_ctc2(xy(0, 0), base + 3);
    gte_ctc2(one, base + 4);
}

static void test_registers(void)
{
    jbi_gte_reset(&gte);
    gte_mtc2(0x12348765, JBI_GTE_VZ0);
    CD(JBI_GTE_VZ0, 0xffff8765);
    gte_mtc2(0x00007fff, JBI_GTE_IR1);
    CD(JBI_GTE_IR1, 0x7fff);
    gte_mtc2(0xffff8000, JBI_GTE_OTZ);
    CD(JBI_GTE_OTZ, 0x8000);
    gte_mtc2(0xabcd9999, JBI_GTE_SZ2);
    CD(JBI_GTE_SZ2, 0x9999);
    gte_mtc2(0x12345678, JBI_GTE_VXY1);
    CD(JBI_GTE_VXY1, 0x12345678);

    /* SXYP pushes the FIFO and reads as SXY2. */
    gte_mtc2(1, JBI_GTE_SXY0);
    gte_mtc2(2, JBI_GTE_SXY1);
    gte_mtc2(3, JBI_GTE_SXY2);
    gte_mtc2(4, JBI_GTE_SXYP);
    CD(JBI_GTE_SXY0, 2);
    CD(JBI_GTE_SXY1, 3);
    CD(JBI_GTE_SXY2, 4);
    CD(JBI_GTE_SXYP, 4);

    /* IRGB: 5-bit components << 7; ORGB: IR >> 7 saturated to 0..31. */
    gte_mtc2(0x7c1f, JBI_GTE_IRGB); /* r 31, g 0, b 31 */
    CD(JBI_GTE_IR1, 0xf80);
    CD(JBI_GTE_IR2, 0);
    CD(JBI_GTE_IR3, 0xf80);
    CD(JBI_GTE_ORGB, 0x7c1f);
    CD(JBI_GTE_IRGB, 0x7c1f);
    gte_mtc2(-5, JBI_GTE_IR1);       /* -> 0 */
    gte_mtc2(0x7fff, JBI_GTE_IR2);   /* 0xff -> 31 */
    gte_mtc2(0x180, JBI_GTE_IR3);    /* 3 */
    CD(JBI_GTE_ORGB, 0x0c00 | 0x03e0);
    gte_mtc2(0xffffffff, JBI_GTE_ORGB); /* read-only */
    CD(JBI_GTE_ORGB, 0x0fe0);

    /* LZCR counts leading bits equal to bit 31. */
    gte_mtc2(0, JBI_GTE_LZCS);
    CD(JBI_GTE_LZCR, 32);
    gte_mtc2(1, JBI_GTE_LZCS);
    CD(JBI_GTE_LZCR, 31);
    gte_mtc2(0xffffffff, JBI_GTE_LZCS);
    CD(JBI_GTE_LZCR, 32);
    gte_mtc2(0x80000000, JBI_GTE_LZCS);
    CD(JBI_GTE_LZCR, 1);
    gte_mtc2(0x00ffffff, JBI_GTE_LZCS);
    CD(JBI_GTE_LZCR, 8);
    gte_mtc2(0xff000000, JBI_GTE_LZCS);
    CD(JBI_GTE_LZCR, 8);
    gte_mtc2(0xfffe0000, JBI_GTE_LZCS);
    CD(JBI_GTE_LZCR, 15);
    gte_mtc2(0, JBI_GTE_LZCR); /* read-only */
    CD(JBI_GTE_LZCR, 15);
    CD(JBI_GTE_LZCS, 0xfffe0000);

    /* 16-bit control registers read back sign-extended, H included. */
    gte_ctc2(0x00018000, JBI_GTE_R33);
    CC(JBI_GTE_R33, 0xffff8000);
    gte_ctc2(0xffff, JBI_GTE_H);
    CC(JBI_GTE_H, 0xffffffff);
    gte_ctc2(0x12345678, JBI_GTE_R11R12);
    CC(JBI_GTE_R11R12, 0x12345678);
    gte_ctc2(0x9abc, JBI_GTE_ZSF4);
    CC(JBI_GTE_ZSF4, 0xffff9abc);

    /* FLAG keeps bits 12..30; bit 31 ORs bits 13..18 and 23..30. */
    gte_ctc2(0xffffffff, JBI_GTE_FLAG);
    CC(JBI_GTE_FLAG, 0xfffff000);
    gte_ctc2(0x1000, JBI_GTE_FLAG);   /* IR0 is not an error bit */
    CC(JBI_GTE_FLAG, 0x1000);
    gte_ctc2(0x400000, JBI_GTE_FLAG); /* nor is IR3 */
    CC(JBI_GTE_FLAG, 0x400000);
    gte_ctc2(0x2000, JBI_GTE_FLAG);
    CC(JBI_GTE_FLAG, 0x80002000);
    gte_ctc2(0x800000, JBI_GTE_FLAG);
    CC(JBI_GTE_FLAG, 0x80800000);
}

static void setup_projection(void)
{
    jbi_gte_reset(&gte);
    identity(JBI_GTE_R11R12, 0x1000);
    gte_ctc2(0, JBI_GTE_TRX);
    gte_ctc2(0, JBI_GTE_TRY);
    gte_ctc2(1000, JBI_GTE_TRZ);
    gte_ctc2(160 << 16, JBI_GTE_OFX);
    gte_ctc2(120 << 16, JBI_GTE_OFY);
    gte_ctc2(500, JBI_GTE_H);
    gte_ctc2(-0x100, JBI_GTE_DQA);
    gte_ctc2(0x1000000, JBI_GTE_DQB);
}

static void test_rtps(void)
{
    /* V0 (100, -50, 0), TR z 1000, identity: MAC/IR = (100, -50, 1000),
       SZ3 = 1000. H/SZ3 = 500/1000: the reciprocal gives exactly 0x8000.
       z = 6, n = 32000, d = 64000, index 244, unr 5, u = 262;
       d = (0x2000080 - 16768000) >> 8 = 65572;
       d = (0x80 + 65572 * 262) >> 8 = 67109;
       q = (32000 * 67109 + 0x8000) >> 16 = 32768.
       SX = 100 * 0x8000 / 0x10000 + 160 = 210, SY = -25 + 120 = 95.
       MAC0 = -0x100 * 0x8000 + 0x1000000 = 0x800000, IR0 = 0x800. */
    setup_projection();
    gte_mtc2(xy(100, -50), JBI_GTE_VXY0);
    gte_mtc2(0, JBI_GTE_VZ0);
    gte_mtc2(7, JBI_GTE_SZ3);
    gte_mtc2(xy(1, 1), JBI_GTE_SXY2);
    gte_cmd(JBI_GTE_CMD_RTPS);
    CD(JBI_GTE_MAC1, 100);
    CD(JBI_GTE_MAC2, -50);
    CD(JBI_GTE_MAC3, 1000);
    CD(JBI_GTE_IR1, 100);
    CD(JBI_GTE_IR2, -50);
    CD(JBI_GTE_IR3, 1000);
    CD(JBI_GTE_SZ2, 7);
    CD(JBI_GTE_SZ3, 1000);
    CD(JBI_GTE_SXY1, xy(1, 1));
    CD(JBI_GTE_SXY2, xy(210, 95));
    CD(JBI_GTE_MAC0, 0x800000);
    CD(JBI_GTE_IR0, 0x800);
    CC(JBI_GTE_FLAG, 0);

    /* Division overflow: TR z 100, H 500 >= 2 * 100, so q = 0x1ffff.
       SX = (100 * 0x1ffff) >> 16 + 160 = 199 + 160 = 359;
       SY = (-50 * 0x1ffff) >> 16 + 120 = -100 + 120 = 20;
       MAC0 = -0x100 * 0x1ffff + 0x1000000 = -16776960, IR0 -> 0 (flag 12). */
    setup_projection();
    gte_ctc2(100, JBI_GTE_TRZ);
    gte_mtc2(xy(100, -50), JBI_GTE_VXY0);
    gte_mtc2(0, JBI_GTE_VZ0);
    gte_cmd(JBI_GTE_CMD_RTPS);
    CD(JBI_GTE_SZ3, 100);
    CD(JBI_GTE_SXY2, xy(359, 20));
    CD(JBI_GTE_MAC0, -16776960);
    CD(JBI_GTE_IR0, 0);
    CC(JBI_GTE_FLAG, 0x80021000);

    /* SX saturation: IR1 3000 -> 1500 + 160 = 1660 -> 0x3ff (flag 14). */
    setup_projection();
    gte_mtc2(xy(3000, 0), JBI_GTE_VXY0);
    gte_mtc2(0, JBI_GTE_VZ0);
    gte_cmd(JBI_GTE_CMD_RTPS);
    CD(JBI_GTE_SXY2, xy(0x3ff, 120));
    CC(JBI_GTE_FLAG, 0x80004000);

    /* Behind the camera: TR z -1000 -> MAC3 -1000, SZ3 0 (flag 18),
       H < 0 false -> divide overflow (flag 17). */
    setup_projection();
    gte_ctc2(-1000, JBI_GTE_TRZ);
    gte_mtc2(0, JBI_GTE_VXY0);
    gte_mtc2(0, JBI_GTE_VZ0);
    gte_cmd(JBI_GTE_CMD_RTPS);
    CD(JBI_GTE_MAC3, -1000);
    CD(JBI_GTE_IR3, -1000);
    CD(JBI_GTE_SZ3, 0);
    CC(JBI_GTE_FLAG, 0x80060000 | 0x1000);

    /* sf = 0: MAC3 keeps the full sum (1000 << 12), IR3 saturates to
       0x7fff, but the IR3 flag tests MAC3 >> 12 = 1000, so it stays clear.
       SZ3 = 1000 regardless of sf. IR1 = 100 << 12 saturates (flag 24). */
    setup_projection();
    gte_mtc2(xy(100, 0), JBI_GTE_VXY0);
    gte_mtc2(0, JBI_GTE_VZ0);
    gte_cmd(JBI_GTE_CMD_RTPS & ~JBI_GTE_SF);
    CD(JBI_GTE_MAC1, 100 << 12);
    CD(JBI_GTE_MAC3, 1000 << 12);
    CD(JBI_GTE_IR1, 0x7fff);
    CD(JBI_GTE_IR3, 0x7fff);
    CD(JBI_GTE_SZ3, 1000);
    CC(JBI_GTE_FLAG, 0x81000000 | 0x4000);
}

static void test_rtpt(void)
{
    /* Three vertices at depth 1000 + vz; each projects with H 500. The
       FIFOs end with the three results in order. V2 at z 1000 (SZ 2000):
       q = 500 / 2000 = 0x4000 exactly (d = 0xfa00 << 1... same index). */
    setup_projection();
    gte_mtc2(xy(100, -50), JBI_GTE_VXY0);
    gte_mtc2(0, JBI_GTE_VZ0);
    gte_mtc2(xy(-100, 50), JBI_GTE_VXY1);
    gte_mtc2(0, JBI_GTE_VZ1);
    gte_mtc2(xy(400, 0), JBI_GTE_VXY2);
    gte_mtc2(1000, JBI_GTE_VZ2);
    gte_cmd(JBI_GTE_CMD_RTPT);
    CD(JBI_GTE_SZ1, 1000);
    CD(JBI_GTE_SZ2, 1000);
    CD(JBI_GTE_SZ3, 2000);
    CD(JBI_GTE_SXY0, xy(210, 95));
    CD(JBI_GTE_SXY1, xy(110, 145));
    CD(JBI_GTE_SXY2, xy(260, 120));
    /* Depth cue from the last vertex: -0x100 * 0x4000 + 0x1000000. */
    CD(JBI_GTE_MAC0, 0xc00000);
    CD(JBI_GTE_IR0, 0xc00);
    CC(JBI_GTE_FLAG, 0);
}

static void test_nclip_avsz(void)
{
    jbi_gte_reset(&gte);
    gte_mtc2(xy(0, 0), JBI_GTE_SXY0);
    gte_mtc2(xy(10, 0), JBI_GTE_SXY1);
    gte_mtc2(xy(0, 10), JBI_GTE_SXY2);
    gte_cmd(JBI_GTE_CMD_NCLIP);
    CD(JBI_GTE_MAC0, 100);
    gte_mtc2(xy(0, 10), JBI_GTE_SXY1);
    gte_mtc2(xy(10, 0), JBI_GTE_SXY2);
    gte_cmd(JBI_GTE_CMD_NCLIP);
    CD(JBI_GTE_MAC0, -100);
    /* (-3, 4), (7, -2), (1, 9): -3*-2 + 7*9 + 1*4 - (-3*9) - 7*4 - 1*-2
       = 6 + 63 + 4 + 27 - 28 + 2 = 74. */
    gte_mtc2(xy(-3, 4), JBI_GTE_SXY0);
    gte_mtc2(xy(7, -2), JBI_GTE_SXY1);
    gte_mtc2(xy(1, 9), JBI_GTE_SXY2);
    gte_cmd(JBI_GTE_CMD_NCLIP);
    CD(JBI_GTE_MAC0, 74);
    CC(JBI_GTE_FLAG, 0);

    /* AVSZ3: 341 * 600 = 204600, OTZ = 49. AVSZ4: 256 * 600 >> 12 = 37. */
    gte_ctc2(0x155, JBI_GTE_ZSF3);
    gte_ctc2(0x100, JBI_GTE_ZSF4);
    gte_mtc2(0, JBI_GTE_SZ0);
    gte_mtc2(100, JBI_GTE_SZ1);
    gte_mtc2(200, JBI_GTE_SZ2);
    gte_mtc2(300, JBI_GTE_SZ3);
    gte_cmd(JBI_GTE_CMD_AVSZ3);
    CD(JBI_GTE_MAC0, 204600);
    CD(JBI_GTE_OTZ, 49);
    gte_cmd(JBI_GTE_CMD_AVSZ4);
    CD(JBI_GTE_MAC0, 153600);
    CD(JBI_GTE_OTZ, 37);
    /* OTZ saturation: 0x7fff * 0xffff * 3 >> 12 > 0xffff (flag 18); MAC0
       0x7fff * 0x2fffd = 0x17ffb8003 overflows 32 bits (flag 16). */
    gte_ctc2(0x7fff, JBI_GTE_ZSF3);
    gte_mtc2(0xffff, JBI_GTE_SZ1);
    gte_mtc2(0xffff, JBI_GTE_SZ2);
    gte_mtc2(0xffff, JBI_GTE_SZ3);
    gte_cmd(JBI_GTE_CMD_AVSZ3);
    CD(JBI_GTE_OTZ, 0xffff);
    CD(JBI_GTE_MAC0, 0x7ffb8003);
    CC(JBI_GTE_FLAG, 0x80050000);
}

static void test_mvmva(void)
{
    /* RT = 90 degrees about z, IR (100, 200, 300), sf 1, lm 1:
       MAC = (-200, 100, 300); IR1 saturates to 0 (flag 24). */
    jbi_gte_reset(&gte);
    gte_ctc2(xy(0, -0x1000), JBI_GTE_R11R12);
    gte_ctc2(xy(0, 0x1000), JBI_GTE_R13R21);
    gte_ctc2(xy(0, 0), JBI_GTE_R22R23);
    gte_ctc2(xy(0, 0), JBI_GTE_R31R32);
    gte_ctc2(0x1000, JBI_GTE_R33);
    gte_mtc2(100, JBI_GTE_IR1);
    gte_mtc2(200, JBI_GTE_IR2);
    gte_mtc2(300, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_MVMVA(1, 0, 3, 3, 1));
    CD(JBI_GTE_MAC1, -200);
    CD(JBI_GTE_MAC2, 100);
    CD(JBI_GTE_MAC3, 300);
    CD(JBI_GTE_IR1, 0);
    CD(JBI_GTE_IR2, 100);
    CD(JBI_GTE_IR3, 300);
    CC(JBI_GTE_FLAG, 0x81000000);

    /* LLM identity, V1 (1, 2, 3), TR (10, 20, 30), sf 1: (11, 22, 33). */
    jbi_gte_reset(&gte);
    identity(JBI_GTE_L11L12, 0x1000);
    gte_ctc2(10, JBI_GTE_TRX);
    gte_ctc2(20, JBI_GTE_TRY);
    gte_ctc2(30, JBI_GTE_TRZ);
    gte_mtc2(xy(1, 2), JBI_GTE_VXY1);
    gte_mtc2(3, JBI_GTE_VZ1);
    gte_cmd(JBI_GTE_CMD_MVMVA(1, 1, 1, 0, 0));
    CD(JBI_GTE_MAC1, 11);
    CD(JBI_GTE_MAC2, 22);
    CD(JBI_GTE_MAC3, 33);

    /* LCM identity, V2 (-1, 0, 5), BK (1, 1, 1), sf 0: MAC = BK << 12 +
       0x1000 * V = (0, 0x1000, 0x6000). */
    jbi_gte_reset(&gte);
    identity(JBI_GTE_LR1LR2, 0x1000);
    gte_ctc2(1, JBI_GTE_RBK);
    gte_ctc2(1, JBI_GTE_GBK);
    gte_ctc2(1, JBI_GTE_BBK);
    gte_mtc2(xy(-1, 0), JBI_GTE_VXY2);
    gte_mtc2(5, JBI_GTE_VZ2);
    gte_cmd(JBI_GTE_CMD_MVMVA(0, 2, 2, 1, 0));
    CD(JBI_GTE_MAC1, 0);
    CD(JBI_GTE_MAC2, 0x1000);
    CD(JBI_GTE_MAC3, 0x6000);

    /* cv = FC is bugged: the result drops FC and the first column.
       RT identity, V0 (5, 6, 7), FC (1000, 0, 0): MAC = (0, 6, 7). */
    jbi_gte_reset(&gte);
    identity(JBI_GTE_R11R12, 0x1000);
    gte_ctc2(1000, JBI_GTE_RFC);
    gte_mtc2(xy(5, 6), JBI_GTE_VXY0);
    gte_mtc2(7, JBI_GTE_VZ0);
    gte_cmd(JBI_GTE_CMD_MVMVA(1, 0, 0, 2, 0));
    CD(JBI_GTE_MAC1, 0);
    CD(JBI_GTE_MAC2, 6);
    CD(JBI_GTE_MAC3, 7);
    CC(JBI_GTE_FLAG, 0);

    /* mx = 3 reads rows (-R << 4, R << 4, IR0), (R13 x3), (R22 x3).
       R 1, IR0 0x1000, R13 0x100, R22 0x200, V0 (0x1000 x3), sf 1:
       (0x1000, 0x300, 0x600). */
    jbi_gte_reset(&gte);
    gte_mtc2(0x00000001, JBI_GTE_RGBC);
    gte_mtc2(0x1000, JBI_GTE_IR0);
    gte_ctc2(xy(0x100, 0), JBI_GTE_R13R21);
    gte_ctc2(xy(0x200, 0), JBI_GTE_R22R23);
    gte_mtc2(xy(0x1000, 0x1000), JBI_GTE_VXY0);
    gte_mtc2(0x1000, JBI_GTE_VZ0);
    gte_cmd(JBI_GTE_CMD_MVMVA(1, 3, 0, 3, 0));
    CD(JBI_GTE_MAC1, 0x1000);
    CD(JBI_GTE_MAC2, 0x300);
    CD(JBI_GTE_MAC3, 0x600);

    /* 44-bit overflow: TRX 0x7fffffff << 12 + 0x7fff * 0x7fff
       = 0x8003ffef001 > 2^43 - 1 (flag 30); MAC1 keeps the low 32 bits. */
    jbi_gte_reset(&gte);
    gte_ctc2(xy(0x7fff, 0), JBI_GTE_R11R12);
    gte_ctc2(0x7fffffff, JBI_GTE_TRX);
    gte_mtc2(xy(0x7fff, 0), JBI_GTE_VXY0);
    gte_mtc2(0, JBI_GTE_VZ0);
    gte_cmd(JBI_GTE_CMD_MVMVA(0, 0, 0, 0, 0));
    CD(JBI_GTE_MAC1, 0x3ffef001);
    CD(JBI_GTE_IR1, 0x7fff);
    CC(JBI_GTE_FLAG, 0xc1000000);
    /* The same with sf 1: the sum wraps to -0x7ffc0010fff, >> 12 =
       -0x7ffc0011 (flag 30, IR1 -0x8000 with flag 24). */
    gte_cmd(JBI_GTE_CMD_MVMVA(1, 0, 0, 0, 0));
    CD(JBI_GTE_MAC1, -0x7ffc0011);
    CD(JBI_GTE_IR1, -0x8000);
    CC(JBI_GTE_FLAG, 0xc1000000);
}

static void test_sqr_op(void)
{
    jbi_gte_reset(&gte);
    gte_mtc2(3, JBI_GTE_IR1);
    gte_mtc2(-4, JBI_GTE_IR2);
    gte_mtc2(0x7fff, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_SQR(0));
    CD(JBI_GTE_MAC1, 9);
    CD(JBI_GTE_MAC2, 16);
    CD(JBI_GTE_MAC3, 0x3fff0001);
    CD(JBI_GTE_IR3, 0x7fff);
    CC(JBI_GTE_FLAG, 0x00400000); /* IR3 saturation is not an error */
    gte_mtc2(0x1000, JBI_GTE_IR1);
    gte_mtc2(0x800, JBI_GTE_IR2);
    gte_mtc2(-0x1000, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_SQR(1));
    CD(JBI_GTE_MAC1, 0x1000);
    CD(JBI_GTE_MAC2, 0x400);
    CD(JBI_GTE_MAC3, 0x1000);
    CC(JBI_GTE_FLAG, 0);

    /* OP with diagonal (1, 2, 3), IR (4, 5, 6):
       (6*2 - 5*3, 4*3 - 6*1, 5*1 - 4*2) = (-3, 6, -3). */
    gte_ctc2(xy(1, 0), JBI_GTE_R11R12);
    gte_ctc2(xy(2, 0), JBI_GTE_R22R23);
    gte_ctc2(3, JBI_GTE_R33);
    gte_mtc2(4, JBI_GTE_IR1);
    gte_mtc2(5, JBI_GTE_IR2);
    gte_mtc2(6, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_OP(0));
    CD(JBI_GTE_MAC1, -3);
    CD(JBI_GTE_MAC2, 6);
    CD(JBI_GTE_MAC3, -3);
    CD(JBI_GTE_IR1, -3);
    /* lm saturates the negatives to 0. */
    gte_mtc2(4, JBI_GTE_IR1);
    gte_mtc2(5, JBI_GTE_IR2);
    gte_mtc2(6, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_OP(0) | JBI_GTE_LM);
    CD(JBI_GTE_IR1, 0);
    CD(JBI_GTE_IR2, 6);
    CD(JBI_GTE_IR3, 0);
    CC(JBI_GTE_FLAG, 0x81400000);
}

static void test_gpf_gpl(void)
{
    /* GPF sf 1: IR0 0x800 * (0x1000, 0x2000, -0x1000) >> 12
       = (0x800, 0x1000, -0x800); colours 0x80, 0x100 -> 0xff (flag 20),
       -0x80 -> 0 (flag 19); CODE from RGBC. */
    jbi_gte_reset(&gte);
    gte_mtc2(0x2c000000, JBI_GTE_RGBC);
    gte_mtc2(0x800, JBI_GTE_IR0);
    gte_mtc2(0x1000, JBI_GTE_IR1);
    gte_mtc2(0x2000, JBI_GTE_IR2);
    gte_mtc2(-0x1000, JBI_GTE_IR3);
    gte_mtc2(0x11, JBI_GTE_RGB1);
    gte_mtc2(0x22, JBI_GTE_RGB2);
    gte_cmd(JBI_GTE_CMD_GPF(1));
    CD(JBI_GTE_MAC1, 0x800);
    CD(JBI_GTE_MAC2, 0x1000);
    CD(JBI_GTE_MAC3, -0x800);
    CD(JBI_GTE_IR3, -0x800);
    CD(JBI_GTE_RGB0, 0x11);
    CD(JBI_GTE_RGB1, 0x22);
    CD(JBI_GTE_RGB2, 0x2c00ff80);
    CC(JBI_GTE_FLAG, 0x00180000);

    /* GPL sf 1: MAC (100, 200, 300), IR0 0x1000, IR (16, 32, 48):
       ((MAC << 12) + 0x1000 * IR) >> 12 = (116, 232, 348). */
    jbi_gte_reset(&gte);
    gte_mtc2(100, JBI_GTE_MAC1);
    gte_mtc2(200, JBI_GTE_MAC2);
    gte_mtc2(300, JBI_GTE_MAC3);
    gte_mtc2(0x1000, JBI_GTE_IR0);
    gte_mtc2(16, JBI_GTE_IR1);
    gte_mtc2(32, JBI_GTE_IR2);
    gte_mtc2(48, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_GPL(1));
    CD(JBI_GTE_MAC1, 116);
    CD(JBI_GTE_MAC2, 232);
    CD(JBI_GTE_MAC3, 348);
    CD(JBI_GTE_RGB2, 7 | 14 << 8 | 21 << 16);
    /* sf 0: MAC + IR0 * IR without shifts. */
    gte_mtc2(2, JBI_GTE_IR0);
    gte_cmd(JBI_GTE_CMD_GPL(0));
    CD(JBI_GTE_MAC1, 116 + 2 * 116);
}

static void test_depth_cue(void)
{
    /* DPCS: RGB (0x80, 0x40, 0), FC (0xff0, 0, 0x800), IR0 0x800:
       R: (0xff0000 - 0x800000) >> 12 = 0x7f0; 0x7f0 * 0x800 + 0x800000
          = 0xbf8000 -> MAC 0xbf8, colour 0xbf.
       G: -0x400 * 0x800 + 0x400000 = 0x200000 -> 0x200, colour 0x20.
       B: 0x800 * 0x800 = 0x400000 -> 0x400, colour 0x40. */
    jbi_gte_reset(&gte);
    gte_mtc2(0x30004080, JBI_GTE_RGBC);
    gte_ctc2(0xff0, JBI_GTE_RFC);
    gte_ctc2(0, JBI_GTE_GFC);
    gte_ctc2(0x800, JBI_GTE_BFC);
    gte_mtc2(0x800, JBI_GTE_IR0);
    gte_cmd(JBI_GTE_CMD_DPCS);
    CD(JBI_GTE_MAC1, 0xbf8);
    CD(JBI_GTE_MAC2, 0x200);
    CD(JBI_GTE_MAC3, 0x400);
    CD(JBI_GTE_IR1, 0xbf8);
    CD(JBI_GTE_RGB2, 0x304020bf);
    CC(JBI_GTE_FLAG, 0);

    /* DPCT with IR0 0 passes RGB0..2 through, CODE from RGBC. */
    gte_mtc2(0, JBI_GTE_IR0);
    gte_mtc2(0x01102030, JBI_GTE_RGB0);
    gte_mtc2(0x02405060, JBI_GTE_RGB1);
    gte_mtc2(0x03708090, JBI_GTE_RGB2);
    gte_cmd(JBI_GTE_CMD_DPCT);
    CD(JBI_GTE_RGB0, 0x30102030);
    CD(JBI_GTE_RGB1, 0x30405060);
    CD(JBI_GTE_RGB2, 0x30708090);

    /* INTPL: IR (0x100, 0x200, 0x300), IR0 0x1000, FC (0x1000, 0, 0):
       the result is FC: MAC (0x1000, 0, 0); colour 0x100 -> 0xff
       (flag 21). */
    jbi_gte_reset(&gte);
    gte_mtc2(0x5a000000, JBI_GTE_RGBC);
    gte_ctc2(0x1000, JBI_GTE_RFC);
    gte_mtc2(0x1000, JBI_GTE_IR0);
    gte_mtc2(0x100, JBI_GTE_IR1);
    gte_mtc2(0x200, JBI_GTE_IR2);
    gte_mtc2(0x300, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_INTPL);
    CD(JBI_GTE_MAC1, 0x1000);
    CD(JBI_GTE_MAC2, 0);
    CD(JBI_GTE_MAC3, 0);
    CD(JBI_GTE_RGB2, 0x5a0000ff);
    CC(JBI_GTE_FLAG, 0x00200000);

    /* DCPL: RGB 0x80 each, IR (0x1000, 0x800, 0), IR0 0:
       MAC = (0x80 * IR) << 4 >> 12 = (0x800, 0x400, 0). */
    jbi_gte_reset(&gte);
    gte_mtc2(0x00808080, JBI_GTE_RGBC);
    gte_mtc2(0x1000, JBI_GTE_IR1);
    gte_mtc2(0x800, JBI_GTE_IR2);
    gte_cmd(JBI_GTE_CMD_DCPL);
    CD(JBI_GTE_MAC1, 0x800);
    CD(JBI_GTE_MAC2, 0x400);
    CD(JBI_GTE_MAC3, 0);
    CD(JBI_GTE_RGB2, 0x00004080);
}

/* LLM picks V.x into row 0; LCM row 0 is 1.0 red; BK (0x10, 0x20, 0x30). */
static void setup_lighting(void)
{
    jbi_gte_reset(&gte);
    gte_ctc2(xy(0x1000, 0), JBI_GTE_L11L12);
    gte_ctc2(xy(0x1000, 0), JBI_GTE_LR1LR2);
    gte_ctc2(0x10, JBI_GTE_RBK);
    gte_ctc2(0x20, JBI_GTE_GBK);
    gte_ctc2(0x30, JBI_GTE_BBK);
    gte_mtc2(0x2000ff80 | 0x10 << 16, JBI_GTE_RGBC); /* R 0x80, G 0xff, B 0x10 */
    gte_mtc2(xy(0x800, 0), JBI_GTE_VXY0);
    gte_mtc2(0, JBI_GTE_VZ0);
}

static void test_lighting(void)
{
    /* NCS: light (0x800, 0, 0); colour matrix (0x810, 0x20, 0x30);
       FIFO (0x81, 0x02, 0x03). */
    setup_lighting();
    gte_cmd(JBI_GTE_CMD_NCS);
    CD(JBI_GTE_MAC1, 0x810);
    CD(JBI_GTE_MAC2, 0x20);
    CD(JBI_GTE_MAC3, 0x30);
    CD(JBI_GTE_RGB2, 0x20030281);

    /* NCCS: then (0x80, 0xff, 0x10) * IR << 4 >> 12 = (0x408, 0x1f, 0x3);
       FIFO (0x40, 0x01, 0x00). */
    setup_lighting();
    gte_cmd(JBI_GTE_CMD_NCCS);
    CD(JBI_GTE_MAC1, 0x408);
    CD(JBI_GTE_MAC2, 0x1f);
    CD(JBI_GTE_MAC3, 0x3);
    CD(JBI_GTE_RGB2, 0x20000140);

    /* NCDS with IR0 0x800 towards FC 0: R 0x408000 -> 0x204000 -> 0x204. */
    setup_lighting();
    gte_mtc2(0x800, JBI_GTE_IR0);
    gte_cmd(JBI_GTE_CMD_NCDS);
    CD(JBI_GTE_MAC1, 0x204);
    check("NCDS RGB2.r", gte_mfc2(JBI_GTE_RGB2) & 0xff, 0x20);

    /* CC from IR (0x800, 0, 0) gives NCCS's result. */
    setup_lighting();
    gte_mtc2(0x800, JBI_GTE_IR1);
    gte_mtc2(0, JBI_GTE_IR2);
    gte_mtc2(0, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_CC);
    CD(JBI_GTE_MAC1, 0x408);
    CD(JBI_GTE_RGB2, 0x20000140);

    /* CDP with IR0 0 is CC. */
    setup_lighting();
    gte_mtc2(0x800, JBI_GTE_IR1);
    gte_mtc2(0, JBI_GTE_IR2);
    gte_mtc2(0, JBI_GTE_IR3);
    gte_cmd(JBI_GTE_CMD_CDP);
    CD(JBI_GTE_MAC1, 0x408);

    /* NCT runs V0..V2: V1 = V2 = 0 give BK only, (0x01, 0x02, 0x03). */
    setup_lighting();
    gte_cmd(JBI_GTE_CMD_NCT);
    CD(JBI_GTE_RGB0, 0x20030281);
    CD(JBI_GTE_RGB1, 0x20030201);
    CD(JBI_GTE_RGB2, 0x20030201);
    /* NCCT and NCDT likewise end with V2's colour. */
    setup_lighting();
    gte_cmd(JBI_GTE_CMD_NCCT);
    CD(JBI_GTE_RGB0, 0x20000140);
    setup_lighting();
    gte_cmd(JBI_GTE_CMD_NCDT);
    CD(JBI_GTE_RGB0, 0x20000140);
}

int main(void)
{
    jbi_gte_init(&gte);
    test_registers();
    test_rtps();
    test_rtpt();
    test_nclip_avsz();
    test_mvmva();
    test_sqr_op();
    test_gpf_gpl();
    test_depth_cue();
    test_lighting();
    printf("gte_test: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
