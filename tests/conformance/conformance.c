/*
 * conformance: plays one ps1-tests program's recorded list (FORMAT.md) on
 * a context through the public API, at 1x or (--scale) above it, and
 * compares what it can with the console; above 1x it is the 1x VRAM kept
 * beside the scaled picture (jb_vram_read) that is compared:
 *   - for a dump fixture, the whole 1024 x 512 VRAM against the console's
 *     dump, on the 15 colour bits (the dumps hold each 5-bit channel
 *     shifted left by 3 and no mask bit);
 *   - the list's EXPECT checks (what the console read back);
 *   - the list's waits (a wait that cannot end is a difference too), and
 *     that the main loop's VRAM and readable GPU state stay as at the
 *     stated pass through the window (FORMAT.md, "Playing a list").
 * With --picture DIR it also writes all of VRAM as the scaled picture shows
 * it (jb_frame after GP1(00h), which keeps VRAM) as DIR/NAME.Nx.png, for a
 * look at the scaled output.
 *
 * The fixture table is the authority (FORMAT.md, "Fixture tables"): the runner is
 * given the table and a program's NAME and reads the program's whole
 * identity from its line, the reference kind, the pass compared, the
 * number of EXPECT ops, the size, the checksum and the exclusion (only if
 * the line names one, and only for the dump), and refuses a list that is
 * not that fixture. Nothing of it can be stated on the command line. The
 * dump is the table's too: third_party/ps1-tests/gpu/NAME/vram.png beside
 * the table's tests/conformance for a repo line, beside the list's for a
 * fetched one (the layout of the conformance-lists output), and so is a
 * log program's psx.log there; either file must have the FNV-1a the line
 * states (REFSUM), checked before anything is compared. A program in
 * expected-failures.txt beside the table is judged as an expected
 * mismatch (jbc_judge).
 *
 * With --probe RESULT.jbpr --platform console|simulation (tools/probe) the
 * reference is what the hardware probe read back instead of the table's:
 * all 16 bits of VRAM are compared, the mask bit too, every word the list
 * read (GPUSTAT without its timing bits 25-28 and 31), and each EXPECT's
 * outcome and value against the console's. The result must be bound to the
 * fixture (of this list, its name, length and CRC-32; played to the stated
 * pass with the two-pass window; from the platform stated) and clean
 * (jbp_result_clean: complete, sound, steady, no wait, DMA or vertical
 * blank that timed out; every EXPECT of the list evaluated, and no other),
 * or it is refused. No pixel is excluded. A check the console failed is
 * reported, and fails the run (jbc_judge). Pixels differing only in the
 * mask bit are yellow in NAME.diff.png.
 * With --probe-fixtures TABLE (tests/conformance/probe/fixtures.txt) the
 * reference is the console result NAME.jbpr beside that table, bound to the
 * fixture its line names (../fixtures.txt or ../open/fixtures.txt), from a
 * console, with the exclusion that line names, if any.
 *
 * It reports the number of differing pixels and their bounding box, and
 * with --out writes NAME.ours.png (our VRAM: R, G, B = channel << 3,
 * alpha 128 where the mask bit is set, else 255, as tests/seed's golden
 * images) and NAME.diff.png (differing pixels magenta, the others the
 * console's colour at a quarter of its brightness, excluded pixels that
 * differ blue) there.
 *
 *   conformance (--fixtures TABLE | --probe-fixtures TABLE) --fixture NAME
 *               [--probe RESULT.jbpr --platform console|simulation]
 *               [--threads N] [--scale N] [--split op|words|random]
 *               [--out DIR] [--picture DIR] LIST.jbc
 *
 * Exit status: 0 when everything matches; 1 when something differs; 2 for
 * an error (a usage error, a table, list or dump that cannot be read, a
 * list that is not the fixture, a dump or log that is not the file its
 * line states, a probe result not bound to it or not
 * clean, an exclusion file that is not the set its line states). For a
 * program in expected-failures.txt a VRAM, check or read difference gives 0
 * and a full match gives 1, so that the list must shrink when a fix lands;
 * a wait that would never end or a main loop that does not settle still
 * gives 1, and errors 2 (jbc_judge).
 */
#include "jbc.h"
#include "jbp_result.h"

#include <stb_image_write.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_png(const char *dir, const char *base, const char *suffix,
                     const unsigned char *rgba)
{
    char path[1024];

    snprintf(path, sizeof path, "%s/%s.%s.png", dir, base, suffix);
    if (!stbi_write_png(path, JBC_VRAM_W, JBC_VRAM_H, 4, rgba, JBC_VRAM_W * 4)) {
        printf("cannot write %s\n", path);
        return -1;
    }
    printf("wrote %s\n", path);
    return 0;
}

