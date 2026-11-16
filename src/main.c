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
#include <time.h>

#include "cli.h"
#include "core/constants.h"
#include "core/stats.h"
#include "core/worker.h"
#include "proto/template.h"
#include "output/history.h"
#include "output/report.h"
#include "output/tui.h"

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
} thread_ctx_t;

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
                ctx->connect_limit, &g_running);
    worker_loop(&ctx->worker);
    return NULL;
}

/* ── signal handler ────────────────────────────────────────────────── */

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    options_t o;
    switch (cli_parse(argc, argv, &o)) {
    case CLI_RUN:
        break;
    case CLI_VERSION:
        printf("gcannon %s\n", GCANNON_VERSION);
        return 0;
    case CLI_CLEAR_HISTORY:
        history_clear();
        printf("Run history cleared.\n");
        return 0;
    default:
        return 1;
    }

    /* Resolve target host */
    char host[256] = {0}, path[1024] = "/";
    int port = 80;
    int tls = 0;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;

    if (o.url) {
        if (cli_parse_url(o.url, host, sizeof(host), &port, path, sizeof(path), &tls) < 0)
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

    /* Build request templates (proto/template.c). Without a URL the target
       comes from the first --raw file's Host header. */
    request_tpl_t *templates = NULL;
    int num_templates = 0;
    if (templates_build(o.raw_files, o.url != NULL, o.pipeline_depth,
                        host, sizeof(host), &port, path, &addr,
                        &templates, &num_templates) < 0)
        return 1;

    /* --addr-span: consecutive IPv4 addresses from the target's. One source
       address holds at most ~64K connections to one destination address and
       port; every 127.x.y.z is local, so a server listening on 0.0.0.0 takes
       a larger fleet spread over 127.0.0.1, .2, ... */
    const uint32_t base_ip = ntohl(addr.sin_addr.s_addr);
    if ((uint64_t)base_ip + (uint64_t)(o.addr_span - 1) > 0xFFFFFFFFULL) {
        fprintf(stderr, "Error: --addr-span %d runs past 255.255.255.255\n", o.addr_span);
        return 1;
    }
    struct sockaddr_in *addrs = calloc(o.addr_span, sizeof(*addrs));
    if (!addrs) { fprintf(stderr, "Error: out of memory\n"); return 1; }
    for (int i = 0; i < o.addr_span; i++) {
        addrs[i] = addr;
        addrs[i].sin_addr.s_addr = htonl(base_ip + (uint32_t)i);
    }

    /* --rate: each connection sends once per interval, so together they send
       at the rate asked for */
    uint64_t interval_ns = 0;
    if (o.rate > 0) {
        interval_ns = (uint64_t)((double)o.num_connections * 1e9 / o.rate);
        if (interval_ns == 0) interval_ns = 1;
    }

    int conns_per_thread = o.num_connections / o.num_threads;
    int extra = o.num_connections % o.num_threads;

    if (!o.json_mode)
        report_banner(&o, host, port, path, tls, num_templates, addrs, interval_ns);

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Spawn worker threads */
    thread_ctx_t *ctxs = calloc(o.num_threads, sizeof(thread_ctx_t));
    pthread_t *threads = calloc(o.num_threads, sizeof(pthread_t));
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
    for (int i = 0; i < o.num_threads; i++) {
        ctxs[i].worker.id        = i;
        ctxs[i].worker.conn_offset = conn_offset;
        ctxs[i].templates        = templates;
        ctxs[i].num_templates    = num_templates;
        ctxs[i].pipeline_depth   = o.pipeline_depth;
        ctxs[i].num_conns        = conns_per_thread + (i < extra ? 1 : 0);
        ctxs[i].requests_per_conn = o.requests_per_conn;
        ctxs[i].expected_status   = o.expected_status;
        ctxs[i].ws_mode           = o.ws_mode;
        ctxs[i].recv_buf_size    = o.recv_buf_size;
        ctxs[i].cqe_latency      = o.cqe_latency;
        ctxs[i].per_tpl_latency  = o.per_tpl_latency;
        ctxs[i].tls              = tls;
        ctxs[i].tls_sni          = tls_sni;
        ctxs[i].ws_host           = host;
        ctxs[i].ws_port           = port;
        ctxs[i].ws_path           = path;
        ctxs[i].ws_payload        = (const uint8_t *)o.ws_message;
        ctxs[i].ws_payload_len    = strlen(o.ws_message);
        ctxs[i].addrs             = addrs;
        ctxs[i].num_addrs         = o.addr_span;
        ctxs[i].interval_ns       = interval_ns;
        ctxs[i].epoch_ns          = epoch_ns;
        ctxs[i].total_conns       = o.num_connections;
        ctxs[i].connect_limit     = o.connect_limit;
        conn_offset += ctxs[i].num_conns;
        pthread_create(&threads[i], NULL, worker_thread, &ctxs[i]);
    }

    /* Warmup — at least WARMUP_MIN_MS, then until every connection has
     * settled (became ready, or failed once and is retrying). A fixed sleep
     * let slow setup — a WebSocket upgrade the server answers late — run into
     * the measured window, where those connections sat idle and dragged the
     * rate down. Capped so a server that never answers can't stall the run. */
    nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = WARMUP_MIN_MS * 1000000L }, NULL);
    {
        struct timespec warm_start, now;
        clock_gettime(CLOCK_MONOTONIC, &warm_start);
        for (;;) {
            int settled = 0;
            for (int i = 0; i < o.num_threads; i++)
                settled += atomic_load_explicit(&ctxs[i].worker.conns_settled,
                                                memory_order_relaxed);
            if (settled >= o.num_connections || !g_running) break;

            clock_gettime(CLOCK_MONOTONIC, &now);
            const long waited_ms = (now.tv_sec - warm_start.tv_sec) * 1000L
                                 + (now.tv_nsec - warm_start.tv_nsec) / 1000000L;
            if (waited_ms >= (long)o.warmup_max_sec * 1000L - WARMUP_MIN_MS) {
                fprintf(stderr, "Warning: %d of %d connections still not set up after "
                                "%ds of warmup; measuring anyway\n",
                        o.num_connections - settled, o.num_connections,
                        o.warmup_max_sec);
                break;
            }
            nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 1000000 }, NULL); /* 1ms */
        }
    }

    /* Reset counters after warmup. Signal each worker to zero its own stats
     * from its own thread; memset-ing them here would race with the workers
     * still incrementing those same counters (a data race / undefined behavior). */
    for (int i = 0; i < o.num_threads; i++)
        ctxs[i].worker.reset_request = 1;
    /* Wait until every worker has acknowledged before we start the clock.
     * A worker that saw Ctrl-C has left its loop without acknowledging, so
     * stop waiting once g_running drops; otherwise this waits forever. */
    for (int i = 0; i < o.num_threads; i++)
        while (ctxs[i].worker.reset_request && g_running)
            nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 100000 }, NULL); /* 100us */

    if (!g_running) {
        /* Ctrl-C before the clock started. Nothing was measured, so there is
         * no result to print (elapsed would be ~0 and the rate absurd) and
         * none to add to the run history. */
        for (int i = 0; i < o.num_threads; i++) {
            pthread_join(threads[i], NULL);
            worker_destroy(&ctxs[i].worker);
        }
        fprintf(stderr, "Interrupted during warmup; nothing was measured\n");
        templates_free(templates, num_templates);
        free(ctxs);
        free(threads);
        return 130;   /* 128 + SIGINT, as a shell reports an interrupted command */
    }

    /* --rate: a line a wrapper can wait on to sample anything else (a
       server's CPU, say) over exactly the measured window */
    if (o.rate > 0)
        fprintf(stderr, "gcannon: warmup done, measuring for %ds\n", o.duration_sec);

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* Wait for duration (with optional TUI progress bar) */
    uint64_t prev_responses = 0;
    int ticks = 0;
    uint64_t *rps_history = NULL;
    if (o.tui_mode) {
        rps_history = calloc(o.duration_sec > 0 ? o.duration_sec : 1, sizeof(uint64_t));
        tui_progress_init(o.duration_sec);
    }

    while (g_running && ticks < o.duration_sec) {
        nanosleep(&(struct timespec){ .tv_sec = 1, .tv_nsec = 0 }, NULL);
        ticks++;

        if (o.tui_mode) {
            uint64_t snap_resp = 0, snap_bytes = 0;
            for (int i = 0; i < o.num_threads; i++) {
                snap_resp  += ctxs[i].worker.stats.responses;
                snap_bytes += ctxs[i].worker.stats.bytes_read;
            }
            rps_history[ticks - 1] = snap_resp - prev_responses;
            tui_progress_update(ticks, o.duration_sec,
                                snap_resp, snap_bytes, prev_responses,
                                rps_history, ticks);
            prev_responses = snap_resp;
        }
    }
    /* --rate: the window is over but every connection is still open; what a
       wrapper samples now excludes the teardown (a million closes) below */
    if (o.rate > 0)
        fprintf(stderr, "gcannon: measurement done\n");
    g_running = 0;

    /* Join threads */
    for (int i = 0; i < o.num_threads; i++)
        pthread_join(threads[i], NULL);

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec)
                   + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    /* Aggregate stats */
    worker_stats_t total = {0};
    if (o.per_tpl_latency && num_templates > 1) {
        total.tpl_latency = calloc(num_templates, sizeof(latency_hist_t));
        total.num_tpl_latency = num_templates;
    }
    for (int i = 0; i < o.num_threads; i++) {
        stats_merge(&total, &ctxs[i].worker.stats);
        worker_destroy(&ctxs[i].worker);
    }

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
                         o.num_connections, o.num_threads,
                         o.pipeline_depth, o.duration_sec);
    if (history) history_save(history, &current_record);

    if (o.json_mode) {
        char target[512];
        if (port == 80)
            snprintf(target, sizeof(target), "%s%s", host, path);
        else
            snprintf(target, sizeof(target), "%s:%d%s", host, port, path);
        report_json(&total, elapsed, num_templates, o.ws_mode,
                    target, o.num_connections, o.num_threads,
                    o.pipeline_depth, o.duration_sec, o.rate);
    } else if (o.tui_mode) {
        tui_print_results(&total, elapsed, num_templates, o.expected_status,
                          o.hist_buckets, prev_runs, num_prev, &current_record,
                          o.ws_mode);
        if (o.rate > 0) report_rate(&total, elapsed, o.rate, o.num_connections);
    } else {
        report_text(&total, elapsed, num_templates, o.ws_mode);
        if (o.rate > 0) report_rate(&total, elapsed, o.rate, o.num_connections);
    }

    /* Check for unexpected status codes (exit code) */
    uint64_t expected_count = 0;
    if (o.expected_status >= 200 && o.expected_status < 300)      expected_count = total.status_2xx;
    else if (o.expected_status >= 300 && o.expected_status < 400)  expected_count = total.status_3xx;
    else if (o.expected_status >= 400 && o.expected_status < 500)  expected_count = total.status_4xx;
    else if (o.expected_status >= 500 && o.expected_status < 600)  expected_count = total.status_5xx;

    /* Saturating: expected_count is a separate counter, so any accounting bug
       that pushes it above `responses` would otherwise wrap this to ~2^64 and
       print a nonsense warning instead of a small, believable one. */
    uint64_t unexpected = total.responses > expected_count
                        ? total.responses - expected_count : 0;
    int exit_code = 0;
    if (unexpected > 0 && total.responses > 0) {
        if (!o.tui_mode && !o.json_mode)
            printf("\n  WARNING: %lu/%lu responses (%.1f%%) had unexpected status (expected %dxx)\n",
                   unexpected, total.responses,
                   100.0 * unexpected / total.responses, o.expected_status / 100);
        exit_code = 1;
    }

    free(total.tpl_latency);
    free(history);
    free(prev_runs);
    templates_free(templates, num_templates);
    free(ctxs);
    free(threads);
    free(rps_history);
    free(addrs);
    return exit_code;
}
