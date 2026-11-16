#!/usr/bin/env python3
"""Keep-alive HTTP/1.1 server for the --rate tests.

Answers every request with a fixed 200 and keeps the connection open. With
--stall-after/--stall-ms it holds every response back for one window, once:
requests arriving in [after, after+ms) are answered when the window ends. A
paced client must then report the wait for everything it had scheduled inside
the window, not only for the requests that were already on the wire.

Binds an ephemeral port on --host (default 0.0.0.0) and prints "PORT <n>".
"""

import argparse
import asyncio
import resource
import sys
import time

RESP = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok"


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--stall-after", type=float, default=0.0)
    ap.add_argument("--stall-ms", type=float, default=0.0)
    args = ap.parse_args()

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))

    t0 = time.monotonic()
    stall_from = t0 + args.stall_after if args.stall_ms > 0 else None
    stall_until = stall_from + args.stall_ms / 1000.0 if stall_from else None

    async def handle(reader, writer):
        try:
            while True:
                await reader.readuntil(b"\r\n\r\n")
                if stall_from is not None:
                    now = time.monotonic()
                    if stall_from <= now < stall_until:
                        await asyncio.sleep(stall_until - now)
                writer.write(RESP)
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.LimitOverrunError):
            pass
        finally:
            writer.close()

    srv = await asyncio.start_server(handle, args.host, 0, backlog=4096)
    print("PORT %d" % srv.sockets[0].getsockname()[1], flush=True)
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
