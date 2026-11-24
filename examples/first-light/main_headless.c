/*
 * first-light-headless: the scene without a window. It draws frames 0 to
 * N-1 with the switches given, prints a hash of each frame asked for and
 * one of all the frames, and can write a frame as a PNG. The hash is
 * FNV-1a 64 over the RGBA bytes jb_frame gives, frame after frame.
 *
 *   first-light-headless [options]
 *     --frames N        frames to draw (300)
 *     --scale S         internal scale, 1 to 8 (1)
 *     --precise 0|1     precise vertices (0)
 *     --perspective 0|1 perspective-correct textures (0)
 *     --dither 0|1      dithering (1)
 *     --widescreen 0|1  the 16:9 display, 427 x 240 (0)
 *     --float 0|1       the float geometry path instead of the GTE (0)
 *     --clut-cache 0|1  the CLUT cache (1)
 *     --pause-at F      pause at frame F: every later frame draws scene
 *                       frame F again, as a paused window does (none)
 *     --threads T       rasterising threads (1; 0 for one per CPU)
 *     --hash F          print the hash of frame F (repeatable)
 *     --png F PATH      write frame F as a PNG (repeatable)
 *     --expect FILE [NAME]
 *                       draw the run NAME of FILE (every run if no NAME)
 *                       and compare its hash with the one FILE gives
 *     --record FILE     draw every run of FILE and write FILE again with
 *                       the hashes the runs now give
 *
 * FILE (expected.txt) has a run a line, '#' starting a comment line:
 *   name frames scale precise perspective dither widescreen float clutcache hash
 * columns separated by spaces or tabs. A run's columns become the options
 * above (--frames N --scale S ... --clut-cache C) and go through the same
 * parser as the command line, so a run draws exactly what those options
 * draw; --threads applies to every run. After each run --expect also checks
 * that the switches reached the context where the picture cannot show it
 * (the CLUT cache, by a probe; dithering, by GPUSTAT; the frame's size).
 * The exit status is 1 on any mismatch.
 *
 * Numbers must be whole and in range (--frames 0..1000000, --scale 1..8,
 * the switches 0 or 1, --threads 0..64, frames 0..1000000000); anything
 * else is refused, with exit status 2, before anything is drawn or
 * written. A run file is read whole and every run in it checked first, so
 * one it cannot use (a line without its columns, a number as above, a
 * frame count outside 1..1000000, a hash not 1 to 16 hex digits) is
 * refused the same way, with nothing drawn, printed on the standard output
 * or written. Status 1 is a mismatch, or a file that cannot be read or
 * written.
 *
 * --record rewrites only each run's hash column, everything else of the
 * file kept byte for byte. It writes a new file beside FILE (FILE.N.tmp,
 * created exclusively, so concurrent recordings never share one) and puts
 * it in FILE's place with one atomic rename only once it is complete; on
 * any failure FILE is left as it was.
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

enum { MAX_ASKED = 64, MAX_RUNS = 32, COLUMNS = 10 };

typedef struct {
    unsigned frame;
    const char *path;
} png_request;

/* Everything the command line says. */
typedef struct {
    fl_switches sw;
    unsigned frames;
    long pause_at; /* -1: none */
    int threads;
    unsigned hash_at[MAX_ASKED];
    int nhash;
    png_request png[MAX_ASKED];
    int npng;
    const char *expect_file, *expect_name, *record_file;
} options;

/* A run of FILE: its columns as written, and where its hash column is in
   the file's text (for --record). */
typedef struct {
    char col[COLUMNS][32];
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
        fprintf(stderr, "first-light-headless: %s %s: not a whole number %ld..%ld\n", opt, s,
                lo, hi);
        return 0;
    }
    *out = v;
    return 1;
}

enum { MAX_FRAME = 1000000000 };

/* The options in argv[first..argc), over what *o holds: 0, or 2 for one
   that is unknown, incomplete or out of range. */
