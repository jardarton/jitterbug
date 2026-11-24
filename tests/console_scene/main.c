/*
 * test_console_scene: the console-level first-light scene (scene.c here)
 * without a window, as examples/first-light's headless runner drove it
 * before the example moved to the friendly level. It draws frames 0 to N-1
 * with the switches given and prints a hash of each frame asked for and one
 * of all the frames: FNV-1a 64 over the RGBA bytes jb_frame gives, frame
 * after frame.
 *
 *   test_console_scene [options]
 *     --frames N        frames to draw (300)
 *     --scale S         internal scale, 1 to 8 (1)
 *     --precise 0|1     precise vertices (0)
 *     --perspective 0|1 perspective-correct textures (0)
 *     --dither 0|1      dithering (1)
 *     --threads T       rasterising threads (1; 0 for one per CPU)
 *     --hash F          print the hash of frame F (repeatable)
 *     --expect FILE [NAME]
 *                       draw the run NAME of FILE (every run if no NAME)
 *                       with its frames and switches, and compare its
 *                       hash with the one FILE gives; --threads still
 *                       applies. The exit status is 1 on any mismatch.
 *
 * FILE (expected.txt) has a run a line, '#' starting a comment:
 *   name frames scale precise perspective dither hash
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scene.h"

enum { MAX_ASKED = 64, MAX_RUNS = 32 };

typedef struct {
    char name[32];
    unsigned frames;
    fl_switches sw;
    unsigned long long hash;
} run;

static unsigned long long fnv1a(unsigned long long h, const uint8_t *p, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static const unsigned long long fnv_basis = 0xcbf29ce484222325ull;

/* Draws frames 0..frames-1; prints the hashes asked for and gives the
   hash of all frames in *all. 0, or 1 on an error. */
static int render(const fl_switches *sw, int threads, unsigned frames, const unsigned *hash_at,
                  int nhash, unsigned long long *all)
{
    fl_scene scene;
    jb_result r;
    unsigned f;
    int i;

    if ((r = fl_scene_init(&scene, sw, threads)) != JB_OK) {
        fprintf(stderr, "first-light: cannot start: %s\n", jb_result_string(r));
        return 1;
    }
    *all = fnv_basis;
    for (f = 0; f < frames; ++f) {
        const uint8_t *px;
        size_t bytes;
        int w, h;

        if ((r = fl_scene_draw(&scene, f)) != JB_OK) {
            fprintf(stderr, "first-light: frame %u: %s (%s)\n", f, jb_result_string(r),
                    jb_error_string(scene.jb));
            fl_scene_free(&scene);
            return 1;
        }
        px = fl_scene_frame(&scene, &w, &h);
        if (px == NULL) {
            fprintf(stderr, "first-light: frame %u: no frame\n", f);
            fl_scene_free(&scene);
            return 1;
        }
        bytes = (size_t)w * (size_t)h * 4;
        *all = fnv1a(*all, px, bytes);
        for (i = 0; i < nhash; ++i)
            if (hash_at[i] == f)
                printf("frame %u: %016llx\n", f, fnv1a(fnv_basis, px, bytes));
    }
    fl_scene_free(&scene);
    return 0;
}

static int read_runs(const char *path, run *runs, int *n)
{
    FILE *fp = fopen(path, "r");
    char line[256];

    *n = 0;
    if (fp == NULL) {
        fprintf(stderr, "first-light: cannot open %s\n", path);
        return 1;
    }
    while (fgets(line, sizeof line, fp) != NULL) {
        run *r = &runs[*n];
        char *hash_end;
        char hash[32], frames[32];
        long count;

        if (line[0] == '#' || strspn(line, " \t\r\n") == strlen(line))
            continue;
        if (*n == MAX_RUNS ||
            sscanf(line, "%31s %31s %d %d %d %d %31s", r->name, frames, &r->sw.scale,
                   &r->sw.precise, &r->sw.perspective, &r->sw.dither, hash) != 7) {
            fprintf(stderr, "first-light: %s: cannot read the line: %s", path, line);
            fclose(fp);
            return 1;
        }
        r->sw.log = NULL;
        r->sw.log_user = NULL;
        r->hash = strtoull(hash, &hash_end, 16);
        if (*hash_end != '\0') {
            fprintf(stderr, "first-light: %s: bad hash %s\n", path, hash);
            fclose(fp);
            return 1;
        }
        /* Digits only, 1 to 1,000,000 (9 digits can't overflow a long). */
        count = strlen(frames) <= 9 && strspn(frames, "0123456789") == strlen(frames)
                    ? strtol(frames, NULL, 10) : 0;
        if (count < 1 || count > 1000000) {
            fprintf(stderr, "first-light: %s: run %s: bad frame count %s\n", path, r->name,
                    frames);
            fclose(fp);
            return 1;
        }
        r->frames = (unsigned)count;
        ++*n;
    }
    fclose(fp);
    if (*n == 0) {
        fprintf(stderr, "first-light: %s: no runs\n", path);
        return 1;
    }
    return 0;
}

