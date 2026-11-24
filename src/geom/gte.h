#ifndef JBI_GEOM_GTE_H
#define JBI_GEOM_GTE_H

/*
 * The geometry transformation engine (COP2), emulated at register level.
 *
 * A jbi_gte is one register file: a libgte-style layer (SetRotMatrix,
 * MulMatrix, ...) and C versions of assembly routines translate each
 * mtc2/mfc2/ctc2/cfc2/lwc2/swc2 and cop2 command one for one onto the
 * functions below. GTEs are independent of each other; one is used from one
 * thread at a time. It allocates nothing: the caller owns the struct.
 *
 * Register semantics follow the hardware:
 *   - VZ0..2, IR0..3 and the 16-bit control registers (R33, L33, LB3, H,
 *     DQA, ZSF3, ZSF4) keep 16 bits and read back sign-extended; H reads
 *     sign-extended although the projection uses it unsigned.
 *   - OTZ and SZ0..3 keep 16 bits and read back zero-extended.
 *   - Writing SXYP pushes the screen XY FIFO; reading it returns SXY2.
 *   - Writing IRGB sets IR1..3 from a 5:5:5 colour; reading IRGB or ORGB
 *     converts IR1..3 back to 5:5:5 (IR >> 7, saturated to 0..31).
 *   - Writing LZCS sets LZCR to the count of leading bits equal to bit 31.
 *     ORGB and LZCR ignore writes.
 *   - FLAG keeps bits 12..30 on write; bit 31 is the OR of bits 13..18 and
 *     23..30. Every command clears FLAG first.
 */

#include <stdint.h>

#include "geom/precise.h"

/* Data registers (mtc2/mfc2, lwc2/swc2). */
enum {
    JBI_GTE_VXY0, JBI_GTE_VZ0, JBI_GTE_VXY1, JBI_GTE_VZ1, JBI_GTE_VXY2, JBI_GTE_VZ2,
    JBI_GTE_RGBC, JBI_GTE_OTZ,
    JBI_GTE_IR0, JBI_GTE_IR1, JBI_GTE_IR2, JBI_GTE_IR3,
    JBI_GTE_SXY0, JBI_GTE_SXY1, JBI_GTE_SXY2, JBI_GTE_SXYP,
    JBI_GTE_SZ0, JBI_GTE_SZ1, JBI_GTE_SZ2, JBI_GTE_SZ3,
    JBI_GTE_RGB0, JBI_GTE_RGB1, JBI_GTE_RGB2, JBI_GTE_RES1,
    JBI_GTE_MAC0, JBI_GTE_MAC1, JBI_GTE_MAC2, JBI_GTE_MAC3,
    JBI_GTE_IRGB, JBI_GTE_ORGB, JBI_GTE_LZCS, JBI_GTE_LZCR
};

/* Control registers (ctc2/cfc2). */
enum {
    JBI_GTE_R11R12, JBI_GTE_R13R21, JBI_GTE_R22R23, JBI_GTE_R31R32, JBI_GTE_R33,
    JBI_GTE_TRX, JBI_GTE_TRY, JBI_GTE_TRZ,
    JBI_GTE_L11L12, JBI_GTE_L13L21, JBI_GTE_L22L23, JBI_GTE_L31L32, JBI_GTE_L33,
    JBI_GTE_RBK, JBI_GTE_GBK, JBI_GTE_BBK,
    JBI_GTE_LR1LR2, JBI_GTE_LR3LG1, JBI_GTE_LG2LG3, JBI_GTE_LB1LB2, JBI_GTE_LB3,
    JBI_GTE_RFC, JBI_GTE_GFC, JBI_GTE_BFC,
    JBI_GTE_OFX, JBI_GTE_OFY, JBI_GTE_H, JBI_GTE_DQA, JBI_GTE_DQB, JBI_GTE_ZSF3,
    JBI_GTE_ZSF4,
    JBI_GTE_FLAG
};

/* Command word fields (the 25-bit immediate of a cop2 instruction). */
#define JBI_GTE_SF (1u << 19)          /* shift results right by 12 */
#define JBI_GTE_LM (1u << 10)          /* saturate IR1..3 to 0..0x7fff */
#define JBI_GTE_MX(n) ((unsigned)(n) << 17) /* MVMVA matrix: 0 RT, 1 LLM, 2 LCM */
#define JBI_GTE_V(n) ((unsigned)(n) << 15)  /* MVMVA vector: 0..2 V0..V2, 3 IR */
#define JBI_GTE_CV(n) ((unsigned)(n) << 13) /* MVMVA add: 0 TR, 1 BK, 2 FC, 3 none */

/* Command words as PsyQ's assembler macros encode them. Bits 20..24 are
   ignored by the hardware. */
