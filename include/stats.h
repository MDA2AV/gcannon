#pragma once

#include <stdint.h>
#include <stdio.h>
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
    uint64_t ws_upgrade_failures;  /* upgrades the server refused; kept out of
                                      status_* because those double as the frame
                                      count in WebSocket mode */
    uint32_t tier1[TIER1_BUCKETS];
    uint32_t tier2[TIER2_BUCKETS];
    uint32_t overflow;
    uint64_t latency_count;
    uint64_t latency_sum_us;
    uint64_t tpl_responses[MAX_TEMPLATES];
    uint64_t tpl_responses_2xx[MAX_TEMPLATES];
    latency_hist_t *tpl_latency;   /* per-template histograms (NULL if disabled) */
    int             num_tpl_latency;
    /* --rate only. A paced request is sent at its scheduled time or as soon
       after it as the connection is free; its latency counts from the
       schedule, so a send that ran late carries the delay in its latency. */
    uint64_t paced_sends;          /* requests sent on a schedule */
    uint64_t paced_late;           /* ...more than 1 ms after it */
    uint64_t paced_lag_max_us;     /* the largest schedule-to-send gap */
    uint64_t conns_active;         /* connections open when the worker stopped
                                      (--ramp-p99: when the step's window closed) */
    /* --ramp-p99 only, counted as a step's window closes */
    uint64_t conns_dead;           /* opened connections left without a socket:
                                      socket() failed, and they never retry */
    uint64_t conns_silent;         /* connections still waiting, past the
                                      threshold, on a request sent before the
                                      window opened: no response all window */
} worker_stats_t;

/* --idle: connections that send one request and are then left alone. Counted
   apart from worker_stats_t, whose figures describe the active connections
   only. */
typedef struct idle_counts {
    uint64_t opened;          /* up, and its one request sent */
    uint64_t answered;        /* ...and answered: parked from here on */
    uint64_t open_now;        /* parked, and not closed yet */
    uint64_t closed;          /* parked, then closed by the server */
    uint64_t failed;          /* never answered: could not connect, or the
                                 server closed it before the response */
    uint64_t connect_errors;  /* ...of which could not connect (or handshake) */
    uint64_t read_errors;     /* ...of which failed reading or parsing it */
    uint64_t bad_status;      /* answered outside the expected class (-s) */
    uint64_t unsolicited;     /* parked connections the server sent data to */
} idle_counts_t;

#define IDLE_AGE_STEP_US  100000   /* how long a parked connection stayed open
                                      before the server closed it: 100 ms... */
#define IDLE_AGE_BUCKETS  36000    /* ...buckets, up to an hour */

typedef struct idle_stats {
    idle_counts_t  c;
    uint64_t       age_count, age_min_us, age_max_us;
    uint32_t       age_hist[IDLE_AGE_BUCKETS];
    uint32_t       age_overflow;
    latency_hist_t first;        /* request sent to response, of the one request */
} idle_stats_t;

/* --ramp-up/--idle: one line of the table over time */
typedef struct timeline_row {
    double   t;                  /* when the interval ended, from the window opening */
    double   elapsed;
    double   rps;                /* active connections' responses per second */
    uint64_t p50_us, p99_us, p999_us;   /* ...and their latency, in the interval */
    uint64_t open_active;        /* connections open as it ended: the active... */
    uint64_t open_idle;          /* ...and the parked ones */
    uint64_t opened, answered, closed, failed;   /* idle, so far */
} timeline_row_t;

/* A stretch of the run: its length, and the active connections' figures in it */
typedef struct timeline_phase {
    double   sec;
    double   rps;
    uint64_t p50_us, p99_us, p999_us;
} timeline_phase_t;

typedef struct timeline {
    int               total_conns;   /* -c */
    int               num_active;    /* ...of which active */
    int               num_idle;      /* ...and idle (0 without --idle) */
    int               floor;         /* active connections opened in the warmup */
    int               ramp_sec;      /* --ramp-up; 0 = none */
    idle_stats_t     *merged;        /* --idle: every worker's, summed */
    uint64_t          pending;       /* idle: neither opened nor failed by the end */
    timeline_row_t   *rows;
    int               num_rows;
    int               has_ramp, has_hold;
    timeline_phase_t  ramp, hold;    /* while the connections open, and after */
} timeline_t;

