# Glass Cannon

[![Discord](https://discordapp.com/api/guilds/1177529388229734410/widget.png?style=shield)](https://discord.com/invite/H84B5ZqDXR)

High-performance load generator built on Linux io_uring. Supports HTTP/1.1, WebSocket and HTTPS (TLS 1.3 over kernel TLS).

## [Http Arena](https://www.http-arena.com/) official Http/1.1 load generator

## Install

```bash
# Requires liburing-dev and libssl-dev (OpenSSL 3)
git clone https://github.com/MDA2AV/gcannon.git && cd gcannon
make
sudo cp gcannon /usr/local/bin/
```

## Quick Start

```bash
# HTTP benchmark
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s

# HTTP with pipelining
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s -p 16

# WebSocket echo
gcannon http://localhost:8080/ws --ws -c 512 -t 8 -d 10s

# HTTPS (TLS 1.3)
gcannon https://localhost:8443/ -c 512 -t 8 -d 10s

# Raw request files (multiple templates)
gcannon http://localhost:8080 --raw get.raw,post.raw -c 256 -t 4 -d 5s
```

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `<url>` | required | Target URL (`http://` or `https://`) |
| `-c` | 100 | Total connections |
| `-t` | 1 | Worker threads |
| `-d` | 10s | Duration (`5s`, `1m`) |
| `-p` | 1 | Pipeline depth (max 64) |
| `-r` | unlimited | Requests per connection (reconnects after N) |
| `-s` | 200 | Expected HTTP status code |
| `--raw` | | Comma-separated raw request files |
| `--ws` | | WebSocket echo mode |
| `--ws-msg` | `hello` | WebSocket message payload |
| `--tui` | | TUI mode: progress bar, live sparkline, colored results |
| `--json` | | JSON output mode: machine-readable results, no banner or progress |
| `-b` | 10 | Histogram buckets in TUI mode (adaptive, max 100) |
| `--recv-buf` | 4096 | Receive buffer size in bytes (min 512). Each worker allocates 4096 buffers of this size. Larger values suit big responses; smaller values save memory. |
| `--cqe-latency` | | Measure latency at CQE arrival instead of after response parsing |
| `--per-tpl-latency` | | Per-template latency histograms (with `--raw` and multiple templates) |
| `--rate` | closed loop | Fixed total request rate (req/s), paced per connection; latency counts from each request's scheduled time. Needs `-p 1` and no `-r`. See [Fixed-rate mode](#fixed-rate-mode---rate) |
| `--addr-span` | 1 | Spread connections over N consecutive IPv4 addresses from the target's (`127.0.0.1`, `.2`, ...): one source address holds at most ~64K connections to one destination |
| `--connect-limit` | all at once | First connection attempts in flight per thread; each completion starts the next |
| `--warmup-max` | 10s | Longest the warmup waits for connections to settle before measuring anyway |
| `--settle` | 0 | How long the load keeps running after every connection is up before the window opens |
| `--version` | | Print the version and exit |
| `--clear-history` | | Clear saved run history and exit |

## Dynamic Placeholders

Raw request templates support `{RAND:min:max}` and `{SEQ:start}` placeholders that are substituted per-request at send time. This enables CRUD benchmarks where each request targets a different database row.

### `{RAND:min:max}` — random number in range

Each request gets a random value between `min` and `max` (inclusive). Uses a per-connection xorshift64 PRNG — zero cross-thread contention.

```http
GET /items/{RAND:1:100000} HTTP/1.1
Host: localhost:8080

```

At runtime: `/items/042357`, `/items/089124`, `/items/000017`, ...

### `{SEQ:start}` — globally incrementing counter

Each request gets a unique, monotonically increasing value starting at `start`. Uses a shared atomic counter across all threads — every request gets a different ID, guaranteed.

```http
POST /items HTTP/1.1
Host: localhost:8080
Content-Type: application/json
Content-Length: 72

{"id":{SEQ:100001},"name":"Bench","category":"test","price":100,"qty":50}
```

At runtime: `"id":100001`, `"id":100002`, `"id":100003`, ... (never repeats)

The width of the emitted value equals `digits(start)`, so the counter can grow within that digit band without changing Content-Length. Pick a `start` with enough digits to cover the expected number of increments — e.g. `{SEQ:10000001}` supports ~89M increments with 8-digit width. Starting near a power of ten (e.g. `{SEQ:999995}`) is not recommended, since the counter will quickly roll past the width.

### How it works

- `{RAND}` values are **zero-padded** to `digits(max)`, keeping Content-Length stable. Safe for URL path segments; avoid placing `{RAND}` inside a JSON number field unless `min` and `max` share the same digit count, since leading zeros are invalid JSON numbers per RFC 8259.
- `{SEQ}` values are written at `digits(start)` width — no leading zeros, so `{SEQ:start}` is safe inside JSON number fields.
- `{RAND}` uses per-connection state — no atomic operations, no lock contention
- `{SEQ}` uses a single `atomic_fetch_add` per request — ~10 ns overhead
- One placeholder per template (first `{RAND:` or `{SEQ:` found)
- Each template uses a per-connection scratch buffer (memcpy + overwrite at the placeholder offset before each send)

### CRUD example

```bash
gcannon http://localhost:8080 \
  --raw crud-read.raw,crud-read2.raw,crud-update.raw,crud-create.raw,crud-delete.raw \
  -c 512 -t 64 -d 5s
```

Where:
- `crud-read.raw`: `GET /crud/{RAND:1:100000}` — reads random existing rows
- `crud-update.raw`: `PUT /crud/{RAND:1:50000}` — updates random rows (first half)
- `crud-create.raw`: `POST /crud` with `{SEQ:100001}` in body — creates unique rows
- `crud-delete.raw`: `DELETE /crud/{RAND:50000:100000}` — deletes random rows (second half)

Each request hits a different ID. No contention, no exhaustion, no 404 storms.

## Modes

### HTTP (default)

Sends pipelined HTTP/1.1 requests. Supports keep-alive and short-lived connections.

```bash
# Keep-alive (max throughput)
gcannon http://localhost:8080/json -c 1024 -t 16 -d 10s -p 16

# Short-lived connections (reconnect after 100 requests)
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s -r 100

# Multiple request templates (round-robin on reconnect)
gcannon http://localhost:8080 --raw get.raw,post.raw,upload.raw -c 256 -t 4 -d 5s -r 5

# Larger recv buffers for big responses
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s --recv-buf 16384
```

### WebSocket

Upgrades each connection to WebSocket via HTTP/1.1, then sends/receives echo frames.

```bash
gcannon http://localhost:8080/ws --ws -c 4096 -t 64 -d 5s -p 16

# Custom message
gcannon http://localhost:8080/ws --ws-msg '{"type":"ping"}' -c 256 -t 8 -d 5s
```

### Fixed-rate mode (`--rate`)

```bash
# 250K req/s spread over 1M keep-alive connections: one request per
# connection every 4s, the fleet spread over 127.0.0.1-127.0.0.32
gcannon http://127.0.0.1:8080/ -c 1000000 -t 64 -d 20s \
    --rate 250000 --addr-span 32 --connect-limit 512 --warmup-max 3m
```

Every other mode sends the next request as soon as the last response arrives, so it answers "how fast can this server go". `--rate` holds the rate fixed instead and answers "what does this load cost, and how long does a request wait":

- **Each connection has its own schedule, on one shared clock.** The interval is connections / rate. Each thread spreads its connections evenly over the whole interval, with the threads' slots interleaved, so the fleet sends evenly spread instead of in waves however long it takes to connect, and no thread ever carries more than its share of the rate. A connection starts at its first slot after it is up.
- **Latency counts from the scheduled time** (coordinated-omission correction). A server that stalls makes every request scheduled during the stall wait, and each one is charged its wait: a 400 ms stall in a 3 s run at 2,000 req/s puts p90 above 100 ms, where timing from the send would only charge the requests already on the wire.
- **A late schedule is caught up, not reset.** A connection that fell behind - its response was slow, or it was reconnecting - sends its owed requests back to back, each still timed from its own slot.
- **Sends go out on time.** The event loop sleeps exactly until the next send is due, with the thread's timer slack set to 1 ns, so a request leaves within tens of microseconds of its slot. `Schedule:` reports the largest gap seen.

Output adds three lines (and a `rate` block plus `connections_active` in `--json`). From 20,000 connections at 5,000 req/s over four addresses, against an actix server:

```
  50008 requests in 10.00s, 50008 responses
  Throughput: 5.00K req/s
  ...
  Rate:       target 5000 req/s, achieved 5000 (100.00%)
  Schedule:   50008 paced sends, 0 more than 1ms late, max lag 765us
  Active:     20000 of 20000 connections open at the end
```

On stderr, `--rate` prints `gcannon: warmup done, measuring for <N>s` when the window opens and `gcannon: measurement done` when it closes, before any connection is torn down, so a wrapper can sample something else (a server's cgroup CPU, say) over exactly the measured window.

A large fleet needs `--addr-span` (one source address holds ~64K connections to one destination, so a server listening on `0.0.0.0` takes a million spread over 32 loopback addresses), `--connect-limit` (a million SYNs at once overflow any listen backlog, and every retry costs seconds), and a `--warmup-max` long enough to open them all. `--settle` then keeps the load running a while longer before the window opens (stderr: `gcannon: all connections up, settling for <N>s`): a server that has just accepted millions of connections can still be working through its accept backlog, or collecting garbage for the first time, and the window should measure what comes after.

When the window closes, every worker closes its own connections before gcannon exits, all workers at once, so the fleet is gone by the time gcannon returns (5 million held connections were otherwise left to the kernel, which closed them one at a time for minutes after exit). A run that follows straight away, or a server stopped right after, finds no half-closed fleet in the way. The teardown is not part of the window: `elapsed_sec` ends when the last worker stopped counting.

### HTTPS

`https://` targets run over TLS 1.3. OpenSSL does the handshake, then the record keys go to kernel TLS (kTLS), which encrypts and decrypts on the socket itself. The io_uring hot path is the same as for `http://`, and every mode works over TLS: pipelining, `--raw`, placeholders, `-r` and `--ws`.

```bash
gcannon https://localhost:8443/ -c 512 -t 8 -d 10s -p 16
gcannon https://localhost:8443/ws --ws -c 256 -t 8 -d 5s
```

- TLS 1.3 only, with the suites kernel TLS supports: AES-128-GCM (offered first), AES-256-GCM and ChaCha20-Poly1305.
- The server certificate is **not verified**. SNI is the URL's host name, and isn't sent for an IP address.
- Session tickets are ignored, so with `-r` every connection does a full handshake.
- Needs kernel TLS; gcannon checks for it at startup.
- Handshake failures count as connect errors. Failures after the handshake count as read errors: a record that doesn't decrypt, a fatal alert, or a server KeyUpdate (rekeying isn't supported). The first TLS failure's reason is printed to stderr.
- Bandwidth counts decrypted bytes.

## Output

### Default

```
gcannon — io_uring HTTP load generator
  Target:    localhost:8080/json
  Threads:   8
  Conns:     512 (64/thread)
  Pipeline:  16
  Duration:  5s

  Thread Stats   Avg      p50      p90      p99    p99.9
    Latency   1.82ms   1.70ms   2.50ms   4.90ms   7.10ms

  4823901 requests in 5.00s, 4823901 responses
  Throughput: 964.78K req/s
  Bandwidth:  72.36MB/s
  Status codes: 2xx=4823901, 3xx=0, 4xx=0, 5xx=0
  Reconnects: 0
```

### TUI Mode (`--tui`)

Enables a rich terminal interface with live progress and colored results.

**During execution** — progress bar, live throughput sparkline, and real-time stats:

```
  ████████████████████████████░░░░░░░░░░░░  70%   7s / 10s
  1.23M req/s  │  8.65M responses  │  456.78MB/s
  Req/s ▂▅▇█████▇▆▇████▇▆▅▆▇█              peak 1.45M
```

**Results** — colored tables with latency percentiles, throughput, status codes, and an adaptive latency histogram:

```
  Glass Cannon ── Results
  ──────────────────────────────────────────────────────────

  Latency

    ┌──────────┬──────────┬──────────┬──────────┬──────────┐
    │   Avg    │   p50    │   p90    │   p99    │  p99.9   │
    ├──────────┼──────────┼──────────┼──────────┼──────────┤
    │  1.82ms  │  1.70ms  │  2.50ms  │  4.90ms  │  7.10ms  │
    └──────────┴──────────┴──────────┴──────────┴──────────┘

  Throughput

    ┌──────────────┬──────────────────────────────────────┐
    │    Req/s     │ 964.78K                              │
    ├──────────────┼──────────────────────────────────────┤
    │  Bandwidth   │ 72.36MB/s                            │
    ├──────────────┼──────────────────────────────────────┤
    │    Total     │ 4.82M req / 4.82M resp in 5.00s     │
    └──────────────┴──────────────────────────────────────┘

  Status Codes

    ┌─────────────┬─────────────┬─────────────┬─────────────┐
    │     2xx     │     3xx     │     4xx     │     5xx     │
    ├─────────────┼─────────────┼─────────────┼─────────────┤
    │     4823901 │           0 │           0 │           0 │
    └─────────────┴─────────────┴─────────────┴─────────────┘

  Latency Distribution

    ≤  1.17ms  ██                                  2.45%
    ≤  1.34ms  ██████████████                     18.32%
    ≤  1.51ms  ██████████████████████████████     38.21%
    ≤  1.68ms  ████████████████████               24.56%
    ≤  1.85ms  ████████                            9.87%
    ≤  2.02ms  ████                                4.12%
    ≤  2.19ms  █                                   1.34%
    >  2.19ms  ▏                                   1.13%
```

The histogram adapts to your data — bucket boundaries are computed from the actual latency range, not fixed thresholds. Control granularity with `-b`:

```bash
gcannon http://localhost:8080/ --tui -b 30   # 30 buckets for more detail
gcannon http://localhost:8080/ --tui -b 5    # 5 buckets for a compact view
```

**Run history** — in TUI mode, bar graphs show req/s and avg latency across the last 10 runs (up to 100 stored in `~/.gcannon/history.bin`). The Y-axis adapts to the data range so small differences are visible. Each bar shows its value at the tip:

```
  Run History (5 runs)

    Req/s
              │                         3.51M
      3.52M │                         █████
            │               3.50M     █████
      3.49M │     3.48M     █████     █████
            │     █████     █████     █████
            │     █████     █████     █████
      3.46M └────────────────────────────────

    Avg Latency
            │ 152us
      154us │ █████
            │ █████                   148us
      149us │ █████     147us         █████
            │ █████     █████         █████
            │ █████     █████         █████
      144us └────────────────────────────────
```

Clear history with `gcannon --clear-history`.

Results are saved after every run (not just TUI mode) so the next `--tui` run always has history.

### Per-Template Latency

When using multiple raw request files, `--per-tpl-latency` tracks latency histograms per template:

```bash
gcannon http://localhost:8080 --raw get.raw,post.raw --per-tpl-latency -c 256 -t 4 -d 5s
```

Each template gets its own percentile breakdown (avg, p50, p90, p99, p99.9). Only allocated when the flag is passed — zero overhead otherwise.

### JSON Mode (`--json`)

Machine-readable output for scripts, CI pipelines, and dashboards. Prints a single JSON object to stdout with no banner or progress output.

```bash
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s --json
```

```json
{
  "target": "localhost:8080/",
  "mode": "http",
  "connections": 512,
  "threads": 8,
  "pipeline_depth": 1,
  "duration_sec": 10,
  "elapsed_sec": 10.001,
  "requests": 9647800,
  "responses": 9647800,
  "rps": 964702.93,
  "bandwidth_bps": 72352719.50,
  "bytes_read": 723548160,
  "latency_us": {
    "avg": 530,
    "p50": 490,
    "p90": 820,
    "p99": 1950,
    "p999": 4100,
    "samples": 9647780
  },
  "status": { "2xx": 9647800, "3xx": 0, "4xx": 0, "5xx": 0, "other": 0 },
  "errors": { "connect": 0, "read": 0, "timeout": 0 },
  "reconnects": 0
}
```

In WebSocket mode, additional `ws_upgrades` and `ws_frames` fields are included. Pipe to `jq` for filtering:

```bash
gcannon http://localhost:8080/ -c 512 -t 8 -d 5s --json | jq '.rps'
```

## How It Works

Each worker thread runs an independent io_uring event loop with zero cross-thread communication.

- **io_uring** with `SINGLE_ISSUER` + `DEFER_TASKRUN` for minimal kernel transitions
- **Provided buffer rings** for efficient recv (kernel picks buffers from a pre-registered pool)
- **Multishot recv** (one SQE arms continuous receive per connection)
- **Batch CQE processing** (up to 2048 completions per loop iteration)
- **Pre-built request buffers** (no per-request formatting)
- **Generation counters** in io_uring user-data to detect stale CQEs after reconnection
- **Kernel TLS** for `https://`: OpenSSL handles only the handshake, then the kernel encrypts and decrypts, so sends and receives stay plain io_uring ops on plaintext (receives use multishot recvmsg to see TLS record types)

```
main thread
  spawn N workers (warmup: >= 100ms and until every connection is set up;
                   reset stats, run for duration)
  join, aggregate stats, print

worker thread
  io_uring ring + provided buffer ring
  for each connection: connect -> send(pipeline) -> recv -> count -> refill -> [reconnect]
  at the end: shutdown(SHUT_RD) wakes every pending recv, reap, close its connections
```

## Building

```bash
make          # build
make clean    # clean
```

Requires Linux 6.1+ (for io_uring features), `liburing-dev` and `libssl-dev` (OpenSSL 3).

HTTP parsing uses [picohttpparser](https://github.com/h2o/picohttpparser) (vendored in `external/`).
