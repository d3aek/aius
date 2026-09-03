#!/usr/bin/env python3
"""Recorder read-lease-deferred recycle probe (recorder-rolling-buffer Phase D
item 2, deferral half).

Item 2's *release* half is already proven on device. Its *deferral* half needs
a read lease held OPEN across a carrier drop, and no sequence of HTTP verbs can
produce that from outside: the board serves one transaction at a time, so the
client holding the lease is the only client and cannot also drive the drop.
This tool is a single process for exactly that reason -- no tool round-trip
sits inside the lease window, and the carrier drop is a local sysfs write
rather than another request the board would have to answer first.

Sequence:
  1. GET /api/recorder (header before). The candidate slot is the frozen slot
     the next recycle would take, `(active_slot + 1) % slot_count`.
  2. Start slow_reader.py on the candidate -> it holds that slot's read lease.
  3. After --hold-before-drop, de-authorize the U24 -> the carrier drop fires
     the carrier_loss trigger, which freezes the active slot; the bank then
     wants to recycle the leased candidate and must DEFER instead.
  4. After --hold-after-drop, kill the reader -> the lease is released.
  5. GET header (after release), re-authorize the U24, wait --relock-wait, GET
     header again, then GET /api/status.

What a pass looks like: `deferred_epochs` and `lease_skips` advance while the
lease is held, `lease_leaks` stays 0, and the candidate slot's
`freeze_generation` is UNCHANGED until the lease is released -- that last one
is the actual claim, since it says the leased capture survived the recycle the
bank wanted to perform.

A candidate slot that is not frozen means there is nothing for the bank to
defer over; the run aborts with exit 2 rather than reporting a vacuous pass.

The U24 is re-authorized in a `finally`. Leaving the bench source
de-authorized is the one outcome this tool must never produce, including on an
exception path. Re-enumeration resets the U24 PCM playback mixer to 70,70 and
every bench assumes unity, so restore it after a run -- see the README.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path
from urllib.request import Request, urlopen

DEFAULT_TARGET = "http://192.0.2.66"
DEFAULT_AUTH_FILE = ".secrets/aius-sd-auth"
DEFAULT_USB_PATH = "/sys/bus/usb/devices/3-1.1/authorized"
# Scratch-run defaults, kept as they were flown. The before-drop hold only has
# to outlast the reader's first recv (the lease is taken when the 200 starts);
# the after-drop hold has to outlast the carrier_loss freeze plus the postroll
# and the drains the bank needs to reach the candidate; the relock wait has to
# outlast U24 re-enumeration plus the board's carrier re-acquisition.
DEFAULT_HOLD_BEFORE_DROP_S = 6.0
DEFAULT_HOLD_AFTER_DROP_S = 15.0
DEFAULT_RELOCK_WAIT_S = 12.0
# Settle between killing the reader and reading the header, so the release the
# header is meant to show has actually been processed.
RELEASE_SETTLE_S = 1.0
READER_KILL_TIMEOUT_S = 5.0
HTTP_TIMEOUT_S = 8.0


def get_json(target: str, path: str, auth: str) -> dict:
    """GET a token-gated JSON route. Every /api/recorder* route is token-gated
    by this plan's contract, the GETs included (an unauthenticated read answers
    401 sd_auth by design)."""
    request = Request(target + path, headers={"X-Aius-SD-Auth": auth})
    with urlopen(request, timeout=HTTP_TIMEOUT_S) as response:
        return json.loads(response.read())


def header_summary(header: dict) -> dict:
    """The bank fields this probe's claim is made of: the three lease counters,
    the recycle count, and each slot's state and freeze generation."""
    return {
        "active_slot": header["active_slot"],
        "slots_recycled": header["slots_recycled"],
        "deferred_epochs": header["deferred_epochs"],
        "lease_skips": header["lease_skips"],
        "lease_leaks": header["lease_leaks"],
        "slots": [
            {
                "slot": slot["slot"],
                "state": slot["state"],
                "gen": slot["freeze_generation"],
                "trigger": slot["trigger"]["which"],
                "postroll_captured": slot["postroll_captured"],
            }
            for slot in header["slots"]
        ],
    }


def slot_generation(header: dict, slot_index: int) -> int | None:
    """The candidate's `freeze_generation` in one header, or None when the slot
    is absent. Compared across the run's headers this is the claim: it must not
    change while the lease is held."""
    for slot in header.get("slots") or []:
        if slot.get("slot") == slot_index:
            return slot.get("freeze_generation")
    return None


