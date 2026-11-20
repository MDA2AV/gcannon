#!/usr/bin/env bash
#
# Teardown: gcannon closes its own connections before it exits.
#
#   closed loop        once gcannon has returned, none of its connections to
#                      the server is still established. Through 0.8.2 nearly
#                      all of them were: each socket's multishot recv held it
#                      open past exit, and the kernel then closed the fleet one
#                      connection after another
#   rate               the same for a --rate fleet ...
#   rate window        ... and closing it is no part of the window: elapsed_sec
#                      stays at the duration
#   reconnect (-r)     connections retired by -r (SO_LINGER 0) close the same way
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
GCANNON="${GCANNON:-$ROOT/gcannon}"
CONNS=10000

if [[ ! -x "$GCANNON" ]]; then
    echo "error: $GCANNON not built — run 'make' first" >&2
    exit 2
fi
for tool in python3 ss; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: $tool is required" >&2
        exit 2
    fi
done
# One fd per connection, for gcannon and for the server
ulimit -n "$(ulimit -Hn)" 2>/dev/null
if [[ "$(ulimit -n)" != unlimited ]] && (( $(ulimit -n) < CONNS + 1024 )); then
    echo "error: needs RLIMIT_NOFILE >= $((CONNS + 1024)), have $(ulimit -n)" >&2
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

python3 "$HERE/rate_server.py" --host 127.0.0.1 >"$TMP/server.out" 2>"$TMP/server.err" &
echo $! >>"$PIDFILE"
for _ in $(seq 1 100); do
    grep -q '^PORT ' "$TMP/server.out" 2>/dev/null && break
    sleep 0.05
done
PORT="$(awk '/^PORT /{print $2; exit}' "$TMP/server.out")"

# gcannon's connections to the server that are still established
established() { ss -Htn state established "( dport = :$PORT )" | wc -l; }

# Runs gcannon (--json) to completion, then counts what it left established.
# Prints: <established after exit> <elapsed_sec> <connections_active>
run_and_count() {
    HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/" "$@" --json >"$TMP/run.json" 2>"$TMP/run.err"
    local left
    left="$(established)"
    python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except ValueError:
    d = {}
print(sys.argv[2], d.get("elapsed_sec", "-"), d.get("connections_active", "-"))
' "$TMP/run.json" "$left"
}

# Waits for the previous case's sockets to go, so each case starts clean
settle_host() {
    for _ in $(seq 1 100); do
        (( $(established) == 0 )) && return
        sleep 0.1
    done
}

echo
echo "gcannon teardown"
echo

if [[ -z "$PORT" ]]; then
    fail "server" "rate_server.py never started"
    exit 1
fi

# ── closed loop: 10K connections, every one of them closed by gcannon ──
read -r left el _ <<<"$(run_and_count -c "$CONNS" -t 2 -d 1s --connect-limit 256)"
if [[ "$el" == "-" ]]; then
    fail "closed loop" "no JSON summary: $(head -c 200 "$TMP/run.err")"
elif (( left > 0 )); then
    fail "closed loop" "$left of $CONNS connections still established after gcannon exited"
else
    pass "closed loop" "0 of $CONNS established after exit"
fi
settle_host

# ── rate: a paced 10K fleet, held to the end of the window ──
read -r left el active <<<"$(run_and_count -c "$CONNS" -t 2 -d 2s --rate 2000 --connect-limit 256)"
if [[ "$el" == "-" ]]; then
    fail "rate" "no JSON summary: $(head -c 200 "$TMP/run.err")"
elif [[ "$active" != "$CONNS" ]]; then
    fail "rate" "connections_active=$active, want $CONNS held to the end"
elif (( left > 0 )); then
    fail "rate" "$left of $CONNS connections still established after gcannon exited"
else
    pass "rate" "0 of $active established after exit"
fi
if [[ "$el" != "-" ]] && ! awk -v e="$el" 'BEGIN { exit !(e >= 1.95 && e <= 2.2) }'; then
    fail "rate window" "elapsed_sec=$el, want ~2: the teardown must not be in the window"
elif [[ "$el" != "-" ]]; then
    pass "rate window" "elapsed_sec=$el for -d 2s"
fi
settle_host

# ── -r: connections retired every 10 requests (SO_LINGER 0) ──
read -r left el _ <<<"$(run_and_count -c 2000 -t 2 -d 1s -r 10 --connect-limit 256)"
if [[ "$el" == "-" ]]; then
    fail "reconnect (-r)" "no JSON summary: $(head -c 200 "$TMP/run.err")"
elif (( left > 0 )); then
    fail "reconnect (-r)" "$left of 2000 connections still established after gcannon exited"
else
    pass "reconnect (-r)" "0 of 2000 established after exit"
fi

echo
if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
