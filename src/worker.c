#include "worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <fcntl.h>

/* io_uring implementation inspired by zerg - https://github.com/MDA2AV/zerg */

static inline struct io_uring_sqe *sqe_get(struct io_uring *ring)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (__builtin_expect(!sqe, 0)) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

static inline void return_buffer(worker_t *w, uint16_t bid)
{
    uint8_t *addr = w->buf_slab + (size_t)bid * RECV_BUF_SIZE;
    io_uring_buf_ring_add(w->buf_ring, addr, RECV_BUF_SIZE,
                          bid, w->buf_mask, w->buf_index++);
    io_uring_buf_ring_advance(w->buf_ring, 1);
}

static inline void arm_recv_multishot(worker_t *w, const int conn_idx)
{
    const gc_conn_t *c = &w->conns[conn_idx];
    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_recv_multishot(sqe, c->fd, NULL, 0, 0);
    sqe->flags    |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BUFFER_RING_BGID;
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_RECV, c->gen, conn_idx));
}

static inline void fire_ws_upgrade(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    if (c->send_inflight) return;
    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_send(sqe, c->fd, w->ws_upgrade_buf, w->ws_upgrade_len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_SEND, c->gen, conn_idx));
    c->send_inflight = 1;
    c->send_total = w->ws_upgrade_len;
    c->send_done  = 0;
}

static inline void fire_requests(worker_t *w, gc_conn_t *c, int conn_idx, int count)
{
    if (count <= 0 || c->send_inflight || c->state != CONN_ACTIVE)
        return;

    if (count > w->pipeline_depth)
        count = w->pipeline_depth;

    /* Cap to remaining requests on this connection */
    if (w->requests_per_conn > 0) {
        const int remaining = w->requests_per_conn - c->requests_sent;
        if (remaining <= 0) return;
        if (count > remaining) count = remaining;
    }

    const char *buf;
    int send_len;

    if (w->ws_mode) {
        send_len = count * w->ws_frame_len;
        buf = (const char *)w->ws_frame_buf;
    } else {
        const request_tpl_t *tpl = &w->templates[c->tpl_idx];
        buf = tpl->pipeline_buf;
        send_len = count * tpl->request_len;
    }

    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_send(sqe, c->fd, buf, send_len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_SEND, c->gen, conn_idx));

    /* All requests in this batch go out in one send, so they share
       the same dispatch timestamp.  Latency = time from batch dispatch
       to individual response arrival. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int k = 0; k < count; k++) {
        c->send_times[c->send_time_tail % PIPELINE_DEPTH_MAX] = now;
        c->send_time_tail++;
    }

    c->send_inflight = 1;
    c->send_total = send_len;
    c->send_done  = 0;
    c->pipeline_inflight += count;
    c->requests_sent += count;
    w->stats.requests += count;
}

static inline uint64_t timespec_diff_us(const struct timespec *start, const struct timespec *end)
{
    int64_t sec  = end->tv_sec  - start->tv_sec;
    int64_t nsec = end->tv_nsec - start->tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    return (uint64_t)(sec * 1000000L + nsec / 1000L);
}

static int create_connect_socket(int linger_rst)
{
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (linger_rst) {
        const struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }
    return fd;
}

static void start_connect(worker_t *w, const int conn_idx)
{
    gc_conn_t *c = &w->conns[conn_idx];
    const int fd = create_connect_socket(w->requests_per_conn > 0);
    if (fd < 0) {
        w->stats.connect_errors++;
        return;
    }

    c->fd = fd;
    c->gen++;
    c->state = CONN_CONNECTING;
    if (c->gen == 1) {
        /* First connect — distribute evenly across templates */
        c->tpl_idx = (w->conn_offset + conn_idx) % w->num_templates;
    } else {
        /* Reconnect — rotate to next template */
        c->tpl_idx = (c->tpl_idx + 1) % w->num_templates;
    }
    c->pipeline_inflight = 0;
    c->send_inflight = 0;
    c->send_total = 0;
    c->send_done = 0;
    c->send_time_head = 0;
    c->send_time_tail = 0;
    c->requests_sent = 0;
    c->responses_recv = 0;
    http_parser_reset(&c->parser);
    ws_parser_reset(&c->ws_parser);

    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_connect(sqe, fd, (struct sockaddr *)&w->server_addr,
                          sizeof(w->server_addr));
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_CONNECT, c->gen, conn_idx));
}

static void close_conn(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    if (c->fd >= 0) {
        struct io_uring_sqe *sqe = sqe_get(&w->ring);
        io_uring_prep_cancel64(sqe, PACK_UD(UD_RECV, c->gen, conn_idx), 0);
        io_uring_sqe_set_data64(sqe, PACK_UD(UD_CANCEL, c->gen, conn_idx));
        close(c->fd);
    }
    c->fd = -1;
    c->state = CONN_CLOSED;
    c->send_inflight = 0;
    c->pipeline_inflight = 0;
}