static int parse_options(int argc, char **argv, int first, options *o)
{
    int i;

    for (i = first; i < argc; ++i) {
        const char *a = argv[i];
        int more = argc - i - 1, ok = 1;
        long v = 0;

        if (strcmp(a, "--frames") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 1000000, &v), o->frames = (unsigned)v;
        else if (strcmp(a, "--scale") == 0 && more >= 1)
            ok = number(a, argv[++i], 1, 8, &v), o->sw.scale = (int)v;
        else if (strcmp(a, "--precise") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 1, &v), o->sw.precise = (int)v;
        else if (strcmp(a, "--perspective") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 1, &v), o->sw.perspective = (int)v;
        else if (strcmp(a, "--dither") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 1, &v), o->sw.dither = (int)v;
        else if (strcmp(a, "--widescreen") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 1, &v), o->sw.widescreen = (int)v;
        else if (strcmp(a, "--float") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 1, &v),
            o->sw.geometry = v ? JB_GEOMETRY_FLOAT : JB_GEOMETRY_GTE;
        else if (strcmp(a, "--clut-cache") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 1, &v), o->sw.clut_cache = (int)v;
        else if (strcmp(a, "--pause-at") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, MAX_FRAME, &v), o->pause_at = v;
        else if (strcmp(a, "--record") == 0 && more >= 1)
            o->record_file = argv[++i];
        else if (strcmp(a, "--threads") == 0 && more >= 1)
            ok = number(a, argv[++i], 0, 64, &v), o->threads = (int)v;
        else if (strcmp(a, "--hash") == 0 && more >= 1 && o->nhash < MAX_ASKED)
            ok = number(a, argv[++i], 0, MAX_FRAME, &v), o->hash_at[o->nhash++] = (unsigned)v;
        else if (strcmp(a, "--png") == 0 && more >= 2 && o->npng < MAX_ASKED) {
            ok = number(a, argv[++i], 0, MAX_FRAME, &v);
            o->png[o->npng].frame = (unsigned)v;
            o->png[o->npng++].path = argv[++i];
        } else if (strcmp(a, "--expect") == 0 && more >= 1) {
            o->expect_file = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-')
                o->expect_name = argv[++i];
        } else {
            fprintf(stderr, "first-light-headless: unknown or incomplete option %s\n"
                            "(see the comment at the top of main_headless.c)\n",
                    a);
            return 2;
        }
        if (!ok)
            return 2;
    }
    return 0;
}

static void defaults(options *o)
{
    fl_switches console = FL_SWITCHES_CONSOLE;

    memset(o, 0, sizeof *o);
    o->sw = console;
    o->frames = 300;
    o->pause_at = -1;
    o->threads = 1;
}

/*
 * Whether the switches reached the context, where the picture does not
 * show it: GPUSTAT's dithering bit, the frame's size (scale, widescreen),
 * and the CLUT cache, by a probe after the run's last frame: a 4-bit pixel
 * drawn red, its palette entry rewritten green in VRAM with no GP0(01h),
 * the pixel drawn again beside it: red and red with the cache on, red and
 * green with it off, and nothing else (every call of the probe must
 * succeed too). The number of mismatches, each printed.
 */
static int check_forwarding(fl_scene *s, const fl_switches *sw)
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

/* Draws the frames o asks for (scene frame f, or pause_at once past it);
   prints the hashes asked for, writes the PNGs asked for, and gives the
   hash of all frames in *all. With forwarding, checks the switches after
   the last frame (check_forwarding). 0, or 1 on an error or mismatch. */
static int render(const options *o, int forwarding, unsigned long long *all)
{
    fl_scene scene;
    jb_result r;
    unsigned f;
    int i, status = 0;

    if ((r = fl_scene_init(&scene, &o->sw, o->threads)) != JB_OK) {
        fprintf(stderr, "first-light: cannot start: %s\n", jb_result_string(r));
        return 1;
    }
    *all = fnv_basis;
    for (f = 0; f < o->frames && status == 0; ++f) {
        unsigned shown = o->pause_at >= 0 && f > (unsigned long)o->pause_at
                             ? (unsigned)o->pause_at : f;
        const uint8_t *px;
        size_t bytes;
        int w, h;

        if ((r = fl_scene_draw(&scene, shown)) != JB_OK) {
            fprintf(stderr, "first-light: frame %u: %s (%s)\n", f, jb_result_string(r),
                    jb_error_string(scene.jb));
            status = 1;
            break;
        }
        if ((px = fl_scene_frame(&scene, &w, &h)) == NULL) {
            fprintf(stderr, "first-light: frame %u: no frame\n", f);
            status = 1;
            break;
        }
        bytes = (size_t)w * (size_t)h * 4;
        *all = fnv1a(*all, px, bytes);
        for (i = 0; i < o->nhash; ++i)
            if (o->hash_at[i] == f)
                printf("frame %u: %016llx\n", f, fnv1a(fnv_basis, px, bytes));
        for (i = 0; i < o->npng; ++i)
            if (o->png[i].frame == f) {
                if (!stbi_write_png(o->png[i].path, w, h, 4, px, w * 4)) {
                    fprintf(stderr, "first-light: cannot write %s\n", o->png[i].path);
                    status = 1;
                    break;
                }
                printf("frame %u: wrote %s (%d x %d)\n", f, o->png[i].path, w, h);
            }
    }
    if (status == 0 && forwarding && check_forwarding(&scene, &o->sw) != 0)
        status = 1;
    fl_scene_free(&scene);
    return status;
}

