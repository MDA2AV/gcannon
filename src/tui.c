#include "tui.h"

#include <stdio.h>
#include <string.h>

/* ── ANSI escape codes ────────────────────────────────────────── */

#define RST     "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define GREEN   "\033[32m"
#define CYAN    "\033[36m"
#define YELLOW  "\033[33m"
#define RED     "\033[31m"
#define GRAY    "\033[90m"

/* ── Box-drawing characters (UTF-8) ──────────────────────────── */

#define BOX_H   "\xe2\x94\x80"  /* ─ */
#define BOX_V   "\xe2\x94\x82"  /* │ */
#define BOX_TL  "\xe2\x94\x8c"  /* ┌ */
#define BOX_TR  "\xe2\x94\x90"  /* ┐ */
#define BOX_BL  "\xe2\x94\x94"  /* └ */
#define BOX_BR  "\xe2\x94\x98"  /* ┘ */
#define BOX_LT  "\xe2\x94\x9c"  /* ├ */
#define BOX_RT  "\xe2\x94\xa4"  /* ┤ */
#define BOX_TT  "\xe2\x94\xac"  /* ┬ */
#define BOX_BT  "\xe2\x94\xb4"  /* ┴ */
#define BOX_X   "\xe2\x94\xbc"  /* ┼ */
#define BLOCK   "\xe2\x96\x88"  /* █ */
#define SHADE   "\xe2\x96\x91"  /* ░ */
#define LEQ     "\xe2\x89\xa4"  /* ≤ */

/* Horizontal runs */
#define H10     BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H
#define H13     H10 BOX_H BOX_H BOX_H
#define H14     H10 BOX_H BOX_H BOX_H BOX_H
#define H38     H10 H10 H10 BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H

/* ── Latency table (5 x 10) ─────────────────────────────────── */

#define LTOP    "    " GRAY BOX_TL H10 BOX_TT H10 BOX_TT H10 BOX_TT H10 BOX_TT H10 BOX_TR RST "\n"
#define LMID    "    " GRAY BOX_LT H10 BOX_X  H10 BOX_X  H10 BOX_X  H10 BOX_X  H10 BOX_RT RST "\n"
#define LBOT    "    " GRAY BOX_BL H10 BOX_BT H10 BOX_BT H10 BOX_BT H10 BOX_BT H10 BOX_BR RST "\n"

/* ── Throughput table (14 + 38 = 52) ─────────────────────────── */

#define TTOP    "    " GRAY BOX_TL H14 BOX_TT H38 BOX_TR RST "\n"
#define TMID    "    " GRAY BOX_LT H14 BOX_X  H38 BOX_RT RST "\n"
#define TBOT    "    " GRAY BOX_BL H14 BOX_BT H38 BOX_BR RST "\n"

/* ── Status codes table (4 x 13 = 52) ───────────────────────── */

#define STOP    "    " GRAY BOX_TL H13 BOX_TT H13 BOX_TT H13 BOX_TT H13 BOX_TR RST "\n"
#define SMID    "    " GRAY BOX_LT H13 BOX_X  H13 BOX_X  H13 BOX_X  H13 BOX_RT RST "\n"
#define SBOT    "    " GRAY BOX_BL H13 BOX_BT H13 BOX_BT H13 BOX_BT H13 BOX_BR RST "\n"

/* Separator line used for section dividers */
#define SEPARATOR \
    BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H \
    BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H \
    BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H \
    BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H \
    BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H \
    BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H BOX_H

#define BAR_WIDTH       40
#define PROGRESS_LINES  4
#define GRAPH_WIDTH     40
#define HIST_BAR_WIDTH  30

/* Sparkline block elements (1/8 to 8/8 height) */
static const char *spark[] = {
    "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
    "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88",
};

/* ── formatting helpers (same logic as stats.c, local copies) ── */

static void fmt_latency(char *buf, size_t sz, uint64_t us)
{
    if (us < 1000)
        snprintf(buf, sz, "%luus", us);
    else if (us < 1000000)
        snprintf(buf, sz, "%.2fms", us / 1000.0);
    else
        snprintf(buf, sz, "%.2fs", us / 1000000.0);
}

static void fmt_count(char *buf, size_t sz, uint64_t n)
{
    if (n >= 1000000)
        snprintf(buf, sz, "%.2fM", n / 1000000.0);
    else if (n >= 1000)
        snprintf(buf, sz, "%.2fK", n / 1000.0);
    else
        snprintf(buf, sz, "%lu", n);
}