static void reconnect(worker_t *w, const int conn_idx)
{
    close_conn(w, &w->conns[conn_idx], conn_idx);
    w->stats.reconnects++;
    if (*w->running)
        start_connect(w, conn_idx);
}


/* ── init ──────────────────────────────────────────────────────────── */

void worker_init(worker_t *w,
                 const int id,
                 const struct sockaddr_in *addr,
                 request_tpl_t *templates,
                 const int num_templates,
                 const int pipeline_depth,
                 const int num_conns,
                 const int conn_offset,
                 const int requests_per_conn,
                 const int expected_status,
                 const int ws_mode,
                 const char *ws_host,
                 const int ws_port,
                 const char *ws_path,
                 const uint8_t *ws_payload,
                 const int ws_payload_len,
                 const int cqe_latency,
                 const int per_tpl_latency,
                 volatile int *running)
{
    memset(w, 0, sizeof(*w));

    if (pipeline_depth > PIPELINE_DEPTH_MAX) {
        fprintf(stderr, "[w%d] pipeline_depth %d exceeds PIPELINE_DEPTH_MAX (%d)\n",
                id, pipeline_depth, PIPELINE_DEPTH_MAX);
        exit(1);
    }

    w->id                = id;
    w->server_addr       = *addr;
    w->templates         = templates;
    w->num_templates     = num_templates;
    w->pipeline_depth    = pipeline_depth;
    w->num_conns         = num_conns;
    w->conn_offset       = conn_offset;
    w->requests_per_conn = requests_per_conn;
    w->expected_status   = expected_status;
    w->ws_mode           = ws_mode;
    w->cqe_latency       = cqe_latency;
    w->per_tpl_latency   = per_tpl_latency;
    w->running           = running;

    if (per_tpl_latency && num_templates > 1) {
        w->stats.tpl_latency = calloc(num_templates, sizeof(latency_hist_t));
        w->stats.num_tpl_latency = num_templates;
    }

    if (ws_mode && ws_host) {
        /* Build upgrade request */
        w->ws_upgrade_buf = malloc(1024);
        if (!w->ws_upgrade_buf) {
            fprintf(stderr, "[w%d] malloc: out of memory\n", id);
            exit(1);
        }
        w->ws_upgrade_len = ws_build_upgrade_request(w->ws_upgrade_buf, 1024,
                                                      ws_host, ws_port, ws_path);
        /* Build pipelined WS frames: pipeline_depth copies */
        uint8_t single_frame[256];
        const int frame_len = ws_build_frame(single_frame, ws_payload, ws_payload_len);
        w->ws_frame_len = frame_len;
        w->ws_pipeline_len = frame_len * pipeline_depth;
        w->ws_frame_buf = malloc(w->ws_pipeline_len);
        if (!w->ws_frame_buf) {
            fprintf(stderr, "[w%d] malloc: out of memory\n", id);
            exit(1);
        }
        for (int i = 0; i < pipeline_depth; i++)
            memcpy(w->ws_frame_buf + i * frame_len, single_frame, frame_len);
    }

    /* io_uring */
    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    const int ret = io_uring_queue_init_params(RING_ENTRIES, &w->ring, &params);
    if (ret < 0) {
        fprintf(stderr, "[w%d] io_uring_queue_init: %s\n", id, strerror(-ret));
        exit(1);
    }

    /* Provided buffer ring */
    int bret;
    w->buf_ring = io_uring_setup_buf_ring(&w->ring, BUF_RING_ENTRIES,
                                           BUFFER_RING_BGID, 0, &bret);
    if (!w->buf_ring) {
        fprintf(stderr, "[w%d] setup_buf_ring: %s\n", id, strerror(-bret));
        exit(1);
    }

    w->buf_mask = (uint32_t)(BUF_RING_ENTRIES - 1);
    const size_t slab_size = (size_t)BUF_RING_ENTRIES * RECV_BUF_SIZE;
    if (posix_memalign((void **)&w->buf_slab, 64, slab_size) != 0) {
        fprintf(stderr, "[w%d] posix_memalign: out of memory\n", id);
        exit(1);
    }
    memset(w->buf_slab, 0, slab_size);

    for (int i = 0; i < BUF_RING_ENTRIES; i++) {
        uint8_t *a = w->buf_slab + (size_t)i * RECV_BUF_SIZE;
        io_uring_buf_ring_add(w->buf_ring, a, RECV_BUF_SIZE,
                              i, w->buf_mask, w->buf_index++);
    }
    io_uring_buf_ring_advance(w->buf_ring, BUF_RING_ENTRIES);

    /* Connection array */
    w->conns = calloc(num_conns, sizeof(gc_conn_t));
    if (!w->conns) {
        fprintf(stderr, "[w%d] calloc: out of memory\n", id);
        exit(1);
    }

    /* Start all connections */
    for (int i = 0; i < num_conns; i++) {
        w->conns[i].fd = -1;
        w->conns[i].state = CONN_CLOSED;
        w->conns[i].gen = 0;
        start_connect(w, i);
    }
}

