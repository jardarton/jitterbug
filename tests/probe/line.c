/*
 * probe_line: jbprobe send against a line that does not take its output
 *. jbprobe (tools/probe/host/jbprobe.c, its main renamed) runs on a
 * thread of its own with --start-ms LIMIT; this program is the other end:
 *
 *   probe_line pty-naks LIMIT LIST.jbc   a pseudo-terminal whose peer sends
 *                     valid NAKs as fast as the line takes them and never
 *                     reads: every NAK makes jbprobe send the START again,
 *                     until the terminal's buffer is full
 *   probe_line tcp-naks LIMIT LIST.jbc   the same over a TCP connection on
 *                     the loopback, the peer's receive buffer made small
 *   probe_line tcp-connect LIMIT LIST.jbc  a TCP listener whose queue of
 *                     connections is full and that never accepts: the
 *                     connection is never made
 *   probe_line pty-late-ack LIMIT LIST.jbc  a pseudo-terminal whose peer
 *                     reads the START and acknowledges it LIMIT + 4 ms
 *                     later, after the START limit: jbprobe must
 *                     fail the session with that limit's reason (no probe
 *                     took the START) and must not send the list
 *
 * jbprobe must give up within the limit and a margin (MARGIN_MS), not
 * before the limit, with exit status 1 (pty-naks, tcp-naks: the session
 * failed) or 2 (tcp-connect: no line), and its own diagnostic (stderr,
 * kept in a file and printed here) must name the reason: the line took no
 * output until the session's limit, or no connection was made within it.
 * A jbprobe that does not return in time is reported and this program
 * exits 1 without waiting for it. Exit status 0 when it held, 77 without a pseudo-terminal or a
 * loopback (skipped, unless the test requires them), else 1.
 */
#define _XOPEN_SOURCE 600

#include "jbp_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

int jbprobe_main(int argc, char **argv); /* jbprobe.c, main renamed */

enum { MARGIN_MS = 2000 };

static uint32_t now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((unsigned long long)t.tv_sec * 1000u + (unsigned long long)t.tv_nsec / 1000000u);
}

/* done is shared with jbprobe's thread, under lock; rc is read
   only after the thread is joined. */
typedef struct run {
    char *argv[12];
    int argc;
    pthread_mutex_t lock;
    int done, rc;
} run;

