/*
 * jbprobe: the host's tool for the hardware probe (docs/probe.md).
 *
 *   jbprobe send (--tty DEV [--baud N] | --tcp HOST:PORT)
 *                [--fixtures TABLE | --passes N] [--no-reset] [--no-clear]
 *                [--wait-ms N] [--start-ms N] [--note TEXT] [--quiet]
 *                LIST.jbc RESULT.jbpr
 *       plays LIST on the probe and writes what came back to RESULT. The
 *       main loop is played to pass N and then two more passes, which must
 *       not change VRAM (tests/conformance/FORMAT.md, "Playing a list"); N
 *       is the PASSES of LIST's line in TABLE (tests/conformance/fixtures.txt
 *       or tests/conformance/open/fixtures.txt; the list must be that
 *       fixture), or given with --passes for a list that has no fixture
 *       yet. A list without a main loop needs neither. --start-ms: how
 *       long to offer the START, and to connect with --tcp (60000, the
 *       protocol's minute; the tests shorten it). Nothing on the line
 *       blocks past the session's limits: a write the line does not take,
 *       or a connection not made, fails the session when its limit passes
 *       (tools/probe/PROTOCOL.md, "Limits and timeouts").
 *   jbprobe info RESULT.jbpr
 *       the console's identification and the session's summary
 *   jbprobe to-png RESULT.jbpr DIR [LIST.jbc]
 *       DIR/vram.png (ps1-tests' convention: each 5-bit channel shifted
 *       left by 3, opaque; the mask bit is lost), DIR/vram16.png (the same
 *       with alpha 128 where the mask bit is set, as tests/seed's golden
 *       images: all 16 bits), DIR/vram.bin (1024 x 512 little-endian
 *       16-bit pixels) and DIR/log.txt (identification, summary, every
 *       read, every check with its label when LIST is given, every wait
 *       that timed out)
 *   jbprobe compare RESULT.jbpr LIST.jbc --fixtures TABLE
 *                [--platform console|simulation] [--out DIR]
 *       the conformance runner (tests/conformance/conformance.c) with the
 *       result as its reference (--probe), bound to LIST's line in TABLE
 *       as a ctest test is (the runner reads the list's identity and pass
 *       from the table itself; the platform: console unless given) and
 *       refused unless it is clean (jbp_result_clean): all 16 bits of
 *       VRAM, the reads, each check's outcome against the console's; diff
 *       PNGs into DIR. No pixel is excluded. info and to-png read results
 *       that are not clean too
 *
 * Exit status 0 on success (compare: everything matches), 1 when a session
 * failed or something differs, 2 for a usage or file error.
 */
#define _POSIX_C_SOURCE 200809L

#include "jbh_session.h"
#include "jbp_result.h"

#include "jbc.h"

#include <stb_image_write.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* tests/conformance/conformance.c, built into this tool with main renamed. */
int jbc_conformance_main(int argc, char **argv);

