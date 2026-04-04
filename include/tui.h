#pragma once

#include "stats.h"
#include "history.h"

/* Print initial empty progress bar (call once before the main loop) */
void tui_progress_init(int duration_sec);

/* Update progress bar — call each second from the main loop.
   prev_responses is used to compute instantaneous req/s.
   rps_history/rps_count provide per-second throughput for the sparkline. */
void tui_progress_update(int elapsed_sec, int duration_sec,
                         uint64_t responses, uint64_t bytes_read,
                         uint64_t prev_responses,
                         const uint64_t *rps_history, int rps_count);

/* Print final benchmark results in TUI format (colors, histogram, etc.)
   hist_buckets controls the number of latency histogram buckets (0 = default 20)
   prev_runs/num_prev_runs: previous run records for comparison table */
void tui_print_results(const worker_stats_t *s, double elapsed_sec,
                       int num_templates, int expected_status, int hist_buckets,
                       const run_record_t *prev_runs, int num_prev_runs,
                       const run_record_t *current_run);
