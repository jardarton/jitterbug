/*
 * probe_sim: the hardware probe's whole path without a console
 * (docs/probe.md, "What is proven"). The probe's own code
 * (tools/probe/probe/probe.c, unchanged) runs over a HAL whose GPU is a
 * jitterbug context, reached only through the GPU's ports as the console's
 * would be (jb_gp0 one word at a time, jb_gp1, jb_gpuread, jb_gpustat),
 * and whose serial line is a loopback to the host's side of the protocol
 * (tools/probe/host/jbh_session.c, unchanged), with bytes flipped or
 * dropped where asked. Then what came back is checked against jitterbug's
 * own player (tests/conformance/jbc.c) for the same list, played by the
 * same rule (FORMAT.md, "Playing a list": to pass N, then a window of two
 * passes): all 16 bits of VRAM, every word read, every check, every wait,
 * and the main loop's summary (the pass compared, the passes played,
 * whether the window changed VRAM and where, whether the pass compared was
 * already settled). Time is virtual: each look at the clock is a tenth of
 * a millisecond, and the host's side runs every few looks, as it would on
 * the other end of a line while the probe works.
 *
 *   probe_sim --passes N [--fault h2p|p2h:flip|drop:N]... [--lie-crc]
 *             [--bad-magic] [--gpu FAULT] [--wait-ms N] [--dma-direction-1]
 *             [--expect-error CODE [--expect-no-results]] [--expect-refused]
 *             [--expect-no-resend] [--expect-naks]
 *             [--save RESULT.jbpr] [--save-tampered KIND RESULT.jbpr]...
 *             (LIST.jbc | --late-loop)
 *   probe_sim --frames | --packets | --fuzz SEED ROUNDS
 *
 * --passes N: the main loop's pass compared (fixtures.txt's PASSES; 0 for
 * a list without a main loop), given to the probe in START and to
 * jitterbug's player alike.
 * --save writes the result file. --save-tampered writes it again changed,
 * for the runner's negative tests (tests/CMakeLists.txt, probe_runner_*):
 *   mask-bit   the mask bit of pixel (0, 0) flipped, the VRAM CRC-32
 *              recomputed: a sound file with one wrong pixel
 *   colour-bit the same with bit 0 (red) of pixel (0, 0)
 *   raw-bit    bit 0 of pixel (0, 0) flipped and the CRC-32 left: damaged
 *   passes     the pass compared and the passes played both one higher: a
 *              sound file played to another pass than the fixture's
 *   truncated  the file cut to half its length
 *   wait-timeout, dma-timeout, vsync-timeout
 *              a sound file that records one timeout of the list's first
 *              WAIT or DRAWSYNC (with its kept record), of a DMA transfer,
 *              of a vertical blank: complete, and not clean
 *   other-check  the first check's word offset one higher: a sound file
 *              whose check is not the list's
 *   console-check  the word the first check reads with the lowest bit of
 *              its mask flipped, in the reads and in every check of that
 *              read: a sound file whose console failed the list's check
 * --lie-crc sends a wrong CRC-32 in START; --bad-magic changes the list's
 * first byte. --dma-direction-1: each GP1(04h) the probe writes with DMA
 * direction 2 reaches the simulated GPU with direction 1, as from a list
 * that set direction 1; the run must then fail, whatever outcome is
 * expected (hal_contract). --gpu makes the simulated GPU fail:
 * never-idle:K (GPUSTAT bits 26 and 28 clear) or never-read:K (bit 27
 * clear, GPUREAD stale) once K + 1 read-back commands (GP0(C0h) and its
 * two parameters) have been written; dma-stall:MS (every DMA busy for MS
 * ms; 0: for ever); prep-busy:MS (bits 26 and 28 clear for MS
 * ms from the probe's GP1(00h) before the list) or fill-busy:MS (the same
 * from the last word of its fills): a GPU unready during the preparation
 * only, ready again while the list plays. --wait-ms sets START's
 * wait limit.
 * --expect-no-resend: the host must not have sent any frame again.
 * --expect-naks: the probe and the host must each have sent a NAK (for a
 * damaged frame). --expect-no-results: with --expect-error, the probe must
 * have failed before its summary (R_HEAD).
 * In every run the probe must keep hal.h's contract (hal_contract): no GP0
 * word to a GPU that shows not ready but a command's own parameters, one
 * hal_gp0_dma_stop for each DMA transfer that timed out, none other, and
 * every DMA transfer started with GPUSTAT's DMA direction 2.
 * --frames: frames made by hand sent to the probe, each with the outcome it
 * must have, and a host that floods the line with frames the probe must
 * ignore. --packets: lists with LOOP or END inside a GPU packet, which
 * the list reader must refuse. --fuzz: ROUNDS sessions made and damaged at
 * random from SEED (xorshift32); R_DONE only for the list START announced,
 * and a round now and then with a flooding host, which must not hold the
 * probe. --expect-error CODE: the probe must refuse the session with that
 * JBP_E_* code (and the host must fail it). --expect-refused: the session
 * completes, and the host must refuse the result because of the main loop
 * (unsettled in the window, or settled before the pass compared), with
 * the probe's summary equal to jitterbug's player's.
 * --late-loop plays, instead of a file, the list of the conformance
 * self-test whose loop draws nothing in pass 2 and draws in pass 3
 * (selftest.c, late_loop_change): the case where "until a pass leaves VRAM
 * unchanged" stops too early.
 * Exit status 0 when everything holds.
 *
 * Built again as probe_pty (JBP_SIM_PTY, POSIX with threads): the serial
 * line is a pseudo-terminal instead, the host's side is jbprobe itself
 * (`jbprobe send --tty`, tools/probe/host/jbprobe.c, on a thread of its
 * own), time is real, and the result is checked from the file it wrote:
 *
 *   probe_pty --save RESULT.jbpr LIST.jbc
 */
#define _XOPEN_SOURCE 600 /* mkstemp; the terminal (JBP_SIM_PTY) */
#include "hal.h"
#include "jbh_session.h"
#include "jbp_result.h"
#include "probe.h"

#include "jbc.h"

#include <jitterbug.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

#ifdef JBP_SIM_PTY
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

int jbprobe_main(int argc, char **argv); /* jbprobe.c, main renamed */
static int pty_fd = -1;
#endif

/* ------------------------------------------------------------------------ */
/* The simulated machine. */

typedef struct queue {
    uint8_t *b;
    size_t head, n, cap;
} queue;

enum { DIR_H2P, DIR_P2H };
enum { F_FLIP, F_DROP };

typedef struct fault {
    int dir, kind;
    unsigned long at; /* byte number in that direction, from 0 */
    int done;
} fault;

/* --gpu: what the simulated GPU does wrong, once K + 1 read-back commands
   have been written whole (GP0(C0h) and its two parameters), or
   (dma-stall) to every DMA. */
enum { G_NONE, G_NEVER_IDLE, G_NEVER_READ, G_DMA_STALL, G_PREP_BUSY, G_FILL_BUSY };

static struct {
    jb_context *jb;
    uint32_t clock;             /* ms, virtual */
    unsigned long tenths;       /* looks at the clock */
    queue to_probe, to_host;
    unsigned long count[2];
    fault faults[16];
    int nfaults;
    jbh_session host;
    unsigned long dma_no_direction, gp0_words, dma_words;
    int dma_direction_1;        /* --dma-direction-1 */
    int oom;
    int gpu_fault;
    unsigned long gpu_fault_at; /* never-idle, never-read: from this C0h on;
                                   dma-stall: ms (0: the DMA never ends) */
    unsigned long c0_words;     /* GP0(C0h) commands written whole (with their
                                   two parameters) */
    int c0_left;                /* parameters of a GP0(C0h) still to come */
    unsigned long gp0_unready;  /* GP0 words written while GPUSTAT, as the
                                   faults show it, said not ready (bits 26
                                   and 28), other than a command's own
                                   parameters */
    int gp0_params;             /* words of the probe's own command still to
                                   come (C0h: 2, a fill: 2) */
    uint32_t dma_until;
    int dma_running;
    /* hal.h's DMA contract: every start followed by busy until it
       reads 0 or by one stop; a stop only for a transfer still running. */
    unsigned long dma_starts, dma_stops, dma_stops_idle, dma_starts_running;
    unsigned long resets;       /* GP1(00h) words written */
    unsigned long gp0_after_reset; /* GP0 words since the first GP1(00h) */
    int prep_busy;              /* prep-busy, fill-busy: begun */
    uint32_t prep_from;         /* ... at this ms */
    void (*pump)(void);         /* the host side, while the probe runs */
} sim;

static void push(queue *q, uint8_t b)
{
    if (q->head > 0 && q->head == q->n) {
        q->head = q->n = 0;
    }
    if (q->n == q->cap) {
        size_t cap = q->cap ? q->cap * 2 : 1 << 16;
        uint8_t *more = realloc(q->b, cap);
        if (more == NULL) {
            sim.oom = 1;
            return;
        }
        q->b = more;
        q->cap = cap;
    }
    q->b[q->n++] = b;
}

/* A byte on the line, through the faults. */
static void line(int dir, uint8_t b)
{
    unsigned long at = sim.count[dir]++;
    int i;

    for (i = 0; i < sim.nfaults; ++i) {
        fault *f = &sim.faults[i];
        if (f->dir != dir || f->at != at)
            continue;
        f->done = 1;
        if (f->kind == F_DROP)
            return;
        b ^= 0x5a;
    }
    push(dir == DIR_H2P ? &sim.to_probe : &sim.to_host, b);
}

static void host_write(void *user, const uint8_t *p, size_t n)
{
    size_t i;

    (void)user;
    for (i = 0; i < n; ++i)
        line(DIR_H2P, p[i]);
}

/* The host runs whenever the probe waits for a byte, and every few ms of
   the probe's own time (hal_ms), as a host on the other end of a line
   would: so a probe that stays silent while busy is seen to be silent. */
static void pump_session(void)
{
    queue *q = &sim.to_host;

    if (q->n > q->head) {
        size_t n = q->n - q->head;
        q->head = q->n;
        jbh_input(&sim.host, q->b + q->n - n, n, sim.clock);
    }
    jbh_poll(&sim.host, sim.clock);
}

static void pump_host(void)
{
    if (sim.pump != NULL)
        sim.pump();
}

static int gpu_fault_on(int kind)
{
    if (kind == G_PREP_BUSY || kind == G_FILL_BUSY)
        return sim.gpu_fault == kind && sim.prep_busy && sim.clock - sim.prep_from < sim.gpu_fault_at;
    return sim.gpu_fault == kind && sim.c0_words > sim.gpu_fault_at;
}

static int gpu_ready_shown(void);

void hal_gp0(uint32_t word)
{
    ++sim.gp0_words;
    /* A word that is not a parameter of a command begun while the GPU was
       ready must find it ready: the probe waits for GPUSTAT bits 26 and 28
       before its own commands (docs/probe.md, "Preparation, restore and
       read-back"). */
    if (sim.gp0_params > 0)
        --sim.gp0_params;
    else {
        if (!gpu_ready_shown())
            ++sim.gp0_unready;
        if (word == 0xc0000000u || word == 0x02000000u)
            sim.gp0_params = 2;
    }
    /* fill-busy: from the probe's sixth GP0 word after its GP1(00h), the
       last of its two fills before the list. */
    if (sim.resets && ++sim.gp0_after_reset == 6 && sim.gpu_fault == G_FILL_BUSY && !sim.prep_busy) {
        sim.prep_busy = 1;
        sim.prep_from = sim.clock;
    }
    /* The probe's read-back command, counted once its two parameters are
       written (the lists played here have no word 0xc0000000;
       probe_sim_readback_* would fail if one did). */
    if (sim.c0_left > 0 && --sim.c0_left == 0)
        ++sim.c0_words;
    else if (word == 0xc0000000u)
        sim.c0_left = 2;
    jb_gp0(sim.jb, &word, 1);
}

static unsigned long gp1_words;

void hal_gp1(uint32_t word)
{
    ++gp1_words;
    /* prep-busy: from the probe's GP1(00h) before the list (the first). */
    if (word == 0 && sim.resets++ == 0 && sim.gpu_fault == G_PREP_BUSY) {
        sim.prep_busy = 1;
        sim.prep_from = sim.clock;
    }
    if (sim.dma_direction_1 && word >> 24 == 0x04u && (word & 3u) == 2u)
        word ^= 3u; /* direction 1 */
    jb_gp1(sim.jb, word);
}

uint32_t hal_gpuread(void)
{
    uint32_t v = jb_gpuread(sim.jb);

    /* A GPU that never started the read-back: GPUREAD holds whatever it
       held, not VRAM. */
    return gpu_fault_on(G_NEVER_READ) ? 0x5a5a5a5au : v;
}

