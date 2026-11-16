#pragma once

/* The command line: every option, its default, and the checks gcannon makes
 * before anything connects. */

typedef struct options {
    const char *url;               /* NULL: the target is the first --raw file's Host */
    const char *raw_files;         /* --raw/-R, comma-separated; NULL = GET the URL */
    int         num_connections;   /* -c, total over all threads */
    int         num_threads;       /* -t */
    int         duration_sec;      /* -d */
    int         pipeline_depth;    /* -p, capped at PIPELINE_DEPTH_MAX */
    int         requests_per_conn; /* -r, 0 = keep-alive forever */
    int         expected_status;   /* -s */
    int         ws_mode;           /* --ws (or --ws-msg) */
    const char *ws_message;        /* --ws-msg */
    int         tui_mode;          /* --tui */
    int         json_mode;         /* --json */
    int         hist_buckets;      /* -b/--buckets, 0 = default (10) */
    int         recv_buf_size;     /* --recv-buf */
    int         cqe_latency;       /* --cqe-latency */
    int         per_tpl_latency;   /* --per-tpl-latency */
    double      rate;              /* --rate: total req/s; 0 = closed loop */
    int         addr_span;         /* --addr-span: consecutive target addresses */
    int         connect_limit;     /* --connect-limit: first connects in flight
                                      per thread; 0 = all at once */
    int         warmup_max_sec;    /* --warmup-max */
} options_t;

typedef enum {
    CLI_RUN,             /* the options are valid: run */
    CLI_ERROR,           /* already explained on stderr: exit with code 1 */
    CLI_VERSION,         /* --version: print the version and exit */
    CLI_CLEAR_HISTORY,   /* --clear-history: clear the run history and exit */
} cli_action_t;

/* Read argv into *o, with the defaults for anything not given, and check it */
cli_action_t cli_parse(int argc, char **argv, options_t *o);

/* Split an http:// or https:// URL into host, port (80 or 443 unless given)
 * and path ("/" unless given). Returns 0, or -1 after printing why. */
int cli_parse_url(const char *url, char *host, int host_sz,
                  int *port, char *path, int path_sz, int *tls);
