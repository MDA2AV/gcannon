#pragma once

#include <liburing.h>
#include <netinet/in.h>
#include <time.h>

#include "constants.h"
#include "stats.h"
#include "http.h"

typedef enum { CONN_CONNECTING, CONN_ACTIVE, CONN_CLOSED } conn_state_t;

typedef struct gc_conn {
    int              fd;
    conn_state_t     state;
    int              pipeline_inflight;
    http_parser_t    parser;
    struct timespec  send_times[PIPELINE_DEPTH_MAX];
    int              send_time_head;
    int              send_time_tail;
    int              send_inflight;
    int              send_total;     /* total bytes we intended to send */
    int              send_done;      /* bytes confirmed sent so far */
} gc_conn_t;

typedef struct worker {
    struct io_uring           ring;
    struct io_uring_buf_ring *buf_ring;
    uint8_t                  *buf_slab;
    uint32_t                  buf_index;
    uint32_t                  buf_mask;

    gc_conn_t                *conns;
    int                      *conn_fd_map; /* fd → conn index, sparse */
    int                       num_conns;

    struct sockaddr_in        server_addr;
    char                     *pipeline_buf;
    int                       request_len;   /* single request length */
    int                       pipeline_len;  /* pipeline_depth * request_len */
    int                       pipeline_depth;

    worker_stats_t            stats;
    volatile int             *running;
    int                       id;
} worker_t;

/* Initialize worker (must be called from worker thread for SINGLE_ISSUER) */
void worker_init(worker_t *w, int id, struct sockaddr_in *addr,
                 char *pipeline_buf, int request_len, int pipeline_depth,
                 int num_conns, volatile int *running);

/* Main event loop — blocks until *running == 0 */
void worker_loop(worker_t *w);

/* Cleanup */
void worker_destroy(worker_t *w);
