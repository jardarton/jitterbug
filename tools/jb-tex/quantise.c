/*
 * jb-tex's quantiser (quantise.h): our own median cut, nearest-colour
 * mapping and Floyd-Steinberg dithering, in integers.
 *
 * Every pixel is first the 15-bit colour jb_palette_from_rgba gives it
 * (each channel's top 5 bits; alpha 0 transparent, 0x0000; alpha 1..127
 * with the semi-transparency bit; opaque black 0x8000). Those colours are
 * what the palette is built from and matched against, so a texture that
 * needs no more colours than the palette has is kept exactly.
 */
#include "quantise.h"

#include <jitterbug.h>

#include <stdlib.h>
#include <string.h>

/* Channel c (0 red, 1 green, 2 blue) of a 15-bit colour, and its bit 15. */
static int chan(unsigned key, int c)
{
    return (int)(key >> (5 * c)) & 31;
}

static int stp(unsigned key)
{
    return (int)(key >> 15) & 1;
}

/* A 5-bit channel as 8 bits, as jb_frame shows it. */
static int widen(int c5)
{
    return c5 << 3 | c5 >> 2;
}

typedef struct colour {
    uint16_t key;
    uint32_t count;
} colour;

typedef struct box {
    int start, end;
} box;

/* Orders by one channel, then by the whole colour, so the order is total
   (no two colours are equal) and qsort's result is the same everywhere. */
#define BY_CHANNEL(name, c)                                                                   \
    static int name(const void *pa, const void *pb)                                           \
    {                                                                                         \
        const colour *a = pa, *b = pb;                                                        \
        int d = chan(a->key, c) - chan(b->key, c);                                            \
        if (d != 0)                                                                           \
            return d;                                                                         \
        return (int)a->key - (int)b->key;                                                     \
    }
BY_CHANNEL(by_red, 0)
BY_CHANNEL(by_green, 1)
BY_CHANNEL(by_blue, 2)

/*
 * The palette's colours for the opaque and semi-transparent pixels (keys,
 * n of them, 0 for transparent ones, which are skipped): at most max
 * entries into out; returns their count, or -1 for no memory.
 *
 * With max or fewer different colours, they are the palette, in order of
 * their 15-bit value. Otherwise median cut: the colours with and without
 * the semi-transparency bit start as separate boxes, so that no entry
 * mixes the two; then, until there are max boxes, the box with the largest
 * score (the range of its widest channel times its pixels; ties to the
 * earlier box) of those with two colours or more is cut along that channel
 * (ties red, green, blue), at the pixel-weighted median: its colours in
 * order of that channel, the first part ending with the colour at which
 * the running count reaches half of the box's, with at least one colour on
 * each side. The new box goes at the end. Each entry is its box's mean,
 * weighted by pixels, rounded half up, with the box's bit 15; a mean of
 * 0x0000 (which would be transparent) is 0x8000, as jb_palette_from_rgba
 * makes opaque black.
 */
