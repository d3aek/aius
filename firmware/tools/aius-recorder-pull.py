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

Exit codes: 0 ok; 1 transfer/protocol failure (unreachable board, 404, a
            freeze_generation that moved mid-pull); 2 not frozen or empty;
            4 usage error (no token, 401, a slot the device rejects).

--slot addresses one of the bank's slots. The default is the NEWEST FROZEN
slot, which is what an operator who just triggered a capture wants and what
the device itself defaults to: `newest_frozen_slot` in the bank header, and
GET /api/recorder/data without `&slot` targeting the same slot (409 when
nothing is frozen, which is exactly when that field is null). Older
firmware emits no `newest_frozen_slot`, so the tool then scans `slots[]` for
the frozen entry with the highest freeze_generation, and only if there is no
frozen slot at all falls back to `active_slot` (which reports its own state,
so the "not frozen" message names what the bank is actually doing). The
chosen slot and the reason are printed.

GET /api/recorder/data is slot-addressed on the device (Phase C): every chunk
request carries &slot, the device validates the range (400 when out of range)
and reads that slot's frozen bytes, refusing a stale/evicted
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

# What each slot state means for an operator who asked for a pull. Only
# `frozen` is pullable; the other four need different advice, and telling an
# operator to "freeze first" while a freeze is already draining is how a
# second trigger gets fired at a bank that was about to be ready.
STATE_ADVICE = {
    "recording": "freeze it first (POST /api/recorder/freeze) and wait for "
                 "state=frozen",
    "postroll": "a freeze is already in flight (post-roll capture); wait for "
                "state=frozen and pull again",
    "draining": "a freeze is already in flight (staging drain); wait for "
                "state=frozen and pull again",
    "absent": "the slot holds no capture at all -- no PSRAM fitted, the arm "
              "was refused, or this slot is parked",
}


class PullFailure(Exception):
    """A failed request, carrying the exit code the docstring documents."""

    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code


def request_failure(error: Exception, what: str) -> PullFailure:
    """Translate a urllib failure into a documented exit code and one line."""
    if isinstance(error, urllib.error.HTTPError):
        if error.code == 401:
            return PullFailure(4, f"{what}: 401 -- the X-Aius-SD-Auth token "
                                  "is missing or wrong (pass --auth or set "
                                  "AIUS_SD_AUTH)")
        if error.code == 400:
            return PullFailure(4, f"{what}: 400 -- the device rejected the "
                                  "request (slot out of range, or a "
                                  "malformed offset/length)")
        if error.code == 409:
            return PullFailure(1, f"{what}: 409 -- the slot's "
                                  "freeze_generation moved mid-pull (thaw, "
                                  "auto-advance or eviction); re-read GET "
                                  "/api/recorder and pull again")
        if error.code == 404:
            return PullFailure(1, f"{what}: 404 -- no such route; is this "
                                  "firmware carrying the flight recorder?")
        return PullFailure(1, f"{what}: HTTP {error.code} {error.reason}")
    return PullFailure(1, f"{what}: board unreachable "
                          f"({type(error).__name__}: {error})")


def fetch(url: str, token: str, timeout_s: float = 10.0) -> bytes:
    request = urllib.request.Request(url, headers={"X-Aius-SD-Auth": token})
    with urllib.request.urlopen(request, timeout=timeout_s) as response:
        return response.read()


def fetch_chunk(base: str, token: str, offset: int, length: int,
                generation: int, slot: int) -> bytes:
    url = (f"{base}/api/recorder/data?offset={offset}&length={length}"
           f"&generation={generation}&slot={slot}")
    what = f"chunk at offset {offset}"
    last_error: Exception | None = None
    for _ in range(RETRIES_PER_CHUNK):
        try:
            return fetch(url, token)
        except urllib.error.HTTPError as error:
            if error.code in (400, 401, 409):
                # Not transient: wrong request, token, or generation.
                raise request_failure(error, what) from error
            last_error = error
        except (urllib.error.URLError, TimeoutError, OSError,
                http.client.HTTPException) as error:
            # The device drops a mid-chunk client on a mount-class epoch
            # stall; chunks are offset-idempotent, so retry the same one.
            # HTTPException covers the FIN-close shape (IncompleteRead) the
            # OSError family does not.
            last_error = error
        time.sleep(RETRY_DELAY_S)
    raise PullFailure(1, f"{what} failed after {RETRIES_PER_CHUNK} "
                        f"retries: {last_error}")