static int gpu_ready_shown(void)
{
    return !(gpu_fault_on(G_NEVER_IDLE) || gpu_fault_on(G_PREP_BUSY) || gpu_fault_on(G_FILL_BUSY));
}

uint32_t hal_gpustat(void)
{
    uint32_t v = jb_gpustat(sim.jb);

    if (gpu_fault_on(G_NEVER_IDLE) || gpu_fault_on(G_PREP_BUSY) || gpu_fault_on(G_FILL_BUSY))
        v &= ~0x14000000u; /* not ready for a command, FIFO not empty */
    if (gpu_fault_on(G_NEVER_READ))
        v &= ~0x08000000u; /* VRAM never ready to send */
    return v;
}

uint32_t hal_ms(void);

void hal_gp0_dma_start(const uint32_t *words, uint32_t n)
{
    /* psx-spx documents DMA to GP0 with DMA direction 2 (GPUSTAT bits
       29-30, "GPU-related DMA Channels"); this simulation forwards the
       words whatever the direction, and counts a transfer started with
       another, which then fails the test: no list played here may rely on
       what a console does with another direction. */
    if ((jb_gpustat(sim.jb) >> 29 & 3u) != 2u)
        ++sim.dma_no_direction;
    if (sim.dma_running)
        ++sim.dma_starts_running;
    ++sim.dma_starts;
    sim.dma_words += n;
    jb_gp0(sim.jb, words, n);
    sim.dma_running = 1;
    sim.dma_until = sim.clock + (uint32_t)sim.gpu_fault_at;
}

int hal_gp0_dma_busy(void)
{
    if (!sim.dma_running)
        return 0;
    if (sim.gpu_fault == G_DMA_STALL &&
        (sim.gpu_fault_at == 0 || (int32_t)(sim.clock - sim.dma_until) < 0))
        return 1;
    sim.dma_running = 0;
    return 0;
}

void hal_gp0_dma_stop(void)
{
    ++sim.dma_stops;
    if (!sim.dma_running)
        ++sim.dma_stops_idle;
    sim.dma_running = 0;
}

void hal_vblank_clear(void)
{
}

int hal_vblank_seen(void)
{
    return 1;
}

#ifdef JBP_SIM_PTY
uint32_t hal_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u);
}

int hal_serial_get(uint32_t timeout_ms)
{
    static uint8_t buf[4096];
    static size_t have, at;
    struct pollfd pf;
    ssize_t got;

    if (at < have)
        return buf[at++];
    pf.fd = pty_fd;
    pf.events = POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, (int)timeout_ms) <= 0)
        return -1;
    got = read(pty_fd, buf, sizeof buf);
    if (got <= 0)
        return -1;
    have = (size_t)got;
    at = 1;
    sim.count[DIR_H2P] += (unsigned long)got;
    return buf[0];
}

int hal_serial_put(uint8_t byte)
{
    ++sim.count[DIR_P2H];
    while (write(pty_fd, &byte, 1) != 1) {
        struct pollfd pf;
        pf.fd = pty_fd;
        pf.events = POLLOUT;
        pf.revents = 0;
        if (poll(&pf, 1, 1000) <= 0)
            return -1;
    }
    return 0;
}
#else
uint32_t hal_ms(void)
{
    /* A tenth of a millisecond a look: about a byte's time on the line at
       115200 baud, so that reading a 4 KB frame takes about as long as on
       a console. */
    if (++sim.tenths % 10 == 0)
        ++sim.clock;
    if (sim.tenths % 8 == 0)
        pump_host();
    return sim.clock;
}

int hal_serial_get(uint32_t timeout_ms)
{
    uint32_t deadline = sim.clock + timeout_ms;
    queue *q = &sim.to_probe;

    for (;;) {
        if (q->n > q->head)
            return q->b[q->head++];
        pump_host();
        if (q->n > q->head)
            continue;
        if ((int32_t)(hal_ms() - deadline) >= 0)
            return -1;
    }
}

int hal_serial_put(uint8_t byte)
{
    line(DIR_P2H, byte);
    return 0;
}
#endif

uint32_t hal_serial_errors(void)
{
    return 0;
}

void hal_identify(hal_ident *id)
{
    memset(id, 0, sizeof *id);
    id->platform = 2;
    snprintf(id->maker, sizeof id->maker, "jitterbug host simulation");
    snprintf(id->version, sizeof id->version, "jitterbug %s", jb_version_string());
}

void hal_show(int state)
{
    (void)state;
}

/* ------------------------------------------------------------------------ */

#ifdef JBP_SIM_PTY
typedef struct host_thread {
    char *argv[10];
    int rc;
} host_thread;

static void *run_jbprobe(void *p)
{
    host_thread *h = p;

    h->rc = jbprobe_main(9, h->argv);
    return NULL;
}
#endif

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long len;

    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        if (f)
            fclose(f);
        return NULL;
    }
    b = malloc((size_t)len + 1);
    if (b == NULL || fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return b;
}

static int usage(void)
{
    fprintf(stderr, "usage: probe_sim --passes N [--fault h2p|p2h:flip|drop:N]... [--lie-crc] "
                    "[--bad-magic] [--gpu never-idle|never-read|dma-stall|prep-busy|fill-busy:N] [--wait-ms N] "
                    "[--dma-direction-1] "
                    "[--expect-error CODE [--expect-no-results]] [--expect-refused] [--expect-no-resend] "
                    "[--expect-naks] [--save R.jbpr] "
                    "[--save-tampered mask-bit|colour-bit|raw-bit|passes|truncated|wait-timeout|dma-timeout|"
                    "vsync-timeout|other-check|console-check R.jbpr]... "
                    "(LIST.jbc | --late-loop)\n"
                    "       probe_sim --frames | --packets\n"
                    "       probe_sim --fuzz SEED ROUNDS\n");
    return 2;
}

static size_t put_string(uint32_t *w, const char *str)
{
    size_t len = strlen(str), i, n = 1 + (len + 3) / 4;

    memset(w, 0, n * sizeof *w);
    w[0] = (uint32_t)len;
    for (i = 0; i < len; ++i)
        w[1 + i / 4] |= (uint32_t)(unsigned char)str[i] << (8 * (i % 4));
    return n;
}

/* The self-test's late-loop list (selftest.c, late_loop_change), as file
   bytes: pass 1 sets an empty drawing area; the loop draws a 16 x 16 tile
   at (0, 0) and then makes the drawing area all of VRAM, so the tile is
   clipped away in pass 2 and drawn in pass 3. */
static uint8_t *late_loop(size_t *bytes)
{
    uint32_t w[64];
    size_t n = 0, i;
    uint8_t *b;

    memcpy(w, JBC_MAGIC, 8);
    n = 2;
    w[n++] = JBC_VERSION;
    w[n++] = 0;
    n += put_string(w + n, "gpu/late-loop");
    n += put_string(w + n, "tests/probe/sim.c, after selftest.c");
    w[3] = (uint32_t)(n * 4);
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 2);
    w[n++] = 0xe3000000u | 10u | 10u << 10;
    w[n++] = 0xe4000000u | 9u | 9u << 10;
    w[n++] = (uint32_t)JBC_OP(JBC_VSYNC, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_LOOP, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_GP0, 4);
    w[n++] = 0x60ffffffu;
    w[n++] = 0x00000000u;
    w[n++] = 0x00100010u;
    w[n++] = 0xe4000000u | 1023u | 511u << 10;
    w[n++] = (uint32_t)JBC_OP(JBC_VSYNC, 0);
    w[n++] = (uint32_t)JBC_OP(JBC_END, 0);
    b = malloc(n * 4);
    if (b == NULL)
        return NULL;
    for (i = 0; i < n; ++i)
        jbp_put32(b + 4 * i, w[i]);
    *bytes = n * 4;
    return b;
}

/* The kinds of save_tampered that leave the file sound but not clean, or
   change its checks. 1 when kind is one, -1 when it cannot be
   made from res (nothing to change), else 0. */
static int tamper_unclean(jbp_result *res, const jbc_list *l, const char *kind, uint32_t *wait)
{
    size_t i;
    uint32_t k;

    if (strcmp(kind, "wait-timeout") == 0) {
        for (i = l->first; JBC_OP_CODE(l->w[i]) != JBC_END; i += 1 + JBC_OP_COUNT(l->w[i]))
            if (JBC_OP_CODE(l->w[i]) == JBC_WAIT || JBC_OP_CODE(l->w[i]) == JBC_DRAWSYNC)
                break;
        if (JBC_OP_CODE(l->w[i]) == JBC_END || res->nwaits != 0 || res->head[JBP_H_WAITS] == 0)
            return -1;
        wait[0] = (uint32_t)i;
        wait[1] = JBC_OP_CODE(l->w[i]) == JBC_WAIT ? l->w[i + 1] : 0x10000000u;
        wait[2] = JBC_OP_CODE(l->w[i]) == JBC_WAIT ? l->w[i + 2] : 0x10000000u;
        wait[3] = 0;
        wait[4] = 1;
        res->waits = wait;
        res->nwaits = 1;
        res->head[JBP_H_WAITS_FAILED] = 1;
        return 1;
    }
    if (strcmp(kind, "dma-timeout") == 0) {
        if (res->head[JBP_H_DMA_OPS] == 0)
            return -1;
        res->head[JBP_H_DMA_TIMEOUTS] = 1;
        return 1;
    }
    if (strcmp(kind, "vsync-timeout") == 0) {
        if (res->head[JBP_H_VSYNCS] == 0)
            return -1;
        res->head[JBP_H_VSYNC_TIMEOUTS] = 1;
        return 1;
    }
    if (strcmp(kind, "other-check") == 0) {
        if (res->nexpects == 0)
            return -1;
        ++res->expects[0];
        return 1;
    }
    if (strcmp(kind, "console-check") == 0) {
        uint32_t *e = res->expects, read, bit, failed = 0;
        if (res->nexpects == 0 || e[2] == 0)
            return -1;
        read = e[1];
        bit = e[2] & (0u - e[2]);
        res->reads[read] ^= bit;
        for (k = 0; k < res->nexpects; ++k) {
            uint32_t *q = res->expects + (size_t)k * JBP_EXPECT_WORDS;
            if (q[1] == read)
                q[4] = res->reads[read];
            failed += (q[4] & q[2]) != q[3];
        }
        res->head[JBP_H_EXPECTS_FAILED] = failed;
        return 1;
    }
    return 0;
}

/* --save-tampered: res changed as KIND says, written to path. 0, or -1. */
static int save_tampered(jbp_result *res, const jbc_list *l, const char *kind, const char *path)
{
    char err[256];
    uint16_t v0 = res->vram[0];
    uint32_t crc = res->done_crc, passes = res->head[JBP_H_PASSES], played = res->head[JBP_H_PLAYED];
    uint32_t at = res->head[JBP_H_CRC_AT], last = res->head[JBP_H_CRC_LAST];
    uint32_t head[JBP_H_WORDS], wait[JBP_WAITFAIL_WORDS], *waits = res->waits, nwaits = res->nwaits;
    uint32_t *reads = NULL, *expects = NULL;
    int r = 0, unclean;

    memcpy(head, res->head, sizeof head);
    if (res->nreads)
        reads = malloc(res->nreads * sizeof *reads);
    if (res->nexpects)
        expects = malloc((size_t)res->nexpects * JBP_EXPECT_WORDS * sizeof *expects);
    if ((res->nreads && reads == NULL) || (res->nexpects && expects == NULL)) {
        free(reads);
        free(expects);
        return -1;
    }
    if (reads)
        memcpy(reads, res->reads, res->nreads * sizeof *reads);
    if (expects)
        memcpy(expects, res->expects, (size_t)res->nexpects * JBP_EXPECT_WORDS * sizeof *expects);
    unclean = tamper_unclean(res, l, kind, wait);

    if (strcmp(kind, "mask-bit") == 0 || strcmp(kind, "colour-bit") == 0 ||
        strcmp(kind, "raw-bit") == 0) {
        res->vram[0] ^= kind[0] == 'm' ? 0x8000u : 0x0001u;
        if (kind[0] != 'r') {
            /* A sound file: the probe's CRC-32s are those of this VRAM. */
            res->done_crc = jbp_result_vram_crc(res);
            if (res->head[JBP_H_LOOP])
                res->head[JBP_H_CRC_AT] = res->head[JBP_H_CRC_LAST] = res->done_crc;
        }
    } else if (strcmp(kind, "passes") == 0) {
        ++res->head[JBP_H_PASSES];
        ++res->head[JBP_H_PLAYED];
    } else if (unclean < 0) {
        printf("FAIL: --save-tampered %s: %s has nothing to change\n", kind, l->name);
        r = -1;
    } else if (unclean == 0 && strcmp(kind, "truncated") != 0) {
        printf("FAIL: --save-tampered %s: no such change\n", kind);
        r = -1;
    }
    if (r == 0 && jbp_result_save(res, path, err, sizeof err) != 0) {
        printf("FAIL: %s\n", err);
        r = -1;
    } else if (r == 0 && strcmp(kind, "truncated") == 0) {
        size_t n = 0;
        uint8_t *b = read_file(path, &n);
        FILE *f = b ? fopen(path, "wb") : NULL;
        if (f == NULL || fwrite(b, 1, n / 2, f) != n / 2)
            r = -1;
        if (f != NULL && fclose(f) != 0)
            r = -1;
        free(b);
        if (r != 0)
            printf("FAIL: cannot truncate %s\n", path);
    }
    if (r == 0)
        printf("wrote %s (%s)\n", path, kind);
    res->vram[0] = v0;
    res->done_crc = crc;
    res->head[JBP_H_PASSES] = passes;
    res->head[JBP_H_PLAYED] = played;
    res->head[JBP_H_CRC_AT] = at;
    res->head[JBP_H_CRC_LAST] = last;
    memcpy(res->head, head, sizeof head);
    res->waits = waits;
    res->nwaits = nwaits;
    if (reads)
        memcpy(res->reads, reads, res->nreads * sizeof *reads);
    if (expects)
        memcpy(res->expects, expects, (size_t)res->nexpects * JBP_EXPECT_WORDS * sizeof *expects);
    free(reads);
    free(expects);
    return r;
}

