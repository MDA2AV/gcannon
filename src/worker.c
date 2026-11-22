#include "worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
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
    uint8_t *addr = w->buf_slab + (size_t)bid * w->recv_buf_size;
    io_uring_buf_ring_add(w->buf_ring, addr, w->recv_buf_size,
                          bid, w->buf_mask, 0);
    io_uring_buf_ring_advance(w->buf_ring, 1);
}

static inline void arm_recv_multishot(worker_t *w, const int conn_idx)
{
    const gc_conn_t *c = &w->conns[conn_idx];
    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    if (w->ktls)
        /* recvmsg, not recv: kTLS reports each record's type in a cmsg and
           fails a recv with no room for one (EIO) as soon as a session
           ticket arrives */
        io_uring_prep_recvmsg_multishot(sqe, c->fd, &w->recv_msg, 0);
    else
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
    c->send_buf   = w->ws_upgrade_buf;
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

/* ── --rate pacing ─────────────────────────────────────────────────── */

/* A connection's next send waits in a per-worker min-heap until it is due.
 * Each connection has at most one live entry; entries left behind by a close
 * or a reschedule are detected when they reach the top and skipped. */

static void pace_push(worker_t *w, const uint64_t due_ns, const int conn_idx, const uint16_t gen)
{
    if (w->pace_len == w->pace_cap) {
        const int cap = w->pace_cap ? w->pace_cap * 2 : 1024;
        pace_entry_t *h = realloc(w->pace_heap, (size_t)cap * sizeof(*h));
        if (!h) {
            fprintf(stderr, "[w%d] pace heap: out of memory\n", w->id);
            exit(1);
        }
        w->pace_heap = h;
        w->pace_cap = cap;
    }
    int i = w->pace_len++;
    while (i > 0) {
        const int parent = (i - 1) / 2;
        if (w->pace_heap[parent].due_ns <= due_ns) break;
        w->pace_heap[i] = w->pace_heap[parent];
        i = parent;
    }
    w->pace_heap[i] = (pace_entry_t){ .due_ns = due_ns, .conn_idx = conn_idx, .gen = gen };
}

static pace_entry_t pace_pop(worker_t *w)
{
    const pace_entry_t top  = w->pace_heap[0];
    const pace_entry_t last = w->pace_heap[--w->pace_len];
    int i = 0;
    for (;;) {
        int child = 2 * i + 1;
        if (child >= w->pace_len) break;
        if (child + 1 < w->pace_len &&
            w->pace_heap[child + 1].due_ns < w->pace_heap[child].due_ns)
            child++;
        if (last.due_ns <= w->pace_heap[child].due_ns) break;
        w->pace_heap[i] = w->pace_heap[child];
        i = child;
    }
    if (w->pace_len > 0) w->pace_heap[i] = last;
    return top;
}

/* Send the request that is due on this connection, or note that it is owed
 * if the previous send has not completed yet (UD_SEND fires it then). */
static void pace_fire(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    if (c->send_inflight) {
        c->pace_pending = 1;
        return;
    }
    const uint64_t now = now_ns();
    if (!fire_requests_at(w, c, conn_idx, 1, c->due_ns))
        return;
    const uint64_t lag_us = now > c->due_ns ? (now - c->due_ns) / 1000 : 0;
    w->stats.paced_sends++;
    if (lag_us > 1000) w->stats.paced_late++;
    if (lag_us > w->stats.paced_lag_max_us) w->stats.paced_lag_max_us = lag_us;
}

/* The connection is free for its next request: send it now if it is due
 * (or overdue), otherwise queue it for its time. */
static void pace_schedule(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    if (c->due_ns <= now_ns())
        pace_fire(w, c, conn_idx);
    else
        pace_push(w, c->due_ns, conn_idx, c->gen);
}

/* Fire everything whose time has come. */
static void pace_tick(worker_t *w)
{
    if (!w->pace_len) return;
    const uint64_t now = now_ns();
    while (w->pace_len && w->pace_heap[0].due_ns <= now) {
        const pace_entry_t e = pace_pop(w);
        gc_conn_t *c = &w->conns[e.conn_idx];
        if (c->gen != e.gen || c->state != CONN_ACTIVE || c->due_ns != e.due_ns)
            continue;   /* closed or rescheduled since it was queued */
        pace_fire(w, c, e.conn_idx);
    }
}

/* How long the loop may sleep: until the next send is due, at most 1 ms. */
static long pace_wait_ns(const worker_t *w)
{
    if (!w->pace_len) return 1000000L;
    const uint64_t now = now_ns();
    const uint64_t due = w->pace_heap[0].due_ns;
    if (due <= now) return 0;
    return due - now < 1000000ULL ? (long)(due - now) : 1000000L;
}

/* A connection is ready for requests (first time, or again after a
 * reconnect): closed loop fills its pipeline, --rate starts or resumes its
 * schedule.
 *
 * Every connection sends on one shared grid anchored at epoch: worker w's
 * connection i at epoch + ((i + off/N) / n)*interval + k*interval, where n
 * is the worker's connection count and off its first global index. Each
 * worker spreads its own connections over the whole interval and the
 * workers' slots interleave (off/N is w/T for an even split), so the fleet
 * fills the same evenly spaced N-slot grid as g/N would, while every thread
 * sends only its 1/T share of the rate at any moment. Numbering the slots
 * g/N in global order gave each worker one contiguous 1/T slice instead:
 * one thread at a time carried the whole fleet's rate, and at 1M
 * connections, 64 threads and 250K req/s nearly every send went out
 * 100-165 ms late, with the responses waiting behind them.
 *
 * Anchoring each schedule at the moment its connection came up instead would
 * bunch the offsets of a fleet that opens over a ramp (in index order, as
 * --connect-limit does), and the offered rate would pulse at the interval's
 * period by about ramp/interval. A connection starts at its first slot after
 * it is up: slots that passed while it was still connecting were setup, not
 * owed. A reconnect keeps its schedule and catches up on what it missed. */
static void conn_start_requests(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    if (!w->interval_ns) {
        fire_requests(w, c, conn_idx, w->pipeline_depth);
        return;
    }
    c->pace_pending = 0;
    if (!c->due_ns) {
        const double slot = ((double)conn_idx
                             + (double)w->conn_offset / (double)w->total_conns)
                            / (double)w->num_conns;
        const uint64_t now = now_ns();
        uint64_t due = w->epoch_ns + (uint64_t)((double)w->interval_ns * slot);
        if (due < now)
            due += (now - due + w->interval_ns - 1) / w->interval_ns * w->interval_ns;
        c->due_ns = due;
    }
    pace_schedule(w, c, conn_idx);
}

static inline uint64_t timespec_diff_us(const struct timespec *start, const struct timespec *end)
{
    int64_t sec  = end->tv_sec  - start->tv_sec;
    int64_t nsec = end->tv_nsec - start->tv_nsec;
    if (nsec < 0) { sec--; nsec += 1000000000L; }
    return (uint64_t)(sec * 1000000L + nsec / 1000L);
}

/* A connection settles when its first setup attempt finishes — it became ready
   for requests, or it failed and is now retrying. main holds the measurement
   clock until every connection has settled, so setup (WS upgrades) never runs
   inside the measured window. Later reconnects don't count again. */
static inline void conn_settle(worker_t *w, gc_conn_t *c)
{
    if (c->settled) return;
    c->settled = 1;
    atomic_fetch_add_explicit(&w->conns_settled, 1, memory_order_relaxed);
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

/* Returns 1 when a connect was submitted (a UD_CONNECT completion will follow) */
static int start_connect(worker_t *w, const int conn_idx)
{
    gc_conn_t *c = &w->conns[conn_idx];
    const int fd = create_connect_socket(w->requests_per_conn > 0);
    if (fd < 0) {
        w->stats.connect_errors++;
        conn_settle(w, c);   /* never retried, so don't hold the warmup on it */
        return 0;
    }

    c->fd = fd;
    c->gen++;
    c->state = CONN_CONNECTING;
    if (c->gen == 1) {
        /* First connect — distribute evenly across templates */
        c->tpl_idx = (w->conn_offset + conn_idx) % w->num_templates;
        /* Seed per-connection RNG for {RAND} placeholders. Mix thread
         * id, connection index, and a time component so each connection
         * gets a unique stream. */
        c->rng_state = (uint64_t)(w->id + 1) * 6364136223846793005ULL
                      + (uint64_t)(conn_idx + 1) * 1442695040888963407ULL
                      + (uint64_t)clock();
        if (c->rng_state == 0) c->rng_state = 1;
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

    /* --addr-span: one source address can hold only ~64K connections to one
       destination, so a large fleet is spread over consecutive addresses */
    const struct sockaddr_in *dst =
        &w->server_addrs[(w->conn_offset + conn_idx) % w->num_addrs];
    struct io_uring_sqe *sqe = sqe_get(&w->ring);
    io_uring_prep_connect(sqe, fd, (const struct sockaddr *)dst, sizeof(*dst));
    io_uring_sqe_set_data64(sqe, PACK_UD(UD_CONNECT, c->gen, conn_idx));
    return 1;
}

/* Start connections not yet started, in index order, up to conn_target (the
   step's share under --ramp-p99, every connection otherwise), and with
   --connect-limit only while fewer than the limit have a first attempt in
   flight. A first attempt holds its slot until it is over (connect_slot_free):
   the connection is ready for requests, or it failed. Later reconnects are not
   counted. */
static void connect_fill(worker_t *w)
{
    const int limit = w->connect_limit > 0 ? w->connect_limit : INT_MAX;
    while (w->connect_next < w->conn_target &&
           w->connect_inflight < limit && *w->running) {
        const int idx = w->connect_next++;
        if (start_connect(w, idx)) {
            w->conns[idx].holds_slot = 1;
            w->connect_inflight++;
        }
    }
}

/* The connection's first attempt is over, so its slot goes to the next
 * connection not yet started.
 *
 * Over, for https, means the handshake has finished, not the TCP connect. The
 * kernel completes a TCP connect on its own, at once, whatever the server is
 * doing; a server finishes handshakes at the rate its CPU signs them. Freeing
 * the slot at the TCP connect let a thread start connections as fast as the
 * kernel accepted them, while their handshakes queued at the server faster
 * than it could finish them, until the ones at the back outlived the server's
 * handshake timeout (ioxide and Kestrel 10 s, actix 3 s) and were closed:
 * "TLS handshake failed: unexpected eof while reading", and a retry that queued
 * again. Held to the end of the handshake, a slot caps what one thread can have
 * waiting at the server at --connect-limit handshakes. */
static void connect_slot_free(worker_t *w, gc_conn_t *c)
{
    if (!c->holds_slot) return;
    c->holds_slot = 0;
    if (w->connect_inflight > 0) w->connect_inflight--;
    connect_fill(w);
}

static void close_conn(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    if (c->fd >= 0) {
        struct io_uring_sqe *sqe = sqe_get(&w->ring);
        io_uring_prep_cancel64(sqe, PACK_UD(UD_RECV, c->gen, conn_idx), 0);
        io_uring_sqe_set_data64(sqe, PACK_UD(UD_CANCEL, c->gen, conn_idx));
        if (c->state == CONN_TLS_HANDSHAKE) {
            /* An armed handshake poll holds the socket open, like the recv */
            sqe = sqe_get(&w->ring);
            io_uring_prep_cancel64(sqe, PACK_UD(UD_POLL, c->gen, conn_idx), 0);
            io_uring_sqe_set_data64(sqe, PACK_UD(UD_CANCEL, c->gen, conn_idx));
        }
        close(c->fd);
    }
    if (c->ssl) {
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    c->fd = -1;
    c->state = CONN_CLOSED;
    c->send_inflight = 0;
    c->pipeline_inflight = 0;
}

static void reconnect(worker_t *w, const int conn_idx)
{
    conn_settle(w, &w->conns[conn_idx]);   /* a failed first attempt counts */
    connect_slot_free(w, &w->conns[conn_idx]);
    close_conn(w, &w->conns[conn_idx], conn_idx);
    w->stats.reconnects++;
    if (*w->running)
        start_connect(w, conn_idx);
}

/* The connection is up (and for https, keyed): start receiving, then send the
   WebSocket upgrade or the first pipeline. */
static void conn_ready(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    connect_slot_free(w, c);
    arm_recv_multishot(w, conn_idx);
    if (w->ws_mode) {
        c->state = CONN_WS_UPGRADING;
        fire_ws_upgrade(w, c, conn_idx);
    } else {
        c->state = CONN_ACTIVE;
        conn_settle(w, c);
        conn_start_requests(w, c, conn_idx);
    }
}

/* ── TLS handshake ─────────────────────────────────────────────────── */

/* OpenSSL runs the handshake directly on the socket, and all that comes out of
 * it are the TLS 1.3 traffic secrets. Those become kernel TLS keys and the SSL
 * object is freed, so from then on the connection is the same plain fd as for
 * http:// — the kernel just encrypts what goes through it. */

/* Key log callback: the only public way OpenSSL 3.0 hands out the secrets */
static void tls_keylog(const SSL *ssl, const char *line)
{
    gc_conn_t *c = SSL_get_app_data(ssl);
    if (c) ktls_keylog_line(&c->tls_secrets, line);
}

static SSL_CTX *tls_ctx_new(void)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return NULL;
    /* Kernel TLS carries TLS 1.3 records in these three suites, so offer
       nothing it can't take. AES-128-GCM first, the order browsers use. */
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) ||
        !SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256:"
                                       "TLS_AES_256_GCM_SHA384:"
                                       "TLS_CHACHA20_POLY1305_SHA256")) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    /* Benchmark targets mostly run with self-signed or test certificates */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    /* The receive path drops session tickets, so there is nothing to cache */
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_keylog_callback(ctx, tls_keylog);
#ifdef SSL_OP_ENABLE_KTLS
    /* Both directions are installed below. OpenSSL's own kTLS, if a system
       config turns it on, would claim the transmit side first. */
    SSL_CTX_clear_options(ctx, SSL_OP_ENABLE_KTLS);
#endif
    return ctx;
}

