#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include "history.h"

static const char *history_path(void)
{
    static char path[PATH_MAX];
    const char *home = getenv("HOME");
    if (!home) return NULL;

    snprintf(path, sizeof(path), "%s/.gcannon", home);
    mkdir(path, 0755); /* ignore EEXIST */

    snprintf(path, sizeof(path), "%s/.gcannon/history.bin", home);
    return path;
}

int history_load(history_file_t *hf)
{
    memset(hf, 0, sizeof(*hf));

    const char *p = history_path();
    if (!p) return -1;

    FILE *f = fopen(p, "rb");
    if (!f) return 0; /* first run */

    size_t n = fread(hf, 1, sizeof(*hf), f);
    fclose(f);

    if (n != sizeof(*hf) || hf->magic != HISTORY_MAGIC ||
        hf->version != HISTORY_VERSION) {
        memset(hf, 0, sizeof(*hf));
        return 0;
    }

    if (hf->count < 0) hf->count = 0;
    if (hf->count > HISTORY_MAX_RUNS) hf->count = HISTORY_MAX_RUNS;

    return 0;
}

int history_save(history_file_t *hf, const run_record_t *rec)
{
    const char *p = history_path();
    if (!p) return -1;

    if (hf->count >= HISTORY_MAX_RUNS) {
        memmove(&hf->runs[0], &hf->runs[1],
                (HISTORY_MAX_RUNS - 1) * sizeof(run_record_t));
        hf->runs[HISTORY_MAX_RUNS - 1] = *rec;
    } else {
        hf->runs[hf->count++] = *rec;
    }

    hf->magic   = HISTORY_MAGIC;
    hf->version = HISTORY_VERSION;

    FILE *f = fopen(p, "wb");
    if (!f) return -1;

    fwrite(hf, 1, sizeof(*hf), f);
    fclose(f);
    return 0;
}

void history_clear(void)
{
    const char *p = history_path();
    if (p) remove(p);
}

void history_build_record(run_record_t *rec,
                          const worker_stats_t *stats,
                          double elapsed_sec,
                          const char *host, int port, const char *path,
                          int num_connections, int num_threads,
                          int pipeline_depth, int duration_sec)
{
    memset(rec, 0, sizeof(*rec));

    rec->timestamp       = (int64_t)time(NULL);
    rec->num_connections = num_connections;
    rec->num_threads     = num_threads;
    rec->pipeline_depth  = pipeline_depth;
    rec->duration_sec    = duration_sec;
    rec->elapsed_sec     = elapsed_sec;

    if (port == 80)
        snprintf(rec->target, sizeof(rec->target), "%s%s", host, path);
    else
        snprintf(rec->target, sizeof(rec->target), "%s:%d%s", host, port, path);

    rec->rps           = elapsed_sec > 0 ? stats->responses / elapsed_sec : 0;
    rec->bandwidth_bps = elapsed_sec > 0 ? (double)stats->bytes_read / elapsed_sec : 0;
    rec->requests      = stats->requests;
    rec->responses     = stats->responses;

    rec->latency_avg_us  = stats->latency_count
                           ? stats->latency_sum_us / stats->latency_count : 0;
    rec->latency_p50_us  = stats_percentile(stats, 0.50);
    rec->latency_p90_us  = stats_percentile(stats, 0.90);
    rec->latency_p99_us  = stats_percentile(stats, 0.99);
    rec->latency_p999_us = stats_percentile(stats, 0.999);

    rec->status_2xx = stats->status_2xx;
    rec->status_3xx = stats->status_3xx;
    rec->status_4xx = stats->status_4xx;
    rec->status_5xx = stats->status_5xx;

    rec->connect_errors = stats->connect_errors;
    rec->read_errors    = stats->read_errors;
    rec->reconnects     = stats->reconnects;
}