#define JBI_GTE_CMD_RTPS 0x0180001u
#define JBI_GTE_CMD_RTPT 0x0280030u
#define JBI_GTE_CMD_NCLIP 0x1400006u
#define JBI_GTE_CMD_OP(sf) (0x170000Cu | ((sf) ? JBI_GTE_SF : 0))
#define JBI_GTE_CMD_DPCS 0x0780010u
#define JBI_GTE_CMD_INTPL 0x0980011u
#define JBI_GTE_CMD_MVMVA(sf, mx, v, cv, lm)                                                     \
    (0x0400012u | ((sf) ? JBI_GTE_SF : 0) | JBI_GTE_MX(mx) | JBI_GTE_V(v) | JBI_GTE_CV(cv) |     \
     ((lm) ? JBI_GTE_LM : 0))
#define JBI_GTE_CMD_NCDS 0x0E80413u
#define JBI_GTE_CMD_CDP 0x1280414u
#define JBI_GTE_CMD_NCDT 0x0F80416u
#define JBI_GTE_CMD_NCCS 0x108041Bu
#define JBI_GTE_CMD_CC 0x138041Cu
#define JBI_GTE_CMD_NCS 0x0C8041Eu
#define JBI_GTE_CMD_NCT 0x0D80420u
#define JBI_GTE_CMD_SQR(sf) (0x0A00428u | ((sf) ? JBI_GTE_SF : 0))
#define JBI_GTE_CMD_DCPL 0x0680029u
#define JBI_GTE_CMD_DPCT 0x0F8002Au
#define JBI_GTE_CMD_AVSZ3 0x158002Du
#define JBI_GTE_CMD_AVSZ4 0x168002Eu
#define JBI_GTE_CMD_GPF(sf) (0x190003Du | ((sf) ? JBI_GTE_SF : 0))
#define JBI_GTE_CMD_GPL(sf) (0x1A0003Eu | ((sf) ? JBI_GTE_SF : 0))
#define JBI_GTE_CMD_NCCT 0x118043Fu

/* FLAG bits. */
#define JBI_GTE_FLAG_ERROR 0x80000000u
#define JBI_GTE_FLAG_MAC_POS(i) (1u << (31 - (i))) /* i = 1..3 */
#define JBI_GTE_FLAG_MAC_NEG(i) (1u << (28 - (i)))
#define JBI_GTE_FLAG_IR(i) (1u << (25 - (i)))
#define JBI_GTE_FLAG_COLOR(i) (1u << (22 - (i)))
#define JBI_GTE_FLAG_SZ_OTZ (1u << 18)
#define JBI_GTE_FLAG_DIVIDE (1u << 17)
#define JBI_GTE_FLAG_MAC0_POS (1u << 16)
#define JBI_GTE_FLAG_MAC0_NEG (1u << 15)
#define JBI_GTE_FLAG_SX (1u << 14)
#define JBI_GTE_FLAG_SY (1u << 13)
#define JBI_GTE_FLAG_IR0 (1u << 12)

/*
 * The GTE. `data` and `ctrl` hold each register as mfc2/cfc2 would return
 * it, except the computed reads (SXYP, IRGB, ORGB); use the functions
 * rather than writing the arrays. The rest is set with jbi_gte_init and the
 * setters below.
 *
 * precise: when set and enabled (precise.h), RTPS/RTPT also record each
 *   projected vertex's unrounded screen position there. Recording only
 *   reads values the projection computed anyway; no register or flag
 *   changes. NULL records nothing.
 * trace: when set and recording is on, called before each command with
 *   the command word and the caller's return address, so a measuring tool
 *   can tell which routine projected what.
 */
typedef struct jbi_gte {
    uint32_t data[32];
    uint32_t ctrl[32];
    uint32_t flag; /* FLAG bits raised by the current command */
    jbi_precise *precise;
    void (*trace)(void *user, uint32_t cmd, void *caller);
    void *trace_user;
} jbi_gte;

/* Every register cleared, no precise table, no trace. */
void jbi_gte_init(jbi_gte *g);

/* Clears every register (the precise table and trace stay). */
void jbi_gte_reset(jbi_gte *g);

/* The table RTPS/RTPT record into, or NULL. It must outlive its use. */
void jbi_gte_set_precise(jbi_gte *g, jbi_precise *t);

/* mtc2/lwc2, mfc2/swc2, ctc2, cfc2. `reg` is 0..31. */
void jbi_gte_write_data(jbi_gte *g, int reg, uint32_t value);
uint32_t jbi_gte_read_data(const jbi_gte *g, int reg);
void jbi_gte_write_ctrl(jbi_gte *g, int reg, uint32_t value);
uint32_t jbi_gte_read_ctrl(const jbi_gte *g, int reg);

/* Executes a cop2 command (the instruction's low 25 bits). */
void jbi_gte_command(jbi_gte *g, uint32_t cmd);

/*
 * The projection's divide, as RTPS and RTPT do it: H / SZ in 1.16 fixed
 * point (0x10000 is 1.0) by the hardware's reciprocal, for h and sz 16
 * bits each (higher bits ignored). When h >= 2 * sz the hardware's result
 * saturates: 0x1FFFF, and *overflow is set to 1 (FLAG bit 17); otherwise
 * *overflow is 0. The friendly geometry path (src/geom/pipeline.c) projects
 * the vertices it makes when clipping with it.
 */
uint32_t jbi_gte_divide(uint32_t h, uint32_t sz, int *overflow);

#endif
