/*
 * The public API under a seeded random stream: GP0 commands from the
 * generator in api_test.h (biased towards complete valid commands over
 * every opcode, with now and then any word at all, loads cut short and
 * polylines that run on), cut into random chunks, with GP1 words (resets,
 * display modes and areas, register reads), GPUREAD, GPUSTAT and frames
 * mixed in. At 1x and 2x, each on 1 and on 4 threads.
 *
 * What it checks: nothing crashes, and under the sanitizer build nothing
 * is reported (no read or write outside any buffer, no undefined
 * behaviour, no leak); every call succeeds; GPUSTAT's fixed bits hold;
 * frames have the size of the display; and the run's final VRAM and frame
 * do not depend on the thread count.
 */
#include "api_test.h"

static const int widths[4] = {256, 320, 512, 640};

typedef struct {
    uint64_t vram_hash, frame_hash;
    int fw, fh;
} Outcome;

static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* A GP1 word: mostly the display commands, sometimes anything. */
static uint32_t gen_gp1(gen *g)
{
    int k = rnd_below(g, 16);
    uint32_t param = rnd(g) & 0xffffffu;

    if (k == 0)
        return rnd(g);
    if (k < 3)
        return 0x10000000u | (param & 0xfu); /* register reads */
    if (k < 5)
        return 0x08000000u | (param & 0xffu); /* display modes */
    if (k < 7)
        return 0x05000000u | param;
    if (k < 9)
        return 0x03000000u | (param & 1);
    return (uint32_t)rnd_below(g, 0x20) << 24 | param;
}

static void run(int scale, int threads, uint64_t seed, int words, Outcome *out)
{
    jb_config c;
    jb_context *jb;
    gen g;
    uint32_t *cmd = must_alloc(GEN_MAX * 4);
    int fed = 0, frames = 0, mode = -1, size_bad = 0; /* mode -1: jb_config's size */
    unsigned stat_bad = 0;
    uint16_t *vram;
    const uint8_t *f;

    memset(&c, 0, sizeof c);
    c.scale = scale;
    c.threads = threads;
    jb = must_create(&c);
    g.s = seed;
    g.cx = 512;
    g.cy = 256;
    while (fed < words) {
        int n = gen_command(&g, cmd, 0), at = 0, k = rnd_below(&g, 100);

        while (at < n) {
            int chunk = 1 + rnd_below(&g, 48);
            if (chunk > n - at)
                chunk = n - at;
            gp0(jb, cmd + at, (size_t)chunk);
            at += chunk;
        }
        fed += n;
        if (k < 3) {
            uint32_t w = gen_gp1(&g);
            if (((w >> 24) & 0x3f) == 0x08)
                mode = (int)(w & 0xff);
            if (((w >> 24) & 0x3f) == 0x00)
                mode = -1; /* back to the configured size */
            gp1(jb, w);
        } else if (k < 6) {
            int r = rnd_below(&g, 8);
            while (r-- > 0)
                jb_gpuread(jb);
        } else if (k < 8) {
            uint32_t s = jb_gpustat(jb);
            stat_bad += ((s >> 26) & 1) != 1 || ((s >> 28) & 1) != 1 || ((s >> 13) & 1) != 1 ||
                        ((s >> 31) & 1) != 0;
        }
        if (fed / 4000 > frames) {
            int w, h, want_w = 320, want_h = 240;

            ++frames;
            f = jb_frame(jb, &w, &h);
            if (mode >= 0) {
                want_w = (mode & 0x40) ? 368 : widths[mode & 3];
                want_h = (mode & 0x04) && (mode & 0x20) ? 480 : 240;
            }
            size_bad += f == NULL || w != want_w * scale || h != want_h * scale;
        }
    }
    check("GPUSTAT's fixed bits", (long long)stat_bad, 0);
    check("frame sizes", size_bad, 0);
    /* The end state: whatever was pending is dropped, then VRAM and a
       frame of all of it. */
    gp1(jb, 0x01000000u);
    vram = read_vram(jb);
    out->vram_hash = fnv(0xcbf29ce484222325ull, (const unsigned char *)vram,
                         (size_t)VRAM_PIXELS * 2);
    gp1(jb, 0x03000000u);
    gp1(jb, 0x05000000u);
    gp1(jb, 0x08000027u);
    f = jb_frame(jb, &out->fw, &out->fh);
    out->frame_hash = f != NULL ? fnv(0xcbf29ce484222325ull, f, (size_t)out->fw * out->fh * 4) : 0;
    printf("  %dx, %d thread%s: %d words, %d frames, VRAM %016llx\n", scale, threads,
           threads == 1 ? "" : "s", fed, frames, (unsigned long long)out->vram_hash);
    free(vram);
    free(cmd);
    jb_destroy(jb);
}

int main(int argc, char **argv)
{
    int words = argc > 1 ? atoi(argv[1]) : 200000, scale;

    for (scale = 1; scale <= 2; ++scale) {
        Outcome one, four;
        uint64_t seed = 0x0b5e55edu + (uint64_t)scale;

        run(scale, 1, seed, words, &one);
        run(scale, 4, seed, words, &four);
        check("the VRAM does not depend on the thread count", one.vram_hash == four.vram_hash, 1);
        check("the frame does not depend on the thread count",
              (long long)(one.frame_hash == four.frame_hash && one.fw == four.fw), 1);
    }
    return finish("api_robust");
}
