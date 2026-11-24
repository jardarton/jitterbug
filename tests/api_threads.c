/*
 * Thread requests, as include/jitterbug.h describes them for jb_config.threads
 * and jb_set_threads, in this build: with threads (JB_THREADS on), or
 * single-threaded (JB_THREADS off, always so for emscripten; the test is
 * built with JB_TEST_SINGLE_THREADED then).
 *
 *   - Every count 0..64 is accepted by jb_create and jb_set_threads (JB_OK).
 *   - Single-threaded: a request for more than one thread is clamped to
 *     one and logged ("started only 1 of N rasterising threads"), once per
 *     request; 0 and 1 log nothing.
 *   - With threads: a request that can be met logs nothing.
 *   - Either way the pixels are those of one thread: a seeded stream of
 *     GP0 commands draws the same frame at every request, at 1x and 2x.
 */
#include "api_test.h"

typedef struct {
    int messages; /* about rasterising threads */
    char last[256];
} logbook;

static void log_fn(void *user, const char *message)
{
    logbook *b = user;

    if (strstr(message, "rasterising threads") != NULL) {
        ++b->messages;
        snprintf(b->last, sizeof b->last, "%s", message);
    }
}

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325u;
    size_t i;

    for (i = 0; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3u;
    return h;
}

/* Draws a seeded scene and hashes the frame. */
static uint64_t draw(jb_context *jb)
{
    static const uint32_t setup[] = {0xe3000000u, 0xe4000000u | 1023u | 511u << 10, 0xe5000000u};
    static uint32_t w[GEN_MAX];
    gen g = {12345, 160, 120};
    const uint8_t *px;
    int i, fw, fh;

    gp0(jb, setup, 3);
    for (i = 0; i < 400; ++i) {
        int n = gen_command(&g, w, 1);
        gp0(jb, w, (size_t)n);
    }
    gp1(jb, 0x03000000u);
    px = jb_frame(jb, &fw, &fh);
    check("jb_frame", px != NULL, 1);
    return px != NULL ? fnv(px, (size_t)fw * (size_t)fh * 4) : 0;
}

/* The messages a request should log in this build. */
static int expected_messages(int threads)
{
#ifdef JB_TEST_SINGLE_THREADED
    return threads > 1;
#else
    (void)threads;
    return 0;
#endif
}

/* An allocator whose every allocation fails while `failing` is set. */
static int failing;

static void *test_alloc(void *user, size_t size)
{
    (void)user;
    return failing ? NULL : malloc(size);
}

static void test_free(void *user, void *ptr)
{
    (void)user;
    free(ptr);
}

/* At 1x on one thread, more threads need a queue of primitives. Without
   memory for it a build with threads reports that and stays on one thread;
   a single-threaded build needs no queue, so the request is met as always:
   JB_OK, clamped to one and logged. Either way the context goes on. */
static void more_threads_without_memory(void)
{
    jb_config cfg;
    jb_context *jb;
    int w, h;

    memset(&cfg, 0, sizeof cfg);
    cfg.threads = 1;
    cfg.alloc = test_alloc;
    cfg.free = test_free;
    jb = must_create(&cfg);
    failing = 1;
#ifdef JB_TEST_SINGLE_THREADED
    check("jb_set_threads 2 with no memory, single-threaded", jb_set_threads(jb, 2), JB_OK);
    check("jb_set_threads 0 with no memory, single-threaded", jb_set_threads(jb, 0), JB_OK);
#else
    check("jb_set_threads 2 with no memory", jb_set_threads(jb, 2), JB_ERROR_OUT_OF_MEMORY);
#endif
    failing = 0;
    check("jb_set_threads 2 with memory", jb_set_threads(jb, 2), JB_OK);
    gp1(jb, 0x03000000u);
    check("a frame afterwards", jb_frame(jb, &w, &h) != NULL && w == 320, 1);
    jb_destroy(jb);
}

int main(void)
{
    static const int counts[] = {0, 1, 2, 4, 16};
    char what[128];
    int scale, k;

    for (scale = 1; scale <= 2; ++scale) {
        uint64_t one = 0;

        for (k = 0; k < (int)(sizeof counts / sizeof counts[0]); ++k) {
            logbook book = {0, ""};
            jb_config c;
            jb_context *jb;
            jb_result r = JB_ERROR_UNSUPPORTED;
            uint64_t h;

            memset(&c, 0, sizeof c);
            c.scale = scale;
            c.threads = counts[k];
            c.log = log_fn;
            c.log_user = &book;
            jb = jb_create(&c, &r);
            snprintf(what, sizeof what, "%dx jb_create threads %d", scale, counts[k]);
            if (!check(what, r, JB_OK))
                continue;
            snprintf(what, sizeof what, "%dx jb_create threads %d: messages", scale, counts[k]);
            check(what, book.messages, expected_messages(counts[k]));
#ifdef JB_TEST_SINGLE_THREADED
            if (counts[k] > 1) {
                char want[96];

                snprintf(want, sizeof want, "jitterbug: started only 1 of %d rasterising threads",
                         counts[k]);
                snprintf(what, sizeof what, "%dx jb_create threads %d: message", scale, counts[k]);
                check_str(what, book.last, want);
            }
#endif
            h = draw(jb);
            if (k == 0)
                one = h;
            snprintf(what, sizeof what, "%dx threads %d: the frame of threads 0", scale, counts[k]);
            check(what, (long long)(h == one), 1);

            /* jb_set_threads to 8 and back to 1: accepted, logged as above. */
            book.messages = 0;
            snprintf(what, sizeof what, "%dx jb_set_threads 8 after %d", scale, counts[k]);
            check(what, jb_set_threads(jb, 8), JB_OK);
            snprintf(what, sizeof what, "%dx jb_set_threads 8 after %d: messages", scale,
                     counts[k]);
            check(what, book.messages, counts[k] == 8 ? 0 : expected_messages(8));
            book.messages = 0;
            snprintf(what, sizeof what, "%dx jb_set_threads 1 after 8", scale);
            check(what, jb_set_threads(jb, 1), JB_OK);
            check(what, book.messages, 0);
            jb_destroy(jb);
        }
    }
    more_threads_without_memory();
#ifdef JB_TEST_SINGLE_THREADED
    printf("api_threads: single-threaded build\n");
#else
    printf("api_threads: build with threads\n");
#endif
    return finish("api_threads");
}
