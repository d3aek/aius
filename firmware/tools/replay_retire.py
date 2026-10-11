#!/usr/bin/env python3
"""Retire the card's replay captures and web pages (plan Phase 4i).

  replay_retire.py --device URL --auth-file PATH [--archive-dir DIR] [--manifest FILE] [--force] archive
  replay_retire.py --device URL --auth-file PATH [--archive-dir DIR] [--manifest FILE] delete-captures
  replay_retire.py --device URL --auth-file PATH delete-www

archive: GET /api/replays (refuses truncated:true), then GET /sd/replays/<id>
for every complete .iec row into DIR/<id>.iec, each checked against the
listing's size, and write the manifest (default evidence/AIUS-REPLAY-CAPTURES.manifest.json,
normal git; refuses to overwrite one without --force): id, bytes and sha256 per capture,
name and listed size of every .part row, the board's build_revision. No date.
The listing rows are {id, bytes, complete, ...} (optical_main.cpp
http_replay_list_json; complete true is .iec, false is .part).

delete-captures: DELETE /api/replays {"id"} for each listed .iec whose archive
file and the manifest are committed (in HEAD, working tree and index equal HEAD;
git cat-file / diff, read-only) and whose sha256 and listed size match the manifest. A fresh listing (refuses truncated:true) must then show no
.iec row, else exit 1 naming them: the firmware delete removes <id>.iec before
<id>.part, so a .part pass beside a skipped .iec would destroy the unarchived
capture. Only then each manifest .part id still listed is deleted, and the
final listing must be empty.

delete-www: DELETE /sd/www/replays.{html,js,css,html.gz}, then poll
/api/status until sd_flush.drained is true (bounded; exit 1 on timeout).

Every call retries 503 sd_busy (bounded). The token is read from --auth-file
and sent as X-Aius-SD-Auth; it is never an argument. Default DIR is
docs/reference/datasheets.mdAIUS-REPLAY-CAPTURES in this repo.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_ARCHIVE = REPO / "evidence" / "artifacts" / "AIUS-REPLAY-CAPTURES"
# Normal git, beside the LFS-only docs/reference/datasheets.md (aius/CLAUDE.md).
DEFAULT_MANIFEST = REPO / "evidence" / "AIUS-REPLAY-CAPTURES.manifest.json"
WWW_FILES = ("replays.html", "replays.js", "replays.css", "replays.html.gz")
BUSY_ATTEMPTS = 30
BUSY_DEFAULT_WAIT_S = 1.0
DRAIN_TIMEOUT_S = 60.0
DRAIN_POLL_S = 1.0
sleep = time.sleep
clock = time.monotonic


class Failure(Exception):
    pass


class Device:
    def __init__(self, base, token):
        self.base = base.rstrip("/")
        self.token = token

    def call(self, method, path, obj=None):
        """(status, body) with 503 sd_busy retried, bounded."""
        data = json.dumps(obj).encode() if obj is not None else None
        for attempt in range(BUSY_ATTEMPTS):
            req = urllib.request.Request(self.base + path, data=data, method=method)
            if self.token:
                req.add_header("X-Aius-SD-Auth", self.token)
            if data is not None:
                req.add_header("Content-Type", "application/json")
            try:
                with urllib.request.urlopen(req, timeout=30) as response:
                    status, body, headers = response.status, response.read(), response.headers
            except urllib.error.HTTPError as error:
                status, body, headers = error.code, error.read(), error.headers
            except (OSError, http.client.HTTPException) as error:
                raise Failure(f"{method} {path}: {error}")
            if status == 503 and b"sd_busy" in body:
                try:
                    wait = max(float(headers.get("Retry-After")), 0.0)
                except (TypeError, ValueError):
                    wait = BUSY_DEFAULT_WAIT_S
                sleep(wait)
                continue
            return status, body
        raise Failure(f"{method} {path}: still sd_busy after {BUSY_ATTEMPTS} attempts")

    def json(self, method, path, obj=None, ok=(200,)):
        status, body = self.call(method, path, obj)
        if status not in ok:
            raise Failure(f"{method} {path}: HTTP {status} {body[:200]!r}")
        try:
            return json.loads(body) if body else {}
        except ValueError:
            raise Failure(f"{method} {path}: not JSON")

    def listing(self):
        reply = self.json("GET", "/api/replays")
        if reply.get("ok") is not True:
            raise Failure(f"GET /api/replays: {reply}")
        if reply.get("truncated") is not False:
            raise Failure("GET /api/replays: truncated listing, set is incomplete")
        files = reply.get("files")
        if not isinstance(files, list):
            raise Failure("GET /api/replays: no files array")
        return files


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def cmd_archive(device, archive_dir, manifest_path, force=False, **_):
    if manifest_path.exists() and not force:
        raise Failure(f"{manifest_path} exists; --force overwrites the manifest and captures")
    files = device.listing()
    status = device.json("GET", "/api/status")
    archive_dir.mkdir(parents=True, exist_ok=True)
    captures, parts = [], []
    for row in files:
        if row.get("complete") is True:
            name = row["id"]
            code, body = device.call("GET", f"/sd/replays/{name}")
            if code != 200:
                raise Failure(f"GET /sd/replays/{name}: HTTP {code}")
            if len(body) != row["bytes"]:
                raise Failure(f"{name}: downloaded {len(body)} bytes, listing says {row['bytes']}")
            (archive_dir / f"{name}.iec").write_bytes(body)
            captures.append({"id": name, "bytes": len(body),
                             "sha256": hashlib.sha256(body).hexdigest()})
        else:
            parts.append({"name": f"{row['id']}.part", "id": row["id"], "bytes": row["bytes"]})
    manifest = {"build_revision": status.get("build_revision", ""),
                "captures": captures, "parts": parts}
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"archived {len(captures)} captures, recorded {len(parts)} .part rows")
    return 0


def committed(path, check):
    """True when path exists in HEAD and the working tree (and index) equal HEAD.

    Tracked is not enough: a re-archive overwrites a tracked file in place."""
    if check == "none":
        return True
    base = ["git", "-C", str(path.parent)]
    # No index refresh, so no index.lock in a checkout other sessions share.
    env = {**os.environ, "GIT_OPTIONAL_LOCKS": "0"}
    try:
        in_head = subprocess.run(base + ["cat-file", "-e", f"HEAD:./{path.name}"],
                                 capture_output=True, env=env)
        if in_head.returncode != 0:
            return False
        clean = subprocess.run(base + ["diff", "--quiet", "HEAD", "--", path.name],
                               capture_output=True, env=env)
    except OSError as error:
        raise Failure(f"git: {error}")
    return clean.returncode == 0


def cmd_delete_captures(device, archive_dir, manifest_path, committed_check="git", **_):
    if not manifest_path.is_file() or not committed(manifest_path, committed_check):
        raise Failure(f"{manifest_path}: manifest missing or not committed")
    manifest = json.loads(manifest_path.read_text())
    shas = {c["id"]: c["sha256"] for c in manifest["captures"]}
    sizes = {c["id"]: c["bytes"] for c in manifest["captures"]}
    for row in device.listing():
        name = row["id"]
        if row.get("complete") is not True:
            continue
        path = archive_dir / f"{name}.iec"
        if name not in shas or not path.is_file():
            print(f"skip {name}: no archived file", file=sys.stderr)
        elif row.get("bytes") != sizes[name]:
            print(f"skip {name}: card size {row.get('bytes')} != archived {sizes[name]}",
                  file=sys.stderr)
        elif not committed(path, committed_check):
            print(f"skip {name}: archive not committed", file=sys.stderr)
        elif sha256_file(path) != shas[name]:
            print(f"skip {name}: sha256 mismatch", file=sys.stderr)
        else:
            device.json("DELETE", "/api/replays", {"id": name})
            print(f"deleted {name}.iec")
    fresh = device.listing()
    left = [r["id"] for r in fresh if r.get("complete") is True]
    if left:
        print("remaining .iec: " + " ".join(left) + " (no .part deleted)", file=sys.stderr)
        return 1
    part_ids = {p["id"] for p in manifest["parts"]}
    for row in fresh:
        if row["id"] in part_ids:
            device.json("DELETE", "/api/replays", {"id": row["id"]})
            print(f"deleted {row['id']}.part")
    final = device.listing()
    if final:
        print("listing not empty: " + " ".join(
            f"{r['id']}.{'iec' if r.get('complete') else 'part'}" for r in final), file=sys.stderr)
        return 1
    print("listing empty")
    return 0


def cmd_delete_www(device, **_):
    for name in WWW_FILES:
        device.json("DELETE", f"/sd/www/{name}")
        print(f"deleted www/{name}")
    deadline = clock() + DRAIN_TIMEOUT_S
    while True:
        status = device.json("GET", "/api/status")
        if (status.get("sd_flush") or {}).get("drained") is True:
            print("sd_flush drained")
            return 0
        if clock() >= deadline:
            print(f"sd_flush.drained still false after {DRAIN_TIMEOUT_S:g} s", file=sys.stderr)
            return 1
        sleep(DRAIN_POLL_S)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Retire replay captures and pages.")
    parser.add_argument("--device", required=True, help="board base URL")
    parser.add_argument("--auth-file", required=True, help="file holding the SD auth token")
    parser.add_argument("--archive-dir", type=Path, default=DEFAULT_ARCHIVE)
    parser.add_argument("--committed-check", choices=("git", "none"), default="git",
                        help="how delete-captures proves an archive is committed")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST,
                        help="manifest path (normal git, outside the LFS artifact dir)")
    parser.add_argument("--force", action="store_true",
                        help="archive: overwrite an existing manifest and captures")
    parser.add_argument("command", choices=("archive", "delete-captures", "delete-www"))
    args = parser.parse_args(argv)
    token = Path(args.auth_file).read_text().strip()
    device = Device(args.device, token)
    handler = {"archive": cmd_archive, "delete-captures": cmd_delete_captures,
               "delete-www": cmd_delete_www}[args.command]
    try:
        return handler(device, archive_dir=args.archive_dir,
                       manifest_path=args.manifest, force=args.force,
                       committed_check=args.committed_check)
    except Failure as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
