#!/usr/bin/env bash
#
# --ramp-p99: grow the fleet a step at a time until p99 goes over a threshold.
# ramp_server.py --service-ms answers one request at a time, so N connections
# wait about N * S each and the ramp has a known place to stop.
#
#   knee           2 ms per request, 12 ms threshold: 1, 2 and 4 connections
#                  pass, 8 fails, is measured again, and the ramp stops; the
#                  results are the 4-connection step's
#   ceiling        every step passes up to -c, in +2 steps over two threads,
#                  each with all of its connections up
#   status         503s from 8 connections fail the step even with p99 low
#   silent         connections the server never answers fail the step, though
#                  the ones it does answer keep p99 far under the threshold
#   sockets        connections that can't get a socket (ulimit -n) fail the
#                  step at once, without a second window
#   no step        a threshold the first step can't meet: text output says
#                  so, and the exit code is 1
#   ctrl-c         before the first step is measured: nothing to report, 130;
#                  after one passed: its results, stopped "interrupted"
#   arg errors     --ramp needs --ramp-p99 or --ramp-up, --ramp-step needs
#                  --ramp-p99, which needs a unit,
#                  stays under 5s, and refuses --rate and --tui; --ramp sits
#                  between -t and -c; --ramp-step takes x<factor> or +<n>
#   no ramp        without --ramp-p99 the output carries no ramp lines
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

# Starts ramp_server.py; sets $PORT.
start_server() {
    local tag="$1"; shift
    python3 "$HERE/ramp_server.py" "$@" >"$TMP/$tag.out" 2>"$TMP/$tag.err" &
    echo $! >>"$PIDFILE"
    local _
    for _ in $(seq 1 100); do
        grep -q '^PORT ' "$TMP/$tag.out" 2>/dev/null && break
        sleep 0.05
    done
    PORT="$(awk '/^PORT /{print $2; exit}' "$TMP/$tag.out")"
    [[ -n "$PORT" ]]
}

# Runs gcannon with --json and prints, tab-separated: its exit code, the
# reported connections, ramp.max_connections, ramp.stopped, then one field per
# step joined by commas - each step's connections, retry, pass, p99, set_up
# and active - so a check can compare whole sequences.
run_ramp() {
    HOME="$TMP" "$GCANNON" "$@" --json 2>/dev/null >"$TMP/run.json"
    local rc=$?
    python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    r = d["ramp"]
except (ValueError, KeyError):
    print("\t".join([sys.argv[2]] + ["-"] * 9)); sys.exit()
steps = r["steps"]
col = lambda f: ",".join(str(f(s)).lower() for s in steps)
print("\t".join([sys.argv[2], str(d["connections"]), str(r["max_connections"]), r["stopped"],
                 col(lambda s: s["connections"]), col(lambda s: s["retry"]),
                 col(lambda s: s["pass"]), col(lambda s: s["latency_us"]["p99"]),
                 col(lambda s: s["set_up"]), col(lambda s: s["active"])]))
' "$TMP/run.json" "$rc"
}

echo
echo "gcannon --ramp-p99"
echo

# ── knee: p99 ~ N * 2 ms against 12 ms ──
if start_server serial --service-ms 2; then
    IFS=$'\t' read -r rc conns max stopped steps retries passes p99s _ _ <<<"$(run_ramp \
        "http://127.0.0.1:$PORT/" -t 1 -c 32 -d 1s --ramp 1 --ramp-p99 12ms)"
    if [[ "$rc" == 0 && "$max" == 4 && "$conns" == 4 && "$stopped" == p99 &&
          "$steps" == "1,2,4,8,8" && "$retries" == "false,false,false,false,true" &&
          "$passes" == "true,true,true,false,false" ]]; then
        pass "knee" "steps=$steps p99=$p99s max=$max"
    else
        fail "knee" "rc=$rc conns=$conns max=$max stopped=$stopped steps=$steps retry=$retries pass=$passes p99=$p99s"
    fi
    SERIAL_PORT="$PORT"