static void fmt_bytes(char *buf, size_t sz, double bps)
{
    if (bps >= 1073741824.0)
        snprintf(buf, sz, "%.2fGB/s", bps / 1073741824.0);
    else if (bps >= 1048576.0)
        snprintf(buf, sz, "%.2fMB/s", bps / 1048576.0);
    else if (bps >= 1024.0)
        snprintf(buf, sz, "%.2fKB/s", bps / 1024.0);
    else
        snprintf(buf, sz, "%.0fB/s", bps);
}

/* ── progress bar ─────────────────────────────────────────────── */

void tui_progress_init(int duration_sec)
{
    /* Line 1: empty progress bar */
    printf("  " GRAY);
    for (int i = 0; i < BAR_WIDTH; i++)
        printf(SHADE);
    printf(RST "    0%%   0s / %ds\n", duration_sec);
    /* Line 2: placeholder */
    printf("  " DIM "Waiting for data..." RST "\n");
    /* Line 3: empty sparkline */
    printf("  " GRAY "Req/s" RST "\n");
    /* Line 4: blank separator */
    printf("\n");
    fflush(stdout);
}

void tui_progress_update(int elapsed, int duration,
                         uint64_t responses, uint64_t bytes_read,
                         uint64_t prev_resp,
                         const uint64_t *rps_history, int rps_count)
{
    /* Move cursor up to overwrite previous 4 lines */
    printf("\033[%dA", PROGRESS_LINES);

    int pct    = duration > 0 ? (elapsed * 100) / duration : 0;
    int filled = duration > 0 ? (elapsed * BAR_WIDTH) / duration : 0;

    /* Line 1: progress bar */
    printf("\033[2K  " GREEN);
    for (int i = 0; i < filled; i++)
        printf(BLOCK);
    printf(GRAY);
    for (int i = filled; i < BAR_WIDTH; i++)
        printf(SHADE);
    printf(RST "  %3d%%   %ds / %ds\n", pct, elapsed, duration);

    /* Line 2: live stats */
    char rps[32], resp[32], bw[32];
    uint64_t inst_rps = responses - prev_resp;
    fmt_count(rps,  sizeof(rps),  inst_rps);
    fmt_count(resp, sizeof(resp), responses);
    if (elapsed > 0)
        fmt_bytes(bw, sizeof(bw), (double)bytes_read / elapsed);
    else
        snprintf(bw, sizeof(bw), "0B/s");

    printf("\033[2K  " CYAN "%s" RST " req/s  " GRAY BOX_V RST
           "  %s responses  " GRAY BOX_V RST "  %s\n",
           rps, resp, bw);

    /* Line 3: throughput sparkline */
    printf("\033[2K  " GRAY "Req/s " RST);

    if (rps_count > 0) {
        /* Find peak across all history */
        uint64_t peak = 0;
        for (int i = 0; i < rps_count; i++)
            if (rps_history[i] > peak) peak = rps_history[i];
        if (peak == 0) peak = 1;

        /* Show last GRAPH_WIDTH seconds */
        int start = rps_count > GRAPH_WIDTH ? rps_count - GRAPH_WIDTH : 0;
        int shown = rps_count - start;

        printf(GREEN);
        for (int i = start; i < rps_count; i++) {
            int level = (int)(rps_history[i] * 7 / peak);
            if (level > 7) level = 7;
            printf("%s", spark[level]);
        }
        printf(RST);

        /* Pad remaining graph area */
        for (int i = shown; i < GRAPH_WIDTH; i++)
            printf(" ");

        char peak_buf[32];
        fmt_count(peak_buf, sizeof(peak_buf), peak);
        printf("  " GRAY "peak" RST " " CYAN "%s" RST, peak_buf);
    }
    printf("\n");

    /* Line 4: blank */
    printf("\033[2K\n");
    fflush(stdout);
}

/* ── latency histogram ────────────────────────────────────────── */

static uint64_t count_tier1(const worker_stats_t *s, int lo, int hi)
{
    uint64_t c = 0;
    if (lo < 0) lo = 0;
    if (hi > TIER1_BUCKETS) hi = TIER1_BUCKETS;
    for (int i = lo; i < hi; i++)
        c += s->tier1[i];
    return c;
}

