#!/usr/bin/env python3
"""Bounded HTTP exerciser for the Aius device control surface.

The soak drives only routes the firmware still serves. POST /api/mode and
POST /api/mix went with the single-config model (Phase 14 slice S4), so driving
them would spend every iteration on two 404s.

What that costs, stated rather than hidden: no request in the soak loop carries
a JSON body any more, because mode and mix were the only body-bearing routes
cheap enough to drive once a second. The two that remain -- PUT
/api/config/public and PUT /api/dsp/config -- each commit an SD write, and a
per-second soak must not spend the card. The loop therefore covers the
serializers, the command routes and the malformed-request paths, but not a
complete buffered body. The buffered-body path is covered instead by the host
`http` suite; the one body the soak still exercises is the abandoned one.
"""

import argparse
import http.client
import json
import os
import socket
import time
from pathlib import Path


# One soak iteration's drive, cursor substituted per iteration. Every path here
# must be a route lib/aius_http/src/aius_http.c still resolves; a retired one
# turns its slot into a 404 and measures nothing.
SOAK_REQUESTS = (
    ("GET", "/api/status", None),
    ("GET", "/api/events?cursor={cursor}", None),
    ("POST", "/api/start", None),
    ("POST", "/api/stop", None),
    ("POST", "/api/clear", None),
)


def request(target, method, path, body=None, timeout=0.5, auth_token=None):
    """One request. Pass `auth_token` for a route that requires it.

    `POST /api/start`, `/api/stop`, `/api/clear` and `/api/reboot` are gated on
    `X-Aius-SD-Auth`; without it they answer 401 and perform nothing. A soak that
    drives them unauthenticated still records a result per request and therefore
    reads as covering them while covering nothing, so the token is threaded
    through rather than left to the caller to remember.
    """
    host, port = target
    started = time.monotonic()
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    encoded = None if body is None else json.dumps(body, separators=(",", ":"))
    headers = {"Connection": "close"}
    if auth_token:
        headers["X-Aius-SD-Auth"] = auth_token
    if encoded is not None:
        headers["Content-Type"] = "application/json"
    try:
        connection.request(method, path, encoded, headers)
        response = connection.getresponse()
        raw = response.read()
        result = {
            "status": response.status,
            "latency_ms": round((time.monotonic() - started) * 1000, 3),
            "body": json.loads(raw.decode("utf-8")) if raw else None,
        }
        return result
    finally:
        connection.close()


def request_bytes(target, method, path, body=None, content_type="application/octet-stream",
                  timeout=0.5):
    host, port = target
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    headers = {"Connection": "close", "Content-Type": content_type}
    try:
        connection.request(method, path, body, headers)
        response = connection.getresponse()
        return {"status": response.status, "body": response.read()}
    finally:
        connection.close()


def abandoned_request(target):
    """Promise a body, then disconnect -- the incomplete-body recovery path.

    The route has to be one that genuinely accepts a buffered body. The parser
    resolves the route and checks the method before it checks the body
    (aius_http.c), so an unknown or method-mismatched path is answered from the
    headers alone and the device never waits for the body this never sends.
    PUT /api/dsp/config is the canonical buffered-body route; no SD write can
    follow, because an incomplete body is never dispatched.
    """
    host, port = target
    connection = socket.create_connection((host, port), timeout=0.5)
    connection.sendall(
        b"PUT /api/dsp/config HTTP/1.0\r\nContent-Length: 256\r\n\r\n")
    connection.close()


def raw_response_record(response):
    parts = response.split(b" ", 2)
    if len(parts) >= 2 and parts[1].isdigit():
        return {"method": "MALFORMED", "status": int(parts[1])}
    return {"method": "MALFORMED", "status": None, "outcome": "connection_closed"}


def exercise_once(target, cursor, auth_token=None):
    records = []
    for method, template, body in SOAK_REQUESTS:
        path = template.format(cursor=cursor)
        # The three POSTs in SOAK_REQUESTS are gated; the two GETs are not, and
        # sending the header to a route that ignores it is harmless.
        result = request(target, method, path, body, auth_token=auth_token)
        records.append({"method": method, "path": path,
                        "authenticated": bool(auth_token), **result})
    for raw in (
        b"GET /api/status HTTP/2.0\r\n\r\n",
        b"PUT /api/status HTTP/1.0\r\n\r\n",
        b"GET /api/status HTTP/1.0\r\n",
    ):
        host, port = target
        connection = socket.create_connection((host, port), timeout=0.5)
        connection.sendall(raw)
        connection.settimeout(0.5)
        try:
            response = connection.recv(256)
            records.append(raw_response_record(response))
        finally:
            connection.close()
    abandoned_request(target)
    # NativeEthernet may need a short close-wait interval after the intentionally
    # incomplete body before its next listening socket is ready.
    time.sleep(0.25)
    return records