/* What the probe owes the HAL whatever the outcome (hal.h): no GP0 word
   to a GPU that shows not ready, other than the parameters of a command
   begun when it was ready; every DMA transfer counted in the summary
   started once, none while another ran, and stopped exactly when it timed
   out: one stop for each transfer that never ended, none for one that did,
   none with no transfer running; every transfer started with
   GPUSTAT's DMA direction 2. 1 when one was broken (printed). */
static int hal_contract(const jbp_result *res)
{
    int bad = 0;
    unsigned long timed_out = res->have_head ? res->head[JBP_H_DMA_TIMEOUTS] : 0;

    if (sim.gp0_unready) {
        printf("FAIL: %lu GP0 words written while GPUSTAT showed the GPU not ready\n", sim.gp0_unready);
        bad = 1;
    }
    if (res->have_head && sim.dma_starts != res->head[JBP_H_DMA_OPS]) {
        printf("FAIL: %lu DMA transfers started, the summary counts %u\n", sim.dma_starts,
               (unsigned)res->head[JBP_H_DMA_OPS]);
        bad = 1;
    }
    if (sim.dma_starts_running || sim.dma_stops_idle || sim.dma_stops != timed_out ||
        (sim.gpu_fault == G_DMA_STALL && sim.gpu_fault_at == 0 ? sim.dma_stops != sim.dma_starts
                                                                : sim.dma_stops != 0)) {
        printf("FAIL: DMA: %lu started (%lu while one ran), %lu stopped (%lu with none running), %lu "
               "timed out by the summary; a transfer that never ends must be stopped once, one that "
               "ends never\n",
               sim.dma_starts, sim.dma_starts_running, sim.dma_stops, sim.dma_stops_idle, timed_out);
        bad = 1;
    } else if (sim.dma_starts)
        printf("DMA: %lu transfers started, %lu stopped, as each one's end required\n", sim.dma_starts,
               sim.dma_stops);
    if (sim.dma_no_direction) {
        printf("FAIL: %lu DMA transfers with GPUSTAT's DMA direction not 2 (what a console does then "
               "is not known here)\n",
               sim.dma_no_direction);
        bad = 1;
    }
    return bad;
}

/* ------------------------------------------------------------------------ */
/* The result file's round trip: every field a result file keeps,
   compared. */

/* The first field in which a and b differ, or NULL: every field
   jbp_result_save writes and jbp_result_load reads (the flags, the
   identification, the summary, the reads, checks and kept waits with their
   counts, the rows received and all of VRAM, DONE's CRC-32 and counts, the
   error, the host's record). */
static const char *result_diff(const jbp_result *a, const jbp_result *b)
{
    if (a->have_hello != b->have_hello)
        return "have_hello";
    if (a->have_ident != b->have_ident)
        return "have_ident";
    if (a->have_head != b->have_head)
        return "have_head";
    if (a->have_done != b->have_done)
        return "have_done";
    if (a->have_error != b->have_error)
        return "have_error";
    if (a->have_host != b->have_host)
        return "have_host";
    if (memcmp(a->hello, b->hello, sizeof a->hello) != 0)
        return "the identification";
    if (memcmp(a->head, b->head, sizeof a->head) != 0)
        return "the summary";
    if (a->nreads != b->nreads)
        return "the count of reads";
    if (a->nreads && memcmp(a->reads, b->reads, a->nreads * sizeof *a->reads) != 0)
        return "a read";
    if (a->nexpects != b->nexpects)
        return "the count of checks";
    if (a->nexpects &&
        memcmp(a->expects, b->expects, (size_t)a->nexpects * JBP_EXPECT_WORDS * sizeof *a->expects) != 0)
        return "a check";
    if (a->nwaits != b->nwaits)
        return "the count of kept waits";
    if (a->nwaits && memcmp(a->waits, b->waits, (size_t)a->nwaits * JBP_WAITFAIL_WORDS * sizeof *a->waits) != 0)
        return "a kept wait";
    if (a->next_row != b->next_row)
        return "the rows received";
    if (memcmp(a->vram, b->vram, (size_t)JBP_VRAM_W * JBP_VRAM_H * sizeof *a->vram) != 0)
        return "VRAM";
    if (a->done_crc != b->done_crc)
        return "DONE's CRC-32";
    if (a->done_frames != b->done_frames || a->done_resent != b->done_resent ||
        a->done_rx_bad != b->done_rx_bad || a->done_sio_errors != b->done_sio_errors)
        return "DONE's counts";
    if (memcmp(a->error, b->error, sizeof a->error) != 0)
        return "the error";
    if (strcmp(a->list_name, b->list_name) != 0)
        return "the list's name";
    if (a->list_bytes != b->list_bytes || a->list_crc != b->list_crc)
        return "the list's length or CRC-32";
    if (strcmp(a->note, b->note) != 0)
        return "the host's note";
    return NULL;
}

/* That result_diff sees each field: every field of b changed in turn
   (one bit, or a count one higher), and put back. The number of changes
   made, or -1 (printed) when one went unseen. */
static long diff_self_check(const jbp_result *a, jbp_result *b)
{
    long made = 0;
    int k;

#define SEEN(what, change, undo)                                                                        \
    do {                                                                                                \
        change;                                                                                         \
        ++made;                                                                                         \
        if (result_diff(a, b) == NULL) {                                                                \
            printf("FAIL: the round-trip comparison does not see %s changed\n", what);                 \
            undo;                                                                                       \
            return -1;                                                                                  \
        }                                                                                               \
        undo;                                                                                           \
    } while (0)
    SEEN("have_hello", b->have_hello ^= 1, b->have_hello ^= 1);
    SEEN("have_ident", b->have_ident ^= 1, b->have_ident ^= 1);
    SEEN("have_head", b->have_head ^= 1, b->have_head ^= 1);
    SEEN("have_done", b->have_done ^= 1, b->have_done ^= 1);
    SEEN("have_error", b->have_error ^= 1, b->have_error ^= 1);
    SEEN("have_host", b->have_host ^= 1, b->have_host ^= 1);
    for (k = 0; k < JBP_HELLO_BYTES; k += JBP_HELLO_BYTES - 1)
        SEEN("the identification", b->hello[k] ^= 1, b->hello[k] ^= 1);
    for (k = 0; k < JBP_H_WORDS; ++k)
        SEEN("a word of the summary", b->head[k] ^= 1, b->head[k] ^= 1);
    SEEN("the count of reads", ++b->nreads, --b->nreads);
    if (b->nreads)
        SEEN("a read", b->reads[b->nreads - 1] ^= 1, b->reads[b->nreads - 1] ^= 1);
    SEEN("the count of checks", ++b->nexpects, --b->nexpects);
    for (k = 0; k < JBP_EXPECT_WORDS && b->nexpects; ++k)
        SEEN("a word of a check", b->expects[(size_t)(b->nexpects - 1) * JBP_EXPECT_WORDS + k] ^= 1,
             b->expects[(size_t)(b->nexpects - 1) * JBP_EXPECT_WORDS + k] ^= 1);
    SEEN("the count of kept waits", ++b->nwaits, --b->nwaits);
    for (k = 0; k < JBP_WAITFAIL_WORDS && b->nwaits; ++k)
        SEEN("a word of a kept wait", b->waits[(size_t)(b->nwaits - 1) * JBP_WAITFAIL_WORDS + k] ^= 1,
             b->waits[(size_t)(b->nwaits - 1) * JBP_WAITFAIL_WORDS + k] ^= 1);
    SEEN("the rows received", ++b->next_row, --b->next_row);
    SEEN("VRAM's first pixel", b->vram[0] ^= 0x8000u, b->vram[0] ^= 0x8000u);
    SEEN("VRAM's last pixel", b->vram[JBP_VRAM_W * JBP_VRAM_H - 1] ^= 1,
         b->vram[JBP_VRAM_W * JBP_VRAM_H - 1] ^= 1);
    SEEN("DONE's CRC-32", b->done_crc ^= 1, b->done_crc ^= 1);
    SEEN("DONE's frames", b->done_frames ^= 1, b->done_frames ^= 1);
    SEEN("DONE's frames sent again", b->done_resent ^= 1, b->done_resent ^= 1);
    SEEN("DONE's damaged frames", b->done_rx_bad ^= 1, b->done_rx_bad ^= 1);
    SEEN("DONE's serial errors", b->done_sio_errors ^= 1, b->done_sio_errors ^= 1);
    for (k = 0; k < 3; ++k)
        SEEN("a word of the error", b->error[k] ^= 1, b->error[k] ^= 1);
    SEEN("the list's name", b->list_name[0] ^= 1, b->list_name[0] ^= 1);
    SEEN("the list's length", b->list_bytes ^= 1, b->list_bytes ^= 1);
    SEEN("the list's CRC-32", b->list_crc ^= 1, b->list_crc ^= 1);
    SEEN("the host's note", b->note[0] ^= 1, b->note[0] ^= 1);
#undef SEEN
    return made;
}

/* The file's records, altered one at a time (bit 0 of the first, middle
   and last byte of its payload flipped, and the record left out), for
   every record but the VRAM rows between the first and the last: each
   altered file must be refused or read back different from res. The
   number of alterations, and of kinds of record, or -1 (printed). */
static long file_self_check(const jbp_result *res, const uint8_t *b, size_t n, int *kinds)
{
    uint8_t *m = malloc(n);
    size_t at = 12, first_vram = 0, last_vram = 0;
    long made = 0;
    unsigned seen_types[32];
    int nseen = 0, j;

    if (m == NULL)
        return -1;
    /* The last VRAM record's place, so that only it and the first are
       altered (each alteration reads 1 MB back). */
    for (at = 12; at + 8 <= n; at += 8 + ((jbp_get32(b + at + 4) + 3u) & ~3u))
        if (jbp_get32(b + at) == JBP_R_VRAM) {
            if (first_vram == 0)
                first_vram = at;
            last_vram = at;
        }
    for (at = 12; at + 8 <= n;) {
        unsigned type = jbp_get32(b + at);
        size_t len = jbp_get32(b + at + 4), next = at + 8 + ((len + 3u) & ~(size_t)3u);
        int how;
        if (type == JBP_R_VRAM && at != first_vram && at != last_vram) {
            at = next;
            continue;
        }
        for (j = 0; j < nseen && seen_types[j] != type; ++j)
            ;
        if (j == nseen && nseen < 32)
            seen_types[nseen++] = type;
        for (how = 0; how < 4; ++how) {
            size_t mn = n;
            jbp_result alt;
            char err[256];
            const char *d = NULL;
            int parsed;
            memcpy(m, b, n);
            if (how < 3) {
                size_t off = how == 0 ? 0 : how == 1 ? len / 2 : len - 1;
                if (len == 0)
                    continue;
                m[at + 8 + off] ^= 1;
            } else {
                memmove(m + at, m + next, n - next);
                mn = n - (next - at);
            }
            ++made;
            parsed = jbp_result_parse(&alt, m, mn, err, sizeof err) == 0;
            if (parsed) {
                d = result_diff(res, &alt);
                jbp_result_free(&alt);
            }
            if (parsed && d == NULL) {
                printf("FAIL: record type 0x%02x at byte %lu %s, and the file reads back the same\n", type,
                       (unsigned long)at,
                       how == 0   ? "with its first byte altered"
                       : how == 1 ? "with its middle byte altered"
                       : how == 2 ? "with its last byte altered"
                                  : "left out");
                free(m);
                return -1;
            }
        }
        at = next;
    }
    free(m);
    *kinds = nseen;
    return made;
}

