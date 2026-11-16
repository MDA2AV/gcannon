#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/constants.h"

/* ── URL parser ────────────────────────────────────────────────────── */

int cli_parse_url(const char *url, char *host, int host_sz,
                  int *port, char *path, int path_sz, int *tls)
{
    const char *hp;
    if (strncmp(url, "http://", 7) == 0) {
        hp = url + 7;
        *tls = 0;
    } else if (strncmp(url, "https://", 8) == 0) {
        hp = url + 8;
        *tls = 1;
    } else {
        fprintf(stderr, "Error: only http:// and https:// URLs supported\n");
        return -1;
    }
    const char *slash = strchr(hp, '/');
    const char *colon = strchr(hp, ':');

    if (slash) {
        snprintf(path, path_sz, "%s", slash);
    } else {
        snprintf(path, path_sz, "/");
        slash = hp + strlen(hp);
    }

    if (colon && colon < slash) {
        int hlen = (int)(colon - hp);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host, hp, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        int hlen = (int)(slash - hp);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host, hp, hlen);
        host[hlen] = '\0';
        *port = *tls ? 443 : 80;
    }
    return 0;
}

static int parse_duration(const char *s)
{
    const int val = atoi(s);
    const int len = strlen(s);
    if (len > 0 && (s[len-1] == 's' || s[len-1] == 'S'))
        return val;
    if (len > 0 && (s[len-1] == 'm' || s[len-1] == 'M'))
        return val * 60;
    return val;
}

/* ── options ───────────────────────────────────────────────────────── */

