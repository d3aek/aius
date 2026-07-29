#!/usr/bin/env python3
"""Validate the generated AC-3 IEC 61937 replay streams."""

from __future__ import annotations

import argparse
from pathlib import Path


PERIOD_BYTES = 6144
PAYLOAD_BYTES = 1792
PREAMBLE = (0xF872, 0x4E1F, 0x0001, 0x3800)


class ValidationError(Exception):
    """A generated replay stream violates the fixed AC-3 contract."""


class BitReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def read(self, width: int) -> int:
        value = 0
        for _ in range(width):
            byte_index = self.offset // 8
            if byte_index >= len(self.data):
                raise ValidationError("truncated AC-3 header")
            shift = 7 - (self.offset % 8)
            value = (value << 1) | ((self.data[byte_index] >> shift) & 1)
            self.offset += 1
        return value


def validate_ac3_header(frame: bytes, context: str) -> None:
    if len(frame) != PAYLOAD_BYTES:
        raise ValidationError(f"{context}: payload is {len(frame)} bytes")
    if frame[:2] != b"\x0b\x77":
        raise ValidationError(f"{context}: AC-3 sync word is {frame[:2].hex()}")

    bits = BitReader(frame[4:])
    fscod = bits.read(2)
    frmsizecod = bits.read(6)
    bits.read(5)  # bsid
    bits.read(3)  # bsmod
    acmod = bits.read(3)
    if (acmod & 1) and acmod != 1:
        bits.read(2)  # cmixlev
    if acmod & 4:
        bits.read(2)  # surmixlev
    if acmod == 2:
        bits.read(2)  # dsurmod
    lfeon = bits.read(1)

    if fscod != 0:
        raise ValidationError(f"{context}: fscod {fscod}, expected 48 kHz")
    if frmsizecod not in (30, 31):
        raise ValidationError(
            f"{context}: frmsizecod {frmsizecod}, expected 448 kbit/s"
        )
    if acmod != 7 or lfeon != 1:
        raise ValidationError(
            f"{context}: acmod/lfeon {acmod}/{lfeon}, expected 5.1 (7/1)"
        )


def unswap_payload(payload: bytes) -> bytes:
    if len(payload) % 2:
        raise ValidationError("odd IEC payload length")
    restored = bytearray(len(payload))
    restored[0::2] = payload[1::2]
    restored[1::2] = payload[0::2]
    return bytes(restored)


def validate_pair(raw_path: Path, iec_path: Path) -> int:
    raw = raw_path.read_bytes()
    iec = iec_path.read_bytes()
    if not iec or len(iec) % PERIOD_BYTES:
        raise ValidationError(
            f"{iec_path.name}: size {len(iec)} is not whole 6144-byte periods"
        )

    periods = len(iec) // PERIOD_BYTES
    if len(raw) != periods * PAYLOAD_BYTES:
        raise ValidationError(
            f"{raw_path.name}: size {len(raw)} does not match "
            f"{periods} x {PAYLOAD_BYTES}-byte payloads"
        )

    for index in range(periods):
        period = iec[index * PERIOD_BYTES : (index + 1) * PERIOD_BYTES]
        words = tuple(
            int.from_bytes(period[offset : offset + 2], "little")
            for offset in range(0, 8, 2)
        )
        context = f"{iec_path.name} period {index}"
        if words != PREAMBLE:
            got = "/".join(f"{word:04X}" for word in words)
            raise ValidationError(f"{context}: preamble {got}")

        wire_payload = period[8 : 8 + PAYLOAD_BYTES]
        payload = unswap_payload(wire_payload)
        expected = raw[index * PAYLOAD_BYTES : (index + 1) * PAYLOAD_BYTES]
        if payload != expected:
            raise ValidationError(f"{context}: payload differs from raw AC-3")
        if any(period[8 + PAYLOAD_BYTES :]):
            raise ValidationError(f"{context}: non-zero repetition-period padding")
        validate_ac3_header(payload, context)

    return periods


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()

    iec_paths = sorted(args.directory.glob("*_ac3.iec"))
    if not iec_paths:
        parser.error(f"no *_ac3.iec files in {args.directory}")

    total_periods = 0
    for iec_path in iec_paths:
        raw_name = iec_path.name.removesuffix("_ac3.iec") + ".ac3"
        raw_path = iec_path.with_name(raw_name)
        if not raw_path.is_file():
            raise ValidationError(f"{iec_path.name}: missing {raw_path.name}")
        periods = validate_pair(raw_path, iec_path)
        total_periods += periods
        print(f"ok: {iec_path.name}: {periods} periods")

    print(f"validated {len(iec_paths)} AC-3 streams, {total_periods} periods")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValidationError as error:
        raise SystemExit(f"error: {error}") from error
