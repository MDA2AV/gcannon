#include "worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <fcntl.h>

/* ── helpers ───────────────────────────────────────────────────────── */

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

static inline void arm_recv_multishot(worker_t *w, int conn_idx)
{
    gc_conn_t *c = &w->conns[conn_idx];
    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_recv_multishot(sqe, c->fd, NULL, 0, 0);
    sqe->flags    |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BUFFER_RING_BGID;
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_RECV, c->gen, conn_idx));
}

static inline void fire_requests(worker_t *w, gc_conn_t *c, int conn_idx, int count)
{
    if (count <= 0 || c->send_inflight || c->state != CONN_ACTIVE)
        return;

    if (count > w->pipeline_depth)
        count = w->pipeline_depth;

    /* Cap to remaining requests on this connection */
    if (w->requests_per_conn > 0) {
        int remaining = w->requests_per_conn - c->requests_sent;
        if (remaining <= 0) return;
        if (count > remaining) count = remaining;
    }

    request_tpl_t *tpl = &w->templates[c->tpl_idx];
    int send_len = count * tpl->request_len;
    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_send(sqe, c->fd, tpl->pipeline_buf, send_len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_SEND, c->gen, conn_idx));

    /* Record send times at SQE-prep time — this is the true start of the
       round-trip.  Recording later (at SEND_COMPLETE) gives near-zero
       latency when DEFER_TASKRUN batches SEND and RECV CQEs together. */
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

static inline uint64_t timespec_diff_us(struct timespec *start, struct timespec *end)
{
    int64_t sec  = end->tv_sec  - start->tv_sec;
    int64_t nsec = end->tv_nsec - start->tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    return (uint64_t)(sec * 1000000L + nsec / 1000L);
}

static int create_connect_socket(int linger_rst)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (linger_rst) {
        struct linger lg = { .l_onoff = 1, .l_linger = 0 };
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }
    return fd;
}

static void start_connect(worker_t *w, int conn_idx)
{
    gc_conn_t *c = &w->conns[conn_idx];
    int fd = create_connect_socket(w->requests_per_conn > 0);
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

    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_connect(sqe, fd, (struct sockaddr *)&w->server_addr,
                          sizeof(w->server_addr));
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_CONNECT, c->gen, conn_idx));
}

static void close_conn(worker_t *w, gc_conn_t *c, int conn_idx)
{
    if (c->fd >= 0) {
        /* Cancel multishot recv so io_uring drops its socket reference,
           allowing the kernel to fully destroy the socket and free the 4-tuple */
        struct io_uring_sqe *sqe = io_uring_get_sqe(&w->ring);
        if (sqe) {
            io_uring_prep_cancel64(sqe, PACK_UD(UD_RECV, c->gen, conn_idx), 0);
            io_uring_sqe_set_data64(sqe, PACK_UD(UD_CANCEL, c->gen, conn_idx));
        }
        close(c->fd);
    }
    c->fd = -1;
    c->state = CONN_CLOSED;
    c->send_inflight = 0;
    c->pipeline_inflight = 0;
}

static void reconnect(worker_t *w, int conn_idx)
{
    close_conn(w, &w->conns[conn_idx], conn_idx);
    w->stats.reconnects++;
    if (*w->running)
        start_connect(w, conn_idx);
}


/* ── init ──────────────────────────────────────────────────────────── */