def choose_slot(bank: dict, requested: int | None) -> tuple[int, str]:
    """Pick the slot to pull and say why, newest-frozen first.

    The device's own default is the newest frozen slot (`newest_frozen_slot`
    in the bank header; GET /api/recorder/data without `&slot` targets it),
    so the tool defaults to the same slot rather than to `active_slot` --
    which, on a bank that just froze and auto-advanced, is the slot still
    recording. Firmware without the field is served by scanning `slots[]`,
    and a bank with nothing frozen falls back to `active_slot` so the caller
    gets that slot's real state in the refusal.
    """
    if requested is not None:
        return requested, "requested with --slot"
    newest = bank.get("newest_frozen_slot")
    if isinstance(newest, int) and not isinstance(newest, bool):
        return newest, "newest frozen slot reported by the device"
    slots = bank.get("slots")
    if isinstance(slots, list):
        frozen = [
            (int(entry.get("freeze_generation", 0)), index)
            for index, entry in enumerate(slots)
            if isinstance(entry, dict) and entry.get("state") == "frozen"
        ]
        if frozen:
            generation, index = max(frozen)
            return index, ("highest freeze_generation among frozen slots "
                           f"({generation}); this firmware reports no "
                           "newest_frozen_slot")
    return int(bank["active_slot"]), ("active slot -- no frozen slot in the "
                                      "bank")


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


def run(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Pull a frozen flight-recorder capture as WAV")
    parser.add_argument("--device", required=True,
                        help="device base URL, e.g. http://192.0.2.66")
    parser.add_argument("--auth", default=os.environ.get("AIUS_SD_AUTH"),
                        help="X-Aius-SD-Auth token (default: $AIUS_SD_AUTH)")
    parser.add_argument("--header-json", default=None,
                        help="also write the header document to this path")
    parser.add_argument("--slot", type=int, default=None,
                        help="bank slot to pull (default: newest frozen slot)")
    parser.add_argument("output", help="output WAV path")
    args = parser.parse_args(argv)
    if not args.auth:
        print("no auth token: pass --auth or set AIUS_SD_AUTH",
              file=sys.stderr)
        return 4
    base = args.device.rstrip("/")

    what = "GET /api/recorder"
    try:
        raw_bank = fetch(f"{base}/api/recorder", args.auth)
    except (urllib.error.URLError, TimeoutError, OSError,
            http.client.HTTPException) as error:
        raise request_failure(error, what) from error
    try:
        bank = json.loads(raw_bank)
    except json.JSONDecodeError as error:
        raise PullFailure(1, f"{what}: malformed JSON ({error})") from error
    if args.header_json:
        # The bank document is written VERBATIM, so every field the firmware
        # adds (rate_change_frames, rate_change_count, newest_frozen_slot, ...)
        # reaches the sidecar without a change here.
        with open(args.header_json, "w", encoding="utf-8") as out:
            json.dump(bank, out, indent=2)
    slot, reason = choose_slot(bank, args.slot)
    if not 0 <= slot < int(bank["slot_count"]):
        print(f"slot {slot} out of range (0..{bank['slot_count'] - 1})",
              file=sys.stderr)
        return 4
    print(f"slot {slot}: {reason}", file=sys.stderr)
    header = bank["slots"][slot]
    state = header["state"]
    if state != "frozen":
        advice = STATE_ADVICE.get(state, "wait for state=frozen")
        print(f"slot {slot} is {state}, not frozen -- {advice}",
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


def main(argv: list[str] | None = None) -> int:
    try:
        return run(argv)
    except PullFailure as failure:
        print(str(failure), file=sys.stderr)
        return failure.code


if __name__ == "__main__":
    raise SystemExit(main())
