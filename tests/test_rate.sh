#!/usr/bin/env bash
#
# --rate: a fixed request rate spread over every connection, with latency
# measured from each request's scheduled time; and the two options a large
# paced fleet needs, --addr-span and --connect-limit.
#
#   rate               the achieved rate matches the one asked for
#   addr-span          a fleet spread over 127.0.0.1-.4 reaches a server on
#                      0.0.0.0 on every address ...
#   addr-span loopback ... and one bound to 127.0.0.1 only on the first
#   connect-limit      a throttled ramp still opens every connection
#   stall              a 400 ms server stall lands in the latency of every
#                      request scheduled inside it, not only the ones already
#                      on the wire (coordinated omission)
#   arg errors         --rate refuses -p > 1, -r, a non-positive rate, and
#                      --addr-span/--connect-limit refuse nonsense
#   closed loop        without --rate the output carries no rate lines
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
GCANNON="${GCANNON:-$ROOT/gcannon}"

if [[ ! -x "$GCANNON" ]]; then
    echo "error: $GCANNON not built — run 'make' first" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required" >&2
    exit 2
fi

TMP="$(mktemp -d)"
PIDFILE="$TMP/pids"
: >"$PIDFILE"
cleanup() {
    while read -r pid; do
        [[ -n "$pid" ]] && kill "$pid" 2>/dev/null
    done <"$PIDFILE"
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

failures=0
pass() { printf '    ok    %-19s %s\n' "$1" "$2"; }
fail() { printf '    FAIL  %-19s %s\n' "$1" "$2"; failures=$((failures + 1)); }

# Starts rate_server.py; sets $PORT.
start_server() {
    local tag="$1"; shift
    python3 "$HERE/rate_server.py" "$@" >"$TMP/$tag.out" 2>"$TMP/$tag.err" &
    echo $! >>"$PIDFILE"
    local _
    for _ in $(seq 1 100); do
        grep -q '^PORT ' "$TMP/$tag.out" 2>/dev/null && break
        sleep 0.05
    done
    PORT="$(awk '/^PORT /{print $2; exit}' "$TMP/$tag.out")"
    [[ -n "$PORT" ]]
}

# Runs gcannon with --json and prints the fields asked for, tab-separated.
# Dotted names reach into objects: rate.ratio, errors.connect.
run_json() {
    local fields="$1"; shift
    HOME="$TMP" "$GCANNON" "$@" --json 2>/dev/null | python3 -c '
import json, sys
fields = sys.argv[1].split(",")
try:
    d = json.load(sys.stdin)
except ValueError:
    print("\t".join("-" for _ in fields)); sys.exit()
out = []
for f in fields:
    v = d
    for k in f.split("."):
        v = v.get(k) if isinstance(v, dict) else None
    out.append("-" if v is None else str(v))
print("\t".join(out))
' "$fields"
}

echo
echo "gcannon --rate"
echo

if ! start_server any --host 0.0.0.0; then
    fail "server" "rate_server.py never started"
    exit 1
fi
ANY_PORT="$PORT"

# ── rate: 2000 req/s over 200 connections, one request each every 100 ms ──
IFS=$'\t' read -r rps ratio active sends <<<"$(run_json rps,rate.ratio,connections_active,rate.paced_sends \
    "http://127.0.0.1:$ANY_PORT/" -c 200 -t 2 -d 3s --rate 2000)"
if [[ "$ratio" == "-" ]]; then
    fail "rate" "no rate block in the JSON"
elif ! awk -v r="$ratio" 'BEGIN { exit !(r >= 0.95 && r <= 1.05) }'; then
    fail "rate" "rps=$rps ratio=$ratio, want 0.95-1.05 of 2000"
elif [[ "$active" != "200" ]]; then
    fail "rate" "connections_active=$active, want 200"
else
    pass "rate" "rps=$rps ratio=$ratio paced_sends=$sends active=$active"
fi

# ── addr-span: 127.0.0.1-.4 against a listener on every address ──
IFS=$'\t' read -r active cerr ratio <<<"$(run_json connections_active,errors.connect,rate.ratio \
    "http://127.0.0.1:$ANY_PORT/" -c 100 -t 2 -d 2s --rate 1000 --addr-span 4)"
if [[ "$active" == "100" && "$cerr" == "0" ]]; then
    pass "addr-span" "active=$active connect_errors=$cerr ratio=$ratio"
else
    fail "addr-span" "active=$active connect_errors=$cerr, want 100 and 0"
fi

# ── addr-span: a listener on 127.0.0.1 only refuses .2-.4 ──
if start_server loop --host 127.0.0.1; then
    IFS=$'\t' read -r active cerr <<<"$(run_json connections_active,errors.connect \
        "http://127.0.0.1:$PORT/" -c 40 -t 2 -d 1s --rate 400 --addr-span 4 --warmup-max 2s)"
    if [[ "$cerr" != "-" && "$cerr" -gt 0 && "$active" != "-" && "$active" -le 10 ]]; then
        pass "addr-span loopback" "active=$active connect_errors=$cerr"
    else
        fail "addr-span loopback" "active=$active connect_errors=$cerr, want <=10 and >0"
    fi
else
    fail "addr-span loopback" "server never started"
fi

# ── connect-limit: 4 first connects in flight still opens all 300 ──
IFS=$'\t' read -r active ratio <<<"$(run_json connections_active,rate.ratio \
    "http://127.0.0.1:$ANY_PORT/" -c 300 -t 1 -d 2s --rate 600 --connect-limit 4)"
if [[ "$active" == "300" ]]; then
    pass "connect-limit" "active=$active ratio=$ratio"
else
    fail "connect-limit" "active=$active, want 300"
fi

# ── stall: 400 ms at ~1s into a 3s run, 2000 req/s over 100 connections ──
# ~800 requests are scheduled inside the stall, 13% of the run, and with the
# latency taken from the schedule they wait up to 400 ms each, so p90 is in
# the tens of milliseconds at least. Timed from the actual send instead, only
# the ~100 already on the wire would wait and p90 would stay sub-millisecond.
if start_server stall --host 0.0.0.0 --stall-after 1.6 --stall-ms 400; then
    IFS=$'\t' read -r p90 p99 ratio <<<"$(run_json latency_us.p90,latency_us.p99,rate.ratio \
        "http://127.0.0.1:$PORT/" -c 100 -t 2 -d 3s --rate 2000)"
    if [[ "$p90" != "-" && "$p90" -ge 20000 && "$p99" -ge 200000 ]]; then
        pass "stall" "p90=${p90}us p99=${p99}us ratio=$ratio"
    else
        fail "stall" "p90=${p90}us p99=${p99}us - the stall did not reach the scheduled requests"
    fi
else
    fail "stall" "server never started"
fi

# ── arg errors ──
check_err() {
    local name="$1" want="$2"; shift 2
    local out rc
    out="$(HOME="$TMP" "$GCANNON" "$@" 2>&1)"; rc=$?
    if (( rc != 0 )) && grep -q -- "$want" <<<"$out"; then
        pass "$name" "$(head -1 <<<"$out")"
    else
        fail "$name" "rc=$rc out=$(head -1 <<<"$out")"
    fi
}
check_err "rate with -p 4"      "needs -p 1"        "http://127.0.0.1:1/" -c 4 -t 1 -d 1s -p 4 --rate 100
check_err "rate with -r"        "cannot be combined" "http://127.0.0.1:1/" -c 4 -t 1 -d 1s -r 10 --rate 100
check_err "rate 0"              "positive"          "http://127.0.0.1:1/" -c 4 -t 1 -d 1s --rate 0
check_err "addr-span 0"         "addr-span"         "http://127.0.0.1:1/" -c 4 -t 1 -d 1s --addr-span 0
check_err "connect-limit -1"    "connect-limit"     "http://127.0.0.1:1/" -c 4 -t 1 -d 1s --connect-limit -1

# ── closed loop: no rate lines unless --rate ──
out="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$ANY_PORT/" -c 8 -t 1 -d 1s 2>&1)"
if grep -qE '^  (Rate|Schedule|Active|Addresses):' <<<"$out"; then
    fail "closed loop" "rate lines printed without --rate"
elif ! grep -q 'Status codes: 2xx=' <<<"$out"; then
    fail "closed loop" "the usual results are missing"
else
    pass "closed loop" "output unchanged"
fi

echo
if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