static void print_switches(const fl_switches *sw)
{
    printf("scale %d, precise %d, perspective %d, dither %d", sw->scale, sw->precise,
           sw->perspective, sw->dither);
}

/* Draws the runs of FILE named name (all of them for NULL) and compares. */
static int expect(const char *path, const char *name, int threads)
{
    run runs[MAX_RUNS];
    int n, i, j, done = 0, failures = 0;

    if (read_runs(path, runs, &n) != 0)
        return 1;
    for (i = 0; i < n; ++i) {
        unsigned long long got;

        if (name != NULL && strcmp(name, runs[i].name) != 0)
            continue;
        ++done;
        if (render(&runs[i].sw, threads, runs[i].frames, NULL, 0, &got) != 0)
            return 1;
        printf("%-10s %4u frames, ", runs[i].name, runs[i].frames);
        print_switches(&runs[i].sw);
        printf(", %d thread%s: %016llx", threads, threads == 1 ? "" : "s", got);
        if (got == runs[i].hash) {
            printf(" ok\n");
        } else {
            printf(" FAIL, want %016llx\n", runs[i].hash);
            ++failures;
        }
    }
    if (done == 0) {
        fprintf(stderr, "first-light: %s has no run %s\n", path, name ? name : "at all");
        return 1;
    }
    /* Runs of the same length must not draw the same frames: a switch
       that is no longer passed on would. (The panel shows each switch, so
       this alone does not prove an option still changes the drawing;
       tests/api_options.c does that.) */
    if (name == NULL)
        for (i = 0; i < n; ++i)
            for (j = i + 1; j < n; ++j)
                if (runs[i].frames == runs[j].frames && runs[i].hash == runs[j].hash) {
                    printf("FAIL: %s and %s draw the same frames\n", runs[i].name,
                           runs[j].name);
                    ++failures;
                }
    return failures != 0;
}

static int flag(const char *s)
{
    return atoi(s) != 0;
}

int main(int argc, char **argv)
{
    fl_switches sw = FL_SWITCHES_CONSOLE;
    unsigned hash_at[MAX_ASKED], frames = 300;
    int nhash = 0, threads = 1, i;
    const char *expect_file = NULL, *expect_name = NULL;
    unsigned long long all;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        int more = argc - i - 1;

        if (strcmp(a, "--frames") == 0 && more >= 1)
            frames = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(a, "--scale") == 0 && more >= 1)
            sw.scale = atoi(argv[++i]);
        else if (strcmp(a, "--precise") == 0 && more >= 1)
            sw.precise = flag(argv[++i]);
        else if (strcmp(a, "--perspective") == 0 && more >= 1)
            sw.perspective = flag(argv[++i]);
        else if (strcmp(a, "--dither") == 0 && more >= 1)
            sw.dither = flag(argv[++i]);
        else if (strcmp(a, "--threads") == 0 && more >= 1)
            threads = atoi(argv[++i]);
        else if (strcmp(a, "--hash") == 0 && more >= 1 && nhash < MAX_ASKED)
            hash_at[nhash++] = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (strcmp(a, "--expect") == 0 && more >= 1) {
            expect_file = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-')
                expect_name = argv[++i];
        } else {
            fprintf(stderr, "test_console_scene: unknown or incomplete option %s\n"
                            "(see the comment at the top of tests/console_scene/main.c)\n",
                    a);
            return 2;
        }
    }
    if (expect_file != NULL)
        return expect(expect_file, expect_name, threads);
    if (render(&sw, threads, frames, hash_at, nhash, &all) != 0)
        return 1;
    printf("all %u frames (", frames);
    print_switches(&sw);
    printf("): %016llx\n", all);
    return 0;
}
