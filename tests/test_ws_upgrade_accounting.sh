#!/usr/bin/env bash
#
# A refused WebSocket upgrade must not be counted as a frame.
#
# It used to be tallied into status_2xx..status_other. Two things went wrong:
#
#   * In WebSocket mode status_2xx IS the frame count — stats_print reports it
#     as "WS frames" — so refusals inflated the frame total above the number of
#     frames actually received.
#   * main.c computes `unexpected = responses - status_2xx` in uint64_t when
#     the expected status is 2xx. A refusal carrying a 2xx pushes status_2xx
#     above responses, so that subtraction wrapped and printed
#     "WARNING: 18446744073709547245/8751460 responses (210784761327933.2%)".
#
# Refusals only land inside the measurement window if connections churn, so
# -r forces reconnects; with plain keep-alive they all happen during warmup and
# the stat reset clears them.
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
GCANNON="$ROOT/gcannon"

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

# Starts ws_server.py; echoes nothing, sets $PORT.
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
echo "gcannon WebSocket upgrade accounting"
echo

# ── refusals must not be counted as frames, and must not wrap the warning ──
if ! start_ws refuse --refuse-every 2 --refuse-status "200 OK"; then
    echo "    FAIL  refusing server never started"
    failures=$((failures + 1))
else
    json="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/ws" --ws \
                -c 8 -t 2 -p 1 -d 3s -r 20 --json 2>/dev/null)"

    if [[ -z "$json" ]]; then
        echo "    FAIL  gcannon produced no JSON"
        failures=$((failures + 1))
    else
        result="$(printf '%s' "$json" | python3 -c '
import json, sys
d = json.load(sys.stdin)
resp = d["responses"]
ok2  = d["status"]["2xx"]
fail = d.get("ws_upgrade_failures")
bad = []
if fail is None:        bad.append("no ws_upgrade_failures field in JSON")
elif fail == 0:         bad.append("no upgrade was refused - test did not exercise the path")
if ok2 > resp:          bad.append("status_2xx=%d exceeds responses=%d (refusals counted as frames)" % (ok2, resp))
if ok2 != resp:         bad.append("ws frames=%d != responses=%d" % (ok2, resp))
print("%s\t%s\t%s\t%s" % (resp, ok2, fail, "; ".join(bad)))
')"
        IFS=$'\t' read -r resp ok2 fail problems <<<"$result"
        if [[ -z "$problems" ]]; then
            printf '    ok    refused upgrades   responses=%-9s frames=%-9s refused=%s\n' \
                   "$resp" "$ok2" "$fail"
        else
            printf '    FAIL  refused upgrades   responses=%-9s frames=%-9s refused=%s\n' \
                   "$resp" "$ok2" "$fail"
            printf '          %s\n' "$problems"
            failures=$((failures + 1))
        fi
    fi

    # The human-readable warning must stay believable, never wrap to ~2^64.
    out="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/ws" --ws \
               -c 8 -t 2 -p 1 -d 3s -r 20 2>&1)"
    warn="$(grep -o 'WARNING: [0-9]*/[0-9]* responses' <<<"$out")"
    if [[ -n "$warn" ]]; then
        n=$(sed 's|WARNING: \([0-9]*\)/.*|\1|' <<<"$warn")
        tot=$(sed 's|WARNING: [0-9]*/\([0-9]*\) .*|\1|' <<<"$warn")
        # Compared in python, not bash: a wrapped count is above 2^63 and bash
        # arithmetic reads it as negative, which silently passes the check.
        if [[ "$(python3 -c "print(int('$n') > int('$tot'))")" == "True" ]]; then
            printf '    FAIL  warning wrapped     %s\n' "$warn"
            failures=$((failures + 1))
        else
            printf '    ok    warning sane        %s\n' "$warn"
        fi
    else
        printf '    ok    warning sane        none emitted\n'
    fi
fi

# ── plain echo, no refusals: frames and responses must agree exactly ──
if ! start_ws plain; then
    echo "    FAIL  echo server never started"
    failures=$((failures + 1))
else
    json="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/ws" --ws \
                -c 8 -t 2 -p 1 -d 3s --json 2>/dev/null)"
    result="$(printf '%s' "$json" | python3 -c '
import json, sys
d = json.load(sys.stdin)
resp = d["responses"]; ok2 = d["status"]["2xx"]
fail = d.get("ws_upgrade_failures", 0); up = d.get("ws_upgrades", 0)
bad = []
if resp == 0:  bad.append("no frames echoed at all")
if ok2 != resp: bad.append("ws frames=%d != responses=%d" % (ok2, resp))
if fail != 0:   bad.append("ws_upgrade_failures=%d, want 0" % fail)
if up == 0:     bad.append("no successful upgrades")
print("%s\t%s\t%s" % (resp, up, "; ".join(bad)))
')"
    IFS=$'\t' read -r resp up problems <<<"$result"
    if [[ -z "$problems" ]]; then
        printf '    ok    clean echo         responses=%-9s upgrades=%s\n' "$resp" "$up"
    else
        printf '    FAIL  clean echo         responses=%-9s upgrades=%s\n' "$resp" "$up"
        printf '          %s\n' "$problems"
        failures=$((failures + 1))
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
