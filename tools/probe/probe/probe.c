/*
 * The probe (probe.h): the session protocol (PROTOCOL.md) and the list
 * player (tests/conformance/FORMAT.md, "Playing a list"), over hal.h.
 *
 * The list arrives in frames and is played as it arrives, word by word,
 * since a list may be larger than the console's RAM (texture-flip is
 * 2 MB). Only a GP0_DMA op's words (the DMA reads them from RAM) and the
 * main loop (played again after END) are kept. Freestanding C99: no library
 * calls (the console build provides memcpy and memset for the compiler).
 *
 * Fail closed: anything the probe cannot do exactly as the list says (a
 * read-back the GPU never offers, a read it could not keep, a frame the
 * protocol does not allow) ends the session with an R_ERROR, and no
 * R_DONE is sent for it.
 */
#include "probe.h"

#include "hal.h"
#include "jbp_proto.h"

#include "../../../tests/conformance/jbc_format.h"

/* Sizes (the console build's memory map, docs/probe.md, "Memory map"). */
#ifndef PROBE_LOOP_WORDS
#define PROBE_LOOP_WORDS (128u * 1024u) /* 512 KB */
#endif
#ifndef PROBE_DMA_WORDS
#define PROBE_DMA_WORDS (16u * 1024u) /* 64 KB, a multiple of 8 */
#endif

/* Timeouts, ms. Every wait polls in probe.c and sends PROGRESS at most
   T_PROGRESS apart, so no wait, however long the list's --wait-ms, leaves
   the host without word for longer than its ACK interval (3 s). */
enum {
    T_ACK = 1000,        /* for the host's ACK of a data frame */
    T_TRIES = 16,        /* sends of one data frame before giving up */
    T_SESSION = 30000,   /* with no frame from the host acted on; looked at
                            on every turn of session_run, byte or not */
    T_HELLO = 1000,      /* between HELLOs while idle */
    T_PROGRESS = 1000,   /* between PROGRESS frames while busy */
    T_VSYNC = 200        /* for a vertical blank */
};

/* ------------------------------------------------------------------------ */
/* State. One session at a time; probe_serve resets what a session uses. */

static uint32_t ident_words[JBP_ID_WORDS];
static hal_ident ident;
static uint8_t hello[JBP_HELLO_BYTES]; /* HELLO's payload, built once */

static jbp_parser rx;
static uint8_t tx[JBP_MAX_FRAME];
static uint8_t payload[JBP_MAX_PAYLOAD];

static uint32_t loop_buf[PROBE_LOOP_WORDS];
static uint32_t dma_buf[PROBE_DMA_WORDS];
static uint32_t reads[JBP_READS_MAX];
static uint32_t expects[JBP_EXPECTS_MAX][JBP_EXPECT_WORDS];
static uint32_t waitfails[JBP_WAITFAIL_KEPT][JBP_WAITFAIL_WORDS];
static uint32_t strip[JBP_VRAM_ROWS * 512];

typedef struct session {
    uint32_t id, flags, list_bytes, list_crc, passes, window, wait_ms;
    unsigned rx_expect, rx_last; /* the host's next LIST seq, the last LIST taken */
    unsigned tx_seq;
    int have_last;      /* a LIST was taken (rx_last is its seq) */
    uint32_t start[JBP_START_WORDS]; /* the START this session began with */
    uint32_t last_good_ms, last_progress_ms, start_ms;
    uint32_t head[JBP_H_WORDS];
    /* The parser of the list. */
    uint32_t bytes;     /* received */
    uint32_t crc;       /* of the bytes received */
    uint32_t word;      /* being assembled */
    uint32_t word_no;   /* words complete */
    uint32_t header_words;
    uint32_t op, op_at; /* the op being read and its word offset */
    unsigned code;
    uint32_t n, k;      /* its payload words, and those read */
    uint32_t small[3];
    int in_op, recording, replaying, ended, error;
    uint32_t error_at;
    uint32_t loop_n, loop_at;
    uint32_t dma_fill;
    uint32_t nreads, nexpects, nwaitfails;
    uint32_t pass;      /* the main loop's pass being played (FORMAT.md): 1 up to
                           LOOP, then 2, 3, ... for each play of the body */
    int reading_back;   /* in a read-back (PROGRESS phase JBP_P_READBACK) */
    int restart;        /* a START for another session came */
} session;

static session s;

/* ------------------------------------------------------------------------ */
/* Sending. */

static void put_bytes(const uint8_t *p, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; ++i)
        (void)hal_serial_put(p[i]);
}

/* An unacknowledged frame (HELLO, PROGRESS, ACK, NAK). Built in its own
   buffer so that a data frame waiting in tx can still be sent again. */
