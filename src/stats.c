#include "stats.h"

#include <stdio.h>
#include <string.h>

void hist_record(latency_hist_t *h, uint64_t latency_us)
{
    h->count++;
    h->sum_us += latency_us;

    if (latency_us < TIER1_MAX_US)
        h->tier1[latency_us]++;
    else if (latency_us < TIER2_MAX_US)
        h->tier2[(latency_us - TIER1_MAX_US) / TIER2_STEP_US]++;
    else
        h->overflow++;
}

void hist_merge(latency_hist_t *dst, const latency_hist_t *src)
{
    dst->count  += src->count;
    dst->sum_us += src->sum_us;
    dst->overflow += src->overflow;
    for (int i = 0; i < TIER1_BUCKETS; i++)
        dst->tier1[i] += src->tier1[i];
    for (int i = 0; i < TIER2_BUCKETS; i++)
        dst->tier2[i] += src->tier2[i];
}

uint64_t hist_percentile(const latency_hist_t *h, double pct)
{
    if (h->count == 0) return 0;

    const uint64_t target = (uint64_t)(h->count * pct);
    uint64_t cumulative = 0;

    for (int i = 0; i < TIER1_BUCKETS; i++) {
        cumulative += h->tier1[i];
        if (cumulative > target)
            return (uint64_t)i;
    }
    for (int i = 0; i < TIER2_BUCKETS; i++) {
        cumulative += h->tier2[i];
        if (cumulative > target)
            return TIER1_MAX_US + (uint64_t)i * TIER2_STEP_US;
    }
    return TIER2_MAX_US;
}