else
    fail "knee" "ramp_server.py never started"
fi

if ! start_server fast; then
    fail "server" "ramp_server.py never started"
    exit 1
fi
FAST_PORT="$PORT"

# ── ceiling: +2 steps over two threads, one first connect in flight each ──
IFS=$'\t' read -r rc conns max stopped steps _ passes _ setup active <<<"$(run_ramp \
    "http://127.0.0.1:$FAST_PORT/" -t 2 -c 8 -d 1s --ramp 2 --ramp-step +2 --ramp-p99 1s \
    --connect-limit 1)"
if [[ "$rc" == 0 && "$max" == 8 && "$conns" == 8 && "$stopped" == ceiling &&
      "$steps" == "2,4,6,8" && "$passes" == "true,true,true,true" &&
      "$setup" == "$steps" && "$active" == "$steps" ]]; then
    pass "ceiling" "steps=$steps active=$active"
else
    fail "ceiling" "rc=$rc conns=$conns max=$max stopped=$stopped steps=$steps pass=$passes set_up=$setup active=$active"
fi

# ── status: 503 while more than 4 connections are open ──
if start_server busy --fail-over 4; then
    IFS=$'\t' read -r rc _ max stopped steps _ _ _ _ _ <<<"$(run_ramp \
        "http://127.0.0.1:$PORT/" -t 1 -c 16 -d 1s --ramp 2 --ramp-p99 1s)"
    if [[ "$rc" == 0 && "$max" == 4 && "$stopped" == status && "$steps" == "2,4,8,8" ]]; then
        pass "status" "steps=$steps max=$max"
    else
        fail "status" "rc=$rc max=$max stopped=$stopped steps=$steps"
    fi
else
    fail "status" "server never started"
fi

# ── silent: connections past the 4th are never answered ──
if start_server hold --hold-over 4; then
    IFS=$'\t' read -r rc _ max stopped steps _ _ p99s _ _ <<<"$(run_ramp \
        "http://127.0.0.1:$PORT/" -t 1 -c 16 -d 1s --ramp 2 --ramp-p99 500ms)"
    p99_at_8="${p99s##*,}"
    if [[ "$rc" == 0 && "$max" == 4 && "$stopped" == silent && "$steps" == "2,4,8,8" &&
          "$p99_at_8" =~ ^[0-9]+$ && "$p99_at_8" -lt 500000 ]]; then
        pass "silent" "steps=$steps max=$max (p99 at 8: ${p99_at_8}us)"
    else
        fail "silent" "rc=$rc max=$max stopped=$stopped steps=$steps p99=$p99s"
    fi
else
    fail "silent" "server never started"
fi

# ── sockets: 24 file descriptors run out before 32 connections ──
IFS=$'\t' read -r rc _ max stopped steps retries _ _ _ _ <<<"$(ulimit -n 24; run_ramp \
    "http://127.0.0.1:$FAST_PORT/" -t 1 -c 64 -d 1s --ramp 4 --ramp-p99 1s)"
if [[ "$rc" == 0 && "$stopped" == sockets && "$max" =~ ^[0-9]+$ && "$max" -ge 4 &&
      "${retries##*,}" == false ]]; then
    pass "sockets" "steps=$steps max=$max, no second window"
else
    fail "sockets" "rc=$rc max=$max stopped=$stopped steps=$steps retry=$retries"
fi

# ── no step: 1 ms is out of reach of a 2 ms-per-request server ──
if [[ -n "${SERIAL_PORT:-}" ]]; then
    out="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$SERIAL_PORT/" -t 1 -c 8 -d 1s \
           --ramp 1 --ramp-p99 1ms 2>&1)"; rc=$?
    lines="$(grep -cE '^ +1 .*p99 over 1\.00ms' <<<"$out")"
    if (( rc == 1 )) && [[ "$lines" == 2 ]] &&
       grep -q 'measuring again' <<<"$out" &&
       grep -q 'Ramp:       held p99 within 1.00ms at no step; at 1, p99 over 1.00ms' <<<"$out"; then
        pass "no step" "exit 1, both windows over"
    else
        fail "no step" "rc=$rc step lines=$lines: $(grep 'Ramp:' <<<"$out" | tail -1)"
    fi
