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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("status", "soak"))
    parser.add_argument("--target", required=True, help="device host or host:port")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--evidence", default="aius-http-soak.jsonl")
    args = parser.parse_args()
    if ":" in args.target and args.target.rsplit(":", 1)[1].isdigit():
        host, port_text = args.target.rsplit(":", 1)
        target = (host, int(port_text))
    else:
        target = (args.target, 80)
    if args.command == "status":
        print(json.dumps(request(target, "GET", "/api/status"), sort_keys=True))
    else:
        run_soak(target, args.duration, args.interval, args.evidence)


if __name__ == "__main__":
    main()