static uint32_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((unsigned long long)t.tv_sec * 1000u + (unsigned long long)t.tv_nsec / 1000000u);
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    long len;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (b = malloc((size_t)len + 1)) != NULL && fread(b, 1, (size_t)len, f) == (size_t)len)
        *n = (size_t)len;
    else {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

/* ------------------------------------------------------------------------ */
/* The line: a tty in raw 8N1, or a TCP connection (an emulator's serial
   port bridge, or a network serial adaptor). */

static int open_tty(const char *dev, long baud)
{
    struct termios t;
    speed_t sp;
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);

    switch (baud) {
    case 9600: sp = B9600; break;
    case 19200: sp = B19200; break;
    case 38400: sp = B38400; break;
    case 57600: sp = B57600; break;
    case 115200: sp = B115200; break;
#ifdef B230400
    case 230400: sp = B230400; break;
#endif
    default:
        fprintf(stderr, "baud %ld: use 9600, 19200, 38400, 57600, 115200 or 230400\n", baud);
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (fd < 0 || tcgetattr(fd, &t) != 0) {
        fprintf(stderr, "%s: %s\n", dev, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    /* Raw: no line editing, translation, signals or software flow control. */
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CSIZE);
    t.c_cflag |= CS8 | CLOCAL | CREAD;
#ifdef CRTSCTS
    t.c_cflag &= ~(tcflag_t)CRTSCTS;
#endif
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, sp);
    cfsetospeed(&t, sp);
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        fprintf(stderr, "%s: %s\n", dev, strerror(errno));
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* Waits for fd to be ready for events until the absolute time until (ms,
   now_ms's clock): 1 ready, 0 when until passed, -1 on an error. Every wait
   on the line goes through here, in steps of at most 100 ms. */
static int wait_fd(int fd, short events, uint32_t until)
{
    for (;;) {
        struct pollfd pf;
        int32_t left = (int32_t)(until - now_ms());
        int r;
        if (left <= 0)
            return 0;
        pf.fd = fd;
        pf.events = events;
        pf.revents = 0;
        r = poll(&pf, 1, left < 100 ? (int)left : 100);
        if (r > 0)
            return 1;
        if (r < 0 && errno != EINTR)
            return -1;
    }
}

/* A connection made within limit_ms from now (non-blocking connect, then
   wait_fd). The name is resolved first by getaddrinfo, which the system's
   resolver bounds, not this limit: give an address to avoid it. */
static int open_tcp(const char *spec, uint32_t limit_ms)
{
    char host[256];
    const char *colon = strrchr(spec, ':');
    struct addrinfo hints, *res, *ai;
    uint32_t until = now_ms() + limit_ms;
    int fd = -1, e;

    if (colon == NULL || (size_t)(colon - spec) >= sizeof host) {
        fprintf(stderr, "--tcp HOST:PORT\n");
        return -1;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if ((e = getaddrinfo(host, colon + 1, &hints, &res)) != 0) {
        fprintf(stderr, "%s: %s\n", spec, gai_strerror(e));
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int err = 0;
        socklen_t len = sizeof err;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0 &&
            (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 ||
             (errno == EINPROGRESS && wait_fd(fd, POLLOUT, until) == 1 &&
              getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)))
            break;
        close(fd);
        fd = -1;
        if ((int32_t)(until - now_ms()) <= 0) {
            fprintf(stderr, "%s: no connection within %u ms\n", spec, (unsigned)limit_ms);
            freeaddrinfo(res);
            return -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0)
        fprintf(stderr, "%s: cannot connect\n", spec);
    return fd;
}

static int line_fd = -1;
static int line_failed;
static int done_set;     /* the session has ended, at done_at */
static uint32_t done_at; /* (a flag apart: the clock's value may be 0, and a
                            time nudged to be non-zero could lie in the future,
                            where the unsigned difference below would wrap) */
enum { TAIL_MS = 1500 }; /* after the end: to acknowledge a resent last frame */

/* The time by which a write must have gone: the session's own deadline
   (jbh_deadline) while it runs, the end of the tail after it. */
static uint32_t line_until(const jbh_session *s)
{
    if (s->state == JBH_RUNNING)
        return jbh_deadline(s);
    if (!done_set) {
        done_set = 1;
        done_at = now_ms();
    }
    return done_at + TAIL_MS;
}

/* The session's output (jbh_options.write; user is the session). A line
   that does not take it (a peer that never reads, CTS held) is waited for
   only until the session's deadline: then the line fails, and with it a
   running session. */
static void line_write(void *user, const uint8_t *p, size_t n)
{
    jbh_session *s = user;

    while (n > 0 && !line_failed) {
        ssize_t w = write(line_fd, p, n);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            int r = wait_fd(line_fd, POLLOUT, line_until(s));
            if (r <= 0) {
                char why[160];
                if (r == 0)
                    snprintf(why, sizeof why, "the line took no output until the session's time limit");
                else
                    snprintf(why, sizeof why, "write: %s", strerror(errno));
                if (s->state == JBH_RUNNING)
                    jbh_abandon(s, why);
                else
                    fprintf(stderr, "%s\n", why);
                line_failed = 1;
            }
        } else {
            char why[160];
            snprintf(why, sizeof why, "write: %s", w < 0 ? strerror(errno) : "nothing written");
            if (s->state == JBH_RUNNING)
                jbh_abandon(s, why);
            else
                fprintf(stderr, "%s\n", why);
            line_failed = 1;
        }
    }
}

static int cmd_send(int argc, char **argv)
{
    const char *tty = NULL, *tcp = NULL, *list_path = NULL, *out_path = NULL, *note = "";
    const char *fixtures = NULL;
    int have_passes = 0;
    long baud = 115200;
    jbh_options o;
    jbh_session *s;
    uint8_t *list;
    size_t n = 0;
    int a, quiet = 0, rc;
    char err[256];

    jbh_defaults(&o);
    o.log = stderr;
    o.session_id = now_ms() ^ (uint32_t)getpid() << 16;
    for (a = 0; a < argc; ++a) {
        if (strcmp(argv[a], "--tty") == 0 && a + 1 < argc)
            tty = argv[++a];
        else if (strcmp(argv[a], "--tcp") == 0 && a + 1 < argc)
            tcp = argv[++a];
        else if (strcmp(argv[a], "--baud") == 0 && a + 1 < argc)
            baud = atol(argv[++a]);
        else if (strcmp(argv[a], "--passes") == 0 && a + 1 < argc) {
            char *end;
            unsigned long v = strtoul(argv[++a], &end, 10);
            if (*end != 0 || v > JBP_PASSES_MAX)
                return -1;
            o.passes = (uint32_t)v;
            have_passes = 1;
        } else if (strcmp(argv[a], "--fixtures") == 0 && a + 1 < argc)
            fixtures = argv[++a];
        else if (strcmp(argv[a], "--no-reset") == 0)
            o.flags &= ~(uint32_t)JBP_FLAG_RESET;
        else if (strcmp(argv[a], "--no-clear") == 0)
            o.flags &= ~(uint32_t)JBP_FLAG_CLEAR;
        else if (strcmp(argv[a], "--wait-ms") == 0 && a + 1 < argc)
            o.wait_ms = (uint32_t)atol(argv[++a]);
        else if (strcmp(argv[a], "--start-ms") == 0 && a + 1 < argc) {
            char *end;
            unsigned long v = strtoul(argv[++a], &end, 10);
            if (*end != 0 || v == 0 || v > 600000ul)
                return -1;
            o.start_ms = (uint32_t)v;
        } else if (strcmp(argv[a], "--note") == 0 && a + 1 < argc)
            note = argv[++a];
        else if (strcmp(argv[a], "--quiet") == 0)
            quiet = 1;
        else if (argv[a][0] == '-')
            return -1;
        else if (list_path == NULL)
            list_path = argv[a];
        else if (out_path == NULL)
            out_path = argv[a];
        else
            return -1;
    }
    if (out_path == NULL || (tty == NULL) == (tcp == NULL) || (fixtures != NULL && have_passes))
        return -1;
    if (quiet)
        o.log = NULL;
    list = read_file(list_path, &n);
    if (list == NULL) {
        fprintf(stderr, "cannot read %s\n", list_path);
        return 2;
    }
    /* The pass compared: the fixture's, or the one given. */
    {
        jbc_list l;
        if (jbc_load(list_path, &l, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", list_path, err);
            free(list);
            return 2;
        }
        if (fixtures != NULL) {
            jbc_fixture f;
            char name[128], where[8];
            if (jbc_fixture_find(fixtures, &l, &f, name, sizeof name, where, err, sizeof err) != 0 ||
                jbc_check_fixture(&l, &f, err, sizeof err) != 0) {
                fprintf(stderr, "%s: %s\n", list_path, err);
                jbc_free(&l);
                free(list);
                return 2;
            }
            o.passes = f.passes;
        } else if (l.loop && !have_passes) {
            fprintf(stderr, "%s has a main loop: give --fixtures TABLE or --passes N\n", list_path);
            jbc_free(&l);
            free(list);
            return 2;
        } else if (!l.loop && o.passes != 0) {
            fprintf(stderr, "%s has no main loop: --passes must be 0\n", list_path);
            jbc_free(&l);
            free(list);
            return 2;
        }
        jbc_free(&l);
        if (!quiet && o.passes)
            fprintf(stderr, "main loop: compared after pass %u, then two more that must not change "
                            "VRAM\n", (unsigned)o.passes);
    }
    done_set = 0;
    done_at = 0;
    line_failed = 0;
    /* A peer that closed the line: write fails with EPIPE, which fails the
       session, rather than SIGPIPE ending the tool. */
    signal(SIGPIPE, SIG_IGN);
    line_fd = tty ? open_tty(tty, baud) : open_tcp(tcp, o.start_ms);
    if (line_fd < 0) {
        free(list);
        return 2;
    }
    o.write = line_write;
    s = malloc(sizeof *s);
    o.user = s;
    if (s == NULL || jbh_begin(s, list, n, &o, now_ms()) != 0) {
        fprintf(stderr, "out of memory\n");
        if (s != NULL)
            jbh_end(s);
        free(s);
        free(list);
        close(line_fd);
        return 2;
    }
    if (!quiet)
        fprintf(stderr, "%s: %lu bytes to %s; waiting for the probe\n", s->result.list_name,
                (unsigned long)n, tty ? tty : tcp);
    /* Until done, and a moment longer to acknowledge a resent last frame. */
    while (!line_failed &&
           (s->state == JBH_RUNNING || (done_set && now_ms() - done_at < TAIL_MS))) {
        struct pollfd pf;
        uint8_t buf[4096];
        ssize_t got;
        uint32_t now;
        int wait = 10, read_err;
        if (s->state == JBH_RUNNING) {
            /* Not past the session's deadline. */
            int32_t left = (int32_t)(jbh_deadline(s) - now_ms());
            wait = left <= 0 ? 0 : left < wait ? (int)left : wait;
        }
        pf.fd = line_fd;
        pf.events = POLLIN;
        pf.revents = 0;
        poll(&pf, 1, wait);
        /* One read a turn (poll returns at once while bytes wait), so that
           jbh_poll and the end of the loop are reached however fast the
           bytes come. The session's limits are applied at the time
           of the read before what was read is: a frame read after the
           deadline does not advance the session. This loop is the
           only place input is taken, for a tty and TCP alike. */
        got = read(line_fd, buf, sizeof buf);
        read_err = errno;
        now = now_ms();
        jbh_poll(s, now);
        if (got > 0)
            jbh_input(s, buf, (size_t)got, now);
        if ((got == 0 && tcp != NULL) ||
            (got < 0 && read_err != EAGAIN && read_err != EWOULDBLOCK && read_err != EINTR)) {
            char why[160];
            snprintf(why, sizeof why, "%s: %s", tty ? tty : tcp,
                     got == 0 ? "connection closed" : strerror(read_err));
            if (s->state == JBH_RUNNING)
                jbh_abandon(s, why);
            else
                fprintf(stderr, "%s\n", why);
            line_failed = 1;
        }
        if (s->state != JBH_RUNNING && !done_set) {
            done_set = 1;
            done_at = now_ms();
        }
    }
    /* Output still queued after the tail (or on a failed line) is
       discarded, so that close does not wait for a line that takes none. */
    if (tty)
        tcflush(line_fd, TCOFLUSH);
    close(line_fd);
    {
        time_t t = time(NULL);
        char when[64];
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %z", localtime(&t));
        char line[300];
        if (tty)
            snprintf(line, sizeof line, "tty %s at %ld baud", tty, baud);
        else
            snprintf(line, sizeof line, "tcp %s", tcp);
        snprintf(s->result.note, sizeof s->result.note, "%s, %s%s%s", line, when, note[0] ? "; " : "",
                 note);
    }
    rc = s->state == JBH_DONE ? 0 : 1;
    if (rc != 0)
        fprintf(stderr, "failed: %s\n", s->why[0] ? s->why : "the line closed");
    if (s->result.have_hello || s->result.have_head) {
        if (jbp_result_save(&s->result, out_path, err, sizeof err) != 0) {
            fprintf(stderr, "%s\n", err);
            rc = 2;
        } else if (!quiet)
            fprintf(stderr, "wrote %s%s\n", out_path, rc ? " (incomplete)" : "");
    }
    if (!quiet && rc == 0)
        jbp_result_describe(&s->result, stdout);
    fprintf(stderr, "line: %lu frames sent again, %lu NAKs sent, %lu damaged and %lu cut frames received\n",
            s->resent, s->naks_sent, s->bad_received, s->cut_received);
    jbh_end(s);
    free(s);
    free(list);
    return rc;
}

/* ------------------------------------------------------------------------ */

/* 0 with r loaded, or -1 with nothing left to free. */
static int load_checked(const char *path, jbp_result *r, int strict)
{
    char err[300], why[256];

    if (jbp_result_load(r, path, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        return -1;
    }
    if (strict && jbp_result_check(r, why, sizeof why) != 0) {
        fprintf(stderr, "%s: %s\n", path, why);
        jbp_result_free(r);
        return -1;
    }
    return 0;
}

/* Whether word at is the start of an EXPECT op of l (as the probe reports
   it), so that its label can be read: an offset from a result file is not
   trusted to point at one. */
static int expect_at(const jbc_list *l, uint32_t at)
{
    size_t i = l->first;

    while (i < l->n) {
        uint32_t w = l->w[i];
        if (i == at)
            return JBC_OP_CODE(w) == JBC_EXPECT;
        if (i > at || JBC_OP_CODE(w) == JBC_END)
            return 0;
        i += 1 + JBC_OP_COUNT(w);
    }
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    jbp_result r;
    char why[256];

    if (argc != 1)
        return -1;
    if (load_checked(argv[0], &r, 0) != 0)
        return 2;
    jbp_result_describe(&r, stdout);
    if (jbp_result_check(&r, why, sizeof why) != 0)
        printf("NOT usable as a reference: %s\n", why);
    else if (jbp_result_clean(&r, why, sizeof why) != 0)
        printf("complete, but NOT usable as a reference: %s\n", why);
    else
        printf("complete and clean: VRAM CRC-32 0x%08x matches the probe's\n", (unsigned)r.done_crc);
    jbp_result_free(&r);
    return 0;
}

static int cmd_to_png(int argc, char **argv)
{
    jbp_result r;
    jbc_list l;
    int have_list = 0, rc = 0;
    unsigned char *img;
    char path[1024], err[256];
    size_t i;
    FILE *f;

    if (argc < 2 || argc > 3)
        return -1;
    if (load_checked(argv[0], &r, 1) != 0)
        return 2;
    if (argc == 3) {
        if (jbc_load(argv[2], &l, err, sizeof err) != 0) {
            fprintf(stderr, "%s: %s\n", argv[2], err);
            jbp_result_free(&r);
            return 2;
        }
        have_list = 1;
    }
    img = malloc((size_t)JBP_VRAM_W * JBP_VRAM_H * 4);
    if (img == NULL) {
        if (have_list)
            jbc_free(&l);
        jbp_result_free(&r);
        return 2;
    }
    for (i = 0; i < (size_t)JBP_VRAM_W * JBP_VRAM_H; ++i) {
        unsigned p = r.vram[i];
        img[4 * i + 0] = (unsigned char)((p & 31) << 3);
        img[4 * i + 1] = (unsigned char)(((p >> 5) & 31) << 3);
        img[4 * i + 2] = (unsigned char)(((p >> 10) & 31) << 3);
        img[4 * i + 3] = 255;
    }
    snprintf(path, sizeof path, "%s/vram.png", argv[1]);
    rc |= !stbi_write_png(path, JBP_VRAM_W, JBP_VRAM_H, 4, img, JBP_VRAM_W * 4);
    for (i = 0; i < (size_t)JBP_VRAM_W * JBP_VRAM_H; ++i)
        img[4 * i + 3] = (r.vram[i] & 0x8000u) ? 128 : 255;
    snprintf(path, sizeof path, "%s/vram16.png", argv[1]);
    rc |= !stbi_write_png(path, JBP_VRAM_W, JBP_VRAM_H, 4, img, JBP_VRAM_W * 4);
    free(img);
    snprintf(path, sizeof path, "%s/vram.bin", argv[1]);
    if ((f = fopen(path, "wb")) != NULL) {
        for (i = 0; i < (size_t)JBP_VRAM_W * JBP_VRAM_H; ++i) {
            fputc(r.vram[i] & 0xff, f);
            fputc(r.vram[i] >> 8, f);
        }
        rc |= fclose(f) != 0;
    } else
        rc = 1;
    snprintf(path, sizeof path, "%s/log.txt", argv[1]);
    if ((f = fopen(path, "w")) != NULL) {
        uint32_t k;
        jbp_result_describe(&r, f);
        for (k = 0; k < r.nreads; ++k)
            fprintf(f, "read %u: 0x%08x\n", (unsigned)k, (unsigned)r.reads[k]);
        for (k = 0; k < r.nexpects; ++k) {
            const uint32_t *e = r.expects + (size_t)k * JBP_EXPECT_WORDS;
            char label[512] = "";
            if (have_list && expect_at(&l, e[0]))
                jbc_string(&l, e[0] + 4, label, sizeof label);
            fprintf(f, "%s - %s: read %u = 0x%08x, & 0x%08x = 0x%x, the list's 0x%x (word %u)\n",
                    (e[4] & e[2]) == e[3] ? "pass" : "fail", label[0] ? label : "?", (unsigned)e[1],
                    (unsigned)e[4], (unsigned)e[2], (unsigned)(e[4] & e[2]), (unsigned)e[3],
                    (unsigned)e[0]);
        }
        for (k = 0; k < r.nwaits; ++k) {
            const uint32_t *w = r.waits + (size_t)k * JBP_WAITFAIL_WORDS;
            fprintf(f, "wait at word %u (pass %u) timed out: GPUSTAT 0x%08x, & 0x%08x is not 0x%08x\n",
                    (unsigned)w[0], (unsigned)w[4], (unsigned)w[3], (unsigned)w[1], (unsigned)w[2]);
        }
        rc |= fclose(f) != 0;
    } else
        rc = 1;
    if (rc)
        fprintf(stderr, "cannot write into %s\n", argv[1]);
    else
        printf("wrote %s/vram.png, vram16.png, vram.bin, log.txt\n", argv[1]);
    if (have_list)
        jbc_free(&l);
    jbp_result_free(&r);
    return rc ? 2 : 0;
}

static int cmd_compare(int argc, char **argv)
{
    const char *fixtures = NULL, *platform = "console", *out = NULL;
    char *args[16], name[128], where[8], err[256];
    jbc_fixture f;
    jbc_list l;
    int n = 0, a;

    if (argc < 2)
        return -1;
    for (a = 2; a < argc; ++a) {
        if (strcmp(argv[a], "--fixtures") == 0 && a + 1 < argc)
            fixtures = argv[++a];
        else if (strcmp(argv[a], "--platform") == 0 && a + 1 < argc)
            platform = argv[++a];
        else if (strcmp(argv[a], "--out") == 0 && a + 1 < argc)
            out = argv[++a];
        else
            return -1;
    }
    if (fixtures == NULL)
        return -1;
    /* The list's line in the table, found by its name; the runner then
       takes everything else from that line itself. */
    if (jbc_load(argv[1], &l, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", argv[1], err);
        return 2;
    }
    if (jbc_fixture_find(fixtures, &l, &f, name, sizeof name, where, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        jbc_free(&l);
        return 2;
    }
    jbc_free(&l);
    args[n++] = "conformance";
    args[n++] = "--fixtures";
    args[n++] = (char *)fixtures;
    args[n++] = "--fixture";
    args[n++] = strncmp(name, "fuzz/", 5) == 0 ? name + 5 : name;
    args[n++] = "--probe";
    args[n++] = argv[0];
    args[n++] = "--platform";
    args[n++] = (char *)platform;
    if (out != NULL) {
        args[n++] = "--out";
        args[n++] = (char *)out;
    }
    args[n++] = argv[1];
    args[n] = NULL;
    return jbc_conformance_main(n, args);
}

static int usage(void)
{
    fprintf(stderr,
            "usage: jbprobe send (--tty DEV [--baud N] | --tcp HOST:PORT) [--fixtures TABLE | --passes N]\n"
            "                    [--no-reset] [--no-clear] [--wait-ms N] [--start-ms N] [--note TEXT]\n"
            "                    [--quiet]\n"
            "                    LIST.jbc RESULT.jbpr\n"
            "       jbprobe info RESULT.jbpr\n"
            "       jbprobe to-png RESULT.jbpr DIR [LIST.jbc]\n"
            "       jbprobe compare RESULT.jbpr LIST.jbc --fixtures TABLE [--platform console|simulation]\n"
            "                       [--out DIR]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int r = -1;

    if (argc < 2)
        return usage();
    if (strcmp(argv[1], "send") == 0)
        r = cmd_send(argc - 2, argv + 2);
    else if (strcmp(argv[1], "info") == 0)
        r = cmd_info(argc - 2, argv + 2);
    else if (strcmp(argv[1], "to-png") == 0)
        r = cmd_to_png(argc - 2, argv + 2);
    else if (strcmp(argv[1], "compare") == 0)
        r = cmd_compare(argc - 2, argv + 2);
    if (r < 0)
        return usage();
    return r;
}
