/*
 * fuzz_files: the texture and mesh loaders on deterministic mutations of
 * valid files (the converters' pinned outputs, tests/files/expected): bits
 * flipped, bytes and header fields set to values near the limits, section
 * offsets swapped, the file cut short or lengthened, with its size field
 * then often set to the new length so the checks past it are reached.
 * Every load must give JB_OK or JB_ERROR_INVALID_ARGUMENT (with a detail);
 * jb_texture_parse must agree with jb_texture_load_mem; a texture loaded
 * is destroyed, a mesh loaded is drawn in a frame (which may refuse it
 * only for the GTE's limits, never crash) and destroyed; and after every
 * load the context holds no more memory than before (a counting
 * allocator). Under the sanitizer build of the check this is the test for
 * reads outside the file and for leaks. The libFuzzer target
 * (fuzz_files_libfuzzer.c) runs the same checks on any input.
 *
 *   test_fuzz_files DIR [ITERATIONS]    DIR is tests/files; 4000 a file
 */
#include "api_test.h"

#include "fuzz_files.h"

static const char *const files[] = {
    "expected/rocks8.jbt", "expected/rocks4d.jbt",   "expected/tiles4.jbt",
    "expected/sky15.jbt",  "expected/water4.jbt",    "expected/bricks8r.jbt",
    "expected/box.jbm",    "expected/box-flat.jbm",  "expected/hexagon.jbm",
    "expected/pyramid.jbm", "expected/hexagon-plain.jbm"};

