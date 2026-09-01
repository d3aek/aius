#!/usr/bin/env python3
"""Pull a frozen flight-recorder capture and reassemble it as a WAV.

The committed host half of the recorder contract
(docs/plans/aius-optical-prototype.md §PSRAM flight recorder, and the
rolling multi-slot bank of aius-recorder-rolling-buffer.md Phase C): reads
the bank header from GET /api/recorder, refuses anything but a FROZEN slot,
pulls the valid extent in bounded offset-addressed chunks keyed by that
slot's freeze_generation, unwraps the physical ring order using
oldest_frame, and writes s24le WAV (the ring is s32le with 24 significant
bits, LSB-aligned).

Usage:
  aius-recorder-pull.py --device http://192.0.2.66 --auth <token> out.wav
  AIUS_SD_AUTH=<token> aius-recorder-pull.py --device http://192.0.2.66 out.wav
  aius-recorder-pull.py --device http://192.0.2.66 --slot 2 out.wav

Env vars:
  AIUS_SD_AUTH   auth token when --auth is absent

Exit codes: 0 ok; 1 transfer/protocol failure; 2 not frozen or empty;
            4 usage error.

--slot addresses one of the bank's slots (default: active_slot from the
header). GET /api/recorder/data is slot-addressed on the device (Phase C):
every chunk request carries &slot, the device validates the range (400 when
out of range) and reads that slot's frozen bytes, refusing a stale/evicted
freeze_generation with 409.

The pull must complete within one boot session: a reboot destroys every
capture, and a thaw or auto-advance between chunks bumps the addressed
slot's freeze_generation, which every chunk request carries -- the device
answers 409 and this script stops rather than splicing two captures into
one file.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request

FRAME_BYTES = 8
RETRIES_PER_CHUNK = 5
RETRY_DELAY_S = 0.5


def fetch(url: str, token: str, timeout_s: float = 10.0) -> bytes:
    request = urllib.request.Request(url, headers={"X-Aius-SD-Auth": token})
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return response.read()


def fetch_chunk(base: str, token: str, offset: int, length: int,
                generation: int, slot: int) -> bytes:
    url = (f"{base}/api/recorder/data?offset={offset}&length={length}"
           f"&generation={generation}&slot={slot}")
    last_error: Exception | None = None
    for _ in range(RETRIES_PER_CHUNK):
        try:
            return fetch(url, token)
        except urllib.error.HTTPError as error:
            if error.code in (400, 401, 409):
                raise  # not transient: wrong request, token, or generation
            last_error = error
        except (urllib.error.URLError, TimeoutError, OSError,
                http.client.HTTPException) as error:
            # The device drops a mid-chunk client on a mount-class epoch
            # stall; chunks are offset-idempotent, so retry the same one.
            # HTTPException covers the FIN-close shape (IncompleteRead) the
            # OSError family does not.
            last_error = error
        time.sleep(RETRY_DELAY_S)
    raise SystemExit(f"chunk at offset {offset} failed after "
                     f"{RETRIES_PER_CHUNK} retries: {last_error}")


def write_wav(path: str, sample_rate: int, frames: bytes) -> None:
    """s24le stereo WAV from the ring's s32le 24-bit-LSB-aligned frames."""
    frame_count = len(frames) // FRAME_BYTES
    data_bytes = frame_count * 6  # 2 ch x 3 bytes
    with open(path, "wb") as out:
        out.write(b"RIFF")
        out.write(struct.pack("<I", 36 + data_bytes))
        out.write(b"WAVEfmt ")
        out.write(struct.pack("<IHHIIHH", 16, 1, 2, sample_rate,
                              sample_rate * 6, 6, 24))
        out.write(b"data")
        out.write(struct.pack("<I", data_bytes))
        # Each s32le word carries the 24-bit sample in its low three bytes;
        # s24le is those three bytes verbatim.
        for i in range(0, len(frames), 4):
            out.write(frames[i:i + 3])


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pull a frozen flight-recorder capture as WAV")
    parser.add_argument("--device", required=True,
                        help="device base URL, e.g. http://192.0.2.66")
    parser.add_argument("--auth", default=os.environ.get("AIUS_SD_AUTH"),
                        help="X-Aius-SD-Auth token (default: $AIUS_SD_AUTH)")
    parser.add_argument("--header-json", default=None,
                        help="also write the header document to this path")
    parser.add_argument("--slot", type=int, default=None,
                        help="bank slot to pull (default: active_slot)")
    parser.add_argument("output", help="output WAV path")
    args = parser.parse_args()
    if not args.auth:
        print("no auth token: pass --auth or set AIUS_SD_AUTH",
              file=sys.stderr)
        return 4
    base = args.device.rstrip("/")

    bank = json.loads(fetch(f"{base}/api/recorder", args.auth))
    if args.header_json:
        with open(args.header_json, "w", encoding="utf-8") as out:
            json.dump(bank, out, indent=2)
    active_slot = int(bank["active_slot"])
    slot = active_slot if args.slot is None else args.slot
    if not 0 <= slot < int(bank["slot_count"]):
        print(f"slot {slot} out of range (0..{bank['slot_count'] - 1})",
              file=sys.stderr)
        return 4
    header = bank["slots"][slot]
    if header["state"] != "frozen":
        print(f"slot {slot} is {header['state']}, not frozen -- freeze "
              "first (POST /api/recorder/freeze) and wait for state=frozen",
              file=sys.stderr)
        return 2
    valid_frames = int(header["valid_frames"])
    if valid_frames == 0:
        print(f"slot {slot}'s frozen capture holds zero frames",
              file=sys.stderr)
        return 2
    if bank["encoding"] != "s32le" or int(bank["frame_bytes"]) != 8:
        print("unrecognised encoding; refusing to guess", file=sys.stderr)
        return 1

    generation = int(header["freeze_generation"])
    chunk_max = int(bank["chunk_max_bytes"])
    valid_bytes = valid_frames * FRAME_BYTES
    ring = bytearray()
    while len(ring) < valid_bytes:
        want = min(chunk_max, valid_bytes - len(ring))
        chunk = fetch_chunk(base, args.auth, len(ring), want, generation, slot)
        if not chunk:
            print(f"empty chunk at offset {len(ring)}", file=sys.stderr)
            return 1
        ring.extend(chunk)
        print(f"\r{len(ring)}/{valid_bytes} bytes", end="", file=sys.stderr)
    print("", file=sys.stderr)

    # Physical ring order -> stream order: the oldest frame sits at
    # oldest_frame once the ring has wrapped (0 otherwise).
    oldest = int(header["oldest_frame"]) * FRAME_BYTES
    stream = bytes(ring[oldest:]) + bytes(ring[:oldest])
    write_wav(args.output, int(header["sample_rate_hz"]), stream)
    seconds = valid_frames / float(header["sample_rate_hz"])
    trigger = header["trigger"]["which"]
    gaps = header["gap_count"]
    print(f"wrote {args.output}: slot={slot} {valid_frames} frames "
          f"({seconds:.1f} s), trigger={trigger}, gaps={gaps}, "
          f"generation={generation}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
