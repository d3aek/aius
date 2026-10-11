#!/usr/bin/env python3
"""Stage a signed Aius OTA container over HTTP and walk it to ARMED — the
operator client, promoted from the 2026-08-16 session's working uploader
(aius-ota-resilience.md Phase 4b) with the incident's lessons folded in.

  tools/ota/aius_ota_flash.py --device http://<addr> --auth-file .secrets/aius-sd-auth firmware.aiuo
  tools/ota/aius_ota_flash.py --device http://<addr> --auth-file .secrets/aius-sd-auth --activate firmware.aiuo
  (--auth <token> is accepted for a hand-typed session; agents must use --auth-file)

What it drives, in the state machine's ACTUAL order (aius_ota.h):
  stage (resumable chunked PUT) -> verify -> arm -> [enable-activation ->
  activate, only with --activate].

Rules learned from the incident:
  - 500/503/507 and transport faults on a chunk are RESUMABLE: re-read the
    device's own next_offset (GET /api/ota) and continue from it. The device
    defers its session absorb to the SD write's commit, so a faulted chunk
    has not moved the session.
  - verify is a real step: arm refuses from STAGED (VERIFIED is the state arm
    consumes), so a client that skips verify dies at arm.
  - activation is THE COMMITMENT POINT. The interim operating rule
    (aius-ota-resilience.md) stands until Phase 3 ships: no remote activation
    without a same-day physical-access plan. Without --activate this client
    stops at ARMED and prints the rule.

Exit codes: 0 the requested terminal state was reached; 1 refusal or fault;
2 usage error.
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request

CHUNK = 4096  # AIUS_OTA_MAX_CHUNK_BYTES
RETRYABLE = (500, 503, 507)
MAX_CHUNK_FAULTS = 40


def urllib_transport(method, url, headers, body, timeout):
    """One HTTP exchange: (status | None, parsed-JSON dict). None spells a
    transport fault; both spellings are retryable at the chunk layer."""
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        try:
            payload = json.loads(error.read() or b"{}")
        except json.JSONDecodeError:
            payload = {}
        return error.code, payload
    except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as error:
        return None, {"transport": str(error)}


class Device:
    """The five verbs this client speaks, over an injectable transport so the
    host suite drives the whole flow against a state-machine fake."""

    def __init__(self, base, token, transport=urllib_transport):
        self.base = base.rstrip("/")
        self.token = token
        self.transport = transport

    def _call(self, method, path, body=None, timeout=10, headers=None):
        merged = {"X-Aius-SD-Auth": self.token}
        if headers:
            merged.update(headers)
        return self.transport(method, self.base + path, merged, body, timeout)

    def status(self):
        return self._call("GET", "/api/ota")

    def api_status(self):
        return self._call("GET", "/api/status")

    def put_chunk(self, image, offset, total):
        end = min(offset + CHUNK, total)
        return self._call(
            "PUT", "/api/ota/image", body=image[offset:end],
            headers={
                "Content-Range": f"bytes {offset}-{end - 1}/{total}",
                "Content-Type": "application/octet-stream",
            })

    def post(self, verb, timeout=30):
        return self._call("POST", f"/api/ota/{verb}", body=b"", timeout=timeout)


def resume_offset(device, total):
    """The device's own next_offset, the only resume authority. None when the
    status read itself failed (retryable)."""
    status, body = device.status()
    if status != 200:
        return None
    next_offset = body.get("next_offset")
    if next_offset is None:
        return None
    offset = int(next_offset)
    return offset if 0 <= offset <= total else None


def stage(device, image, log=print, sleep=time.sleep):
    """The resumable chunk loop. Returns True when the container is fully
    staged (the device reports state staged)."""
    total = len(image)
    log(f"container: {total} bytes, {(total + CHUNK - 1) // CHUNK} chunks")

    offset = 0
    status, body = device.status()
    if status == 200 and body.get("state") == "staging":
        resumed = resume_offset(device, total)
        if resumed is not None and resumed > 0:
            offset = resumed
            log(f"resuming staging at device offset {offset}")

    faults = 0
    while offset < total:
        status, body = device.put_chunk(image, offset, total)
        if status == 200:
            offset = int(body["next_offset"])
            continue
        faults += 1
        log(f"  chunk at {offset}: status {status} {body} (fault {faults})")
        if faults > MAX_CHUNK_FAULTS:
            log("too many chunk faults; aborting")
            return False
        if status in RETRYABLE or status is None:
            # Resumable per the route's contract: the chunk may or may not
            # have landed, and the device's next_offset is the truth. A
            # failed status read is itself retryable.
            sleep(0.5)
            resumed = resume_offset(device, total)
            if resumed is not None:
                offset = resumed
            continue
        if status == 409 and body.get("error") == "ota_range":
            # The instruction-bearing refusal: seek and continue.
            offset = int(body["next_offset"])
            continue
        log("non-retryable refusal; aborting")
        return False
    log(f"staged {total} bytes, {faults} retried faults")
    return True


def run(device, image, activate, log=print, sleep=time.sleep):
    if not stage(device, image, log=log, sleep=sleep):
        return 1

    # The state machine's actual order: STAGED -verify-> VERIFIED -arm->
    # ARMED. Each step confirms the state it was supposed to reach, so a
    # refusal names itself here rather than two verbs later.
    status, body = device.post("verify")
    log(f"verify: {status} {body}")
    if status != 200:
        return 1
    status, body = device.post("arm")
    log(f"arm: {status} {body}")
    if status != 200:
        return 1
    status, body = device.status()
    log(f"ota status: {status} {json.dumps(body)}")
    if status != 200 or body.get("state") != "armed":
        log(f"expected armed, got {body.get('state')}")
        return 1

    if not activate:
        log("ARMED. Stopping here: POST /api/ota/activate is the commitment "
            "point, and the interim rule (aius-ota-resilience.md) is no "
            "remote activation without a same-day physical-access plan. "
            "Re-run with --activate to proceed.")
        return 0

    status, body = post_waiting_flush(device, "enable-activation", log, sleep)
    log(f"enable-activation: {status} {body}")
    if status != 200:
        return 1 if not is_flush_pending(status, body) else FLUSH_EXIT
    status, body = post_waiting_flush(device, "activate", log, sleep)
    log(f"activate: {status} {body}")
    if is_flush_pending(status, body):
        return FLUSH_EXIT
    if status != 200 or not body.get("activating"):
        return 1
    fault = watch_activation(device, log, sleep)
    if fault is not None:
        log(f"ACTIVATION ABANDONED: activation_fault {fault}; the board "
            "stays on the running image. This is not a success.")
        return 1
    log("activation acknowledged; the board verifies, flashes and reboots now")
    return 0


FLUSH_EXIT = 1  # nonzero; a 409 left pending at the wait ceiling
FLUSH_WAIT_S = 60  # N6: a full sync drains in ~40 s source-off; 30 s cut short
FLUSH_POLL_S = 1.0
ABANDON_FAULTS = ("ota_flush_pending", "ota_sd_busy")
ACTIVATION_WATCH_S = 15
ACTIVATION_POLL_S = 1.0


def is_flush_pending(status, body):
    return status == 409 and body.get("error") == "sd_flush_pending"


def post_waiting_flush(device, verb, log, sleep):
    """POST a verb; on 409 sd_flush_pending poll /api/status sd_flush (only
    `dirty` is read) for up to FLUSH_WAIT_S, stopping the moment it drains,
    then retry once. Still pending at the ceiling returns the last 409."""
    status, body = device.post(verb)
    if not is_flush_pending(status, body):
        return status, body
    waited = 0.0
    while waited < FLUSH_WAIT_S:
        st, sbody = device.api_status()
        dirty = (sbody.get("sd_flush") or {}).get("dirty") if st == 200 else None
        log(f"{verb}: 409 sd_flush_pending, sd_flush.dirty={dirty}; "
            f"waited {waited:.0f}s of {FLUSH_WAIT_S}s")
        if dirty == 0:
            status, body = device.post(verb)
            if not is_flush_pending(status, body):
                return status, body
            # Drained then re-dirtied (H9 re-check): keep waiting.
        sleep(FLUSH_POLL_S)
        waited += FLUSH_POLL_S
    st, sbody = device.api_status()
    dirty = (sbody.get("sd_flush") or {}).get("dirty") if st == 200 else None
    log(f"{verb}: still 409 sd_flush_pending after {FLUSH_WAIT_S}s, "
        f"sd_flush.dirty={dirty}; giving up")
    return status, body


def watch_activation(device, log, sleep):
    """After an acknowledged activate, poll /api/status ota_diag
    .activation_fault. Returns an abandon fault name, or None when none shows
    (a transport loss means the board is rebooting: the activation is live)."""
    waited = 0.0
    while waited < ACTIVATION_WATCH_S:
        st, body = device.api_status()
        if st is None:
            return None
        if st == 200:
            fault = (body.get("ota_diag") or {}).get("activation_fault")
            if fault in ABANDON_FAULTS:
                return fault
        sleep(ACTIVATION_POLL_S)
        waited += ACTIVATION_POLL_S
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--device", required=True,
                        help="base URL, e.g. http://192.0.2.66")
    auth = parser.add_mutually_exclusive_group(required=True)
    auth.add_argument("--auth", help="X-Aius-SD-Auth token")
    # The token on a command line is what the repo's guards refuse (aius
    # CLAUDE.md §Device access); a file path is the sanctioned form, the same
    # one the SD sync and recorder tools take.
    auth.add_argument("--auth-file",
                      help="file holding the X-Aius-SD-Auth token (one line)")
    parser.add_argument("--activate", action="store_true",
                        help="continue past ARMED through enable-activation "
                             "and activate (the commitment point; see the "
                             "interim rule)")
    parser.add_argument("container", help="signed .aiuo container")
    args = parser.parse_args()

    token = args.auth
    if args.auth_file is not None:
        with open(args.auth_file, "r", encoding="utf-8") as handle:
            token = handle.read().strip()
    with open(args.container, "rb") as handle:
        image = handle.read()
    return run(Device(args.device, token), image, args.activate)


if __name__ == "__main__":
    sys.exit(main())
