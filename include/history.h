#pragma once

#include <stdint.h>
#include <time.h>
#include "stats.h"

#define HISTORY_MAGIC     0x47434E48   /* "GCNH" */
#define HISTORY_VERSION   1
#define HISTORY_MAX_RUNS  5

typedef struct run_record {
    int64_t   timestamp;
    char      target[256];
    int32_t   num_connections;
    int32_t   num_threads;
    int32_t   pipeline_depth;
    int32_t   duration_sec;
    double    elapsed_sec;

    double    rps;
    double    bandwidth_bps;
    uint64_t  requests;
    uint64_t  responses;

    uint64_t  latency_avg_us;
    uint64_t  latency_p50_us;
    uint64_t  latency_p90_us;
    uint64_t  latency_p99_us;
    uint64_t  latency_p999_us;

    uint64_t  status_2xx;
    uint64_t  status_3xx;
    uint64_t  status_4xx;
    uint64_t  status_5xx;

    uint64_t  connect_errors;
    uint64_t  read_errors;
    uint64_t  reconnects;

    uint8_t   reserved[32];
} run_record_t;

typedef struct history_file {
    uint32_t      magic;
    uint32_t      version;
    int32_t       count;
    int32_t       pad;
    run_record_t  runs[HISTORY_MAX_RUNS];
} history_file_t;

int  history_load(history_file_t *hf);
int  history_save(history_file_t *hf, const run_record_t *rec);
void history_build_record(run_record_t *rec,
                          const worker_stats_t *stats,
                          double elapsed_sec,
                          const char *host, int port, const char *path,
                          int num_connections, int num_threads,
                          int pipeline_depth, int duration_sec);
