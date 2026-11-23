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

void stats_print_json(const worker_stats_t *s, double elapsed_sec,
                      int num_templates, int ws_mode,
                      const char *target, int num_connections, int num_threads,
                      int pipeline_depth, int duration_sec, double target_rate,
                      const ramp_result_t *ramp)
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
