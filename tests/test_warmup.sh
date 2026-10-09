#!/usr/bin/env bash
#
# The measurement clock must not start while connections are still being set
# up.
#
# Warmup used to be a fixed 100 ms sleep. Anything slower than that to set up a
# connection — here a WebSocket upgrade the server answers 1.5 s late — ran
# into the measured window, where those connections sat idle and the reported
# rate fell. With -d 1s every frame landed after the window had closed and the
# run reported 0 frames.
#
# Warmup now lasts until every connection has settled: became ready, or failed
# once and is retrying. The second check makes sure a failure counts as
# settled, so an unreachable target doesn't sit in warmup until the 10 s cap.
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
GCANNON="${GCANNON:-$ROOT/gcannon}"

# Nothing listens here, so every connect is refused straight away.
DEAD_URL="http://127.0.0.1:1/"

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

now_ms() { python3 -c 'import time; print(int(time.monotonic() * 1000))'; }

# Starts ws_server.py; sets $PORT.
start_ws() {
    local tag="$1"; shift
    python3 "$HERE/ws_server.py" "$@" >"$TMP/$tag.out" 2>"$TMP/$tag.err" &
    echo $! >>"$PIDFILE"
    local _
    for _ in $(seq 1 100); do
        grep -q '^PORT ' "$TMP/$tag.out" 2>/dev/null && break
        sleep 0.05
    done
    PORT="$(awk '/^PORT /{print $2; exit}' "$TMP/$tag.out")"
    [[ -n "$PORT" ]]
}

echo
echo "gcannon warmup"
echo

# ── slow setup must finish before the clock starts ──
if ! start_ws slow --upgrade-delay 1.5; then
    echo "    FAIL  slow-upgrade server never started"
    failures=$((failures + 1))
else
    t0="$(now_ms)"
    json="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/ws" --ws \
                -c 8 -t 2 -p 1 -d 1s --json 2>/dev/null)"
    wall=$(( $(now_ms) - t0 ))

    result="$(printf '%s' "$json" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    print("-\t-\tgcannon produced no JSON"); sys.exit()
resp = d["responses"]; up = d.get("ws_upgrades", 0)
bad = []
if resp == 0: bad.append("0 frames measured - setup ran into the measured window")
if up != 8:   bad.append("ws_upgrades=%d, want all 8 before the clock started" % up)
print("%s\t%s\t%s" % (resp, up, "; ".join(bad)))
')"
    IFS=$'\t' read -r resp up problems <<<"$result"
    if (( wall > 6000 )); then
        problems="${problems:+$problems; }run took ${wall}ms - warmup should end at ~1.5s, not the 10s cap"
    fi
    if [[ -z "$problems" ]]; then
        printf '    ok    slow upgrades     frames=%-9s upgrades=%s wall=%sms\n' "$resp" "$up" "$wall"
    else
        printf '    FAIL  slow upgrades     frames=%-9s upgrades=%s wall=%sms\n' "$resp" "$up" "$wall"
        printf '          %s\n' "$problems"
        failures=$((failures + 1))
    fi
fi

# ── a refused connection counts as settled, so warmup doesn't wait it out ──
t0="$(now_ms)"
json="$(HOME="$TMP" "$GCANNON" "$DEAD_URL" -c 8 -t 2 -d 1s --json 2>/dev/null)"
wall=$(( $(now_ms) - t0 ))
cerr="$(printf '%s' "$json" | python3 -c '
import json, sys
try:
    print(json.load(sys.stdin)["errors"]["connect"])
except (ValueError, KeyError):
    print(0)
')"
if (( cerr == 0 )); then
    printf '    FAIL  unreachable       no connect errors - test did not exercise the path\n'
    failures=$((failures + 1))
elif (( wall > 4000 )); then
    printf '    FAIL  unreachable       run took %sms - refused connections held the warmup\n' "$wall"
    failures=$((failures + 1))
else
    printf '    ok    unreachable       connect_errors=%-9s wall=%sms\n' "$cerr" "$wall"
fi

echo
if (( failures )); then
    echo "$failures check(s) failed"
    echo
    exit 1
fi
echo "all checks passed"
echo
