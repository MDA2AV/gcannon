#!/usr/bin/env python3
"""Keep-alive HTTP/1.1 server for the --ramp-p99 tests.

--service-ms S answers one request at a time, each after S ms, so N
closed-loop connections each wait for all N of them: latency is about N * S,
and a ramp against a threshold has to stop at a known connection count.
Without it, every request is answered at once.

--fail-over K answers 503 while more than K connections are open.
--hold-over K reads, but never answers, every connection past the first K.

Binds an ephemeral port on 127.0.0.1 and prints "PORT <n>".
"""

import argparse
import asyncio
import resource
import sys

OK = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok"
BUSY = (b"HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
        b"Content-Length: 4\r\n\r\nbusy")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--service-ms", type=float, default=0.0)
    ap.add_argument("--fail-over", type=int, default=0)
    ap.add_argument("--hold-over", type=int, default=0)
    args = ap.parse_args()

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))

    one_at_a_time = asyncio.Lock()
    state = {"open": 0, "accepted": 0}

    async def handle(reader, writer):
        state["open"] += 1
        state["accepted"] += 1
        held = args.hold_over and state["accepted"] > args.hold_over
        try:
            while True:
                await reader.readuntil(b"\r\n\r\n")
                if held:
                    continue
                if args.service_ms > 0:
                    async with one_at_a_time:
                        await asyncio.sleep(args.service_ms / 1000.0)
                busy = args.fail_over and state["open"] > args.fail_over
                writer.write(BUSY if busy else OK)
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.LimitOverrunError):
            pass
        finally:
            state["open"] -= 1
            writer.close()

    srv = await asyncio.start_server(handle, "127.0.0.1", 0, backlog=4096)
    print("PORT %d" % srv.sockets[0].getsockname()[1], flush=True)
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
