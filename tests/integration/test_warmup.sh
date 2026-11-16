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
# once and is retrying. The first check also bounds the run from below: the
# upgrade delay plus -d is the least a run can take once the clock waits for
# setup. The second check makes sure a failure counts as settled, so an
# unreachable target doesn't sit in warmup until the 10 s cap. The third
# presses Ctrl-C during warmup, which must end the run rather than hang it.
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SERVERS="$ROOT/tests/servers"
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
    python3 "$SERVERS/ws_server.py" "$@" >"$TMP/$tag.out" 2>"$TMP/$tag.err" &
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
UPGRADE_DELAY=1.5   # seconds the server sits on each upgrade
DURATION_S=1
# Setup alone takes UPGRADE_DELAY and the clock then runs for -d, so a shorter
# run means the clock started before setup finished. ws_upgrades can't show
# that by itself: it isn't reset with the other stats, so it counts upgrades
# that land inside the measured window too.
MIN_WALL_MS=$(( $(awk -v s="$UPGRADE_DELAY" 'BEGIN { printf "%d", s * 1000 }') \
                + DURATION_S * 1000 - 100 ))
if ! start_ws slow --upgrade-delay "$UPGRADE_DELAY"; then
    echo "    FAIL  slow-upgrade server never started"
    failures=$((failures + 1))
else
    t0="$(now_ms)"
    json="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/ws" --ws \
                -c 8 -t 2 -p 1 -d "${DURATION_S}s" --json 2>/dev/null)"
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
    elif (( wall < MIN_WALL_MS )); then
        problems="${problems:+$problems; }run took ${wall}ms, under the ${MIN_WALL_MS}ms that setup plus -d take - the clock started before setup finished"
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

# ── Ctrl-C during warmup ends the run instead of hanging it ──
# Workers leave their loop on Ctrl-C without acknowledging the stats reset that
# starts the clock, so main has to stop waiting for that acknowledgement too.
# The server sits on every upgrade far longer than the test runs, so the
# SIGINT is sure to land while warmup is still waiting for setup.
handles_sigint() {   # gcannon installs its SIGINT handler at startup
    local mask
    mask="$(awk '/^SigCgt:/ { print $2 }' "/proc/$1/status" 2>/dev/null)"
    [[ -n "$mask" ]] && (( (16#$mask & 2) != 0 ))
}
if ! start_ws stall --upgrade-delay 60; then
    echo "    FAIL  stalled-upgrade server never started"
    failures=$((failures + 1))
else
    mkdir -p "$TMP/int-home"
    HOME="$TMP/int-home" "$GCANNON" "http://127.0.0.1:$PORT/ws" --ws \
        -c 8 -t 2 -d 3s --json >"$TMP/int.out" 2>"$TMP/int.err" &
    gpid=$!
    # A background job starts with SIGINT ignored, so wait for gcannon's own
    # handler before sending one, then give warmup a moment to start waiting.
    for _ in $(seq 1 100); do
        handles_sigint "$gpid" && break
        sleep 0.02
    done
    sleep 0.3
    kill -INT "$gpid" 2>/dev/null
    t0="$(now_ms)"
    for _ in $(seq 1 60); do   # up to 3 s
        kill -0 "$gpid" 2>/dev/null || break
        sleep 0.05
    done
    if kill -0 "$gpid" 2>/dev/null; then
        kill -KILL "$gpid" 2>/dev/null
        wait "$gpid" 2>/dev/null
        printf '    FAIL  ctrl-c in warmup  still running 3s after SIGINT\n'
        failures=$((failures + 1))
    else
        wait "$gpid"; rc=$?
        wall=$(( $(now_ms) - t0 ))
        problems=""
        (( rc == 130 )) || problems="exit code $rc, want 130"
        grep -q 'Interrupted during warmup' "$TMP/int.err" ||
            problems="${problems:+$problems; }no 'Interrupted during warmup' on stderr"
        [[ -s "$TMP/int.out" ]] &&
            problems="${problems:+$problems; }printed a result for a run that never started"
        [[ -e "$TMP/int-home/.gcannon/history.bin" ]] &&
            problems="${problems:+$problems; }saved the unmeasured run to the history"
        if [[ -z "$problems" ]]; then
            printf '    ok    ctrl-c in warmup  exited %sms after SIGINT, rc=%s\n' "$wall" "$rc"
        else
            printf '    FAIL  ctrl-c in warmup  %s\n' "$problems"
            failures=$((failures + 1))
        fi
    fi
fi

echo
if (( failures )); then
    echo "$failures check(s) failed"
    echo
    exit 1
fi
echo "all checks passed"
echo
