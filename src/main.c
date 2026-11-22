#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <time.h>

#include "constants.h"
#include "worker.h"
#include "http.h"
#include "stats.h"
#include "tui.h"
#include "history.h"

/* ── globals ───────────────────────────────────────────────────────── */

static volatile int g_running = 1;

typedef struct thread_ctx {
    worker_t         worker;
    request_tpl_t   *templates;
    int              num_templates;
    int              pipeline_depth;
    int              num_conns;
    int              requests_per_conn;
    int              expected_status;
    int              ws_mode;
    int              recv_buf_size;
    int              cqe_latency;
    int              per_tpl_latency;
    int              tls;
    const char      *tls_sni;
    const char      *ws_host;
    int              ws_port;
    const char      *ws_path;
    const uint8_t   *ws_payload;
    int              ws_payload_len;
    const struct sockaddr_in *addrs;
    int              num_addrs;
    uint64_t         interval_ns;
    uint64_t         epoch_ns;
    int              total_conns;
    int              connect_limit;
    int              conn_target;     /* --ramp-p99: the first step's share */
    uint64_t         ramp_p99_us;
} thread_ctx_t;

/* ── URL parser ────────────────────────────────────────────────────── */

static int parse_url(const char *url, char *host, int host_sz,
                     int *port, char *path, int path_sz, int *tls)
{
    const char *hp;
    if (strncmp(url, "http://", 7) == 0) {
        hp = url + 7;
        *tls = 0;
    } else if (strncmp(url, "https://", 8) == 0) {
        hp = url + 8;
        *tls = 1;
    } else {
        fprintf(stderr, "Error: only http:// and https:// URLs supported\n");
        return -1;
    }
    const char *slash = strchr(hp, '/');
    const char *colon = strchr(hp, ':');

    if (slash) {
        snprintf(path, path_sz, "%s", slash);
    } else {
        snprintf(path, path_sz, "/");
        slash = hp + strlen(hp);
    }

    if (colon && colon < slash) {
        int hlen = (int)(colon - hp);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host, hp, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        int hlen = (int)(slash - hp);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host, hp, hlen);
        host[hlen] = '\0';
        *port = *tls ? 443 : 80;
    }
    return 0;
}

static int parse_duration(const char *s)
{
    const int val = atoi(s);
    const int len = strlen(s);
    if (len > 0 && (s[len-1] == 's' || s[len-1] == 'S'))
        return val;
    if (len > 0 && (s[len-1] == 'm' || s[len-1] == 'M'))
        return val * 60;
    return val;
}

/* ── placeholder scanning for {RAND:min:max} and {SEQ:start} ──────── */

static int count_digits(uint64_t n)
{
    if (n == 0) return 1;
    int d = 0;
    while (n > 0) { d++; n /= 10; }
    return d;
}

/*
 * Scan raw template bytes for a single {RAND:min:max} or {SEQ:start}
 * placeholder. If found, replace it in-place with zero-padded digits
 * and populate ph. Returns 0 on success, -1 on parse error.
 * The buffer may be resized if the placeholder text and the padded
 * width differ — caller must use the returned buf/len.
 */
static int scan_placeholder(char **buf, int *len, placeholder_t *ph)
{
    memset(ph, 0, sizeof(*ph));
    ph->type = SUB_NONE;

    /* find {RAND: or {SEQ: */
    char *p = NULL;
    for (int i = 0; i < *len - 5; i++) {
        if ((*buf)[i] == '{' &&
            (memcmp(*buf + i, "{RAND:", 6) == 0 ||
             memcmp(*buf + i, "{SEQ:",  5) == 0)) {
            p = *buf + i;
            break;
        }
    }
    if (!p) return 0; /* no placeholder */

    /* find closing } */
    char *end = memchr(p, '}', *len - (int)(p - *buf));
    if (!end) {
        fprintf(stderr, "Error: unclosed placeholder in raw template\n");
        return -1;
    }

    int ph_start = (int)(p - *buf);
    int ph_len   = (int)(end - p) + 1; /* includes { and } */

    /* parse type and params */
    char tmp[128];
    int tlen = ph_len < (int)sizeof(tmp) ? ph_len : (int)sizeof(tmp) - 1;
    memcpy(tmp, p, tlen);
    tmp[tlen] = '\0';

    if (strncmp(tmp, "{RAND:", 6) == 0) {
        ph->type = SUB_RAND;
        uint64_t mn = 0, mx = 0;
        if (sscanf(tmp, "{RAND:%lu:%lu}", &mn, &mx) != 2 || mx < mn) {
            fprintf(stderr, "Error: bad {RAND:min:max} syntax: %s\n", tmp);
            return -1;
        }
        ph->min = mn;
        ph->max = mx;
        ph->width = count_digits(mx);
    } else if (strncmp(tmp, "{SEQ:", 5) == 0) {
        ph->type = SUB_SEQ;
        uint64_t start = 0;
        if (sscanf(tmp, "{SEQ:%lu}", &start) != 1) {
            fprintf(stderr, "Error: bad {SEQ:start} syntax: %s\n", tmp);
            return -1;
        }
        ph->min = start;
        ph->max = 0;
        atomic_store(&ph->seq, start);
        /* Width matches digits(start) so the first emitted value has no
         * leading zeros. JSON number fields reject leading zeros per
         * RFC 8259, so anything else silently corrupts POST bodies.
         * Users who need the counter to grow past 10^digits(start)
         * must pick a start with enough digit headroom — e.g.
         * {SEQ:10000001} covers ~89M increments with 8-digit width. */
        ph->width = count_digits(start);
    } else {
        return 0;
    }

    ph->offset = ph_start;

    /* replace placeholder text with zero-padded digits in the buffer.
     * If ph_len != ph->width, we need to shift the rest of the buffer. */
    int delta = ph->width - ph_len;
    int new_len = *len + delta;
    if (delta != 0) {
        char *new_buf = malloc(new_len);
        if (!new_buf) return -1;
        memcpy(new_buf, *buf, ph_start);
        memset(new_buf + ph_start, '0', ph->width);
        memcpy(new_buf + ph_start + ph->width,
               *buf + ph_start + ph_len,
               *len - ph_start - ph_len);
        free(*buf);
        *buf = new_buf;
        *len = new_len;
    } else {
        memset(*buf + ph_start, '0', ph->width);
    }

    return 0;
}

