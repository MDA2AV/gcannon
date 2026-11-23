#!/usr/bin/env bash
#
# --idle and --ramp-up: connections that open, send one request and are then
# left alone, and a fleet that grows over time at a fixed rate.
#
#   shed           all 160 idle connections are opened, answered, and closed by
#                  the server about 2 s after their answer; none is reopened, so
#                  the server accepts exactly -c of them; the active ones hold
#                  the rate throughout, with no late sends to speak of
#   ramp shape     from 8 to 40 active and 0 to 160 idle over 4 s, evenly
#   not shed       a server that keeps idle connections: all still open at the
#                  end, none closed
#   at once        --idle without --ramp-up: every idle connection is opened as
#                  the window opens, and the active ones were all up before it
#   percent        --idle 50% and --idle 50 are the same of -c 100
#   ramp, no idle  --ramp-up on its own: the active ones grow, and the rate holds
#   no answer      connections whose response never comes are opened but not
#                  answered, and are reported as waiting
#   408            data the server sends on an idle connection is counted, and
#                  the close that follows it still counts as a close
#   closed loop    without --rate the active ones keep sending
#   interrupted    Ctrl-C in the middle still reports what happened
#   arg errors     a bad --idle, --ramp-up or --ramp, and the options they
#                  refuse
#   no flags       without them, no idle or ramp-up lines or keys appear
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
pass() { printf '    ok    %-17s %s\n' "$1" "$2"; }
fail() { printf '    FAIL  %-17s %s\n' "$1" "$2"; failures=$((failures + 1)); }

# Starts idle_server.py with the given options; sets $PORT and $SERVER.
start_server() {
    local tag="$1"; shift
    python3 "$HERE/idle_server.py" "$@" >"$TMP/$tag.out" 2>"$TMP/$tag.err" &
    SERVER=$!
    echo "$SERVER" >>"$PIDFILE"
    local _
    for _ in $(seq 1 100); do
        grep -q '^PORT ' "$TMP/$tag.out" 2>/dev/null && break
        sleep 0.05
    done
    PORT="$(awk '/^PORT /{print $2; exit}' "$TMP/$tag.out")"
    [[ -n "$PORT" ]]
}

# Stops the server and echoes what it counted: "CLOSED_IDLE n MAX_OPEN n ACCEPTED n"
stop_server() {
    kill "$SERVER" 2>/dev/null
    wait "$SERVER" 2>/dev/null
    cat "$TMP/$1.err"
}

# Runs gcannon with --json; its exit code is $rc, the JSON in $TMP/run.json.
run_json() {
    HOME="$TMP" "$GCANNON" "$@" --json >"$TMP/run.json" 2>"$TMP/run.err"
    rc=$?
}

# ev <expression>: true when it holds of the JSON. d is the whole object, i the
# idle block, t the rows, a(f) a row field over all rows.
ev() {
    python3 -I -c '
import json, sys
d = json.load(open(sys.argv[1]))
i = d.get("idle", {}); t = d.get("timeline", []); r = d.get("rate", {})
c = i.get("closed_after_s", {})
a = lambda f: [row[f] for row in t]
sys.exit(0 if eval(sys.argv[2]) else 1)' "$TMP/run.json" "$1"
}

# check <name> <expression> <what to show on success>
check() {
    if (( rc == 0 )) && ev "$2"; then
        pass "$1" "$(python3 -I -c '
import json, sys
d = json.load(open(sys.argv[1])); i = d.get("idle", {}); r = d.get("rate", {})
print("opened=%s answered=%s closed=%s open=%s rate=%s" % (i.get("opened"), i.get("answered"), i.get("closed_by_server"), i.get("open_at_end"), r.get("achieved")))' "$TMP/run.json")"
    else
        fail "$1" "rc=$rc $(head -c 300 "$TMP/run.json" | tr '\n' ' ') $(tail -1 "$TMP/run.err")"
    fi
}

echo
echo "gcannon --idle and --ramp-up"
echo

