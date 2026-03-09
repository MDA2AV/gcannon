#include "stats.h"

#include <stdio.h>
#include <string.h>

void stats_record_latency(worker_stats_t *s, uint64_t latency_us)
{
    s->latency_count++;
    s->latency_sum_us += latency_us;

    if (latency_us < TIER1_MAX_US) {
        s->tier1[latency_us]++;
    } else if (latency_us < TIER2_MAX_US) {
        uint32_t idx = (latency_us - TIER1_MAX_US) / TIER2_STEP_US;
        s->tier2[idx]++;
    } else {
        s->overflow++;
    }
}

void stats_merge(worker_stats_t *dst, const worker_stats_t *src)
{
    dst->requests       += src->requests;
    dst->responses      += src->responses;
    dst->bytes_read     += src->bytes_read;
    dst->connect_errors += src->connect_errors;
    dst->read_errors    += src->read_errors;
    dst->timeouts       += src->timeouts;
    dst->reconnects     += src->reconnects;
    dst->status_2xx     += src->status_2xx;
    dst->status_3xx     += src->status_3xx;
    dst->status_4xx     += src->status_4xx;
    dst->status_5xx     += src->status_5xx;
    dst->status_other   += src->status_other;
    dst->latency_count  += src->latency_count;
    dst->latency_sum_us += src->latency_sum_us;
    dst->overflow       += src->overflow;

    for (int i = 0; i < TIER1_BUCKETS; i++)
        dst->tier1[i] += src->tier1[i];
    for (int i = 0; i < TIER2_BUCKETS; i++)
        dst->tier2[i] += src->tier2[i];
}

uint64_t stats_percentile(const worker_stats_t *s, double pct)
{
    if (s->latency_count == 0) return 0;

    uint64_t target = (uint64_t)(s->latency_count * pct);
    uint64_t cumulative = 0;

    for (int i = 0; i < TIER1_BUCKETS; i++) {
        cumulative += s->tier1[i];
        if (cumulative > target)
            return (uint64_t)i;
    }

    for (int i = 0; i < TIER2_BUCKETS; i++) {
        cumulative += s->tier2[i];
        if (cumulative > target)
            return TIER1_MAX_US + (uint64_t)i * TIER2_STEP_US;
    }

    return TIER2_MAX_US;
}

static void format_latency(char *buf, size_t sz, uint64_t us)
{
    if (us < 1000)
        snprintf(buf, sz, "%luus", us);
    else if (us < 1000000)
        snprintf(buf, sz, "%.2fms", us / 1000.0);
    else
        snprintf(buf, sz, "%.2fs", us / 1000000.0);
}

static void format_count(char *buf, size_t sz, uint64_t n)
{
    if (n >= 1000000)
        snprintf(buf, sz, "%.2fM", n / 1000000.0);
    else if (n >= 1000)
        snprintf(buf, sz, "%.2fK", n / 1000.0);
    else
        snprintf(buf, sz, "%lu", n);
}

static void format_bytes(char *buf, size_t sz, double bytes_per_sec)
{
    if (bytes_per_sec >= 1073741824.0)
        snprintf(buf, sz, "%.2fGB/s", bytes_per_sec / 1073741824.0);
    else if (bytes_per_sec >= 1048576.0)
        snprintf(buf, sz, "%.2fMB/s", bytes_per_sec / 1048576.0);
    else if (bytes_per_sec >= 1024.0)
        snprintf(buf, sz, "%.2fKB/s", bytes_per_sec / 1024.0);
    else
        snprintf(buf, sz, "%.0fB/s", bytes_per_sec);
}

void stats_print(const worker_stats_t *s, double elapsed_sec)
{
    char p50[32], p90[32], p99[32], p999[32];
    char rps_buf[32], bw_buf[32], avg_buf[32];

    format_latency(p50,  sizeof(p50),  stats_percentile(s, 0.50));
    format_latency(p90,  sizeof(p90),  stats_percentile(s, 0.90));
    format_latency(p99,  sizeof(p99),  stats_percentile(s, 0.99));
    format_latency(p999, sizeof(p999), stats_percentile(s, 0.999));

    uint64_t avg_us = s->latency_count ? s->latency_sum_us / s->latency_count : 0;
    format_latency(avg_buf, sizeof(avg_buf), avg_us);

    double rps = s->responses / elapsed_sec;
    format_count(rps_buf, sizeof(rps_buf), (uint64_t)rps);
    format_bytes(bw_buf, sizeof(bw_buf), s->bytes_read / elapsed_sec);

    printf("\n");
    printf("  Thread Stats   Avg      p50      p90      p99    p99.9\n");
    printf("    Latency   %6s   %6s   %6s   %6s   %6s\n",
           avg_buf, p50, p90, p99, p999);
    printf("\n");
    printf("  %lu requests in %.2fs, %lu responses\n",
           s->requests, elapsed_sec, s->responses);
    printf("  Throughput: %s req/s\n", rps_buf);
    printf("  Bandwidth:  %s\n", bw_buf);

    printf("  Status codes: 2xx=%lu, 3xx=%lu, 4xx=%lu, 5xx=%lu",
           s->status_2xx, s->status_3xx, s->status_4xx, s->status_5xx);
    if (s->status_other)
        printf(", other=%lu", s->status_other);
    printf("\n");

    printf("  Latency samples: %lu / %lu responses (%.1f%%)\n",
           s->latency_count, s->responses,
           s->responses ? 100.0 * s->latency_count / s->responses : 0.0);
    if (s->overflow)
        printf("  Latency overflow (>110ms): %u\n", s->overflow);
    if (s->reconnects)
        printf("  Reconnects: %lu\n", s->reconnects);
    if (s->connect_errors || s->read_errors || s->timeouts) {
        printf("  Errors: connect %lu, read %lu, timeout %lu\n",
               s->connect_errors, s->read_errors, s->timeouts);
    }
}