/* ── raw request file loading ──────────────────────────────────────── */

static char *load_file(const char *path, int *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open raw request file '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz);
    if (!buf) {
        fprintf(stderr, "Error: out of memory reading '%s'\n", path);
        fclose(f);
        return NULL;
    }
    if ((long)fread(buf, 1, sz, f) != sz) {
        fprintf(stderr, "Error: failed to read '%s'\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (int)sz;
    return buf;
}

static int build_pipeline_from_raw(const char *raw, int raw_len,
                                   int pipeline_depth, request_tpl_t *tpl)
{
    tpl->request_len  = raw_len;
    tpl->pipeline_len = raw_len * pipeline_depth;
    tpl->pipeline_buf = malloc(tpl->pipeline_len);
    if (!tpl->pipeline_buf) {
        fprintf(stderr, "Error: out of memory building pipeline\n");
        return -1;
    }
    memcpy(tpl->pipeline_buf, raw, raw_len);
    for (int i = 1; i < pipeline_depth; i++)
        memcpy(tpl->pipeline_buf + i * raw_len, raw, raw_len);
    return 0;
}

/* ── worker thread entry ───────────────────────────────────────────── */

static void *worker_thread(void *arg)
{
    thread_ctx_t *ctx = arg;
    worker_init(&ctx->worker, ctx->worker.id, ctx->addrs, ctx->num_addrs,
                ctx->templates, ctx->num_templates, ctx->pipeline_depth,
                ctx->num_conns, ctx->worker.conn_offset,
                ctx->requests_per_conn, ctx->expected_status,
                ctx->ws_mode, ctx->ws_host, ctx->ws_port, ctx->ws_path,
                ctx->ws_payload, ctx->ws_payload_len,
                ctx->recv_buf_size,
                ctx->cqe_latency, ctx->per_tpl_latency,
                ctx->tls, ctx->tls_sni,
                ctx->interval_ns, ctx->epoch_ns, ctx->total_conns,
                ctx->connect_limit, ctx->conn_target, ctx->ramp_p99_us,
                &g_running);
    worker_loop(&ctx->worker);
    worker_close_all(&ctx->worker);
    return NULL;
}

/* ── signal handler ────────────────────────────────────────────────── */

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── phases ────────────────────────────────────────────────────────── */

/* Ask every worker for `bits` (CTL_*) and wait until each has done it. A
   worker that saw Ctrl-C has left its loop without doing it, so stop waiting
   once g_running drops; otherwise this waits forever. */
static void ctl_all(thread_ctx_t *ctxs, const int num_threads, const int bits)
{
    for (int i = 0; i < num_threads; i++)
        atomic_fetch_or_explicit(&ctxs[i].worker.ctl, bits, memory_order_release);
    for (int i = 0; i < num_threads; i++)
        while ((atomic_load_explicit(&ctxs[i].worker.ctl, memory_order_acquire) & bits) &&
               g_running)
            nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 100000 }, NULL); /* 100us */
}

static long ms_since(const struct timespec *t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000L + (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

/* Wait for the first `target` connections to settle (become ready, or fail
 * once and retry): at least WARMUP_MIN_MS, then until they all have, for at
 * most warmup_max_sec in all. Returns how many had. A fixed sleep let slow
 * setup — a WebSocket upgrade the server answers late — run into the measured
 * window, where those connections sat idle and dragged the rate down. Capped
 * so a server that never answers can't stall the run. */
static int wait_settled(thread_ctx_t *ctxs, const int num_threads, const int target,
                        const int warmup_max_sec)
{
    nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = WARMUP_MIN_MS * 1000000L }, NULL);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int settled = 0;
        for (int i = 0; i < num_threads; i++)
            settled += atomic_load_explicit(&ctxs[i].worker.conns_settled,
                                            memory_order_relaxed);
        if (settled >= target || !g_running)
            return settled;
        if (ms_since(&start) >= (long)warmup_max_sec * 1000L - WARMUP_MIN_MS)
            return settled;
        nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 1000000 }, NULL); /* 1ms */
    }
}

/* Sleep for ms, or until Ctrl-C */
static void sleep_running(const long ms)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (g_running) {
        const long left = ms - ms_since(&start);
        if (left <= 0)
            break;
        nanosleep(&(struct timespec){ .tv_sec = 0,
                                      .tv_nsec = (left < 50 ? left : 50) * 1000000L }, NULL);
    }
}

/* Responses outside the expected status class (-s). In WebSocket mode every
   frame counts as a 2xx. */
static uint64_t unexpected_responses(const worker_stats_t *s, const int expected_status)
{
    uint64_t expected = 0;
    if (expected_status >= 200 && expected_status < 300)      expected = s->status_2xx;
    else if (expected_status >= 300 && expected_status < 400) expected = s->status_3xx;
    else if (expected_status >= 400 && expected_status < 500) expected = s->status_4xx;
    else if (expected_status >= 500 && expected_status < 600) expected = s->status_5xx;

    /* Saturating: expected is a separate counter, so any accounting bug that
       pushes it above `responses` would otherwise wrap this to ~2^64 and
       print a nonsense warning instead of a small, believable one. */
    return s->responses > expected ? s->responses - expected : 0;
}

/* ── --ramp-p99 ────────────────────────────────────────────────────── */

typedef struct ramp_plan {
    uint64_t p99_us;          /* the threshold; 0 = no ramp */
    int      start;           /* the first step's connections */
    double   factor;          /* x<factor> multiplies each step's connections... */
    int      increment;       /* ...or +<n> adds to them (factor is then 0) */
    int      ceiling;         /* -c, the last step */
    int      window_sec;      /* -d, each step's window */
    int      settle_sec;      /* --settle, before each step's window */
    int      warmup_max_sec;  /* --warmup-max, for each step's new connections */
    int      expected_status; /* -s */
} ramp_plan_t;

/* "5ms", "500us", "1.5s" -> microseconds. The unit is required: a bare number
   means seconds everywhere else on the command line. */