/* Creates a new, empty file of its own (exclusively: never one that
   exists) named name with its last six characters, XXXXXX, replaced: 0,
   or -1 with nothing made. */
static int private_file(char *name)
{
    int fd;

#ifdef _WIN32
    if (_mktemp_s(name, strlen(name) + 1) != 0 ||
        _sopen_s(&fd, name, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
        return -1;
    _close(fd);
#else
    if ((fd = mkstemp(name)) < 0)
        return -1;
    close(fd);
#endif
    return 0;
}

/* --save: res written to path, then read back and compared field by field
   (result_diff), saved again from what was read and compared byte for byte
   with the file, and both comparisons shown to notice a change of every
   field and of every kind of record the file has. The second save goes to
   a file of its own, made beside path by private_file under a short name
   and removed after: no existing file is written over, and
   the length of path's own name does not matter. 0, or -1 (printed). */
static int round_trip(const jbp_result *res, const char *path)
{
    jbp_result back;
    char err[300], why[256], *again = NULL;
    uint8_t *b1 = NULL, *b2 = NULL;
    size_t n1 = 0, n2 = 0;
    const char *d;
    long fields, alts;
    int kinds = 0, r = -1;

    if (jbp_result_save(res, path, err, sizeof err) != 0 || jbp_result_load(&back, path, err, sizeof err) != 0) {
        printf("FAIL: %s\n", err);
        return -1;
    }
    {
        /* A short name of its own in path's directory: appending to path's
           name could pass the file system's limit on a name. The
           directory is whatever precedes the name, of any length. */
        static const char tmpl[] = "jbprXXXXXX";
        const char *slash = strrchr(path, '/');
        size_t dir;
#ifdef _WIN32
        const char *back_slash = strrchr(path, '\\');
        if (back_slash != NULL && (slash == NULL || back_slash > slash))
            slash = back_slash;
        /* D:name is in drive D's current directory, not this process's:
           the drive stays in front. */
        if (slash == NULL && path[0] != 0 && path[1] == ':')
            slash = path + 1;
#endif
        dir = slash != NULL ? (size_t)(slash - path) + 1 : 0;
        again = malloc(dir + sizeof tmpl);
        if (again == NULL) {
            printf("FAIL: out of memory\n");
            jbp_result_free(&back);
            return -1;
        }
        memcpy(again, path, dir);
        memcpy(again + dir, tmpl, sizeof tmpl);
    }
    if (private_file(again) != 0) {
        printf("FAIL: cannot make a temporary file beside %s\n", path);
        free(again);
        jbp_result_free(&back);
        return -1;
    }
    if (jbp_result_check(&back, why, sizeof why) != 0)
        printf("FAIL: %s does not read back sound: %s\n", path, why);
    else if ((d = result_diff(res, &back)) != NULL)
        printf("FAIL: %s reads back with %s different\n", path, d);
    else if (jbp_result_save(&back, again, err, sizeof err) != 0 || (b1 = read_file(path, &n1)) == NULL ||
             (b2 = read_file(again, &n2)) == NULL)
        printf("FAIL: cannot save %s again, or read it\n", path);
    else if (n1 != n2 || memcmp(b1, b2, n1) != 0)
        printf("FAIL: %s saved again from what was read back is not the same bytes\n", path);
    else if ((fields = diff_self_check(res, &back)) >= 0 && (alts = file_self_check(res, b1, n1, &kinds)) >= 0) {
        printf("wrote %s: read back with every field equal (identification, summary, %u reads, %u checks, "
               "%u kept waits, %u rows of VRAM, DONE, error, host's record, flags and counts), and saved "
               "again byte for byte the same; the comparison noticed each of %ld field changes and %ld "
               "alterations of the file's %d kinds of record\n",
               path, (unsigned)res->nreads, (unsigned)res->nexpects, (unsigned)res->nwaits,
               (unsigned)res->next_row, fields, alts, kinds);
        r = 0;
    }
    remove(again);
    free(again);
    free(b1);
    free(b2);
    jbp_result_free(&back);
    return r;
}

static int parse_gpu_fault(const char *arg)
{
    char kind[16];
    unsigned long at;

    if (sscanf(arg, "%15[a-z-]:%lu", kind, &at) != 2)
        return -1;
    sim.gpu_fault = strcmp(kind, "never-idle") == 0   ? G_NEVER_IDLE
                    : strcmp(kind, "never-read") == 0 ? G_NEVER_READ
                    : strcmp(kind, "dma-stall") == 0  ? G_DMA_STALL
                    : strcmp(kind, "prep-busy") == 0  ? G_PREP_BUSY
                    : strcmp(kind, "fill-busy") == 0  ? G_FILL_BUSY
                                                      : G_NONE;
    sim.gpu_fault_at = at;
    return sim.gpu_fault == G_NONE ? -1 : 0;
}

static int parse_fault(const char *s)
{
    fault *f = &sim.faults[sim.nfaults];
    char dir[8], kind[8];
    unsigned long at;

    if (sim.nfaults == 16 || sscanf(s, "%3[a-z0-9]:%4[a-z]:%lu", dir, kind, &at) != 3)
        return -1;
    f->dir = strcmp(dir, "h2p") == 0 ? DIR_H2P : strcmp(dir, "p2h") == 0 ? DIR_P2H : -1;
    f->kind = strcmp(kind, "flip") == 0 ? F_FLIP : strcmp(kind, "drop") == 0 ? F_DROP : -1;
    f->at = at;
    if (f->dir < 0 || f->kind < 0)
        return -1;
    ++sim.nfaults;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* --frames and --fuzz: frames made by hand, and at random, sent to the
   probe's own code (probe.c, the same as everywhere), with a host that only
   collects what comes back and acknowledges it. */

#ifndef JBP_SIM_PTY
enum { C_ACK, C_BAD_ACK_FIRST, C_BAD_NAK_FIRST, C_NO_ACK };

static struct {
    jbp_parser p;
    int mode, done, error, have_head;
    uint32_t error_code, head_bytes, head_crc;
    unsigned last_seq;
    unsigned long frames, again, hellos;
    unsigned long acks[256]; /* the probe's ACKs, by seq */
} col;

/* Frames put on the line to the probe at a virtual time (in order). */
static struct {
    uint32_t at;
    uint8_t f[JBP_HEADER_BYTES + 4 * JBP_START_WORDS + 8 + JBP_TRAILER_BYTES];
    size_t n;
} timed[16];
static int ntimed, timed_next;

static void col_at(uint32_t at, unsigned type, unsigned seq, const void *payload, size_t n)
{
    if (ntimed == 16 || n > 4 * JBP_START_WORDS + 8)
        exit(2);
    timed[ntimed].at = at;
    timed[ntimed].n = jbp_frame(timed[ntimed].f, type, seq, payload, n);
    ++ntimed;
}

static void col_send(unsigned type, unsigned seq, const void *payload, size_t n)
{
    uint8_t f[JBP_MAX_FRAME];
    size_t len = jbp_frame(f, type, seq, payload, n), i;

    for (i = 0; i < len; ++i)
        push(&sim.to_probe, f[i]);
}

static void pump_collector(void)
{
    queue *q = &sim.to_host;

    while (timed_next < ntimed && (int32_t)(sim.clock - timed[timed_next].at) >= 0) {
        size_t i;
        for (i = 0; i < timed[timed_next].n; ++i)
            push(&sim.to_probe, timed[timed_next].f[i]);
        ++timed_next;
    }
    while (q->head < q->n) {
        int r = jbp_parse(&col.p, q->b[q->head++], 0);
        unsigned t = col.p.type, seq = col.p.seq;
        int repeat;
        if (r == JBP_PARSE_FRAME && t == JBP_ACK && col.p.len == 0)
            ++col.acks[seq & 0xffu];
        if (r == JBP_PARSE_FRAME && t == JBP_HELLO)
            ++col.hellos;
        if (r != JBP_PARSE_FRAME || t < JBP_R_HEAD || t > JBP_R_IDENT)
            continue;
        repeat = col.frames > 0 && seq == col.last_seq;
        col.again += repeat;
        col.frames += !repeat;
        col.last_seq = seq;
        if (t == JBP_R_DONE)
            col.done = 1;
        if (t == JBP_R_ERROR && col.p.len == JBP_ERROR_BYTES) {
            col.error = 1;
            col.error_code = jbp_get32(col.p.payload);
        }
        if (t == JBP_R_HEAD && col.p.len == 4u * JBP_H_WORDS) {
            col.have_head = 1;
            col.head_bytes = jbp_get32(col.p.payload + 4 * JBP_H_LIST_BYTES);
            col.head_crc = jbp_get32(col.p.payload + 4 * JBP_H_LIST_CRC);
        }
        if (col.mode == C_NO_ACK)
            continue; /* a host that never acknowledges */
        if (col.mode == C_BAD_ACK_FIRST && !repeat)
            col_send(JBP_ACK, seq, "x", 1); /* not an ACK: the probe must send again */
        else if (col.mode == C_BAD_NAK_FIRST) {
            col_send(JBP_NAK, 0, "x", 1); /* not a NAK: the probe must not send again */
            col_send(JBP_ACK, seq, NULL, 0);
        } else
            col_send(JBP_ACK, seq, NULL, 0);
    }
}

/* A host that never stops sending: whenever the line to the probe
   is empty, until `until` ms, one more frame or run of bytes that the probe
   must ignore (or answer only with a NAK), so that the probe never sees an
   idle line. The kinds (FL_*) are taken in turn from `kinds`. */
enum { FL_TYPE = 1, FL_START = 2, FL_LIST = 4, FL_CTRL = 8, FL_DAMAGED = 16, FL_NOISE = 32 };

static struct {
    int on;
    unsigned kinds;
    uint32_t until;
    const uint8_t *start; /* the session's START payload, for FL_START */
    unsigned long n, bytes;
} flood;

/* A fresh machine, line and collector for one session. */
static void raw_reset(int mode)
{
    if (sim.jb != NULL)
        jb_destroy(sim.jb);
    free(sim.to_probe.b);
    free(sim.to_host.b);
    memset(&sim, 0, sizeof sim);
    memset(&col, 0, sizeof col);
    memset(&flood, 0, sizeof flood);
    ntimed = timed_next = 0;
    jbp_parser_init(&col.p);
    col.mode = mode;
    sim.jb = jb_create(NULL, NULL);
    sim.pump = pump_collector;
    if (sim.jb == NULL) {
        fprintf(stderr, "jb_create failed\n");
        exit(2);
    }
    probe_init();
}

/* The smallest list (FORMAT.md): header, two empty strings, then `nops`
   GP0(00h) words in one GP0 op (none: no op), END. Its length in bytes. */
static size_t tiny_list(uint8_t *b, size_t nops)
{
    size_t n = 0, i;

    memcpy(b, JBC_MAGIC, 8);
    n = 8;
    jbp_put32(b + n, JBC_VERSION), n += 4;
    jbp_put32(b + n, 24), n += 4;
    jbp_put32(b + n, 0), n += 4;
    jbp_put32(b + n, 0), n += 4;
    if (nops) {
        jbp_put32(b + n, (uint32_t)JBC_OP(JBC_GP0, nops)), n += 4;
        for (i = 0; i < nops; ++i)
            jbp_put32(b + n, 0), n += 4;
    }
    jbp_put32(b + n, (uint32_t)JBC_OP(JBC_END, 0)), n += 4;
    return n;
}

static uint32_t start_id = 0x1234u; /* START_SESSION of start_payload */

static void start_payload(uint8_t *p, uint32_t version, uint32_t bytes, uint32_t crc, uint32_t passes,
                          uint32_t window)
{
    memset(p, 0, 4 * JBP_START_WORDS + 8);
    jbp_put32(p + 4 * JBP_START_PROTOCOL, version);
    jbp_put32(p + 4 * JBP_START_SESSION, start_id);
    jbp_put32(p + 4 * JBP_START_FLAGS, JBP_FLAGS_DEFAULT);
    jbp_put32(p + 4 * JBP_START_LIST_BYTES, bytes);
    jbp_put32(p + 4 * JBP_START_LIST_CRC, crc);
    jbp_put32(p + 4 * JBP_START_PASSES, passes);
    jbp_put32(p + 4 * JBP_START_WINDOW, window);
}

typedef struct frame_case {
    const char *name;
    unsigned start_len, start_seq, version;
    long declared;            /* START's list length: the list's plus this */
    uint32_t passes, window;
    size_t nops;              /* GP0 NOPs in the list */
    size_t chunk;             /* LIST frame size (0: 4096) */
    size_t extra;             /* zero bytes added to the last LIST frame */
    int cut;                  /* the last word of the list not sent */
    int empty_list_frame;     /* an empty LIST frame first */
    int mode;                 /* the collector's */
    int expect;               /* -1: R_DONE; else the R_ERROR code */
    int untouched;            /* the GPU must not have been touched */
} frame_case;

static int frame_case_run(const frame_case *c)
{
    static uint8_t list[3 * 4096 + 64], start[4 * JBP_START_WORDS + 8];
    size_t n = tiny_list(list, c->nops), at = 0, send_n;
    unsigned seq = (c->start_seq + 1) & 0xffu;
    unsigned long gp_before;
    int r, ok;

    raw_reset(c->mode);
    start_payload(start, c->version, (uint32_t)((long)n + c->declared), jbp_crc32(0, list, n), c->passes,
                  c->window);
    col_send(JBP_START, c->start_seq, start, c->start_len);
    if (c->empty_list_frame)
        col_send(JBP_LIST, seq++, list, 0);
    send_n = n - (c->cut ? 4 : 0);
    memset(list + n, 0, c->extra);
    send_n += c->extra;
    while (at < send_n) {
        size_t k = send_n - at, chunk = c->chunk ? c->chunk : JBP_LIST_CHUNK;
        if (k > chunk)
            k = chunk;
        col_send(JBP_LIST, seq, list + at, k);
        seq = (seq + 1) & 0xffu;
        at += k;
    }
    gp_before = sim.gp0_words + gp1_words;
    r = probe_serve(100000);
    pump_collector();
    ok = c->expect < 0 ? r == 0 && col.done && !col.error && col.head_bytes == n
                       : r == 1 && !col.done && col.error && (int)col.error_code == c->expect;
    if (c->untouched && sim.gp0_words + gp1_words != gp_before)
        ok = 0;
    if (c->mode == C_BAD_ACK_FIRST && col.again != col.frames)
        ok = 0; /* every frame sent again once: the bad ACK was not taken */
    if (c->mode == C_BAD_NAK_FIRST && col.again != 0)
        ok = 0; /* none sent again: the bad NAK was not taken */
    printf("%s %s: probe %d, %s, error %u, %lu frames, %lu sent again, %lu GPU words, virtual %u ms\n",
           ok ? "ok  " : "FAIL", c->name, r, col.done ? "R_DONE" : "no R_DONE", (unsigned)col.error_code,
           col.frames, col.again, sim.gp0_words + gp1_words - gp_before, (unsigned)sim.clock);
    return ok ? 0 : 1;
}

/* START frames that come while a session is active. Each case:
   a good START (session 0x1234, the list's length and CRC-32), then the
   list in two LIST frames with `mid` between them, or (quiet) only the
   first LIST frame and then `mid` every 5 s from 5 s to 55 s. */
enum { M_SHORT, M_LONG, M_PROTOCOL, M_P_BIG, M_LEN_BAD, M_SEQ, M_SAME, M_NEW, M_LIST0 };

typedef struct active_case {
    const char *name;
    int mid;      /* M_* */
    int before;   /* mid goes before the first LIST frame, not between */
    int quiet;
    int expect;   /* -1: R_DONE; else the R_ERROR code */
    long acks0;   /* the probe's ACKs of seq 0 (START), or -1 */
    long resets;  /* GP1(00h) written (sessions prepared), or -1 */
} active_case;

/* The frame `mid` names, made from the session's START. */
static void mid_frame(int mid, const uint8_t *start, uint8_t *f, unsigned *type, unsigned *seq, size_t *n)
{
    memcpy(f, start, 4 * JBP_START_WORDS + 8);
    *type = JBP_START;
    *seq = 0;
    *n = 4 * JBP_START_WORDS;
    switch (mid) {
    case M_SHORT: *n = 20; break;
    case M_LONG: *n = 4 * JBP_START_WORDS + 4; break;
    case M_PROTOCOL: jbp_put32(f + 4 * JBP_START_PROTOCOL, JBP_PROTOCOL_VERSION - 1); break;
    case M_P_BIG: jbp_put32(f + 4 * JBP_START_PASSES, JBP_PASSES_MAX + 1); break;
    case M_LEN_BAD: jbp_put32(f + 4 * JBP_START_LIST_BYTES, jbp_get32(f + 4 * JBP_START_LIST_BYTES) + 2); break;
    case M_SEQ: *seq = 7; break;
    case M_NEW: jbp_put32(f + 4 * JBP_START_SESSION, 0x4321u); break;
    case M_LIST0: *type = JBP_LIST; *n = 8; break;
    default: break; /* M_SAME */
    }
}

static void flood_one(void)
{
    static const unsigned order[6] = {FL_TYPE, FL_START, FL_LIST, FL_CTRL, FL_DAMAGED, FL_NOISE};
    uint8_t f[JBP_HEADER_BYTES + 4 * JBP_START_WORDS + 8 + JBP_TRAILER_BYTES];
    uint8_t p[4 * JBP_START_WORDS + 8];
    unsigned kind = 0, type, seq, i;
    size_t n = 0, pn;

    for (i = 0; i < 6 && kind == 0; ++i, ++flood.n)
        if (flood.kinds & order[flood.n % 6])
            kind = order[flood.n % 6];
    memset(p, 0x5a, sizeof p);
    switch (kind) {
    case FL_TYPE: n = jbp_frame(f, 0xff, flood.n & 0xffu, p, 4); break;
    case FL_START:
        /* One of the STARTs a session ignores (active_cases, M_SHORT to M_SEQ). */
        mid_frame(M_SHORT + (int)(flood.n / 6 % 6), flood.start, p, &type, &seq, &pn);
        n = jbp_frame(f, type, seq, p, pn);
        break;
    case FL_LIST: n = jbp_frame(f, JBP_LIST, 0x80, p, 8); break; /* never the next seq or the last */
    case FL_CTRL: n = jbp_frame(f, flood.n & 64 ? JBP_ACK : JBP_NAK, 0, p, 1); break; /* with a payload */
    case FL_DAMAGED:
        n = jbp_frame(f, 0xff, 0, p, 4);
        f[JBP_HEADER_BYTES] ^= 1; /* its CRC-32 no longer holds: the probe NAKs it */
        break;
    case FL_NOISE: memset(f, 0, n = 16); break; /* no sync byte */
    default: return;
    }
    for (i = 0; i < n; ++i)
        push(&sim.to_probe, f[i]);
    flood.bytes += n;
}

static void pump_flood(void)
{
    pump_collector();
    if (flood.on && sim.to_probe.head == sim.to_probe.n && (int32_t)(sim.clock - flood.until) < 0)
        flood_one();
}

static int active_case_run(const active_case *c)
{
    static uint8_t list[512], start[4 * JBP_START_WORDS + 8], f[4 * JBP_START_WORDS + 8];
    size_t n = tiny_list(list, 40), half = n / 2;
    unsigned type, seq;
    size_t fn;
    uint32_t t;
    int r, ok;

    raw_reset(C_ACK);
    start_payload(start, JBP_PROTOCOL_VERSION, (uint32_t)n, jbp_crc32(0, list, n), 0, 0);
    mid_frame(c->mid, start, f, &type, &seq, &fn);
    col_send(JBP_START, 0, start, 4 * JBP_START_WORDS);
    if (c->before)
        col_send(type, seq, f, fn);
    col_send(JBP_LIST, 1, list, half);
    if (c->quiet) {
        for (t = 5000; t <= 55000; t += 5000) {
            mid_frame(M_SHORT + (int)(t / 5000) % 6, start, f, &type, &seq, &fn);
            col_at(t, type, seq, f, fn);
        }
    } else {
        if (!c->before)
            col_send(type, seq, f, fn);
        if (c->mid == M_NEW) {
            /* The new session's list, from the start. */
            col_send(JBP_LIST, 1, list, half);
            col_send(JBP_LIST, 2, list + half, n - half);
        } else
            col_send(JBP_LIST, 2, list + half, n - half);
    }
    r = probe_serve(100000);
    pump_collector();
    ok = c->expect < 0 ? r == 0 && col.done && !col.error && col.head_bytes == n
                       : r == 1 && !col.done && col.error && (int)col.error_code == c->expect;
    if (c->acks0 >= 0 && col.acks[0] != (unsigned long)c->acks0)
        ok = 0;
    if (c->resets >= 0 && sim.resets != (unsigned long)c->resets)
        ok = 0;
    /* A quiet host: the session ends T_SESSION (30 s) after the last LIST
       taken (at about 0 ms), however many bad STARTs came after it. */
    if (c->quiet && (sim.clock < 30000 || sim.clock > 34000))
        ok = 0;
    printf("%s %s: probe %d, %s, error %u, %lu ACKs of seq 0, %lu sessions prepared, virtual %u ms\n",
           ok ? "ok  " : "FAIL", c->name, r, col.done ? "R_DONE" : "no R_DONE", (unsigned)col.error_code,
           col.acks[0], sim.resets, (unsigned)sim.clock);
    return ok ? 0 : 1;
}

static int active_cases(void)
{
    static const active_case cases[] = {
        /* name                                                   mid  before quiet expect acks0 resets */
        {"in a session: a 20-byte START, ignored",                M_SHORT, 0, 0, -1, 1, 1},
        {"in a session: a 36-byte START, ignored",                M_LONG, 0, 0, -1, 1, 1},
        {"in a session: a START of the last protocol, ignored",   M_PROTOCOL, 0, 0, -1, 1, 1},
        {"in a session: a START with P too big, ignored",         M_P_BIG, 0, 0, -1, 1, 1},
        {"in a session: a START of an unaligned length, ignored", M_LEN_BAD, 0, 0, -1, 1, 1},
        {"in a session: a START at seq 7, ignored",               M_SEQ, 0, 0, -1, 1, 1},
        {"before the list: a START of the last protocol with the session's id, not acknowledged",
         M_PROTOCOL, 1, 0, -1, 1, 1},
        {"before the list: a short START with the session's id, not acknowledged", M_SHORT, 1, 0, -1, 1, 1},
        {"before the list: the session's START again, acknowledged again", M_SAME, 1, 0, -1, 2, 1},
        {"before the list: a LIST at seq 0, not taken or acknowledged", M_LIST0, 1, 0, -1, 1, 1},
        {"in a session: the session's START again, a new session", M_SAME, 0, 0, JBP_E_TIMEOUT, -1, 2},
        {"in a session: another session's START, then its list", M_NEW, 0, 0, -1, 2, 2},
        {"a quiet host: bad STARTs every 5 s do not hold the session", M_SHORT, 0, 1, JBP_E_TIMEOUT, 1, 1},
    };
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        bad |= active_case_run(&cases[i]);
    return bad;
}

/* A host that never lets the line go idle. Every deadline of the
   probe must hold while bytes keep coming: the session's T_SESSION (from
   the last frame acted on), the idle loop's own limit and its HELLOs, and
   send_data's tries for an ACK that never comes. `where`: 0 in a session
   (START and half the list taken, then the flood), 1 idle (no START: the
   flood only), 2 the results (the whole list, a host that never
   acknowledges). */
typedef struct flood_case {
    const char *name;
    int where;
    unsigned kinds; /* FL_* */
} flood_case;

static int flood_case_run(const flood_case *c)
{
    static uint8_t list[512], start[4 * JBP_START_WORDS + 8];
    size_t n = tiny_list(list, 40), half = n / 2;
    uint32_t at_return;
    int r, ok;

    raw_reset(c->where == 2 ? C_NO_ACK : C_ACK);
    start_payload(start, JBP_PROTOCOL_VERSION, (uint32_t)n, jbp_crc32(0, list, n), 0, 0);
    flood.on = 1;
    flood.kinds = c->kinds;
    flood.until = 70000;
    flood.start = start;
    sim.pump = pump_flood;
    if (c->where != 1) {
        col_send(JBP_START, 0, start, 4 * JBP_START_WORDS);
        col_send(JBP_LIST, 1, list, c->where == 2 ? n : half);
    }
    r = probe_serve(5000);
    at_return = sim.clock;
    pump_collector();
    switch (c->where) {
    case 0: /* the session ends T_SESSION after the last LIST taken (at about 0 ms) */
        ok = r == 1 && !col.done && col.error && col.error_code == JBP_E_TIMEOUT && sim.clock >= 30000 &&
             sim.clock <= 34000;
        break;
    case 1: /* the idle loop's limit, and a HELLO each second */
        ok = r == -1 && !col.error && sim.clock >= 5000 && sim.clock <= 6000 && col.hellos >= 5;
        break;
    default: /* T_TRIES sends of R_IDENT, T_ACK apart at most */
        ok = r == 1 && !col.done && sim.clock <= 20000;
        break;
    }
    printf("%s %s: probe %d, %s, error %u, %lu HELLOs, %lu flood bytes, virtual %u ms\n", ok ? "ok  " : "FAIL",
           c->name, r, col.done ? "R_DONE" : "no R_DONE", (unsigned)col.error_code, col.hellos, flood.bytes,
           (unsigned)at_return);
    return ok ? 0 : 1;
}

static int flood_cases(void)
{
    enum { ALL = FL_TYPE | FL_START | FL_LIST | FL_CTRL | FL_DAMAGED | FL_NOISE };
    static const flood_case cases[] = {
        {"a flooding host: frames of no type do not hold the session", 0, FL_TYPE},
        {"a flooding host: STARTs to ignore do not hold the session", 0, FL_START},
        {"a flooding host: LISTs of another seq do not hold the session", 0, FL_LIST},
        {"a flooding host: ACKs and NAKs with a payload do not hold the session", 0, FL_CTRL},
        {"a flooding host: damaged frames (NAKed) do not hold the session", 0, FL_DAMAGED},
        {"a flooding host: bytes with no frame do not hold the session", 0, FL_NOISE},
        {"a flooding host: all of these do not hold the session", 0, ALL},
        {"a flooding host, idle: the idle limit holds and HELLOs go on", 1, ALL & ~FL_START},
        {"a flooding host that never acknowledges: the results are given up", 2, ALL},
        {"a flooding host that never acknowledges, no damage: given up after T_TRIES", 2, FL_TYPE | FL_NOISE},
    };
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        bad |= flood_case_run(&cases[i]);
    return bad;
}

/* probe_sim_frames: START, LIST and the control frames, well and badly
   made, and STARTs in an active session. */
static int frame_cases(void)
{
    enum { L = 4 * JBP_START_WORDS, E5 = JBP_E_LENGTH, E10 = JBP_E_FRAME, V = JBP_PROTOCOL_VERSION };
    static const frame_case cases[] = {
        /* name                  len  seq ver decl P W nops chunk extra cut empty mode expect untouched */
        {"a good session",        L,    0, V,  0, 0, 0, 0, 0, 0, 0, 0, C_ACK, -1, 0},
        {"4096-byte frames",      L,    0, V,  0, 0, 0, 2000, 0, 0, 0, 0, C_ACK, -1, 0},
        {"1-byte frames",         L,    0, V,  0, 0, 0, 3, 1, 0, 0, 0, C_ACK, -1, 0},
        {"ACK with a payload",    L,    0, V,  0, 0, 0, 0, 0, 0, 0, 0, C_BAD_ACK_FIRST, -1, 0},
        {"NAK with a payload",    L,    0, V,  0, 0, 0, 0, 0, 0, 0, 0, C_BAD_NAK_FIRST, -1, 0},
        {"short START",           20,   0, V,  0, 0, 0, 0, 0, 0, 0, 0, C_ACK, E10, 1},
        {"long START",            L + 1, 0, V, 0, 0, 0, 0, 0, 0, 0, 0, C_ACK, E10, 1},
        {"START at seq 9",        L,    9, V,  0, 0, 0, 0, 0, 0, 0, 0, C_ACK, E10, 1},
        {"START of protocol 2",   L,    0, 2,  0, 0, 0, 0, 0, 0, 0, 0, C_ACK, JBP_E_PROTOCOL, 1},
        {"P too big",             L,    0, V,  0, 1000001, 0, 0, 0, 0, 0, 0, C_ACK, JBP_E_PASSES, 1},
        {"W too big",             L,    0, V,  0, 0, 1000001, 0, 0, 0, 0, 0, C_ACK, JBP_E_PASSES, 1},
        {"length zero",           L,    0, V, -28, 0, 0, 0, 0, 0, 0, 0, C_ACK, E5, 1},
        {"length 24",             L,    0, V, -4, 0, 0, 0, 0, 0, 0, 0, C_ACK, E5, 1},
        {"length unaligned",      L,    0, V,  1, 0, 0, 0, 0, 0, 0, 0, C_ACK, E5, 1},
        {"a byte after END",      L,    0, V,  0, 0, 0, 0, 0, 1, 0, 0, C_ACK, E5, 0},
        {"a word after END",      L,    0, V,  4, 0, 0, 0, 0, 4, 0, 0, C_ACK, E5, 0},
        {"END before the length", L,    0, V,  4, 0, 0, 0, 0, 0, 0, 0, C_ACK, E5, 0},
        {"an op past the end",    L,    0, V, -4, 0, 0, 2, 0, 0, 1, 0, C_ACK, E5, 0},
        {"an empty LIST frame",   L,    0, V,  0, 0, 0, 0, 0, 0, 0, 1, C_ACK, E10, 0},
        {"a 4097-byte LIST",      L,    0, V,  0, 0, 0, 1017, 4097, 0, 0, 0, C_ACK, E10, 0},
    };
    size_t i;
    int bad = 0;

    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i)
        bad |= frame_case_run(&cases[i]);
    /* An op cut by the declared end is reported at once, not after the
       host has been quiet for T_SESSION (30 s). */
    {
        frame_case c = cases[17];
        c.name = "an op past the end, at once";
        bad |= frame_case_run(&c);
        if (sim.clock > 5000) {
            printf("FAIL: the length error took %u virtual ms\n", (unsigned)sim.clock);
            bad = 1;
        }
    }
    bad |= active_cases();
    bad |= flood_cases();
    jb_destroy(sim.jb);
    sim.jb = NULL;
    free(sim.to_probe.b);
    free(sim.to_host.b);
    printf("%s\n", bad ? "FAIL" : "OK");
    return bad;
}

/* probe_sim_fuzz: sessions made at random and damaged at random, the
   frames' CRC-32s mostly made good again so that the damage reaches the
   probe's checks. Whatever comes, the probe must stay inside its memory
   (the sanitizers), end every session, and send R_DONE only for a list
   that is exactly the one the last START it can have taken announced. */
static uint32_t fz;

static uint32_t rnd(uint32_t n)
{
    fz ^= fz << 13;
    fz ^= fz >> 17;
    fz ^= fz << 5;
    return n ? fz % n : 0;
}

static void fz_frame(queue *q, unsigned type, unsigned seq, const uint8_t *p, size_t n)
{
    static uint8_t f[JBP_MAX_FRAME + 64];
    size_t len, i;

    if (n > JBP_MAX_PAYLOAD)
        n = JBP_MAX_PAYLOAD;
    len = jbp_frame(f, type, seq, p, n);
    for (i = 0; i < len; ++i)
        push(q, f[i]);
}

/* A START the probe may begin a session with (PROTOCOL.md, "Session flow",
   2), as the fuzz's checker sees it. */
static int start_ok(const jbp_parser *p)
{
    uint32_t bytes;

    if (p->len != 4u * JBP_START_WORDS || p->seq != 0 || jbp_get32(p->payload) != JBP_PROTOCOL_VERSION ||
        jbp_get32(p->payload + 4 * JBP_START_PASSES) > JBP_PASSES_MAX ||
        jbp_get32(p->payload + 4 * JBP_START_WINDOW) > JBP_PASSES_MAX)
        return 0;
    bytes = jbp_get32(p->payload + 4 * JBP_START_LIST_BYTES);
    return bytes % 4 == 0 && bytes >= JBP_LIST_BYTES_MIN;
}

static int fuzz_probe(uint32_t seed, unsigned long rounds)
{
    static uint8_t list[3 * 4096 + 64], start[4 * JBP_START_WORDS + 8], junk[JBP_MAX_PAYLOAD];
    unsigned long k, done = 0, refused = 0, bad = 0, done_bad_start = 0, nflooded = 0;

    fz = seed ? seed : 1;
    for (k = 0; k < rounds; ++k) {
        size_t n = tiny_list(list, rnd(4) ? rnd(40) : rnd(2500)), at = 0, i;
        uint32_t crc = jbp_crc32(0, list, n), decl = (uint32_t)n;
        unsigned seq = 1, start_seq = 0, start_len = 4 * JBP_START_WORDS;
        queue q = {NULL, 0, 0, 0};
        jbp_parser chk;
        int r, have_start = 0, first = 1, bad_start = 0, touched = 0, flooded;
        uint32_t passes;
        uint32_t want_bytes = 0, want_crc = 0;

        raw_reset(C_ACK);
        /* START, perhaps wrong. */
        switch (rnd(12)) {
        case 0: start_len = rnd(4 * JBP_START_WORDS + 8); touched = 1; break;
        case 1: start_seq = 1 + rnd(255); touched = 1; break;
        case 2: decl = rnd(3) ? decl + (uint32_t)rnd(9) - 4 : rnd(64); touched = 1; break;
        case 3: crc ^= 1u << rnd(32); touched = 1; break;
        default: break;
        }
        {
            uint32_t version = rnd(16) ? JBP_PROTOCOL_VERSION : rnd(5);
            passes = rnd(10) ? 0 : rnd(3);
            touched |= version != JBP_PROTOCOL_VERSION || passes != 0; /* the list has no main loop */
            start_payload(start, version, decl, crc, passes, rnd(10) ? 0 : rnd(3));
        }
        fz_frame(&q, JBP_START, start_seq, start, start_len);
        /* The list in frames of random sizes, with frames added, dropped,
           repeated, resized and renumbered now and then. */
        while (at < n) {
            size_t len = 1 + rnd(rnd(3) ? JBP_LIST_CHUNK : 64);
            unsigned s2 = seq;
            if (len > n - at)
                len = n - at;
            int what = (int)rnd(40);
            touched |= what <= 6;
            switch (what) {
            case 0: s2 = rnd(256); break;
            case 1: len = 0; break;
            case 2: fz_frame(&q, JBP_LIST, seq - 1, list + at, len); break;
            case 3: fz_frame(&q, rnd(256), rnd(256), junk, rnd(64)); break;
            case 4: fz_frame(&q, rnd(2) ? JBP_ACK : JBP_NAK, rnd(256), junk, rnd(3)); break;
            case 5: at += len; continue; /* dropped */
            case 6: fz_frame(&q, JBP_START, 0, start, 4 * JBP_START_WORDS); break;
            case 7:
            case 8: {
                /* A START the probe must not act on in a session. */
                static uint8_t f[4 * JBP_START_WORDS + 8];
                unsigned type, sq;
                size_t fn;
                mid_frame(M_SHORT + (int)rnd(6), start, f, &type, &sq, &fn);
                fz_frame(&q, type, sq, f, fn);
                bad_start = 1;
                break;
            }
            default: break;
            }
            if (rnd(60) == 0 && at + len == n) {
                memset(list + n, (int)rnd(256), 8);
                len += 1 + rnd(8); /* bytes after END */
                touched = 1;
            }
            fz_frame(&q, JBP_LIST, s2, list + at, len);
            if (len == 0)
                continue;
            at += len;
            seq = (seq + 1) & 0xffu;
        }
        /* Raw damage, which the CRC-32s catch. */
        if (rnd(4) == 0)
            for (i = rnd(4); i > 0 && q.n; --i, touched = 1)
                q.b[rnd((uint32_t)q.n)] ^= (uint8_t)(1u << rnd(8));
        if (rnd(10) == 0 && q.n) {
            q.n = rnd((uint32_t)q.n); /* cut short */
            touched = 1;
        }
        /* What the probe can have taken: nothing when the first START on
           the line is one it refuses (the session ends with it); else the
           last START it may begin a session with, the others being
           ignored in a session. */
        jbp_parser_init(&chk);
        for (i = 0; i < q.n; ++i)
            if (jbp_parse(&chk, q.b[i], 0) == JBP_PARSE_FRAME && chk.type == JBP_START) {
                int good = start_ok(&chk);
                if (first && !good) {
                    have_start = 0;
                    break;
                }
                first = 0;
                if (good) {
                    have_start = 1;
                    want_bytes = jbp_get32(chk.payload + 4 * JBP_START_LIST_BYTES);
                    want_crc = jbp_get32(chk.payload + 4 * JBP_START_LIST_CRC);
                }
            }
        for (i = 0; i < q.n; ++i)
            push(&sim.to_probe, q.b[i]);
        free(q.b);
        /* Now and then a host that, once its frames are sent, never lets
           the line go idle again: frames the probe must ignore or
           NAK, never a LIST (whose seq could be the last one taken). */
        flooded = rnd(25) == 0;
        if (flooded) {
            flood.on = 1;
            flood.kinds = (1 + rnd(63)) & ~(unsigned)FL_LIST;
            if (flood.kinds == 0)
                flood.kinds = FL_TYPE;
            flood.until = 100000;
            flood.start = start;
            sim.pump = pump_flood;
            touched = 1;
        }
        r = probe_serve(5000);
        pump_collector();
        if (flooded) {
            ++nflooded;
            if (sim.clock > 50000) {
                printf("FAIL: round %lu (seed %u): a flooding host held the probe for %u virtual ms\n", k,
                       (unsigned)seed, (unsigned)sim.clock);
                ++bad;
            }
        }
        if (col.done && (r != 0 || col.error || !have_start || !col.have_head ||
                         col.head_bytes != want_bytes || col.head_crc != want_crc)) {
            printf("FAIL: round %lu (seed %u): R_DONE for %u bytes, CRC-32 0x%08x; the last good START "
                   "%s %u bytes, CRC-32 0x%08x\n",
                   k, (unsigned)seed, (unsigned)col.head_bytes, (unsigned)col.head_crc,
                   have_start ? "said" : "(none) said", (unsigned)want_bytes, (unsigned)want_crc);
            ++bad;
        }
        if (col.done && (want_bytes != n || want_crc != jbp_crc32(0, list, n))) {
            printf("FAIL: round %lu (seed %u): R_DONE for a list that is not the one generated\n", k,
                   (unsigned)seed);
            ++bad;
        }
        /* A round whose only disturbance is STARTs the probe must ignore
           in a session completes (they neither restart nor fail it). */
        if (!touched && !col.done) {
            printf("FAIL: round %lu (seed %u): an undamaged session%s did not complete (error %u)\n", k,
                   (unsigned)seed, bad_start ? " with STARTs to ignore" : "", (unsigned)col.error_code);
            ++bad;
        }
        done += col.done;
        refused += col.error;
        done_bad_start += col.done && bad_start;
    }
    jb_destroy(sim.jb);
    sim.jb = NULL;
    free(sim.to_probe.b);
    free(sim.to_host.b);
    printf("%lu rounds (seed %u): %lu sessions completed (%lu with a START to ignore in the middle), "
           "%lu refused, %lu neither, %lu with a flooding host; %lu failures\n%s\n",
           rounds, (unsigned)seed, done, done_bad_start, refused, rounds - done - refused, nflooded, bad,
           bad ? "FAIL" : "OK");
    /* No branch may be empty, or the fuzz proves nothing. */
    if (done == 0 || refused == 0 || done_bad_start == 0 || nflooded == 0) {
        printf("FAIL: the rounds did not reach both outcomes\n");
        return 1;
    }
    return bad != 0;
}
#else
static int frame_cases(void)
{
    return 2;
}

static int fuzz_probe(uint32_t seed, unsigned long rounds)
{
    (void)seed;
    (void)rounds;
    return 2;
}
#endif

/* probe_sim_packets: the conformance reader refuses a list whose LOOP or
   END falls inside a GPU packet, which the probe could not read VRAM back
   at (FORMAT.md, "Packet boundaries at LOOP and END"), and takes one whose
   packets end there. */
static int packet_cases(void)
{
    typedef struct pcase {
        const char *name;
        uint32_t before[8], after[8];
        int nb, na, loop, take;
    } pcase;
    static const pcase cases[] = {
        {"a fill, then LOOP", {0x02000000u, 0, 0x00100010u}, {0}, 3, 0, 1, 1},
        {"a fill's header before LOOP, its words after", {0x02000000u}, {0, 0x00100010u}, 1, 2, 1, 0},
        {"a fill cut by END", {0x02000000u, 0}, {0}, 2, 0, 0, 0},
        {"a 2 x 1 load cut by LOOP", {0xa0000000u, 0, 0x00010002u}, {0x7fff7fffu}, 3, 1, 1, 0},
        {"a 2 x 1 load, then LOOP", {0xa0000000u, 0, 0x00010002u, 0x7fff7fffu}, {0}, 4, 0, 1, 1},
        {"a polyline without its end before END", {0x48ffffffu, 0, 0x00100010u, 0x00200020u}, {0}, 4, 0,
         0, 0},
        {"a polyline ended", {0x48ffffffu, 0, 0x00100010u, 0x55555555u}, {0}, 4, 0, 0, 1},
        {"a polyline whose second vertex looks like its end",
         {0x48ffffffu, 0, 0x55555555u}, {0}, 3, 0, 0, 0},
        {"a shaded triangle cut by LOOP", {0x30ffffffu, 0, 0x00ff00ffu, 0x00100010u}, {0xffu, 0x00100000u},
         4, 2, 1, 0},
        {"a packet dropped by GP1(01h), then LOOP", {0x02000000u}, {0}, 1, 0, 2, 1},
    };
    size_t c;
    int bad = 0;

    for (c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const pcase *k = &cases[c];
        uint32_t w[64];
        size_t n = 0, i;
        jbc_list l;
        char err[256];
        int took;
        memcpy(w, JBC_MAGIC, 8);
        n = 2;
        w[n++] = JBC_VERSION;
        w[n++] = 24;
        w[n++] = 0;
        w[n++] = 0;
        w[n++] = (uint32_t)JBC_OP(JBC_GP0, k->nb);
        for (i = 0; i < (size_t)k->nb; ++i)
            w[n++] = k->before[i];
        if (k->loop == 2) {
            w[n++] = (uint32_t)JBC_OP(JBC_GP1, 1);
            w[n++] = 0x01000000u;
        }
        if (k->loop)
            w[n++] = (uint32_t)JBC_OP(JBC_LOOP, 0);
        if (k->na) {
            w[n++] = (uint32_t)JBC_OP(JBC_GP0, k->na);
            for (i = 0; i < (size_t)k->na; ++i)
                w[n++] = k->after[i];
        }
        w[n++] = (uint32_t)JBC_OP(JBC_END, 0);
        err[0] = 0;
        took = jbc_parse(w, n, &l, err, sizeof err) == 0;
        if (took)
            jbc_free(&l);
        printf("%s %s: %s%s%s\n", took == k->take ? "ok  " : "FAIL", k->name, took ? "taken" : "refused",
               err[0] ? ": " : "", err);
        bad |= took != k->take;
    }
    printf("%s\n", bad ? "FAIL" : "OK");
    return bad;
}

int main(int argc, char **argv)
{
    const char *list_path = NULL, *save = NULL, *tamper[8][2];
    int lie_crc = 0, bad_magic = 0, expect_error = -1, expect_refused = 0, late = 0, ntamper = 0;
    int have_passes = 0, a, bad = 0, r, i, expect_no_resend = 0, expect_naks = 0, expect_no_results = 0;
    unsigned long passes = 0, wait_ms = 0;
    jbh_options ho;
    jbc_list l;
    jbc_options po;
    jbc_outcome ref;
    char err[256];
    uint8_t *bytes;
    size_t n = 0;
    jbp_result *res;

    for (a = 1; a < argc; ++a) {
        if (strcmp(argv[a], "--fault") == 0 && a + 1 < argc) {
            if (parse_fault(argv[++a]) != 0)
                return usage();
        } else if (strcmp(argv[a], "--lie-crc") == 0)
            lie_crc = 1;
        else if (strcmp(argv[a], "--bad-magic") == 0)
            bad_magic = 1;
        else if (strcmp(argv[a], "--dma-direction-1") == 0)
            sim.dma_direction_1 = 1;
        else if (strcmp(argv[a], "--expect-error") == 0 && a + 1 < argc)
            expect_error = atoi(argv[++a]);
        else if (strcmp(argv[a], "--save") == 0 && a + 1 < argc)
            save = argv[++a];
        else if (strcmp(argv[a], "--save-tampered") == 0 && a + 2 < argc && ntamper < 8) {
            tamper[ntamper][0] = argv[++a];
            tamper[ntamper++][1] = argv[++a];
        } else if (strcmp(argv[a], "--passes") == 0 && a + 1 < argc) {
            char *end;
            passes = strtoul(argv[++a], &end, 10);
            if (*end != 0 || passes > JBP_PASSES_MAX)
                return usage();
            have_passes = 1;
        } else if (strcmp(argv[a], "--expect-refused") == 0)
            expect_refused = 1;
        else if (strcmp(argv[a], "--gpu") == 0 && a + 1 < argc) {
            if (parse_gpu_fault(argv[++a]) != 0)
                return usage();
        } else if (strcmp(argv[a], "--wait-ms") == 0 && a + 1 < argc)
            wait_ms = strtoul(argv[++a], NULL, 10);
        else if (strcmp(argv[a], "--expect-no-resend") == 0)
            expect_no_resend = 1;
        else if (strcmp(argv[a], "--expect-naks") == 0)
            expect_naks = 1;
        else if (strcmp(argv[a], "--expect-no-results") == 0)
            expect_no_results = 1;
        else if (strcmp(argv[a], "--frames") == 0)
            return frame_cases();
        else if (strcmp(argv[a], "--packets") == 0)
            return packet_cases();
        else if (strcmp(argv[a], "--fuzz") == 0 && a + 2 < argc) {
            unsigned long seed = strtoul(argv[a + 1], NULL, 10), rounds = strtoul(argv[a + 2], NULL, 10);
            return fuzz_probe((uint32_t)seed, rounds);
        }
        else if (strcmp(argv[a], "--late-loop") == 0)
            late = 1;
        else if (argv[a][0] == '-' || list_path != NULL)
            return usage();
        else
            list_path = argv[a];
    }
    if ((list_path == NULL) == !late || !have_passes || (expect_refused && (save || ntamper)))
        return usage();

    /* The list, and jitterbug's own player as the reference, by the same
       rule as the probe's. */
    bytes = late ? late_loop(&n) : read_file(list_path, &n);
    if (late)
        list_path = "late-loop";
    if (bytes == NULL) {
        fprintf(stderr, "cannot read %s\n", list_path);
        return 2;
    }
    {
        uint32_t *w = malloc(n + 4);
        size_t k;
        if (w == NULL || n % 4 != 0)
            return 2;
        for (k = 0; k < n / 4; ++k)
            w[k] = jbp_get32(bytes + 4 * k);
        if (jbc_parse(w, n / 4, &l, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", list_path, err);
            return 2;
        }
        free(w);
    }
    memset(&po, 0, sizeof po);
    po.passes = (unsigned)passes;
    if (jbc_play(&l, &po, &ref, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", list_path, err);
        return 2;
    }
    if (bad_magic)
        bytes[0] ^= 0x20;

    /* The console: a fresh context. */
    sim.jb = jb_create(NULL, NULL);
    if (sim.jb == NULL) {
        fprintf(stderr, "jb_create failed\n");
        return 2;
    }
#ifdef JBP_SIM_PTY
    {
        /* jbprobe on the terminal's other end, on a thread. */
        static host_thread h;
        static char passes_arg[16];
        static jbp_result loaded;
        pthread_t th;
        struct termios tio;
        int slave;
        char *name;
        if (save == NULL || late || sim.nfaults || lie_crc || bad_magic || expect_error >= 0 ||
            expect_refused || ntamper)
            return usage();
        pty_fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (pty_fd < 0 || grantpt(pty_fd) != 0 || unlockpt(pty_fd) != 0 || (name = ptsname(pty_fd)) == NULL) {
            printf("no pseudo-terminal here: skipped\n");
            return 77;
        }
        /* Raw from the start, before jbprobe sets it so: no echo of HELLOs. */
        slave = open(name, O_RDWR | O_NOCTTY);
        if (slave < 0 || tcgetattr(slave, &tio) != 0) {
            fprintf(stderr, "%s: cannot open\n", name);
            return 2;
        }
        tio.c_iflag = 0;
        tio.c_oflag = 0;
        tio.c_lflag = 0;
        tcsetattr(slave, TCSANOW, &tio);
        h.argv[0] = "jbprobe";
        h.argv[1] = "send";
        h.argv[2] = "--quiet";
        h.argv[3] = "--tty";
        h.argv[4] = name;
        h.argv[5] = "--passes";
        snprintf(passes_arg, sizeof passes_arg, "%lu", passes);
        h.argv[6] = passes_arg;
        h.argv[7] = (char *)list_path;
        h.argv[8] = (char *)save;
        h.argv[9] = NULL;
        if (pthread_create(&th, NULL, run_jbprobe, &h) != 0)
            return 2;
        probe_init();
        r = probe_serve(60000);
        pthread_join(th, NULL);
        close(slave);
        printf("%s: probe %s; jbprobe send exit status %d; %lu bytes to the probe, %lu back over %s\n",
               l.name, r == 0 ? "sent its results" : r > 0 ? "failed the session" : "idle", h.rc,
               sim.count[DIR_H2P], sim.count[DIR_P2H], name);
        if (h.rc != 0 || r != 0 || jbp_result_load(&loaded, save, err, sizeof err) != 0) {
            printf("FAIL: the session over the terminal did not complete\n");
            return 1;
        }
        res = &loaded;
        save = NULL; /* already written, by jbprobe */
        goto check;
    }
#endif
    jbh_defaults(&ho);
    ho.session_id = 0x5eed0001u;
    ho.lie_crc = lie_crc;
    ho.passes = (uint32_t)passes;
    ho.wait_ms = (uint32_t)wait_ms;
    ho.write = host_write;
    ho.log = getenv("PROBE_SIM_LOG") ? stderr : NULL;
    if (jbh_begin(&sim.host, bytes, n, &ho, sim.clock) != 0) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    sim.pump = pump_session;
    probe_init();
    r = probe_serve(100000000u);
    if (sim.oom) {
        fprintf(stderr, "out of memory\n");
        return 2;
    }
    /* The probe's last ACK wait may end before the host saw a resent
       frame; give the host the bytes still on the line. */
    pump_host();
    res = &sim.host.result;

    printf("%s: probe %s, host %s; %lu bytes to the probe, %lu back; virtual %u ms\n", l.name,
           r == 0 ? "sent its results" : r > 0 ? "failed the session" : "idle",
           sim.host.state == JBH_DONE ? "done" : sim.host.state == JBH_FAILED ? "failed" : "running",
           sim.count[DIR_H2P], sim.count[DIR_P2H], (unsigned)sim.clock);
    printf("line: host sent %lu again, %lu NAKs sent, %lu received, %lu damaged and %lu cut "
           "frames received; probe: %u damaged frames received, %u sent again\n",
           sim.host.resent, sim.host.naks_sent, sim.host.naks_received, sim.host.bad_received,
           sim.host.cut_received, (unsigned)res->done_rx_bad, (unsigned)res->done_resent);
    for (i = 0; i < sim.nfaults; ++i)
        if (!sim.faults[i].done) {
            printf("FAIL: fault %d (byte %lu) was never reached\n", i, sim.faults[i].at);
            bad = 1;
        }

    if (hal_contract(res))
        bad = 1;
    if (expect_naks && (sim.host.naks_received == 0 || sim.host.naks_sent == 0)) {
        /* Each side answers a damaged frame with a NAK (PROTOCOL.md, the
           frame table): the faults given put one in a LIST frame and one in
           a VRAM frame. */
        printf("FAIL: the probe sent %lu NAKs, the host %lu: each side must answer a damaged frame "
               "with one\n",
               sim.host.naks_received, sim.host.naks_sent);
        bad = 1;
    } else if (expect_naks)
        printf("NAKs: the probe sent %lu, the host %lu\n", sim.host.naks_received, sim.host.naks_sent);
    if (expect_no_resend) {
        /* The probe was busy for longer than the host's ACK interval: its
           PROGRESS frames must have kept the host from sending again. */
        if (sim.host.resent != 0) {
            printf("FAIL: the host sent %lu frames again while the probe was busy\n", sim.host.resent);
            bad = 1;
        } else
            printf("the host sent nothing again while the probe was busy\n");
    }
    if (expect_error >= 0) {
        if (r != 1 || sim.host.state != JBH_FAILED || !res->have_error || res->have_done ||
            (int)res->error[0] != expect_error) {
            printf("FAIL: expected the probe to refuse with error %d (got r %d, host %d, error %u)\n",
                   expect_error, r, sim.host.state, res->have_error ? (unsigned)res->error[0] : 0u);
            bad = 1;
        } else if (expect_no_results && res->have_head) {
            /* The session failed before its results (restore's ready
               wait): no summary was sent. */
            printf("FAIL: the probe sent its summary before it failed\n");
            bad = 1;
        } else if (expect_error == JBP_E_PREPARE &&
                   (sim.gp0_after_reset > 6 || res->error[1] != 0 || res->error[2] != 0)) {
            /* No word of the list may have reached the GPU: at most the
               six of the fills went after GP1(00h), and no list byte was
               taken. */
            printf("FAIL: %lu GP0 words after the reset, error at word %u, %u list bytes taken\n",
                   sim.gp0_after_reset, (unsigned)res->error[1], (unsigned)res->error[2]);
            bad = 1;
        } else
            printf("refused as expected: error %d (%s); %lu GP0 words after the reset\n", expect_error,
                   sim.host.why, sim.gp0_after_reset);
        goto end;
    }
    if (expect_refused) {
        char why[256];
        if (r != 0 || sim.host.state != JBH_FAILED || !res->have_done ||
            jbp_result_check(res, why, sizeof why) == 0) {
            printf("FAIL: expected a complete session whose result the host refuses (probe r %d, "
                   "host %d: %s)\n",
                   r, sim.host.state, sim.host.why);
            bad = 1;
            goto end;
        }
        printf("refused as expected: %s\n", sim.host.why);
    } else if (r != 0 || sim.host.state != JBH_DONE) {
        printf("FAIL: the session did not complete (%s)\n", sim.host.why);
        bad = 1;
        goto end;
    }
    if (sim.nfaults && sim.host.bad_received + sim.host.cut_received + sim.host.resent +
                               res->done_rx_bad + res->done_resent ==
                           0) {
        printf("FAIL: faults were injected and nothing noticed\n");
        bad = 1;
    }
#ifdef JBP_SIM_PTY
check:
    if (hal_contract(res))
        bad = 1;
#endif
    if (res->head[JBP_H_VSYNC_TIMEOUTS] != 0 ||
        res->head[JBP_H_DMA_TIMEOUTS] != (sim.gpu_fault == G_DMA_STALL && sim.gpu_fault_at == 0
                                              ? res->head[JBP_H_DMA_OPS]
                                              : 0)) {
        printf("FAIL: %u DMA transfers and %u vertical blanks timed out\n",
               (unsigned)res->head[JBP_H_DMA_TIMEOUTS], (unsigned)res->head[JBP_H_VSYNC_TIMEOUTS]);
        bad = 1;
    }
    if (res->head[JBP_H_DMA_OPS] == 0 && sim.gpu_fault == G_DMA_STALL) {
        printf("FAIL: --gpu dma-stall, and the list has no DMA\n");
        bad = 1;
    }


    /* What came back against jitterbug's own player. When the window
       changed VRAM, the probe's VRAM is that of the pass that changed it
       and the player's that of the pass compared: not comparable. */
    if (!ref.steady)
        printf("VRAM: not compared (pass %u changed it)\n", ref.changed_at);
    else {
        unsigned long differ = 0;
        size_t k;
        /* Two blank VRAMs and no reads: nothing would have been compared
           (gp0-e1, a log program, leaves VRAM blank; its reads and checks
           are what it is about). */
        for (k = 0; k < (size_t)JBC_VRAM_PIXELS && ref.vram[k] == 0; ++k)
            ;
        if (k == (size_t)JBC_VRAM_PIXELS && (ref.nreads == 0 || ref.checks == 0)) {
            printf("FAIL: the list leaves VRAM all zero and checks nothing: nothing compared\n");
            bad = 1;
        }
        for (k = 0; k < (size_t)JBC_VRAM_PIXELS; ++k)
            if (res->vram[k] != ref.vram[k]) {
                if (differ < 5)
                    printf("  (%d, %d): probe 0x%04x, jitterbug 0x%04x\n", (int)(k % JBC_VRAM_W),
                           (int)(k / JBC_VRAM_W), res->vram[k], ref.vram[k]);
                ++differ;
            }
        if (differ) {
            printf("FAIL: VRAM: %lu pixels differ (all 16 bits compared)\n", differ);
            bad = 1;
        } else
            printf("VRAM: identical to jitterbug's player, all 16 bits\n");
    }
    if (res->nreads != ref.nreads || res->head[JBP_H_READS] != ref.nreads ||
        (ref.nreads && memcmp(res->reads, ref.reads, ref.nreads * sizeof *ref.reads) != 0)) {
        printf("FAIL: reads: %u from the probe, %u from jitterbug's player, or different values\n",
               (unsigned)res->nreads, ref.nreads);
        bad = 1;
    } else
        printf("reads: %u, identical\n", ref.nreads);
    if (res->head[JBP_H_EXPECTS] != ref.checks || res->head[JBP_H_EXPECTS_FAILED] != ref.checks_failed ||
        res->nexpects != ref.checks) {
        printf("FAIL: checks: probe %u (%u failed), jitterbug's player %u (%u failed)\n",
               (unsigned)res->head[JBP_H_EXPECTS], (unsigned)res->head[JBP_H_EXPECTS_FAILED], ref.checks,
               ref.checks_failed);
        bad = 1;
    } else {
        /* Each check record against the list's own EXPECT ops, in order
           (none is after LOOP: played once): its word, read, mask and
           value, and what the probe saw against jitterbug's read. */
        uint32_t k = 0;
        size_t at;
        int checks_bad = 0;
        for (at = l.first; at < l.n && JBC_OP_CODE(l.w[at]) != JBC_END; at += 1 + JBC_OP_COUNT(l.w[at])) {
            const uint32_t *e;
            if (JBC_OP_CODE(l.w[at]) != JBC_EXPECT)
                continue;
            if (k == res->nexpects) {
                checks_bad = 1;
                printf("FAIL: checks: the list has more EXPECT ops than the probe's %u records\n",
                       (unsigned)res->nexpects);
                break;
            }
            e = res->expects + (size_t)k * JBP_EXPECT_WORDS;
            if (e[0] != at || e[1] != l.w[at + 1] || e[2] != l.w[at + 2] ||
                e[3] != l.w[at + 3] || e[1] >= ref.nreads || e[4] != ref.reads[e[1]]) {
                printf("FAIL: check %u: the probe's record (word %u, read %u, mask 0x%08x, value 0x%x, "
                       "saw 0x%08x) is not the list's EXPECT at word %lu as jitterbug's player read it\n",
                       (unsigned)k, (unsigned)e[0], (unsigned)e[1], (unsigned)e[2], (unsigned)e[3],
                       (unsigned)e[4], (unsigned long)at);
                checks_bad = 1;
                break;
            }
            ++k;
        }
        if (!checks_bad && k != res->nexpects) {
            printf("FAIL: checks: %u records, %u EXPECT ops in the list\n", (unsigned)res->nexpects,
                   (unsigned)k);
            checks_bad = 1;
        }
        if (checks_bad)
            bad = 1;
        else
            printf("checks: %u, %u failed, as jitterbug's player; each record the list's EXPECT (word, "
                   "read, mask, value), what the probe saw jitterbug's read\n",
                   ref.checks, ref.checks_failed);
    }
    if (res->head[JBP_H_WAITS_FAILED] != ref.waits_failed || res->head[JBP_H_WAITS] < ref.waits) {
        printf("FAIL: waits: probe %u (%u timed out), jitterbug's player %u (%u never end)\n",
               (unsigned)res->head[JBP_H_WAITS], (unsigned)res->head[JBP_H_WAITS_FAILED], ref.waits,
               ref.waits_failed);
        bad = 1;
    }
    if (l.loop) {
        const uint32_t *d = res->head;
        unsigned changed = ref.steady ? 0u : ref.changed_at;
        if (d[JBP_H_PASSES] != ref.passes || d[JBP_H_WINDOW] != JBP_WINDOW_DEFAULT ||
            d[JBP_H_PLAYED] != ref.loops + 1 || (int)d[JBP_H_STEADY] != ref.steady ||
            (int)d[JBP_H_EARLY] != ref.early || d[JBP_H_CHANGED_AT] != changed) {
            printf("FAIL: main loop: probe: pass %u, window %u, %u played, steady %u, early %u, "
                   "changed at %u; jitterbug's player: pass %u, window %u, %u played, steady %d, "
                   "early %d, changed at %u\n",
                   (unsigned)d[JBP_H_PASSES], (unsigned)d[JBP_H_WINDOW], (unsigned)d[JBP_H_PLAYED],
                   (unsigned)d[JBP_H_STEADY], (unsigned)d[JBP_H_EARLY], (unsigned)d[JBP_H_CHANGED_AT],
                   ref.passes, (unsigned)JBP_WINDOW_DEFAULT, ref.loops + 1, ref.steady, ref.early,
                   changed);
            bad = 1;
        } else
            printf("main loop: compared after pass %u, %u passes played, %s%s, as jitterbug's "
                   "player\n",
                   ref.passes, ref.loops + 1, ref.steady ? "the window left VRAM unchanged"
                                                          : "a pass of the window changed VRAM",
                   ref.early ? ", the pass compared changed nothing" : "");
    } else if (res->head[JBP_H_PASSES] != 0 || res->head[JBP_H_LOOP] != 0) {
        printf("FAIL: no main loop, and the probe reports one or a pass\n");
        bad = 1;
    }

    if (save != NULL) {
        snprintf(res->note, sizeof res->note, "host simulation (tests/probe/sim.c), virtual time");
        if (round_trip(res, save) != 0)
            bad = 1;
    }
    for (i = 0; i < ntamper; ++i)
        if (save_tampered(res, &l, tamper[i][0], tamper[i][1]) != 0)
            bad = 1;

end:
    printf("%s\n", bad ? "FAIL" : "OK");
    jbh_end(&sim.host);
    jb_destroy(sim.jb);
    free(sim.to_probe.b);
    free(sim.to_host.b);
    free(bytes);
    jbc_outcome_free(&ref);
    jbc_free(&l);
    return bad;
}