static uint64_t count_tier2(const worker_stats_t *s, int lo, int hi)
{
    uint64_t c = 0;
    if (lo < 0) lo = 0;
    if (hi > TIER2_BUCKETS) hi = TIER2_BUCKETS;
    for (int i = lo; i < hi; i++)
        c += s->tier2[i];
    return c;
}

/* Count latency samples in [lo_us, hi_us) */
static uint64_t count_range(const worker_stats_t *s,
                            uint64_t lo_us, uint64_t hi_us)
{
    uint64_t count = 0;

    /* Tier 1: 0 .. TIER1_MAX_US-1 at 1 us resolution */
    if (lo_us < (uint64_t)TIER1_MAX_US) {
        int t1_lo = (int)lo_us;
        int t1_hi = hi_us < (uint64_t)TIER1_MAX_US ? (int)hi_us : TIER1_BUCKETS;
        count += count_tier1(s, t1_lo, t1_hi);
    }

    /* Tier 2: TIER1_MAX_US .. TIER2_MAX_US at TIER2_STEP_US resolution */
    if (hi_us > (uint64_t)TIER1_MAX_US) {
        uint64_t t2_lo = lo_us > (uint64_t)TIER1_MAX_US ? lo_us : (uint64_t)TIER1_MAX_US;
        uint64_t t2_hi = hi_us < (uint64_t)TIER2_MAX_US ? hi_us : (uint64_t)TIER2_MAX_US;
        int idx_lo = (int)((t2_lo - TIER1_MAX_US) / TIER2_STEP_US);
        int idx_hi = (int)((t2_hi - TIER1_MAX_US) / TIER2_STEP_US);
        count += count_tier2(s, idx_lo, idx_hi);
    }

    return count;
}

static void print_histogram(const worker_stats_t *s, int num_buckets)
{
    if (s->latency_count == 0) return;

    if (num_buckets <= 0) num_buckets = 10;
    if (num_buckets > 100) num_buckets = 100;
    const int N = num_buckets;

    /* Find where the data actually lives */
    uint64_t data_lo = stats_percentile(s, 0.0);
    uint64_t data_hi = stats_percentile(s, 0.999);
    if (data_hi <= data_lo)
        data_hi = data_lo + 1;

    uint64_t range = data_hi - data_lo;

    /* N equal-width buckets spanning [data_lo, data_hi] + 1 tail */
    uint64_t b_lo[101], b_hi[101], b_count[101];

    for (int i = 0; i < N; i++) {
        b_lo[i] = data_lo + range * (uint64_t)i / N;
        b_hi[i] = data_lo + range * (uint64_t)(i + 1) / N;
    }
    /* First bucket catches everything from 0 */
    b_lo[0] = 0;
    /* Tail bucket for anything above p99.9 */
    b_lo[N] = b_hi[N - 1];
    b_hi[N] = 0;  /* sentinel = infinity */

    int nb = N + 1;

    /* Count samples per bucket */
    uint64_t max_count = 0;
    for (int i = 0; i < nb; i++) {
        b_count[i] = b_hi[i] == 0
            ? count_range(s, b_lo[i], TIER2_MAX_US) + s->overflow
            : count_range(s, b_lo[i], b_hi[i]);
        if (b_count[i] > max_count)
            max_count = b_count[i];
    }

    uint64_t total = s->latency_count + s->overflow;
    if (total == 0) total = 1;

    for (int i = 0; i < nb; i++) {
        if (b_count[i] == 0) continue;

        double pct = 100.0 * b_count[i] / total;
        int bar_len = max_count > 0
            ? (int)((double)b_count[i] / max_count * HIST_BAR_WIDTH) : 0;

        char val[16], label[32];
        if (b_hi[i] == 0) {
            fmt_latency(val, sizeof(val), b_lo[i]);
            snprintf(label, sizeof(label), "> %8s", val);
        } else {
            fmt_latency(val, sizeof(val), b_hi[i]);
            snprintf(label, sizeof(label), LEQ " %8s", val);
        }

        printf("    %s  " GREEN, label);
        for (int j = 0; j < bar_len; j++)
            printf(BLOCK);
        printf(RST);
        for (int j = bar_len; j < HIST_BAR_WIDTH; j++)
            printf(" ");
        printf("  %6.2f%%\n", pct);
    }
}

/* ── results display ──────────────────────────────────────────── */