/* The whole of a file, NUL-terminated (free it), or NULL. */
static char *load(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    size_t cap = 4096, n = 0, got;
    char *buf = fp != NULL ? malloc(cap) : NULL;

    if (buf == NULL) {
        fprintf(stderr, "first-light: cannot read %s\n", path);
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
    if (ferror(fp) || !feof(fp)) {
        fprintf(stderr, "first-light: cannot read %s\n", path);
        fclose(fp);
        free(buf);
        return NULL;
    }
    fclose(fp);
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* 1 when text, len bytes, holds a NUL: the reader stops at the first one,
   so a file with one is refused rather than read in part. */
static int has_nul(const char *path, const char *text, size_t len)
{
    if (memchr(text, '\0', len) == NULL)
        return 0;
    fprintf(stderr, "first-light: %s has a NUL byte\n", path);
    return 1;
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
                            "first-light: %s: a run line needs %d columns of at most 31 "
                            "characters (and at most %d runs): %.*s\n",
                            path, COLUMNS, MAX_RUNS, (int)(end - line), line);
                    return 2;
                }
                hash = r->col[COLUMNS - 1];
                r->hash = strtoull(hash, &hash_end, 16);
                if (strlen(hash) > 16 || strspn(hash, "0123456789abcdefABCDEF") != strlen(hash) ||
                    *hash_end != '\0') {
                    fprintf(stderr, "first-light: %s: run %s: the hash %s is not 1 to 16 hex "
                                    "digits\n",
                            path, r->col[0], hash);
                    return 2;
                }
                ++*n;
            }
        }
        line = *end == '\n' ? end + 1 : end;
    }
    if (*n == 0) {
        fprintf(stderr, "first-light: %s: no runs\n", path);
        return 2;
    }
    return 0;
}

/* A run's options: its columns as command-line options, parsed by the
   command line's parser over the defaults, with the caller's threads: 0,
   or 2 (refused). */
static int run_options(const run *r, int threads, options *o)
{
    static const char *const names[] = {"--frames",     "--scale",      "--precise",
                                        "--perspective", "--dither",     "--widescreen",
                                        "--float",       "--clut-cache"};
    char *argv[1 + 2 * 8];
    long count;
    int i;

    /* Digits only, 1 to 1,000,000 (9 digits can't overflow a long). */
    count = strlen(r->col[1]) <= 9 && strspn(r->col[1], "0123456789") == strlen(r->col[1])
                ? strtol(r->col[1], NULL, 10) : 0;
    if (count < 1 || count > 1000000) {
        fprintf(stderr, "first-light: run %s: bad frame count %s\n", r->col[0], r->col[1]);
        return 2;
    }
    defaults(o);
    o->threads = threads;
    argv[0] = (char *)"first-light-headless";
    for (i = 0; i < 8; ++i) {
        argv[1 + 2 * i] = (char *)names[i];
        argv[2 + 2 * i] = (char *)r->col[1 + i];
    }
    return parse_options(1 + 2 * 8, argv, 1, o) != 0 ? 2 : 0;
}

static void print_switches(const fl_switches *sw)
{
    printf("scale %d, precise %d, perspective %d, dither %d, widescreen %d, float %d, "
           "CLUT cache %d",
           sw->scale, sw->precise, sw->perspective, sw->dither, sw->widescreen, sw->geometry,
           sw->clut_cache);
}

/* Draws the runs of FILE named name (all of them for NULL) and compares.
   The whole file is read and every run checked before anything is drawn:
   a file it cannot use is refused (2) with nothing drawn or printed. */
