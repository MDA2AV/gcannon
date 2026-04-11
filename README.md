# Glass Cannon

[![Discord](https://discordapp.com/api/guilds/1177529388229734410/widget.png?style=shield)](https://discord.com/invite/H84B5ZqDXR)

High-performance load generator built on Linux io_uring. Supports HTTP/1.1 and WebSocket.

## [Http Arena](https://www.http-arena.com/) official Http/1.1 load generator

## Install

```bash
# Requires liburing-dev
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

# Raw request files (multiple templates)
gcannon http://localhost:8080 --raw get.raw,post.raw -c 256 -t 4 -d 5s
```

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `<url>` | required | Target URL (`http://` only) |
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
| `--inc-buf` | | Incremental buffer consumption (kernel 6.10+). The kernel fills buffers sequentially across recv completions instead of consuming a whole buffer per recv. Reduces buffer ring churn. |
| `--cqe-latency` | | Measure latency at CQE arrival instead of after response parsing |
| `--per-tpl-latency` | | Per-template latency histograms (with `--raw` and multiple templates) |
| `--clear-history` | | Clear saved run history and exit |

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

# Incremental buffer consumption (reduces buffer ring churn)
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s --inc-buf
```

### WebSocket

Upgrades each connection to WebSocket via HTTP/1.1, then sends/receives echo frames.

```bash
gcannon http://localhost:8080/ws --ws -c 4096 -t 64 -d 5s -p 16

# Custom message
gcannon http://localhost:8080/ws --ws-msg '{"type":"ping"}' -c 256 -t 8 -d 5s
```

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
- **Provided buffer rings** for efficient recv (kernel picks buffers from a pre-registered pool), with optional **incremental consumption** (`--inc-buf`) where the kernel fills buffers sequentially across multiple recvs instead of one-buffer-per-recv
- **Multishot recv** (one SQE arms continuous receive per connection)
- **Batch CQE processing** (up to 2048 completions per loop iteration)
- **Pre-built request buffers** (no per-request formatting)
- **Generation counters** in io_uring user-data to detect stale CQEs after reconnection

```
main thread
  spawn N workers (100ms warmup, reset stats, run for duration)
  join, aggregate stats, print

worker thread
  io_uring ring + provided buffer ring
  for each connection: connect -> send(pipeline) -> recv -> count -> refill -> [reconnect]
```

## Building

```bash
make          # build
make clean    # clean
```

Requires Linux 6.1+ (for io_uring features) and `liburing-dev`.

HTTP parsing uses [picohttpparser](https://github.com/h2o/picohttpparser) (vendored in `external/`).
