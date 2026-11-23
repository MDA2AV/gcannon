#!/usr/bin/env python3
"""Keep-alive HTTP/1.1 server for the --idle and --ramp-up tests.

Answers every request at once. With --idle-timeout S it closes a connection
that has had no request for S seconds (as servers do with keep-alive
connections), so connections that open, ask once and go quiet are shed about
S seconds after their answer.

--notify-408 sends a 408 response before closing one, as some servers do.
--hold-after K answers nothing on any connection accepted past the first K.
--accept-delay-ms D pauses D ms after each accept (a slow accept loop).

Binds an ephemeral port on 127.0.0.1 and prints "PORT <n>". SIGTERM prints
"CLOSED_IDLE <n> MAX_OPEN <n> ACCEPTED <n>" to stderr on the way out.
"""

import argparse
import asyncio
import resource
import signal
import sys

OK = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok"
T408 = (b"HTTP/1.1 408 Request Timeout\r\nContent-Length: 0\r\n"
        b"Connection: close\r\n\r\n")


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--idle-timeout", type=float, default=0.0)
    ap.add_argument("--notify-408", action="store_true")
    ap.add_argument("--hold-after", type=int, default=0)
    ap.add_argument("--accept-delay-ms", type=float, default=0.0)
    args = ap.parse_args()

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))

    state = {"open": 0, "max": 0, "accepted": 0, "idled": 0}

    async def handle(reader, writer):
        state["open"] += 1
        state["max"] = max(state["max"], state["open"])
        state["accepted"] += 1
        held = args.hold_after and state["accepted"] > args.hold_after
        if args.accept_delay_ms > 0:
            await asyncio.sleep(args.accept_delay_ms / 1000.0)
        try:
            while True:
                if args.idle_timeout > 0:
                    try:
                        await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), args.idle_timeout)
                    except asyncio.TimeoutError:
                        state["idled"] += 1
                        if args.notify_408:
                            writer.write(T408)
                        break
                else:
                    await reader.readuntil(b"\r\n\r\n")
                if held:
                    continue
                writer.write(OK)
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.LimitOverrunError):
            pass
        finally:
            state["open"] -= 1
            writer.close()

    loop = asyncio.get_running_loop()
    stop = asyncio.Event()
    for s in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(s, stop.set)
    srv = await asyncio.start_server(handle, "127.0.0.1", 0, backlog=65535)
    print("PORT %d" % srv.sockets[0].getsockname()[1], flush=True)
    async with srv:
        await stop.wait()
    print("CLOSED_IDLE %d MAX_OPEN %d ACCEPTED %d" % (state["idled"], state["max"],
                                                       state["accepted"]), file=sys.stderr, flush=True)


asyncio.run(main())