/* The first TLS failure of a run goes to stderr, so a target that won't do
   TLS 1.3 says why instead of only piling up connect errors. */
static void tls_report(const char *what, const char *why)
{
    static atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&reported))
        fprintf(stderr, "TLS %s failed: %s\n", what, why);
}

static void tls_finish(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    const ktls_secrets_t *k = &c->tls_secrets;
    const char *name = SSL_CIPHER_get_name(SSL_get_current_cipher(c->ssl));
    const ktls_suite_t suite = ktls_suite_from_name(name);
    char why[128];
    int rc = -EPROTO;

    if (!suite) {
        snprintf(why, sizeof(why), "kernel TLS can't carry %s", name ? name : "the suite");
        tls_report("setup", why);
    } else if (!k->client_len || k->client_len != k->server_len) {
        tls_report("setup", "the handshake produced no traffic secrets");
    } else if ((rc = ktls_install(c->fd, suite, k->client, k->server, k->client_len)) < 0) {
        snprintf(why, sizeof(why), "installing kernel TLS keys: %s", strerror(-rc));
        tls_report("setup", why);
    }

    OPENSSL_cleanse(&c->tls_secrets, sizeof(c->tls_secrets));
    SSL_free(c->ssl);   /* SSL_set_fd doesn't own the fd, so it stays open */
    c->ssl = NULL;

    if (rc < 0) {
        w->stats.connect_errors++;
        reconnect(w, conn_idx);
        return;
    }
    conn_ready(w, c, conn_idx);
}

