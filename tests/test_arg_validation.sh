#!/usr/bin/env bash
#
# Invalid CLI arguments must be rejected with a clear message before anything
# derives from them.
#
# -t 0 in particular used to reach `num_connections / num_threads` and kill the
# process with SIGFPE (exit 136), which reads like a gcannon crash rather than a
# typo in the command line.
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
GCANNON="$ROOT/gcannon"

# Never connected to — validation must reject before host resolution.
DEAD_URL="http://127.0.0.1:1/"

if [[ ! -x "$GCANNON" ]]; then
    echo "error: $GCANNON not built — run 'make' first" >&2
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

# An invalid flag combination: must exit non-zero, must not die from a signal,
# and must say something on stderr.
expect_rejected() {
    local desc="$1"; shift
    local out rc
    out="$("$GCANNON" "$@" 2>&1)"; rc=$?

    if (( rc >= 128 )); then
        printf '    FAIL  %-26s died from signal %d (exit %d)\n' \
               "$desc" $(( rc - 128 )) "$rc"
        failures=$((failures + 1))
    elif (( rc == 0 )); then
        printf '    FAIL  %-26s accepted, expected rejection\n' "$desc"
        failures=$((failures + 1))
    elif ! grep -qi 'error' <<<"$out"; then
        printf '    FAIL  %-26s exit %d but no error message\n' "$desc" "$rc"
        failures=$((failures + 1))
    else
        printf '    ok    %-26s %s\n' "$desc" "$(head -1 <<<"$out")"
    fi
}

# Guard against over-rejecting: a sane command line must still start a run.
expect_accepted() {
    local desc="$1"; shift
    local out rc port

    python3 "$HERE/chunked_server.py" --mode whole \
            >"$TMP/srv.out" 2>"$TMP/srv.err" &
    echo $! >>"$PIDFILE"
    for _ in $(seq 1 100); do
        grep -q '^PORT ' "$TMP/srv.out" 2>/dev/null && break
        sleep 0.05
    done
    port="$(awk '/^PORT /{print $2; exit}' "$TMP/srv.out")"
    if [[ -z "$port" ]]; then
        printf '    FAIL  %-26s test server never started\n' "$desc"
        failures=$((failures + 1))
        return
    fi

    out="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$port/" "$@" 2>&1)"; rc=$?

    if (( rc >= 128 )); then
        printf '    FAIL  %-26s died from signal %d\n' "$desc" $(( rc - 128 ))
        failures=$((failures + 1))
    elif ! grep -q 'Threads:' <<<"$out"; then
        printf '    FAIL  %-26s rejected a valid command line\n' "$desc"
        printf '          %s\n' "$(head -1 <<<"$out")"
        failures=$((failures + 1))
    else
        printf '    ok    %-26s ran to completion\n' "$desc"
    fi
}

echo
echo "gcannon CLI argument validation"
echo

expect_rejected "-t 0"          "$DEAD_URL" -c 4 -t 0  -d 1s
expect_rejected "-t negative"   "$DEAD_URL" -c 4 -t -1 -d 1s
expect_rejected "-c 0"          "$DEAD_URL" -c 0 -t 1  -d 1s
expect_rejected "-c negative"   "$DEAD_URL" -c -8 -t 1 -d 1s
expect_rejected "-p 0"          "$DEAD_URL" -c 4 -t 1  -d 1s -p 0
expect_rejected "-d 0"          "$DEAD_URL" -c 4 -t 1  -d 0
expect_rejected "-d unparseable" "$DEAD_URL" -c 4 -t 1 -d abc
expect_rejected "-r negative"   "$DEAD_URL" -c 4 -t 1  -d 1s -r -3
expect_rejected "fewer conns than threads" "$DEAD_URL" -c 2 -t 4 -d 1s

expect_accepted "valid run" -c 4 -t 2 -d 1s

echo
if (( failures )); then
    echo "$failures check(s) failed"
    echo
    exit 1
fi
echo "all checks passed"
echo
