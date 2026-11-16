#pragma once

#include "core/worker.h"

/*
 * --rate: every connection sends one request per interval (connections / rate)
 * on one clock shared by all workers, and each request's latency counts from
 * the moment it was due. A connection's next send waits in a per-worker
 * min-heap until then.
 */

/* Allocate the heap and tighten the thread's timer slack (worker thread) */
void pace_init(worker_t *w);

/* The connection is up (first time or after a reconnect): start or resume
 * its schedule */
void pace_start(worker_t *w, gc_conn_t *c, int conn_idx);

/* Its previous request is answered: send the next one now if it is due,
 * otherwise queue it for its time */
void pace_schedule(worker_t *w, gc_conn_t *c, int conn_idx);

/* Send the request due on this connection, or mark it owed while a send is
 * still in flight */
void pace_fire(worker_t *w, gc_conn_t *c, int conn_idx);

/* Send everything that has come due */
void pace_tick(worker_t *w);

/* How long the loop may sleep: until the next send is due, at most 1 ms */
long pace_wait_ns(const worker_t *w);