void tui_print_results(const worker_stats_t *s, double elapsed_sec,
                       int num_templates, int expected_status, int hist_buckets)
{
    char avg[32], p50[32], p90[32], p99[32], p999[32];
    char rps[32], bw[32], req[32], resp[32];

    uint64_t avg_us = s->latency_count ? s->latency_sum_us / s->latency_count : 0;
    fmt_latency(avg,  sizeof(avg),  avg_us);
    fmt_latency(p50,  sizeof(p50),  stats_percentile(s, 0.50));
    fmt_latency(p90,  sizeof(p90),  stats_percentile(s, 0.90));
    fmt_latency(p99,  sizeof(p99),  stats_percentile(s, 0.99));
    fmt_latency(p999, sizeof(p999), stats_percentile(s, 0.999));

    double rps_val = elapsed_sec > 0 ? s->responses / elapsed_sec : 0;
    fmt_count(rps,  sizeof(rps),  (uint64_t)rps_val);
    fmt_bytes(bw,   sizeof(bw),   elapsed_sec > 0 ? s->bytes_read / elapsed_sec : 0);
    fmt_count(req,  sizeof(req),  s->requests);
    fmt_count(resp, sizeof(resp), s->responses);

    /* ── title ── */
    printf("\n");
    printf("  " BOLD "Glass Cannon" RST GRAY " " BOX_H BOX_H " " RST BOLD "Results" RST "\n");
    printf("  " GRAY SEPARATOR RST "\n");

    /* ── latency table ── */
    printf("\n");
    printf("  " BOLD "Latency" RST "\n");
    printf("\n");
    printf(LTOP);
    printf("    " GRAY BOX_V RST BOLD "   Avg    " RST
           GRAY BOX_V RST BOLD "   p50    " RST
           GRAY BOX_V RST BOLD "   p90    " RST
           GRAY BOX_V RST BOLD "   p99    " RST
           GRAY BOX_V RST BOLD "  p99.9   " RST
           GRAY BOX_V RST "\n");
    printf(LMID);
    printf("    " GRAY BOX_V RST CYAN " %8s " RST
           GRAY BOX_V RST CYAN " %8s " RST
           GRAY BOX_V RST CYAN " %8s " RST
           GRAY BOX_V RST YELLOW " %8s " RST
           GRAY BOX_V RST RED " %8s " RST
           GRAY BOX_V RST "\n",
           avg, p50, p90, p99, p999);
    printf(LBOT);

    /* ── throughput table ── */
    printf("\n");
    printf("  " BOLD "Throughput" RST "\n");
    printf("\n");

    char dur[96];
    snprintf(dur, sizeof(dur), "%s req / %s resp in %.2fs", req, resp, elapsed_sec);

    printf(TTOP);
    printf("    " GRAY BOX_V RST BOLD "    Req/s     " RST GRAY BOX_V RST CYAN " %-36s " RST GRAY BOX_V RST "\n", rps);
    printf(TMID);
    printf("    " GRAY BOX_V RST BOLD "  Bandwidth   " RST GRAY BOX_V RST CYAN " %-36s " RST GRAY BOX_V RST "\n", bw);
    printf(TMID);
    printf("    " GRAY BOX_V RST BOLD "    Total     " RST GRAY BOX_V RST " %-36s " GRAY BOX_V RST "\n", dur);
    printf(TBOT);

    /* ── status codes table ── */
    printf("\n");
    printf("  " BOLD "Status Codes" RST "\n");
    printf("\n");
    printf(STOP);
    printf("    " GRAY BOX_V RST GREEN  "     2xx     " RST
           GRAY BOX_V RST CYAN   "     3xx     " RST
           GRAY BOX_V RST YELLOW "     4xx     " RST
           GRAY BOX_V RST RED    "     5xx     " RST
           GRAY BOX_V RST "\n");
    printf(SMID);
    printf("    " GRAY BOX_V RST " %11lu " GRAY BOX_V RST " %11lu " GRAY BOX_V RST " %11lu " GRAY BOX_V RST " %11lu " GRAY BOX_V RST "\n",
           s->status_2xx, s->status_3xx, s->status_4xx, s->status_5xx);
    if (s->status_other) {
        printf(SMID);
        printf("    " GRAY BOX_V RST "   other: %-3lu" GRAY BOX_V RST "             " GRAY BOX_V RST "             " GRAY BOX_V RST "             " GRAY BOX_V RST "\n",
               s->status_other);
    }
    printf(SBOT);

    /* ── latency histogram ── */
    printf("\n");
    printf("  " BOLD "Latency Distribution" RST "\n");
    printf("\n");
    print_histogram(s, hist_buckets);

    /* ── extra info ── */
    printf("\n");
    printf("  " GRAY "Latency samples: %lu / %lu (%.1f%%)" RST "\n",
           s->latency_count, s->responses,
           s->responses ? 100.0 * s->latency_count / s->responses : 0.0);
    if (s->overflow)
        printf("  " YELLOW "Latency overflow (>5s): %u" RST "\n", s->overflow);
    if (s->reconnects)
        printf("  " GRAY "Reconnects: %lu" RST "\n", s->reconnects);
    if (s->connect_errors || s->read_errors || s->timeouts)
        printf("  " RED "Errors: connect %lu, read %lu, timeout %lu" RST "\n",
               s->connect_errors, s->read_errors, s->timeouts);

    /* ── per-template ── */
    if (num_templates > 1) {
        printf("  " GRAY "Per-template: ");
        for (int i = 0; i < num_templates; i++) {
            if (i > 0) printf(", ");
            printf("%lu", s->tpl_responses[i]);
        }
        printf(RST "\n");
        printf("  " GRAY "Per-template-ok: ");
        for (int i = 0; i < num_templates; i++) {
            if (i > 0) printf(", ");
            printf("%lu", s->tpl_responses_2xx[i]);
        }
        printf(RST "\n");
    }

    /* ── per-template latency ── */
    if (s->tpl_latency && num_templates > 1) {
        printf("\n");
        printf("  " BOLD "Per-Template Latency" RST "\n");
        printf("\n");
        printf(LTOP);
        printf("    " GRAY BOX_V RST BOLD "   Tpl    " RST
               GRAY BOX_V RST BOLD "   Avg    " RST
               GRAY BOX_V RST BOLD "   p50    " RST
               GRAY BOX_V RST BOLD "   p99    " RST
               GRAY BOX_V RST BOLD "  p99.9   " RST
               GRAY BOX_V RST "\n");
        printf(LMID);
        for (int i = 0; i < s->num_tpl_latency && i < num_templates; i++) {
            const latency_hist_t *h = &s->tpl_latency[i];
            if (h->count == 0) continue;
            char a[32], p5[32], p99b[32], p999b[32];
            char label[11];
            snprintf(label, sizeof(label), "#%d", i);
            fmt_latency(a,     sizeof(a),     h->count ? h->sum_us / h->count : 0);
            fmt_latency(p5,    sizeof(p5),    hist_percentile(h, 0.50));
            fmt_latency(p99b,  sizeof(p99b),  hist_percentile(h, 0.99));
            fmt_latency(p999b, sizeof(p999b), hist_percentile(h, 0.999));
            printf("    " GRAY BOX_V RST BOLD " %8s " RST
                   GRAY BOX_V RST CYAN " %8s " RST
                   GRAY BOX_V RST CYAN " %8s " RST
                   GRAY BOX_V RST YELLOW " %8s " RST
                   GRAY BOX_V RST RED " %8s " RST
                   GRAY BOX_V RST "\n",
                   label, a, p5, p99b, p999b);
            if (i < s->num_tpl_latency - 1) printf(LMID);
        }
        printf(LBOT);
    }

    /* ── expected status warning ── */
    uint64_t expected_count = 0;
    if (expected_status >= 200 && expected_status < 300)      expected_count = s->status_2xx;
    else if (expected_status >= 300 && expected_status < 400)  expected_count = s->status_3xx;
    else if (expected_status >= 400 && expected_status < 500)  expected_count = s->status_4xx;
    else if (expected_status >= 500 && expected_status < 600)  expected_count = s->status_5xx;

    uint64_t unexpected = s->responses - expected_count;
    if (unexpected > 0 && s->responses > 0) {
        printf("\n  " RED BOLD "WARNING" RST RED ": %lu/%lu responses (%.1f%%) "
               "had unexpected status (expected %dxx)" RST "\n",
               unexpected, s->responses,
               100.0 * unexpected / s->responses, expected_status / 100);
    }

    printf("\n  " GRAY SEPARATOR RST "\n\n");
}
