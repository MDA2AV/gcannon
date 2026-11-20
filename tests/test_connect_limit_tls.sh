#!/usr/bin/env bash
#
# --connect-limit over https://: a first attempt holds its slot until its TLS
# handshake is done, not just its TCP connect.
#
# The server holds every handshake back 300 ms, as one busy signing other
# handshakes would, and reports how many accepted connections were waiting on
# a handshake at once before the first one finished. With 2 threads and
# --connect-limit 4 that must be at most 8. Through 0.8.3 a slot was freed at
# the TCP connect, which the kernel completes at once, so all 32 connections
# were waiting together.
#
# Needs kernel TLS (the tls module: gcannon refuses https:// without it) and
# the openssl CLI for a throwaway certificate; skipped without them.
#
# Run: make test-integration (or run this script directly)

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
GCANNON="${GCANNON:-$ROOT/gcannon}"
THREADS=2
LIMIT=4
CONNS=32

if [[ ! -x "$GCANNON" ]]; then
    echo "error: $GCANNON not built — run 'make' first" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required" >&2
    exit 2
fi

echo
echo "gcannon --connect-limit over https://"
echo

if ! command -v openssl >/dev/null 2>&1; then
    echo "    SKIP  openssl CLI not found (needed for a test certificate)"
    echo
    exit 0
fi
ulp="$(python3 -c '
import socket
l = socket.socket(); l.bind(("127.0.0.1", 0)); l.listen(1)
c = socket.create_connection(l.getsockname())
try:
    c.setsockopt(socket.IPPROTO_TCP, 31, b"tls")   # TCP_ULP
    print("ok")
except OSError as e:
    print(e.strerror)
')"
if [[ "$ulp" != "ok" ]]; then
    echo "    SKIP  kernel TLS unavailable here ($ulp)"
    echo
    exit 0
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

if ! openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -days 1 -subj /CN=localhost \
        >/dev/null 2>&1; then
    echo "    FAIL  could not create a test certificate"
    exit 1
fi

python3 "$HERE/tls_server.py" --cert "$TMP/cert.pem" --key "$TMP/key.pem" \
    --handshake-delay 300 >"$TMP/server.out" 2>"$TMP/server.err" &
echo $! >>"$PIDFILE"
for _ in $(seq 1 100); do
    grep -q '^PORT ' "$TMP/server.out" 2>/dev/null && break
    sleep 0.05
done
PORT="$(awk '/^PORT /{print $2; exit}' "$TMP/server.out")"
if [[ -z "$PORT" ]]; then
    echo "    FAIL  tls_server.py never started"
    exit 1
fi

# Only the count before the first handshake finishes is checked, not the run.
HOME="$TMP" "$GCANNON" "https://127.0.0.1:$PORT/" -c "$CONNS" -t "$THREADS" -d 1s \
    --connect-limit "$LIMIT" --warmup-max 20s >/dev/null 2>"$TMP/run.err"

pending="$(awk '/^PENDING_MAX /{print $2; exit}' "$TMP/server.out")"
want=$((THREADS * LIMIT))
failures=0
if [[ -z "$pending" ]]; then
    echo "    FAIL  handshakes        no handshake finished: $(head -c 200 "$TMP/run.err")"
    failures=1
elif (( pending > want )); then
    echo "    FAIL  handshakes        $pending waiting at once, want at most $want ($THREADS threads x --connect-limit $LIMIT)"
    failures=1
else
    echo "    ok    handshakes        $pending waiting at once of $CONNS connections (limit $want)"
fi

echo
if (( failures )); then
    echo "1 check(s) failed"
    exit 1
fi
echo "all checks passed"
