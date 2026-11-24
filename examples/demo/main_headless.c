/*
 * demo-headless: the demo without a window. It runs N ticks: at each, the
 * keys a script gives for that tick are pressed (dm_key, the window's and
 * the page's keyboard), the scene's frame is drawn and hashed, and the
 * frame counter moves on by the step unless paused. It prints the hashes
 * asked for and one of all the frames, and can write frames as PNGs. The
 * hash is FNV-1a 64 over the RGBA bytes jb_frame gives, frame after frame.
 *
 *   demo-headless [options]
 *     --frames N        ticks to run (240)
 *     --step S          scene frames a tick, 1 to 64 (1): with 8, 346
 *                       ticks fly the whole loop (DM_LOOP frames)
 *     --start F         the scene frame of the first tick (0)
 *     --scale S         internal scale, 1 to 8 (1)
 *     --precise 0|1     precise vertices (0)
 *     --perspective 0|1 perspective-correct textures (0)
 *     --dither 0|1      dithering (1)
 *     --widescreen 0|1  the 16:9 display, 427 x 240 (0)
 *     --float 0|1       the float geometry path instead of the GTE (0)
 *     --clut-cache 0|1  the CLUT cache (1)
 *     --fog 0|1         the haze (1)
 *     --panel 0|1       the panel (1)
 *     --gaps 0|1        the gap check (0): each tick's frame is drawn
 *                       twice, cleared to two key colours, without the sky,
 *                       and every pixel that must show the world
 *                       (dm_scene_covered) must be the same in both, or
 *                       the clear colour showed through there: a crack, a
 *                       culled face, a missing chunk; then the same for
 *                       the sea drawn alone, opaque (gaps(), which says
 *                       what this cannot see).
 *                       Prints each tick with gaps (how many, the first
 *                       few); status 1 if any. --png writes the first of
 *                       the two frames with its gaps painted red; the
 *                       hashes are of the first, unpainted
 *     --fade N          the fade check (0, off; with the haze on): each
 *                       tick's frame is drawn again with every chunk of
 *                       terrain one fade level farther on (dm_scene,
 *                       fade_bias), as a chunk is drawn when it crosses
 *                       to the next; prints the largest change of a
 *                       pixel's channel (0..255) over the run; status 1
 *                       if above N
 *     --pier 0|1        the pier check (0): each tick's frame is drawn
 *                       again without the pier's posts, and without them
 *                       and its deck; a pixel the posts change (first
 *                       against second) where the deck shows (second
 *                       against third) is a post drawn over the deck,
 *                       which the eye, always above it, must never see;
 *                       prints how many; status 1 if any
 *                       (--gaps, --pier and --fade: one at a time)
 *     --script S        keys by tick: "-" for none, or TICK:KEYS[,TICK:KEYS...],
 *                       KEYS one or more of dm_key's (1-4 v t d c w g f h p n),
 *                       pressed in order at the start of that tick
 *     --threads T       rasterising threads (1; 0 for one per CPU)
 *     --hash F          print the hash of tick F's frame (repeatable)
 *     --png F PATH      write tick F's frame as a PNG (repeatable)
 *     --expect FILE [NAME]
 *                       run NAME of FILE (every run if no NAME) and compare
 *                       its hash with the one FILE gives
 *     --record FILE     run every run of FILE and write FILE again with
 *                       the hashes the runs now give
 *
 * FILE (expected.txt) has a run a line, '#' starting a comment line:
 *   name frames step scale precise perspective dither widescreen float
 *   clutcache fog script hash
 * columns separated by spaces or tabs. A run's columns become the options
 * above and go through the same parser as the command line, so a run draws
 * exactly what those options draw; --threads applies to every run. After
 * each run --expect also checks that the switches it ended with reached
 * the context where the picture cannot show it (dithering, by GPUSTAT; the
 * frame's size; the CLUT cache, by a probe). The exit status is 1 on any
 * mismatch.
 *
 * Numbers must be whole and in range (--frames 0..1000000, the switches 0
 * or 1, --threads 0..64, ticks 0..1000000000), and a script well formed;
 * anything else is refused, with exit status 2, before anything is drawn
 * or written, as is a run file it cannot use (checked whole first). Status
 * 1 is a mismatch, or a file that cannot be read or written. --record
 * rewrites only each run's hash column, every other byte kept, through a
 * file of its own beside FILE (FILE.N.tmp) renamed over it once complete.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene.h"
#include "stb_image_write.h"

#ifdef _WIN32
#include <windows.h> /* MoveFileExA: rename does not replace a file there */
#endif

