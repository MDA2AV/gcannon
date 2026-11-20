#!/usr/bin/env python3
"""
HTTP/1.1 keep-alive server over TLS 1.3, for gcannon's https:// tests.

Python's ssl module is OpenSSL underneath, so this is a real TLS 1.3 server:
right after every handshake it sends two session tickets, which arrive on
gcannon's kernel-TLS socket as non-data records before the first response.

Every request (headers only, as gcannon sends them; pipelining is fine) gets
a fixed 200 with a 2-byte body.

  --cert F / --key F  certificate chain and key (PEM)
  --close-after N     after N responses on a connection, send close_notify
                      and drop it (0 = never)
  --garbage           not TLS at all: answer every connection with a plain
                      HTTP 400 and close, so the client's handshake fails
  --handshake-delay MS
                      hold every handshake back this long after the accept,
                      like a server busy signing other handshakes, and print
                      "PENDING_MAX <n>" when the first one finishes: how many
                      accepted connections were waiting on a handshake at
                      once until then

Binds an ephemeral port and prints "PORT <n>" on stdout, then "SNI <name>"
the first time each server name is seen.
"""

import argparse
import socket
import ssl
import sys
import threading
import time

RESPONSE = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
GARBAGE = b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"

_sni_lock = threading.Lock()
_sni_seen = set()

# --handshake-delay: connections accepted and not yet through their handshake
_pending_lock = threading.Lock()
_pending = 0
_pending_max = 0
_first_done = False


def pending_add():
    global _pending, _pending_max
    with _pending_lock:
        _pending += 1
        if not _first_done:
            _pending_max = max(_pending_max, _pending)


def pending_done():
    global _pending, _first_done
    with _pending_lock:
        _pending -= 1
        if not _first_done:
            _first_done = True
            print("PENDING_MAX %d" % _pending_max, flush=True)


def on_sni(_sslsock, name, _ctx):
    with _sni_lock:
        if name not in _sni_seen:
            _sni_seen.add(name)
            print("SNI %s" % name, flush=True)


def serve_conn(conn, ctx, args):
    try:
        if args.garbage:
            conn.sendall(GARBAGE)
            return
        if args.handshake_delay:
            time.sleep(args.handshake_delay / 1000.0)
            try:
                conn = ctx.wrap_socket(conn, server_side=True)
            finally:
                pending_done()
        else:
            conn = ctx.wrap_socket(conn, server_side=True)
        buf = b""
        served = 0
        while True:
            data = conn.recv(65536)
            if not data:
                return
            buf += data
            n = buf.count(b"\r\n\r\n")
            if not n:
                continue
            buf = buf[buf.rfind(b"\r\n\r\n") + 4:]
            if args.close_after:
                n = min(n, args.close_after - served)
            conn.sendall(RESPONSE * n)
            served += n
            if args.close_after and served >= args.close_after:
                conn.settimeout(1.0)
                try:
                    conn.unwrap()           # sends close_notify
                except (OSError, ValueError):
                    pass
                return
    except (OSError, ssl.SSLError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--close-after", type=int, default=0)
    ap.add_argument("--garbage", action="store_true")
    ap.add_argument("--handshake-delay", type=int, default=0)
    args = ap.parse_args()

    ctx = None
    if not args.garbage:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.load_cert_chain(args.cert, args.key)
        ctx.sni_callback = on_sni

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1024)
    srv.settimeout(0.5)
    print("PORT %d" % srv.getsockname()[1], flush=True)

    try:
        while True:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            if args.handshake_delay:
                pending_add()
            threading.Thread(target=serve_conn, args=(conn, ctx, args),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    sys.exit(main())
