/* Reading the seed traces (trace.h). */
#include "trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_image.h"

static const char *const op_names[TRACE_OP_LIMIT] = {
    [TRACE_OP_END] = "END", [TRACE_OP_GP0] = "GP0", [TRACE_OP_FILL] = "FILL",
    [TRACE_OP_LOAD] = "LOAD", [TRACE_OP_STORE] = "STORE", [TRACE_OP_COPY] = "COPY",
    [TRACE_OP_FLUSH] = "FLUSH", [TRACE_OP_SYNC] = "SYNC",
    [TRACE_OP_RESET_STATE] = "RESET_STATE", [TRACE_OP_HIRES_REPLICATE] = "HIRES_REPLICATE",
    [TRACE_OP_LOWRES_MARK] = "LOWRES_MARK", [TRACE_OP_LOWRES_RESOLVE] = "LOWRES_RESOLVE",
    [TRACE_OP_TRACE_CALL] = "TRACE_CALL", [TRACE_OP_TRACE_TIME] = "TRACE_TIME",
    [TRACE_OP_CONFIGURE] = "CONFIGURE", [TRACE_OP_SET_EXACT_1X] = "SET_EXACT_1X",
    [TRACE_OP_SET_TEXTURE_MAPPING] = "SET_TEXTURE_MAPPING", [TRACE_OP_VRAM_GET] = "VRAM_GET",
    [TRACE_OP_PRECISE_ENABLE] = "PRECISE_ENABLE",
    [TRACE_OP_PRECISE_BEGIN_FRAME] = "PRECISE_BEGIN_FRAME",
    [TRACE_OP_PRECISE_RECORD] = "PRECISE_RECORD", [TRACE_OP_PRECISE_STATS] = "PRECISE_STATS",
    [TRACE_OP_PRESENT] = "PRESENT", [TRACE_OP_CAPTURE] = "CAPTURE", [TRACE_OP_POKE] = "POKE",
};

const char *trace_op_name(unsigned code)
{
    return code < TRACE_OP_LIMIT && op_names[code] != NULL ? op_names[code] : "prelude";
}

void trace_die(const trace_reader *r, const char *what)
{
    fprintf(stderr, "%s: offset %zu: %s\n", r->path, r->pos, what);
    exit(2);
}

uint32_t trace_u32(trace_reader *r)
{
    const unsigned char *b = r->data + r->pos;

    if (r->end - r->pos < 4)
        trace_die(r, "truncated");
    r->pos += 4;
    return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

int trace_i32(trace_reader *r)
{
    uint32_t v = trace_u32(r);
    return v <= INT32_MAX ? (int)v : -(int)(~v) - 1;
}

uint64_t trace_u64(trace_reader *r)
{
    uint64_t lo = trace_u32(r);
    return lo | (uint64_t)trace_u32(r) << 32;
}

uint16_t *trace_u16s(trace_reader *r, uint32_t n)
{
    uint16_t *p = malloc((size_t)n * 2 + 2);
    size_t bytes = ((size_t)n * 2 + 3) & ~(size_t)3;
    uint32_t i;

    if (p == NULL)
        trace_die(r, "out of memory");
    if (r->end - r->pos < bytes)
        trace_die(r, "truncated");
    for (i = 0; i < n; ++i)
        p[i] = (uint16_t)(r->data[r->pos + 2 * i] | r->data[r->pos + 2 * i + 1] << 8);
    r->pos += bytes;
    return p;
}

static void load_file(trace_reader *r, const char *path)
{
    FILE *f = fopen(path, "rb");
    long size = 0;

    r->path = path;
    r->pos = 0;
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0)
        trace_die(r, "cannot read");
    rewind(f);
    r->data = malloc((size_t)size + 1);
    if (r->data == NULL || fread(r->data, 1, (size_t)size, f) != (size_t)size)
        trace_die(r, "cannot read");
    fclose(f);
    r->len = r->end = (size_t)size;
}