cli_action_t cli_parse(int argc, char **argv, options_t *o)
{
    *o = (options_t){
        .num_connections   = 100,
        .num_threads       = 1,
        .duration_sec      = 10,
        .pipeline_depth    = 1,
        .requests_per_conn = 0,      /* 0 = keep-alive forever */
        .expected_status   = 200,    /* expected HTTP status code */
        .recv_buf_size     = RECV_BUF_SIZE_DEFAULT,
        .hist_buckets      = 0,      /* 0 = default (10) */
        .rate              = 0,      /* --rate: total req/s; 0 = closed loop */
        .addr_span         = 1,      /* --addr-span: consecutive target addresses */
        .connect_limit     = 0,      /* --connect-limit: first connects in flight per thread */
        .warmup_max_sec    = WARMUP_MAX_MS / 1000,
        .ws_message        = "hello",
    };

    /* Parse CLI args */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            o->url = argv[i];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            o->num_connections = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            o->num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            o->duration_sec = parse_duration(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            o->pipeline_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            o->requests_per_conn = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            o->expected_status = atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--raw") == 0 || strcmp(argv[i], "-R") == 0) && i + 1 < argc) {
            o->raw_files = argv[++i];
        } else if (strcmp(argv[i], "--ws") == 0) {
            o->ws_mode = 1;
        } else if (strcmp(argv[i], "--ws-msg") == 0 && i + 1 < argc) {
            o->ws_message = argv[++i];
            o->ws_mode = 1;
        } else if (strcmp(argv[i], "--tui") == 0) {
            o->tui_mode = 1;
        } else if (strcmp(argv[i], "--json") == 0) {
            o->json_mode = 1;
        } else if ((strcmp(argv[i], "--buckets") == 0 || strcmp(argv[i], "-b") == 0) && i + 1 < argc) {
            o->hist_buckets = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cqe-latency") == 0) {
            o->cqe_latency = 1;
        } else if (strcmp(argv[i], "--recv-buf") == 0 && i + 1 < argc) {
            o->recv_buf_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--per-tpl-latency") == 0) {
            o->per_tpl_latency = 1;
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            o->rate = atof(argv[++i]);
            if (o->rate <= 0) {
                fprintf(stderr, "Error: --rate must be a positive number of requests/s\n");
                return CLI_ERROR;
            }
        } else if (strcmp(argv[i], "--addr-span") == 0 && i + 1 < argc) {
            o->addr_span = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--connect-limit") == 0 && i + 1 < argc) {
            o->connect_limit = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup-max") == 0 && i + 1 < argc) {
            o->warmup_max_sec = parse_duration(argv[++i]);
        } else if (strcmp(argv[i], "--version") == 0) {
            return CLI_VERSION;
        } else if (strcmp(argv[i], "--clear-history") == 0) {
            return CLI_CLEAR_HISTORY;
        } else {
            fprintf(stderr, "Usage: gcannon <url> -c <conns> -t <threads> "
                            "-d <duration> [-p <pipeline>] [-r <req/conn>] "
                            "[-s <status>] [-R|--raw file1,file2,...] "
                            "[--ws [--ws-msg <message>]] [--tui] [--json] [-b <buckets>] "
                            "[--recv-buf <bytes>] [--cqe-latency] [--rate <req/s>] "
                            "[--addr-span <n>] [--connect-limit <n>] "
                            "[--warmup-max <duration>] [--version]\n");
            return CLI_ERROR;
        }
    }

    if (!o->url && !o->raw_files) {
        fprintf(stderr, "Usage: gcannon <url> -c <conns> -t <threads> "
                        "-d <duration> [-p <pipeline>] [-r <req/conn>] "
                        "[-R|--raw file1,file2,...] [--tui] [--json]\n");
        return CLI_ERROR;
    }

    /* Validate counts before anything derives from them. num_threads in
       particular is a divisor below, so -t 0 used to die with SIGFPE. */
    if (o->num_threads < 1) {
        fprintf(stderr, "Error: -t must be at least 1 (got %d)\n", o->num_threads);
        return CLI_ERROR;
    }
    if (o->num_connections < 1) {
        fprintf(stderr, "Error: -c must be at least 1 (got %d)\n", o->num_connections);
        return CLI_ERROR;
    }
    if (o->pipeline_depth < 1) {
        fprintf(stderr, "Error: -p must be at least 1 (got %d)\n", o->pipeline_depth);
        return CLI_ERROR;
    }
    if (o->duration_sec < 1) {
        fprintf(stderr, "Error: -d must be at least 1 second (got %d)\n", o->duration_sec);
        return CLI_ERROR;
    }
    if (o->requests_per_conn < 0) {
        fprintf(stderr, "Error: -r must not be negative (got %d)\n", o->requests_per_conn);
        return CLI_ERROR;
    }
    if (o->num_connections < o->num_threads) {
        /* Threads past the remainder would get zero connections and spin in
           the event loop doing nothing, quietly producing a slower run than
           the flags suggest. */
        fprintf(stderr, "Error: -c (%d) must be at least -t (%d); every thread "
                        "needs at least one connection\n",
                o->num_connections, o->num_threads);
        return CLI_ERROR;
    }

    /* --rate paces requests one at a time per connection: a pipeline would
       send several on one schedule slot, and a per-connection quota would end
       a connection's schedule early. */
    if (o->rate > 0 && o->pipeline_depth != 1) {
        fprintf(stderr, "Error: --rate needs -p 1 (got -p %d)\n", o->pipeline_depth);
        return CLI_ERROR;
    }
    if (o->rate > 0 && o->requests_per_conn > 0) {
        fprintf(stderr, "Error: --rate cannot be combined with -r\n");
        return CLI_ERROR;
    }
    if (o->addr_span < 1 || o->addr_span > 65536) {
        fprintf(stderr, "Error: --addr-span must be 1..65536 (got %d)\n", o->addr_span);
        return CLI_ERROR;
    }
    if (o->connect_limit < 0) {
        fprintf(stderr, "Error: --connect-limit must not be negative (got %d)\n",
                o->connect_limit);
        return CLI_ERROR;
    }
    if (o->warmup_max_sec < 1) {
        fprintf(stderr, "Error: --warmup-max must be at least 1 second\n");
        return CLI_ERROR;
    }

    if (o->pipeline_depth > PIPELINE_DEPTH_MAX)
        o->pipeline_depth = PIPELINE_DEPTH_MAX;

    if (o->recv_buf_size < 512) {
        fprintf(stderr, "Error: --recv-buf must be at least 512\n");
        return CLI_ERROR;
    }
    if (o->recv_buf_size > 4 * 1024 * 1024) {
        /* Each worker pre-allocates BUF_RING_ENTRIES buffers of this size, so an
           unbounded value (e.g. --recv-buf 1000000000) asks for hundreds of GB
           per worker. Reject obviously-wrong sizes up front. */
        fprintf(stderr, "Error: --recv-buf must be at most %d (each worker "
                        "pre-allocates %d buffers of this size)\n",
                        4 * 1024 * 1024, BUF_RING_ENTRIES);
        return CLI_ERROR;
    }

    return CLI_RUN;
}