def run_soak(target, duration, interval, evidence, auth_token=None):
    deadline = time.monotonic() + duration
    cursor = 0
    index = 0
    evidence_path = Path(evidence)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    with evidence_path.open("a", encoding="utf-8") as stream:
        while time.monotonic() < deadline:
            started = time.monotonic()
            record = {"kind": "http_iteration", "iteration": index,
                      "started_at": time.time()}
            try:
                records = exercise_once(target, cursor, auth_token)
                status = request(target, "GET", "/api/status")
                events = request(target, "GET", f"/api/events?cursor={cursor}")
                if isinstance(events.get("body"), dict):
                    cursor = int(events["body"].get("next", cursor))
                record.update({"requests": records, "status": status,
                               "events": events, "duration_ms": round((time.monotonic() - started) * 1000, 3)})
            except (OSError, ValueError, http.client.HTTPException) as error:
                record.update({"error": type(error).__name__, "message": str(error)})
            stream.write(json.dumps(record, sort_keys=True) + "\n")
            stream.flush()
            index += 1
            time.sleep(max(0.0, interval - (time.monotonic() - started)))


def reboot_and_wait(target, timeout, auth_token=None):
    # Gated route: without the token this is a 401 and the board does not reset,
    # so the wait below would time out against a board that never rebooted.
    acknowledgement = request(target, "POST", "/api/reboot", auth_token=auth_token)
    deadline = time.monotonic() + timeout
    attempts = 0
    last_error = None
    while time.monotonic() < deadline:
        attempts += 1
        try:
            status = request(target, "GET", "/api/status", timeout=0.5)
            return {"acknowledgement": acknowledgement, "attempts": attempts,
                    "status": status}
        except (OSError, http.client.HTTPException) as error:
            last_error = type(error).__name__
            time.sleep(0.25)
    raise TimeoutError(f"device did not return after reboot ({last_error})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("status", "soak", "reboot", "sd-status",
                                             "sd-get", "sd-put", "sd-delete"))
    parser.add_argument("--target", required=True, help="device host or host:port")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--evidence", default="aius-http-soak.jsonl")
    parser.add_argument("--resource", choices=("index.html","app.js", "style.css", "config.json"))
    parser.add_argument("--file")
    # The four control routes are gated on X-Aius-SD-Auth. Read the board's token
    # with `AIUS AUTH <id>` over the CDC port; without it `soak` records 401s for
    # its three POSTs and `reboot` never reboots.
    parser.add_argument("--auth-token", default=os.environ.get("AIUS_SD_TOKEN"),
                        help="X-Aius-SD-Auth for /api/start|stop|clear|reboot")
    # Agents pass the token by file: the classifier denies it on a command line.
    parser.add_argument("--auth-file", help="file whose first token is X-Aius-SD-Auth")
    args = parser.parse_args()
    if args.auth_file is not None:
        args.auth_token = Path(args.auth_file).read_text(encoding="utf-8").split()[0]
    if ":" in args.target and args.target.rsplit(":", 1)[1].isdigit():
        host, port_text = args.target.rsplit(":", 1)
        target = (host, int(port_text))
    else:
        target = (args.target, 80)
    if args.command == "status":
        print(json.dumps(request(target, "GET", "/api/status"), sort_keys=True))
    elif args.command == "reboot":
        print(json.dumps(reboot_and_wait(target, args.timeout, args.auth_token),
                         sort_keys=True))
    elif args.command == "sd-status":
        print(json.dumps(request(target, "GET", "/api/sd"), sort_keys=True))
    elif args.command == "sd-get":
        if not args.resource:
            parser.error("sd-get requires --resource")
        result = request_bytes(target, "GET", f"/sd/{args.resource}", timeout=args.timeout)
        if args.file:
            Path(args.file).write_bytes(result["body"])
        else:
            print(result["body"].decode("utf-8", errors="replace"), end="")
    elif args.command == "sd-put":
        if not args.resource or not args.file:
            parser.error("sd-put requires --resource and --file")
        body = Path(args.file).read_bytes()
        print(json.dumps(request_bytes(target, "PUT", f"/sd/{args.resource}", body,
                                        timeout=args.timeout), default=lambda value: value.decode("utf-8"),
                         sort_keys=True))
    elif args.command == "sd-delete":
        if not args.resource:
            parser.error("sd-delete requires --resource")
        print(json.dumps(request(target, "DELETE", f"/sd/{args.resource}"), sort_keys=True))
    else:
        run_soak(target, args.duration, args.interval, args.evidence,
                 args.auth_token)


if __name__ == "__main__":
    main()
