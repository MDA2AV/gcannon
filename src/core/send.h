#pragma once

/*
 * The send path: how a request leaves. Shared by the event loop (core/worker.c)
 * and the --rate scheduler (core/pace.c), and kept static inline because it
 * runs for every request.
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include <liburing.h>

#include "core/worker.h"

static inline struct io_uring_sqe *sqe_get(struct io_uring *ring)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (__builtin_expect(!sqe, 0)) {
        io_uring_submit(ring);
        sqe = io_uring_get_sqe(ring);
    }
    return sqe;
}

/* Fast xorshift64 PRNG — one per connection, no shared state */
static inline uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* Write a zero-padded decimal number into buf at the given width.
 * E.g. write_padded(buf, 6, 42357) writes "042357". */
static inline void write_padded(char *buf, int width, uint64_t val)
{
    for (int i = width - 1; i >= 0; i--) {
        buf[i] = '0' + (char)(val % 10);
        val /= 10;
    }
}

static inline uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* Send up to `count` requests. at_ns, when non-zero, is the time the requests
 * were scheduled for (--rate): latency is then measured from the schedule, not
 * from the send, so a server that holds a connection up delays everything
 * queued behind it in the figures too (coordinated-omission correction).
 * Returns 1 if anything was sent. */
static inline int fire_requests_at(worker_t *w, gc_conn_t *c, int conn_idx, int count,
                                   const uint64_t at_ns)
{
    if (count <= 0 || c->send_inflight || c->state != CONN_ACTIVE)
        return 0;

    if (count > w->pipeline_depth)
        count = w->pipeline_depth;

    /* Cap to remaining requests on this connection */
    if (w->requests_per_conn > 0) {
        const int remaining = w->requests_per_conn - c->requests_sent;
        if (remaining <= 0) return 0;
        if (count > remaining) count = remaining;
    }

    const char *buf;
    int send_len;

    if (w->ws_mode) {
        send_len = count * w->ws_frame_len;
        buf = (const char *)w->ws_frame_buf;
    } else {
        const request_tpl_t *tpl = &w->templates[c->tpl_idx];

        if (tpl->ph.type != SUB_NONE) {
            /* Template has a placeholder — copy to scratch buffer and
             * substitute the value before sending. Each pipelined copy
             * in the batch gets its own substituted value. */
            send_len = count * tpl->request_len;

            /* Ensure scratch buffer is large enough */
            if (c->scratch_len < send_len) {
                char *nb = malloc(send_len);
                if (!nb) return 0; /* OOM — skip this batch; retry on next event
                                      (keeps the old buffer; never deref NULL) */
                free(c->scratch_buf);
                c->scratch_buf = nb;
                c->scratch_len = send_len;
            }

            for (int k = 0; k < count; k++) {
                char *dst = c->scratch_buf + k * tpl->request_len;
                memcpy(dst, tpl->pipeline_buf, tpl->request_len);

                uint64_t val;
                if (tpl->ph.type == SUB_RAND) {
                    uint64_t range = tpl->ph.max - tpl->ph.min + 1;
                    val = tpl->ph.min + (xorshift64(&c->rng_state) % range);
                } else { /* SUB_SEQ */
                    val = atomic_fetch_add(&((request_tpl_t *)tpl)->ph.seq, 1);
                }
                write_padded(dst + tpl->ph.offset, tpl->ph.width, val);
            }
            buf = c->scratch_buf;
        } else {
            buf = tpl->pipeline_buf;
            send_len = count * tpl->request_len;
        }
    }

    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_send(sqe, c->fd, buf, send_len, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_SEND, c->gen, conn_idx));

    /* All requests in this batch go out in one send, so they share
       the same dispatch timestamp.  Latency = time from batch dispatch
       to individual response arrival. */
    struct timespec now;
    if (at_ns) {
        now.tv_sec  = (time_t)(at_ns / 1000000000ULL);
        now.tv_nsec = (long)(at_ns % 1000000000ULL);
    } else {
        clock_gettime(CLOCK_MONOTONIC, &now);
    }
    for (int k = 0; k < count; k++) {
        c->send_times[c->send_time_tail % PIPELINE_DEPTH_MAX] = now;
        c->send_time_tail++;
    }

    c->send_inflight = 1;
    c->send_total = send_len;
    c->send_done  = 0;
    c->send_buf   = buf;
    c->pipeline_inflight += count;
    c->requests_sent += count;
    w->stats.requests += count;
    return 1;
}

static inline void fire_requests(worker_t *w, gc_conn_t *c, int conn_idx, int count)
{
    fire_requests_at(w, c, conn_idx, count, 0);
}