static int build_palette(const uint16_t *keys, size_t n, int max, uint16_t *out)
{
    static int (*const by[3])(const void *, const void *) = {by_red, by_green, by_blue};
    uint32_t *hist = calloc(65536, sizeof *hist);
    colour *cols;
    box boxes[256];
    int ncol = 0, nb = 0, i, g;
    size_t k;

    if (hist == NULL)
        return -1;
    for (k = 0; k < n; k++)
        if (keys[k] != 0)
            hist[keys[k]]++;
    for (i = 1; i < 65536; i++)
        ncol += hist[i] != 0;
    cols = malloc(((size_t)ncol + 1) * sizeof *cols);
    if (cols == NULL) {
        free(hist);
        return -1;
    }
    ncol = 0;
    for (i = 1; i < 65536; i++)
        if (hist[i] != 0) {
            cols[ncol].key = (uint16_t)i;
            cols[ncol].count = hist[i];
            ncol++;
        }
    free(hist);
    if (ncol <= max) {
        for (i = 0; i < ncol; i++)
            out[i] = cols[i].key;
        free(cols);
        return ncol;
    }
    /* In key order: those without bit 15 first. */
    for (g = 0; g < ncol && !stp(cols[g].key); g++)
        ;
    if (g > 0)
        boxes[nb].start = 0, boxes[nb].end = g, nb++;
    if (g < ncol)
        boxes[nb].start = g, boxes[nb].end = ncol, nb++;
    while (nb < max) {
        uint64_t best_score = 0, total = 0, acc = 0;
        int best = -1, best_c = 0, m, b;

        for (b = 0; b < nb; b++) {
            int lo[3] = {31, 31, 31}, hi[3] = {0, 0, 0}, c, wide = 0;
            uint64_t count = 0, score;
            if (boxes[b].end - boxes[b].start < 2)
                continue;
            for (i = boxes[b].start; i < boxes[b].end; i++) {
                for (c = 0; c < 3; c++) {
                    int v = chan(cols[i].key, c);
                    lo[c] = v < lo[c] ? v : lo[c];
                    hi[c] = v > hi[c] ? v : hi[c];
                }
                count += cols[i].count;
            }
            for (c = 1; c < 3; c++)
                if (hi[c] - lo[c] > hi[wide] - lo[wide])
                    wide = c;
            score = (uint64_t)(hi[wide] - lo[wide]) * count;
            if (best < 0 || score > best_score) {
                best = b;
                best_score = score;
                best_c = wide;
            }
        }
        if (best < 0)
            break;
        qsort(cols + boxes[best].start, (size_t)(boxes[best].end - boxes[best].start),
              sizeof *cols, by[best_c]);
        for (i = boxes[best].start; i < boxes[best].end; i++)
            total += cols[i].count;
        m = boxes[best].end - 1;
        for (i = boxes[best].start; i < boxes[best].end; i++) {
            acc += cols[i].count;
            if (acc * 2 >= total) {
                m = i + 1;
                break;
            }
        }
        if (m >= boxes[best].end)
            m = boxes[best].end - 1;
        if (m <= boxes[best].start)
            m = boxes[best].start + 1;
        boxes[nb].start = m;
        boxes[nb].end = boxes[best].end;
        boxes[best].end = m;
        nb++;
    }
    for (g = 0; g < nb; g++) {
        uint64_t sum[3] = {0, 0, 0}, count = 0;
        unsigned key;
        int c;
        for (i = boxes[g].start; i < boxes[g].end; i++) {
            for (c = 0; c < 3; c++)
                sum[c] += (uint64_t)chan(cols[i].key, c) * cols[i].count;
            count += cols[i].count;
        }
        key = (unsigned)stp(cols[boxes[g].start].key) << 15;
        for (c = 0; c < 3; c++)
            key |= (unsigned)((sum[c] + count / 2) / count) << (5 * c);
        out[g] = (uint16_t)(key == 0 ? 0x8000u : key);
    }
    free(cols);
    return nb;
}

/*
 * The entry of pal[first .. first + count) nearest key: the least sum of
 * squared differences of the 5-bit channels among entries with key's bit
 * 15 (any entry if none has it), ties to the lowest index.
 */
static int nearest(unsigned key, const uint16_t *pal, int first, int count)
{
    int pass, i, best = first;
    long best_d = -1;

    for (pass = 0; pass < 2 && best_d < 0; pass++)
        for (i = first; i < first + count; i++) {
            long d = 0;
            int c;
            if (pass == 0 && stp(pal[i]) != stp(key))
                continue;
            for (c = 0; c < 3; c++) {
                long e = chan(pal[i], c) - chan(key, c);
                d += e * e;
            }
            if (best_d < 0 || d < best_d) {
                best_d = d;
                best = i;
            }
        }
    return best;
}

/*
 * The same for a dithered target: t[c] the wanted channel in 8 bits times
 * 16 (with the error carried to it), against the entries as shown
 * (widen); ties to the lowest index.
 */
static int nearest_wide(const long *t, int bit, const uint16_t *pal, int first, int count)
{
    int pass, i, best = first;
    long long best_d = -1;

    for (pass = 0; pass < 2 && best_d < 0; pass++)
        for (i = first; i < first + count; i++) {
            long long d = 0;
            int c;
            if (pass == 0 && stp(pal[i]) != bit)
                continue;
            for (c = 0; c < 3; c++) {
                long long e = (long long)widen(chan(pal[i], c)) * 16 - t[c];
                d += e * e;
            }
            if (best_d < 0 || d < best_d) {
                best_d = d;
                best = i;
            }
        }
    return best;
}

/* The 5-bit channel whose shown value is nearest t (8 bits times 16), ties
   to the lower. */
static int nearest5(long t)
{
    int c = (int)(t / 128), best = 0, k;
    long best_d = -1;

    for (k = c - 1; k <= c + 1; k++) {
        long d;
        if (k < 0 || k > 31)
            continue;
        d = (long)widen(k) * 16 - t;
        d = d < 0 ? -d : d;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = k;
        }
    }
    return best;
}

/* Floyd-Steinberg: e (16ths of an 8-bit level) from pixel x of this row
   to the pixels after it: 7/16 right, 3/16 below left, 5/16 below, the
   rest (1/16 and what the divisions left) below right. Divisions truncate
   towards 0, as C's do. */
static void diffuse(long *cur, long *next, int x, const long *e)
{
    int c;

    for (c = 0; c < 3; c++) {
        long e7 = e[c] * 7 / 16, e3 = e[c] * 3 / 16, e5 = e[c] * 5 / 16;
        cur[(x + 2) * 3 + c] += e7;
        next[x * 3 + c] += e3;
        next[(x + 1) * 3 + c] += e5;
        next[(x + 2) * 3 + c] += e[c] - e7 - e3 - e5;
    }
}