void stats_record_latency(worker_stats_t *s, uint64_t latency_us)
{
    s->latency_count++;
    s->latency_sum_us += latency_us;

    if (latency_us < TIER1_MAX_US) {
        s->tier1[latency_us]++;
    } else if (latency_us < TIER2_MAX_US) {
        const uint32_t idx = (latency_us - TIER1_MAX_US) / TIER2_STEP_US;
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
    dst->ws_upgrades    += src->ws_upgrades;
    dst->latency_count  += src->latency_count;
    dst->latency_sum_us += src->latency_sum_us;
    dst->overflow       += src->overflow;

    for (int i = 0; i < MAX_TEMPLATES; i++) {
        dst->tpl_responses[i] += src->tpl_responses[i];
        dst->tpl_responses_2xx[i] += src->tpl_responses_2xx[i];
    }

    for (int i = 0; i < TIER1_BUCKETS; i++)
        dst->tier1[i] += src->tier1[i];
    for (int i = 0; i < TIER2_BUCKETS; i++)
        dst->tier2[i] += src->tier2[i];

    if (src->tpl_latency && dst->tpl_latency) {
        int n = dst->num_tpl_latency < src->num_tpl_latency
                ? dst->num_tpl_latency : src->num_tpl_latency;
        for (int i = 0; i < n; i++)
            hist_merge(&dst->tpl_latency[i], &src->tpl_latency[i]);
    }
}

uint64_t stats_percentile(const worker_stats_t *s, double pct)
{
    if (s->latency_count == 0) return 0;

    const uint64_t target = (uint64_t)(s->latency_count * pct);
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

static void format_latency(char *buf, size_t sz, const uint64_t us)
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

void stats_print(const worker_stats_t *s, double elapsed_sec, int num_templates,
                 int ws_mode)
{
    char p50[32], p90[32], p99[32], p999[32];
    char rps_buf[32], bw_buf[32], avg_buf[32];

    format_latency(p50,  sizeof(p50),  stats_percentile(s, 0.50));
    format_latency(p90,  sizeof(p90),  stats_percentile(s, 0.90));
    format_latency(p99,  sizeof(p99),  stats_percentile(s, 0.99));
    format_latency(p999, sizeof(p999), stats_percentile(s, 0.999));

    const uint64_t avg_us = s->latency_count ? s->latency_sum_us / s->latency_count : 0;
    format_latency(avg_buf, sizeof(avg_buf), avg_us);

    const double rps = s->responses / elapsed_sec;
    format_count(rps_buf, sizeof(rps_buf), (uint64_t)rps);
    format_bytes(bw_buf, sizeof(bw_buf), s->bytes_read / elapsed_sec);

    printf("\n");
    printf("  Thread Stats   Avg      p50      p90      p99    p99.9\n");
    printf("    Latency   %6s   %6s   %6s   %6s   %6s\n",
           avg_buf, p50, p90, p99, p999);
    printf("\n");
    if (ws_mode)
        printf("  %lu frames sent in %.2fs, %lu frames received\n",
               s->requests, elapsed_sec, s->responses);
    else
        printf("  %lu requests in %.2fs, %lu responses\n",
               s->requests, elapsed_sec, s->responses);
    printf("  Throughput: %s req/s\n", rps_buf);
    printf("  Bandwidth:  %s\n", bw_buf);

    if (ws_mode) {
        printf("  WS upgrades: %lu\n", s->ws_upgrades);
        printf("  WS frames:   %lu\n", s->status_2xx);
    } else {
        printf("  Status codes: 2xx=%lu, 3xx=%lu, 4xx=%lu, 5xx=%lu",
               s->status_2xx, s->status_3xx, s->status_4xx, s->status_5xx);
        if (s->status_other)
            printf(", other=%lu", s->status_other);
        printf("\n");
    }

    printf("  Latency samples: %lu / %lu responses (%.1f%%)\n",
           s->latency_count, s->responses,
           s->responses ? 100.0 * s->latency_count / s->responses : 0.0);
    if (s->overflow)
        printf("  Latency overflow (>5s): %u\n", s->overflow);
    if (s->reconnects)
        printf("  Reconnects: %lu\n", s->reconnects);
    if (s->connect_errors || s->read_errors || s->timeouts) {
        printf("  Errors: connect %lu, read %lu, timeout %lu\n",
               s->connect_errors, s->read_errors, s->timeouts);
    }

    if (num_templates > 1) {
        printf("  Per-template: ");
        for (int i = 0; i < num_templates; i++) {
            if (i > 0) printf(",");
            printf("%lu", s->tpl_responses[i]);
        }
        printf("\n");
        printf("  Per-template-ok: ");
        for (int i = 0; i < num_templates; i++) {
            if (i > 0) printf(",");
            printf("%lu", s->tpl_responses_2xx[i]);
        }
        printf("\n");
    }

    if (s->tpl_latency && num_templates > 1) {
        printf("\n");
        printf("  Per-template latency:\n");
        printf("  %4s  %8s  %8s  %8s  %8s  %8s\n",
               "Tpl", "Avg", "p50", "p90", "p99", "p99.9");
        for (int i = 0; i < s->num_tpl_latency && i < num_templates; i++) {
            const latency_hist_t *h = &s->tpl_latency[i];
            if (h->count == 0) continue;
            char a[32], p5[32], p9[32], p99b[32], p999b[32];
            format_latency(a,    sizeof(a),    h->count ? h->sum_us / h->count : 0);
            format_latency(p5,   sizeof(p5),   hist_percentile(h, 0.50));
            format_latency(p9,   sizeof(p9),   hist_percentile(h, 0.90));
            format_latency(p99b, sizeof(p99b), hist_percentile(h, 0.99));
            format_latency(p999b,sizeof(p999b),hist_percentile(h, 0.999));
            printf("    #%-3d %8s  %8s  %8s  %8s  %8s\n",
                   i, a, p5, p9, p99b, p999b);
        }
    }
}

void stats_print_json(const worker_stats_t *s, double elapsed_sec,
                      int num_templates, int ws_mode,
                      const char *target, int num_connections, int num_threads,
                      int pipeline_depth, int duration_sec)
{
    const uint64_t avg_us = s->latency_count
                            ? s->latency_sum_us / s->latency_count : 0;
    const double rps = elapsed_sec > 0 ? s->responses / elapsed_sec : 0;
    const double bw = elapsed_sec > 0 ? (double)s->bytes_read / elapsed_sec : 0;

    printf("{\n");
    printf("  \"version\": \"%s\",\n", GCANNON_VERSION);
    printf("  \"target\": \"%s\",\n", target);
    printf("  \"mode\": \"%s\",\n", ws_mode ? "websocket" : "http");
    printf("  \"connections\": %d,\n", num_connections);
    printf("  \"threads\": %d,\n", num_threads);
    printf("  \"pipeline_depth\": %d,\n", pipeline_depth);
    printf("  \"duration_sec\": %d,\n", duration_sec);
    printf("  \"elapsed_sec\": %.3f,\n", elapsed_sec);

    printf("  \"requests\": %lu,\n", s->requests);
    printf("  \"responses\": %lu,\n", s->responses);
    printf("  \"rps\": %.2f,\n", rps);
    printf("  \"bandwidth_bps\": %.2f,\n", bw);
    printf("  \"bytes_read\": %lu,\n", s->bytes_read);

    printf("  \"latency_us\": {\n");
    printf("    \"avg\": %lu,\n", avg_us);
    printf("    \"p50\": %lu,\n", stats_percentile(s, 0.50));
    printf("    \"p90\": %lu,\n", stats_percentile(s, 0.90));
    printf("    \"p99\": %lu,\n", stats_percentile(s, 0.99));
    printf("    \"p999\": %lu,\n", stats_percentile(s, 0.999));
    printf("    \"samples\": %lu\n", s->latency_count);
    printf("  },\n");

    if (ws_mode) {
        printf("  \"ws_upgrades\": %lu,\n", s->ws_upgrades);
        printf("  \"ws_frames\": %lu,\n", s->status_2xx);
    }

    printf("  \"status\": {\n");
    printf("    \"2xx\": %lu,\n", s->status_2xx);
    printf("    \"3xx\": %lu,\n", s->status_3xx);
    printf("    \"4xx\": %lu,\n", s->status_4xx);
    printf("    \"5xx\": %lu,\n", s->status_5xx);
    printf("    \"other\": %lu\n", s->status_other);
    printf("  },\n");

    printf("  \"errors\": {\n");
    printf("    \"connect\": %lu,\n", s->connect_errors);
    printf("    \"read\": %lu,\n", s->read_errors);
    printf("    \"timeout\": %lu\n", s->timeouts);
    printf("  },\n");

    printf("  \"reconnects\": %lu", s->reconnects);

    if (num_templates > 1) {
        printf(",\n  \"per_template\": [");
        for (int i = 0; i < num_templates; i++) {
            if (i > 0) printf(",");
            printf("\n    {\"responses\": %lu, \"2xx\": %lu",
                   s->tpl_responses[i], s->tpl_responses_2xx[i]);
            if (s->tpl_latency && i < s->num_tpl_latency && s->tpl_latency[i].count > 0) {
                const latency_hist_t *h = &s->tpl_latency[i];
                printf(", \"latency_us\": {\"avg\": %lu, \"p50\": %lu, \"p99\": %lu}",
                       h->sum_us / h->count,
                       hist_percentile(h, 0.50),
                       hist_percentile(h, 0.99));
            }
            printf("}");
        }
        printf("\n  ]");
    }

    printf("\n}\n");
}
