#!/usr/bin/env python3
"""U24 carrier toggle -- drop or restore the bench source carrier.

Deauthorizing the ESI U24XL at its sysfs `authorized` flag drops the S/PDIF
carrier feeding the Teensy optical input; the board then reports no carrier and
will accept a replay (`POST /api/replays/start` answers `409
replay_carrier_present` while a carrier is up). Restoring the flag re-enumerates
the U24, which resets its PCM playback mixer -- re-apply unity separately
(`amixer -c 0 cset numid=5 110,110`) if a non-silent playback level is needed;
the carrier itself does not depend on the mixer.

This is the standalone toggle the gate tools lacked: `recorder_transitions.py`
and `lease_deferral_probe.py` drop the carrier only inside a full run and
re-authorize in a `finally`, so neither leaves it down for an out-of-band task
(a screenshot pass, a manual replay). The write is the same one-liner as
`recorder_transitions.usb_authorize`; it is inlined here to keep this helper
dependency-free (that module imports numpy). The flag is group-writable to
plugdev (the operator's group).

Owns: only the sysfs authorized toggle. It does NOT start replays, drive the
mixer, or poll the board -- those stay their own steps.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

DEFAULT_USB_PATH = "/sys/bus/usb/devices/3-1.1/authorized"


def set_authorized(path: str, value: int) -> None:
    """Write the USB `authorized` flag: 0 = carrier down, 1 = carrier up."""
    Path(path).write_text(str(int(value)), encoding="utf-8")


def read_authorized(path: str) -> str:
    return Path(path).read_text(encoding="utf-8").strip()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Drop or restore the U24 bench carrier.")
    parser.add_argument("action", choices=("drop", "restore", "state"),
                        help="drop = deauthorize (carrier down); restore = reauthorize; state = print flag")
    parser.add_argument("--usb-path", default=DEFAULT_USB_PATH,
                        help="U24 sysfs authorized flag (default: %(default)s)")
    args = parser.parse_args(argv)

    if args.action == "state":
        print(read_authorized(args.usb_path))
        return 0
    set_authorized(args.usb_path, 0 if args.action == "drop" else 1)
    print(f"{args.action}: authorized = {read_authorized(args.usb_path)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
