#!/usr/bin/env bash
#
# https:// end to end: OpenSSL handshake, keys handed to kernel TLS, then the
# normal io_uring send/recv path on plaintext.
#
# The servers are Python's ssl module, which is OpenSSL, so these are real
# TLS 1.3 peers. Each sends two session tickets right after the handshake;
# they reach gcannon as non-data records ahead of the first response, so a
# receive path that can't tell record types apart fails every check here
# with read errors and reconnects instead of responses.
#
# Needs kernel TLS (the tls module) and the openssl CLI for a throwaway
# certificate; skipped without them.
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

echo
echo "gcannon https://"
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

failures=0

# Starts a test server script; sets $PORT.
start_server() {
    local tag="$1" script="$2"; shift 2
    python3 "$HERE/$script" "$@" >"$TMP/$tag.out" 2>"$TMP/$tag.err" &
    echo $! >>"$PIDFILE"
    local _
    for _ in $(seq 1 100); do
        grep -q '^PORT ' "$TMP/$tag.out" 2>/dev/null && break
        sleep 0.05
    done
    PORT="$(awk '/^PORT /{print $2; exit}' "$TMP/$tag.out")"
    [[ -n "$PORT" ]]
}

# check NAME PYTHON_ASSERTIONS -- GCANNON_ARGS...
# Runs gcannon with --json; the assertions see the parsed output as `d` and
# append to `bad`. stderr is kept in $TMP/NAME.stderr.
check() {
    local name="$1" asserts="$2"; shift 3
    local json
    json="$(HOME="$TMP" "$GCANNON" "$@" --json 2>"$TMP/$name.stderr")"
    local result
    result="$(printf '%s' "$json" | STDERR_FILE="$TMP/$name.stderr" python3 -c '
import json, os, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    print("-\tgcannon produced no JSON"); sys.exit()
stderr = open(os.environ["STDERR_FILE"]).read()
bad = []
'"$asserts"'
print("%s\t%s" % (d["responses"], "; ".join(bad)))
')"
    local resp problems
    IFS=$'\t' read -r resp problems <<<"$result"
    if [[ -z "$problems" ]]; then
        printf '    ok    %-18s responses=%s\n' "$name" "$resp"
    else
        printf '    FAIL  %-18s responses=%s\n' "$name" "$resp"
        printf '          %s\n' "$problems"
        failures=$((failures + 1))
    fi
}

CLEAN='
e = d["errors"]
if d["responses"] == 0:               bad.append("no responses")
if d["status"]["2xx"] != d["responses"]: bad.append("2xx=%d != responses" % d["status"]["2xx"])
if e["connect"] or e["read"]:         bad.append("errors: connect=%d read=%d" % (e["connect"], e["read"]))
'

if start_server http tls_server.py --cert "$TMP/cert.pem" --key "$TMP/key.pem"; then
    # Keep-alive with pipelining; tickets arrive before the first responses
    check keep-alive "$CLEAN"'
if d["reconnects"]: bad.append("reconnects=%d, want 0" % d["reconnects"])
' -- "https://127.0.0.1:$PORT/" -c 8 -t 2 -p 4 -d 2s

    # A host name goes out as SNI; the server echoes what it saw
    check sni "$CLEAN" -- "https://localhost:$PORT/" -c 2 -t 1 -d 1s
    if grep -q '^SNI localhost$' "$TMP/http.out"; then
        printf '    ok    %-18s server saw "localhost"\n' "sni name"
    else
        printf '    FAIL  %-18s server saw no SNI "localhost"\n' "sni name"
        failures=$((failures + 1))
    fi

    # -r: a fresh handshake for every connection
    check reconnect "$CLEAN"'
if d["reconnects"] == 0: bad.append("no reconnects - -r did not churn connections")
' -- "https://127.0.0.1:$PORT/" -c 8 -t 2 -p 1 -d 2s -r 20
else
    echo "    FAIL  TLS server never started"
    failures=$((failures + 1))
fi

# The server ends connections with close_notify: an orderly close, not an error
if start_server notify tls_server.py --cert "$TMP/cert.pem" --key "$TMP/key.pem" \
        --close-after 10; then
    check close-notify "$CLEAN"'
if d["reconnects"] == 0: bad.append("no reconnects - server never closed")
' -- "https://127.0.0.1:$PORT/" -c 4 -t 1 -p 1 -d 2s
else
    echo "    FAIL  close_notify server never started"
    failures=$((failures + 1))
fi

# WebSocket over TLS: handshake, then upgrade, then frames
if start_server wss ws_server.py --tls-cert "$TMP/cert.pem" --tls-key "$TMP/key.pem"; then
    check websocket "$CLEAN"'
if d.get("ws_upgrades") != 4: bad.append("ws_upgrades=%s, want 4" % d.get("ws_upgrades"))
' -- "https://127.0.0.1:$PORT/ws" --ws -c 4 -t 2 -d 2s
else
    echo "    FAIL  TLS WebSocket server never started"
    failures=$((failures + 1))
fi

# A server that doesn't speak TLS: every handshake fails, the reason is
# printed once, and nothing is counted as a response
if start_server garbage tls_server.py --garbage; then
    check not-tls '
if d["responses"]:                 bad.append("responses from a server that cannot do TLS")
if d["errors"]["connect"] == 0:    bad.append("handshake failures not counted as connect errors")
lines = [l for l in stderr.splitlines() if l.startswith("TLS ")]
if len(lines) != 1:                bad.append("%d TLS error lines on stderr, want exactly 1" % len(lines))
' -- "https://127.0.0.1:$PORT/" -c 2 -t 1 -d 1s
else
    echo "    FAIL  garbage server never started"
    failures=$((failures + 1))
fi

echo
if (( failures )); then
    echo "$failures check(s) failed"
    echo
    exit 1
fi
echo "all checks passed"
echo
