#include "core/stats.h"

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
    dst->ws_upgrade_failures += src->ws_upgrade_failures;
    dst->latency_count  += src->latency_count;
    dst->latency_sum_us += src->latency_sum_us;
    dst->overflow       += src->overflow;
    dst->paced_sends    += src->paced_sends;
    dst->paced_late     += src->paced_late;
    dst->conns_active   += src->conns_active;
    if (src->paced_lag_max_us > dst->paced_lag_max_us)
        dst->paced_lag_max_us = src->paced_lag_max_us;

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
