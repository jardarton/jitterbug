/*
 * The host's side of a probe session (jbh_session.h, PROTOCOL.md).
 */
#include "jbh_session.h"

#include <string.h>

enum { PH_START, PH_LIST, PH_RESULTS };

void jbh_defaults(jbh_options *o)
{
    memset(o, 0, sizeof *o);
    o->flags = JBP_FLAGS_DEFAULT;
    o->ack_ms = 3000;
    o->tries = 16;
    o->start_ms = 60000;
    o->quiet_ms = 60000;
}

static void out(jbh_session *s, const uint8_t *p, size_t n)
{
    if (s->o.write != NULL)
        s->o.write(s->o.user, p, n);
}

static void fail(jbh_session *s, const char *why)
{
    if (s->state != JBH_RUNNING)
        return;
    s->state = JBH_FAILED;
    snprintf(s->why, sizeof s->why, "%s", why);
    if (s->o.log != NULL)
        fprintf(s->o.log, "probe session failed: %s\n", why);
}

static void send_plain(jbh_session *s, unsigned type, unsigned seq)
{
    uint8_t f[JBP_HEADER_BYTES + JBP_TRAILER_BYTES];

    out(s, f, jbp_frame(f, type, seq, NULL, 0));
}

static void send_data(jbh_session *s, unsigned type, const uint8_t *p, size_t n, uint32_t now)
{
    s->frame_n = jbp_frame(s->frame, type, s->seq, p, n);
    s->outstanding = 1;
    s->tries = 1;
    s->sent_ms = now;
    out(s, s->frame, s->frame_n);
}

static void resend(jbh_session *s, uint32_t now)
{
    if (s->phase != PH_START && s->tries >= s->o.tries) {
        fail(s, "the probe did not acknowledge a frame");
        return;
    }
    ++s->tries;
    ++s->resent;
    s->sent_ms = now;
    out(s, s->frame, s->frame_n);
}

static void send_chunk(jbh_session *s, uint32_t now)
{
    size_t n = s->list_n - s->next;

    if (n > JBP_LIST_CHUNK)
        n = JBP_LIST_CHUNK;
    send_data(s, JBP_LIST, s->list + s->next, n, now);
}

int jbh_begin(jbh_session *s, const uint8_t *list, size_t n, const jbh_options *o, uint32_t now)
{
    uint8_t p[4 * JBP_START_WORDS];
    uint32_t crc = jbp_crc32(0, list, n);

    memset(s, 0, sizeof *s);
    if ((unsigned long long)n > 0xffffffffull)
        return -1; /* START's length is a word */
    s->o = *o;
    s->list = list;
    s->list_n = n;
    jbp_parser_init(&s->rx);
    if (jbp_result_init(&s->result) != 0)
        return -1;
    jbp_list_name(list, n, s->result.list_name, sizeof s->result.list_name);
    s->result.list_bytes = (uint32_t)n;
    s->result.list_crc = crc;
    s->result.have_host = 1;
    s->state = JBH_RUNNING;
    s->phase = PH_START;
    s->first_ms = s->heard_ms = now;
    jbp_put32(p + 4 * JBP_START_PROTOCOL, JBP_PROTOCOL_VERSION);
    jbp_put32(p + 4 * JBP_START_SESSION, o->session_id);
    jbp_put32(p + 4 * JBP_START_FLAGS, o->flags);
    jbp_put32(p + 4 * JBP_START_LIST_BYTES, (uint32_t)n);
    jbp_put32(p + 4 * JBP_START_LIST_CRC, o->lie_crc ? ~crc : crc);
    jbp_put32(p + 4 * JBP_START_PASSES, o->passes);
    jbp_put32(p + 4 * JBP_START_WINDOW, o->window);
    jbp_put32(p + 4 * JBP_START_WAIT_MS, o->wait_ms);
    send_data(s, JBP_START, p, sizeof p, now);
    return 0;
}

void jbh_end(jbh_session *s)
{
    jbp_result_free(&s->result);
}

static void acked(jbh_session *s, uint32_t now)
{
    s->outstanding = 0;
    s->seq = (s->seq + 1) & 0xffu;
    if (s->phase == PH_START)
        s->phase = PH_LIST;
    else
        s->next += s->frame_n - JBP_HEADER_BYTES - JBP_TRAILER_BYTES;
    if (s->next < s->list_n)
        send_chunk(s, now);
    else {
        s->phase = PH_RESULTS;
        s->heard_ms = now;
        if (s->o.log != NULL)
            fprintf(s->o.log, "list sent (%lu bytes); waiting for the results\n",
                    (unsigned long)s->list_n);
    }
}