void trace_open(trace_reader *r, const char *path, char *name, size_t name_size,
                trace_prelude *p)
{
    uint32_t header, nlen, scene;
    char raw[64];

    load_file(r, path);
    if (r->len < 8 || memcmp(r->data, "JBSEEDT\0", 8) != 0)
        trace_die(r, "bad magic");
    r->pos = 8;
    if (trace_u32(r) != 1)
        trace_die(r, "unknown version");
    header = trace_u32(r);
    scene = trace_u32(r);
    nlen = trace_u32(r);
    if (nlen >= sizeof raw || r->len - r->pos < nlen)
        trace_die(r, "bad name");
    memcpy(raw, r->data + r->pos, nlen);
    raw[nlen] = 0;
    snprintf(name, name_size, "%02u-%s", scene, raw);
    r->pos += (nlen + 3) & ~3u;

    p->scale = trace_i32(r);
    p->dither = trace_i32(r);
    p->threads = trace_i32(r);
    p->exact_1x = trace_i32(r);
    p->texture_mapping = trace_i32(r);
    p->persp_span = trace_i32(r);
    p->persp_span_1x = trace_i32(r);
    p->precise_enabled = trace_i32(r);
    p->precise_lookups = trace_u32(r);
    p->precise_hits = trace_u32(r);
    p->precise_count = trace_u32(r);
    if ((r->end - r->pos) / 16 < p->precise_count)
        trace_die(r, "precise table too large");
    p->precise_table = r->pos;
    r->pos += (size_t)p->precise_count * 16;
    p->gpu_scale = trace_u32(r);
    p->start_hash_1x = trace_u64(r);
    p->start_hash_scaled = trace_u64(r);
    if (r->pos != header)
        trace_die(r, "header bytes disagree with the prelude");
}

void trace_close(trace_reader *r)
{
    free(r->data);
    r->data = NULL;
}

void trace_precise_entry(const trace_reader *r, const trace_prelude *p, uint32_t i,
                         uint32_t *key, int *x, int *y, int *z)
{
    trace_reader at = *r;

    at.pos = p->precise_table + (size_t)i * 16;
    at.end = at.len;
    *key = trace_u32(&at);
    *x = trace_i32(&at);
    *y = trace_i32(&at);
    *z = trace_i32(&at);
}

uint32_t trace_next_op(trace_reader *r)
{
    uint32_t code;
    size_t plen;

    r->end = r->len;
    code = trace_u32(r);
    plen = trace_u32(r);
    if (plen % 4 != 0 || r->len - r->pos < plen)
        trace_die(r, "bad op length");
    r->end = r->pos + plen;
    if (code == TRACE_OP_END && (plen != 0 || r->end != r->len))
        trace_die(r, "END has a payload or data after it");
    return code;
}

void trace_op_done(trace_reader *r)
{
    if (r->pos != r->end)
        trace_die(r, "op payload longer than its fields");
}

uint64_t trace_fnv_bytes(uint64_t h, const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint64_t trace_fnv_pixels(const uint16_t *p, size_t n)
{
    uint64_t h = TRACE_FNV_INIT;
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i] & 0xffu;
        h *= 0x100000001b3ull;
        h ^= (unsigned)p[i] >> 8;
        h *= 0x100000001b3ull;
    }
    return h;
}

uint16_t *trace_golden_load(const char *path, int w, int h, const char **why)
{
    int gw = 0, gh = 0, comp;
    unsigned char *rgba = stbi_load(path, &gw, &gh, &comp, 4);
    uint16_t *pix;
    size_t i;

    if (rgba == NULL) {
        *why = stbi_failure_reason();
        return NULL;
    }
    if (gw != w || gh != h) {
        *why = "wrong size";
        stbi_image_free(rgba);
        return NULL;
    }
    pix = malloc((size_t)w * h * sizeof *pix);
    if (pix == NULL) {
        *why = "out of memory";
        stbi_image_free(rgba);
        return NULL;
    }
    for (i = 0; i < (size_t)w * h; ++i) {
        const unsigned char *o = rgba + 4 * i;
        pix[i] = (uint16_t)(o[0] >> 3 | (o[1] >> 3) << 5 | (o[2] >> 3) << 10 |
                            (o[3] == 128 ? 0x8000 : 0));
    }
    stbi_image_free(rgba);
    return pix;
}
