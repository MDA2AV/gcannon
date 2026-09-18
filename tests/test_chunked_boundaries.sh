#!/usr/bin/env bash
#
# End-to-end test: drive the real gcannon binary against chunked servers that
# flush the terminating CRLF at different boundaries.
#
# A correct parser produces identical results in all three modes. A parser that
# only consumes the trailer when it happens to be in the current recv will
# reconnect once per response in the split modes, which shows up as
# reconnects == responses and read errors == responses.
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
GCANNON="$ROOT/gcannon"

CONNS=4
THREADS=1
DURATION=2s

if [[ ! -x "$GCANNON" ]]; then
    echo "error: $GCANNON not built — run 'make' first" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required for the integration tests" >&2
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

# Start chunked_server.py in the given mode and wait for it to report its port.
# The pid goes to a file rather than a shell array: this must stay callable
# without a command substitution, or the background pid is recorded in a
# subshell and the server outlives the test run.
start_server() {
    local mode="$1"
    local out="$TMP/$mode.out"

    python3 "$HERE/chunked_server.py" --mode "$mode" >"$out" 2>"$TMP/$mode.err" &
    echo $! >>"$PIDFILE"

    for _ in $(seq 1 100); do
        grep -q '^PORT ' "$out" 2>/dev/null && return 0
        sleep 0.05
    done

    echo "error: server ($mode) never reported a port" >&2
    cat "$TMP/$mode.err" >&2
    return 1
}

server_port() {
    awk '/^PORT /{print $2; exit}' "$TMP/$1.out"
}

run_mode() {
    local mode="$1" expectation="$2"
    local port json

    start_server "$mode" || { failures=$((failures + 1)); return; }
    port="$(server_port "$mode")"

    # HOME is redirected so the run does not append to the user's real
    # ~/.gcannon/history.bin.
    json="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$port/" \
                -c "$CONNS" -t "$THREADS" -d "$DURATION" -p 1 --json 2>"$TMP/$mode.gc.err")"

    if [[ -z "$json" ]]; then
        echo "  FAIL  $mode: gcannon produced no JSON output"
        sed 's/^/        /' "$TMP/$mode.gc.err" >&2
        failures=$((failures + 1))
        return
    fi

    local result
    result="$(printf '%s' "$json" | python3 -c '
import json, sys
d = json.load(sys.stdin)
resp = d["responses"]
rec  = d["reconnects"]
rerr = d["errors"]["read"]
ok2  = d["status"]["2xx"]
bad  = []
if resp == 0:                bad.append("no responses at all")
if rec != 0:                 bad.append("reconnects=%d (want 0)" % rec)
if rerr != 0:                bad.append("read_errors=%d (want 0)" % rerr)
if ok2 != resp:              bad.append("2xx=%d but responses=%d" % (ok2, resp))
print("%d\t%d\t%d\t%d\t%s" % (resp, rec, rerr, ok2, "; ".join(bad)))
')"

    local resp rec rerr ok2 problems
    IFS=$'\t' read -r resp rec rerr ok2 problems <<<"$result"

    if [[ -z "$problems" ]]; then
        printf '    ok    %-18s responses=%-8s reconnects=%-8s read_errors=%s\n' \
               "$mode" "$resp" "$rec" "$rerr"
    else
        printf '    FAIL  %-18s responses=%-8s reconnects=%-8s read_errors=%s\n' \
               "$mode" "$resp" "$rec" "$rerr"
        printf '          %s\n' "$problems"
        printf '          %s\n' "$expectation"
        failures=$((failures + 1))
    fi
}

echo
echo "gcannon integration tests (chunked flush boundaries)"
echo
echo "  a complete chunked response must be parsed identically no matter how"
echo "  the server splits it across writes"
echo

run_mode whole \
    "the terminator arrived in one write — this is the case that works"
run_mode split-trailer \
    "server flushed after '0\\r\\n'; the trailing CRLF lands in its own recv"
run_mode split-mid-trailer \
    "server flushed after '0\\r\\n\\r'; a single '\\n' lands in its own recv"

echo
if (( failures )); then
    echo "$failures/3 modes failed"
    echo
    exit 1
fi
echo "3/3 modes passed"
echo