static int parse_latency(const char *s, uint64_t *out_us)
{
    char *end;
    errno = 0;
    const double v = strtod(s, &end);
    if (end == s || errno || !(v > 0))
        return -1;
    double us;
    if (strcmp(end, "us") == 0)      us = v;
    else if (strcmp(end, "ms") == 0) us = v * 1e3;
    else if (strcmp(end, "s") == 0)  us = v * 1e6;
    else                             return -1;
    if (!(us >= 1) || us > 1e12)
        return -1;
    *out_us = (uint64_t)(us + 0.5);
    return 0;
}

/* "x2" (x1.5, ...) multiplies each step's connections, "+256" adds to them */
static int parse_ramp_step(const char *s, double *factor, int *increment)
{
    char *end;
    errno = 0;
    if (s[0] == 'x' || s[0] == 'X') {
        const double f = strtod(s + 1, &end);
        if (end == s + 1 || *end || errno || !(f > 1.0) || f > 1e6)
            return -1;
        *factor = f;
        *increment = 0;
        return 0;
    }
    if (s[0] == '+') {
        const long n = strtol(s + 1, &end, 10);
        if (end == s + 1 || *end || errno || n < 1 || n > INT_MAX)
            return -1;
        *factor = 0;
        *increment = (int)n;
        return 0;
    }
    return -1;
}

/* The step after n connections: always more, never past the ceiling */
static int ramp_next(const ramp_plan_t *p, const int n)
{
    double next;
    if (p->increment > 0) {
        next = (double)n + p->increment;
    } else {
        next = (double)(long long)((double)n * p->factor);
        if (next < (double)n * p->factor) next += 1;   /* round up */
    }
    if (next < (double)n + 1) next = (double)n + 1;
    if (next > p->ceiling)    next = p->ceiling;
    return (int)next;
}

/* Worker i's share of a fleet of n, split the way -c is */
static int ramp_share(const int n, const int num_threads, const int i)
{
    return n / num_threads + (i < n % num_threads ? 1 : 0);
}

/* One window of a step, from the workers' merged copies: its figures and
   whether it passed. settled is how many of the step's connections had set
   up when the window opened. */
static ramp_step_t ramp_evaluate(const worker_stats_t *s, const ramp_plan_t *p,
                                 const int n, const int settled, const int retry,
                                 const double elapsed)
{
    ramp_step_t st = {
        .connections = n,
        .set_up      = settled,
        .retry       = retry,
        .elapsed_sec = elapsed,
        .responses   = s->responses,
        .samples     = s->latency_count,
        .p50_us      = stats_percentile(s, 0.50),
        .p90_us      = stats_percentile(s, 0.90),
        .p99_us      = stats_percentile(s, 0.99),
        .p999_us     = stats_percentile(s, 0.999),
        .active      = s->conns_active,
        .dead        = s->conns_dead,
        .silent      = s->conns_silent,
        .errors      = s->connect_errors + s->read_errors + s->ws_upgrade_failures,
        .unexpected  = unexpected_responses(s, p->expected_status),
    };
    if (st.dead)                    st.verdict = RAMP_SOCKETS;
    else if (settled < n)           st.verdict = RAMP_SETUP;
    else if (!st.responses)         st.verdict = RAMP_RESPONSES;
    else if (st.silent)             st.verdict = RAMP_SILENT;
    else if (st.unexpected)         st.verdict = RAMP_STATUS;
    else if (st.errors)             st.verdict = RAMP_ERRORS;
    else if (st.p99_us > p->p99_us) st.verdict = RAMP_P99;
    else                            st.verdict = RAMP_OK;
    return st;
}

/* --ramp-p99: grow one fleet a step at a time, until a step fails or -c is
 * reached. A step opens its new connections and waits for them as the warmup
 * does, runs --settle, then measures one -d window: the workers zero their
 * stats as it opens and copy them out as it closes, and main merges the copies
 * and works out p99 from them, once per window. Connections stay open from
 * one step to the next, so a step only opens the difference.
 *
 * A failing step is measured once more, straight away, before the ramp stops:
 * p99 isn't strictly monotonic in the connection count, and one noisy window
 * shouldn't end the ramp. Not when connections lost their sockets, though,
 * which they never get back.
 *
 * a and b take turns holding a window's merged stats. Returns the one holding
 * the last step that passed, or else the last window measured; NULL if Ctrl-C
 * came before any. */