# ── shed: the server times out every idle connection, and gcannon counts it ──
start_server shed --idle-timeout 2
run_json "http://127.0.0.1:$PORT/" -c 200 -t 2 -d 8s --rate 500 --idle 80% --ramp 8 --ramp-up 4s
counted="$(stop_server shed)"
check "shed" 'i["opened"]==160 and i["answered"]==160 and i["failed"]==0 and i["closed_by_server"]==160 and i["open_at_end"]==0 and 1.8<c["first"]<2.6 and c["last"]<2.8 and abs(r["achieved"]-500)<10 and r["late"]<20 and d["reconnects"]==0'
got="$(awk '{print $6}' <<<"$counted")"
if [[ "$got" == 200 ]]; then pass "no reopening" "the server accepted $got connections, -c is 200"
else fail "no reopening" "server said: $counted"; fi
check "ramp shape" 'len(t)==8 and a("active")[:4]==[16,24,32,40] and a("active")[4:]==[40]*4 and [40<=t[k]["idle_opened"] for k in range(0,1)] and abs(t[1]["idle_opened"]-80)<=4 and t[3]["idle_opened"]==160 and a("idle_opened")==sorted(a("idle_opened")) and a("idle_closed")==sorted(a("idle_closed"))'
check "phases" '"ramp" in d["phases"] and "hold" in d["phases"] and abs(d["phases"]["ramp"]["sec"]-4)<0.1'

# ── not shed ──
start_server keep
run_json "http://127.0.0.1:$PORT/" -c 100 -t 2 -d 3s --rate 200 --idle 60 --ramp 4 --ramp-up 2s
stop_server keep >/dev/null
check "not shed" 'i["opened"]==60 and i["answered"]==60 and i["closed_by_server"]==0 and i["open_at_end"]==60 and "closed_after_s" not in i'

# ── at once, and percent equals count ──
start_server atonce
run_json "http://127.0.0.1:$PORT/" -c 100 -t 2 -d 2s --rate 200 --idle 50%
check "at once" 't[0]["idle_opened"]==50 and t[0]["active"]==50 and i["opened"]==50 and "ramp_up" not in d'
run_json "http://127.0.0.1:$PORT/" -c 100 -t 2 -d 2s --rate 200 --idle 50
check "percent" 'i["connections"]==50 and i["opened"]==50'
stop_server atonce >/dev/null

# ── ramp without idle ──
start_server ramp
run_json "http://127.0.0.1:$PORT/" -c 40 -t 2 -d 4s --rate 300 --ramp 4 --ramp-up 2s
check "ramp, no idle" '"idle" not in d and a("active")[0]<a("active")[1]<=a("active")[-1]==40 and abs(r["achieved"]-300)<6 and d["ramp_up"]=={"sec":2,"from":4,"to":40}'
stop_server ramp >/dev/null

# ── connections the server never answers ──
start_server hold --hold-after 40
run_json "http://127.0.0.1:$PORT/" -c 100 -t 2 -d 4s --rate 100 --idle 60 --ramp 4 --ramp-up 2s
check "no answer" 'i["opened"]==60 and i["answered"]<60 and i["closed_by_server"]==0'
out="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/" -c 100 -t 2 -d 3s --rate 100 --idle 60 --ramp 4 --ramp-up 1s 2>/dev/null)"
if grep -q 'still waiting for their response' <<<"$out"; then pass "waiting line" "$(grep -o '[0-9]* still waiting.*' <<<"$out")"
else fail "waiting line" "$(grep 'Idle:' <<<"$out")"; fi
stop_server hold >/dev/null

# ── a server that says 408 before it closes ──
start_server timeout408 --idle-timeout 1 --notify-408
run_json "http://127.0.0.1:$PORT/" -c 60 -t 2 -d 4s --rate 100 --idle 30 --ramp 4 --ramp-up 1s
check "408" 'i["unsolicited"]==30 and i["closed_by_server"]==30 and i["open_at_end"]==0'
stop_server timeout408 >/dev/null

# ── closed loop ──
start_server loop --idle-timeout 1
run_json "http://127.0.0.1:$PORT/" -c 40 -t 2 -d 4s --idle 50% --ramp 2 --ramp-up 2s
check "closed loop" 'd["responses"]>1000 and i["opened"]==20 and i["closed_by_server"]==20 and "rate" not in d'
stop_server loop >/dev/null

