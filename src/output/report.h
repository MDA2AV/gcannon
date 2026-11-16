#pragma once

#include <netinet/in.h>
#include <stdint.h>

#include "cli.h"
#include "core/stats.h"

/* What a run prints outside --tui: the settings before it starts, then the
 * results as text or as one JSON object. */

/* The settings banner (not with --json) */
void report_banner(const options_t *o, const char *host, int port, const char *path,
                   int tls, int num_templates,
                   const struct sockaddr_in *addrs, uint64_t interval_ns);

void report_text(const worker_stats_t *s, double elapsed_sec, int num_templates,
                 int ws_mode);

void report_json(const worker_stats_t *s, double elapsed_sec,
                 int num_templates, int ws_mode,
                 const char *target, int num_connections, int num_threads,
                 int pipeline_depth, int duration_sec, double target_rate);

/* --rate only: target vs achieved rate, schedule lag, connections still open */
void report_rate(const worker_stats_t *s, double elapsed_sec,
                 double target_rate, int num_connections);