/* Drive the handshake as far as the socket allows, then wait for it with a
   one-shot poll. */
static void tls_step(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    ERR_clear_error();   /* SSL_get_error reads this thread's error queue */
    const int r = SSL_do_handshake(c->ssl);
    if (r == 1) {
        tls_finish(w, c, conn_idx);
        return;
    }
    const int err = SSL_get_error(c->ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        struct io_uring_sqe *sqe = sqe_get(&w->ring);
        io_uring_prep_poll_add(sqe, c->fd, err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT);
        io_uring_sqe_set_data64(sqe, PACK_UD(UD_POLL, c->gen, conn_idx));
        return;
    }

    char why[256];
    const unsigned long e = ERR_peek_last_error();
    const char *reason = e ? ERR_reason_error_string(e) : NULL;
    if (reason)
        snprintf(why, sizeof(why), "%s", reason);
    else if (e)
        ERR_error_string_n(e, why, sizeof(why));
    else if (err == SSL_ERROR_SYSCALL && errno)
        snprintf(why, sizeof(why), "%s", strerror(errno));
    else
        snprintf(why, sizeof(why), "server closed the connection");
    tls_report("handshake", why);
    ERR_clear_error();

    w->stats.connect_errors++;
    reconnect(w, conn_idx);
}

static void tls_start(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    c->state = CONN_TLS_HANDSHAKE;
    memset(&c->tls_secrets, 0, sizeof(c->tls_secrets));
    /* OpenSSL reads one whole record at a time (no read-ahead), so when the
       handshake ends nothing past the server's Finished has left the socket:
       kernel TLS starts at the first application record, sequence 0. */
    c->ssl = SSL_new(w->ssl_ctx);
    if (!c->ssl || !SSL_set_fd(c->ssl, c->fd)) {
        tls_report("setup", "out of memory");
        w->stats.connect_errors++;
        reconnect(w, conn_idx);
        return;
    }
    SSL_set_app_data(c->ssl, c);
    if (w->tls_sni)
        SSL_set_tlsext_host_name(c->ssl, w->tls_sni);
    SSL_set_connect_state(c->ssl);
    tls_step(w, c, conn_idx);
}