static void send_plain(unsigned type, unsigned seq, const uint8_t *data, unsigned long n)
{
    static uint8_t f[JBP_HEADER_BYTES + JBP_HELLO_BYTES + JBP_TRAILER_BYTES];

    put_bytes(f, jbp_frame(f, type, seq, data, n));
}

static void send_ack(unsigned seq)
{
    send_plain(JBP_ACK, seq, 0, 0);
}

/* Called from every loop that waits for the machine: PROGRESS at most
   every T_PROGRESS ms, so that the host does not send its frame again. */
static void busy(void)
{
    uint8_t p[JBP_PROGRESS_BYTES];
    uint32_t now = hal_ms();
    unsigned phase = s.reading_back ? JBP_P_READBACK : s.replaying ? JBP_P_LOOP : JBP_P_LIST;

    if (now - s.last_progress_ms < T_PROGRESS)
        return;
    s.last_progress_ms = now;
    jbp_put32(p, phase);
    jbp_put32(p + 4, phase == JBP_P_LIST ? s.bytes : s.pass);
    jbp_put32(p + 8, now - s.start_ms);
    send_plain(JBP_PROGRESS, 0, p, sizeof p);
}

static void handle_host_frame(void);

/* A data frame, sent until the host acknowledges it: 0, or -1 when it
   never did (or a new session started). Only an ACK with no payload and
   this frame's seq counts; a NAK must have no payload either. */