fi

# ── ctrl-c ──
interrupt() {   # $1: seconds before SIGINT; then gcannon's arguments
    local after="$1"; shift
    HOME="$TMP" "$GCANNON" "$@" >"$TMP/intr.out" 2>"$TMP/intr.err" &
    local pid=$!
    sleep "$after"
    kill -INT "$pid"
    wait "$pid"
}
interrupt 0.6 "http://127.0.0.1:$FAST_PORT/" -t 1 -c 8 -d 1s --ramp 2 --ramp-p99 1s
rc=$?
if (( rc == 130 )) && grep -q 'nothing to report' "$TMP/intr.err"; then
    pass "ctrl-c first step" "exit 130"
else
    fail "ctrl-c first step" "rc=$rc $(tail -1 "$TMP/intr.err")"
fi
interrupt 1.6 "http://127.0.0.1:$FAST_PORT/" -t 1 -c 8 -d 1s --ramp 2 --ramp-p99 1s --json
rc=$?
got="$(python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    print(d["ramp"]["stopped"], d["ramp"]["max_connections"], d["connections"])
except (ValueError, KeyError):
    print("-")' "$TMP/intr.out")"
if (( rc == 0 )) && [[ "$got" == "interrupted 2 2" ]]; then
    pass "ctrl-c later" "stopped, max, connections = $got"
else
    fail "ctrl-c later" "rc=$rc got=$got"
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
U="http://127.0.0.1:1/"
check_err "ramp alone"          "needs --ramp-p99 or --ramp-up"   "$U" -c 8 -t 1 -d 1s --ramp 2
check_err "ramp-step alone"     "needs --ramp-p99"   "$U" -c 8 -t 1 -d 1s --ramp-step x2
check_err "p99 with no unit"    "with its unit"     "$U" -c 8 -t 1 -d 1s --ramp-p99 5
check_err "p99 of 0"            "with its unit"     "$U" -c 8 -t 1 -d 1s --ramp-p99 0ms
check_err "p99 of 5s"           "under 5s"          "$U" -c 8 -t 1 -d 1s --ramp-p99 5s
check_err "p99 with --rate"     "with --rate"       "$U" -c 8 -t 1 -d 1s --ramp-p99 5ms --rate 100
check_err "p99 with --tui"      "with --tui"        "$U" -c 8 -t 1 -d 1s --ramp-p99 5ms --tui
check_err "ramp below -t"       "at least -t"       "$U" -c 8 -t 2 -d 1s --ramp-p99 5ms --ramp 1
check_err "ramp above -c"       "above -c"          "$U" -c 8 -t 1 -d 1s --ramp-p99 5ms --ramp 9
check_err "ramp-step x1"        "ramp-step takes"   "$U" -c 8 -t 1 -d 1s --ramp-p99 5ms --ramp-step x1
check_err "ramp-step +0"        "ramp-step takes"   "$U" -c 8 -t 1 -d 1s --ramp-p99 5ms --ramp-step +0
check_err "ramp-step 2"         "ramp-step takes"   "$U" -c 8 -t 1 -d 1s --ramp-p99 5ms --ramp-step 2

# ── no ramp: nothing changes without --ramp-p99 ──
out="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$FAST_PORT/" -c 8 -t 1 -d 1s 2>&1)"
json="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$FAST_PORT/" -c 8 -t 1 -d 1s --json 2>/dev/null)"
if grep -qE '^  Ramp:|per step' <<<"$out"; then
    fail "no ramp" "ramp lines printed without --ramp-p99"
elif grep -q '"ramp"' <<<"$json"; then
    fail "no ramp" "a ramp block in the JSON without --ramp-p99"
elif ! grep -q 'Status codes: 2xx=' <<<"$out"; then
    fail "no ramp" "the usual results are missing"
else
    pass "no ramp" "output unchanged"
fi

echo
if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
