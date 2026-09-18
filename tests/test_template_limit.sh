#!/usr/bin/env bash
#
# --raw must refuse more templates than the per-template stat arrays can hold.
#
# worker_stats_t indexes tpl_responses[] and tpl_responses_2xx[] by template
# index, both sized MAX_TEMPLATES (64). Nothing checked the number of files
# passed to --raw, so template 64 and up wrote past those arrays and into the
# adjacent tpl_latency pointer — a wild pointer, and a segfault rather than an
# error message.
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
GCANNON="$ROOT/gcannon"

MAX_TEMPLATES=64   # keep in sync with include/constants.h

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

# Write $1 raw request templates, echo the comma-separated list.
make_templates() {
    local n="$1"
    local dir="$TMP/tpl$n"
    local i
    mkdir -p "$dir"
    for (( i = 1; i <= n; i++ )); do
        printf 'GET /t%d HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' "$i" >"$dir/t$i.raw"
    done
    ls "$dir"/t*.raw | tr '\n' ',' | sed 's/,$//'
}

start_server() {
    python3 "$HERE/chunked_server.py" --mode whole \
            >"$TMP/srv.out" 2>"$TMP/srv.err" &
    echo $! >>"$PIDFILE"
    local _
    for _ in $(seq 1 100); do
        grep -q '^PORT ' "$TMP/srv.out" 2>/dev/null && break
        sleep 0.05
    done
    awk '/^PORT /{print $2; exit}' "$TMP/srv.out"
}

echo
echo "gcannon --raw template limit (MAX_TEMPLATES = $MAX_TEMPLATES)"
echo

# ── over the limit: must be refused, not a segfault ──────────────────
over=$(( MAX_TEMPLATES + 6 ))
list="$(make_templates "$over")"
out="$("$GCANNON" "http://127.0.0.1:1/" --raw "$list" -c "$over" -t 1 -d 1s 2>&1)"
rc=$?

if (( rc >= 128 )); then
    printf '    FAIL  %-22s died from signal %d (exit %d)\n' \
           "$over templates" $(( rc - 128 )) "$rc"
    failures=$((failures + 1))
elif (( rc == 0 )); then
    printf '    FAIL  %-22s accepted, expected rejection\n' "$over templates"
    failures=$((failures + 1))
elif ! grep -qi 'error' <<<"$out"; then
    printf '    FAIL  %-22s exit %d but no error message\n' "$over templates" "$rc"
    failures=$((failures + 1))
else
    printf '    ok    %-22s %s\n' "$over templates" "$(head -1 <<<"$out")"
fi

# ── zero templates: strtok_r skips empty fields, so these load nothing
#    and num_templates becomes a zero divisor in start_connect ─────────
for empty_arg in "" "," ",,,"; do
    out="$("$GCANNON" "http://127.0.0.1:1/" --raw "$empty_arg" -c 4 -t 1 -d 1s 2>&1)"
    rc=$?
    label="--raw '$empty_arg'"

    if (( rc >= 128 )); then
        printf '    FAIL  %-22s died from signal %d (exit %d)\n' \
               "$label" $(( rc - 128 )) "$rc"
        failures=$((failures + 1))
    elif (( rc == 0 )); then
        printf '    FAIL  %-22s accepted, expected rejection\n' "$label"
        failures=$((failures + 1))
    elif ! grep -qi 'error' <<<"$out"; then
        printf '    FAIL  %-22s exit %d but no error message\n' "$label" "$rc"
        failures=$((failures + 1))
    else
        printf '    ok    %-22s %s\n' "$label" "$(head -1 <<<"$out")"
    fi
done

# ── exactly at the limit: must still work ────────────────────────────
port="$(start_server)"
if [[ -z "$port" ]]; then
    echo "    FAIL  test server never started"
    failures=$((failures + 1))
else
    list="$(make_templates "$MAX_TEMPLATES")"
    out="$(HOME="$TMP" "$GCANNON" "http://127.0.0.1:$port/" --raw "$list" \
              -c "$MAX_TEMPLATES" -t 1 -d 2s 2>&1)"
    rc=$?

    if (( rc >= 128 )); then
        printf '    FAIL  %-22s died from signal %d\n' \
               "$MAX_TEMPLATES templates" $(( rc - 128 ))
        failures=$((failures + 1))
    elif ! grep -q "Templates: *$MAX_TEMPLATES" <<<"$out"; then
        printf '    FAIL  %-22s rejected at the limit\n' "$MAX_TEMPLATES templates"
        printf '          %s\n' "$(head -1 <<<"$out")"
        failures=$((failures + 1))
    else
        printf '    ok    %-22s ran to completion\n' "$MAX_TEMPLATES templates"
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