/* --ramp-p99: how a step's window went. A step passes on RAMP_OK; anything
   else fails it, checked in this order. The last two only end the ramp. */
typedef enum {
    RAMP_OK = 0,
    RAMP_SOCKETS,       /* connections that could not get a socket */
    RAMP_SETUP,         /* connections not set up within --warmup-max */
    RAMP_RESPONSES,     /* no responses at all */
    RAMP_SILENT,        /* connections with no response all window */
    RAMP_STATUS,        /* responses with an unexpected status */
    RAMP_ERRORS,        /* connect or read errors, refused WebSocket upgrades */
    RAMP_P99,           /* p99 over the threshold */
    RAMP_CEILING,       /* every step passed, up to -c */
    RAMP_INTERRUPTED,   /* Ctrl-C */
} ramp_verdict_t;

typedef struct ramp_step {
    int            connections;   /* the step's connection count */
    int            set_up;        /* ...of which settled before the window opened */
    int            retry;         /* the second window of a step whose first failed */
    ramp_verdict_t verdict;
    double         elapsed_sec;
    uint64_t       responses;
    uint64_t       samples;
    uint64_t       p50_us, p90_us, p99_us, p999_us;
    uint64_t       active;        /* connections open when the window closed */
    uint64_t       dead;          /* see conns_dead */
    uint64_t       silent;        /* see conns_silent */
    uint64_t       errors;        /* connect + read errors + refused upgrades */
    uint64_t       unexpected;    /* responses outside the expected status class */
} ramp_step_t;

typedef struct ramp_result {
    uint64_t       p99_max_us;    /* the threshold */
    ramp_step_t   *steps;         /* every window measured, in order */
    int            num_steps;
    int            best;          /* the last step that passed, -1 if none did */
    ramp_verdict_t stop;          /* why the ramp ended */
} ramp_result_t;

void stats_record_latency(worker_stats_t *s, uint64_t latency_us);
void hist_record(latency_hist_t *h, uint64_t latency_us);
void hist_merge(latency_hist_t *dst, const latency_hist_t *src);
uint64_t hist_percentile(const latency_hist_t *h, double pct);
void stats_merge(worker_stats_t *dst, const worker_stats_t *src);
/* Zero s, keeping its per-template histograms (zeroed too) */
void stats_clear(worker_stats_t *s);
/* Copy src into dst, per-template histograms included, into dst's own */
void stats_copy(worker_stats_t *dst, const worker_stats_t *src);
uint64_t stats_percentile(const worker_stats_t *s, double pct);
void stats_print(const worker_stats_t *s, double elapsed_sec, int num_templates,
                 int ws_mode);
/* ramp is NULL except for --ramp-p99, whose steps it adds; timeline likewise
   for --ramp-up and --idle */
void stats_print_json(const worker_stats_t *s, double elapsed_sec,
                      int num_templates, int ws_mode,
                      const char *target, int num_connections, int num_threads,
                      int pipeline_depth, int duration_sec, double target_rate,
                      const ramp_result_t *ramp, const timeline_t *timeline);
/* --rate only: target vs achieved rate, schedule lag, connections still open */
void stats_print_rate(const worker_stats_t *s, double elapsed_sec,
                      double target_rate, int num_connections);

/* --ramp-p99: the plan for the banner, the step table (a header, then one
   line per window as it is measured) and the outcome after the results */
void ramp_print_plan(int start, double factor, int increment, uint64_t p99_max_us);
void ramp_print_header(FILE *out);
void ramp_print_step(FILE *out, const ramp_step_t *st, uint64_t p99_max_us, int again);
void ramp_print_result(const ramp_result_t *r);

/* --ramp-up/--idle: counters, the table over time, and what came of the
   connections */
void idle_age_record(idle_stats_t *s, uint64_t age_us);
uint64_t idle_age_percentile(const idle_stats_t *s, double pct);
void idle_stats_merge(idle_stats_t *dst, const idle_stats_t *src);
void timeline_print_plan(const timeline_t *r);
void timeline_print_header(FILE *out, int has_idle);
void timeline_print_row(FILE *out, const timeline_row_t *row, int has_idle);
void timeline_print_result(const timeline_t *r);
