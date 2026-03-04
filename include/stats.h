#pragma once

#include <stdint.h>

/*
 * Two-tier latency histogram (~88KB per worker):
 *   Tier 1: 0–9999μs   at 1μs resolution   (10000 buckets)
 *   Tier 2: 10ms–110ms  at 100μs resolution (1000 buckets)
 * Anything > 110ms goes into an overflow counter.
 */

#define TIER1_BUCKETS   10000   /* 0–9999μs, 1μs each */
#define TIER2_BUCKETS   1000    /* 10000–109999μs, 100μs each */
#define TIER1_MAX_US    10000
#define TIER2_STEP_US   100
#define TIER2_MAX_US    (TIER1_MAX_US + TIER2_BUCKETS * TIER2_STEP_US)

typedef struct worker_stats {
    uint64_t requests;
    uint64_t responses;
    uint64_t bytes_read;
    uint64_t connect_errors;
    uint64_t read_errors;
    uint64_t timeouts;
    uint32_t tier1[TIER1_BUCKETS];
    uint32_t tier2[TIER2_BUCKETS];
    uint32_t overflow;
    uint64_t latency_count;
    uint64_t latency_sum_us;
} worker_stats_t;

void stats_record_latency(worker_stats_t *s, uint64_t latency_us);
void stats_merge(worker_stats_t *dst, const worker_stats_t *src);
uint64_t stats_percentile(const worker_stats_t *s, double pct);
void stats_print(const worker_stats_t *s, double elapsed_sec);