def usb_authorize_helper():
    """The sibling `recorder_transitions.usb_authorize`. Imported here rather
    than reimplemented so there is ONE de-auth path on the bench: it writes the
    sysfs file directly rather than through `sh -c "echo ... > path"`, which
    would interpolate a CLI-supplied path into a command string."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import recorder_transitions

    return recorder_transitions.usb_authorize


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", "--base", default=DEFAULT_TARGET,
                        help="board base URL (default: %(default)s)")
    parser.add_argument("--auth",
                        help="X-Aius-SD-Auth token (default: read --auth-file)")
    parser.add_argument("--auth-file", default=DEFAULT_AUTH_FILE,
                        help="token file (default: %(default)s)")
    parser.add_argument("--evidence", "--output", type=Path,
                        help="evidence JSON path (default: a timestamped file "
                             "under firmware/test/evidence/rate-gate-capture/)")
    parser.add_argument("--usb-path", default=DEFAULT_USB_PATH,
                        help="U24 sysfs authorized flag (default: %(default)s)")
    parser.add_argument("--hold-before-drop", type=float,
                        default=DEFAULT_HOLD_BEFORE_DROP_S,
                        help="seconds the lease is held before the carrier "
                             "drop (default: %(default)s)")
    parser.add_argument("--hold-after-drop", type=float,
                        default=DEFAULT_HOLD_AFTER_DROP_S,
                        help="seconds the lease is held after the carrier "
                             "drop (default: %(default)s)")
    parser.add_argument("--relock-wait", type=float,
                        default=DEFAULT_RELOCK_WAIT_S,
                        help="seconds to wait after re-authorizing the U24 "
                             "(default: %(default)s)")
    parser.add_argument("--length", type=int, default=65536,
                        help="bytes the holding reader requests")
    parser.add_argument("--step-bytes", type=int, default=512,
                        help="bytes per reader recv")
    parser.add_argument("--step-seconds", type=float, default=1.0,
                        help="seconds between reader recvs")
    args = parser.parse_args(argv)

    # Sibling modules: the one sysfs de-auth path and the shared evidence
    # emission. Added by this file's own directory so the tool runs by absolute
    # path, not only from its directory.
    usb_authorize = usb_authorize_helper()
    import capture

    auth = args.auth or Path(args.auth_file).read_text(
        encoding="utf-8").strip()
    if args.evidence is None:
        args.evidence = (capture.default_output_dir("lease-deferral")
                         / "lease-deferral.json")
    evidence_path = Path(args.evidence)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    reader_log = evidence_path.parent / "slow_reader.log"

    record: dict = {
        "scenario": "lease-deferral",
        "target": args.target,
        "usb_path": args.usb_path,
        "hold_before_drop_s": args.hold_before_drop,
        "hold_after_drop_s": args.hold_after_drop,
        "relock_wait_s": args.relock_wait,
        "steps": [],
    }
    started = time.monotonic()

    def note(label: str, **fields) -> None:
        entry = {"t": round(time.monotonic() - started, 2), "label": label}
        entry.update(fields)
        record["steps"].append(entry)
        print(json.dumps(entry), flush=True)

    def emit() -> None:
        evidence_path.write_text(json.dumps(record, indent=1) + "\n",
                                 encoding="utf-8")

    before = get_json(args.target, "/api/recorder", auth)
    note("before", header=header_summary(before))
    candidate = (before["active_slot"] + 1) % before["slot_count"]
    candidate_slot = before["slots"][candidate]
    record["candidate_slot"] = candidate
    if candidate_slot["state"] != "frozen":
        # Nothing for the bank to defer over: the next recycle would take a
        # slot that is not a capture, so a clean run would prove nothing.
        note("abort", reason="candidate slot not frozen", candidate=candidate)
        emit()
        return 2
    generation = candidate_slot["freeze_generation"]
    record["candidate_generation_before"] = generation

    reader: subprocess.Popen | None = None
    deauthorized = False
    log_handle = reader_log.open("w", encoding="utf-8")
    try:
        reader = subprocess.Popen(
            [sys.executable, str(Path(__file__).resolve().parent
                                 / "slow_reader.py"),
             "--target", args.target, "--auth", auth,
             "--slot", str(candidate), "--generation", str(generation),
             "--length", str(args.length),
             "--step-bytes", str(args.step_bytes),
             "--step-seconds", str(args.step_seconds)],
            stdout=log_handle, stderr=subprocess.STDOUT)
        note("reader_started", pid=reader.pid, slot=candidate,
             generation=generation, log=str(reader_log))

        time.sleep(args.hold_before_drop)
        usb_authorize(args.usb_path, 0)
        deauthorized = True
        note("u24_deauthorized",
             readback=Path(args.usb_path).read_text().strip())

        time.sleep(args.hold_after_drop)
        # Own child, killed by its Popen handle -- never a name match, and
        # never a PID this process did not spawn.
        reader.kill()
        reader.wait(timeout=READER_KILL_TIMEOUT_S)
        note("reader_killed", returncode=reader.returncode)
        reader = None
        time.sleep(RELEASE_SETTLE_S)

        after_release = get_json(args.target, "/api/recorder", auth)
        record["candidate_generation_after_release"] = slot_generation(
            after_release, candidate)
        note("after_release", header=header_summary(after_release))

        usb_authorize(args.usb_path, 1)
        deauthorized = False
        note("u24_reauthorized",
             readback=Path(args.usb_path).read_text().strip())
        time.sleep(args.relock_wait)

        after_relock = get_json(args.target, "/api/recorder", auth)
        record["candidate_generation_after_relock"] = slot_generation(
            after_relock, candidate)
        note("after_relock", header=header_summary(after_relock))
        status = get_json(args.target, "/api/status", auth)
        note("status", build=status.get("build_revision"),
             clock_state=status.get("clock", {}).get("state"),
             rate=status.get("clock", {}).get("stream_rate_hz"),
             underrun=status.get("clock", {}).get("tx_underrun_frames"))
    finally:
        if reader is not None and reader.poll() is None:
            reader.kill()
            reader.wait(timeout=READER_KILL_TIMEOUT_S)
        log_handle.close()
        if deauthorized:
            # ALWAYS restore the U24. A tool that exits leaving the bench
            # source de-authorized silently takes the room's audio down and
            # every later run reads a carrier that is not there.
            usb_authorize(args.usb_path, 1)
            note("u24_reauthorized_on_exit",
                 readback=Path(args.usb_path).read_text().strip())
        emit()

    capture.announce_evidence(evidence_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