static unsigned char *read_file(const char *dir, const char *name, size_t *size)
{
    char path[1024];
    unsigned char *buf;
    long n;
    FILE *f;

    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    buf = must_alloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("cannot read %s\n", path);
        exit(2);
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* A value near a limit, for a field of a file of size bytes. */
static uint32_t interesting(gen *g, size_t size)
{
    static const uint32_t v[] = {0,          1,          2,     3,     4,     8,
                                 15,         16,         255,   256,   257,   4095,
                                 4096,       4097,       32767, 32768, 65535, 65536,
                                 65537,      1048576,    1048577,
                                 0x7fffffffu, 0x80000000u, 0xfffffffcu, 0xffffffffu};
    int k = rnd_below(g, 32);

    if (k < (int)(sizeof v / sizeof *v))
        return v[k];
    if (k < 28)
        return (uint32_t)size + (uint32_t)rnd_below(g, 9) - 4u;
    return rnd(g);
}

/* Mutates data (size bytes, room for cap) in place; returns the new size. */
static size_t mutate(gen *g, unsigned char *data, size_t size, size_t cap)
{
    int ops = 1 + rnd_below(g, 4), i;

    for (i = 0; i < ops; i++) {
        int k = rnd_below(g, 10);
        if (size == 0)
            k = 6;
        if (k == 0) { /* a bit anywhere */
            size_t at = (size_t)rnd_below(g, (int)size);
            data[at] ^= (unsigned char)(1u << rnd_below(g, 8));
        } else if (k == 1) { /* a byte anywhere */
            size_t at = (size_t)rnd_below(g, (int)size);
            data[at] = (unsigned char)interesting(g, size);
        } else if (k <= 4 && size >= 64) { /* a header field, aligned */
            size_t at = (size_t)rnd_below(g, 16) * 4;
            put32(data + at, interesting(g, size));
        } else if (k == 5 && size >= 4) { /* a 16-bit value past the header */
            size_t at = (size_t)rnd_below(g, (int)size - 1);
            uint32_t v = interesting(g, size);
            data[at] = (unsigned char)v;
            data[at + 1] = (unsigned char)(v >> 8);
        } else if (k == 6) { /* longer */
            size_t n = (size_t)rnd_below(g, 64) + 1, j;
            if (size + n > cap)
                n = cap - size;
            for (j = 0; j < n; j++)
                data[size + j] = (unsigned char)rnd(g);
            size += n;
        } else if (k == 7) { /* shorter */
            size = (size_t)rnd_below(g, (int)size + 1);
        } else if (k == 8 && size >= 64) { /* one section offset over another */
            int a = 5 + rnd_below(g, 7), b = 5 + rnd_below(g, 7);
            memcpy(data + 4 * a, data + 4 * b, 4);
        } else if (size > 0) {
            size_t at = (size_t)rnd_below(g, (int)size);
            data[at] = (unsigned char)rnd(g);
        }
    }
    /* Often a size field that agrees, so the checks after it are reached. */
    if (size >= 12 && rnd_below(g, 4) != 0)
        put32(data + 8, (uint32_t)size);
    return size;
}

/*
 * A valid mesh of more faces than a frame holds (a 100 x 100 grid of
 * quads, double-sided, in view): its draw fills the frame, and the
 * harness must take JB_ERROR_OUT_OF_MEMORY naming max_primitives as an
 * outcome, not a failure. The file is laid out as docs/formats.md says.
 */
static unsigned char *big_mesh(size_t *size)
{
    enum { N = 100, V = (N + 1) * (N + 1), F = N * N };
    size_t pos = 64, faces = pos + ((size_t)V * 6 + 3) / 4 * 4, mats = faces + (size_t)F * 12;
    unsigned char *d = must_alloc(mats + 8);
    int x, y;

    memcpy(d, "JBMS", 4);
    put32(d + 4, 1);
    put32(d + 8, (uint32_t)(mats + 8));
    put32(d + 16, V);
    put32(d + 20, F);
    put32(d + 24, 1);
    put32(d + 32, (uint32_t)pos);
    put32(d + 48, (uint32_t)faces);
    put32(d + 52, (uint32_t)mats);
    for (y = 0; y <= N; y++)
        for (x = 0; x <= N; x++) {
            unsigned char *p = d + pos + ((size_t)y * (N + 1) + x) * 6;
            int px = x * 3 - 150, py = y * 3 - 150;
            p[0] = (unsigned char)px, p[1] = (unsigned char)((unsigned)px >> 8);
            p[2] = (unsigned char)py, p[3] = (unsigned char)((unsigned)py >> 8);
        }
    for (y = 0; y < N; y++)
        for (x = 0; x < N; x++) {
            unsigned char *p = d + faces + ((size_t)y * N + x) * 12;
            unsigned v0 = (unsigned)(y * (N + 1) + x), k;
            unsigned v[4];
            v[0] = v0, v[1] = v0 + 1, v[2] = v0 + N + 2, v[3] = v0 + N + 1;
            for (k = 0; k < 4; k++)
                p[2 * k] = (unsigned char)v[k], p[2 * k + 1] = (unsigned char)(v[k] >> 8);
            p[8] = 4;
        }
    d[mats + 4] = 0;    /* blend none */
    d[mats + 5] = 4;    /* double-sided */
    d[mats + 6] = 0xff; /* no slot */
    d[mats + 7] = 0xff;
    *size = mats + 8;
    return d;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    long iterations = argc > 2 ? atol(argv[2]) : 4000;
    jbf_state st;
    size_t f;
    gen g;

    if (dir == NULL || iterations < 1) {
        printf("usage: test_fuzz_files DIR [ITERATIONS]\n");
        return 2;
    }
    if (jbf_init(&st) != 0) {
        printf("FAIL setting up\n");
        return 1;
    }
    {
        size_t size;
        unsigned char *big = big_mesh(&size);
        long drawn = st.drawn;
        if (jbf_one(&st, big, size, 1) != 0 || st.drawn != drawn) {
            printf("FAIL a valid mesh of more faces than a frame holds\n");
            return 1;
        }
        free(big);
    }
    memset(&g, 0, sizeof g);
    g.s = 0x6a627466757a7a31ull;
    for (f = 0; f < sizeof files / sizeof *files; f++) {
        size_t size, cap;
        unsigned char *orig = read_file(dir, files[f], &size), *buf;
        long i;

        /* The original first: it must load. */
        if (jbf_one(&st, orig, size, 1) != 0) {
            printf("FAIL %s: the original does not load\n", files[f]);
            return 1;
        }
        cap = size + 64 * 4;
        buf = must_alloc(cap);
        for (i = 0; i < iterations; i++) {
            size_t n;
            memcpy(buf, orig, size);
            n = mutate(&g, buf, size, cap);
            if (jbf_one(&st, buf, n, 0) != 0) {
                printf("FAIL %s: mutation %ld (seed state 0x%016llx)\n", files[f], i,
                       (unsigned long long)g.s);
                return 1;
            }
        }
        free(buf);
        free(orig);
    }
    printf("fuzz_files: %ld loads: %ld accepted, %ld refused; %ld meshes drawn\n", st.loads,
           st.accepted, st.refused, st.drawn);
    jbf_free(&st);
    return 0;
}
