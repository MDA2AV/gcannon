#include "output/report.h"

#include <stdio.h>
#include <arpa/inet.h>

#include "core/constants.h"

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

void report_banner(const options_t *o, const char *host, const int port, const char *path,
                   const int tls, const int num_templates,
                   const struct sockaddr_in *addrs, const uint64_t interval_ns)
{
    printf("gcannon v%s", GCANNON_VERSION);
    if (o->ws_mode || o->tui_mode || o->cqe_latency || o->per_tpl_latency) {
        printf(" [");
        int first = 1;
        if (o->ws_mode)        { printf("WS");             first = 0; }
        if (o->tui_mode)       { printf("%sTUI",   first ? "" : ", "); first = 0; }
        if (o->cqe_latency)    { printf("%sCQE",   first ? "" : ", "); first = 0; }
        if (o->per_tpl_latency){ printf("%sTPL",   first ? "" : ", "); first = 0; }
        printf("]");
    }
    printf("\n");
    printf("  Target:    %s:%d%s\n", host, port, path);
    if (tls)
        printf("  TLS:       1.3 via kernel TLS (certificate not verified)\n");
    printf("  Threads:   %d\n", o->num_threads);
    printf("  Conns:     %d (%.0f/thread)\n", o->num_connections,
           (double)o->num_connections / o->num_threads);
    printf("  Pipeline:  %d\n", o->pipeline_depth);
    if (o->requests_per_conn > 0)
        printf("  Req/conn:  %d\n", o->requests_per_conn);
    else
        printf("  Req/conn:  unlimited (keep-alive)\n");
    if (num_templates > 1)
        printf("  Templates: %d\n", num_templates);
    printf("  Expected:  %d\n", o->expected_status);
    printf("  Duration:  %ds\n", o->duration_sec);
    if (o->addr_span > 1) {
        char first[INET_ADDRSTRLEN], last[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addrs[0].sin_addr, first, sizeof(first));
        inet_ntop(AF_INET, &addrs[o->addr_span - 1].sin_addr, last, sizeof(last));
        printf("  Addresses: %s - %s (%d)\n", first, last, o->addr_span);
    }
    if (o->rate > 0)
        printf("  Rate:      %.0f req/s, one request per connection every %.3fs\n",
               o->rate, interval_ns / 1e9);
    printf("\n");
}

void report_text(const worker_stats_t *s, double elapsed_sec, int num_templates,
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

void report_json(const worker_stats_t *s, double elapsed_sec,
                 int num_templates, int ws_mode,
                 const char *target, int num_connections, int num_threads,
                 int pipeline_depth, int duration_sec, double target_rate)
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

    printf("\n}\n");
}

void report_rate(const worker_stats_t *s, double elapsed_sec,
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
