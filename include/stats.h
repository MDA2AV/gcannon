#pragma once

#include <stdint.h>
#include "constants.h"

/*
 * Two-tier latency histogram (~234KB per worker):
 *   Tier 1: 0–9999μs   at 1μs resolution   (10000 buckets)
 *   Tier 2: 10ms–5000ms at 100μs resolution (49900 buckets)
 * Anything > 5s goes into an overflow counter.
 */

#define TIER1_BUCKETS   10000   /* 0–9999μs, 1μs each */
#define TIER2_BUCKETS   49900   /* 10000–4999999μs, 100μs each */
#define TIER1_MAX_US    10000
#define TIER2_STEP_US   100
#define TIER2_MAX_US    (TIER1_MAX_US + TIER2_BUCKETS * TIER2_STEP_US)

/* Per-template latency histogram (same two-tier layout, heap-allocated) */
typedef struct latency_hist {
    uint32_t tier1[TIER1_BUCKETS];
    uint32_t tier2[TIER2_BUCKETS];
    uint32_t overflow;
    uint64_t count;
    uint64_t sum_us;
} latency_hist_t;

typedef struct worker_stats {
    uint64_t requests;
    uint64_t responses;
    uint64_t bytes_read;
    uint64_t connect_errors;
    uint64_t read_errors;
    uint64_t timeouts;
    uint64_t reconnects;
    uint64_t status_2xx;
    uint64_t status_3xx;
    uint64_t status_4xx;
    uint64_t status_5xx;
    uint64_t status_other;
    uint64_t ws_upgrades;
    uint32_t tier1[TIER1_BUCKETS];
    uint32_t tier2[TIER2_BUCKETS];
    uint32_t overflow;
    uint64_t latency_count;
    uint64_t latency_sum_us;
    uint64_t tpl_responses[MAX_TEMPLATES];
    uint64_t tpl_responses_2xx[MAX_TEMPLATES];
    latency_hist_t *tpl_latency;   /* per-template histograms (NULL if disabled) */
    int             num_tpl_latency;
} worker_stats_t;

void stats_record_latency(worker_stats_t *s, uint64_t latency_us);
void hist_record(latency_hist_t *h, uint64_t latency_us);
void hist_merge(latency_hist_t *dst, const latency_hist_t *src);
uint64_t hist_percentile(const latency_hist_t *h, double pct);
void stats_merge(worker_stats_t *dst, const worker_stats_t *src);
uint64_t stats_percentile(const worker_stats_t *s, double pct);
void stats_print(const worker_stats_t *s, double elapsed_sec, int num_templates,
                 int ws_mode);
void stats_print_json(const worker_stats_t *s, double elapsed_sec,
                      int num_templates, int ws_mode,
                      const char *target, int num_connections, int num_threads,
                      int pipeline_depth, int duration_sec);
