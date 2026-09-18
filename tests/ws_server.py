#!/usr/bin/env python3
"""
WebSocket echo server that can refuse a share of upgrade handshakes.

A refused upgrade is a failed handshake, not a response to a benchmark request.
Refusing with a 2xx is the interesting case: it used to be tallied into
status_2xx, which in WebSocket mode is also what gets reported as "WS frames",
and which main.c subtracts from `responses` when checking the expected status.
Pushing it above `responses` underflowed that unsigned subtraction.

gcannon sends a fixed masked text frame per request (2 header + 4 mask +
payload), so frames are a known constant size and can be counted by byte total
rather than fully parsed. It also waits for the 101 before sending any frame,
so the handshake reply never shares a write with frame data.

  --refuse-every N   refuse every Nth upgrade (0 = never refuse)
  --refuse-status S  status line to refuse with (default "200 OK")

Binds an ephemeral port and prints "PORT <n>" on stdout.
"""

import argparse
import socket
import sys
import threading

ACCEPT = (
    b"HTTP/1.1 101 Switching Protocols\r\n"
    b"Upgrade: websocket\r\n"
    b"Connection: Upgrade\r\n"
    b"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"
)

_seq_lock = threading.Lock()
_seq = 0


def next_seq():
    global _seq
    with _seq_lock:
        _seq += 1
        return _seq


def serve_conn(conn, args, frame_size, echo, refuse):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.settimeout(2.0)
    buf = b""
    upgraded = False
    carry = 0
    try:
        while True:
            try:
                data = conn.recv(65536)
            except socket.timeout:
                continue
            if not data:
                return

            if not upgraded:
                buf += data
                idx = buf.find(b"\r\n\r\n")
                if idx < 0:
                    continue
                carry = len(buf) - (idx + 4)
                buf = b""
                upgraded = True
                if args.refuse_every and next_seq() % args.refuse_every == 0:
                    conn.sendall(refuse)
                    return          # refused: drop it, gcannon reconnects
                conn.sendall(ACCEPT)
            else:
                carry += len(data)

            frames, carry = divmod(carry, frame_size)
            if frames:
                conn.sendall(echo * frames)
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refuse-every", type=int, default=0)
    ap.add_argument("--refuse-status", default="200 OK")
    ap.add_argument("--payload", type=int, default=5)
    args = ap.parse_args()

    frame_size = 2 + 4 + args.payload            # client frames are masked
    echo = bytes([0x81, args.payload]) + b"x" * args.payload
    refuse = ("HTTP/1.1 %s\r\nContent-Length: 0\r\n\r\n"
              % args.refuse_status).encode("ascii")

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(512)
    srv.settimeout(0.5)
    print("PORT %d" % srv.getsockname()[1], flush=True)

    try:
        while True:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            threading.Thread(target=serve_conn,
                             args=(conn, args, frame_size, echo, refuse),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()


if __name__ == "__main__":
    sys.exit(main())
