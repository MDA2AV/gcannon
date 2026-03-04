# gcannon

io_uring HTTP load generator. Designed to benchmark HTTP servers with minimal syscall overhead.

## Why not wrk?

wrk uses epoll — one `epoll_wait` + individual `read`/`write` syscalls per event. gcannon uses io_uring:

- **Single syscall per loop** — `io_uring_submit_and_wait_timeout` submits and harvests in one kernel entry
- **Zero-copy recv** — provided buffer rings, kernel writes directly into pre-registered memory
- **Multishot recv** — one SQE arms continuous receive, no re-arming per event
- **Batch CQE processing** — harvests up to 2048 completions at once
- **HTTP pipelining** — fire N requests per connection without waiting for responses
- **Pre-built request buffer** — no per-request formatting, same pointer/length every send

## Building

Requires liburing (`liburing-dev` on Debian/Ubuntu).

```bash
make
```

## Usage

```
gcannon <url> -c <connections> -t <threads> -d <duration> [-p <pipeline>]
```

| Flag | Default | Description |
|------|---------|-------------|
| `<url>` | required | Target URL (`http://` only) |
| `-c` | 100 | Total connections |
| `-t` | 1 | Worker threads |
| `-d` | 10s | Duration (`10s`, `1m`) |
| `-p` | 16 | Pipeline depth (max 64) |

```bash
gcannon http://127.0.0.1:8080/plaintext -c 512 -t 8 -d 10s -p 16
```

## Architecture

```
main thread
├── parse args, resolve host, build HTTP request buffer
├── spawn N worker threads (100ms warmup, then reset stats)
├── sleep(duration)
├── set running=0, join threads
└── aggregate stats + print

worker thread [0..N-1]
├── own io_uring ring (SINGLE_ISSUER | DEFER_TASKRUN)
├── provided buffer ring for zero-copy recv
├── manages connections_per_thread connections
└── event loop: connect → send(pipeline) → recv → count → send(refill)
```

Each worker thread is fully independent — no locks, no cross-thread communication.

## Project Structure

```
include/
  constants.h  — UD packing macros, ring/buffer tunables
  worker.h     — worker thread struct, connection state
  http.h       — request builder, response parser
  stats.h      — two-tier latency histogram

src/
  main.c       — CLI, thread orchestration, stats reporting
  worker.c     — io_uring event loop (connect/send/recv)
  http.c       — HTTP request builder, response boundary parser
  stats.c      — histogram, percentile computation
```

## Output

```
gcannon — io_uring HTTP load generator
  Target:    127.0.0.1:8080/plaintext
  Threads:   8
  Conns:     512 (64/thread)
  Pipeline:  16
  Duration:  10s

  Thread Stats   Avg      p50      p90      p99    p99.9
    Latency   4.24ms   4.11ms   4.20ms   5.50ms   33.50ms

  19382819 requests in 10.00s, 19366819 responses
  Throughput: 1.94M req/s
  Bandwidth:  144.04MB/s
```