/* A data frame from the probe (R_*). Only one applied (the next seq, and
   jbp_result_apply takes it) or acknowledged again (equal to the last one
   applied, type, length and payload) counts as hearing from the probe. A
   frame with the last seq that is not that frame is ignored: neither
   acknowledged nor heard. */
static void probe_data(jbh_session *s, uint32_t now)
{
    jbp_parser *r = &s->rx;

    if (s->phase != PH_RESULTS && r->type != JBP_R_ERROR) {
        /* Results before the last LIST frame was acknowledged (that ACK
           was lost): not taken yet, so the probe sends them again. An
           R_ERROR can come at any time after START. */
        ++s->ignored;
        return;
    }
    if (s->have_last && r->seq == s->rx_last) {
        if (r->type != s->last_type || r->len != s->last_len ||
            jbp_crc32(0, r->payload, r->len) != s->last_crc) {
            ++s->ignored;
            return;
        }
        s->heard_ms = now;
        send_plain(s, JBP_ACK, r->seq); /* our ACK was lost */
        return;
    }
    if (r->seq != s->rx_expect || s->state != JBH_RUNNING) {
        ++s->ignored;
        return;
    }
    if (jbp_result_apply(&s->result, r->type, r->payload, r->len) != 0) {
        fail(s, "the probe sent a malformed result frame");
        return;
    }
    s->heard_ms = now;
    s->rx_last = r->seq;
    s->have_last = 1;
    s->last_type = r->type;
    s->last_len = r->len;
    s->last_crc = jbp_crc32(0, r->payload, r->len);
    s->rx_expect = (r->seq + 1) & 0xffu;
    send_plain(s, JBP_ACK, r->seq);
    if (r->type == JBP_R_VRAM && s->o.log != NULL) {
        uint32_t y = jbp_get32(r->payload);
        if (y % 64 == 0)
            fprintf(s->o.log, "VRAM row %u\n", (unsigned)y);
    }
    if (r->type == JBP_R_HEAD) {
        /* The summary must be of the list sent, byte for byte. */
        const uint32_t *d = s->result.head;
        if (d[JBP_H_LIST_BYTES] != s->result.list_bytes || d[JBP_H_LIST_CRC] != s->result.list_crc) {
            char why[256];
            snprintf(why, sizeof why, "the probe's results are of a list of %u bytes, CRC-32 0x%08x, "
                     "not the %u bytes, CRC-32 0x%08x sent",
                     (unsigned)d[JBP_H_LIST_BYTES], (unsigned)d[JBP_H_LIST_CRC],
                     (unsigned)s->result.list_bytes, (unsigned)s->result.list_crc);
            fail(s, why);
        }
    } else if (r->type == JBP_R_ERROR) {
        char why[256];
        (void)jbp_result_check(&s->result, why, sizeof why);
        fail(s, why);
    } else if (r->type == JBP_R_DONE) {
        char why[256];
        uint32_t window = s->o.window ? s->o.window : JBP_WINDOW_DEFAULT;
        if (jbp_result_check(&s->result, why, sizeof why) != 0)
            fail(s, why);
        else if (s->result.head[JBP_H_PASSES] != s->o.passes ||
                 s->result.head[JBP_H_WINDOW] != window) {
            snprintf(why, sizeof why, "the probe played to pass %u with a window of %u, not %u and %u",
                     (unsigned)s->result.head[JBP_H_PASSES], (unsigned)s->result.head[JBP_H_WINDOW],
                     (unsigned)s->o.passes, (unsigned)window);
            fail(s, why);
        } else
            s->state = JBH_DONE;
    }
}

/* A good frame. Its type, way and size are checked before anything is
   done with it: a frame that is not one the probe sends, or not of its
   type's size, is ignored and does not count as hearing from the probe.
   Nor does a control frame that is not acted on: an ACK that acknowledges
   nothing outstanding, a NAK with nothing to send again. A HELLO after the
   probe took the START says that it has left the session (it is idle):
   the session fails. */