/* ── init ──────────────────────────────────────────────────────────── */

void worker_init(worker_t *w,
                 const int id,
                 const struct sockaddr_in *addrs,
                 const int num_addrs,
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
                 const int recv_buf_size,
                 const int cqe_latency,
                 const int per_tpl_latency,
                 const int tls,
                 const char *tls_sni,
                 const uint64_t interval_ns,
                 const uint64_t epoch_ns,
                 const int total_conns,
                 const int connect_limit,
                 const int conn_target,
                 const uint64_t ramp_p99_us,
                 volatile int *running)
{
    memset(w, 0, sizeof(*w));

    if (pipeline_depth > PIPELINE_DEPTH_MAX) {
        fprintf(stderr, "[w%d] pipeline_depth %d exceeds PIPELINE_DEPTH_MAX (%d)\n",
                id, pipeline_depth, PIPELINE_DEPTH_MAX);
        exit(1);
    }

    w->id                = id;
    w->server_addrs      = addrs;
    w->num_addrs         = num_addrs > 0 ? num_addrs : 1;
    w->interval_ns       = interval_ns;
    w->epoch_ns          = epoch_ns;
    w->total_conns       = total_conns > 0 ? total_conns : num_conns;
    w->connect_limit     = connect_limit;
    w->conn_target       = conn_target > 0 && conn_target < num_conns
                         ? conn_target : num_conns;
    w->ramp_p99_us       = ramp_p99_us;
    w->templates         = templates;
    w->num_templates     = num_templates;
    w->pipeline_depth    = pipeline_depth;
    w->num_conns         = num_conns;
    w->conn_offset       = conn_offset;
    w->requests_per_conn = requests_per_conn;
    w->expected_status   = expected_status;
    w->ws_mode           = ws_mode;
    w->recv_buf_size     = recv_buf_size;
    w->cqe_latency       = cqe_latency;
    w->per_tpl_latency   = per_tpl_latency;
    w->ktls              = tls;
    w->tls_sni           = tls_sni;
    w->running           = running;

    if (per_tpl_latency && num_templates > 1) {
        w->stats.tpl_latency = calloc(num_templates, sizeof(latency_hist_t));
        w->stats.num_tpl_latency = num_templates;
    }

    if (ramp_p99_us) {
        /* What main reads at the end of each step (CTL_SNAPSHOT) */
        w->snap = calloc(1, sizeof(*w->snap));
        if (!w->snap) {
            fprintf(stderr, "[w%d] calloc: out of memory\n", id);
            exit(1);
        }
        if (w->stats.tpl_latency) {
            w->snap->tpl_latency = calloc(num_templates, sizeof(latency_hist_t));
            if (!w->snap->tpl_latency) {
                fprintf(stderr, "[w%d] calloc: out of memory\n", id);
                exit(1);
            }
            w->snap->num_tpl_latency = num_templates;
        }
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
    {
        int bret;
        w->buf_ring = io_uring_setup_buf_ring(&w->ring, BUF_RING_ENTRIES,
                                               BUFFER_RING_BGID, 0, &bret);
        if (!w->buf_ring) {
            fprintf(stderr, "[w%d] setup_buf_ring: %s\n", id, strerror(-bret));
            exit(1);
        }
    }

    /* recvmsg template for kTLS sockets: no address, room for the
       record-type cmsg ahead of the payload in each provided buffer */
    w->recv_msg.msg_controllen = KTLS_RECORD_CMSG_SPACE;

    w->buf_mask = (uint32_t)(BUF_RING_ENTRIES - 1);
    const size_t slab_size = (size_t)BUF_RING_ENTRIES * w->recv_buf_size;
    if (posix_memalign((void **)&w->buf_slab, 64, slab_size) != 0) {
        fprintf(stderr, "[w%d] posix_memalign: out of memory\n", id);
        exit(1);
    }
    memset(w->buf_slab, 0, slab_size);

    for (int i = 0; i < BUF_RING_ENTRIES; i++) {
        uint8_t *a = w->buf_slab + (size_t)i * w->recv_buf_size;
        io_uring_buf_ring_add(w->buf_ring, a, w->recv_buf_size,
                              i, w->buf_mask, w->buf_index++);
    }
    io_uring_buf_ring_advance(w->buf_ring, BUF_RING_ENTRIES);

    /* Connection array */
    w->conns = calloc(num_conns, sizeof(gc_conn_t));
    if (!w->conns) {
        fprintf(stderr, "[w%d] calloc: out of memory\n", id);
        exit(1);
    }

    if (tls) {
        w->ssl_ctx = tls_ctx_new();
        if (!w->ssl_ctx) {
            fprintf(stderr, "[w%d] TLS setup failed\n", id);
            exit(1);
        }
    }

    if (interval_ns) {
        /* Sends are timed by the loop's wait, so the wait has to end when it
           was asked to: the default 50 us timer slack would land in every
           paced request's latency. */
        prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
        w->pace_cap  = num_conns + 64;
        w->pace_heap = malloc((size_t)w->pace_cap * sizeof(*w->pace_heap));
        if (!w->pace_heap) {
            fprintf(stderr, "[w%d] pace heap: out of memory\n", id);
            exit(1);
        }
    }

    for (int i = 0; i < num_conns; i++) {
        w->conns[i].fd = -1;
        w->conns[i].state = CONN_CLOSED;
        w->conns[i].gen = 0;
    }

    /* Start the connections: all at once, or with --connect-limit only that
       many first attempts in flight, each one that is over starting the next
       (connect_slot_free). A million SYNs at once overflow any listen backlog,
       and the retries that follow cost seconds each; a million handshakes at
       once outlast any handshake timeout. --ramp-p99 starts only the first
       step's, and CTL_GROW the rest. */
    if (connect_limit > 0 || w->conn_target < num_conns) {
        connect_fill(w);
    } else {
        for (int i = 0; i < num_conns; i++)
            start_connect(w, i);
        w->connect_next = num_conns;
    }
}

/* ── event loop ────────────────────────────────────────────────────── */

/* A failed read on a kTLS socket that the record layer caused, not the peer
   going away (plain sockets don't count that either): EBADMSG for a record
   that doesn't decrypt, EKEYEXPIRED for a KeyUpdate, which isn't supported,
   and the like. ENOBUFS and ECANCELED come from gcannon's own ring. */
static inline int ktls_read_error(const int res)
{
    switch (-res) {
    case ECONNRESET: case ECONNABORTED: case EPIPE: case ETIMEDOUT:
    case ENOBUFS: case ECANCELED:
        return 0;
    default:
        return res < 0;
    }
}

/* Zero the stats as a window opens. WebSocket upgrades happen before it, so
   their counts are kept for the results. */
static void stats_window_reset(worker_t *w)
{
    const uint64_t ws      = w->stats.ws_upgrades;
    const uint64_t ws_fail = w->stats.ws_upgrade_failures;
    stats_clear(&w->stats);
    w->stats.ws_upgrades = ws;
    w->stats.ws_upgrade_failures = ws_fail;
    w->ws_fail_base = ws_fail;
    clock_gettime(CLOCK_MONOTONIC, &w->window_start);
}

/* --ramp-p99: copy the stats out for main as a step's window closes, with
 * what only this thread can see: how many connections are open, how many
 * lost their socket, and how many have had no response all window.
 *
 * That last one is what a p99 can't show. A closed loop sends nothing more on
 * a connection that is waiting, so a request the server never answers (a
 * full accept queue, a stalled worker) leaves no latency sample at all: the
 * connections still answered keep p99 low while the rest sit idle. A
 * connection counts once its oldest request has waited past both the window
 * and the threshold, so a slow but answered request never does. */
static void ramp_snapshot(worker_t *w)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t window_us = timespec_diff_us(&w->window_start, &now);
    const uint64_t silent_us = window_us > w->ramp_p99_us ? window_us : w->ramp_p99_us;

    worker_stats_t *s = w->snap;
    stats_copy(s, &w->stats);
    s->ws_upgrade_failures -= w->ws_fail_base;   /* refused in this window */
    s->conns_active = s->conns_dead = s->conns_silent = 0;
    for (int i = 0; i < w->connect_next; i++) {
        const gc_conn_t *c = &w->conns[i];
        if (c->state == CONN_ACTIVE) {
            s->conns_active++;
            if (c->send_time_head < c->send_time_tail &&
                timespec_diff_us(&c->send_times[c->send_time_head % PIPELINE_DEPTH_MAX],
                                 &now) > silent_us)
                s->conns_silent++;
        } else if (c->state == CONN_CLOSED) {
            s->conns_dead++;   /* socket() failed, so it never retries */
        }
    }
    w->snap_at = now;
}

