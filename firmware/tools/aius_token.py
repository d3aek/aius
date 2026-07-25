#!/usr/bin/env python3
"""Derive the Aius SD maintenance token from a board's fused OCOTP MAC words.

Mirrors derive_auth_token() in firmware/src/optical_main.cpp and
aius_auth_derive_token() in lib/aius_auth/src/aius_auth.c, so a token can be
recomputed on the host from a board identity recorded once, without reattaching
USB.

What this does NOT do: recover the token from anything visible on the network.
The Ethernet MAC the device uses (02:41:49:55:01:01) is a hardcoded constant
shared by every board and is unrelated to the fused OCOTP words this reads. The
words are readable only on the device.

Usage:
  aius_token.py --mac0 0x0086416e --mac1 0x12345678 --mac2 0x9abcdef0
  aius_token.py --self-test

Exit codes: 0 success, 2 usage error, 3 self-test failure.
"""
import argparse
import sys

# lib/aius_auth/src/aius_auth.c: a compiled, public salt for domain separation,
# not a secret key. The firmware hashes sizeof(k_compiled_salt) bytes, which for
# a char array initialised from a string literal INCLUDES the NUL terminator --
# omitting it here would silently produce a different token.
SALT = b"AIUS-P7-physical-access-bootstrap-v1\x00"

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


def fnv1a64(data: bytes) -> int:
    state = FNV_OFFSET
    for byte in data:
        state = ((state ^ byte) * FNV_PRIME) & MASK64
    return state


def derive_token(material: bytes) -> str:
    """`material` is the 16 device bytes; returns 16 lowercase hex characters."""
    if len(material) != 16:
        raise ValueError(f"device material must be 16 bytes, got {len(material)}")
    return f"{fnv1a64(SALT + material):016x}"


def material_from_mac_words(mac0: int, mac1: int, mac2: int) -> bytes:
    """optical_main.cpp lays the material out as mac0, mac1, mac2, mac0 --
    little-endian, because it memcpy's the uint32_t words directly."""
    return b"".join(word.to_bytes(4, "little") for word in (mac0, mac1, mac2, mac0))


def self_test() -> bool:
    """Pinned against test/auth/test_auth.c's vector, so a change to either the
    firmware derivation or this script is caught rather than silently diverging
    from every token already recorded."""
    material = bytes(range(0x00, 0x100, 0x11))  # 00 11 22 ... ff
    expected = "e360e564a72368b9"
    actual = derive_token(material)
    if actual != expected:
        print(f"self-test FAILED: expected {expected}, got {actual}", file=sys.stderr)
        return False
    print(f"self-test ok: {expected}")
    return True


def parse_word(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError(f"not a 32-bit word: {text}")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Derive the Aius SD maintenance token from fused MAC words.")
    parser.add_argument("--mac0", type=parse_word, help="HW_OCOTP_MAC0")
    parser.add_argument("--mac1", type=parse_word, help="HW_OCOTP_MAC1")
    parser.add_argument("--mac2", type=parse_word, help="HW_OCOTP_MAC2")
    parser.add_argument("--self-test", action="store_true",
                        help="check the derivation against the pinned vector")
    args = parser.parse_args()

    if args.self_test:
        return 0 if self_test() else 3

    if args.mac0 is None or args.mac1 is None or args.mac2 is None:
        parser.print_usage(sys.stderr)
        print("all three of --mac0, --mac1 and --mac2 are required",
              file=sys.stderr)
        return 2

    print(derive_token(material_from_mac_words(args.mac0, args.mac1, args.mac2)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
