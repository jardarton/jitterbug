/*
 * Frames and CRC-32 (jbp_proto.h, PROTOCOL.md). Freestanding C99.
 */
#include "jbp_proto.h"

/* CRC-32, polynomial EDB88320h (reflected), four bits at a time. */
static const uint32_t crc_nibble[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
    0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu};

uint32_t jbp_crc32(uint32_t crc, const void *data, size_t n)
{
    const uint8_t *p = data;
    size_t i;

    crc = ~crc;
    for (i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
        crc = (crc >> 4) ^ crc_nibble[crc & 15];
    }
    return ~crc;
}

void jbp_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t jbp_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t jbp_frame(uint8_t *out, unsigned type, unsigned seq, const void *payload, size_t n)
{
    const uint8_t *src = payload;
    size_t i;
    uint32_t crc;

    out[0] = JBP_SYNC0;
    out[1] = JBP_SYNC1;
    out[2] = (uint8_t)type;
    out[3] = (uint8_t)seq;
    out[4] = (uint8_t)n;
    out[5] = (uint8_t)(n >> 8);
    for (i = 0; i < n; ++i)
        out[JBP_HEADER_BYTES + i] = src[i];
    crc = jbp_crc32(0, out + 2, JBP_HEADER_BYTES - 2 + n);
    jbp_put32(out + JBP_HEADER_BYTES + n, crc);
    return JBP_HEADER_BYTES + n + JBP_TRAILER_BYTES;
}

enum { S_SYNC0, S_SYNC1, S_HEAD, S_PAYLOAD, S_CRC };

void jbp_parser_init(jbp_parser *p)
{
    p->state = S_SYNC0;
    p->got = 0;
    p->len = 0;
    p->bad = 0;
    p->cut = 0;
    p->last_ms = 0;
}

int jbp_parser_idle(jbp_parser *p, uint32_t now_ms)
{
    if (p->state != S_SYNC0 && now_ms - p->last_ms > JBP_BYTE_GAP_MS) {
        p->state = S_SYNC0;
        ++p->cut;
        return 1;
    }
    return 0;
}

int jbp_parse(jbp_parser *p, uint8_t b, uint32_t now_ms)
{
    jbp_parser_idle(p, now_ms);
    p->last_ms = now_ms;
    switch (p->state) {
    case S_SYNC0:
        if (b == JBP_SYNC0)
            p->state = S_SYNC1;
        return JBP_PARSE_NONE;
    case S_SYNC1:
        /* "JJB" is a frame starting at the second J. */
        p->state = b == JBP_SYNC1 ? S_HEAD : b == JBP_SYNC0 ? S_SYNC1 : S_SYNC0;
        p->got = 2;
        p->head[0] = JBP_SYNC0;
        p->head[1] = JBP_SYNC1;
        return JBP_PARSE_NONE;
    case S_HEAD:
        p->head[p->got++] = b;
        if (p->got < JBP_HEADER_BYTES)
            return JBP_PARSE_NONE;
        p->type = p->head[2];
        p->seq = p->head[3];
        p->len = (size_t)p->head[4] | (size_t)p->head[5] << 8;
        if (p->len > JBP_MAX_PAYLOAD) {
            ++p->bad;
            p->state = S_SYNC0;
            return JBP_PARSE_BAD;
        }
        p->got = 0;
        p->state = p->len ? S_PAYLOAD : S_CRC;
        return JBP_PARSE_NONE;
    case S_PAYLOAD:
        p->payload[p->got++] = b;
        if (p->got == p->len) {
            p->got = 0;
            p->state = S_CRC;
        }
        return JBP_PARSE_NONE;
    case S_CRC: {
        uint32_t crc;
        p->crc[p->got++] = b;
        if (p->got < 4)
            return JBP_PARSE_NONE;
        p->state = S_SYNC0;
        crc = jbp_crc32(0, p->head + 2, JBP_HEADER_BYTES - 2);
        crc = jbp_crc32(crc, p->payload, p->len);
        if (crc != jbp_get32(p->crc)) {
            ++p->bad;
            return JBP_PARSE_BAD;
        }
        return JBP_PARSE_FRAME;
    }
    }
    p->state = S_SYNC0;
    return JBP_PARSE_NONE;
}
