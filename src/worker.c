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

static inline void arm_recv_multishot(worker_t *w, int fd)
{
    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_recv_multishot(sqe, fd, NULL, 0, 0);
    sqe->flags    |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BUFFER_RING_BGID;
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_RECV, fd));
}

static inline void fire_requests(worker_t *w, gc_conn_t *c, int count)
{
    if (count <= 0 || c->send_inflight || c->state != CONN_ACTIVE)
        return;

    if (count > w->pipeline_depth)
        count = w->pipeline_depth;

    int send_len = count * w->request_len;
    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_send(sqe, c->fd, w->pipeline_buf, send_len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_SEND, c->fd));

    c->send_inflight = 1;
    c->send_total = send_len;
    c->send_done  = 0;
    c->pipeline_inflight += count;
    w->stats.requests += count;

    /* Record send times for latency tracking */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < count; i++) {
        c->send_times[c->send_time_tail % PIPELINE_DEPTH_MAX] = now;
        c->send_time_tail++;
    }
}

static inline uint64_t timespec_diff_us(struct timespec *start, struct timespec *end)
{
    int64_t sec  = end->tv_sec  - start->tv_sec;
    int64_t nsec = end->tv_nsec - start->tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    return (uint64_t)(sec * 1000000L + nsec / 1000L);
}

static int create_connect_socket(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static void start_connect(worker_t *w, int conn_idx)
{
    gc_conn_t *c = &w->conns[conn_idx];
    int fd = create_connect_socket();
    if (fd < 0) {
        w->stats.connect_errors++;
        return;
    }

    if (fd >= MAX_CONNS_PER_WORKER) {
        close(fd);
        w->stats.connect_errors++;
        return;
    }

    c->fd = fd;
    c->state = CONN_CONNECTING;
    c->pipeline_inflight = 0;
    c->send_inflight = 0;
    c->send_total = 0;
    c->send_done = 0;
    c->send_time_head = 0;
    c->send_time_tail = 0;
    http_parser_reset(&c->parser);
    w->conn_fd_map[fd] = conn_idx;

    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_connect(sqe, fd, (struct sockaddr *)&w->server_addr,
                          sizeof(w->server_addr));
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_CONNECT, fd));
}

static void close_conn(worker_t *w, gc_conn_t *c)
{
    if (c->fd >= 0 && c->fd < MAX_CONNS_PER_WORKER)
        w->conn_fd_map[c->fd] = -1;
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    c->state = CONN_CLOSED;
    c->send_inflight = 0;
    c->pipeline_inflight = 0;
}

static void reconnect(worker_t *w, int conn_idx)
{
    gc_conn_t *c = &w->conns[conn_idx];
    close_conn(w, c);
    if (*w->running)
        start_connect(w, conn_idx);
}

/* ── init ──────────────────────────────────────────────────────────── */

void worker_init(worker_t *w, int id, struct sockaddr_in *addr,
                 char *pipeline_buf, int request_len, int pipeline_depth,
                 int num_conns, volatile int *running)
{
    memset(w, 0, sizeof(*w));
    w->id             = id;
    w->server_addr    = *addr;
    w->pipeline_buf   = pipeline_buf;
    w->request_len    = request_len;
    w->pipeline_depth = pipeline_depth;
    w->pipeline_len   = pipeline_depth * request_len;
    w->num_conns      = num_conns;
    w->running        = running;

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

    /* Connection arrays */
    w->conns = calloc(num_conns, sizeof(gc_conn_t));
    w->conn_fd_map = malloc(MAX_CONNS_PER_WORKER * sizeof(int));
    memset(w->conn_fd_map, -1, MAX_CONNS_PER_WORKER * sizeof(int));

    /* Start all connections */
    for (int i = 0; i < num_conns; i++) {
        w->conns[i].fd = -1;
        w->conns[i].state = CONN_CLOSED;
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
            uint64_t   ud   = cqe->user_data;
            ud_kind_t  kind = UD_KIND(ud);
            int        fd   = UD_FD(ud);
            int        res  = cqe->res;

            if (fd < 0 || fd >= MAX_CONNS_PER_WORKER) continue;
            int conn_idx = w->conn_fd_map[fd];

            switch (kind) {

            case UD_CONNECT: {
                if (conn_idx < 0) break;
                gc_conn_t *c = &w->conns[conn_idx];
                if (res < 0) {
                    w->stats.connect_errors++;
                    reconnect(w, conn_idx);
                    break;
                }
                c->state = CONN_ACTIVE;
                arm_recv_multishot(w, fd);
                fire_requests(w, c, w->pipeline_depth);
                break;
            }

            case UD_SEND: {
                if (conn_idx < 0) break;
                gc_conn_t *c = &w->conns[conn_idx];

                if (res < 0) {
                    c->send_inflight = 0;
                    w->stats.read_errors++;
                    reconnect(w, conn_idx);
                    break;
                }

                c->send_done += res;
                if (c->send_done < c->send_total) {
                    /* Partial send — resubmit remainder */
                    int off = c->send_done;
                    struct io_uring_sqe *sqe = sqe_get(&w->ring);
                    io_uring_prep_send(sqe, c->fd,
                                       w->pipeline_buf + off,
                                       c->send_total - off, MSG_NOSIGNAL);
                    io_uring_sqe_set_data64(sqe, PACK_UD(UD_SEND, c->fd));
                } else {
                    c->send_inflight = 0;
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
                    if (conn_idx >= 0)
                        reconnect(w, conn_idx);
                    break;
                }

                if (!has_buffer) break;

                uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

                if (conn_idx >= 0) {
                    gc_conn_t *c = &w->conns[conn_idx];
                    uint8_t *buf = w->buf_slab + (size_t)bid * RECV_BUF_SIZE;

                    w->stats.bytes_read += res;
                    int completed = http_parse_responses(&c->parser, buf, res);

                    /* Record latencies */
                    struct timespec now;
                    if (completed > 0)
                        clock_gettime(CLOCK_MONOTONIC, &now);

                    for (int j = 0; j < completed; j++) {
                        w->stats.responses++;
                        c->pipeline_inflight--;

                        if (c->send_time_head < c->send_time_tail) {
                            struct timespec *ts_start =
                                &c->send_times[c->send_time_head % PIPELINE_DEPTH_MAX];
                            uint64_t lat = timespec_diff_us(ts_start, &now);
                            stats_record_latency(&w->stats, lat);
                            c->send_time_head++;
                        }
                    }

                    return_buffer(w, bid);

                    if (!has_more)
                        arm_recv_multishot(w, fd);

                    /* Refill pipeline with completed count */
                    if (completed > 0)
                        fire_requests(w, c, completed);
                } else {
                    return_buffer(w, bid);
                }
                break;
            }

            case UD_CANCEL:
                break;
            }
        }

        io_uring_cq_advance(&w->ring, got);
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
    free(w->conn_fd_map);
}
