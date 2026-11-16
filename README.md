# gcannon

[![Discord](https://discordapp.com/api/guilds/1177529388229734410/widget.png?style=shield)](https://discord.com/invite/H84B5ZqDXR)

**Glass Cannon** is a load generator for HTTP/1.1, WebSocket and HTTPS on Linux, built on io_uring and written in C. It is the load generator behind [HttpArena](https://http-arena.com/)'s HTTP/1.1 and WebSocket tests.

Website and manual: **[gcannon.org](https://gcannon.org/)**

- [Install](#install)
- [Quick start](#quick-start)
- [Options](#options)
- [Request templates](#request-templates)
- [Modes](#modes)
- [Output](#output)
- [How it works](#how-it-works)
- [Source layout](#source-layout)
- [Building and testing](#building-and-testing)

## Install

You need:

- Linux 6.1 or newer, with io_uring
- liburing and OpenSSL 3 headers (`liburing-dev`, `libssl-dev`), a C compiler and `make`
- for `https://` targets, kernel TLS; gcannon checks for it at startup

```bash
sudo apt install build-essential liburing-dev libssl-dev   # Debian / Ubuntu
git clone https://github.com/MDA2AV/gcannon.git && cd gcannon
make
sudo cp gcannon /usr/local/bin/
gcannon --version
```

gcannon uses liburing's provided-buffer-ring helpers (`io_uring_setup_buf_ring`). Ubuntu 24.04's liburing 2.5 has them; on an older distribution, build liburing from source. HTTP parsing uses [picohttpparser](https://github.com/h2o/picohttpparser), vendored in `external/`, so there is nothing else to fetch.

## Quick start

A URL and three numbers: connections (`-c`), threads (`-t`) and duration (`-d`).

```bash
# 512 keep-alive connections on 8 threads for 10 seconds
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s

# The same with 16 requests in flight per connection
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s -p 16

# WebSocket echo
gcannon http://localhost:8080/ws --ws -c 512 -t 8 -d 10s

# HTTPS (TLS 1.3)
gcannon https://localhost:8443/ -c 512 -t 8 -d 10s

# Your own requests, from raw files
gcannon http://localhost:8080 --raw get.raw,post.raw -c 256 -t 4 -d 5s
```

Four things to get right:

- **`-c` is the total**, split evenly over the threads, so `-c 512 -t 8` is 64 connections per thread. `-c` must be at least `-t`.
- **Give gcannon its own cores.** If it shares cores with the server, you measure the two fighting for them. Pin them apart, for example with `taskset`.
- **The run starts after a warmup.** gcannon waits at least 100 ms and until every connection is set up, zeroes its counters, then measures for exactly `-d`.
- **Check the exit code.** gcannon expects status 200 (change it with `-s`). If responses come back in another status class, it prints a warning and exits with code 1.

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `<url>` | required | Target, `http://` or `https://`. The port defaults to 80 or 443 and the path to `/`. Optional with `--raw`: host and port then come from the first file's `Host:` header |
| `-c N` | 100 | Total connections, split evenly over the threads. At least 1, and at least `-t` |
| `-t N` | 1 | Worker threads |
| `-d D` | 10s | Measured duration: `5s`, `1m`, or a bare number of seconds. At least 1 second |
| `-p N` | 1 | Pipeline depth: requests (or WebSocket frames) in flight per connection. Values above 64 are capped at 64 |
| `-r N` | unlimited | Requests per connection; after N the connection is closed and reopened |
| `-s CODE` | 200 | Expected status. Responses outside its class (2xx, 3xx, 4xx, 5xx) make gcannon warn and exit with code 1 |
| `--raw F,...`, `-R F,...` | | Comma-separated request files, sent byte for byte, at most 64. See [Request templates](#request-templates) |
| `--ws` | | WebSocket echo mode |
| `--ws-msg TEXT` | `hello` | WebSocket message payload; turns on WebSocket mode |
| `--tui` | | Terminal UI: progress bar, live sparkline, coloured results, latency histogram, run history |
| `--json` | | Print one JSON object to stdout, with no banner or progress |
| `-b N`, `--buckets N` | 10 | Latency histogram buckets in TUI mode (adaptive, at most 100) |
| `--recv-buf BYTES` | 4096 | Receive buffer size, 512 to 4194304. Each worker allocates 4096 buffers of this size: larger values suit big responses, smaller ones save memory |
| `--cqe-latency` | | Stop the latency clock when the completion arrives, before the response is parsed |
| `--per-tpl-latency` | | A latency histogram per template (with `--raw` and more than one file) |
| `--rate N` | closed loop | Fixed total request rate (req/s), paced per connection, with latency counted from each request's scheduled time. Needs `-p 1` and no `-r`. See [Fixed rate](#fixed-rate---rate) |
| `--addr-span N` | 1 | Spread the connections over N consecutive IPv4 addresses from the target's (`127.0.0.1`, `.2`, ...), 1 to 65536 |
| `--connect-limit N` | all at once | First connection attempts in flight per thread; each completion starts the next |
| `--warmup-max D` | 10s | Longest the warmup waits for connections to be set up before measuring anyway |
| `--version` | | Print the version and exit |
| `--clear-history` | | Clear the run history (`~/.gcannon/history.bin`) and exit |

Arguments are checked before anything connects, and a bad combination is refused with the reason, for example `Error: --rate needs -p 1 (got -p 16)`.

## Request templates

Without `--raw`, gcannon sends a GET for the URL. With `--raw`, it sends the exact bytes of each file, so you choose the method, headers and body: mixed GET and POST, chunked bodies, a byte-exact copy of a real request.

Each file must be a complete HTTP/1.1 request, with CRLF line endings and the empty line that ends the headers. `printf` is a reliable way to write one:

```bash
printf 'GET /json HTTP/1.1\r\nHost: localhost:8080\r\n\r\n' > get.raw
printf 'POST /echo HTTP/1.1\r\nHost: localhost:8080\r\nContent-Length: 2\r\n\r\n20' > post.raw
```

Templates are handed out round-robin over the connections, so each connection sends one kind of request, and with `-r` a connection moves to the next template every time it reconnects.

### Placeholders

A template can carry one placeholder, which gcannon fills in on every request at send time. That is what makes create, read, update and delete benchmarks work: every request can address a different row.

**`{RAND:min:max}`** is a random number between `min` and `max`, inclusive, from a per-connection xorshift64 generator, so threads never contend for it.

```http
GET /items/{RAND:1:100000} HTTP/1.1
Host: localhost:8080

```

On the wire: `/items/042357`, `/items/089124`, `/items/000017`, ...

**`{SEQ:start}`** counts up by one per request from `start`, from a single atomic counter shared by all threads, so no two requests get the same value.

```http
POST /items HTTP/1.1
Host: localhost:8080
Content-Type: application/json
Content-Length: 72

{"id":{SEQ:100001},"name":"Bench","category":"test","price":100,"qty":50}
```

On the wire: `"id":100001`, `"id":100002`, `"id":100003`, ...

The value always has the same width, so `Content-Length` never changes: write it for the substituted value, not for the placeholder text.

- `{RAND}` is zero-padded to the digits of `max`. That is fine in a URL path, but a JSON number can't start with a zero, so inside one use it only when `min` and `max` have the same number of digits.
- `{SEQ}` is written at the digits of `start`, with no leading zeros, so it is safe inside JSON numbers. Pick a `start` with room to grow: `{SEQ:10000001}` allows about 89 million increments at 8 digits, while `{SEQ:999995}` runs out of width almost at once.
- Only the first `{RAND:` or `{SEQ:` in a template is replaced.
- With pipelining, each request in the batch gets its own value.

A CRUD run:

```bash
gcannon http://localhost:8080 \
  --raw crud-read.raw,crud-read2.raw,crud-update.raw,crud-create.raw,crud-delete.raw \
  -c 512 -t 64 -d 5s
```

- `crud-read.raw`: `GET /crud/{RAND:1:100000}` reads random existing rows
- `crud-update.raw`: `PUT /crud/{RAND:1:50000}` updates rows in the first half
- `crud-create.raw`: `POST /crud` with `{SEQ:100001}` in the body creates unique rows
- `crud-delete.raw`: `DELETE /crud/{RAND:50000:100000}` deletes rows in the second half

## Modes

### Keep-alive HTTP (default)

Every connection stays open for the whole run and sends its next request as soon as the previous response has arrived.

```bash
gcannon http://localhost:8080/json -c 1024 -t 16 -d 10s

# Larger receive buffers for big responses
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s --recv-buf 16384
```

### Pipelining (`-p`)

Each connection keeps N requests in flight. They are built into one buffer before the run and go out in one send; as responses complete, the same number of new requests follow.

```bash
gcannon http://localhost:8080/json -c 1024 -t 16 -d 10s -p 16
```

The server has to support HTTP/1.1 pipelining. Requests sent together share one timestamp, so a pipelined latency includes the time a response waited behind the ones before it.

### Short-lived connections (`-r`)

`-r N` closes each connection after N requests and opens a new one, so the run includes connection setup. `Reconnects:` in the output counts them.

```bash
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s -r 100
```

### WebSocket (`--ws`)

Each connection upgrades to WebSocket over HTTP/1.1, then sends frames and counts the echoes. The target is still an `http://` or `https://` URL, and `-p` sets how many frames are in flight per connection.

```bash
gcannon http://localhost:8080/ws --ws -c 4096 -t 64 -d 5s -p 16

# Custom message
gcannon http://localhost:8080/ws --ws-msg '{"type":"ping"}' -c 256 -t 8 -d 5s
```

The summary reports frames sent and received, `WS upgrades:`, `WS frames:` and, if the server refused any, `WS upgrades refused:`.

### HTTPS

An `https://` URL runs over TLS 1.3. OpenSSL does the handshake, then the record keys go to kernel TLS, which encrypts and decrypts on the socket itself. The io_uring hot path is the same as for `http://`, and every mode works over TLS: pipelining, `--raw`, placeholders, `-r` and `--ws`.

```bash
gcannon https://localhost:8443/ -c 512 -t 8 -d 10s -p 16
gcannon https://localhost:8443/ws --ws -c 256 -t 8 -d 5s
```

- TLS 1.3 only, with the suites kernel TLS supports: AES-128-GCM (offered first), AES-256-GCM and ChaCha20-Poly1305.
- The server certificate is **not verified**. SNI is the URL's host name, and is not sent for an IP address.
- Session tickets are ignored, so with `-r` every connection does a full handshake.
- Kernel TLS is required, and gcannon checks for it at startup.
- Handshake failures count as connect errors. Failures after the handshake count as read errors: a record that doesn't decrypt, a fatal alert, or a server KeyUpdate (rekeying isn't supported). The reason for the first TLS failure is printed to stderr.
- Bandwidth counts decrypted bytes.

### Fixed rate (`--rate`)

Every other mode sends the next request as soon as the last response arrives, so it answers "how fast can this server go". `--rate` holds the total rate fixed instead and answers "what does this load cost, and how long does a request wait".

```bash
# 5,000 req/s over 200 keep-alive connections
gcannon http://localhost:8080/ -c 200 -t 4 -d 30s --rate 5000
```

- **Each connection has its own schedule, on one shared clock.** The interval is connections / rate. Connection *g* of *N* sends at *g/N* of the interval and every interval after that, so the fleet sends evenly spread instead of in waves, however long the connections take to open. A connection starts at its first slot after it is up.
- **Latency counts from the scheduled time.** This is coordinated-omission correction: a server that stalls makes every request scheduled during the stall wait, and each one is charged its wait. Timing from the send would only charge the requests already on the wire. `tests/integration/test_rate.sh` checks this with a 400 ms stall.
- **A late schedule is caught up, not reset.** A connection that fell behind, because a response was slow or it was reconnecting, sends what it owes back to back, each request still timed from its own slot.
- **Sends go out on time.** The event loop sleeps exactly until the next send is due, with the thread's timer slack set to 1 ns. The `Schedule:` line reports how many sends left more than 1 ms late and the largest lag, so every run checks its own generator.

`--rate` needs `-p 1` and cannot be combined with `-r`. It adds three lines to the output, plus a `rate` block and `connections_active` in `--json`:

```
  Rate:       target <rate> req/s, achieved <rate> (<percent>%)
  Schedule:   <n> paced sends, <n> more than 1ms late, max lag <time>
  Active:     <n> of <connections> connections open at the end
```

On stderr, `--rate` prints `gcannon: warmup done, measuring for <N>s` when the measured window opens and `gcannon: measurement done` when it closes, before any connection is torn down. A wrapper can use them to sample something else, such as the server's cgroup CPU, over exactly the measured window.

**Very large fleets.** Fixed rate is how you hold hundreds of thousands of mostly idle connections and still send at a known rate. Three options make that work:

- `--addr-span`: one source address holds at most about 64K connections to one destination address and port. Every 127.x.y.z address is local, so a server listening on `0.0.0.0` can take a million connections spread over 32 loopback addresses.
- `--connect-limit`: a million SYNs at once overflow any listen backlog, and every retry costs seconds.
- `--warmup-max`: long enough to open them all.

```bash
# 250K req/s over 1M keep-alive connections: one request per connection
# every 4 s, the fleet spread over 127.0.0.1-127.0.0.32
gcannon http://127.0.0.1:8080/ -c 1000000 -t 64 -d 20s \
    --rate 250000 --addr-span 32 --connect-limit 512 --warmup-max 3m
```

## Output

### Default

A banner with the settings, then the results. The numbers here are illustrative, not a measurement:

```
gcannon v0.8.0
  Target:    localhost:8080/json
  Threads:   8
  Conns:     512 (64/thread)
  Pipeline:  16
  Req/conn:  unlimited (keep-alive)
  Expected:  200
  Duration:  5s


  Thread Stats   Avg      p50      p90      p99    p99.9
    Latency   1.82ms   1.70ms   2.50ms   4.90ms   7.10ms

  4823901 requests in 5.00s, 4823901 responses
  Throughput: 964.78K req/s
  Bandwidth:  72.36MB/s
  Status codes: 2xx=4823901, 3xx=0, 4xx=0, 5xx=0
  Latency samples: 4823901 / 4823901 responses (100.0%)
```

Some lines appear only when they apply:

- the banner adds `TLS:` for `https://`, `Templates:` for several `--raw` files, and `Addresses:` and `Rate:` for `--addr-span` and `--rate`
- the first line names the active modes in brackets, for example `gcannon v0.8.0 [WS, CQE]` (`WS`, `TUI`, `CQE`, `TPL`)
- the summary adds `Reconnects:`, `Errors: connect N, read N, timeout N` and `Latency overflow (>5s):` when they are not zero, `Per-template:` and `Per-template-ok:` counts with several templates, and the `--rate` lines above

### JSON (`--json`)

One JSON object on stdout and nothing else, for scripts, CI and dashboards:

```bash
gcannon http://localhost:8080/ -c 512 -t 8 -d 10s --json
gcannon http://localhost:8080/ -c 512 -t 8 -d 5s --json | jq '.rps'
```

```json
{
  "version": "0.8.0",
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
  "status": {
    "2xx": 9647800,
    "3xx": 0,
    "4xx": 0,
    "5xx": 0,
    "other": 0
  },
  "errors": {
    "connect": 0,
    "read": 0,
    "timeout": 0
  },
  "reconnects": 0
}
```

WebSocket mode adds `ws_upgrades`, `ws_upgrade_failures` and `ws_frames`. Several templates add a `per_template` array (responses and 2xx per template, plus `latency_us` with `--per-tpl-latency`). `--rate` adds the `rate` block (`target`, `achieved`, `ratio`, `paced_sends`, `late`, `max_lag_us`) and `connections_active`.

### Terminal UI (`--tui`)

While the run goes, a progress bar, live throughput and a sparkline of requests per second:

```
  ████████████████████████████░░░░░░░░░░░░  70%   7s / 10s
  1.23M req/s  │  8.65M responses  │  456.78MB/s
  Req/s ▂▅▇█████▇▆▇████▇▆▅▆▇█              peak 1.45M
```

At the end, coloured tables for latency, throughput and status codes, then a latency histogram whose bucket boundaries come from the latencies actually seen:

```
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

`-b` sets the number of buckets, for example `--tui -b 30` for more detail or `--tui -b 5` for a compact view.

TUI mode also draws bar graphs of requests per second and average latency over the last 10 runs. Every run, in any output mode, is saved to `~/.gcannon/history.bin` (up to 100 runs); `gcannon --clear-history` deletes them.

### Per-template latency (`--per-tpl-latency`)

With several `--raw` files, this keeps a separate latency histogram per template, so a slow POST can't hide behind a fast GET:

```bash
gcannon http://localhost:8080 --raw get.raw,post.raw --per-tpl-latency -c 256 -t 4 -d 5s
```

The summary gains a `Per-template latency:` table, one row per template in the order the files were listed, with average, p50, p90, p99 and p99.9. The histograms are allocated only when the flag is given.

## How it works

The main thread starts the workers, waits for the warmup, lets them run for the duration, then joins them, merges their statistics and prints. All the load comes from the workers, and none of them waits on another.

- **One io_uring ring per worker**, set up with `SINGLE_ISSUER` and `DEFER_TASKRUN`, and its own share of the connections
- **A provided buffer ring**: each worker registers 4096 receive buffers with the kernel up front, the kernel picks one for every arrival of data, and the worker hands it back after parsing
- **Multishot receive**: one submission arms a receive that keeps producing completions, with no system call per receive (`https://` uses multishot recvmsg, to see TLS record types)
- **Batched completions**: up to 2048 per loop iteration
- **Requests built before the run**: a whole pipeline goes out in one send, and a placeholder is written into a per-connection copy
- **Generation counters** in each submission's user data, so a completion for a connection that has since reconnected is recognised and dropped
- **A latency histogram per worker**: 1 µs buckets up to 10 ms, then 100 µs buckets up to 5 s, merged at the end
- **Kernel TLS** for `https://`: OpenSSL does only the handshake, then the kernel encrypts and decrypts, so sends and receives stay ordinary io_uring operations on plaintext

```
main thread
  spawn N workers (warmup: >= 100ms and until every connection is set up;
                   reset stats, run for duration)
  join, aggregate stats, print

worker thread
  io_uring ring + provided buffer ring
  for each connection: connect -> send(pipeline) -> recv -> count -> refill -> [reconnect]
```

## Source layout

```
src/
  main.c         the run: target, setup, warmup, measurement, exit code
  cli.c          options, defaults, and the checks made before anything connects
  core/          the engine
    worker.c     one thread's io_uring event loop and its connections
    send.h       the send path, shared by the loop and the scheduler
    pace.c       the --rate scheduler
    stats.c      counters and latency histograms
    constants.h  version, ring sizes, limits, completion tagging
  proto/         what goes over the wire
    template.c   requests: the URL's GET, --raw files, {RAND}/{SEQ}
    http.c       HTTP/1.1 response parser
    ws.c         WebSocket upgrade, frames and frame parser
    tls.c        TLS 1.3 handshake with OpenSSL
    ktls.c       TLS 1.3 record keys into kernel TLS, record reads
  output/        what a run prints and keeps
    report.c     the banner, and the text, JSON and --rate results
    tui.c        --tui
    history.c    run history
tests/
  unit/          C tests: HTTP parser, buffer ring, kernel TLS keys
  integration/   shell tests that run gcannon against the servers below
  servers/       small Python test servers
external/picohttpparser/
```

Each `.c` file has its header next to it, and includes name the folder (`#include "core/worker.h"`).

## Building and testing

```bash
make                    # build ./gcannon
make clean              # remove build/ and the binary
make test               # unit and integration tests
make test-unit          # C tests only
make test-integration   # tests/integration/test_*.sh against a fresh build
```

The integration tests need `python3`. The TLS tests also need kernel TLS and the `openssl` command, and are skipped without them. gcannon saves every run to `~/.gcannon/history.bin`, so the tests point `HOME` at a temporary directory.

## License

MIT, see [LICENSE](LICENSE).
