#include "core/pace.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>

#include "core/send.h"

void pace_init(worker_t *w)
{
    /* Sends are timed by the loop's wait, so the wait has to end when it
       was asked to: the default 50 us timer slack would land in every
       paced request's latency. */
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
    w->pace_cap  = w->num_conns + 64;
    w->pace_heap = malloc((size_t)w->pace_cap * sizeof(*w->pace_heap));
    if (!w->pace_heap) {
        fprintf(stderr, "[w%d] pace heap: out of memory\n", w->id);
        exit(1);
    }
}

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
void pace_fire(worker_t *w, gc_conn_t *c, const int conn_idx)
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
void pace_schedule(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    if (c->due_ns <= now_ns())
        pace_fire(w, c, conn_idx);
    else
        pace_push(w, c->due_ns, conn_idx, c->gen);
}

/* Fire everything whose time has come. */
void pace_tick(worker_t *w)
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
long pace_wait_ns(const worker_t *w)
{
    if (!w->pace_len) return 1000000L;
    const uint64_t now = now_ns();
    const uint64_t due = w->pace_heap[0].due_ns;
    if (due <= now) return 0;
    return due - now < 1000000ULL ? (long)(due - now) : 1000000L;
}

/* A connection is ready for requests (first time, or again after a
 * reconnect): start or resume its schedule.
 *
 * Every connection sends on one shared grid: connection g at
 * epoch + (g/N)*interval + k*interval. Anchoring each schedule at the moment
 * its connection came up instead would bunch the offsets of a fleet that
 * opens over a ramp (in index order, as --connect-limit does), and the
 * offered rate would pulse at the interval's period by about ramp/interval.
 * A connection starts at its first slot after it is up: slots that passed
 * while it was still connecting were setup, not owed. A reconnect keeps its
 * schedule and catches up on what it missed. */
void pace_start(worker_t *w, gc_conn_t *c, const int conn_idx)
{
    c->pace_pending = 0;
    if (!c->due_ns) {
        const int g = w->conn_offset + conn_idx;
        const uint64_t now = now_ns();
        uint64_t due = w->epoch_ns + (uint64_t)((double)w->interval_ns * g / w->total_conns);
        if (due < now)
            due += (now - due + w->interval_ns - 1) / w->interval_ns * w->interval_ns;
        c->due_ns = due;
    }
    pace_schedule(w, c, conn_idx);
}