static int expect(const char *path, const char *name, int threads)
{
    static options opts[MAX_RUNS];
    run runs[MAX_RUNS];
    size_t len;
    char *text = load(path, &len);
    int n, i, j, done = 0, failures = 0, st;

    if (text == NULL)
        return 1;
    if (has_nul(path, text, len)) {
        free(text);
        return 2;
    }
    st = read_runs(path, text, runs, &n);
    free(text);
    if (st != 0)
        return st;
    for (i = 0; i < n; ++i)
        if ((st = run_options(&runs[i], threads, &opts[i])) != 0)
            return st;
    for (i = 0; i < n; ++i)
        done += name == NULL || strcmp(name, runs[i].col[0]) == 0;
    if (done == 0) {
        fprintf(stderr, "first-light: %s has no run %s\n", path, name ? name : "at all");
        return 2;
    }
    for (i = 0; i < n; ++i) {
        unsigned long long got = 0;
        const options o = opts[i];

        if (name != NULL && strcmp(name, runs[i].col[0]) != 0)
            continue;
        if (render(&o, 1, &got) != 0)
            ++failures;
        printf("%-10s %4u frames, ", runs[i].col[0], o.frames);
        print_switches(&o.sw);
        printf(", %d thread%s: %016llx", threads, threads == 1 ? "" : "s", got);
        if (got == runs[i].hash) {
            printf(" ok\n");
        } else {
            printf(" FAIL, want %016llx\n", runs[i].hash);
            ++failures;
        }
    }
    /* Runs of the same length must not draw the same frames: a switch
       that is no longer passed on would. (The panel shows each switch, so
       this alone does not prove an option still changes the drawing; the
       library's own tests do that, and check_forwarding the switches the
       picture does not show.) */
    if (name == NULL)
        for (i = 0; i < n; ++i)
            for (j = i + 1; j < n; ++j)
                if (strcmp(runs[i].col[1], runs[j].col[1]) == 0 && runs[i].hash == runs[j].hash) {
                    printf("FAIL: %s and %s draw the same frames\n", runs[i].col[0],
                           runs[j].col[0]);
                    ++failures;
                }
    return failures != 0;
}

/* Puts the file tmp in path's place in one step, path replaced whole or
   not at all (never removed first): 1, or 0. */
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
   it was. Every run's options are checked before anything is drawn; the
   new text goes to a file of this call's own beside FILE (FILE.N.tmp,
   created exclusively: "x"), which then replaces FILE. */
static int record(const char *path, int threads)
{
    static options opts[MAX_RUNS];
    run runs[MAX_RUNS];
    char tmp[4096], hash[17];
    size_t len, at = 0;
    char *text = load(path, &len);
    FILE *fp = NULL;
    int n, i, ok;

    if (text == NULL)
        return 1;
    if (has_nul(path, text, len)) {
        free(text);
        return 2;
    }
    if (strlen(path) + 16 > sizeof tmp) {
        fprintf(stderr, "first-light: the path %s is too long\n", path);
        free(text);
        return 2;
    }
    if ((ok = read_runs(path, text, runs, &n)) != 0) {
        free(text);
        return ok;
    }
    for (i = 0; i < n; ++i)
        if ((ok = run_options(&runs[i], threads, &opts[i])) != 0) {
            free(text);
            return ok;
        }
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
        fprintf(stderr, "first-light: cannot create a file beside %s to write\n", path);
        free(text);
        return 1;
    }
    ok = 1;
    for (i = 0; i < n; ++i) {
        sprintf(hash, "%016llx", runs[i].hash);
        ok = ok && fwrite(text + at, 1, runs[i].hash_at - at, fp) == runs[i].hash_at - at &&
             fwrite(hash, 1, 16, fp) == 16;
        at = runs[i].hash_end;
        printf("%-10s %016llx\n", runs[i].col[0], runs[i].hash);
    }
    ok = ok && fwrite(text + at, 1, len - at, fp) == len - at;
    ok = fclose(fp) == 0 && ok;
    free(text);
    if (!ok) {
        fprintf(stderr, "first-light: cannot write %s; %s is unchanged\n", tmp, path);
        remove(tmp);
        return 1;
    }
    if (!replace(tmp, path)) {
        fprintf(stderr, "first-light: cannot replace %s with %s; %s is unchanged\n", path, tmp,
                path);
        remove(tmp);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    options o;
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
    printf("all %u frames (", o.frames);
    print_switches(&o.sw);
    if (o.pause_at >= 0)
        printf(", paused at %ld", o.pause_at);
    printf("): %016llx\n", all);
    return 0;
}