/* The bits compared: 0x7fff for a dump, 0xffff with --probe. */
static unsigned cmp_bits = 0x7fffu;

static int write_images(const char *dir, const char *base, const uint16_t *ours,
                        const uint16_t *hw, const unsigned char *excluded)
{
    unsigned char *img = malloc((size_t)JBC_VRAM_PIXELS * 4);
    size_t i;
    int r = 0;

    if (img == NULL)
        return -1;
    for (i = 0; i < JBC_VRAM_PIXELS; ++i) {
        unsigned p = ours[i];
        img[4 * i + 0] = (unsigned char)((p & 31) << 3);
        img[4 * i + 1] = (unsigned char)(((p >> 5) & 31) << 3);
        img[4 * i + 2] = (unsigned char)(((p >> 10) & 31) << 3);
        img[4 * i + 3] = (p & 0x8000) ? 128 : 255;
    }
    r |= write_png(dir, base, "ours", img);
    if (hw != NULL) {
        for (i = 0; i < JBC_VRAM_PIXELS; ++i) {
            unsigned h = hw[i] & cmp_bits;
            if ((ours[i] & cmp_bits) != h) {
                int ex = excluded != NULL && excluded[i];
                int mask_only = ((ours[i] ^ h) & 0x7fffu) == 0;
                img[4 * i + 0] = ex ? 0 : 255;
                img[4 * i + 1] = mask_only && !ex ? 255 : 0;
                img[4 * i + 2] = mask_only && !ex ? 0 : 255;
            } else {
                img[4 * i + 0] = (unsigned char)((h & 31) << 1);
                img[4 * i + 1] = (unsigned char)(((h >> 5) & 31) << 1);
                img[4 * i + 2] = (unsigned char)(((h >> 10) & 31) << 1);
            }
            img[4 * i + 3] = 255;
        }
        r |= write_png(dir, base, "diff", img);
    }
    free(img);
    return r;
}

static int usage(void)
{
    printf("usage: conformance (--fixtures TABLE | --probe-fixtures TABLE) --fixture NAME "
           "[--probe RESULT.jbpr --platform console|simulation] [--threads N] [--scale N] "
           "[--split op|words|random] [--out DIR] [--picture DIR] LIST.jbc\n");
    return 2;
}

/* The directory part of path ("." for none), into dir. */
static void dir_of(const char *path, char *dir, size_t size)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL)
        snprintf(dir, size, ".");
    else
        snprintf(dir, size, "%.*s", (int)(slash - path), path);
}

/* out = a b c d (any may be ""), or 1 in *bad when it does not fit. */
static void join(char *out, size_t size, const char *a, const char *b, const char *c, const char *d,
                 int *bad)
{
    const char *part[4];
    size_t at = 0, k, n;

    part[0] = a;
    part[1] = b;
    part[2] = c;
    part[3] = d;
    out[0] = 0;
    for (k = 0; k < 4; ++k) {
        n = strlen(part[k]);
        if (at + n >= size) {
            *bad = 1;
            out[0] = 0;
            return;
        }
        memcpy(out + at, part[k], n);
        at += n;
        out[at] = 0;
    }
}

/* Whether path names a file that can be opened. */
static int readable(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return 0;
    fclose(f);
    return 1;
}

/* Whether expected-failures.txt in dir names key (a program name first on
   a line; # starts a comment). No file: no. */
