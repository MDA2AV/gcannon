#pragma once

#include <liburing.h>
#include <netinet/in.h>
#include <time.h>

#include "constants.h"
#include "stats.h"
#include "http.h"
#include "ws.h"

typedef enum { CONN_CONNECTING, CONN_WS_UPGRADING, CONN_ACTIVE, CONN_CLOSED } conn_state_t;

/* Request template — one per raw request file (or the auto-generated GET) */
typedef struct {
    char *pipeline_buf;   /* pipeline_depth copies concatenated */
    int   request_len;    /* single request length */
    int   pipeline_len;   /* pipeline_depth * request_len */
} request_tpl_t;

typedef struct gc_conn {
    int              fd;
    uint16_t         gen;            /* generation counter for stale CQE detection */
    conn_state_t     state;
    int              tpl_idx;        /* which request template this conn uses */
    int              pipeline_inflight;
    http_parser_t    parser;
    ws_parser_t      ws_parser;
    struct timespec  send_times[PIPELINE_DEPTH_MAX];
    int              send_time_head;
    int              send_time_tail;
    int              send_inflight;
    int              send_total;     /* total bytes we intended to send */
    int              send_done;      /* bytes confirmed sent so far */
    int              requests_sent;  /* total requests sent on this connection */
    int              responses_recv; /* total responses received on this connection */
} gc_conn_t;

typedef struct worker {
    struct io_uring           ring;
    struct io_uring_buf_ring *buf_ring;
    uint8_t                  *buf_slab;
    uint32_t                  buf_index;
    uint32_t                  buf_mask;

    gc_conn_t                *conns;
    int                       num_conns;

    struct sockaddr_in        server_addr;
    request_tpl_t            *templates;
    int                       num_templates;
    int                       pipeline_depth;

    int                       requests_per_conn; /* 0 = keep-alive forever */
    int                       expected_status;   /* expected HTTP status code (0 = any) */
    int                       conn_offset;       /* global connection index offset for template assignment */
    int                       ws_mode;           /* 1 = WebSocket echo mode */
    char                     *ws_upgrade_buf;    /* pre-built upgrade request */
    int                       ws_upgrade_len;
    uint8_t                  *ws_frame_buf;      /* pipeline_depth copies of the WS frame */
    int                       ws_frame_len;      /* single frame length */
    int                       ws_pipeline_len;   /* total buffer length */
    worker_stats_t            stats;
    volatile int             *running;
    int                       id;
} worker_t;

/* Initialize worker (must be called from worker thread for SINGLE_ISSUER) */
void worker_init(worker_t *w, int id, const struct sockaddr_in *addr,
                 request_tpl_t *templates, int num_templates, int pipeline_depth,
                 int num_conns, int conn_offset, int requests_per_conn,
                 int expected_status, int ws_mode,
                 const char *ws_host, int ws_port, const char *ws_path,
                 const uint8_t *ws_payload, int ws_payload_len,
                 volatile int *running);

/* Main event loop — blocks until *running == 0 */
void worker_loop(worker_t *w);

/* Cleanup */
void worker_destroy(worker_t *w);
