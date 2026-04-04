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
#include <time.h>

#include "constants.h"
#include "worker.h"
#include "http.h"
#include "stats.h"
#include "tui.h"

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
    int              cqe_latency;
    int              per_tpl_latency;
    const char      *ws_host;
    int              ws_port;
    const char      *ws_path;
    const uint8_t   *ws_payload;
    int              ws_payload_len;
    struct sockaddr_in addr;
} thread_ctx_t;

/* ── URL parser ────────────────────────────────────────────────────── */

static int parse_url(const char *url, char *host, int host_sz,
                     int *port, char *path, int path_sz)
{
    if (strncmp(url, "http://", 7) != 0) {
        fprintf(stderr, "Error: only http:// URLs supported\n");
        return -1;
    }
    const char *hp = url + 7;
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
        *port = 80;
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
    worker_init(&ctx->worker, ctx->worker.id, &ctx->addr,
                ctx->templates, ctx->num_templates, ctx->pipeline_depth,
                ctx->num_conns, ctx->worker.conn_offset,
                ctx->requests_per_conn, ctx->expected_status,
                ctx->ws_mode, ctx->ws_host, ctx->ws_port, ctx->ws_path,
                ctx->ws_payload, ctx->ws_payload_len,
                ctx->cqe_latency, ctx->per_tpl_latency,
                &g_running);
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
    int num_connections = 100;
    int num_threads = 1;
    int duration_sec = 10;
    int pipeline_depth = 1;
    int requests_per_conn = 0; /* 0 = keep-alive forever */
    int expected_status = 200; /* expected HTTP status code */
    int ws_mode = 0;
    int tui_mode = 0;
    int cqe_latency = 0;
    int per_tpl_latency = 0;
    int hist_buckets = 0; /* 0 = default (10) */
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
        } else if ((strcmp(argv[i], "--buckets") == 0 || strcmp(argv[i], "-b") == 0) && i + 1 < argc) {
            hist_buckets = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cqe-latency") == 0) {
            cqe_latency = 1;
        } else if (strcmp(argv[i], "--per-tpl-latency") == 0) {
            per_tpl_latency = 1;
        } else {
            fprintf(stderr, "Usage: gcannon <url> -c <conns> -t <threads> "
                            "-d <duration> [-p <pipeline>] [-r <req/conn>] "
                            "[-s <status>] [-R|--raw file1,file2,...] "
                            "[--ws [--ws-msg <message>]] [--tui] [-b <buckets>] [--cqe-latency]\n");
            return 1;
        }
    }

    if (!url && !raw_files) {
        fprintf(stderr, "Usage: gcannon <url> -c <conns> -t <threads> "
                        "-d <duration> [-p <pipeline>] [-r <req/conn>] "
                        "[-R|--raw file1,file2,...] [--tui]\n");
        return 1;
    }

    if (pipeline_depth > PIPELINE_DEPTH_MAX)
        pipeline_depth = PIPELINE_DEPTH_MAX;

    /* Resolve target host */
    char host[256] = {0}, path[1024] = "/";
    int port = 80;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;

    if (url) {
        if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0)
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

    /* Build request templates */
    request_tpl_t *templates = NULL;
    int num_templates = 0;

    if (raw_files) {
        /* Count commas to get number of files */
        int count = 1;
        for (const char *p = raw_files; *p; p++)
            if (*p == ',') count++;

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

            build_pipeline_from_raw(raw, raw_len, pipeline_depth, &templates[num_templates]);
            num_templates++;
            free(raw);
            tok = strtok_r(NULL, ",", &saveptr);
        }
        free(files_dup);
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

    int conns_per_thread = num_connections / num_threads;
    int extra = num_connections % num_threads;

    printf("gcannon — io_uring %s load generator\n", ws_mode ? "WebSocket" : "HTTP");
    printf("  Target:    %s:%d%s\n", host, port, path);
    printf("  Threads:   %d\n", num_threads);
    printf("  Conns:     %d (%.0f/thread)\n", num_connections,
           (double)num_connections / num_threads);
    printf("  Pipeline:  %d\n", pipeline_depth);
    if (requests_per_conn > 0)
        printf("  Req/conn:  %d\n", requests_per_conn);
    else
        printf("  Req/conn:  unlimited (keep-alive)\n");
    if (num_templates > 1)
        printf("  Templates: %d\n", num_templates);
    printf("  Expected:  %d\n", expected_status);
    printf("  Duration:  %ds\n", duration_sec);
    printf("\n");

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Spawn worker threads */
    thread_ctx_t *ctxs = calloc(num_threads, sizeof(thread_ctx_t));
    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));
    if (!ctxs || !threads) {
        fprintf(stderr, "Error: out of memory\n");
        return 1;
    }

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
        ctxs[i].cqe_latency      = cqe_latency;
        ctxs[i].per_tpl_latency  = per_tpl_latency;
        ctxs[i].ws_host           = host;
        ctxs[i].ws_port           = port;
        ctxs[i].ws_path           = path;
        ctxs[i].ws_payload        = (const uint8_t *)ws_message;
        ctxs[i].ws_payload_len    = strlen(ws_message);
        ctxs[i].addr              = addr;
        conn_offset += ctxs[i].num_conns;
        pthread_create(&threads[i], NULL, worker_thread, &ctxs[i]);
    }

    /* Warmup — let connections establish */
    nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 100000000 }, NULL); /* 100ms */

    /* Reset counters after warmup (including latency histogram) */
    for (int i = 0; i < num_threads; i++) {
        latency_hist_t *saved_tpl = ctxs[i].worker.stats.tpl_latency;
        int saved_n = ctxs[i].worker.stats.num_tpl_latency;
        memset(&ctxs[i].worker.stats, 0, sizeof(worker_stats_t));
        if (saved_tpl) {
            memset(saved_tpl, 0, saved_n * sizeof(latency_hist_t));
            ctxs[i].worker.stats.tpl_latency = saved_tpl;
            ctxs[i].worker.stats.num_tpl_latency = saved_n;
        }
    }

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* Wait for duration (with optional TUI progress bar) */
    uint64_t prev_responses = 0;
    int ticks = 0;
    uint64_t *rps_history = NULL;
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
    g_running = 0;

    /* Join threads */
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed = (end_time.tv_sec - start_time.tv_sec)
                   + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    /* Aggregate stats */
    worker_stats_t total = {0};
    if (per_tpl_latency && num_templates > 1) {
        total.tpl_latency = calloc(num_templates, sizeof(latency_hist_t));
        total.num_tpl_latency = num_templates;
    }
    for (int i = 0; i < num_threads; i++) {
        stats_merge(&total, &ctxs[i].worker.stats);
        worker_destroy(&ctxs[i].worker);
    }

    if (tui_mode) {
        tui_print_results(&total, elapsed, num_templates, expected_status, hist_buckets);
    } else {
        stats_print(&total, elapsed, num_templates);
    }

    /* Check for unexpected status codes (exit code) */
    uint64_t expected_count = 0;
    if (expected_status >= 200 && expected_status < 300)      expected_count = total.status_2xx;
    else if (expected_status >= 300 && expected_status < 400)  expected_count = total.status_3xx;
    else if (expected_status >= 400 && expected_status < 500)  expected_count = total.status_4xx;
    else if (expected_status >= 500 && expected_status < 600)  expected_count = total.status_5xx;

    uint64_t unexpected = total.responses - expected_count;
    int exit_code = 0;
    if (unexpected > 0 && total.responses > 0) {
        if (!tui_mode)
            printf("\n  WARNING: %lu/%lu responses (%.1f%%) had unexpected status (expected %dxx)\n",
                   unexpected, total.responses,
                   100.0 * unexpected / total.responses, expected_status / 100);
        exit_code = 1;
    }

    free(total.tpl_latency);
    for (int i = 0; i < num_templates; i++)
        free(templates[i].pipeline_buf);
    free(templates);
    free(ctxs);
    free(threads);
    free(rps_history);
    return exit_code;
}
