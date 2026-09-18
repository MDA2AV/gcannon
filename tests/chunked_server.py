#!/usr/bin/env python3
"""
Chunked HTTP server with controllable flush boundaries.

Real servers decide for themselves how a chunked response is split into TCP
segments, so gcannon's parser has to cope with the terminating CRLF landing in
a recv of its own. This server makes that split deterministic instead of
leaving it to buffering luck.

Modes:
  whole             "0\\r\\n\\r\\n" goes out in one write (the easy case)
  split-trailer     write up to "0\\r\\n", flush, then write "\\r\\n"
  split-mid-trailer write up to "0\\r\\n\\r", flush, then write "\\n"

Binds an ephemeral port and prints "PORT <n>" on stdout so the caller does not
have to guess a free one.
"""

import argparse
import socket
import sys
import threading
import time

HEAD = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n"
TERM = b"0\r\n\r\n"

# Where to cut the terminating "0\r\n\r\n" into two writes.
CUTS = {
    "whole": None,
    "split-trailer": 3,      # "0\r\n" | "\r\n"
    "split-mid-trailer": 4,  # "0\r\n\r" | "\n"
}

# Long enough to guarantee separate segments, short enough to keep the test
# quick. TCP_NODELAY is set so neither piece waits on Nagle.
FLUSH_GAP_SEC = 0.002


def serve_conn(conn, cut, stop):
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    conn.settimeout(1.0)
    try:
        while not stop.is_set():
            try:
                data = conn.recv(65536)
            except socket.timeout:
                continue
            if not data:
                return
            # One response per pipelined request in this recv.
            for _ in range(max(1, data.count(b"\r\n\r\n"))):
                if cut is None:
                    conn.sendall(HEAD + TERM)
                else:
                    conn.sendall(HEAD + TERM[:cut])
                    time.sleep(FLUSH_GAP_SEC)
                    conn.sendall(TERM[cut:])
    except OSError:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=sorted(CUTS), default="whole")
    args = ap.parse_args()
    cut = CUTS[args.mode]

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(512)
    srv.settimeout(0.5)

    print("PORT %d" % srv.getsockname()[1], flush=True)

    stop = threading.Event()
    try:
        while True:
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            threading.Thread(target=serve_conn, args=(conn, cut, stop),
                             daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        srv.close()


if __name__ == "__main__":
    sys.exit(main())