static int send_data(unsigned type, const uint8_t *data, unsigned long n)
{
    unsigned long len = jbp_frame(tx, type, s.tx_seq, data, n);
    unsigned tries;

    for (tries = 0; tries < T_TRIES; ++tries) {
        uint32_t t0;
        if (tries)
            ++s.head[JBP_H_TX_RESENT];
        put_bytes(tx, len);
        t0 = hal_ms();
        while (hal_ms() - t0 < T_ACK) {
            int c = hal_serial_get(10), r;
            if (c < 0) {
                if (jbp_parser_idle(&rx, hal_ms()))
                    ++s.head[JBP_H_RX_BAD];
                continue;
            }
            r = jbp_parse(&rx, (uint8_t)c, hal_ms());
            if (r == JBP_PARSE_BAD) {
                ++s.head[JBP_H_RX_BAD];
                break; /* most likely our ACK, damaged: send again */
            }
            if (r != JBP_PARSE_FRAME)
                continue;
            if (rx.type == JBP_ACK || rx.type == JBP_NAK) {
                if (rx.len != 0)
                    continue; /* not a frame the protocol has */
                if (rx.type == JBP_NAK)
                    break;
                if (rx.seq == (s.tx_seq & 0xffu)) {
                    s.last_good_ms = hal_ms();
                    s.tx_seq = (s.tx_seq + 1) & 0xffu;
                    return 0;
                }
                continue;
            }
            handle_host_frame();
            if (s.restart)
                return -1;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* Playing. */

static void fail(unsigned code)
{
    if (!s.error) {
        s.error = (int)code;
        s.error_at = s.op_at;
    }
}

static void note_waitfail(uint32_t mask, uint32_t value, uint32_t stat)
{
    ++s.head[JBP_H_WAITS_FAILED];
    if (s.nwaitfails < JBP_WAITFAIL_KEPT) {
        uint32_t *w = waitfails[s.nwaitfails++];
        w[0] = s.op_at;
        w[1] = mask;
        w[2] = value;
        w[3] = stat;
        w[4] = s.pass;
    }
}

/* Polls GPUSTAT until (stat & mask) == value: 1, or 0 after ms. */
static int poll_stat(uint32_t mask, uint32_t value, uint32_t ms, uint32_t *last)
{
    uint32_t t0 = hal_ms(), stat;

    for (;;) {
        stat = hal_gpustat();
        if ((stat & mask) == value) {
            *last = stat;
            return 1;
        }
        if (hal_ms() - t0 >= ms) {
            *last = stat;
            return 0;
        }
        busy();
    }
}

static void play_wait(uint32_t mask, uint32_t value)
{
    uint32_t stat;

    ++s.head[JBP_H_WAITS];
    if (!poll_stat(mask, value, s.wait_ms, &stat))
        note_waitfail(mask, value, stat);
}

/* The SDK's DrawSync(0) (FORMAT.md): with DMA direction 0, GPUSTAT bit 28;
   otherwise the DMA's end (every DMA here has ended before the next op)
   and bits 26 and 28. */
static void play_drawsync(void)
{
    uint32_t need = (hal_gpustat() >> 29 & 3u) ? 0x14000000u : 0x10000000u;

    play_wait(need, need);
}

static void dma_flush(void)
{
    uint32_t t0;

    if (s.dma_fill == 0)
        return;
    ++s.head[JBP_H_DMA_OPS];
    hal_gp0_dma_start(dma_buf, s.dma_fill);
    t0 = hal_ms();
    while (hal_gp0_dma_busy()) {
        if (hal_ms() - t0 >= s.wait_ms) {
            hal_gp0_dma_stop();
            ++s.head[JBP_H_DMA_TIMEOUTS];
            break;
        }
        busy();
    }
    s.dma_fill = 0;
}

static void play_vsync(void)
{
    uint32_t t0;

    ++s.head[JBP_H_VSYNCS];
    hal_vblank_clear();
    t0 = hal_ms();
    while (!hal_vblank_seen()) {
        if (hal_ms() - t0 >= T_VSYNC) {
            ++s.head[JBP_H_VSYNC_TIMEOUTS];
            return;
        }
        busy();
    }
}

static void play_read(uint32_t port, uint32_t count)
{
    uint32_t i;

    /* Every word read is kept and sent: a READ that would not fit ends the
       session before it reads anything. */
    if (count > JBP_READS_MAX - s.nreads) {
        fail(JBP_E_READS);
        return;
    }
    for (i = 0; i < count; ++i)
        reads[s.nreads++] = port == JBC_PORT_GPUSTAT ? hal_gpustat() : hal_gpuread();
}

static void play_expect(void)
{
    uint32_t idx = s.small[0], mask = s.small[1], value = s.small[2], got;
    uint32_t *e;

    if (idx >= s.nreads) {
        fail(JBP_E_OP);
        return;
    }
    if (s.nexpects == JBP_EXPECTS_MAX) {
        fail(JBP_E_EXPECTS);
        return;
    }
    got = reads[idx]; /* idx < nreads <= JBP_READS_MAX: kept */
    ++s.head[JBP_H_EXPECTS];
    if ((got & mask) != value)
        ++s.head[JBP_H_EXPECTS_FAILED];
    e = expects[s.nexpects++];
    e[0] = s.op_at;
    e[1] = idx;
    e[2] = mask;
    e[3] = value;
    e[4] = got;
}

/* Called when an op's payload is complete (or at once when it has none). */
static void op_done(void)
{
    s.in_op = 0;
    if (s.recording)
        return;
    switch (s.code) {
    case JBC_GP0_DMA:
        dma_flush();
        break;
    case JBC_READ:
        if (s.small[0] > JBC_PORT_GPUSTAT || s.small[1] == 0)
            fail(JBP_E_OP);
        else
            play_read(s.small[0], s.small[1]);
        break;
    case JBC_WAIT:
        play_wait(s.small[0], s.small[1]);
        break;
    case JBC_DRAWSYNC:
        play_drawsync();
        break;
    case JBC_VSYNC:
        play_vsync();
        break;
    case JBC_EXPECT:
        play_expect();
        break;
    default:
        break;
    }
}

static void op_word(uint32_t w)
{
    unsigned code = JBC_OP_CODE(w);
    uint32_t n = (uint32_t)JBC_OP_COUNT(w);
    int ok;

    s.op = w;
    s.op_at = s.word_no;
    s.code = code;
    s.n = n;
    s.k = 0;
    if (!s.replaying)
        ++s.head[JBP_H_OPS];
    switch (code) {
    case JBC_END:
    case JBC_DRAWSYNC:
    case JBC_VSYNC:
    case JBC_LOOP:
        ok = n == 0;
        break;
    case JBC_READ:
    case JBC_WAIT:
        ok = n == 2;
        break;
    case JBC_EXPECT:
        ok = n >= 4;
        break;
    case JBC_GP0:
    case JBC_GP0_DMA:
    case JBC_GP1:
    case JBC_CALL:
        ok = n >= 1;
        break;
    default:
        ok = 0;
        break;
    }
    if (!ok || (s.recording && (code == JBC_READ || code == JBC_EXPECT || code == JBC_LOOP))) {
        fail(JBP_E_OP);
        return;
    }
    if (code == JBC_END) {
        s.ended = 1;
        return;
    }
    if (code == JBC_LOOP) {
        /* From here to END is the main loop: kept, and played after END. */
        s.recording = 1;
        s.loop_n = 0;
        s.loop_at = s.word_no + 1;
        s.head[JBP_H_LOOP] = 1;
        return;
    }
    if (code == JBC_GP0_DMA) {
        s.dma_fill = 0;
    }
    s.in_op = 1;
    if (n == 0)
        op_done();
}

static void payload_word(uint32_t w)
{
    if (s.k < 3)
        s.small[s.k] = w;
    ++s.k;
    if (!s.recording) {
        switch (s.code) {
        case JBC_GP0:
            hal_gp0(w);
            break;
        case JBC_GP1:
            hal_gp1(w);
            break;
        case JBC_GP0_DMA:
            dma_buf[s.dma_fill++] = w;
            /* A full buffer goes now; in blocks it is a multiple of 8,
               so the next part is too. */
            if (s.dma_fill == PROBE_DMA_WORDS && s.k < s.n)
                dma_flush();
            break;
        default:
            break;
        }
    }
    if (s.k == s.n)
        op_done();
}

/* One word of the list, in order (header included), received or replayed. */
static void list_word(uint32_t w)
{
    if (s.error)
        return;
    if (!s.replaying) {
        /* The header: magic, version, its own length (FORMAT.md). */
        if (s.word_no < 4 || s.word_no < s.header_words) {
            if ((s.word_no == 0 && w != 0x4c43424au) || (s.word_no == 1 && w != 0x00545349u))
                fail(JBP_E_MAGIC);
            else if (s.word_no == 2 && w != JBC_VERSION)
                fail(JBP_E_VERSION);
            else if (s.word_no == 3) {
                if (w % 4 != 0 || w / 4 < 6)
                    fail(JBP_E_MAGIC);
                s.header_words = w / 4;
            }
            ++s.word_no;
            return;
        }
        if (s.recording && !(!s.in_op && JBC_OP_CODE(w) == JBC_END)) {
            if (s.loop_n == PROBE_LOOP_WORDS) {
                fail(JBP_E_LOOP_FULL);
                return;
            }
            loop_buf[s.loop_n++] = w;
        }
    }
    if (s.in_op)
        payload_word(w);
    else
        op_word(w);
    if (!s.replaying)
        ++s.word_no;
}

/* Bytes of the list as they come. The list ends with END exactly at the
   length START gave: a byte after END, or the last byte without END, is a
   length error at once. */
static void list_bytes(const uint8_t *p, unsigned long n)
{
    unsigned long i;

    s.crc = jbp_crc32(s.crc, p, n);
    for (i = 0; i < n && !s.error; ++i) {
        if (s.ended || s.bytes == s.list_bytes) {
            fail(JBP_E_LENGTH); /* past END, or past the length */
            break;
        }
        s.word |= (uint32_t)p[i] << (8 * (s.bytes % 4));
        ++s.bytes;
        if (s.bytes % 4 == 0) {
            list_word(s.word);
            s.word = 0;
        }
    }
    s.bytes += (uint32_t)(n - i); /* what was not taken, for the error report */
    if (!s.error && s.bytes == s.list_bytes && !s.ended)
        fail(JBP_E_LENGTH); /* the declared end leaves the list unfinished */
}

/* ------------------------------------------------------------------------ */
/* VRAM read-back (psx-spx, VRAM to CPU blitting): the GPU ready for a
   command (GPUSTAT bits 26 and 28); GP0(C0h), its position and size; wait
   for GPUSTAT bit 27; then w * h / 2 words from GPUREAD, two pixels a word,
   the first in the low half (1024 wide: no padding). Either wait timing
   out ends the session: what GPUREAD gives then is not VRAM. */

static int gpu_ready(void)
{
    uint32_t stat;

    return poll_stat(0x14000000u, 0x14000000u, JBP_READY_MS, &stat);
}

/* 0, or -1 with the session failed. */
static int read_rows(uint32_t y, uint32_t rows)
{
    uint32_t i, stat, n = rows * 512u;

    s.reading_back = 1;
    if (!gpu_ready()) {
        fail(JBP_E_NOT_IDLE);
        s.reading_back = 0;
        return -1;
    }
    hal_gp0(0xc0000000u);
    hal_gp0(y << 16);
    hal_gp0(rows << 16 | 1024u);
    if (!poll_stat(0x08000000u, 0x08000000u, JBP_READY_MS, &stat)) {
        fail(JBP_E_READBACK);
        s.reading_back = 0;
        return -1;
    }
    for (i = 0; i < n; ++i)
        strip[i] = hal_gpuread();
    s.reading_back = 0;
    return 0;
}

static uint32_t strip_crc(uint32_t crc, uint32_t rows)
{
    uint32_t i;
    uint8_t b[4];

    for (i = 0; i < rows * 512u; ++i) {
        jbp_put32(b, strip[i]);
        crc = jbp_crc32(crc, b, 4);
    }
    return crc;
}

/* The CRC-32 of all of VRAM read back: 0, or -1 with the session failed. */
static int vram_crc(uint32_t *crc)
{
    uint32_t y;

    *crc = 0;
    for (y = 0; y < 512; y += JBP_VRAM_ROWS) {
        if (read_rows(y, JBP_VRAM_ROWS) != 0)
            return -1;
        *crc = strip_crc(*crc, JBP_VRAM_ROWS);
        busy();
    }
    return 0;
}

/* After pass s.pass of the main loop: the read-backs the rule needs
   (FORMAT.md, "Playing a list"). VRAM is read back, and its CRC-32 taken,
   after pass P - 1 (when P > 1), after P, and after each pass of the window;
   no earlier pass is read back. 1 when a pass of the window changed VRAM
   (the loop stops there) or the read-back failed, else 0. */
static int checkpoint(void)
{
    uint32_t p = s.passes, crc;

    if (s.pass + 1 < p)
        return 0;
    if (vram_crc(&crc) != 0)
        return 1;
    s.head[JBP_H_CRC_LAST] = crc;
    if (s.pass + 1 == p)
        s.head[JBP_H_CRC_BEFORE] = crc;
    else if (s.pass == p) {
        s.head[JBP_H_CRC_AT] = crc;
        s.head[JBP_H_EARLY] = p > 1 && crc == s.head[JBP_H_CRC_BEFORE];
    } else if (crc != s.head[JBP_H_CRC_AT]) {
        s.head[JBP_H_STEADY] = 0;
        s.head[JBP_H_CHANGED_AT] = s.pass;
        return 1;
    }
    return 0;
}

/* The main loop, as the conformance runner plays it (FORMAT.md, "Playing a
   list"): pass 1 is everything up to LOOP, already played; the body is
   played again for passes 2 to P, then for the W passes of the window,
   which must leave VRAM as it was after pass P. The window ends at the
   first pass that changes VRAM. */
static void play_loop(void)
{
    uint32_t i, last = s.passes + s.window;
    uint32_t t0 = hal_ms();

    s.head[JBP_H_STEADY] = 1;
    s.replaying = 1;
    s.recording = 0;
    if (!checkpoint()) {
        while (s.pass < last) {
            ++s.pass;
            s.in_op = 0;
            for (i = 0; i < s.loop_n && !s.error; ++i) {
                s.word_no = s.loop_at + i;
                list_word(loop_buf[i]);
            }
            dma_flush();
            if (s.error)
                break;
            busy();
            if (checkpoint())
                break;
        }
    }
    s.replaying = 0;
    s.head[JBP_H_PLAYED] = s.pass;
    s.head[JBP_H_LOOP_MS] = hal_ms() - t0;
}

/* ------------------------------------------------------------------------ */
/* Results. */

static int send_words(unsigned type, const uint32_t *w, uint32_t n_items, uint32_t item_words)
{
    uint32_t per = (JBP_MAX_PAYLOAD - 4) / (4 * item_words), first, i;

    for (first = 0; first < n_items; first += per) {
        uint32_t m = n_items - first < per ? n_items - first : per;
        jbp_put32(payload, first);
        for (i = 0; i < m * item_words; ++i)
            jbp_put32(payload + 4 + 4 * i, w[first * item_words + i]);
        if (send_data(type, payload, 4 + 4 * m * item_words) != 0)
            return -1;
    }
    return 0;
}

static void send_error(void)
{
    jbp_put32(payload, (uint32_t)s.error);
    jbp_put32(payload + 4, s.error_at);
    jbp_put32(payload + 8, s.bytes);
    (void)send_data(JBP_R_ERROR, payload, JBP_ERROR_BYTES);
}

/* 0 when R_DONE was acknowledged, else -1 (an R_ERROR was sent when a
   read-back failed). */
static int send_results(void)
{
    uint32_t i, y, crc = 0;

    s.head[JBP_H_READS] = s.nreads;
    s.head[JBP_H_READS_KEPT] = s.nreads;
    s.head[JBP_H_SIO_ERRORS] = hal_serial_errors();
    /* The identification first: the host may have missed every HELLO. */
    if (send_data(JBP_R_IDENT, hello, sizeof hello) != 0)
        return -1;
    for (i = 0; i < JBP_H_WORDS; ++i)
        jbp_put32(payload + 4 * i, s.head[i]);
    if (send_data(JBP_R_HEAD, payload, 4 * JBP_H_WORDS) != 0)
        return -1;
    if (send_words(JBP_R_READS, reads, s.nreads, 1) != 0 ||
        send_words(JBP_R_EXPECTS, &expects[0][0], s.nexpects, JBP_EXPECT_WORDS) != 0 ||
        send_words(JBP_R_WAITS, &waitfails[0][0], s.nwaitfails, JBP_WAITFAIL_WORDS) != 0)
        return -1;
    for (y = 0; y < 512; y += JBP_VRAM_ROWS) {
        if (read_rows(y, JBP_VRAM_ROWS) != 0) {
            send_error();
            return -1;
        }
        crc = strip_crc(crc, JBP_VRAM_ROWS);
        jbp_put32(payload, y);
        jbp_put32(payload + 4, JBP_VRAM_ROWS);
        for (i = 0; i < JBP_VRAM_ROWS * 512u; ++i)
            jbp_put32(payload + 8 + 4 * i, strip[i]);
        if (send_data(JBP_R_VRAM, payload, 8 + JBP_VRAM_ROWS * 2048u) != 0)
            return -1;
    }
    /* The line's counts again: the summary went before the VRAM did. */
    jbp_put32(payload, crc);
    jbp_put32(payload + 4, 512 / JBP_VRAM_ROWS);
    jbp_put32(payload + 8, s.head[JBP_H_TX_RESENT]);
    jbp_put32(payload + 12, s.head[JBP_H_RX_BAD]);
    jbp_put32(payload + 16, hal_serial_errors());
    return send_data(JBP_R_DONE, payload, JBP_DONE_BYTES);
}

/* ------------------------------------------------------------------------ */
/* Sessions. */

static int prepare(void);

/* The START in rx checked completely (PROTOCOL.md, "Session flow", 2): 0
   when it may begin a session, else the JBP_E_* code it is refused with.
   Nothing is done with a START before this. */
static unsigned start_check(void)
{
    uint32_t passes, window, bytes;

    if (rx.len >= 4 && jbp_get32(rx.payload) != JBP_PROTOCOL_VERSION)
        return JBP_E_PROTOCOL;
    if (rx.len != 4u * JBP_START_WORDS || rx.seq != 0)
        return JBP_E_FRAME;
    passes = jbp_get32(rx.payload + 4 * JBP_START_PASSES);
    window = jbp_get32(rx.payload + 4 * JBP_START_WINDOW);
    bytes = jbp_get32(rx.payload + 4 * JBP_START_LIST_BYTES);
    if (passes > JBP_PASSES_MAX || window > JBP_PASSES_MAX)
        return JBP_E_PASSES;
    if (bytes % 4 != 0 || bytes < JBP_LIST_BYTES_MIN)
        return JBP_E_LENGTH;
    return JBP_E_NONE;
}

/* A START, as received (in rx), taken while idle: a new session. One that
   start_check refuses fails the session at once, before the GPU is
   touched; a preparation that times out fails it before any word of the
   list is played (JBP_E_PREPARE). */
static void start_session(void)
{
    uint32_t w[JBP_START_WORDS];
    unsigned i, seq = rx.seq, code = start_check();

    for (i = 0; i < JBP_START_WORDS; ++i)
        w[i] = code == JBP_E_NONE ? jbp_get32(rx.payload + 4 * i) : 0;
    /* Everything a session uses starts again (the parser's counts too). */
    {
        unsigned char *p = (unsigned char *)&s;
        unsigned long k;
        for (k = 0; k < sizeof s; ++k)
            p[k] = 0;
    }
    s.id = w[JBP_START_SESSION];
    s.flags = w[JBP_START_FLAGS];
    s.list_bytes = w[JBP_START_LIST_BYTES];
    s.list_crc = w[JBP_START_LIST_CRC];
    s.passes = w[JBP_START_PASSES];
    s.window = w[JBP_START_WINDOW] ? w[JBP_START_WINDOW] : JBP_WINDOW_DEFAULT;
    s.pass = 1;
    s.head[JBP_H_PASSES] = s.passes;
    s.head[JBP_H_WINDOW] = s.window;
    s.wait_ms = w[JBP_START_WAIT_MS] ? w[JBP_START_WAIT_MS] : 1000;
    for (i = 0; i < JBP_START_WORDS; ++i)
        s.start[i] = w[i];
    s.rx_expect = (seq + 1) & 0xffu; /* the first LIST; no LIST taken yet */
    s.start_ms = s.last_good_ms = s.last_progress_ms = hal_ms();
    if (code != JBP_E_NONE)
        s.error = (int)code;
    else if (prepare() != 0)
        s.error = JBP_E_PREPARE;
    send_ack(seq); /* after preparing: the host sends the list once it has it */
}

/* A good frame from the host that is not an ACK or NAK for us, in a
   session. Each is checked before anything is done with it, and only one
   acted on (a LIST taken or acknowledged again, a START acknowledged
   again or begun) counts as hearing from the host:
   - a START that start_check refuses is ignored (the host's own START is
     never such a one; refused at once when the probe is idle);
   - a START word for word the session's own, before any list byte came,
     is the host's START again (our ACK was lost): acknowledged again;
   - any other START begins a new session (s.restart);
   - a LIST of the wrong size ends the session (JBP_E_FRAME) until the
     list is whole, and is ignored after;
   - a LIST with the seq of the last LIST taken is acknowledged again, one
     with the next seq is taken, any other is ignored. */
static void handle_host_frame(void)
{
    if (rx.type == JBP_START) {
        unsigned i;
        int same = 1;
        if (start_check() != JBP_E_NONE)
            return;
        for (i = 0; i < JBP_START_WORDS; ++i)
            same &= jbp_get32(rx.payload + 4 * i) == s.start[i];
        s.last_good_ms = hal_ms();
        if (same && s.bytes == 0 && !s.have_last)
            send_ack(0); /* our ACK was lost */
        else
            s.restart = 1;
        return;
    }
    if (rx.type != JBP_LIST)
        return;
    if (rx.len < JBP_LIST_MIN || rx.len > JBP_LIST_CHUNK) {
        if (!s.ended)
            fail(JBP_E_FRAME); /* after END the list is whole: not taken */
        return;
    }
    if (s.have_last && rx.seq == s.rx_last) {
        s.last_good_ms = hal_ms();
        send_ack(rx.seq);
        return;
    }
    if (rx.seq != s.rx_expect || s.ended || s.error)
        return;
    s.last_good_ms = hal_ms();
    list_bytes(rx.payload, rx.len);
    s.rx_last = rx.seq;
    s.have_last = 1;
    s.rx_expect = (rx.seq + 1) & 0xffu;
    send_ack(rx.seq);
}

/* Before the list (START's flags): the GPU reset, VRAM filled with 0, and
   in every case the GPU ready for a command (GPUSTAT bits 26 and 28) before
   the fills and after them, so that the list's first word goes to a GPU
   that takes it and the VRAM it starts from is the one promised. 0, or -1
   when the GPU was not ready within JBP_READY_MS (the session fails with
   JBP_E_PREPARE before any word of the list is played). */
static int prepare(void)
{
    if (s.flags & JBP_FLAG_RESET) {
        hal_gp1(0x00000000u);
        hal_gp1(0x01000000u);
        hal_gp1(0x09000000u);
    }
    if (!gpu_ready())
        return -1;
    if (s.flags & JBP_FLAG_CLEAR) {
        /* GP0(02h) with width 3FFh fills 1024 pixels; height at most 511,
           so two fills of 256 rows (psx-spx, Masking and Rounding for FILL
           Command parameters). Bit 15 is written 0. */
        hal_gp0(0x02000000u);
        hal_gp0(0x00000000u);
        hal_gp0(0x010003ffu);
        hal_gp0(0x02000000u);
        hal_gp0(0x01000000u);
        hal_gp0(0x010003ffu);
        if (!gpu_ready())
            return -1;
    }
    return 0;
}

/* After the list: what the read-back needs, whatever the list left. 0, or
   -1 when the GPU is not ready for a command (the session fails: a packet
   left unfinished, which the host's player would not have sent on). */
static int restore(void)
{
    uint32_t r = JBP_RESTORE_DMA_OFF | JBP_RESTORE_VRAM_SIZE;

    s.head[JBP_H_GPUSTAT_END] = hal_gpustat();
    s.reading_back = 1;
    if (!gpu_ready()) {
        s.reading_back = 0;
        fail(JBP_E_NOT_IDLE);
        return -1;
    }
    s.reading_back = 0;
    hal_gp1(0x04000000u);
    hal_gp1(0x09000000u);
    s.head[JBP_H_RESTORE] = r;
    s.head[JBP_H_GPUSTAT_READ] = hal_gpustat();
    return 0;
}

static int session_run(void)
{
    while (!s.ended && !s.error) {
        int c, r;
        /* On every turn, byte or no byte: a host that never lets the line
           go idle, with frames that are not acted on, must not hold the
           session open. */
        if (hal_ms() - s.last_good_ms > T_SESSION) {
            fail(JBP_E_TIMEOUT);
            break;
        }
        c = hal_serial_get(100);
        if (c < 0) {
            if (jbp_parser_idle(&rx, hal_ms()))
                ++s.head[JBP_H_RX_BAD];
            continue;
        }
        r = jbp_parse(&rx, (uint8_t)c, hal_ms());
        if (r == JBP_PARSE_BAD) {
            ++s.head[JBP_H_RX_BAD];
            send_plain(JBP_NAK, 0, 0, 0);
        } else if (r == JBP_PARSE_FRAME) {
            handle_host_frame();
            if (s.restart)
                return -1;
        }
    }
    if (!s.error && (s.bytes != s.list_bytes))
        fail(JBP_E_LENGTH);
    if (!s.error && s.crc != s.list_crc)
        fail(JBP_E_CRC);
    /* A main loop needs its pass P; a list without one has none. */
    if (!s.error && (s.head[JBP_H_LOOP] != 0) != (s.passes != 0))
        fail(JBP_E_PASSES);
    s.head[JBP_H_LIST_BYTES] = s.bytes;
    s.head[JBP_H_LIST_CRC] = s.crc;
    s.head[JBP_H_LIST_MS] = hal_ms() - s.start_ms;
    if (!s.error && s.head[JBP_H_LOOP])
        play_loop();
    if (!s.error)
        (void)restore();
    if (s.error) {
        s.head[JBP_H_STATUS] = (uint32_t)s.error;
        send_error();
        return 1;
    }
    return send_results() == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------ */

static void hello_payload(void);

void probe_init(void)
{
    uint32_t stat, v;
    unsigned i;

    {
        unsigned char *p = (unsigned char *)&s;
        unsigned long k;
        for (k = 0; k < sizeof s; ++k)
            p[k] = 0;
    }
    jbp_parser_init(&rx);
    hal_identify(&ident);
    stat = hal_gpustat();
    ident_words[JBP_ID_PROTOCOL] = JBP_PROTOCOL_VERSION;
    ident_words[JBP_ID_PLATFORM] = ident.platform;
    ident_words[JBP_ID_CPU_PRID] = ident.cpu_prid;
    ident_words[JBP_ID_GPUSTAT_BOOT] = stat;
    /* psx-spx, GPU Detection: index 4 latched first, so that a v0 GPU
       (which leaves the latch alone for index 7) cannot read 2. */
    hal_gp1(0x10000004u);
    hal_gp1(0x10000007u);
    v = hal_gpuread();
    ident_words[JBP_ID_GP1_10_7] = v;
    if ((v & 0x00ffffffu) == 2) {
        ident_words[JBP_ID_GPU_VERSION] = 2;
    } else {
        /* GP0(E1h) bit 12 shows in GPUSTAT on v1 only. */
        hal_gp0((stat & 0x3fffu) | 0xe1001000u);
        (void)hal_gpuread();
        ident_words[JBP_ID_GPUSTAT_E1] = hal_gpustat();
        ident_words[JBP_ID_GPU_VERSION] = (ident_words[JBP_ID_GPUSTAT_E1] & 0x1000u) ? 1 : 0;
    }
    {
        static const unsigned idx[6] = {2, 3, 4, 5, 7, 8};
        for (i = 0; i < 6; ++i) {
            hal_gp1(0x10000000u | idx[i]);
            ident_words[JBP_ID_GP1_10_2 + i] = hal_gpuread();
        }
    }
    ident_words[JBP_ID_BIOS_DATE] = ident.bios_date;
    ident_words[JBP_ID_CONSOLE_TYPE] = ident.console_type;
    ident_words[JBP_ID_SIO_FACTOR] = ident.sio_factor;
    ident_words[JBP_ID_SIO_RELOAD] = ident.sio_reload;
    ident_words[JBP_ID_LOOP_CAP] = PROBE_LOOP_WORDS;
    ident_words[JBP_ID_READ_CAP] = JBP_READS_MAX;
    hello_payload();
    hal_show(HAL_SHOW_IDLE);
}

static void hello_payload(void)
{
    uint8_t *h = hello;
    unsigned i;

    for (i = 0; i < JBP_ID_WORDS; ++i)
        jbp_put32(h + 4 * i, ident_words[i]);
    for (i = 0; i < JBP_ID_MAKER_BYTES; ++i)
        h[4 * JBP_ID_WORDS + i] = (uint8_t)ident.maker[i];
    for (i = 0; i < JBP_ID_VERSION_BYTES; ++i)
        h[4 * JBP_ID_WORDS + JBP_ID_MAKER_BYTES + i] = (uint8_t)ident.version[i];
}

static void send_hello(void)
{
    send_plain(JBP_HELLO, 0, hello, sizeof hello);
}

int probe_serve(uint32_t idle_ms)
{
    uint32_t t0 = hal_ms(), last_hello = t0 - T_HELLO;
    int pending = s.restart; /* a START that ended the last session */

    for (;;) {
        int c, r;
        if (pending) {
            pending = 0;
            break;
        }
        if (hal_ms() - last_hello >= T_HELLO) {
            send_hello();
            last_hello = hal_ms();
        }
        if (idle_ms && hal_ms() - t0 >= idle_ms)
            return -1;
        c = hal_serial_get(50);
        if (c < 0) {
            (void)jbp_parser_idle(&rx, hal_ms());
            continue;
        }
        r = jbp_parse(&rx, (uint8_t)c, hal_ms());
        if (r == JBP_PARSE_FRAME && rx.type == JBP_START)
            break; /* any START: start_session refuses a malformed one */
    }
    for (;;) {
        int r;
        start_session();
        r = session_run();
        if (r >= 0) {
            hal_show(r == 0 ? HAL_SHOW_IDLE : HAL_SHOW_ERROR);
            return r;
        }
        /* Another session's START came: it is in rx; begin it. */
    }
}
