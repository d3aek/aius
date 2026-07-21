#!/usr/bin/env python3
"""Aius optical prototype UDP discovery/status runner."""

import argparse
import json
import random
import socket
import struct
import time
import zlib

MAGIC = b"AUN1"
VERSION = 1
HEADER = 16
CRC = 4
MAX_PAYLOAD = 512
PORT = 4242

CMD_DISCOVER = 0x01
CMD_GET_STATUS = 0x02
CMD_CLEAR_COUNTERS = 0x04
CMD_START_TEST = 0x05
CMD_STOP_TEST = 0x06
RSP_ACK = 0x80
RSP_DISCOVERY = 0x82
RSP_STATUS = 0x83


def encode(msg_type, request_id, payload=b"", flags=0):
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    frame = struct.pack("<4sBBHIHH", MAGIC, VERSION, msg_type, flags,
                        request_id, len(payload), 0) + payload
    return frame + struct.pack("<I", zlib.crc32(frame) & 0xffffffff)


def decode(frame):
    if len(frame) < HEADER + CRC:
        raise ValueError("short frame")
    magic, version, msg_type, flags, request_id, length, reserved = \
        struct.unpack_from("<4sBBHIHH", frame)
    if magic != MAGIC or version != VERSION or reserved != 0:
        raise ValueError("invalid header")
    expected = HEADER + length + CRC
    if length > MAX_PAYLOAD or len(frame) != expected:
        raise ValueError("invalid length")
    if struct.unpack_from("<I", frame, HEADER + length)[0] != \
            zlib.crc32(frame[:HEADER + length]) & 0xffffffff:
        raise ValueError("invalid crc")
    return {"type": msg_type, "flags": flags, "request_id": request_id,
            "payload": frame[HEADER:HEADER + length]}


def parse_status(payload):
    if len(payload) < 23 or payload[0] != 1:
        raise ValueError("unsupported status payload")
    result = {
        "network_mode": {1: "dhcp", 2: "static-fallback"}.get(payload[1], "unknown"),
        "state": {1: "boot-muted", 2: "waiting-for-carrier",
                   3: "pcm-active", 4: "dts-active"}.get(payload[2], "unknown"),
        "carrier_locked": bool(payload[3] & 1),
        "boot_epoch": struct.unpack_from("<I", payload, 4)[0],
        "uptime_ms": struct.unpack_from("<I", payload, 8)[0],
        "ip": ".".join(str(x) for x in payload[12:16]),
        "mac": ":".join(f"{x:02x}" for x in payload[16:22]),
        "ntp_valid": bool(payload[22]),
    }
    if len(payload) >= 37:
        result = {
            **result,
            "format": {0: "unknown", 1: "pcm", 2: "dts"}.get(payload[23], "unknown"),
            "pipeline_status": payload[24],
            "sample_rate_hz": struct.unpack_from("<I", payload, 25)[0],
            "pipeline_blocks": struct.unpack_from("<I", payload, 29)[0],
            "pipeline_overruns": struct.unpack_from("<I", payload, 33)[0],
        }
    return result


def request(target, msg_type, payload=b"", timeout=0.25, retries=3, broadcast=False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    if broadcast:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    request_id = random.randrange(1, 0xffffffff)
    packet = encode(msg_type, request_id, payload)
    try:
        for _ in range(retries):
            sock.sendto(packet, target)
            try:
                while True:
                    response, address = sock.recvfrom(2048)
                    decoded = decode(response)
                    if decoded["request_id"] == request_id:
                        return decoded, address
            except socket.timeout:
                pass
    finally:
        sock.close()
    raise TimeoutError(f"no response from {target[0]}:{target[1]}")


def discover(timeout):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(timeout)
    request_id = random.randrange(1, 0xffffffff)
    packet = encode(CMD_DISCOVER, request_id)
    found = []
    try:
        sock.sendto(packet, ("255.255.255.255", PORT))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                response, address = sock.recvfrom(2048)
            except socket.timeout:
                break
            try:
                decoded = decode(response)
                if decoded["request_id"] != request_id or decoded["type"] != RSP_DISCOVERY:
                    continue
                found.append({"source": address[0], "port": address[1],
                              **parse_status(decoded["payload"])})
            except ValueError:
                continue
    finally:
        sock.close()
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["discover", "status", "clear", "start", "stop"])
    parser.add_argument("--target", default=None, help="target IPv4 address")
    parser.add_argument("--timeout", type=float, default=0.25)
    args = parser.parse_args()

    if args.command == "discover":
        print(json.dumps(discover(max(args.timeout, 0.1)), indent=2, sort_keys=True))
        return
    if not args.target:
        parser.error("--target is required for this command")
    command = {"status": CMD_GET_STATUS, "clear": CMD_CLEAR_COUNTERS,
               "start": CMD_START_TEST, "stop": CMD_STOP_TEST}[args.command]
    response, address = request((args.target, PORT), command,
                                timeout=args.timeout)
    result = {"source": address[0], "type": response["type"],
              "request_id": response["request_id"]}
    if response["type"] in (RSP_DISCOVERY, RSP_STATUS):
        result["status"] = parse_status(response["payload"])
    elif response["type"] == RSP_ACK:
        result["ack"] = True
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
