#!/usr/bin/env python3
"""Hold a `/api/recorder/data` read lease open by draining the response slowly.

The board takes the addressed slot's read lease for the lifetime of the HTTP
transaction and releases it on the response's completion hook, so the only way
to hold a lease open for a measurable window is to leave a 200 unfinished. A
tiny `SO_RCVBUF` keeps the board's TCP send window nearly closed, so the body
cannot be dumped into the kernel buffer in one go; the board's response
deadline (`RESPONSE_TIMEOUT_MS` 5000) is re-armed on every completed write, so
a trickle read keeps the transaction -- and the slot's read lease -- alive
until this process exits or is killed.

This is a lease *holder*, not a data puller: the bytes are counted and
discarded. Use `aius-recorder-pull.py` to actually retrieve a capture.

Written for `lease_deferral_probe.py`, which runs it as a child process and
kills it to release the lease; it is standalone-runnable for the same purpose
by hand.
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from urllib.parse import urlparse

# Small enough that the board's send window stays nearly closed between reads.
# The point is to make the kernel refuse to buffer the body, not to be frugal.
RECV_BUFFER_BYTES = 1024
SOCKET_TIMEOUT_S = 30.0
DEFAULT_TARGET = "http://192.0.2.66"


def target_endpoint(target: str) -> tuple[str, int]:
    """(host, port) from a `--target` URL, so this tool and the HTTP-speaking
    siblings take the same `--target http://192.0.2.66` value even though this
    one speaks raw sockets."""
    parsed = urlparse(target if "//" in target else f"http://{target}")
    if not parsed.hostname:
        raise argparse.ArgumentTypeError(f"no host in target {target!r}")
    return parsed.hostname, parsed.port or 80


def build_request(host: str, auth: str, slot: int, generation: int,
                  length: int) -> bytes:
    """The one GET whose 200 this process refuses to finish reading.

    `generation` and `slot` address one specific frozen capture: the board
    rejects a generation that no longer matches, so a slot recycled out from
    under the reader cannot be mistaken for a lease still being honoured."""
    return (
        f"GET /api/recorder/data?offset=0&length={length}"
        f"&generation={generation}&slot={slot} HTTP/1.1\r\n"
        f"Host: {host}\r\nX-Aius-SD-Auth: {auth}\r\n"
        f"Connection: close\r\n\r\n"
    ).encode("utf-8")


def hold_lease(host: str, port: int, auth: str, slot: int, generation: int,
               length: int, step_bytes: int, step_seconds: float) -> int:
    """Drain the response `step_bytes` at a time with `step_seconds` between
    reads, logging each step so the holder's liveness is visible in the
    probe's evidence directory. Returns the exit status."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RECV_BUFFER_BYTES)
    sock.settimeout(SOCKET_TIMEOUT_S)
    sock.connect((host, port))
    sock.sendall(build_request(host, auth, slot, generation, length))
    start = time.time()
    total = 0
    first = b""
    print(f"{time.strftime('%H:%M:%S')} connected, request sent "
          f"(slot {slot} generation {generation})", flush=True)
    while True:
        try:
            chunk = sock.recv(step_bytes)
        except socket.timeout:
            print(f"{time.strftime('%H:%M:%S')} recv timeout after {total} B",
                  flush=True)
            return 2
        except OSError as error:
            print(f"{time.strftime('%H:%M:%S')} socket error {error} after "
                  f"{total} B", flush=True)
            return 3
        if not chunk:
            print(f"{time.strftime('%H:%M:%S')} closed by peer after {total} B, "
                  f"{time.time() - start:.1f} s", flush=True)
            return 0
        if not first:
            # The status line says whether the lease was ever taken: a 4xx here
            # means the read was refused and NO lease is being held, which is
            # the one failure that would otherwise look like a clean hold.
            first = chunk[:160]
            print(f"{time.strftime('%H:%M:%S')} first bytes: {first!r}",
                  flush=True)
        total += len(chunk)
        print(f"{time.strftime('%H:%M:%S')} +{len(chunk)} B total {total} B "
              f"t={time.time() - start:.1f} s", flush=True)
        time.sleep(step_seconds)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", "--base", default=DEFAULT_TARGET,
                        help="board base URL (default: %(default)s)")
    parser.add_argument("--auth", required=True,
                        help="X-Aius-SD-Auth token")
    parser.add_argument("--slot", required=True, type=int,
                        help="frozen slot to hold the read lease on")
    parser.add_argument("--generation", required=True, type=int,
                        help="that slot's freeze_generation")
    parser.add_argument("--length", type=int, default=65536,
                        help="bytes requested (default: %(default)s)")
    parser.add_argument("--step-bytes", type=int, default=512,
                        help="bytes per recv (default: %(default)s)")
    parser.add_argument("--step-seconds", type=float, default=1.0,
                        help="seconds between recvs (default: %(default)s)")
    args = parser.parse_args(argv)

    host, port = target_endpoint(args.target)
    return hold_lease(host, port, args.auth, args.slot, args.generation,
                      args.length, args.step_bytes, args.step_seconds)


if __name__ == "__main__":
    sys.exit(main())