static int expected_failure(const char *dir, const char *key)
{
    char path[1024], line[512];
    FILE *f;
    int found = 0;

    join(path, sizeof path, dir, "/expected-failures.txt", "", "", &found);
    if (found || (f = fopen(path, "r")) == NULL)
        return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strcspn(line, "# \t\r\n");
        if (n > 0 && n == strlen(key) && strncmp(line, key, n) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}

/*
 * A line of tests/conformance/probe/fixtures.txt (tests/conformance/FORMAT.md,
 * "Console probe results table (`probe/fixtures.txt`)"):
 * NAME TABLE [EXCLUDE PIXELS CHECKSUM], TABLE ps1-tests or open. 0, or -1
 * with the reason.
 */
typedef struct probe_line {
    char table[16];
    char exclude[128];
    unsigned long long pixels, checksum;
} probe_line;

/* Whether s is 1 to max characters, each of the set. */
static int all_of(const char *s, const char *set, size_t max)
{
    size_t n = strlen(s);

    return n >= 1 && n <= max && strspn(s, set) == n;
}

/* The whole table is read and checked before the line for key is taken:
   every line well formed, its optional exclusion fields too, and no NAME
   on two lines (FORMAT.md, "Fixture tables"). */
static int probe_lookup(const char *table, const char *key, probe_line *pl, char *err, size_t size)
{
    static const char names[] = "abcdefghijklmnopqrstuvwxyz0123456789-";
    /* Read whole, once; a zero byte is refused (jbc_read_text). */
    const char *why = NULL;
    size_t tsize = 0, tpos = 0;
    char *t = jbc_read_text(table, &tsize, &why);
    char line[512];
    unsigned lineno = 0, n = 0, cap = 0, k, found = 0;
    int more_lines;
    struct seen {
        char name[128];
        unsigned line;
    } *seen = NULL;

    if (t == NULL) {
        snprintf(err, size, "%s: %s", table, why);
        return -1;
    }
    while ((more_lines = jbc_text_line(t, tsize, &tpos, line, sizeof line)) != 0) {
        char nm[128], tb[16], ex[128], px[16], sum[24], extra[2];
        int got;
        ++lineno;
        if (more_lines < 0) {
            snprintf(err, size, "%s, line %u: too long", table, lineno);
            goto fail;
        }
        if (line[0] == '#' || line[0] == '\n' || line[0] == 0)
            continue;
        got = sscanf(line, "%127s %15s %127s %15s %23s %1s", nm, tb, ex, px, sum, extra);
        if ((got != 2 && got != 5) || !all_of(nm, names, 100) ||
            (strcmp(tb, "ps1-tests") != 0 && strcmp(tb, "open") != 0) ||
            (got == 5 && (!all_of(ex, "abcdefghijklmnopqrstuvwxyz0123456789-._", 100) || ex[0] == '.' ||
                          !all_of(px, "0123456789", 7) || !all_of(sum, "0123456789abcdef", 16)))) {
            snprintf(err, size, "%s, line %u: malformed", table, lineno);
            goto fail;
        }
        for (k = 0; k < n; ++k)
            if (strcmp(seen[k].name, nm) == 0) {
                snprintf(err, size, "%s, line %u: a second line for %s (the first is line %u)",
                         table, lineno, nm, seen[k].line);
                goto fail;
            }
        if (n == cap) {
            struct seen *more;
            cap = cap ? cap * 2 : 64;
            more = realloc(seen, cap * sizeof *seen);
            if (more == NULL) {
                snprintf(err, size, "out of memory");
                goto fail;
            }
            seen = more;
        }
        memcpy(seen[n].name, nm, sizeof nm);
        seen[n++].line = lineno;
        if (strcmp(nm, key) != 0)
            continue;
        found = 1;
        memset(pl, 0, sizeof *pl);
        snprintf(pl->table, sizeof pl->table, "%s", tb);
        if (got == 5) {
            pl->pixels = strtoull(px, NULL, 10);
            pl->checksum = strtoull(sum, NULL, 16);
            snprintf(pl->exclude, sizeof pl->exclude, "%s", ex);
        }
    }
    free(t);
    free(seen);
    if (!found) {
        snprintf(err, size, "%s has no line for %s", table, key);
        return -1;
    }
    return 0;
fail:
    free(t);
    free(seen);
    return -1;
}

/* Options the table states, which the runner refuses. */
static const char *const table_options[] = {"--reference", "--passes", "--expects", "--words",
                                            "--checksum", "--dump", "--exclude", "--exclude-pixels",
                                            "--exclude-checksum", "--expect-mismatch"};

int main(int argc, char **argv)
{
    const char *out_dir = NULL, *list_path = NULL, *why = "", *picture_dir = NULL;
    const char *table = NULL, *probe_table = NULL, *key = NULL;
    const char *probe = NULL, *platform = NULL;
    char dump_buf[1024], exclude_buf[1024], probe_buf[1024], table_buf[1024], dir[1024];
    char ref_buf[1024];
    const char *dump = NULL, *exclude = NULL, *ref_file = NULL;
    jbp_result pr;
    int have_probe = 0;
    unsigned char *excluded = NULL;
    unsigned excluded_pixels = 0, excluded_entries = 0;
    unsigned long long ex_pixels = 0, ex_checksum = 0;
    int expect_mismatch = 0, a, status, too_long = 0;
    unsigned console_failed = 0;
    jbc_table_entry te;
    jbc_fixture fx;
    jbc_verdict v;
    char line[512];
    jbc_options o;
    jbc_list l;
    jbc_outcome res;
    uint16_t *hw = NULL;
    unsigned char *ref_bytes = NULL;
    size_t ref_size = 0;
    char err[512], base[128];
    const char *slash;
    size_t t;

    memset(&o, 0, sizeof o);
    o.log = stdout;
    for (a = 1; a < argc; ++a) {
        for (t = 0; t < sizeof table_options / sizeof table_options[0]; ++t)
            if (strcmp(argv[a], table_options[t]) == 0) {
                printf("ERROR %s: the fixture table states what is played and compared "
                       "(FORMAT.md, \"Fixture tables\"); it is not an option\n",
                       argv[a]);
                return 2;
            }
        if (strcmp(argv[a], "--threads") == 0 && a + 1 < argc)
            o.threads = atoi(argv[++a]);
        else if (strcmp(argv[a], "--scale") == 0 && a + 1 < argc)
            o.scale = atoi(argv[++a]);
        else if (strcmp(argv[a], "--picture") == 0 && a + 1 < argc) {
            picture_dir = argv[++a];
            o.picture = 1;
        }
        else if (strcmp(argv[a], "--split") == 0 && a + 1 < argc) {
            const char *sp = argv[++a];
            if (strcmp(sp, "op") == 0)
                o.split = JBC_SPLIT_OP;
            else if (strcmp(sp, "words") == 0)
                o.split = JBC_SPLIT_WORDS;
            else if (strcmp(sp, "random") == 0)
                o.split = JBC_SPLIT_RANDOM;
            else
                return usage();
        } else if (strcmp(argv[a], "--fixtures") == 0 && a + 1 < argc && table == NULL)
            table = argv[++a];
        else if (strcmp(argv[a], "--probe-fixtures") == 0 && a + 1 < argc && probe_table == NULL)
            probe_table = argv[++a];
        else if (strcmp(argv[a], "--fixture") == 0 && a + 1 < argc && key == NULL)
            key = argv[++a];
        else if (strcmp(argv[a], "--probe") == 0 && a + 1 < argc && probe == NULL)
            probe = argv[++a];
        else if (strcmp(argv[a], "--platform") == 0 && a + 1 < argc && platform == NULL)
            platform = argv[++a];
        else if (strcmp(argv[a], "--out") == 0 && a + 1 < argc)
            out_dir = argv[++a];
        else if (argv[a][0] == '-' || list_path != NULL)
            return usage();
        else
            list_path = argv[a];
    }
    if (list_path == NULL || key == NULL || (table == NULL) == (probe_table == NULL))
        return usage();
    if (platform != NULL && strcmp(platform, "console") != 0 && strcmp(platform, "simulation") != 0)
        return usage();
    if ((probe == NULL) != (platform == NULL) || (probe_table != NULL && probe != NULL)) {
        printf("ERROR --probe and --platform go together, with --fixtures (--probe-fixtures names "
               "its own result)\n");
        return 2;
    }

    /* The reference and the fixture, from the tables alone. */
    if (probe_table != NULL) {
        probe_line pl;
        if (probe_lookup(probe_table, key, &pl, err, sizeof err) != 0) {
            printf("ERROR %s\n", err);
            return 2;
        }
        dir_of(probe_table, dir, sizeof dir);
        join(table_buf, sizeof table_buf, dir,
             strcmp(pl.table, "open") == 0 ? "/../open/fixtures.txt" : "/../fixtures.txt", "", "",
             &too_long);
        table = table_buf;
        join(probe_buf, sizeof probe_buf, dir, "/", key, ".jbpr", &too_long);
        probe = probe_buf;
        platform = "console";
        if (pl.exclude[0]) {
            join(exclude_buf, sizeof exclude_buf, dir, "/", pl.exclude, "", &too_long);
            exclude = exclude_buf;
            ex_pixels = pl.pixels;
            ex_checksum = pl.checksum;
        }
        if (jbc_table_lookup(table, key, &te, err, sizeof err) != 0) {
            printf("ERROR %s\n", err);
            return 2;
        }
        if ((strcmp(pl.table, "open") == 0) != (strcmp(te.where, "open") == 0)) {
            printf("ERROR %s: %s is a line of %s, not of a %s table\n", probe_table, key, table,
                   pl.table);
            return 2;
        }
    } else if (jbc_table_lookup(table, key, &te, err, sizeof err) != 0) {
        printf("ERROR %s\n", err);
        return 2;
    }
    fx = te.fixture;
    dir_of(table, dir, sizeof dir);
    if (probe == NULL) {
        if (fx.reference == JBC_REF_PROBE) {
            printf("ERROR %s is compared with a probe result (%s): --probe and --platform are "
                   "required\n",
                   key, table);
            return 2;
        }
        if (fx.reference == JBC_REF_DUMP || fx.reference == JBC_REF_LOG) {
            /* The console's file, the dump or the log, beside the table
               (repo) or the list (fetched); bound to the line by its REFSUM
               (checked below, before anything is compared). */
            char ldir[1024];
            if (strcmp(te.where, "repo") == 0)
                snprintf(ldir, sizeof ldir, "%s", dir);
            else
                dir_of(list_path, ldir, sizeof ldir);
            join(ref_buf, sizeof ref_buf, ldir, "/../../third_party/ps1-tests/gpu/", key,
                 fx.reference == JBC_REF_DUMP ? "/vram.png" : "/psx.log", &too_long);
            ref_file = ref_buf;
        }
        if (fx.reference == JBC_REF_DUMP) {
            snprintf(dump_buf, sizeof dump_buf, "%s", ref_buf);
            dump = dump_buf;
            if (te.exclude[0]) {
                join(exclude_buf, sizeof exclude_buf, dir, "/", te.exclude, "", &too_long);
                exclude = exclude_buf;
                ex_pixels = te.exclude_pixels;
                ex_checksum = te.exclude_checksum;
            }
        }
        expect_mismatch = expected_failure(dir, key);
    } else
        fx.reference = JBC_REF_PROBE;
    o.passes = fx.passes;
    if (too_long) {
        printf("ERROR a path is too long\n");
        return 2;
    }
    if (probe != NULL && !readable(probe)) {
        printf("ERROR cannot open %s\n", probe);
        return 2;
    }

    if (jbc_load(list_path, &l, err, sizeof err) != 0) {
        printf("ERROR %s: %s\n", list_path, err);
        return 2;
    }
    slash = strrchr(l.name, '/');
    snprintf(base, sizeof base, "%s", slash ? slash + 1 : l.name);
    printf("%s: %s\n", l.name, l.source);
    if (jbc_check_fixture(&l, &fx, err, sizeof err) != 0) {
        printf("ERROR %s is not the fixture %s of %s: %s\n", list_path, key, table, err);
        jbc_free(&l);
        return 2;
    }
    printf("fixture: %s (%s), %lu words, checksum %016llx, %u checks, compared at pass %u of the "
           "main loop, against %s%s\n",
           key, table, fx.words, (unsigned long long)fx.checksum, fx.expects, fx.passes,
           probe != NULL                   ? "VRAM as the probe read it back: "
           : fx.reference == JBC_REF_DUMP ? "the console's dump "
                                           : "the console's log",
           probe != NULL ? probe : dump != NULL ? dump : "");
    if (ref_file != NULL) {
        /* The reference is the console's file the line names, byte for
           byte: another program's dump, or an image made from a replay,
           in its place is refused before anything is compared. */
        uint64_t sum = 0;
        if (!te.has_ref_checksum) {
            printf("ERROR %s: the fixture's line has no REFSUM\n", ref_file);
            jbc_free(&l);
            return 2;
        }
        /* Read once: the bytes that are hashed are the bytes that are
           decoded below, so the file cannot change in between. */
        ref_bytes = jbc_read_file(ref_file, &ref_size, &why);
        if (ref_bytes == NULL) {
            printf("ERROR %s: %s\n", ref_file, why);
            jbc_free(&l);
            return 2;
        }
        sum = jbc_bytes_checksum(ref_bytes, ref_size);
        if (sum != te.ref_checksum) {
            printf("ERROR %s: FNV-1a %016llx; the fixture %s of %s states %016llx for the console's "
                   "%s of %s: not that file\n",
                   ref_file, (unsigned long long)sum, key, table, (unsigned long long)te.ref_checksum,
                   fx.reference == JBC_REF_DUMP ? "dump" : "log", l.name);
            free(ref_bytes);
            jbc_free(&l);
            return 2;
        }
        printf("reference: %s, FNV-1a %016llx as the fixture states\n", ref_file,
               (unsigned long long)sum);
        if (dump != NULL) {
            hw = jbc_decode_dump(ref_bytes, ref_size, &why);
            if (hw == NULL) {
                printf("ERROR %s: %s\n", dump, why);
                free(ref_bytes);
                jbc_free(&l);
                return 2;
            }
        }
        free(ref_bytes);
        ref_bytes = NULL;
    }
    if (probe != NULL) {
        /* A probe result as the reference (docs/probe.md, "Turning console
           dumps into golden references"), bound
           to the fixture as a dump is, and clean. */
        char perr[512];
        uint32_t crc = 0, plat, want;
        int bound = 0;
        size_t k;
        if (jbp_result_load(&pr, probe, perr, sizeof perr) != 0 ||
            jbp_result_check(&pr, perr, sizeof perr) != 0) {
            printf("ERROR %s: %s\n", probe, perr);
            jbp_result_free(&pr);
            jbc_free(&l);
            return 2;
        }
        have_probe = 1;
        for (k = 0; k < l.n; ++k) {
            uint8_t b[4];
            jbp_put32(b, l.w[k]);
            crc = jbp_crc32(crc, b, 4);
        }
        plat = jbp_get32(pr.hello + 4 * JBP_ID_PLATFORM);
        want = strcmp(platform, "console") == 0 ? 1u : 2u;
        if (crc != pr.list_crc || crc != pr.head[JBP_H_LIST_CRC] || strcmp(pr.list_name, l.name) != 0 ||
            pr.list_bytes != 4 * l.n || pr.head[JBP_H_LIST_BYTES] != 4 * l.n)
            printf("ERROR %s: a result of %s (%u bytes, CRC-32 0x%08x), not of this list (%lu bytes, "
                   "0x%08x)\n",
                   probe, pr.list_name, (unsigned)pr.list_bytes, (unsigned)pr.list_crc,
                   (unsigned long)(4 * l.n), (unsigned)crc);
        else if (pr.head[JBP_H_PASSES] != fx.passes ||
                 (fx.passes != 0 && pr.head[JBP_H_WINDOW] != JBP_WINDOW_DEFAULT))
            printf("ERROR %s: played to pass %u with a window of %u; the fixture %s says pass %u, "
                   "window %u\n",
                   probe, (unsigned)pr.head[JBP_H_PASSES], (unsigned)pr.head[JBP_H_WINDOW],
                   key, fx.passes, (unsigned)JBP_WINDOW_DEFAULT);
        else if (plat != want)
            printf("ERROR %s: from %s (platform %u), and the reference must be from %s\n", probe,
                   plat == 1 ? "a console" : plat == 2 ? "the host simulation" : "an unknown platform",
                   (unsigned)plat, want == 1 ? "a console" : "the host simulation");
        else if (jbp_result_clean(&pr, perr, sizeof perr) != 0)
            printf("ERROR %s: no reference: %s (jbprobe info shows the capture)\n", probe, perr);
        else if (pr.nexpects != l.expects)
            printf("ERROR %s: no reference: the probe evaluated %u checks; the fixture %s has %u\n",
                   probe, (unsigned)pr.nexpects, key, l.expects);
        else {
            /* Each check the probe evaluated is the list's, in order. */
            size_t i = l.first;
            uint32_t e = 0;
            bound = 1;
            for (; JBC_OP_CODE(l.w[i]) != JBC_END; i += 1 + JBC_OP_COUNT(l.w[i])) {
                const uint32_t *q, *op = l.w + i + 1;
                if (JBC_OP_CODE(l.w[i]) != JBC_EXPECT)
                    continue;
                q = pr.expects + (size_t)e * JBP_EXPECT_WORDS;
                if (q[0] != i || q[1] != op[0] || q[2] != op[1] || q[3] != op[2]) {
                    printf("ERROR %s: no reference: its check %u (word %u, read %u, mask 0x%08x, value "
                           "0x%08x) is not the list's (word %lu, read %u, mask 0x%08x, value 0x%08x)\n",
                           probe, (unsigned)e, (unsigned)q[0], (unsigned)q[1], (unsigned)q[2],
                           (unsigned)q[3], (unsigned long)i, (unsigned)op[0], (unsigned)op[1],
                           (unsigned)op[2]);
                    bound = 0;
                    break;
                }
                ++e;
            }
        }
        if (!bound) {
            jbp_result_free(&pr);
            jbc_free(&l);
            return 2;
        }
        jbp_result_describe(&pr, stdout);
        hw = malloc(JBC_VRAM_PIXELS * sizeof *hw);
        if (hw == NULL) {
            jbp_result_free(&pr);
            jbc_free(&l);
            return 2;
        }
        memcpy(hw, pr.vram, JBC_VRAM_PIXELS * sizeof *hw);
        cmp_bits = 0xffffu;
    }
    if (dump != NULL && hw == NULL) {
        /* Every dump is a fixture's reference, read and checked above. */
        printf("ERROR %s: a dump with no fixture line\n", dump);
        jbc_free(&l);
        return 2;
    }
    if (exclude != NULL &&
        ((excluded = calloc(JBC_VRAM_PIXELS, 1)) == NULL ||
         jbc_load_exclusions(exclude, excluded, &excluded_pixels, &excluded_entries, err,
                             sizeof err) != 0)) {
        printf("ERROR %s: %s\n", exclude, excluded == NULL ? "out of memory" : err);
        free(excluded);
        free(hw);
        if (have_probe)
            jbp_result_free(&pr);
        jbc_free(&l);
        return 2;
    }
    if (excluded != NULL &&
        (excluded_pixels != ex_pixels || jbc_exclusion_checksum(excluded) != ex_checksum)) {
        printf("ERROR %s: %u pixels with checksum %016llx; the exclusion %s states for %s is "
               "%llu pixels with checksum %016llx (docs/conformance.md)\n",
               exclude, excluded_pixels, (unsigned long long)jbc_exclusion_checksum(excluded),
               probe_table != NULL ? probe_table : table, key, ex_pixels, ex_checksum);
        free(excluded);
        free(hw);
        if (have_probe)
            jbp_result_free(&pr);
        jbc_free(&l);
        return 2;
    }
    if (hw == NULL && l.expects == 0) {
        printf("ERROR %s: no dump and no checks: nothing to compare\n", list_path);
        jbc_free(&l);
        return 2;
    }
    if (jbc_play(&l, &o, &res, err, sizeof err) != 0) {
        printf("ERROR %s: %s\n", list_path, err);
        free(hw);
        if (have_probe)
            jbp_result_free(&pr);
        jbc_free(&l);
        return 2;
    }

    memset(&v, 0, sizeof v);
    v.expect_mismatch = expect_mismatch;
    if (l.loop) {
        char loop_lines[1024];
        jbc_describe_loop(&res, loop_lines, sizeof loop_lines);
        printf("%s\n", loop_lines);
        v.loop_failed = !res.steady || res.early || !res.state_steady ||
                        res.window_played < res.window;
    }
    printf("waits: %u, %u that would never end\n", res.waits, res.waits_failed);
    v.waits_failed = res.waits_failed;
    if (l.expects) {
        printf("checks: %u, %u differ from the list's values\n", res.checks, res.checks_failed);
        v.checks_failed = res.checks_failed;
    }

    if (have_probe) {
        /* Every word the list read, against the console's. GPUSTAT's bits
           25-28 and 31 are timing (DMA request, ready flags, interlace
           line), out of scope (docs/design.md, "Out of scope"). */
        unsigned k, differ = 0;
        if (pr.nreads != res.nreads) {
            printf("reads: %u here, %u from the console\n", res.nreads, (unsigned)pr.nreads);
            v.reads_differ = 1;
        } else {
            unsigned port = 0, left = 0;
            size_t op = l.first, at = 0;
            for (k = 0; k < res.nreads; ++k) {
                uint32_t keep;
                while (left == 0 && op < l.n) {
                    uint32_t w = l.w[op];
                    if (JBC_OP_CODE(w) == JBC_READ) {
                        port = l.w[op + 1];
                        left = l.w[op + 2];
                        at = op;
                    }
                    op += 1 + JBC_OP_COUNT(w);
                }
                keep = port == JBC_PORT_GPUSTAT ? 0x61ffffffu : 0xffffffffu;
                if ((res.reads[k] & keep) != (pr.reads[k] & keep)) {
                    if (differ < 8)
                        printf("  read %u (%s, word %lu): ours 0x%08x, console 0x%08x\n", k,
                               port == JBC_PORT_GPUSTAT ? "GPUSTAT" : "GPUREAD", (unsigned long)at,
                               res.reads[k], (unsigned)pr.reads[k]);
                    ++differ;
                }
                --left;
            }
            printf("reads: %u, %u differ from the console's\n", res.nreads, differ);
            v.reads_differ = differ;
        }
        /* The checks (FORMAT.md, "Playing on a console"): the console's read is the
           reference, so jitterbug must give each the console's outcome and
           value under its mask. A check the console failed is a difference
           between this console and the one the list's values came from:
           reported, and never accepted. */
        if (l.expects) {
            size_t i;
            unsigned differ_checks = 0;
            for (i = l.first, k = 0; JBC_OP_CODE(l.w[i]) != JBC_END; i += 1 + JBC_OP_COUNT(l.w[i])) {
                const uint32_t *op = l.w + i + 1, *q;
                uint32_t ours, theirs;
                char label[256];
                if (JBC_OP_CODE(l.w[i]) != JBC_EXPECT)
                    continue;
                q = pr.expects + (size_t)k++ * JBP_EXPECT_WORDS;
                ours = op[0] < res.nreads ? res.reads[op[0]] : 0;
                theirs = q[4];
                jbc_string(&l, i + 4, label, sizeof label);
                if ((theirs & op[1]) != op[2]) {
                    printf("THE CONSOLE FAILED check %u (%s): read %u was 0x%08x, & 0x%08x = 0x%x; "
                           "the list's value is 0x%x\n",
                           k, label, (unsigned)op[0], (unsigned)theirs, (unsigned)op[1],
                           (unsigned)(theirs & op[1]), (unsigned)op[2]);
                    ++console_failed;
                }
                if ((ours & op[1]) != (theirs & op[1])) {
                    printf("  check %u (%s): ours 0x%x, the console's 0x%x (read %u & 0x%08x)\n", k,
                           label, (unsigned)(ours & op[1]), (unsigned)(theirs & op[1]),
                           (unsigned)op[0], (unsigned)op[1]);
                    ++differ_checks;
                }
            }
            printf("checks against the console's: %u, %u differ; the console failed %u of the "
                   "list's\n",
                   l.expects, differ_checks, console_failed);
            v.checks_failed = differ_checks;
            v.console_checks_failed = console_failed;
        }
    }
    if (hw != NULL) {
        jbc_diff d;
        size_t i;
        int shown = 0;
        jbc_compare_bits(res.vram, hw, excluded, cmp_bits, &d);
        if (excluded != NULL) {
            unsigned long differ = 0;
            for (i = 0; i < JBC_VRAM_PIXELS; ++i)
                differ += excluded[i] && (res.vram[i] & cmp_bits) != (hw[i] & cmp_bits);
            printf("excluded: %u pixels in %u entries of %s, not compared (%lu of them differ)\n",
                   excluded_pixels, excluded_entries, exclude, differ);
        }
        for (i = 0; i < JBC_VRAM_PIXELS && shown < 8; ++i)
            if ((res.vram[i] & cmp_bits) != (hw[i] & cmp_bits) && !(excluded != NULL && excluded[i])) {
                printf("  (%d, %d): ours 0x%04x, console 0x%04x\n", (int)(i % JBC_VRAM_W),
                       (int)(i / JBC_VRAM_W), res.vram[i] & cmp_bits, hw[i] & cmp_bits);
                ++shown;
            }
        if (d.count == 0)
            printf(have_probe ? "VRAM: identical to the probe's result, all 16 bits\n"
                              : "VRAM: identical to the console's dump on the 15 colour bits\n");
        else {
            printf("VRAM: %lu pixels differ, bounding box (%d, %d)-(%d, %d), %d x %d\n",
                   d.count, d.x0, d.y0, d.x1, d.y1, d.x1 - d.x0 + 1, d.y1 - d.y0 + 1);
            v.vram_differs = 1;
        }
        v.excluded = excluded_pixels;
    }
    if (out_dir != NULL && (v.vram_differs || v.checks_failed || v.reads_differ || v.waits_failed ||
                            v.loop_failed))
        write_images(out_dir, base, res.vram, hw, excluded);
    if (picture_dir != NULL && res.picture != NULL) {
        char path[1024];

        snprintf(path, sizeof path, "%s/%s.%dx.png", picture_dir, base, o.scale ? o.scale : 1);
        if (stbi_write_png(path, res.picture_w, res.picture_h, 4, res.picture,
                           res.picture_w * 4))
            printf("wrote %s\n", path);
        /* How much of the picture is the 1x VRAM blown up: the 1x pixels
           whose N x N block is not all that pixel (drawn at finer
           resolution, or different). */
        {
            int n = res.picture_w / JBC_VRAM_W, x, y, i, j;
            unsigned long other = 0;
            int x0 = JBC_VRAM_W, y0 = JBC_VRAM_H, x1 = -1, y1 = -1;

            for (y = 0; y < JBC_VRAM_H; ++y)
                for (x = 0; x < JBC_VRAM_W; ++x) {
                    unsigned p = res.vram[(size_t)y * JBC_VRAM_W + x], c, k;
                    int same = 1;
                    for (j = 0; j < n && same; ++j)
                        for (i = 0; i < n && same; ++i) {
                            const unsigned char *q =
                                res.picture + ((size_t)(y * n + j) * res.picture_w + x * n + i) * 4;
                            for (k = 0; k < 3; ++k) {
                                c = (p >> (5 * k)) & 31;
                                same &= q[k] == (unsigned char)(c << 3 | c >> 2);
                            }
                        }
                    if (same)
                        continue;
                    ++other;
                    x0 = x < x0 ? x : x0;
                    y0 = y < y0 ? y : y0;
                    x1 = x > x1 ? x : x1;
                    y1 = y > y1 ? y : y1;
                }
            printf("picture: %lu 1x pixels are not one block of their 1x colour", other);
            if (other)
                printf(", in (%d, %d)-(%d, %d)", x0, y0, x1, y1);
            printf("\n");
        }
    }

    if (have_probe)
        jbp_result_free(&pr);
    jbc_outcome_free(&res);
    free(excluded);
    free(hw);
    jbc_free(&l);
    status = jbc_judge(base, &v, line, sizeof line);
    printf("%s\n", line);
    return status;
}