# ── Ctrl-C in the middle of the ramp ──
start_server intr --idle-timeout 5
(
    HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/" -c 100 -t 2 -d 20s --rate 200 --idle 50 \
        --ramp 4 --ramp-up 10s --json >"$TMP/run.json" 2>"$TMP/run.err" &
    pid=$!
    sleep 2.5
    kill -INT "$pid"
    wait "$pid"
)
rc=$?
check "interrupted" 'len(t)>=2 and 0<i["opened"]<50 and d["elapsed_sec"]<4 and t[-1]["t"]<=d["elapsed_sec"]+0.5'
stop_server intr >/dev/null

# ── arg errors ──
check_err() {
    local name="$1" want="$2"; shift 2
    local out rcx
    out="$(HOME="$TMP" "$GCANNON" "$@" 2>&1)"; rcx=$?
    if (( rcx != 0 )) && grep -qF -- "$want" <<<"$out"; then
        pass "$name" "$(head -1 <<<"$out")"
    else
        fail "$name" "rc=$rcx out=$(head -1 <<<"$out")"
    fi
}
U="http://127.0.0.1:1/"
check_err "idle of nothing"  "takes a number of connections or a percentage"  "$U" -c 100 -t 2 -d 1s --idle 0
check_err "idle not a number" "takes a number of connections or a percentage" "$U" -c 100 -t 2 -d 1s --idle abc
check_err "idle over 100%"   "takes a number of connections or a percentage"  "$U" -c 100 -t 2 -d 1s --idle 101%
check_err "idle of all"      "leaves 0 of -c (100) active"                     "$U" -c 100 -t 2 -d 1s --idle 100
check_err "idle too many"    "fewer than -t (2)"                                "$U" -c 100 -t 2 -d 1s --idle 99
check_err "idle with --ws"   "--idle cannot be combined with --ws"            "$U" -c 100 -t 2 -d 1s --idle 50% --ws
check_err "idle with --tui"  "--idle cannot be combined with --tui"           "$U" -c 100 -t 2 -d 1s --idle 50% --tui
check_err "idle with p99"    "--idle cannot be combined with --ramp-p99"      "$U" -c 100 -t 2 -d 1s --idle 50% --ramp-p99 5ms
check_err "ramp-up alone"    "--ramp-up needs --ramp <n>"                     "$U" -c 100 -t 2 -d 5s --ramp-up 2s
check_err "ramp-up of 0"     "at least 1 second"                              "$U" -c 100 -t 2 -d 5s --ramp-up 0 --ramp 4
check_err "ramp-up past -d"  "must not be longer than -d (5s)"                "$U" -c 100 -t 2 -d 5s --ramp-up 6s --ramp 4
check_err "floor below -t"   "must be at least -t (2)"                        "$U" -c 100 -t 2 -d 5s --ramp-up 2s --ramp 1
check_err "floor above active" "not be above the 50 active connections"       "$U" -c 100 -t 2 -d 5s --ramp-up 2s --ramp 60 --idle 50
check_err "ramp-up with p99" "cannot be combined with --ramp-p99"             "$U" -c 100 -t 2 -d 5s --ramp-up 2s --ramp 4 --ramp-p99 5ms
check_err "ramp-up with tui" "cannot be combined with --tui"                  "$U" -c 100 -t 2 -d 5s --ramp-up 2s --ramp 4 --tui
check_err "ramp-up with --ws" "cannot be combined with --ws"                  "$U" -c 100 -t 2 -d 5s --ramp-up 2s --ramp 4 --ws
check_err "ramp-step alone"  "--ramp-step needs --ramp-p99"                   "$U" -c 100 -t 2 -d 5s --ramp-up 2s --ramp 4 --ramp-step x2

# ── nothing changes without them ──
start_server plain
out="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/" -c 8 -t 1 -d 1s --rate 100 2>&1)"
json="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$PORT/" -c 8 -t 1 -d 1s --json 2>/dev/null)"
if grep -qE 'Idle:|Ramp-up|Phases:|  Time ' <<<"$out"; then
    fail "no flags" "idle or ramp-up lines printed without the flags"
elif grep -qE '"idle"|"timeline"|"ramp_up"' <<<"$json"; then
    fail "no flags" "idle or ramp-up keys in the JSON without the flags"
else
    pass "no flags" "output unchanged"
fi
stop_server plain >/dev/null

echo
if (( failures )); then
    echo "$failures check(s) failed"
    exit 1
fi
echo "all checks passed"
