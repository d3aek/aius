#!/usr/bin/env python3
"""Poll GET /api/status at a fixed cadence through a listening-room session.

Writes every sample as one JSONL line ({"host_utc", "status"}) to --output,
and prints one line to stdout whenever a watched field changes, so a session
can be followed from the change log and analysed afterwards from the JSONL.
The 1 Hz default is the cadence the Phase G capture proved clean; reads need
no auth token.

Usage:
  aius_status_poll.py --device http://192.0.2.66 --output session.jsonl
  aius_status_poll.py --device http://192.0.2.66 --output s.jsonl --seconds 7200

Runs until --seconds elapse (0 = until interrupted).
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
import time
import urllib.request

# (label, path into the status document) for the change log.
WATCHED = (
    ("carrier", ("carrier",)),
    ("format", ("format",)),
    ("rate", ("sample_rate",)),
    ("clock", ("clock", "state")),
    ("gate_open", ("tx_gate", "open")),
    ("pipeline_class", ("pipeline_diag", "class")),
    ("underrun_episodes", ("clock", "underrun_episode_count")),
    ("tx_underrun_frames", ("clock", "tx_underrun_frames")),
    ("trim_updates", ("clock", "trim_updates")),
    ("governor_trims", ("clock", "governor_trims")),
    ("crc_rejects", ("pipeline_diag", "crc_rejects")),
    ("au_abandons", ("audio_diag", "au_abandons")),
    ("missed_fills", ("clock", "tx_missed_fills")),
)


def dig(document: dict, path: tuple[str, ...]):
    for key in path:
        if not isinstance(document, dict):
            return None
        document = document.get(key)
    return document


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--device", required=True)
    parser.add_argument("--output", required=True, help="JSONL sample file")
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--seconds", type=float, default=0.0)
    args = parser.parse_args()
    url = args.device.rstrip("/") + "/api/status"
    start = time.monotonic()
    previous: dict[str, object] = {}
    with open(args.output, "a", encoding="utf-8") as out:
        while args.seconds <= 0 or time.monotonic() - start < args.seconds:
            tick = time.monotonic()
            now = datetime.datetime.now(datetime.timezone.utc)
            stamp = now.strftime("%H:%M:%S")
            try:
                with urllib.request.urlopen(url, timeout=5) as response:
                    status = json.loads(response.read())
            except Exception as error:  # board busy or unreachable: log, go on
                print(f"{stamp} poll failed: {type(error).__name__}: {error}",
                      flush=True)
                status = None
            if status is not None:
                out.write(json.dumps({"host_utc": now.isoformat(),
                                      "status": status}) + "\n")
                out.flush()
                if not previous:
                    print(f"{stamp} baseline " + "; ".join(
                        f"{label} {dig(status, path)}"
                        for label, path in WATCHED), flush=True)
                changes = []
                for label, path in WATCHED:
                    value = dig(status, path)
                    if label in previous and previous[label] != value:
                        changes.append(f"{label} {previous[label]}->{value}")
                    previous[label] = value
                if changes:
                    clock = status.get("clock", {})
                    print(f"{stamp} " + "; ".join(changes)
                          + f" | carrier_mhz={clock.get('carrier_rate_millihz')}"
                          f" out_mhz={clock.get('output_rate_millihz')}"
                          f" tx_level={clock.get('tx_level_frames')}",
                          flush=True)
            time.sleep(max(0.0, args.interval - (time.monotonic() - tick)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