static worker_stats_t *run_ramp(thread_ctx_t *ctxs, const int num_threads,
                                const ramp_plan_t *p, FILE *out,
                                worker_stats_t *a, worker_stats_t *b,
                                ramp_result_t *res)
{
    worker_stats_t *cur = a, *best = NULL;
    int cap = 0;
    res->p99_max_us = p->p99_us;
    res->best = -1;
    res->stop = RAMP_INTERRUPTED;

    ramp_print_header(out);
    for (int n = p->start, step = 0; ; step++) {
        if (step > 0) {
            for (int i = 0; i < num_threads; i++)
                ctxs[i].worker.conn_target_next = ramp_share(n, num_threads, i);
            ctl_all(ctxs, num_threads, CTL_GROW);
        }
        for (int retry = 0; ; retry++) {
            const int settled = wait_settled(ctxs, num_threads, n, p->warmup_max_sec);
            if (!retry && p->settle_sec > 0)
                sleep_running(p->settle_sec * 1000L);
            if (!g_running) goto out;

            ctl_all(ctxs, num_threads, CTL_RESET);
            struct timespec opened;
            clock_gettime(CLOCK_MONOTONIC, &opened);
            sleep_running(p->window_sec * 1000L);
            if (!g_running) goto out;   /* a window cut short says nothing */
            ctl_all(ctxs, num_threads, CTL_SNAPSHOT);
            if (!g_running) goto out;

            /* The window closed when the last worker copied its stats out */
            stats_clear(cur);
            struct timespec closed = opened;
            for (int i = 0; i < num_threads; i++) {
                const worker_t *w = &ctxs[i].worker;
                stats_merge(cur, w->snap);
                if (w->snap_at.tv_sec > closed.tv_sec ||
                    (w->snap_at.tv_sec == closed.tv_sec && w->snap_at.tv_nsec > closed.tv_nsec))
                    closed = w->snap_at;
            }
            const double elapsed = (closed.tv_sec - opened.tv_sec)
                                 + (closed.tv_nsec - opened.tv_nsec) / 1e9;

            if (res->num_steps == cap) {
                cap = cap ? cap * 2 : 16;
                ramp_step_t *grown = realloc(res->steps, (size_t)cap * sizeof(*grown));
                if (!grown) {
                    fprintf(stderr, "Error: out of memory\n");
                    exit(1);
                }
                res->steps = grown;
            }
            const ramp_step_t st = ramp_evaluate(cur, p, n, settled, retry, elapsed);
            res->steps[res->num_steps++] = st;

            const int again = st.verdict != RAMP_OK && !retry && st.verdict != RAMP_SOCKETS;
            ramp_print_step(out, &st, p->p99_us, again);
            if (st.verdict == RAMP_OK) {
                res->best = res->num_steps - 1;
                best = cur;
                cur = cur == a ? b : a;
                break;
            }
            if (!again) {
                res->stop = st.verdict;
                goto out;
            }
        }
        if (n >= p->ceiling) {
            res->stop = RAMP_CEILING;
            goto out;
        }
        n = ramp_next(p, n);
    }
out:
    return best ? best : (res->num_steps ? cur : NULL);
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    int num_connections = 100;
    int num_threads = 1;
    int duration_sec = 10;
    int pipeline_depth = 1;
    int requests_per_conn = 0; /* 0 = keep-alive forever */
    int expected_status = 200; /* expected HTTP status code */
    int ws_mode = 0;
    int tui_mode = 0;
    int json_mode = 0;
    int cqe_latency = 0;
    int per_tpl_latency = 0;
    int recv_buf_size = RECV_BUF_SIZE_DEFAULT;
    int hist_buckets = 0; /* 0 = default (10) */
    double rate = 0;          /* --rate: total req/s; 0 = closed loop */
    int addr_span = 1;        /* --addr-span: consecutive target addresses */
    int connect_limit = 0;    /* --connect-limit: first attempts in flight per thread
                                 (for https, through the handshake) */
    int warmup_max_sec = WARMUP_MAX_MS / 1000;
    int settle_sec = 0;
    ramp_plan_t ramp = { .factor = 2.0 };   /* --ramp-p99; x2 unless --ramp-step */
    int ramp_start_set = 0, ramp_step_set = 0;
    const char *ws_message = "hello";
    const char *url = NULL;
    const char *raw_files = NULL;

    /* Parse CLI args */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            url = argv[i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            num_connections = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            duration_sec = parse_duration(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pipeline_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            requests_per_conn = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            expected_status = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--raw") == 0 || strcmp(argv[i], "-R") == 0) && i + 1 < argc) {
            raw_files = argv[++i];
        } else if (strcmp(argv[i], "--ws") == 0) {
            ws_mode = 1;
        } else if (strcmp(argv[i], "--ws-msg") == 0 && i + 1 < argc) {
            ws_message = argv[++i];
            ws_mode = 1;
        } else if (strcmp(argv[i], "--tui") == 0) {
            tui_mode = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            json_mode = 1;
        } else if ((strcmp(argv[i], "--buckets") == 0 || strcmp(argv[i], "-b") == 0) && i + 1 < argc) {
            hist_buckets = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cqe-latency") == 0) {
            cqe_latency = 1;
        } else if (strcmp(argv[i], "--recv-buf") == 0 && i + 1 < argc) {
            recv_buf_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--per-tpl-latency") == 0) {
            per_tpl_latency = 1;
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate = atof(argv[++i]);
            if (rate <= 0) {
                fprintf(stderr, "Error: --rate must be a positive number of requests/s\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--addr-span") == 0 && i + 1 < argc) {
            addr_span = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--connect-limit") == 0 && i + 1 < argc) {
            connect_limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup-max") == 0 && i + 1 < argc) {
            warmup_max_sec = parse_duration(argv[++i]);
        } else if (strcmp(argv[i], "--settle") == 0 && i + 1 < argc) {
            settle_sec = parse_duration(argv[++i]);
        } else if (strcmp(argv[i], "--ramp-p99") == 0 && i + 1 < argc) {
            if (parse_latency(argv[++i], &ramp.p99_us) < 0) {
                fprintf(stderr, "Error: --ramp-p99 takes a latency with its unit, "
                                "like 5ms, 500us or 1.5s (got '%s')\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--ramp") == 0 && i + 1 < argc) {
            ramp.start = atoi(argv[++i]);
            ramp_start_set = 1;
        } else if (strcmp(argv[i], "--ramp-step") == 0 && i + 1 < argc) {
            if (parse_ramp_step(argv[++i], &ramp.factor, &ramp.increment) < 0) {
                fprintf(stderr, "Error: --ramp-step takes x<factor> above 1 or "
                                "+<connections> (got '%s')\n", argv[i]);
                return 1;
            }
            ramp_step_set = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("gcannon %s\n", GCANNON_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--clear-history") == 0) {
            history_clear();
            printf("Run history cleared.\n");
            return 0;
        } else {
            fprintf(stderr, "Usage: gcannon <url> -c <conns> -t <threads> "
                            "-d <duration> [-p <pipeline>] [-r <req/conn>] "
                            "[-s <status>] [-R|--raw file1,file2,...] "
                            "[--ws [--ws-msg <message>]] [--tui] [--json] [-b <buckets>] "
                            "[--recv-buf <bytes>] [--cqe-latency] [--rate <req/s>] "
                            "[--addr-span <n>] [--connect-limit <n>] "
                            "[--warmup-max <duration>] [--settle <duration>] "
                            "[--ramp-p99 <latency> [--ramp <conns>] [--ramp-step x2|+<n>]] "
                            "[--version]\n");
            return 1;
        }
    }

    if (!url && !raw_files) {
        fprintf(stderr, "Usage: gcannon <url> -c <conns> -t <threads> "
                        "-d <duration> [-p <pipeline>] [-r <req/conn>] "
                        "[-R|--raw file1,file2,...] [--tui] [--json]\n");
        return 1;
    }

    /* Validate counts before anything derives from them. num_threads in
       particular is a divisor below, so -t 0 used to die with SIGFPE. */
    if (num_threads < 1) {
        fprintf(stderr, "Error: -t must be at least 1 (got %d)\n", num_threads);
        return 1;
    }
    if (num_connections < 1) {
        fprintf(stderr, "Error: -c must be at least 1 (got %d)\n", num_connections);
        return 1;
    }
    if (pipeline_depth < 1) {
        fprintf(stderr, "Error: -p must be at least 1 (got %d)\n", pipeline_depth);
        return 1;
    }
    if (duration_sec < 1) {
        fprintf(stderr, "Error: -d must be at least 1 second (got %d)\n", duration_sec);
        return 1;
    }
    if (requests_per_conn < 0) {
        fprintf(stderr, "Error: -r must not be negative (got %d)\n", requests_per_conn);
        return 1;
    }
    if (num_connections < num_threads) {
        /* Threads past the remainder would get zero connections and spin in
           the event loop doing nothing, quietly producing a slower run than
           the flags suggest. */
        fprintf(stderr, "Error: -c (%d) must be at least -t (%d); every thread "
                        "needs at least one connection\n",
                num_connections, num_threads);
        return 1;
    }

    /* --rate paces requests one at a time per connection: a pipeline would
       send several on one schedule slot, and a per-connection quota would end
       a connection's schedule early. */
    if (rate > 0 && pipeline_depth != 1) {
        fprintf(stderr, "Error: --rate needs -p 1 (got -p %d)\n", pipeline_depth);
        return 1;
    }
    if (rate > 0 && requests_per_conn > 0) {
        fprintf(stderr, "Error: --rate cannot be combined with -r\n");
        return 1;
    }
    if (addr_span < 1 || addr_span > 65536) {
        fprintf(stderr, "Error: --addr-span must be 1..65536 (got %d)\n", addr_span);
        return 1;
    }
    if (connect_limit < 0) {
        fprintf(stderr, "Error: --connect-limit must not be negative (got %d)\n",
                connect_limit);
        return 1;
    }
    if (warmup_max_sec < 1) {
        fprintf(stderr, "Error: --warmup-max must be at least 1 second\n");
        return 1;
    }
    if (settle_sec < 0) {
        fprintf(stderr, "Error: --settle must not be negative (got %d)\n", settle_sec);
        return 1;
    }

    /* --ramp-p99: -c is the ceiling and -d each step's window */
    if (!ramp.p99_us && (ramp_start_set || ramp_step_set)) {
        fprintf(stderr, "Error: --ramp and --ramp-step need --ramp-p99\n");
        return 1;
    }
    if (ramp.p99_us) {
        if (ramp.p99_us >= TIER2_MAX_US) {
            fprintf(stderr, "Error: --ramp-p99 must be under %ds, the top of the "
                            "latency histogram\n", TIER2_MAX_US / 1000000);
            return 1;
        }
        /* --rate lays every connection's send slot out for the whole fleet,
           so a fleet part of the way up would send in bursts; --tui's
           progress bar counts down one fixed duration */
        if (rate > 0) {
            fprintf(stderr, "Error: --ramp-p99 cannot be combined with --rate\n");
            return 1;
        }
        if (tui_mode) {
            fprintf(stderr, "Error: --ramp-p99 cannot be combined with --tui\n");
            return 1;
        }
        if (!ramp_start_set)
            ramp.start = num_threads;
        if (ramp.start < num_threads) {
            fprintf(stderr, "Error: --ramp (%d) must be at least -t (%d); every "
                            "thread needs at least one connection\n",
                    ramp.start, num_threads);
            return 1;
        }
        if (ramp.start > num_connections) {
            fprintf(stderr, "Error: --ramp (%d) must not be above -c (%d), where "
                            "the ramp stops\n", ramp.start, num_connections);
            return 1;
        }
        ramp.ceiling         = num_connections;
        ramp.window_sec      = duration_sec;
        ramp.settle_sec      = settle_sec;
        ramp.warmup_max_sec  = warmup_max_sec;
        ramp.expected_status = expected_status;
    }

    if (pipeline_depth > PIPELINE_DEPTH_MAX)
        pipeline_depth = PIPELINE_DEPTH_MAX;

    if (recv_buf_size < 512) {
        fprintf(stderr, "Error: --recv-buf must be at least 512\n");
        return 1;
    }
    if (recv_buf_size > 4 * 1024 * 1024) {
        /* Each worker pre-allocates BUF_RING_ENTRIES buffers of this size, so an
           unbounded value (e.g. --recv-buf 1000000000) asks for hundreds of GB
           per worker. Reject obviously-wrong sizes up front. */
        fprintf(stderr, "Error: --recv-buf must be at most %d (each worker "
                        "pre-allocates %d buffers of this size)\n",
                        4 * 1024 * 1024, BUF_RING_ENTRIES);
        return 1;
    }

    /* Resolve target host */
    char host[256] = {0}, path[1024] = "/";
    int port = 80;
    int tls = 0;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;

    if (url) {
        if (parse_url(url, host, sizeof(host), &port, path, sizeof(path), &tls) < 0)
            return 1;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            struct hostent *he = gethostbyname(host);
            if (!he) {
                fprintf(stderr, "Error: cannot resolve host '%s'\n", host);
                return 1;
            }
            memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        }
    }

    /* https:// hands the record layer to kernel TLS. Check the kernel can do
       it before connecting anything, or every connection would handshake and
       then fail. */
    const char *tls_sni = NULL;
    if (tls) {
        const int rc = ktls_probe();
        if (rc < 0) {
            fprintf(stderr, "Error: https:// needs kernel TLS, which this kernel "
                            "refused (%s)%s\n", strerror(-rc),
                    rc == -ENOENT ? " - is the tls module loaded?" : "");
            return 1;
        }
        /* SNI carries host names only, never an address (RFC 6066) */
        struct in_addr ip;
        if (inet_pton(AF_INET, host, &ip) != 1)
            tls_sni = host;
    }

    /* Build request templates */
    request_tpl_t *templates = NULL;
    int num_templates = 0;

    if (raw_files) {
        /* Count commas to get number of files */
        int count = 1;
        for (const char *p = raw_files; *p; p++)
            if (*p == ',') count++;

        /* worker_stats_t indexes tpl_responses[]/tpl_responses_2xx[] by
           template, and both are fixed at MAX_TEMPLATES. Going past that
           writes over the adjacent tpl_latency pointer. */
        if (count > MAX_TEMPLATES) {
            fprintf(stderr, "Error: at most %d --raw templates supported "
                            "(got %d)\n", MAX_TEMPLATES, count);
            return 1;
        }

        templates = calloc(count, sizeof(request_tpl_t));
        if (!templates) { fprintf(stderr, "Error: out of memory\n"); return 1; }

        /* Parse comma-separated file list */
        char *files_dup = strdup(raw_files);
        if (!files_dup) { fprintf(stderr, "Error: out of memory\n"); return 1; }
        char *saveptr;
        char *tok = strtok_r(files_dup, ",", &saveptr);
        while (tok) {
            int raw_len;
            char *raw = load_file(tok, &raw_len);
            if (!raw) { free(files_dup); return 1; }

            /* If no URL was given, extract host:port from the first raw file's Host header */
            if (!url && num_templates == 0) {
                char *host_hdr = strstr(raw, "Host: ");
                if (!host_hdr) host_hdr = strstr(raw, "host: ");
                if (host_hdr) {
                    host_hdr += 6;
                    char *eol = strstr(host_hdr, "\r\n");
                    int hlen = eol ? (int)(eol - host_hdr) : (int)strlen(host_hdr);
                    if (hlen >= (int)sizeof(host)) hlen = sizeof(host) - 1;
                    memcpy(host, host_hdr, hlen);
                    host[hlen] = '\0';
                    /* Parse host:port */
                    char *colon = strchr(host, ':');
                    if (colon) {
                        *colon = '\0';
                        port = atoi(colon + 1);
                    }
                    addr.sin_port = htons(port);
                    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
                        struct hostent *he = gethostbyname(host);
                        if (!he) {
                            fprintf(stderr, "Error: cannot resolve host '%s'\n", host);
                            free(files_dup); return 1;
                        }
                        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
                    }
                }
            }

            /* Scan for {RAND:min:max} or {SEQ:start} placeholder.
             * Modifies raw in-place (may realloc) and populates .ph */
            placeholder_t ph;
            if (scan_placeholder(&raw, &raw_len, &ph) != 0) {
                free(raw); free(files_dup); return 1;
            }

            build_pipeline_from_raw(raw, raw_len, pipeline_depth, &templates[num_templates]);
            templates[num_templates].ph = ph;
            num_templates++;
            free(raw);
            tok = strtok_r(NULL, ",", &saveptr);
        }
        free(files_dup);

        /* strtok_r skips empty fields, so --raw "" and --raw ",,," both get
           here with nothing loaded. num_templates is a divisor in
           start_connect's template assignment, so zero is a SIGFPE. */
        if (num_templates == 0) {
            fprintf(stderr, "Error: --raw listed no template files\n");
            free(templates);
            return 1;
        }
    } else {
        /* Build GET request from URL */
        templates = calloc(1, sizeof(request_tpl_t));
        if (!templates) { fprintf(stderr, "Error: out of memory\n"); return 1; }
        num_templates = 1;

        char *single_buf;
        int request_len = http_build_pipeline(host, port, path, 1, &single_buf);
        free(single_buf);

        char *pipeline_buf;
        http_build_pipeline(host, port, path, pipeline_depth, &pipeline_buf);

        templates[0].pipeline_buf = pipeline_buf;
        templates[0].request_len  = request_len;
        templates[0].pipeline_len = request_len * pipeline_depth;
    }

    /* --addr-span: consecutive IPv4 addresses from the target's. One source
       address holds at most ~64K connections to one destination address and
       port; every 127.x.y.z is local, so a server listening on 0.0.0.0 takes
       a larger fleet spread over 127.0.0.1, .2, ... */
    const uint32_t base_ip = ntohl(addr.sin_addr.s_addr);
    if ((uint64_t)base_ip + (uint64_t)(addr_span - 1) > 0xFFFFFFFFULL) {
        fprintf(stderr, "Error: --addr-span %d runs past 255.255.255.255\n", addr_span);
        return 1;
    }
    struct sockaddr_in *addrs = calloc(addr_span, sizeof(*addrs));
    if (!addrs) { fprintf(stderr, "Error: out of memory\n"); return 1; }
    for (int i = 0; i < addr_span; i++) {
        addrs[i] = addr;
        addrs[i].sin_addr.s_addr = htonl(base_ip + (uint32_t)i);
    }

    /* --rate: each connection sends once per interval, so together they send
       at the rate asked for */
    uint64_t interval_ns = 0;
    if (rate > 0) {
        interval_ns = (uint64_t)((double)num_connections * 1e9 / rate);
        if (interval_ns == 0) interval_ns = 1;
    }

    int conns_per_thread = num_connections / num_threads;
    int extra = num_connections % num_threads;

    if (!json_mode) {
        printf("gcannon v%s", GCANNON_VERSION);
        if (ws_mode || tui_mode || cqe_latency || per_tpl_latency) {
            printf(" [");
            int first = 1;
            if (ws_mode)        { printf("WS");             first = 0; }
            if (tui_mode)       { printf("%sTUI",   first ? "" : ", "); first = 0; }
            if (cqe_latency)    { printf("%sCQE",   first ? "" : ", "); first = 0; }
            if (per_tpl_latency){ printf("%sTPL",   first ? "" : ", "); first = 0; }
            printf("]");
        }
        printf("\n");
        printf("  Target:    %s:%d%s\n", host, port, path);
        if (tls)
            printf("  TLS:       1.3 via kernel TLS (certificate not verified)\n");
        printf("  Threads:   %d\n", num_threads);
        printf("  Conns:     %s%d (%.0f/thread)\n", ramp.p99_us ? "up to " : "",
               num_connections, (double)num_connections / num_threads);
        printf("  Pipeline:  %d\n", pipeline_depth);
        if (requests_per_conn > 0)
            printf("  Req/conn:  %d\n", requests_per_conn);
        else
            printf("  Req/conn:  unlimited (keep-alive)\n");
        if (num_templates > 1)
            printf("  Templates: %d\n", num_templates);
        printf("  Expected:  %d\n", expected_status);
        printf("  Duration:  %ds%s\n", duration_sec, ramp.p99_us ? " per step" : "");
        if (addr_span > 1) {
            char first[INET_ADDRSTRLEN], last[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addrs[0].sin_addr, first, sizeof(first));
            inet_ntop(AF_INET, &addrs[addr_span - 1].sin_addr, last, sizeof(last));
            printf("  Addresses: %s - %s (%d)\n", first, last, addr_span);
        }
        if (rate > 0)
            printf("  Rate:      %.0f req/s, one request per connection every %.3fs\n",
                   rate, interval_ns / 1e9);
        if (ramp.p99_us)
            ramp_print_plan(ramp.start, ramp.factor, ramp.increment, ramp.p99_us);
        printf("\n");
    }

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Spawn worker threads */
    thread_ctx_t *ctxs = calloc(num_threads, sizeof(thread_ctx_t));
    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
    if (!ctxs || !threads) {
        fprintf(stderr, "Error: out of memory\n");
        return 1;
    }

    /* --rate: the shared clock every connection's schedule is laid on */
    struct timespec epoch_ts;
    clock_gettime(CLOCK_MONOTONIC, &epoch_ts);
    const uint64_t epoch_ns = (uint64_t)epoch_ts.tv_sec * 1000000000ULL
                            + (uint64_t)epoch_ts.tv_nsec;

    int conn_offset = 0;
    for (int i = 0; i < num_threads; i++) {
        ctxs[i].worker.id        = i;
        ctxs[i].worker.conn_offset = conn_offset;
        ctxs[i].templates        = templates;
        ctxs[i].num_templates    = num_templates;
        ctxs[i].pipeline_depth   = pipeline_depth;
        ctxs[i].num_conns        = conns_per_thread + (i < extra ? 1 : 0);
        ctxs[i].requests_per_conn = requests_per_conn;
        ctxs[i].expected_status   = expected_status;
        ctxs[i].ws_mode           = ws_mode;
        ctxs[i].recv_buf_size    = recv_buf_size;
        ctxs[i].cqe_latency      = cqe_latency;
        ctxs[i].per_tpl_latency  = per_tpl_latency;
        ctxs[i].tls              = tls;
        ctxs[i].tls_sni          = tls_sni;
        ctxs[i].ws_host           = host;
        ctxs[i].ws_port           = port;
        ctxs[i].ws_path           = path;
        ctxs[i].ws_payload        = (const uint8_t *)ws_message;
        ctxs[i].ws_payload_len    = strlen(ws_message);
        ctxs[i].addrs             = addrs;
        ctxs[i].num_addrs         = addr_span;
        ctxs[i].interval_ns       = interval_ns;
        ctxs[i].epoch_ns          = epoch_ns;
        ctxs[i].total_conns       = num_connections;
        ctxs[i].connect_limit     = connect_limit;
        ctxs[i].conn_target       = ramp.p99_us ? ramp_share(ramp.start, num_threads, i)
                                                : ctxs[i].num_conns;
        ctxs[i].ramp_p99_us       = ramp.p99_us;
        conn_offset += ctxs[i].num_conns;
        pthread_create(&threads[i], NULL, worker_thread, &ctxs[i]);
    }

    /* What the results describe: the window, or with --ramp-p99 one step's
       window, and how many connections it had */
    worker_stats_t total = {0};
    if (per_tpl_latency && num_templates > 1) {
        total.tpl_latency = calloc(num_templates, sizeof(latency_hist_t));
        total.num_tpl_latency = num_templates;
    }
    double elapsed = 0;
    int report_conns = num_connections;
    uint64_t *rps_history = NULL;
    ramp_result_t ramp_res = {0};

    if (ramp.p99_us) {
        /* Each window's merged stats land in total or alt, in turns */
        worker_stats_t *alt = calloc(1, sizeof(*alt));
        if (alt && total.tpl_latency) {
            alt->tpl_latency = calloc(num_templates, sizeof(latency_hist_t));
            alt->num_tpl_latency = num_templates;
        }
        if (!alt || (total.tpl_latency && !alt->tpl_latency)) {
            fprintf(stderr, "Error: out of memory\n");
            return 1;
        }
        /* The step table goes to stdout, or to stderr beside --json's */
        const worker_stats_t *shown = run_ramp(ctxs, num_threads, &ramp,
                                               json_mode ? stderr : stdout,
                                               &total, alt, &ramp_res);
        if (shown && shown != &total)
            stats_copy(&total, shown);
        free(alt->tpl_latency);
        free(alt);

        g_running = 0;
        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
            worker_destroy(&ctxs[i].worker);
        }
        if (!shown) {
            fprintf(stderr, "Interrupted before the first step was measured; "
                            "nothing to report\n");
            free(total.tpl_latency);
            for (int i = 0; i < num_templates; i++)
                free(templates[i].pipeline_buf);
            free(templates);
            free(ctxs);
            free(threads);
            return 130;
        }
        /* The results are the last step that passed, or else the last one */
        const ramp_step_t *st = &ramp_res.steps[ramp_res.best >= 0 ? ramp_res.best
                                                                     : ramp_res.num_steps - 1];
        elapsed = st->elapsed_sec;
        report_conns = st->connections;
        goto report;
    }

    /* Warmup: until every connection is set up (wait_settled) */
    {
        const int settled = wait_settled(ctxs, num_threads, num_connections, warmup_max_sec);
        if (settled < num_connections && g_running)
            fprintf(stderr, "Warning: %d of %d connections still not set up after "
                            "%ds of warmup; measuring anyway\n",
                    num_connections - settled, num_connections, warmup_max_sec);
    }

    /* --settle: keep the load running this much longer once every connection
     * is up, and only then open the window. A fleet of millions takes a
     * server a while to absorb - accepts still queued, a first garbage
     * collection after allocating for all of them - and without a settle the
     * window opens on that backlog and describes the tail of the connect
     * phase rather than the steady state. Ctrl-C ends it like the warmup. */
    if (settle_sec > 0 && g_running) {
        if (rate > 0)
            fprintf(stderr, "gcannon: all connections up, settling for %ds\n", settle_sec);
        sleep_running(settle_sec * 1000L);
    }

    /* Reset counters after warmup. Each worker zeroes its own stats from its
     * own thread; memset-ing them here would race with the workers still
     * incrementing those same counters (a data race / undefined behavior).
     * The clock starts once every worker has. */
    ctl_all(ctxs, num_threads, CTL_RESET);

    if (!g_running) {
        /* Ctrl-C before the clock started. Nothing was measured, so there is
         * no result to print (elapsed would be ~0 and the rate absurd) and
         * none to add to the run history. */
        for (int i = 0; i < num_threads; i++) {
            pthread_join(threads[i], NULL);
            worker_destroy(&ctxs[i].worker);
        }
        fprintf(stderr, "Interrupted during warmup; nothing was measured\n");
        free(total.tpl_latency);
        for (int i = 0; i < num_templates; i++)
            free(templates[i].pipeline_buf);
        free(templates);
        free(ctxs);
        free(threads);
        return 130;   /* 128 + SIGINT, as a shell reports an interrupted command */
    }

    /* --rate: a line a wrapper can wait on to sample anything else (a
       server's CPU, say) over exactly the measured window */
    if (rate > 0)
        fprintf(stderr, "gcannon: warmup done, measuring for %ds\n", duration_sec);

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* Wait for duration (with optional TUI progress bar) */
    uint64_t prev_responses = 0;
    int ticks = 0;
    if (tui_mode) {
        rps_history = calloc(duration_sec > 0 ? duration_sec : 1, sizeof(uint64_t));
        tui_progress_init(duration_sec);
    }

    while (g_running && ticks < duration_sec) {
        nanosleep(&(struct timespec){ .tv_sec = 1, .tv_nsec = 0 }, NULL);
        ticks++;

        if (tui_mode) {
            uint64_t snap_resp = 0, snap_bytes = 0;
            for (int i = 0; i < num_threads; i++) {
                snap_resp  += ctxs[i].worker.stats.responses;
                snap_bytes += ctxs[i].worker.stats.bytes_read;
            }
            rps_history[ticks - 1] = snap_resp - prev_responses;
            tui_progress_update(ticks, duration_sec,
                                snap_resp, snap_bytes, prev_responses,
                                rps_history, ticks);
            prev_responses = snap_resp;
        }
    }
    /* --rate: the window is over but every connection is still open; what a
       wrapper samples now excludes the teardown (a million closes) below */
    if (rate > 0)
        fprintf(stderr, "gcannon: measurement done\n");
    g_running = 0;

    /* Join threads */
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    /* The window ends when the last worker stopped counting, not at the join:
       each worker closes its own connections before it returns, and that
       teardown is no part of the measurement */
    struct timespec end_time = ctxs[0].worker.stopped_at;
    for (int i = 1; i < num_threads; i++) {
        const struct timespec *t = &ctxs[i].worker.stopped_at;
        if (t->tv_sec > end_time.tv_sec ||
            (t->tv_sec == end_time.tv_sec && t->tv_nsec > end_time.tv_nsec))
            end_time = *t;
    }
    if (end_time.tv_sec < start_time.tv_sec ||
        (end_time.tv_sec == start_time.tv_sec && end_time.tv_nsec < start_time.tv_nsec))
        clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed = (end_time.tv_sec - start_time.tv_sec)
            + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    /* Aggregate stats */
    for (int i = 0; i < num_threads; i++) {
        stats_merge(&total, &ctxs[i].worker.stats);
        worker_destroy(&ctxs[i].worker);
    }

report:
    /* Save run history */
    history_file_t *history = calloc(1, sizeof(history_file_t));
    run_record_t current_record = {0};
    if (history) history_load(history);

    int num_prev = history ? history->count : 0;
    run_record_t *prev_runs = NULL;
    if (num_prev > 0) {
        prev_runs = malloc(num_prev * sizeof(run_record_t));
        memcpy(prev_runs, history->runs, num_prev * sizeof(run_record_t));
    }

    history_build_record(&current_record, &total, elapsed,
                         host, port, path,
                         report_conns, num_threads,
                         pipeline_depth, duration_sec);
    if (history) history_save(history, &current_record);

    if (json_mode) {
        char target[512];
        if (port == 80)
            snprintf(target, sizeof(target), "%s%s", host, path);
        else
            snprintf(target, sizeof(target), "%s:%d%s", host, port, path);
        stats_print_json(&total, elapsed, num_templates, ws_mode,
                         target, report_conns, num_threads,
                         pipeline_depth, duration_sec, rate,
                         ramp.p99_us ? &ramp_res : NULL);
    } else if (tui_mode) {
        tui_print_results(&total, elapsed, num_templates, expected_status,
                          hist_buckets, prev_runs, num_prev, &current_record,
                          ws_mode);
        if (rate > 0) stats_print_rate(&total, elapsed, rate, num_connections);
    } else {
        stats_print(&total, elapsed, num_templates, ws_mode);
        if (rate > 0) stats_print_rate(&total, elapsed, rate, num_connections);
        if (ramp.p99_us) ramp_print_result(&ramp_res);
    }

    /* Check for unexpected status codes (exit code) */
    const uint64_t unexpected = unexpected_responses(&total, expected_status);
    int exit_code = 0;
    if (unexpected > 0 && total.responses > 0) {
        if (!tui_mode && !json_mode)
            printf("\n  WARNING: %lu/%lu responses (%.1f%%) had unexpected status (expected %dxx)\n",
                   unexpected, total.responses,
                   100.0 * unexpected / total.responses, expected_status / 100);
        exit_code = 1;
    }
    /* --ramp-p99: no step kept p99 within the threshold */
    if (ramp.p99_us && ramp_res.best < 0)
        exit_code = 1;

    free(ramp_res.steps);
    free(total.tpl_latency);
    free(history);
    free(prev_runs);
    for (int i = 0; i < num_templates; i++)
        free(templates[i].pipeline_buf);
    free(templates);
    free(ctxs);
    free(threads);
    free(rps_history);
    free(addrs);
    return exit_code;
}