static void frame(jbh_session *s, uint32_t now)
{
    jbp_parser *r = &s->rx;

    switch (r->type) {
    case JBP_ACK:
        if (r->len != 0) {
            ++s->ignored;
            break;
        }
        if (s->outstanding && r->seq == s->seq && s->state == JBH_RUNNING) {
            s->heard_ms = now;
            acked(s, now);
        } else
            ++s->ignored;
        break;
    case JBP_NAK:
        if (r->len != 0) {
            ++s->ignored;
            break;
        }
        ++s->naks_received;
        if (s->outstanding && s->state == JBH_RUNNING) {
            s->heard_ms = now;
            resend(s, now);
        } else
            ++s->ignored;
        break;
    case JBP_HELLO:
        if (r->len != JBP_HELLO_BYTES || jbp_get32(r->payload + 4 * JBP_ID_PROTOCOL) != JBP_PROTOCOL_VERSION) {
            if (r->len == JBP_HELLO_BYTES && s->o.log != NULL && !s->said_protocol) {
                fprintf(s->o.log, "the probe speaks protocol %u, not %u: rebuild it\n",
                        (unsigned)jbp_get32(r->payload + 4 * JBP_ID_PROTOCOL),
                        (unsigned)JBP_PROTOCOL_VERSION);
                s->said_protocol = 1;
            }
            ++s->ignored;
            break;
        }
        if (s->phase != PH_START) {
            /* The probe is idle: it left the session, or it never took
               this START (another host's ACK). Its results cannot come. */
            ++s->ignored;
            fail(s, "the probe is idle (HELLO) in the middle of the session");
            break;
        }
        s->heard_ms = now;
        (void)jbp_result_apply(&s->result, JBP_HELLO, r->payload, r->len);
        /* An idle probe takes a START at once. */
        if (s->phase == PH_START && s->outstanding && s->state == JBH_RUNNING &&
            now - s->sent_ms > 100)
            resend(s, now);
        break;
    case JBP_PROGRESS: {
        uint32_t phase;
        if (r->len != JBP_PROGRESS_BYTES || (phase = jbp_get32(r->payload)) < JBP_P_LIST ||
            phase > JBP_P_READBACK) {
            ++s->ignored;
            break;
        }
        s->heard_ms = now;
        if (s->outstanding)
            s->sent_ms = now; /* busy playing: not a lost frame */
        if (s->o.log != NULL)
            fprintf(s->o.log, "probe busy: %s %u, %u ms\n",
                    phase == JBP_P_LIST ? "list byte" : phase == JBP_P_LOOP ? "main loop pass" : "read-back, pass",
                    (unsigned)jbp_get32(r->payload + 4), (unsigned)jbp_get32(r->payload + 8));
        break;
    }
    default:
        if (r->type >= JBP_R_HEAD && r->type <= JBP_R_IDENT)
            probe_data(s, now);
        else
            ++s->ignored; /* START, LIST (the host's own) or no type at all */
        break;
    }
}

void jbh_input(jbh_session *s, const uint8_t *b, size_t n, uint32_t now)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        int r = jbp_parse(&s->rx, b[i], now);
        if (r == JBP_PARSE_FRAME)
            frame(s, now);
        else if (r == JBP_PARSE_BAD) {
            ++s->bad_received;
            if (s->state != JBH_RUNNING)
                continue;
            if (s->outstanding)
                resend(s, now); /* probably our ACK, damaged */
            else {
                ++s->naks_sent;
                send_plain(s, JBP_NAK, 0);
            }
        }
    }
}

void jbh_poll(jbh_session *s, uint32_t now)
{
    if (jbp_parser_idle(&s->rx, now))
        ++s->cut_received;
    if (s->state != JBH_RUNNING)
        return;
    /* Every limit on every poll, whatever came: a START sent again on
       each HELLO, NAK or damaged frame (resend) must not keep the host
       offering it for ever. */
    if (s->phase == PH_START && now - s->first_ms > s->o.start_ms) {
        fail(s, "no probe took the START");
        return;
    }
    if (s->outstanding && now - s->sent_ms > s->o.ack_ms)
        resend(s, now);
    if (!s->outstanding && s->phase == PH_RESULTS && now - s->heard_ms > s->o.quiet_ms)
        fail(s, "the probe went quiet");
}

uint32_t jbh_deadline(const jbh_session *s)
{
    if (s->phase == PH_START)
        return s->first_ms + s->o.start_ms + 1u;
    if (s->outstanding) {
        /* The sends left, each ack_ms apart, the last one's ack_ms
           included: jbh_poll fails the session then. */
        uint32_t left = s->tries < s->o.tries ? s->o.tries - s->tries : 0u;
        return s->sent_ms + s->o.ack_ms * (left + 1u) + 1u;
    }
    return s->heard_ms + s->o.quiet_ms + 1u;
}

void jbh_abandon(jbh_session *s, const char *why)
{
    fail(s, why);
}