/* Do what main asked for (worker_t.ctl), then tell it so */
static void worker_ctl(worker_t *w, const int req)
{
    if (req & CTL_SNAPSHOT)
        ramp_snapshot(w);
    if (req & CTL_RESET)
        stats_window_reset(w);
    if (req & CTL_GROW) {
        const int next = w->conn_target_next;
        w->conn_target = next < w->num_conns ? next : w->num_conns;
        connect_fill(w);
    }
    atomic_fetch_and_explicit(&w->ctl, ~req, memory_order_release);
}

void worker_loop(worker_t *w)
{
    struct io_uring_cqe *cqes[BATCH_CQES];
    struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */

    for (;;) {
        /* Exit check must be unconditional: under sustained load, peek always
         * returns > 0, so a check gated on `got == 0` never fires and the
         * worker spins past shutdown. */
        if (!*w->running) {
            clock_gettime(CLOCK_MONOTONIC, &w->stopped_at);
            /* How many connections were still open at the end: --rate
               reports it, since a connection that dropped and never came
               back sends nothing and would otherwise show only as rate. */
            if (w->interval_ns) {
                uint64_t active = 0;
                for (int i = 0; i < w->num_conns; i++)
                    if (w->conns[i].state == CONN_ACTIVE) active++;
                w->stats.conns_active = active;
            }
            break;
        }

        /* Main's requests: zero our own stats as the window opens, and for
         * --ramp-p99 copy them out as a step's window closes, or open the next
         * step's connections. Done here, on this thread: zeroing them from the
         * main thread (as before) raced with these same counters being
         * incremented here — a concurrent memset vs ++ is undefined behavior.
         * One load per loop iteration, not per request; p99 itself is worked
         * out by main from the copy, once per step. */
        const int ctl = atomic_load_explicit(&w->ctl, memory_order_acquire);
        if (__builtin_expect(ctl != 0, 0))
            worker_ctl(w, ctl);

        /* --rate: send what has come due */
        if (w->interval_ns) {
            pace_tick(w);
            io_uring_submit(&w->ring);
        }

        unsigned got = io_uring_peek_batch_cqe(&w->ring, cqes, BATCH_CQES);
        if (got == 0) {
            struct io_uring_cqe *wait_cqe;
            if (w->interval_ns) {
                /* Sleep only until the next send is due, so it goes out on
                   time rather than up to a millisecond late. */
                const long wait = pace_wait_ns(w);
                if (wait == 0) continue;
                struct __kernel_timespec pts = { .tv_sec = 0, .tv_nsec = wait };
                io_uring_submit_and_wait_timeout(&w->ring, &wait_cqe, 1, &pts, NULL);
            } else {
                io_uring_submit_and_wait_timeout(&w->ring, &wait_cqe, 1, &ts, NULL);
            }
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
                if (kind == UD_RECV && (cqe->flags & IORING_CQE_F_BUFFER)) {
                    const uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                    return_buffer(w, bid);
                }
                continue;
            }

            switch (kind) {

            case UD_CONNECT: {
                /* --connect-limit: the slot stays held through a TLS
                   handshake; conn_ready or reconnect frees it */
                if (res < 0) {
                    w->stats.connect_errors++;
                    reconnect(w, conn_idx);
                    break;
                }
                if (w->ktls)
                    tls_start(w, c, conn_idx);
                else
                    conn_ready(w, c, conn_idx);
                break;
            }

            case UD_POLL: {
                /* The handshake can move again, or the socket failed (which
                   SSL_do_handshake will report) */
                if (c->state != CONN_TLS_HANDSHAKE) break;
                if (res < 0) {
                    w->stats.connect_errors++;
                    reconnect(w, conn_idx);
                    break;
                }
                tls_step(w, c, conn_idx);
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
                    /* Partial send — resubmit remainder from the exact buffer
                       that was sent. For placeholder templates this is the
                       per-conn scratch buffer holding the substituted value,
                       not the template's pipeline_buf (which still carries the
                       zero padding). send_buf stays valid because fire_requests
                       early-returns while send_inflight is set. */
                    const int off = c->send_done;
                    struct io_uring_sqe *sqe = sqe_get(&w->ring);
                    io_uring_prep_send(sqe, c->fd,
                                       c->send_buf + off,
                                       c->send_total - off, MSG_NOSIGNAL);
                    io_uring_sqe_set_data64(sqe, PACK_UD(UD_SEND, c->gen, conn_idx));
                } else {
                    c->send_inflight = 0;

                    if (w->interval_ns) {
                        /* --rate: a request came due while this send was
                           still in flight; send it now. Never refill the way
                           the closed loop does - that would break the pacing. */
                        if (c->pace_pending && c->state == CONN_ACTIVE) {
                            c->pace_pending = 0;
                            pace_fire(w, c, conn_idx);
                        }
                        break;
                    }

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
                    /* Count a session the record layer gave up on; left
                       uncounted, it shows only as reconnects and a low rate */
                    if (w->ktls && ktls_read_error(res)) {
                        w->stats.read_errors++;
                        tls_report("read", res == -EKEYEXPIRED
                                   ? "the server sent a KeyUpdate; rekeying isn't supported"
                                   : strerror(-res));
                    }
                    reconnect(w, conn_idx);
                    break;
                }

                if (!has_buffer) break;

                const uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                uint8_t *slot = w->buf_slab + (size_t)bid * w->recv_buf_size;
                const uint8_t *buf = slot;
                int len = res;

                if (w->ktls) {
                    /* recvmsg completion: header, record-type cmsg, then the
                       plaintext, which is all the parsers see */
                    uint32_t plen = 0;
                    const int type = ktls_recvmsg_record(slot, res, &w->recv_msg,
                                                         &buf, &plen);
                    len = (int)plen;
                    if (type == KTLS_RECORD_HANDSHAKE) {
                        /* Post-handshake message, in practice a session
                           ticket. Sessions aren't resumed, so drop it. (A
                           KeyUpdate makes the next read fail with EKEYEXPIRED,
                           which reconnects.) */
                        return_buffer(w, bid);
                        if (!has_more) arm_recv_multishot(w, conn_idx);
                        break;
                    }
                    if (type < 0 || len == 0 ||
                        (type != KTLS_RECORD_DATA && type != 0)) {
                        /* Malformed completion, end of stream, or an alert.
                           End of stream and close_notify (description 0) are
                           orderly, like a FIN on a plain socket; every other
                           alert is fatal in TLS 1.3. */
                        const int orderly = type >= 0 &&
                            (len == 0 || (type == KTLS_RECORD_ALERT &&
                                          len >= 2 && buf[1] == 0));
                        if (!orderly) {
                            char why[64];
                            if (type == KTLS_RECORD_ALERT && len >= 2)
                                snprintf(why, sizeof(why), "the server sent alert %d", buf[1]);
                            else if (type < 0)
                                snprintf(why, sizeof(why), "malformed recvmsg completion");
                            else
                                snprintf(why, sizeof(why), "unexpected record type %d", type);
                            tls_report("read", why);
                            w->stats.read_errors++;
                        }
                        return_buffer(w, bid);
                        reconnect(w, conn_idx);
                        break;
                    }
                }

                w->stats.bytes_read += len;

                int completed = 0;

                if (c->state == CONN_WS_UPGRADING) {
                    /* Parse HTTP upgrade response — look for 101 */
                    completed = http_parse_responses(&c->parser, buf, len);
                    if (completed < 0) {
                        /* Unrecoverable parse failure — drop and reconnect */
                        w->stats.read_errors++;
                        return_buffer(w, bid);
                        reconnect(w, conn_idx);
                        break;
                    }
                    if (completed > 0 && c->parser.completed_statuses[0] == 101) {
                        c->state = CONN_ACTIVE;
                        conn_settle(w, c);
                        w->stats.ws_upgrades++;
                        ws_parser_reset(&c->ws_parser);
                        return_buffer(w, bid);
                        if (!has_more) arm_recv_multishot(w, conn_idx);
                        conn_start_requests(w, c, conn_idx);
                        break;
                    } else if (completed > 0) {
                        /* Upgrade refused. This is a failed handshake, not a
                           response to a benchmark request, so it must not land
                           in status_*: in WebSocket mode those counters ARE the
                           frame tally (stats_print reports status_2xx as "WS
                           frames"), and a refusal carrying a 2xx also pushed
                           status_2xx above `responses`, underflowing the
                           unsigned subtraction in main's expected-status check. */
                        w->stats.ws_upgrade_failures++;
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
                    completed = ws_parse_frames(&c->ws_parser, buf, len);
                } else {
                    completed = http_parse_responses(&c->parser, buf, len);
                    if (completed < 0) {
                        /* Unrecoverable parse failure (oversized headers or
                           malformed response) — drop and reconnect */
                        w->stats.read_errors++;
                        return_buffer(w, bid);
                        reconnect(w, conn_idx);
                        break;
                    }
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

                /* HTTP status classes: add the parser's per-call tallies in
                   bulk. These are unbounded, so accounting stays exact even
                   when one recv carried more than 256 responses (the cap on
                   completed_statuses[] would otherwise drop the overflow). */
                if (!w->ws_mode) {
                    w->stats.status_2xx   += c->parser.cls_2xx;
                    w->stats.status_3xx   += c->parser.cls_3xx;
                    w->stats.status_4xx   += c->parser.cls_4xx;
                    w->stats.status_5xx   += c->parser.cls_5xx;
                    w->stats.status_other += c->parser.cls_other;
                    w->stats.tpl_responses_2xx[c->tpl_idx] += c->parser.cls_2xx;
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

                    if (completed > 0) {
                        if (w->interval_ns) {
                            /* --rate: the next request is due one interval
                               after the last one was, however late it ran */
                            c->due_ns += w->interval_ns;
                            pace_schedule(w, c, conn_idx);
                        } else {
                            fire_requests(w, c, conn_idx, completed);
                        }
                    }
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

/* ── teardown ──────────────────────────────────────────────────────── */

/* Reap completions until none arrives for 10 ms, or the deadline passes. The
   requests complete as task work this thread runs while it waits
   (DEFER_TASKRUN). Returns how many it reaped. */
static unsigned drain_quiet(worker_t *w, const uint64_t deadline_ns)
{
    unsigned total = 0;
    while (now_ns() < deadline_ns) {
        struct io_uring_cqe *cqe;
        struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 }; /* 10ms */
        const int ret = io_uring_submit_and_wait_timeout(&w->ring, &cqe, 1, &ts, NULL);
        unsigned head, seen = 0;
        io_uring_for_each_cqe(&w->ring, head, cqe)
            seen++;
        io_uring_cq_advance(&w->ring, seen);
        total += seen;
        if (seen == 0 && ret != -EINTR)
            break;
    }
    return total;
}

/* Close every connection from this worker's own thread, before it returns.
 *
 * worker_destroy's close() runs on main while each socket's multishot recv
 * still holds a reference to it, so it closes nothing: the sockets outlive
 * gcannon, until the kernel tears the ring down after exit and closes them one
 * by one on a single kernel worker. Unnoticeable at a few thousand
 * connections; at five million it took minutes, and the host was still closing
 * one run's fleet while the next run connected its own.
 *
 * So the requests are ended first: shutdown(SHUT_RD) wakes each socket's recv
 * (or connect, or handshake poll) to complete, sending nothing on the wire,
 * and reaping the completions drops their references. A recv that finds a
 * response still arriving delivers it and stays armed instead, with that
 * wakeup spent, so the round repeats until one completes nothing. The close()
 * below is then each socket's last, and the socket is closed here - FIN, or
 * RST under SO_LINGER 0, as before - by every worker at once.
 *
 * Bounded: a request that never completes delays the exit, never hangs it,
 * and its socket is closed the old way. (IORING_ASYNC_CANCEL_ALL ends them
 * too, but the kernel searches the ring afresh for every request it cancels:
 * 40K took 11 s.) */
void worker_close_all(worker_t *w)
{
    const uint64_t give_up = now_ns() + 5000000000ULL;   /* 5 s */
    for (int round = 0; round < 8 && now_ns() < give_up; round++) {
        for (int i = 0; i < w->num_conns; i++)
            if (w->conns[i].fd >= 0)
                shutdown(w->conns[i].fd, SHUT_RD);
        if (drain_quiet(w, give_up) == 0)
            break;
    }

    for (int i = 0; i < w->num_conns; i++) {
        if (w->conns[i].fd >= 0) {
            close(w->conns[i].fd);
            w->conns[i].fd = -1;
        }
    }
}

/* ── cleanup ───────────────────────────────────────────────────────── */

void worker_destroy(worker_t *w)
{
    for (int i = 0; i < w->num_conns; i++) {
        if (w->conns[i].fd >= 0)
            close(w->conns[i].fd);
        SSL_free(w->conns[i].ssl);
        free(w->conns[i].scratch_buf);
    }
    SSL_CTX_free(w->ssl_ctx);

    io_uring_free_buf_ring(&w->ring, w->buf_ring,
                           BUF_RING_ENTRIES, BUFFER_RING_BGID);
    io_uring_queue_exit(&w->ring);
    free(w->buf_slab);
    free(w->conns);
    free(w->stats.tpl_latency);
    free(w->pace_heap);
    if (w->snap) {
        free(w->snap->tpl_latency);
        free(w->snap);
    }
}
