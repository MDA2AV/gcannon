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
    dst->ws_upgrade_failures += src->ws_upgrade_failures;
    dst->latency_count  += src->latency_count;
    dst->latency_sum_us += src->latency_sum_us;
    dst->overflow       += src->overflow;
    dst->paced_sends    += src->paced_sends;
    dst->paced_late     += src->paced_late;
    dst->conns_active   += src->conns_active;
    dst->conns_dead     += src->conns_dead;
    dst->conns_silent   += src->conns_silent;
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

void stats_clear(worker_stats_t *s)
{
    latency_hist_t *tpl = s->tpl_latency;
    const int n = s->num_tpl_latency;
    memset(s, 0, sizeof(*s));
    if (tpl) {
        memset(tpl, 0, (size_t)n * sizeof(*tpl));
        s->tpl_latency = tpl;
        s->num_tpl_latency = n;
    }
}

void stats_copy(worker_stats_t *dst, const worker_stats_t *src)
{
    latency_hist_t *tpl = dst->tpl_latency;
    const int n = dst->num_tpl_latency;
    *dst = *src;
    dst->tpl_latency = tpl;
    dst->num_tpl_latency = n;
    if (tpl && src->tpl_latency) {
        const int m = n < src->num_tpl_latency ? n : src->num_tpl_latency;
        memcpy(tpl, src->tpl_latency, (size_t)m * sizeof(*tpl));
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
        if (s->ws_upgrade_failures)
            printf("  WS upgrades refused: %lu\n", s->ws_upgrade_failures);
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

static void ramp_print_json(const ramp_result_t *r);
static void timeline_print_json(const timeline_t *t);

void stats_print_json(const worker_stats_t *s, double elapsed_sec,
                      int num_templates, int ws_mode,
                      const char *target, int num_connections, int num_threads,
                      int pipeline_depth, int duration_sec, double target_rate,
                      const ramp_result_t *ramp, const timeline_t *timeline)
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
        printf("  \"ws_upgrade_failures\": %lu,\n", s->ws_upgrade_failures);
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

    /* --rate only, so every other run's JSON stays exactly as it was */
    if (target_rate > 0) {
        printf(",\n  \"rate\": {\n");
        printf("    \"target\": %.0f,\n", target_rate);
        printf("    \"achieved\": %.2f,\n", rps);
        printf("    \"ratio\": %.4f,\n", rps / target_rate);
        printf("    \"paced_sends\": %lu,\n", s->paced_sends);
        printf("    \"late\": %lu,\n", s->paced_late);
        printf("    \"max_lag_us\": %lu\n", s->paced_lag_max_us);
        printf("  },\n");
        printf("  \"connections_active\": %lu", s->conns_active);
    }

    /* --ramp-p99 only, like the rate block */
    if (ramp)
        ramp_print_json(ramp);
    if (timeline)
        timeline_print_json(timeline);

    printf("\n}\n");
}

void stats_print_rate(const worker_stats_t *s, double elapsed_sec,
                      double target_rate, int num_connections)
{
    const double rps = elapsed_sec > 0 ? s->responses / elapsed_sec : 0;
    char lag[32];
    format_latency(lag, sizeof(lag), s->paced_lag_max_us);
    printf("  Rate:       target %.0f req/s, achieved %.0f (%.2f%%)\n",
           target_rate, rps, target_rate > 0 ? 100.0 * rps / target_rate : 0.0);
    printf("  Schedule:   %lu paced sends, %lu more than 1ms late, max lag %s\n",
           s->paced_sends, s->paced_late, lag);
    printf("  Active:     %lu of %d connections open at the end\n",
           s->conns_active, num_connections);
}

/* ── --ramp-p99 ────────────────────────────────────────────────────── */

/* The word --json reports for a verdict */
static const char *ramp_verdict_name(const ramp_verdict_t v)
{
    switch (v) {
    case RAMP_OK:          return "ok";
    case RAMP_SOCKETS:     return "sockets";
    case RAMP_SETUP:       return "setup";
    case RAMP_RESPONSES:   return "responses";
    case RAMP_SILENT:      return "silent";
    case RAMP_STATUS:      return "status";
    case RAMP_ERRORS:      return "errors";
    case RAMP_P99:         return "p99";
    case RAMP_CEILING:     return "ceiling";
    case RAMP_INTERRUPTED: return "interrupted";
    }
    return "unknown";
}

static const char *plural(const uint64_t n)
{
    return n == 1 ? "" : "s";
}

/* Why a step failed, in words */
static void ramp_describe(char *buf, const size_t sz, const ramp_step_t *st,
                          const uint64_t p99_max_us)
{
    char thr[32];
    format_latency(thr, sizeof(thr), p99_max_us);
    switch (st->verdict) {
    case RAMP_SOCKETS:
        snprintf(buf, sz, "%lu connection%s could not get a socket (ulimit -n?)",
                 st->dead, plural(st->dead));
        break;
    case RAMP_SETUP:
        snprintf(buf, sz, "only %d of %d connections set up", st->set_up, st->connections);
        break;
    case RAMP_RESPONSES:
        snprintf(buf, sz, "no responses");
        break;
    case RAMP_SILENT:
        snprintf(buf, sz, "%lu connection%s got no response", st->silent, plural(st->silent));
        break;
    case RAMP_STATUS:
        snprintf(buf, sz, "%lu response%s with an unexpected status",
                 st->unexpected, plural(st->unexpected));
        break;
    case RAMP_ERRORS:
        snprintf(buf, sz, "%lu error%s", st->errors, plural(st->errors));
        break;
    case RAMP_P99:
        snprintf(buf, sz, "p99 over %s", thr);
        break;
    default:
        buf[0] = '\0';
    }
}

void ramp_print_plan(const int start, const double factor, const int increment,
                     const uint64_t p99_max_us)
{
    char thr[32], step[32];
    format_latency(thr, sizeof(thr), p99_max_us);
    if (increment > 0)
        snprintf(step, sizeof(step), "+%d", increment);
    else
        snprintf(step, sizeof(step), "x%g", factor);
    printf("  Ramp:      from %d connection%s, %s per step, while p99 stays within %s\n",
           start, plural((uint64_t)start), step, thr);
}

void ramp_print_header(FILE *out)
{
    fprintf(out, "  %8s  %9s  %8s  %8s  %8s  %8s\n",
            "Conns", "Req/s", "p50", "p90", "p99", "p99.9");
    fflush(out);
}

void ramp_print_step(FILE *out, const ramp_step_t *st, const uint64_t p99_max_us,
                     const int again)
{
    char rps[32], p50[32], p90[32], p99[32], p999[32];
    format_count(rps, sizeof(rps),
                 st->elapsed_sec > 0 ? (uint64_t)(st->responses / st->elapsed_sec) : 0);
    format_latency(p50,  sizeof(p50),  st->p50_us);
    format_latency(p90,  sizeof(p90),  st->p90_us);
    format_latency(p99,  sizeof(p99),  st->p99_us);
    format_latency(p999, sizeof(p999), st->p999_us);
    fprintf(out, "  %8d  %9s  %8s  %8s  %8s  %8s",
            st->connections, rps, p50, p90, p99, p999);
    if (st->verdict != RAMP_OK) {
        char why[128];
        ramp_describe(why, sizeof(why), st, p99_max_us);
        fprintf(out, "   %s%s", why, again ? ", measuring again" : "");
    }
    fprintf(out, "\n");
    fflush(out);
}

void ramp_print_result(const ramp_result_t *r)
{
    char thr[32];
    format_latency(thr, sizeof(thr), r->p99_max_us);

    printf("  Ramp:       held p99 within %s ", thr);
    if (r->best >= 0) {
        const ramp_step_t *b = &r->steps[r->best];
        char p99[32];
        format_latency(p99, sizeof(p99), b->p99_us);
        printf("up to %d connection%s (p99 %s)", b->connections,
               plural((uint64_t)b->connections), p99);
    } else {
        printf("at no step");
    }

    const ramp_step_t *last = r->num_steps > 0 ? &r->steps[r->num_steps - 1] : NULL;
    if (r->stop == RAMP_CEILING) {
        printf(", the -c ceiling\n");
    } else if (r->stop == RAMP_INTERRUPTED || !last) {
        printf("; interrupted\n");
    } else {
        char why[128];
        ramp_describe(why, sizeof(why), last, r->p99_max_us);
        printf("; at %d, %s\n", last->connections, why);
    }
}

static void ramp_print_json(const ramp_result_t *r)
{
    printf(",\n  \"ramp\": {\n");
    printf("    \"p99_max_us\": %lu,\n", r->p99_max_us);
    printf("    \"max_connections\": %d,\n",
           r->best >= 0 ? r->steps[r->best].connections : 0);
    printf("    \"stopped\": \"%s\",\n", ramp_verdict_name(r->stop));
    printf("    \"steps\": [");
    for (int i = 0; i < r->num_steps; i++) {
        const ramp_step_t *st = &r->steps[i];
        const double rps = st->elapsed_sec > 0 ? st->responses / st->elapsed_sec : 0;
        printf("%s\n      {\"connections\": %d, \"set_up\": %d, \"active\": %lu, "
               "\"elapsed_sec\": %.3f, \"responses\": %lu, \"rps\": %.2f, "
               "\"latency_us\": {\"p50\": %lu, \"p90\": %lu, \"p99\": %lu, "
               "\"p999\": %lu, \"samples\": %lu}, "
               "\"errors\": %lu, \"unexpected\": %lu, \"silent\": %lu, \"dead\": %lu, "
               "\"retry\": %s, \"pass\": %s",
               i ? "," : "", st->connections, st->set_up, st->active,
               st->elapsed_sec, st->responses, rps,
               st->p50_us, st->p90_us, st->p99_us, st->p999_us, st->samples,
               st->errors, st->unexpected, st->silent, st->dead,
               st->retry ? "true" : "false",
               st->verdict == RAMP_OK ? "true" : "false");
        if (st->verdict != RAMP_OK)
            printf(", \"fail\": \"%s\"", ramp_verdict_name(st->verdict));
        printf("}");
    }
    printf("\n    ]\n  }");
}

/* ── --ramp-up and --idle ──────────────────────────────────────────── */

void idle_age_record(idle_stats_t *s, const uint64_t age_us)
{
    if (!s->age_count || age_us < s->age_min_us) s->age_min_us = age_us;
    if (!s->age_count || age_us > s->age_max_us) s->age_max_us = age_us;
    s->age_count++;
    const uint64_t bucket = age_us / IDLE_AGE_STEP_US;
    if (bucket < IDLE_AGE_BUCKETS) s->age_hist[bucket]++;
    else                           s->age_overflow++;
}

/* The age under which pct of the closes fell, within 100 ms. */
uint64_t idle_age_percentile(const idle_stats_t *s, const double pct)
{
    if (!s->age_count) return 0;
    const uint64_t target = (uint64_t)(s->age_count * pct);
    uint64_t cumulative = 0;
    for (int i = 0; i < IDLE_AGE_BUCKETS; i++) {
        cumulative += s->age_hist[i];
        if (cumulative > target) {
            uint64_t us = (uint64_t)i * IDLE_AGE_STEP_US + IDLE_AGE_STEP_US / 2;
            if (us < s->age_min_us) us = s->age_min_us;
            if (us > s->age_max_us) us = s->age_max_us;
            return us;
        }
    }
    return s->age_max_us;
}

void idle_stats_merge(idle_stats_t *dst, const idle_stats_t *src)
{
    dst->c.opened         += src->c.opened;
    dst->c.answered       += src->c.answered;
    dst->c.open_now       += src->c.open_now;
    dst->c.closed         += src->c.closed;
    dst->c.failed         += src->c.failed;
    dst->c.connect_errors += src->c.connect_errors;
    dst->c.read_errors    += src->c.read_errors;
    dst->c.bad_status     += src->c.bad_status;
    dst->c.unsolicited    += src->c.unsolicited;
    if (src->age_count) {
        if (!dst->age_count || src->age_min_us < dst->age_min_us)
            dst->age_min_us = src->age_min_us;
        if (!dst->age_count || src->age_max_us > dst->age_max_us)
            dst->age_max_us = src->age_max_us;
        dst->age_count += src->age_count;
    }
    for (int i = 0; i < IDLE_AGE_BUCKETS; i++)
        dst->age_hist[i] += src->age_hist[i];
    dst->age_overflow += src->age_overflow;
    hist_merge(&dst->first, &src->first);
}

static void format_seconds(char *buf, const size_t sz, const uint64_t us)
{
    snprintf(buf, sz, "%.1fs", us / 1e6);
}

void timeline_print_plan(const timeline_t *t)
{
    if (t->ramp_sec > 0)
        printf("  Ramp-up:   from %d to %d active connections over %ds\n",
               t->floor, t->num_active, t->ramp_sec);
    if (t->num_idle > 0)
        printf("  Idle:      %d of %d connections (%.1f%%), one request each%s\n",
               t->num_idle, t->total_conns, 100.0 * t->num_idle / t->total_conns,
               t->ramp_sec > 0 ? "" : ", opened when the window does");
}

void timeline_print_header(FILE *out, const int has_idle)
{
    if (has_idle)
        fprintf(out, "  %7s  %8s  %8s  %8s  %8s  %9s  %8s  %8s  %8s\n",
                "Time", "Active", "Idle", "Opened", "Closed", "Req/s", "p50", "p99", "p99.9");
    else
        fprintf(out, "  %7s  %8s  %9s  %8s  %8s  %8s\n",
                "Time", "Active", "Req/s", "p50", "p99", "p99.9");
    fflush(out);
}

void timeline_print_row(FILE *out, const timeline_row_t *r, const int has_idle)
{
    char time[32], active[32], idle[32], opened[32], closed[32];
    char rps[32], p50[32], p99[32], p999[32];
    snprintf(time, sizeof(time), "%.1fs", r->t);
    format_count(active, sizeof(active), r->open_active);
    format_count(rps, sizeof(rps), (uint64_t)r->rps);
    format_latency(p50,  sizeof(p50),  r->p50_us);
    format_latency(p99,  sizeof(p99),  r->p99_us);
    format_latency(p999, sizeof(p999), r->p999_us);
    if (has_idle) {
        format_count(idle,   sizeof(idle),   r->open_idle);
        format_count(opened, sizeof(opened), r->opened);
        format_count(closed, sizeof(closed), r->closed);
        fprintf(out, "  %7s  %8s  %8s  %8s  %8s  %9s  %8s  %8s  %8s",
                time, active, idle, opened, closed, rps, p50, p99, p999);
        if (r->failed)
            fprintf(out, "   %lu failed", r->failed);
    } else {
        fprintf(out, "  %7s  %8s  %9s  %8s  %8s  %8s",
                time, active, rps, p50, p99, p999);
    }
    fprintf(out, "\n");
    fflush(out);
}

static void phase_print(const char *label, const timeline_phase_t *p)
{
    char rps[32], p50[32], p99[32], p999[32];
    format_count(rps, sizeof(rps), (uint64_t)p->rps);
    format_latency(p50,  sizeof(p50),  p->p50_us);
    format_latency(p99,  sizeof(p99),  p->p99_us);
    format_latency(p999, sizeof(p999), p->p999_us);
    printf("%s %.0fs: %s req/s, p50 %s, p99 %s, p99.9 %s", label, p->sec, rps, p50, p99, p999);
}

void timeline_print_result(const timeline_t *t)
{
    if (t->has_ramp || t->has_hold) {
        printf("  Phases:     ");
        if (t->has_ramp) phase_print("ramp", &t->ramp);
        if (t->has_ramp && t->has_hold) printf("; ");
        if (t->has_hold) phase_print(t->has_ramp ? "after" : "all", &t->hold);
        printf("\n");
    }
    const idle_stats_t *m = t->merged;
    if (!m || t->num_idle <= 0) return;
    const idle_counts_t *c = &m->c;
    char p50[32], p99[32];
    printf("  Idle:       %d asked for, %lu opened, %lu answered, %lu failed",
           t->num_idle, c->opened, c->answered, c->failed);
    if (c->connect_errors || c->read_errors)
        printf(" (connect %lu, read %lu)", c->connect_errors, c->read_errors);
    if (t->pending)
        printf(", %lu not opened by the end", t->pending);
    const uint64_t lost = c->failed - (c->connect_errors < c->failed ? c->connect_errors : c->failed);
    const uint64_t waiting = c->opened > c->answered + lost ? c->opened - c->answered - lost : 0;
    if (waiting)
        printf(", %lu still waiting for their response", waiting);
    printf("\n");
    if (m->first.count) {
        format_latency(p50, sizeof(p50), hist_percentile(&m->first, 0.50));
        format_latency(p99, sizeof(p99), hist_percentile(&m->first, 0.99));
        printf("              first response p50 %s, p99 %s\n", p50, p99);
    }
    if (c->closed) {
        char first[32], med[32], last[32];
        format_seconds(first, sizeof(first), m->age_min_us);
        format_seconds(med,   sizeof(med),   idle_age_percentile(m, 0.50));
        format_seconds(last,  sizeof(last),  m->age_max_us);
        printf("              %lu closed by the server (first after %s idle, "
               "median %s, last %s), %lu still open at the end\n",
               c->closed, first, med, last, c->open_now);
    } else {
        printf("              none closed by the server, %lu still open at the end\n",
               c->open_now);
    }
    if (c->bad_status)
        printf("              %lu answered with an unexpected status\n", c->bad_status);
    if (c->unsolicited)
        printf("              %lu were sent data nobody asked for\n", c->unsolicited);
}

static void phase_print_json(const char *name, const timeline_phase_t *p)
{
    printf("\"%s\": {\"sec\": %.3f, \"rps\": %.2f, \"p50_us\": %lu, "
           "\"p99_us\": %lu, \"p999_us\": %lu}",
           name, p->sec, p->rps, p->p50_us, p->p99_us, p->p999_us);
}

static void timeline_print_json(const timeline_t *t)
{
    if (t->ramp_sec > 0)
        printf(",\n  \"ramp_up\": {\"sec\": %d, \"from\": %d, \"to\": %d}",
               t->ramp_sec, t->floor, t->num_active);
    if (t->merged && t->num_idle > 0) {
        const idle_stats_t *m = t->merged;
        const idle_counts_t *c = &m->c;
        printf(",\n  \"idle\": {\"connections\": %d, \"opened\": %lu, "
               "\"answered\": %lu, \"failed\": %lu, \"connect_errors\": %lu, "
               "\"read_errors\": %lu, \"not_opened\": %lu, \"unexpected_status\": %lu, "
               "\"unsolicited\": %lu, \"closed_by_server\": %lu, \"open_at_end\": %lu",
               t->num_idle, c->opened, c->answered, c->failed, c->connect_errors,
               c->read_errors, t->pending, c->bad_status, c->unsolicited, c->closed,
               c->open_now);
        if (c->closed)
            printf(", \"closed_after_s\": {\"first\": %.3f, \"p50\": %.3f, "
                   "\"p90\": %.3f, \"p99\": %.3f, \"last\": %.3f}",
                   m->age_min_us / 1e6, idle_age_percentile(m, 0.50) / 1e6,
                   idle_age_percentile(m, 0.90) / 1e6,
                   idle_age_percentile(m, 0.99) / 1e6, m->age_max_us / 1e6);
        if (m->first.count)
            printf(", \"first_response_us\": {\"p50\": %lu, \"p99\": %lu, \"samples\": %lu}",
                   hist_percentile(&m->first, 0.50), hist_percentile(&m->first, 0.99),
                   m->first.count);
        printf("}");
    }
    if (t->has_ramp || t->has_hold) {
        printf(",\n  \"phases\": {");
        if (t->has_ramp) phase_print_json("ramp", &t->ramp);
        if (t->has_ramp && t->has_hold) printf(", ");
        if (t->has_hold) phase_print_json("hold", &t->hold);
        printf("}");
    }
    printf(",\n  \"timeline\": [");
    for (int i = 0; i < t->num_rows; i++) {
        const timeline_row_t *r = &t->rows[i];
        printf("%s\n    {\"t\": %.3f, \"active\": %lu, \"rps\": %.2f, "
               "\"p50_us\": %lu, \"p99_us\": %lu, \"p999_us\": %lu",
               i ? "," : "", r->t, r->open_active, r->rps, r->p50_us, r->p99_us, r->p999_us);
        if (t->num_idle > 0)
            printf(", \"idle_open\": %lu, \"idle_opened\": %lu, \"idle_answered\": %lu, "
                   "\"idle_closed\": %lu, \"idle_failed\": %lu",
                   r->open_idle, r->opened, r->answered, r->closed, r->failed);
        printf("}");
    }
    printf("\n  ]");
}