int jbq_convert(const uint8_t *rgba, int w, int h, int depth, int dither, uint8_t *idx,
                uint16_t *palette, uint16_t *texels)
{
    size_t n = (size_t)w * h, k;
    uint16_t *keys = malloc(n * sizeof *keys);
    long *err = NULL;
    int entries = depth == 4 ? 16 : depth == 8 ? 256 : 0;
    int transparent = 0, first = 0, count = 0, x, y;

    if (keys == NULL)
        return -1;
    jb_palette_from_rgba(rgba, n, keys);
    if (dither) {
        /* Two rows of carried error, each with a pixel of margin either
           side: cur for this row (from x + 1), next for the one below. */
        err = calloc((size_t)(w + 2) * 3 * 2, sizeof *err);
        if (err == NULL) {
            free(keys);
            return -1;
        }
    }
    if (entries != 0) {
        for (k = 0; k < n && !transparent; k++)
            transparent = keys[k] == 0;
        memset(palette, 0, (size_t)entries * sizeof *palette);
        first = transparent; /* entry 0 is 0x0000 when a pixel is transparent */
        count = build_palette(keys, n, entries - first, palette + first);
        if (count < 0) {
            free(keys);
            free(err);
            return -1;
        }
    }
    if (entries != 0 && !dither) {
        int16_t *cache = malloc(65536 * sizeof *cache);
        if (cache == NULL) {
            free(keys);
            return -1;
        }
        memset(cache, 0xff, 65536 * sizeof *cache);
        for (k = 0; k < n; k++) {
            unsigned key = keys[k];
            if (key == 0) {
                idx[k] = 0;
                continue;
            }
            if (cache[key] < 0)
                cache[key] = (int16_t)nearest(key, palette, first, count);
            idx[k] = (uint8_t)cache[key];
        }
        free(cache);
        free(keys);
        return 0;
    }
    if (entries == 0 && !dither) {
        memcpy(texels, keys, n * sizeof *keys);
        free(keys);
        return 0;
    }
    for (y = 0; y < h; y++) {
        long *cur = err + (y % 2) * (w + 2) * 3, *next = err + ((y + 1) % 2) * (w + 2) * 3;
        memset(next, 0, (size_t)(w + 2) * 3 * sizeof *next);
        for (x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            const uint8_t *px = rgba + 4 * i;
            long t[3], e[3];
            int c, bit;
            if (keys[i] == 0) {
                /* Transparent: nothing carried through it. */
                if (entries != 0)
                    idx[i] = 0;
                else
                    texels[i] = 0;
                continue;
            }
            bit = stp(keys[i]);
            for (c = 0; c < 3; c++) {
                t[c] = (long)px[c] * 16 + cur[(x + 1) * 3 + c];
                t[c] = t[c] < 0 ? 0 : t[c] > 255 * 16 ? 255 * 16 : t[c];
            }
            if (entries != 0) {
                int j = nearest_wide(t, bit, palette, first, count);
                idx[i] = (uint8_t)j;
                for (c = 0; c < 3; c++)
                    e[c] = t[c] - (long)widen(chan(palette[j], c)) * 16;
            } else {
                unsigned key = 0;
                for (c = 0; c < 3; c++) {
                    int q = nearest5(t[c]);
                    key |= (unsigned)q << (5 * c);
                    e[c] = t[c] - (long)widen(q) * 16;
                }
                /* As jb_palette_from_rgba: bit 15 from alpha, and opaque
                   black 0x8000. */
                key |= (px[3] < 128) ? 0x8000u : 0;
                texels[i] = (uint16_t)(key == 0 ? 0x8000u : key);
            }
            diffuse(cur, next, x, e);
        }
    }
    free(err);
    free(keys);
    return 0;
}

void jbq_frame_palette(const uint8_t *rgba, int w, int h, const uint8_t *idx, int entries,
                       const uint16_t *base, uint16_t *out)
{
    uint64_t sum[256][3], count[256];
    size_t n = (size_t)w * h, k;
    int i, c;

    memset(sum, 0, sizeof sum);
    memset(count, 0, sizeof count);
    for (k = 0; k < n; k++) {
        for (c = 0; c < 3; c++)
            sum[idx[k]][c] += (unsigned)rgba[4 * k + c] >> 3;
        count[idx[k]]++;
    }
    for (i = 0; i < entries; i++) {
        unsigned key;
        if (count[i] == 0 || base[i] == 0) {
            out[i] = base[i];
            continue;
        }
        key = base[i] & 0x8000u;
        for (c = 0; c < 3; c++)
            key |= (unsigned)((sum[i][c] + count[i] / 2) / count[i]) << (5 * c);
        out[i] = (uint16_t)(key == 0 ? 0x8000u : key);
    }
}