enum { MAX_ASKED = 64, MAX_RUNS = 32, COLUMNS = 13, MAX_EVENTS = 256, MAX_KEYS = 8 };
enum { MAX_TICK = 1000000000 };

typedef struct {
    unsigned frame;
    const char *path;
} png_request;

/* A tick's keys. */
typedef struct {
    unsigned tick;
    char keys[MAX_KEYS + 1];
} event;

/* Everything the command line says. */
typedef struct {
    dm_switches sw;
    unsigned frames, step, start;
    int threads, gaps, fade, pier;
    event ev[MAX_EVENTS];
    int nev;
    unsigned hash_at[MAX_ASKED];
    int nhash;
    png_request png[MAX_ASKED];
    int npng;
    const char *expect_file, *expect_name, *record_file;
} options;

/* A run of FILE: its columns as written, and where its hash column is in
   the file's text (for --record). */
typedef struct {
    char col[COLUMNS][256];
    unsigned long long hash;
    size_t hash_at, hash_end;
} run;

static const unsigned long long fnv_basis = 0xcbf29ce484222325ull;

static unsigned long long fnv1a(unsigned long long h, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* A whole number lo..hi, all of s (digits only): 1, or 0 with the reason
   printed. */
static int number(const char *opt, const char *s, long lo, long hi, long *out)
{
    char *end;
    long v;

    errno = 0;
    v = s[0] >= '0' && s[0] <= '9' ? strtol(s, &end, 10) : 0;
    if (s[0] < '0' || s[0] > '9' || *end != '\0' || errno == ERANGE || v < lo || v > hi) {
        fprintf(stderr, "demo-headless: %s %s: not a whole number %ld..%ld\n", opt, s, lo, hi);
        return 0;
    }
    *out = v;
    return 1;
}

/* A script into o->ev: 1, or 0 with the reason printed. Each key is tried
   on a scratch state, so only keys dm_key knows are taken. */
static int script(const char *s, options *o)
{
    const char *p = s;

    o->nev = 0;
    if (strcmp(s, "-") == 0)
        return 1;
    while (*p != '\0') {
        const char *colon = strchr(p, ':'), *end;
        char num[16];
        event *e = &o->ev[o->nev];
        size_t n, k;
        long v;

        if (colon == NULL || colon == p || (size_t)(colon - p) >= sizeof num || o->nev == MAX_EVENTS)
            break;
        memcpy(num, p, (size_t)(colon - p));
        num[colon - p] = '\0';
        if (!number("--script", num, 0, MAX_TICK, &v))
            break; /* and the whole script is named below */
        end = colon + 1 + strcspn(colon + 1, ",");
        n = (size_t)(end - colon - 1);
        if (n == 0 || n > MAX_KEYS)
            break;
        for (k = 0; k < n; ++k) {
            dm_switches sw = o->sw;
            int paused = 0;
            unsigned frame = 0;
            if (!dm_key(colon[1 + k], &sw, &paused, &frame))
                break;
        }
        if (k != n)
            break;
        e->tick = (unsigned)v;
        memcpy(e->keys, colon + 1, n);
        e->keys[n] = '\0';
        ++o->nev;
        if (*end == '\0')
            return 1;
        p = end + 1; /* past the comma: another event must follow */
    }
    fprintf(stderr, "demo-headless: --script %s: not TICK:KEYS[,TICK:KEYS...] with at most %d "
                    "events of 1 to %d of the keys 1-4 v t d c w g f h p n\n",
            s, MAX_EVENTS, MAX_KEYS);
    return 0;
}

static int binary(const char *opt, const char *s, int *out)
{
    long v;

    if (!number(opt, s, 0, 1, &v))
        return 0;
    *out = (int)v;
    return 1;
}

/* The options in argv[first..argc), over what *o holds: 0, or 2 for one
   that is unknown, incomplete or out of range. */
static int parse_options(int argc, char **argv, int first, options *o)
{
    int i;

    for (i = first; i < argc; ++i) {
        const char *a = argv[i];
        int more = argc - i - 1, ok = 1, flt = 0;
        long v = 0;

        if (strcmp(a, "--frames") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 1000000, &v), o->frames = (unsigned)v;
        else if (strcmp(a, "--step") == 0 && more >= 1)
            ok = number(a, argv[++i], 1, 64, &v), o->step = (unsigned)v;
        else if (strcmp(a, "--start") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, MAX_TICK, &v), o->start = (unsigned)v;
        else if (strcmp(a, "--scale") == 0 && more >= 1)
            ok = number(a, argv[++i], 1, 8, &v), o->sw.scale = (int)v;
        else if (strcmp(a, "--precise") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->sw.precise);
        else if (strcmp(a, "--perspective") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->sw.perspective);
        else if (strcmp(a, "--dither") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->sw.dither);
        else if (strcmp(a, "--widescreen") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->sw.widescreen);
        else if (strcmp(a, "--float") == 0 && more >= 1) {
            ok = binary(a, argv[++i], &flt);
            o->sw.geometry = flt ? JB_GEOMETRY_FLOAT : JB_GEOMETRY_GTE;
        } else if (strcmp(a, "--clut-cache") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->sw.clut_cache);
        else if (strcmp(a, "--fog") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->sw.fog);
        else if (strcmp(a, "--panel") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->sw.panel);
        else if (strcmp(a, "--script") == 0 && more >= 1)
            ok = script(argv[++i], o);
        else if (strcmp(a, "--gaps") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->gaps);
        else if (strcmp(a, "--pier") == 0 && more >= 1)
            ok = binary(a, argv[++i], &o->pier);
        else if (strcmp(a, "--fade") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 255, &v), o->fade = (int)v;
        else if (strcmp(a, "--record") == 0 && more >= 1)
            o->record_file = argv[++i];
        else if (strcmp(a, "--threads") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 64, &v), o->threads = (int)v;
        else if (strcmp(a, "--hash") == 0 && more >= 1 && o->nhash < MAX_ASKED)
            ok = number(a, argv[++i], 0, MAX_TICK, &v), o->hash_at[o->nhash++] = (unsigned)v;
        else if (strcmp(a, "--png") == 0 && more >= 2 && o->npng < MAX_ASKED) {
            ok = number(a, argv[++i], 0, MAX_TICK, &v);
            o->png[o->npng].frame = (unsigned)v;
            o->png[o->npng++].path = argv[++i];
        } else if (strcmp(a, "--expect") == 0 && more >= 1) {
            o->expect_file = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-')
                o->expect_name = argv[++i];
        } else {
            fprintf(stderr, "demo-headless: unknown or incomplete option %s\n"
                            "(see the comment at the top of main_headless.c)\n",
                    a);
            return 2;
        }
        if (!ok)
            return 2;
    }
    if ((o->gaps != 0) + (o->pier != 0) + (o->fade != 0) > 1) {
        fprintf(stderr, "demo-headless: --gaps, --pier and --fade are checks of their own: give one "
                        "at a time\n");
        return 2;
    }
    return 0;
}

static void defaults(options *o)
{
    dm_switches console = DM_SWITCHES_CONSOLE;

    memset(o, 0, sizeof *o);
    o->sw = console;
    o->frames = 240;
    o->step = 1;
    o->threads = 1;
}

/*
 * Whether the switches reached the context, where the picture does not
 * show it: GPUSTAT's dithering bit, the frame's size (scale, widescreen),
 * and the CLUT cache, by a probe after the run's last frame: a 4-bit pixel
 * drawn red, its palette entry rewritten green in VRAM with no GP0(01h),
 * the pixel drawn again beside it: red and red with the cache on, red and
 * green with it off. The number of mismatches, each printed.
 */
static int check_forwarding(dm_scene *s, const dm_switches *sw)
{
    static const uint16_t texel = 1, red[2] = {0, 0x001f}, green = 0x03e0;
    jb_texture t;
    jb_rect r;
    uint16_t px[2] = {0, 0}, want = sw->clut_cache ? 0x001f : 0x03e0;
    int w, h, bad = 0, calls;

    if ((int)(jb_gpustat(s->jb) >> 9 & 1) != (sw->dither != 0))
        printf("FAIL: dithering %d did not reach the context\n", sw->dither), ++bad;
    jb_frame(s->jb, &w, &h);
    if (w != (sw->widescreen ? 427 : 320) * sw->scale || h != 240 * sw->scale)
        printf("FAIL: the frame is %d x %d at scale %d, widescreen %d\n", w, h, sw->scale,
               sw->widescreen), ++bad;
    memset(&t, 0, sizeof t);
    t.page_x = 960, t.page_y = 256, t.depth = JB_TEX_4BIT, t.clut_x = 992, t.clut_y = 511;
    memset(&r, 0, sizeof r);
    r.w = r.h = 1, r.texture = &t, r.raw = 1, r.x = 1000, r.y = 300;
    calls = jb_vram_load(s->jb, 960, 256, 1, 1, &texel) == JB_OK;
    calls &= jb_vram_load(s->jb, 992, 511, 2, 1, red) == JB_OK;
    calls &= jb_set_draw_area(s->jb, 0, 0, 1023, 511) == JB_OK;
    calls &= jb_set_draw_offset(s->jb, 0, 0) == JB_OK;
    calls &= jb_prim_rect(s->jb, &r) == JB_OK;
    calls &= jb_vram_load(s->jb, 993, 511, 1, 1, &green) == JB_OK;
    r.x = 1001;
    calls &= jb_prim_rect(s->jb, &r) == JB_OK;
    calls &= jb_vram_read(s->jb, 1000, 300, 2, 1, px) == JB_OK;
    if (!calls || px[0] != 0x001f || px[1] != want)
        printf("FAIL: CLUT cache %d did not reach the context\n", sw->clut_cache), ++bad;
    return bad;
}

/*
 * The gap check (--gaps), for one tick. Two layers, each drawn twice,
 * cleared to two keys, without the sky (DM_GAPS_NO_SKY, so that a crack in
 * a ridge shows the clear colour rather than the sky behind it):
 *   - the scene (the frame on show, key holds its bytes): the land and the
 *     seabed under the sea;
 *   - the sea alone (DM_GAPS_WATER), opaque: no seabed, land or anything
 *     on it, so that a hole in the sea, which the seabed under it would
 *     hide in the scene, shows the clear colour.
 * Every pixel that must show the layer's surface (dm_scene_covered) and
 * differs between its two drawings let the clear colour through: a gap.
 * Gaps are painted red in key (for --png) and the first few printed.
 * What the clear colour cannot show: a crack in the land with something
 * else drawn behind it (farther land, the seabed or the sea, a tree, the
 * pier), which is drawn in the crack instead; only a crack with nothing
 * behind it within the world (against the sky, which is left out, or the
 * world's end) shows. The number of gaps, or -1 if a frame could not be
 * drawn.
 */
static long gaps(dm_scene *s, unsigned tick, unsigned frame, int paused, uint8_t *key, uint8_t *other)
{
    static const char *const layer_name[2] = {"", " in the sea"};
    const uint8_t *px;
    long n = 0, before;
    int w, h, x, y, layer;

    for (layer = 0; layer < 2; ++layer) {
        const uint8_t *a = key;
        before = n;
        s->gaps = DM_GAPS_NO_SKY | (layer ? DM_GAPS_WATER : 0);
        if (layer) {
            /* The sea layer's first drawing, kept in other. */
            s->clear = JB_RGB(0, 255, 0);
            if (dm_scene_draw(s, frame, paused) != JB_OK || (px = dm_scene_frame(s, &w, &h)) == NULL)
                return -1;
            memcpy(other, px, (size_t)w * (size_t)h * 4);
            a = other;
        }
        s->clear = JB_RGB(255, 0, 255);
        if (dm_scene_draw(s, frame, paused) != JB_OK || (px = dm_scene_frame(s, &w, &h)) == NULL)
            return -1;
        for (y = 0; y < h; ++y)
            for (x = 0; x < w; ++x) {
                size_t at = ((size_t)y * (size_t)w + (size_t)x) * 4;
                if (memcmp(px + at, a + at, 4) == 0 || !dm_scene_covered(s, x, y))
                    continue;
                key[at] = 255, key[at + 1] = key[at + 2] = 0;
                if (n - before < 4)
                    printf("tick %u (frame %u): a gap%s at %d, %d\n", tick, frame, layer_name[layer], x, y);
                ++n;
            }
        if (n > before)
            printf("tick %u (frame %u): %ld gap pixels%s in %d x %d\n", tick, frame, n - before,
                   layer_name[layer], w, h);
    }
    s->gaps = DM_GAPS_NO_SKY;
    return n;
}

/* Runs the ticks o asks for; prints the hashes asked for, writes the PNGs
   asked for, and gives the hash of all frames in *all. With forwarding, checks them after the last frame
   (check_forwarding). 0, or 1 on an error or mismatch. */
static int render(const options *o, int forwarding, unsigned long long *all)
{
    dm_scene scene;
    dm_switches sw = o->sw;
    jb_result r;
    unsigned tick, frame = o->start;
    int i, status = 0, paused = 0;
    uint8_t *key = NULL, *other = NULL;
    size_t key_size = 0;
    long gapped = 0;
    int faded = 0, fade_tick = 0;
    long over = 0;
    uint8_t *nopost = NULL;

    if ((r = dm_scene_init(&scene, &sw, o->threads)) != JB_OK) {
        fprintf(stderr, "demo: cannot start: %s\n", jb_result_string(r));
        return 1;
    }
    *all = fnv_basis;
    for (tick = 0; tick < o->frames && status == 0; ++tick) {
        dm_switches want = sw;
        const uint8_t *px;
        size_t bytes;
        int w, h;

        for (i = 0; i < o->nev; ++i)
            if (o->ev[i].tick == tick) {
                const char *k;
                for (k = o->ev[i].keys; *k != '\0'; ++k)
                    dm_key(*k, &want, &paused, &frame);
            }
        if (memcmp(&want, &sw, sizeof sw) != 0) {
            if ((r = dm_scene_set_switches(&scene, &want)) != JB_OK) {
                fprintf(stderr, "demo: tick %u: switches refused: %s\n", tick, jb_result_string(r));
                status = 1;
                break;
            }
            sw = want;
        }
        if (o->gaps)
            scene.clear = JB_RGB(0, 255, 0), scene.gaps = DM_GAPS_NO_SKY;
        if ((r = dm_scene_draw(&scene, frame, paused)) != JB_OK) {
            fprintf(stderr, "demo: tick %u: %s (%s)\n", tick, jb_result_string(r),
                    jb_error_string(scene.jb));
            status = 1;
            break;
        }
        if ((px = dm_scene_frame(&scene, &w, &h)) == NULL) {
            fprintf(stderr, "demo: tick %u: no frame\n", tick);
            status = 1;
            break;
        }
        bytes = (size_t)w * (size_t)h * 4;
        *all = fnv1a(*all, px, bytes);
        for (i = 0; i < o->nhash; ++i)
            if (o->hash_at[i] == tick)
                printf("tick %u (frame %u): %016llx\n", tick, frame, fnv1a(fnv_basis, px, bytes));
        if (o->gaps) {
            long n;
            if (bytes > key_size) {
                free(key);
                free(other);
                other = malloc(bytes);
                if ((key = malloc(bytes)) == NULL || other == NULL) {
                    fprintf(stderr, "demo: out of memory\n");
                    status = 1;
                    break;
                }
                key_size = bytes;
            }
            memcpy(key, px, bytes);
            if ((n = gaps(&scene, tick, frame, paused, key, other)) < 0) {
                fprintf(stderr, "demo: tick %u: %s\n", tick, jb_error_string(scene.jb));
                status = 1;
                break;
            }
            gapped += n;
            px = key;
        }
        if (o->pier) {
            /* The pier check: without the posts, then without the deck. */
            const uint8_t *q;
            size_t b;
            long here = 0;
            if (bytes > key_size) {
                free(key);
                free(nopost);
                nopost = malloc(bytes);
                if ((key = malloc(bytes)) == NULL || nopost == NULL) {
                    fprintf(stderr, "demo: out of memory\n");
                    status = 1;
                    break;
                }
                key_size = bytes;
            }
            memcpy(key, px, bytes);
            scene.hide = DM_HIDE_POSTS;
            r = dm_scene_draw(&scene, frame, paused);
            if (r == JB_OK && (q = dm_scene_frame(&scene, &w, &h)) != NULL) {
                memcpy(nopost, q, bytes);
                scene.hide = DM_HIDE_POSTS | DM_HIDE_DECK;
                r = dm_scene_draw(&scene, frame, paused);
            }
            scene.hide = 0;
            if (r != JB_OK || (q = dm_scene_frame(&scene, &w, &h)) == NULL) {
                fprintf(stderr, "demo: tick %u: %s\n", tick, jb_error_string(scene.jb));
                status = 1;
                break;
            }
            for (b = 0; b < bytes; b += 4)
                here += memcmp(key + b, nopost + b, 4) != 0 && memcmp(nopost + b, q + b, 4) != 0;
            if (here > 0)
                printf("tick %u (frame %u): %ld pixels of posts over the deck\n", tick, frame, here);
            over += here;
            px = key;
        }
        if (o->fade) {
            /* The fade check: the same frame, every chunk a level on. */
            const uint8_t *q;
            size_t b;
            if (bytes > key_size) {
                free(key);
                if ((key = malloc(bytes)) == NULL) {
                    fprintf(stderr, "demo: out of memory\n");
                    status = 1;
                    break;
                }
                key_size = bytes;
            }
            memcpy(key, px, bytes);
            scene.fade_bias = 1;
            r = dm_scene_draw(&scene, frame, paused);
            scene.fade_bias = 0;
            if (r != JB_OK || (q = dm_scene_frame(&scene, &w, &h)) == NULL) {
                fprintf(stderr, "demo: tick %u: %s\n", tick, jb_error_string(scene.jb));
                status = 1;
                break;
            }
            for (b = 0; b < bytes; ++b) {
                int dlt = (int)q[b] - (int)key[b];
                dlt = dlt < 0 ? -dlt : dlt;
                if (dlt > faded)
                    faded = dlt, fade_tick = (int)tick;
            }
            px = key;
        }
        for (i = 0; i < o->npng; ++i)
            if (o->png[i].frame == tick) {
                if (!stbi_write_png(o->png[i].path, w, h, 4, px, w * 4)) {
                    fprintf(stderr, "demo: cannot write %s\n", o->png[i].path);
                    status = 1;
                    break;
                }
                printf("tick %u (frame %u): wrote %s (%d x %d)\n", tick, frame, o->png[i].path, w, h);
            }
        if (!paused)
            frame += o->step;
    }
    if (status == 0 && forwarding && check_forwarding(&scene, &sw) != 0)
        status = 1;
    if (o->gaps) {
        printf("gaps: %ld pixels over %u ticks\n", gapped, tick);
        if (gapped != 0)
            status = 1;
    }
    if (o->pier) {
        printf("pier: %ld pixels of posts over the deck over %u ticks\n", over, tick);
        if (over != 0)
            status = 1;
    }
    free(nopost);
    if (o->fade) {
        printf("fade: a level on changes a channel by %d at most (tick %d), of %d allowed\n", faded,
               fade_tick, o->fade);
        if (faded > o->fade)
            status = 1;
    }
    free(key);
    free(other);
    dm_scene_free(&scene);
    return status;
}

/* The whole of a file, NUL-terminated (free it), or NULL. */
static char *load(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    size_t cap = 4096, n = 0, got;
    char *buf = fp != NULL ? malloc(cap) : NULL;

    if (buf == NULL) {
        fprintf(stderr, "demo: cannot read %s\n", path);
        if (fp != NULL)
            fclose(fp);
        return NULL;
    }
    while ((got = fread(buf + n, 1, cap - n - 1, fp)) > 0) {
        char *more;
        n += got;
        if (cap - n - 1 > 0)
            continue;
        if ((more = realloc(buf, cap * 2)) == NULL)
            break;
        buf = more, cap *= 2;
    }
    if (ferror(fp) || !feof(fp) || memchr(buf, '\0', n) != NULL) {
        fprintf(stderr, "demo: cannot read %s (or it holds a NUL byte)\n", path);
        fclose(fp);
        free(buf);
        return NULL;
    }
    fclose(fp);
    buf[n] = '\0';
    *len = n;
    return buf;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

/* The runs of a file's text: '#' first on a line makes it a comment, a line
   of nothing but white space is skipped, and any other line is a run of
   COLUMNS columns separated by spaces or tabs, its hash 1 to 16 hex
   digits. 0, or 2 (refused) with the reason. */
static int read_runs(const char *path, const char *text, run *runs, int *n)
{
    const char *line = text;

    *n = 0;
    while (*line != '\0') {
        const char *end = line + strcspn(line, "\n"), *p = line;
        run *r = &runs[*n];
        int c = 0, too_long = 0;

        if (*line != '#') {
            while (p < end) {
                const char *start;
                while (p < end && is_space(*p))
                    ++p;
                if (p == end)
                    break;
                start = p;
                while (p < end && !is_space(*p))
                    ++p;
                if ((size_t)(p - start) >= sizeof r->col[0])
                    too_long = 1;
                else if (c < COLUMNS && *n < MAX_RUNS) {
                    memcpy(r->col[c], start, (size_t)(p - start));
                    r->col[c][p - start] = '\0';
                    if (c == COLUMNS - 1)
                        r->hash_at = (size_t)(start - text), r->hash_end = (size_t)(p - text);
                }
                ++c;
            }
            if (c != 0) {
                char *hash_end;
                const char *hash;
                if (c != COLUMNS || *n == MAX_RUNS || too_long) {
                    fprintf(stderr,
                            "demo: %s: a run line needs %d columns of at most 255 characters "
                            "(and at most %d runs): %.*s\n",
                            path, COLUMNS, MAX_RUNS, (int)(end - line), line);
                    return 2;
                }
                hash = r->col[COLUMNS - 1];
                r->hash = strtoull(hash, &hash_end, 16);
                if (strlen(hash) > 16 || strspn(hash, "0123456789abcdefABCDEF") != strlen(hash) ||
                    *hash_end != '\0') {
                    fprintf(stderr, "demo: %s: run %s: the hash %s is not 1 to 16 hex digits\n", path,
                            r->col[0], hash);
                    return 2;
                }
                ++*n;
            }
        }
        line = *end == '\n' ? end + 1 : end;
    }
    if (*n == 0) {
        fprintf(stderr, "demo: %s: no runs\n", path);
        return 2;
    }
    return 0;
}

/* A run's options: its columns as command-line options, parsed by the
   command line's parser over the defaults, with the caller's threads: 0,
   or 2 (refused). */
static int run_options(const run *r, int threads, options *o)
{
    static const char *const names[] = {"--frames",     "--step",  "--scale",      "--precise",
                                        "--perspective", "--dither", "--widescreen", "--float",
                                        "--clut-cache",  "--fog",    "--script"};
    enum { N = sizeof names / sizeof names[0] };
    char *argv[1 + 2 * N];
    int i;

    if (strcmp(r->col[1], "0") == 0) {
        fprintf(stderr, "demo: run %s: no frames\n", r->col[0]);
        return 2;
    }
    defaults(o);
    o->threads = threads;
    argv[0] = (char *)"demo-headless";
    for (i = 0; i < N; ++i) {
        argv[1 + 2 * i] = (char *)names[i];
        argv[2 + 2 * i] = (char *)r->col[1 + i];
    }
    return parse_options(1 + 2 * N, argv, 1, o) != 0 ? 2 : 0;
}

/* Reads FILE's runs and their options; 0, or the status to exit with. The
   text is kept in *text (free it) for --record. */
static int read_file(const char *path, int threads, char **text, size_t *len, run *runs,
                     options *opts, int *n)
{
    int st, i;

    if ((*text = load(path, len)) == NULL)
        return 1;
    if ((st = read_runs(path, *text, runs, n)) != 0)
        return free(*text), st;
    for (i = 0; i < *n; ++i)
        if ((st = run_options(&runs[i], threads, &opts[i])) != 0)
            return free(*text), st;
    return 0;
}

/* Runs FILE's runs named name (all of them for NULL) and compares. */
static int expect(const char *path, const char *name, int threads)
{
    static options opts[MAX_RUNS];
    static run runs[MAX_RUNS];
    size_t len;
    char *text;
    int n, i, j, done = 0, failures = 0, st;

    if ((st = read_file(path, threads, &text, &len, runs, opts, &n)) != 0)
        return st;
    free(text);
    for (i = 0; i < n; ++i)
        done += name == NULL || strcmp(name, runs[i].col[0]) == 0;
    if (done == 0) {
        fprintf(stderr, "demo: %s has no run %s\n", path, name ? name : "at all");
        return 2;
    }
    for (i = 0; i < n; ++i) {
        unsigned long long got = 0;

        if (name != NULL && strcmp(name, runs[i].col[0]) != 0)
            continue;
        if (render(&opts[i], 1, &got) != 0)
            ++failures;
        printf("%-8s %4u ticks, step %u, script %s, %d thread%s: %016llx", runs[i].col[0],
               opts[i].frames, opts[i].step, runs[i].col[11], threads, threads == 1 ? "" : "s", got);
        if (got == runs[i].hash) {
            printf(" ok\n");
        } else {
            printf(" FAIL, want %016llx\n", runs[i].hash);
            ++failures;
        }
    }
    /* Runs of the same length must not draw the same frames: a switch
       that is no longer passed on would. */
    if (name == NULL)
        for (i = 0; i < n; ++i)
            for (j = i + 1; j < n; ++j)
                if (opts[i].frames == opts[j].frames && runs[i].hash == runs[j].hash) {
                    printf("FAIL: %s and %s draw the same frames\n", runs[i].col[0], runs[j].col[0]);
                    ++failures;
                }
    return failures != 0;
}

/* Puts the file tmp in path's place in one step: 1, or 0. */
static int replace(const char *tmp, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, path) == 0; /* POSIX: atomic, replacing path */
#endif
}

/* --record: every run of FILE drawn, and FILE written again with each
   run's hash column replaced by the hash it now gives, every other byte as
   it was, through FILE.N.tmp (created exclusively) renamed over FILE. */
static int record(const char *path, int threads)
{
    static options opts[MAX_RUNS];
    static run runs[MAX_RUNS];
    char tmp[4096], hash[17];
    size_t len, at = 0;
    char *text;
    FILE *fp = NULL;
    int n, i, ok;

    if (strlen(path) + 16 > sizeof tmp) {
        fprintf(stderr, "demo: the path %s is too long\n", path);
        return 2;
    }
    if ((ok = read_file(path, threads, &text, &len, runs, opts, &n)) != 0)
        return ok;
    for (i = 0; i < n; ++i)
        if (render(&opts[i], 0, &runs[i].hash) != 0) {
            free(text);
            return 1;
        }
    for (i = 0; i < 1000 && fp == NULL; ++i) {
        sprintf(tmp, "%s.%d.tmp", path, i);
        fp = fopen(tmp, "wbx");
    }
    if (fp == NULL) {
        fprintf(stderr, "demo: cannot create a file beside %s to write\n", path);
        free(text);
        return 1;
    }
    ok = 1;
    for (i = 0; i < n; ++i) {
        sprintf(hash, "%016llx", runs[i].hash);
        ok = ok && fwrite(text + at, 1, runs[i].hash_at - at, fp) == runs[i].hash_at - at &&
             fwrite(hash, 1, 16, fp) == 16;
        at = runs[i].hash_end;
        printf("%-8s %016llx\n", runs[i].col[0], runs[i].hash);
    }
    ok = ok && fwrite(text + at, 1, len - at, fp) == len - at;
    ok = fclose(fp) == 0 && ok;
    free(text);
    if (!ok || !replace(tmp, path)) {
        fprintf(stderr, "demo: cannot write %s over %s; %s is unchanged\n", tmp, path, path);
        remove(tmp);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static options o;
    unsigned long long all;
    int r;

    defaults(&o);
    if ((r = parse_options(argc, argv, 1, &o)) != 0)
        return r;
    if (o.record_file != NULL)
        return record(o.record_file, o.threads);
    if (o.expect_file != NULL)
        return expect(o.expect_file, o.expect_name, o.threads);
    if (render(&o, 0, &all) != 0)
        return 1;
    printf("all %u ticks: %016llx\n", o.frames, all);
    return 0;
}
