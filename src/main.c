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

/* ── globals ───────────────────────────────────────────────────────── */

static volatile int g_running = 1;

typedef struct thread_ctx {
    worker_t   worker;
    char      *pipeline_buf;
    int        request_len;
    int        pipeline_depth;
    int        num_conns;
    int        cpu_id;
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
    int val = atoi(s);
    int len = strlen(s);
    if (len > 0 && (s[len-1] == 's' || s[len-1] == 'S'))
        return val;
    if (len > 0 && (s[len-1] == 'm' || s[len-1] == 'M'))
        return val * 60;
    return val;
}

/* ── worker thread entry ───────────────────────────────────────────── */

static void *worker_thread(void *arg)
{
    thread_ctx_t *ctx = arg;
    worker_init(&ctx->worker, ctx->worker.id, &ctx->addr,
                ctx->pipeline_buf, ctx->request_len, ctx->pipeline_depth,
                ctx->num_conns, &g_running);
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
    int pipeline_depth = 16;
    const char *url = NULL;

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
        } else {
            fprintf(stderr, "Usage: gcannon <url> -c <conns> -t <threads> "
                            "-d <duration> [-p <pipeline>]\n");
            return 1;
        }
    }

    if (!url) {
        fprintf(stderr, "Usage: gcannon <url> -c <conns> -t <threads> "
                        "-d <duration> [-p <pipeline>]\n");
        return 1;
    }

    if (pipeline_depth > PIPELINE_DEPTH_MAX)
        pipeline_depth = PIPELINE_DEPTH_MAX;

    /* Parse URL */
    char host[256], path[1024];
    int port;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0)
        return 1;

    /* Resolve host */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            fprintf(stderr, "Error: cannot resolve host '%s'\n", host);
            return 1;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    /* Build HTTP pipeline buffer */
    char *pipeline_buf;
    int request_len = http_build_pipeline(host, port, path, 1, &pipeline_buf);
    free(pipeline_buf);
    /* Build the full pipeline */
    int pipeline_total_len = http_build_pipeline(host, port, path,
                                                  pipeline_depth, &pipeline_buf);
    (void)pipeline_total_len;

    int conns_per_thread = num_connections / num_threads;
    int extra = num_connections % num_threads;

    printf("gcannon — io_uring HTTP load generator\n");
    printf("  Target:    %s:%d%s\n", host, port, path);
    printf("  Threads:   %d\n", num_threads);
    printf("  Conns:     %d (%.0f/thread)\n", num_connections,
           (double)num_connections / num_threads);
    printf("  Pipeline:  %d\n", pipeline_depth);
    printf("  Duration:  %ds\n", duration_sec);
    printf("\n");

    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Spawn worker threads */
    thread_ctx_t *ctxs = calloc(num_threads, sizeof(thread_ctx_t));
    pthread_t *threads = calloc(num_threads, sizeof(pthread_t));

    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);

    for (int i = 0; i < num_threads; i++) {
        ctxs[i].worker.id      = i;
        ctxs[i].pipeline_buf   = pipeline_buf;
        ctxs[i].request_len    = request_len;
        ctxs[i].pipeline_depth = pipeline_depth;
        ctxs[i].num_conns      = conns_per_thread + (i < extra ? 1 : 0);
        ctxs[i].cpu_id         = i % num_cpus;
        ctxs[i].addr           = addr;
        pthread_create(&threads[i], NULL, worker_thread, &ctxs[i]);
    }

    /* Warmup — let connections establish */
    nanosleep(&(struct timespec){ .tv_sec = 0, .tv_nsec = 100000000 }, NULL); /* 100ms */

    /* Reset stats after warmup so connect phase doesn't pollute results */
    for (int i = 0; i < num_threads; i++)
        memset(&ctxs[i].worker.stats, 0, sizeof(worker_stats_t));

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* Wait for duration */
    struct timespec sleep_ts = { .tv_sec = duration_sec, .tv_nsec = 0 };
    while (g_running && sleep_ts.tv_sec > 0) {
        nanosleep(&(struct timespec){ .tv_sec = 1, .tv_nsec = 0 }, NULL);
        sleep_ts.tv_sec--;
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
    worker_stats_t total;
    memset(&total, 0, sizeof(total));
    for (int i = 0; i < num_threads; i++) {
        stats_merge(&total, &ctxs[i].worker.stats);
        worker_destroy(&ctxs[i].worker);
    }

    stats_print(&total, elapsed);

    free(pipeline_buf);
    free(ctxs);
    free(threads);
    return 0;
}
