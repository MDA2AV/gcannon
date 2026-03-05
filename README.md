# gcannon

io_uring HTTP load generator. Designed to benchmark HTTP servers with minimal syscall overhead.

## Features

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

HTTP response parsing uses [picohttpparser](https://github.com/h2o/picohttpparser), included under `external/`.

## Usage

```
gcannon <url> -c <connections> -t <threads> -d <duration> [-p <pipeline>] [-r <req/conn>]
```

| Flag | Default | Description |
|------|---------|-------------|
| `<url>` | required | Target URL (`http://` only) |
| `-c` | 100 | Total connections |
| `-t` | 1 | Worker threads |
| `-d` | 10s | Duration (`10s`, `1m`) |
| `-p` | 16 | Pipeline depth (max 64) |
| `-r` | unlimited | Requests per connection before reconnecting |

### Keep-alive mode (default)

Connections stay open for the entire run. Maximum throughput.

```bash
gcannon http://127.0.0.1:8080/plaintext -c 512 -t 8 -d 10s -p 16
```

### Short-lived connections

Each connection sends N requests, then closes and reconnects. Tests the full accept → serve → close cycle.

```bash
gcannon http://127.0.0.1:8080/plaintext -c 256 -t 4 -d 10s -p 1 -r 1000
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
└── event loop: connect → send(pipeline) → recv → count → send(refill) [→ reconnect]
```

Each worker thread is fully independent — no locks, no cross-thread communication.

Connection identity uses a generation counter in io_uring user-data to detect and ignore stale CQEs from previous connections on the same slot.

## Project Structure

```
include/
  constants.h  — UD packing macros (kind/gen/idx), ring/buffer tunables
  worker.h     — worker thread struct, connection state
  http.h       — request builder, response parser
  stats.h      — two-tier latency histogram

src/
  main.c       — CLI, thread orchestration, stats reporting
  worker.c     — io_uring event loop (connect/send/recv/reconnect)
  http.c       — HTTP request builder, response parser (picohttpparser)
  stats.c      — histogram, percentile computation

external/
  picohttpparser/  — h2o/picohttpparser
```

## Output

```
gcannon — io_uring HTTP load generator
  Target:    127.0.0.1:8080/plaintext
  Threads:   4
  Conns:     256 (64/thread)
  Pipeline:  16
  Req/conn:  1000
  Duration:  5s

  Thread Stats   Avg      p50      p90      p99    p99.9
    Latency   2.19ms   2.06ms   2.90ms   5.14ms   7.27ms

  9331080 requests in 5.00s, 9188954 responses
  Throughput: 1.84M req/s
  Bandwidth:  136.68MB/s
  Reconnects: 9202
```
