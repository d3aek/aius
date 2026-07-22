#!/usr/bin/env python3
"""Bounded HTTP exerciser for the Aius device control surface."""

import argparse
import http.client
import json
import socket
import time
from pathlib import Path


MODES = ("flat", "speech", "bass-reduced")
MIXES = ("standard", "lfe-minus10", "clipfree")


def request(target, method, path, body=None, timeout=0.5):
    host, port = target
    started = time.monotonic()
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    encoded = None if body is None else json.dumps(body, separators=(",", ":"))
    headers = {"Connection": "close"}
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
    host, port = target
    connection = socket.create_connection((host, port), timeout=0.5)
    connection.sendall(b"POST /api/mode HTTP/1.0\r\nContent-Length: 256\r\n\r\n")
    connection.close()


def raw_response_record(response):
    parts = response.split(b" ", 2)
    if len(parts) >= 2 and parts[1].isdigit():
        return {"method": "MALFORMED", "status": int(parts[1])}
    return {"method": "MALFORMED", "status": None, "outcome": "connection_closed"}


def exercise_once(target, cursor, mode, mix):
    records = []
    for method, path, body in (
        ("GET", "/api/status", None),
        ("GET", f"/api/events?cursor={cursor}", None),
        ("POST", "/api/mode", {"mode": mode}),
        ("POST", "/api/mix", {"mix": mix}),
        ("POST", "/api/start", None),
        ("POST", "/api/stop", None),
        ("POST", "/api/clear", None),
    ):
        result = request(target, method, path, body)
        records.append({"method": method, "path": path, **result})
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


def run_soak(target, duration, interval, evidence):
    deadline = time.monotonic() + duration
    cursor = 0
    index = 0
    evidence_path = Path(evidence)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    with evidence_path.open("a", encoding="utf-8") as stream:
        while time.monotonic() < deadline:
            mode = MODES[index % len(MODES)]
            mix = MIXES[index % len(MIXES)]
            started = time.monotonic()
            record = {"kind": "http_iteration", "iteration": index,
                      "started_at": time.time(), "mode": mode, "mix": mix}
            try:
                records = exercise_once(target, cursor, mode, mix)
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


def reboot_and_wait(target, timeout):
    acknowledgement = request(target, "POST", "/api/reboot")
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
    parser.add_argument("--resource", choices=("index.html", "app.js", "style.css", "config.json"))
    parser.add_argument("--file")
    args = parser.parse_args()
    if ":" in args.target and args.target.rsplit(":", 1)[1].isdigit():
        host, port_text = args.target.rsplit(":", 1)
        target = (host, int(port_text))
    else:
        target = (args.target, 80)
    if args.command == "status":
        print(json.dumps(request(target, "GET", "/api/status"), sort_keys=True))
    elif args.command == "reboot":
        print(json.dumps(reboot_and_wait(target, args.timeout), sort_keys=True))
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
        run_soak(target, args.duration, args.interval, args.evidence)


if __name__ == "__main__":
    main()