static void *run_jbprobe(void *p)
{
    run *r = p;

    r->rc = jbprobe_main(r->argc, r->argv);
    pthread_mutex_lock(&r->lock);
    r->done = 1;
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

static int is_done(run *r)
{
    int d;

    pthread_mutex_lock(&r->lock);
    d = r->done;
    pthread_mutex_unlock(&r->lock);
    return d;
}

/* A NAK frame, many times over: what the peer sends. */
static uint8_t naks[100 * (JBP_HEADER_BYTES + JBP_TRAILER_BYTES)];
static size_t naks_n;

static void make_naks(void)
{
    uint8_t f[JBP_HEADER_BYTES + JBP_TRAILER_BYTES];
    size_t n = jbp_frame(f, JBP_NAK, 0, NULL, 0), i;

    for (i = 0; i + n <= sizeof naks; i += n)
        memcpy(naks + i, f, n);
    naks_n = i;
}

/* pty-late-ack: jbprobe's output so far, read and parsed: its START
   acknowledged once, limit + 4 ms after it was read; LIST frames counted. */
static void late_ack_read(int peer, jbp_parser *rx, unsigned long limit, int *acked, int *lists_seen)
{
    uint8_t b[4096];
    ssize_t got, i;

    while ((got = read(peer, b, sizeof b)) > 0)
        for (i = 0; i < got; ++i)
            if (jbp_parse(rx, b[i], now_ms()) == JBP_PARSE_FRAME) {
                if (rx->type == JBP_LIST)
                    ++*lists_seen;
                else if (rx->type == JBP_START && !*acked) {
                    uint8_t f[JBP_HEADER_BYTES + JBP_TRAILER_BYTES];
                    size_t fn = jbp_frame(f, JBP_ACK, rx->seq, NULL, 0);
                    usleep((useconds_t)(limit + 4) * 1000u);
                    if (write(peer, f, fn) == (ssize_t)fn)
                        *acked = 1;
                }
            }
}

int main(int argc, char **argv)
{
    static run r;
    static char limit_arg[16], tcp_arg[64];
    const char *mode;
    unsigned long limit;
    int peer = -1, other = -1, fill[4] = {-1, -1, -1, -1}, k, connect_case, late_ack, done;
    int lists_seen = 0, acked = 0;
    jbp_parser rx;
    unsigned long pushed = 0;
    uint32_t t0, took;
    pthread_t th;
    FILE *log;
    int saved_err;
    static char said[8192], want[160];

    /* jbprobe closes the line when it gives up; a NAK written after that
       must fail with EPIPE, not end this program. */
    signal(SIGPIPE, SIG_IGN);
    if (argc != 4 || (limit = strtoul(argv[2], NULL, 10)) == 0 || limit > 60000)
        return 2;
    mode = argv[1];
    connect_case = strcmp(mode, "tcp-connect") == 0;
    late_ack = strcmp(mode, "pty-late-ack") == 0;
    jbp_parser_init(&rx);
    if (pthread_mutex_init(&r.lock, NULL) != 0)
        return 2;
    make_naks();
    snprintf(limit_arg, sizeof limit_arg, "%lu", limit);
    r.argv[r.argc++] = "jbprobe";
    r.argv[r.argc++] = "send";
    if (strcmp(mode, "pty-naks") == 0 || late_ack) {
        struct termios tio;
        char *name;
        peer = posix_openpt(O_RDWR | O_NOCTTY);
        if (peer < 0 || grantpt(peer) != 0 || unlockpt(peer) != 0 || (name = ptsname(peer)) == NULL) {
            printf("no pseudo-terminal here: skipped\n");
            return 77;
        }
        /* Raw from the start (no echo of the NAKs back to the peer); held
           open so that the terminal stays up whatever jbprobe does. */
        other = open(name, O_RDWR | O_NOCTTY);
        if (other < 0 || tcgetattr(other, &tio) != 0)
            return 2;
        tio.c_iflag = 0;
        tio.c_oflag = 0;
        tio.c_lflag = 0;
        tcsetattr(other, TCSANOW, &tio);
        r.argv[r.argc++] = "--tty";
        r.argv[r.argc++] = name;
    } else if (strcmp(mode, "tcp-naks") == 0 || connect_case) {
        struct sockaddr_in a;
        socklen_t len = sizeof a;
        int small = 2048;
        other = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (other < 0 || setsockopt(other, SOL_SOCKET, SO_RCVBUF, &small, sizeof small) != 0 ||
            bind(other, (struct sockaddr *)&a, sizeof a) != 0 || listen(other, connect_case ? 0 : 1) != 0 ||
            getsockname(other, (struct sockaddr *)&a, &len) != 0) {
            printf("no loopback here: skipped\n");
            return 77;
        }
        if (connect_case) {
            /* The listener's queue filled by connections never accepted:
               a SYN to it then goes unanswered. */
            for (k = 0; k < 4; ++k) {
                fill[k] = socket(AF_INET, SOCK_STREAM, 0);
                if (fill[k] < 0)
                    return 2;
                fcntl(fill[k], F_SETFL, fcntl(fill[k], F_GETFL) | O_NONBLOCK);
                (void)connect(fill[k], (struct sockaddr *)&a, sizeof a);
            }
            usleep(200000);
        }
        snprintf(tcp_arg, sizeof tcp_arg, "127.0.0.1:%u", (unsigned)ntohs(a.sin_port));
        r.argv[r.argc++] = "--tcp";
        r.argv[r.argc++] = tcp_arg;
    } else
        return 2;
    r.argv[r.argc++] = "--start-ms";
    r.argv[r.argc++] = limit_arg;
    r.argv[r.argc++] = "--passes";
    r.argv[r.argc++] = "1";
    r.argv[r.argc++] = argv[3];
    r.argv[r.argc++] = "/dev/null";
    r.argv[r.argc] = NULL;

    /* jbprobe's stderr into a file, to be read back. */
    fflush(stderr);
    if ((log = tmpfile()) == NULL || (saved_err = dup(2)) < 0 || dup2(fileno(log), 2) < 0)
        return 2;
    t0 = now_ms();
    if (pthread_create(&th, NULL, run_jbprobe, &r) != 0)
        return 2;
    if (strcmp(mode, "tcp-naks") == 0) {
        /* The connection, accepted; then NAKs and no reads. */
        struct pollfd pf;
        pf.fd = other;
        pf.events = POLLIN;
        pf.revents = 0;
        if (poll(&pf, 1, (int)limit) <= 0 || (peer = accept(other, NULL, NULL)) < 0) {
            printf("FAIL: jbprobe did not connect\n");
            return 1;
        }
    }
    if (peer >= 0)
        fcntl(peer, F_SETFL, fcntl(peer, F_GETFL) | O_NONBLOCK);
    while (!(done = is_done(&r)) && now_ms() - t0 < limit + MARGIN_MS) {
        if (late_ack) {
            late_ack_read(peer, &rx, limit, &acked, &lists_seen);
            usleep(1000);
            continue;
        }
        if (peer >= 0) {
            ssize_t w = write(peer, naks, naks_n);
            if (w > 0)
                pushed += (unsigned long)w;
        }
        usleep(2000);
    }
    took = now_ms() - t0;
    if (late_ack)
        late_ack_read(peer, &rx, limit, &acked, &lists_seen); /* what came last */
    fflush(stderr);
    dup2(saved_err, 2);
    {
        size_t got;
        rewind(log);
        got = fread(said, 1, sizeof said - 1, log);
        said[got] = 0;
        printf("jbprobe said:\n%s", said);
    }
    if (!done) {
        if (late_ack)
            printf("FAIL: %s: jbprobe still running %u ms after it began, its limit %lu ms (%s; %d "
                   "LIST frames received)\n",
                   mode, (unsigned)took, limit,
                   acked ? "START acknowledged after its limit" : "no START seen", lists_seen);
        else
            printf("FAIL: %s: jbprobe still running %u ms after it began, its limit %lu ms (%lu bytes of "
                   "NAKs sent to it, none of its output read)\n",
                   mode, (unsigned)took, limit, pushed);
        fflush(stdout);
        _exit(1);
    }
    pthread_join(th, NULL);
    if (late_ack)
        printf("%s: jbprobe send gave up after %u ms (limit %lu ms), exit status %d; START "
               "acknowledged %lu ms after it was read, %d LIST frames received\n",
               mode, (unsigned)took, limit, r.rc, limit + 4, lists_seen);
    else
        printf("%s: jbprobe send gave up after %u ms (limit %lu ms), exit status %d; %lu bytes of NAKs "
               "sent to it, none of its output read\n",
               mode, (unsigned)took, limit, r.rc, pushed);
    if (r.rc != (connect_case ? 2 : 1) || took + 50 < limit) {
        printf("FAIL: expected exit status %d no earlier than the limit\n", connect_case ? 2 : 1);
        return 1;
    }
    if (late_ack)
        snprintf(want, sizeof want, "failed: no probe took the START\n");
    else
        snprintf(want, sizeof want, connect_case ? "%s: no connection within %lu ms\n"
                                                  : "failed: the line took no output until the session's "
                                                    "time limit\n",
                 tcp_arg, limit);
    if (strstr(said, want) == NULL) {
        printf("FAIL: not the reason required: %s", want);
        return 1;
    }
    if (late_ack && (!acked || lists_seen != 0)) {
        printf("FAIL: %s\n", acked ? "jbprobe took an ACK that came after the START limit and sent the list"
                                   : "jbprobe sent no START");
        return 1;
    }
    if (!connect_case && !late_ack && pushed == 0) {
        printf("FAIL: no NAK reached jbprobe\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