void worker_init(worker_t *w, int id, struct sockaddr_in *addr,
                 request_tpl_t *templates, int num_templates, int pipeline_depth,
                 int num_conns, int conn_offset, int requests_per_conn,
                 int expected_status, volatile int *running)
{
    memset(w, 0, sizeof(*w));
    w->id                = id;
    w->server_addr       = *addr;
    w->templates         = templates;
    w->num_templates     = num_templates;
    w->pipeline_depth    = pipeline_depth;
    w->num_conns         = num_conns;
    w->conn_offset       = conn_offset;
    w->requests_per_conn = requests_per_conn;
    w->expected_status   = expected_status;
    w->running           = running;

    /* io_uring */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

    int ret = io_uring_queue_init_params(RING_ENTRIES, &w->ring, &params);
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
    size_t slab_size = (size_t)BUF_RING_ENTRIES * RECV_BUF_SIZE;
    w->buf_slab = aligned_alloc(64, slab_size);
    memset(w->buf_slab, 0, slab_size);

    for (int i = 0; i < BUF_RING_ENTRIES; i++) {
        uint8_t *a = w->buf_slab + (size_t)i * RECV_BUF_SIZE;
        io_uring_buf_ring_add(w->buf_ring, a, RECV_BUF_SIZE,
                              i, w->buf_mask, w->buf_index++);
    }
    io_uring_buf_ring_advance(w->buf_ring, BUF_RING_ENTRIES);

    /* Connection array */
    w->conns = calloc(num_conns, sizeof(gc_conn_t));

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

    while (*w->running) {

        unsigned got = io_uring_peek_batch_cqe(&w->ring, cqes, BATCH_CQES);
        if (got == 0) {
            struct io_uring_cqe *wait_cqe;
            io_uring_submit_and_wait_timeout(&w->ring, &wait_cqe, 1, &ts, NULL);
            got = io_uring_peek_batch_cqe(&w->ring, cqes, BATCH_CQES);
            if (got == 0) continue;
        }

        for (unsigned i = 0; i < got; i++) {
            struct io_uring_cqe *cqe = cqes[i];
            uint64_t   ud       = cqe->user_data;
            ud_kind_t  kind     = UD_KIND(ud);
            uint16_t   cqe_gen  = UD_GEN(ud);
            int        conn_idx = UD_IDX(ud);
            int        res      = cqe->res;

            if (conn_idx < 0 || conn_idx >= w->num_conns) continue;
            gc_conn_t *c = &w->conns[conn_idx];

            /* Stale CQE from a previous generation — ignore */
            if (cqe_gen != c->gen) {
                /* Still need to return buffer if recv had one */
                if (kind == UD_RECV && (cqe->flags & IORING_CQE_F_BUFFER)) {
                    uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
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
                c->state = CONN_ACTIVE;
                arm_recv_multishot(w, conn_idx);
                fire_requests(w, c, conn_idx, w->pipeline_depth);
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
                    request_tpl_t *tpl = &w->templates[c->tpl_idx];
                    int off = c->send_done;
                    struct io_uring_sqe *sqe = sqe_get(&w->ring);
                    io_uring_prep_send(sqe, c->fd,
                                       tpl->pipeline_buf + off,
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
                int has_buffer = (cqe->flags & IORING_CQE_F_BUFFER) != 0;
                int has_more   = (cqe->flags & IORING_CQE_F_MORE)   != 0;

                if (res <= 0) {
                    if (has_buffer) {
                        uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                        return_buffer(w, bid);
                    }
                    reconnect(w, conn_idx);
                    break;
                }

                if (!has_buffer) break;

                uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                uint8_t *buf = w->buf_slab + (size_t)bid * RECV_BUF_SIZE;

                w->stats.bytes_read += res;
                int completed = http_parse_responses(&c->parser, buf, res);

                /* Record latencies */
                struct timespec now;
                if (completed > 0)
                    clock_gettime(CLOCK_MONOTONIC, &now);

                for (int j = 0; j < completed; j++) {
                    w->stats.responses++;
                    w->stats.tpl_responses[c->tpl_idx]++;
                    c->pipeline_inflight--;
                    c->responses_recv++;

                    /* Track status codes */
                    if (j < c->parser.completed_count) {
                        int sc = c->parser.completed_statuses[j];
                        if (sc >= 200 && sc < 300) {
                            w->stats.status_2xx++;
                            w->stats.tpl_responses_2xx[c->tpl_idx]++;
                        }
                        else if (sc >= 300 && sc < 400)  w->stats.status_3xx++;
                        else if (sc >= 400 && sc < 500)  w->stats.status_4xx++;
                        else if (sc >= 500 && sc < 600)  w->stats.status_5xx++;
                        else                             w->stats.status_other++;
                    }

                    if (c->send_time_head < c->send_time_tail) {
                        struct timespec *ts_start =
                            &c->send_times[c->send_time_head % PIPELINE_DEPTH_MAX];
                        uint64_t lat = timespec_diff_us(ts_start, &now);
                        stats_record_latency(&w->stats, lat);
                        c->send_time_head++;
                    }
                }

                return_buffer(w, bid);

                /* Check if this connection's quota is done */
                int done = w->requests_per_conn > 0 &&
                           c->requests_sent >= w->requests_per_conn;

                if (done) {
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
}