/* ── event loop ────────────────────────────────────────────────────── */

void worker_loop(worker_t *w)
{
    struct io_uring_cqe *cqes[BATCH_CQES];
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */

    for (;;) {

        unsigned got = io_uring_peek_batch_cqe(&w->ring, cqes, BATCH_CQES);
        if (got == 0) {
            /* Nothing pending — if shutting down, exit now */
            if (!*w->running) break;
            struct io_uring_cqe *wait_cqe;
            io_uring_submit_and_wait_timeout(&w->ring, &wait_cqe, 1, &ts, NULL);
            got = io_uring_peek_batch_cqe(&w->ring, cqes, BATCH_CQES);
            if (got == 0) continue;
        }

        for (unsigned i = 0; i < got; i++) {
            const struct io_uring_cqe *cqe = cqes[i];
            const uint64_t   ud       = cqe->user_data;
            const ud_kind_t  kind     = UD_KIND(ud);
            const uint16_t   cqe_gen  = UD_GEN(ud);
            const int        conn_idx = UD_IDX(ud);
            const int        res      = cqe->res;

            if (conn_idx < 0 || conn_idx >= w->num_conns) continue;
            gc_conn_t *c = &w->conns[conn_idx];

            /* Stale CQE from a previous generation — ignore */
            if (cqe_gen != c->gen) {
                /* Still need to return buffer if recv had one */
                if (kind == UD_RECV && (cqe->flags & IORING_CQE_F_BUFFER)) {
                    const uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                    return_buffer(w, bid);
                }
                continue;
            }

            switch (kind) {

            case UD_CONNECT: {
                if (res < 0) {
                    w->stats.connect_errors++;
                    reconnect(w, conn_idx);
                    break;
                }
                arm_recv_multishot(w, conn_idx);
                if (w->ws_mode) {
                    c->state = CONN_WS_UPGRADING;
                    fire_ws_upgrade(w, c, conn_idx);
                } else {
                    c->state = CONN_ACTIVE;
                    fire_requests(w, c, conn_idx, w->pipeline_depth);
                }
                break;
            }

            case UD_SEND: {
                if (res < 0) {
                    c->send_inflight = 0;
                    w->stats.read_errors++;
                    reconnect(w, conn_idx);
                    break;
                }

                c->send_done += res;
                if (c->send_done < c->send_total) {
                    /* Partial send — resubmit remainder */
                    const char *base;
                    if (c->state == CONN_WS_UPGRADING) {
                        base = w->ws_upgrade_buf;
                    } else if (w->ws_mode) {
                        base = (const char *)w->ws_frame_buf;
                    } else {
                        base = w->templates[c->tpl_idx].pipeline_buf;
                    }
                    const int off = c->send_done;
                    struct io_uring_sqe *sqe = sqe_get(&w->ring);
                    io_uring_prep_send(sqe, c->fd,
                                       base + off,
                                       c->send_total - off, MSG_NOSIGNAL);
                    io_uring_sqe_set_data64(sqe, PACK_UD(UD_SEND, c->gen, conn_idx));
                } else {
                    c->send_inflight = 0;

                    /* Refill pipeline gap — RECV may have arrived before SEND
                       in the same batch, consuming responses while send_inflight
                       blocked fire_requests. */
                    if (c->state == CONN_ACTIVE) {
                        int to_refill = w->pipeline_depth - c->pipeline_inflight;
                        if (to_refill > 0)
                            fire_requests(w, c, conn_idx, to_refill);
                    }
                }
                break;
            }

            case UD_RECV: {
                const int has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
                const int has_more   = (cqe->flags & IORING_CQE_F_MORE)   != 0;

                if (res <= 0) {
                    if (has_buffer) {
                        const uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                        return_buffer(w, bid);
                    }
                    reconnect(w, conn_idx);
                    break;
                }

                if (!has_buffer) break;

                const uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                const uint8_t *buf = w->buf_slab + (size_t)bid * RECV_BUF_SIZE;

                w->stats.bytes_read += res;

                int completed = 0;

                if (c->state == CONN_WS_UPGRADING) {
                    /* Parse HTTP upgrade response — look for 101 */
                    completed = http_parse_responses(&c->parser, buf, res);
                    if (completed > 0 && c->parser.completed_statuses[0] == 101) {
                        c->state = CONN_ACTIVE;
                        w->stats.status_2xx++;
                        ws_parser_reset(&c->ws_parser);
                        return_buffer(w, bid);
                        if (!has_more) arm_recv_multishot(w, conn_idx);
                        fire_requests(w, c, conn_idx, w->pipeline_depth);
                        break;
                    } else if (completed > 0) {
                        /* Upgrade rejected — record actual status class */
                        const int sc = c->parser.completed_statuses[0];
                        if      (sc >= 200 && sc < 300) w->stats.status_2xx++;
                        else if (sc >= 300 && sc < 400) w->stats.status_3xx++;
                        else if (sc >= 400 && sc < 500) w->stats.status_4xx++;
                        else if (sc >= 500 && sc < 600) w->stats.status_5xx++;
                        else                            w->stats.status_other++;
                        return_buffer(w, bid);
                        reconnect(w, conn_idx);
                        break;
                    }
                    /* Incomplete upgrade response — wait for more data */
                    return_buffer(w, bid);
                    if (!has_more) arm_recv_multishot(w, conn_idx);
                    break;
                }

                /* In CQE latency mode, timestamp before parsing */
                struct timespec now;
                if (w->cqe_latency)
                    clock_gettime(CLOCK_MONOTONIC, &now);

                if (w->ws_mode) {
                    completed = ws_parse_frames(&c->ws_parser, buf, res);
                } else {
                    completed = http_parse_responses(&c->parser, buf, res);
                }

                /* In default mode, timestamp after parsing */
                if (!w->cqe_latency && completed > 0)
                    clock_gettime(CLOCK_MONOTONIC, &now);

                for (int j = 0; j < completed; j++) {
                    w->stats.responses++;
                    w->stats.tpl_responses[c->tpl_idx]++;
                    if (c->pipeline_inflight > 0)
                        c->pipeline_inflight--;
                    else
                        w->stats.read_errors++;
                    c->responses_recv++;

                    if (w->ws_mode) {
                        w->stats.status_2xx++;
                        w->stats.tpl_responses_2xx[c->tpl_idx]++;
                    } else {
                        /* Track status codes */
                        if (j < c->parser.completed_count) {
                            const int sc = c->parser.completed_statuses[j];
                            if (sc >= 200 && sc < 300) {
                                w->stats.status_2xx++;
                                w->stats.tpl_responses_2xx[c->tpl_idx]++;
                            }
                            else if (sc >= 300 && sc < 400)  w->stats.status_3xx++;
                            else if (sc >= 400 && sc < 500)  w->stats.status_4xx++;
                            else if (sc >= 500 && sc < 600)  w->stats.status_5xx++;
                            else                             w->stats.status_other++;
                        }
                    }

                    if (c->send_time_head < c->send_time_tail) {
                        const struct timespec *ts_start =
                            &c->send_times[c->send_time_head % PIPELINE_DEPTH_MAX];
                        const uint64_t lat = timespec_diff_us(ts_start, &now);
                        stats_record_latency(&w->stats, lat);
                        if (w->stats.tpl_latency)
                            hist_record(&w->stats.tpl_latency[c->tpl_idx], lat);
                        c->send_time_head++;
                    }
                }

                return_buffer(w, bid);

                /* Check if this connection's quota is fully drained */
                const int quota_sent = w->requests_per_conn > 0 &&
                    c->requests_sent >= w->requests_per_conn;
                const int quota_done = quota_sent &&
                    c->responses_recv >= w->requests_per_conn &&
                    c->pipeline_inflight == 0;

                if (quota_done) {
                    reconnect(w, conn_idx);
                } else {
                    if (!has_more)
                        arm_recv_multishot(w, conn_idx);

                    if (completed > 0)
                        fire_requests(w, c, conn_idx, completed);
                }
                break;
            }

            case UD_CANCEL:
                break;
            }
        }

        io_uring_cq_advance(&w->ring, got);
        io_uring_submit(&w->ring);
    }
}

/* ── cleanup ───────────────────────────────────────────────────────── */

void worker_destroy(worker_t *w)
{
    for (int i = 0; i < w->num_conns; i++) {
        if (w->conns[i].fd >= 0)
            close(w->conns[i].fd);
    }

    io_uring_free_buf_ring(&w->ring, w->buf_ring,
                           BUF_RING_ENTRIES, BUFFER_RING_BGID);
    io_uring_queue_exit(&w->ring);
    free(w->buf_slab);
    free(w->conns);
    free(w->stats.tpl_latency);
}
