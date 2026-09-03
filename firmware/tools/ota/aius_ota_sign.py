#!/usr/bin/env python3
"""Host-side OTA image signer for the Aius optical firmware.

Generates an Ed25519 keypair, signs a firmware image into a slice-1 image
container, and verifies a container by driving the device's own staged-upgrade
state machine over it.

    tools/ota/aius_ota_sign.py keygen --public K.pub --secret K.sec
    tools/ota/aius_ota_sign.py sign --image firmware.hex --secret K.sec \\
        --build-revision 7a66153703d1 --output firmware.aiuo
    tools/ota/aius_ota_sign.py verify --container firmware.aiuo --public K.pub

WHAT THIS DRIVES, AND WHY IT IS THIN. The container layout, the authentication
digest and the verification rules all live in `lib/aius_ota`, and this tool
calls that C code rather than re-implementing any of it in Python: a second copy
of the offsets would agree with the firmware only until somebody edited one, and
the thing it would be trusted to get right is precisely what the signature
covers. Two small native helpers are compiled on demand from the same vendored
TweetNaCl the firmware verifies with (see `tools/ota/aius_ota_cli.c` and
`aius_ota_keygen.c`); this file is argument handling, key hygiene and evidence.

WHY TWO HELPERS. `lib/aius_ota`'s adapter defines TweetNaCl's `randombytes` as a
trap that never returns, because a firmware image must never generate keys. Key
generation therefore cannot share a link with the signer, and gets its own
binary with a seeded entropy source. The signer links the trap and never trips
it, which is what proves Ed25519 signing needs no randomness.

DETERMINISM. Ed25519 is deterministic and the container has no timestamp of its
own beyond `--build-utc`, so signing the same image with the same key, target,
revision and build-utc produces a byte-identical container. Every run prints the
payload digest, authentication digest, signature, public key and container
digest so two runs, or two machines, can be compared without trusting either.

Env vars:
  CC   host C compiler used for the native helpers  (default: cc)

Exit codes: 0 ok; 1 usage, I/O or build error; 2 a refusal (verification failed,
or an output file already exists -- this tool never overwrites a key, a
container or an image).
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import subprocess
import sys
from pathlib import Path

FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
TOOL_DIR = FIRMWARE_ROOT / "tools" / "ota"
LIB_DIR = FIRMWARE_ROOT / "lib" / "aius_ota"
VENDOR_DIR = FIRMWARE_ROOT / "vendor" / "tweetnacl"
BUILD_DIR = FIRMWARE_ROOT / ".build" / "native"

EXIT_ERROR = 1
EXIT_REFUSED = 2

# The board this firmware is built for; an offered header's target_id must match
# the device's, and the device's comes from its own build.
DEFAULT_TARGET_ID = "teensy41-optical"

CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O2"]

# The vendored TweetNaCl carve-out, scoped to that one translation unit so the
# adapter and the OTA core keep full strictness. Its `FOR(i,n)` macro compares a
# signed loop counter against an unsigned bound throughout; the file is verbatim
# (vendor/tweetnacl/NOTICE.md) so the accommodation lives in the build.
VENDOR_CFLAGS = ["-Wno-sign-compare"]

LIB_SOURCES = (
    LIB_DIR / "src" / "aius_ota.c",
    LIB_DIR / "src" / "aius_sha256.c",
    LIB_DIR / "src" / "aius_ota_ed25519.c",
)

PRODUCTION_KEYGEN_HELP = """\
production keys are an operator step, deliberately not automated:

    tools/ota/aius_ota_sign.py keygen \\
        --public  ~/.config/aius/ota-signing-key.pub \\
        --secret  ~/.config/aius/ota-signing-key.sec
    chmod 400 ~/.config/aius/ota-signing-key.sec

The secret key is the only thing standing between a reachable device and
attacker-supplied firmware; for a PRODUCTION key keep it off the build host and
off the network. The public key is what a firmware build compiles in. keygen
refuses to overwrite either file, so rotating a key is a deliberate move of the
old one.

Three tiers, owner decision 2026-08-04:

- test/ota/test-only-signing-key.{pub,sec} -- the COMMITTED TEST FIXTURE. Its
  seed is published, so its secret needs no repo access; it exists so the host
  suites have a real keypair and must never be compiled into an image again.
- config/ota-home-signing-key.{pub,sec} -- the STANDING HOME KEY, committed
  deliberately: the repository is this home deployment's trust boundary, and a
  repo reader can already push commits to the firmware the key signs. Signs
  every home OTA for the foreseeable; ota_diag reports test_key false with its
  fingerprint.
- production -- the later cycle documented above: keygen to an off-repo path,
  swap the compiled-in bytes, one signed upgrade.
"""


def fail(message: str, code: int = EXIT_ERROR) -> int:
    print(f"aius_ota_sign: {message}", file=sys.stderr)
    return code


def newest(paths) -> float:
    return max(path.stat().st_mtime for path in paths)


def run_compiler(command: list[str]) -> None:
    print("==> " + " ".join(command), file=sys.stderr)
    subprocess.run(command, check=True)


def build_helpers(cc: str) -> tuple[Path, Path]:
    """Compile the two native helpers if any input is newer than them.

    Kept next to the suites' own build output (`.build/native/`, gitignored) so
    a checkout carries no binaries and a stale helper cannot survive an edit to
    the C sources it was built from.
    """
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    vendor_source = VENDOR_DIR / "tweetnacl.c"
    vendor_object = BUILD_DIR / "ota-tool-tweetnacl.o"
    keygen = BUILD_DIR / "aius-ota-keygen"
    cli = BUILD_DIR / "aius-ota-cli"

    headers = sorted((LIB_DIR / "include").glob("*.h"))
    vendor_inputs = [vendor_source, VENDOR_DIR / "tweetnacl.h"]
    if not vendor_object.exists() or vendor_object.stat().st_mtime < newest(
        vendor_inputs
    ):
        run_compiler(
            [cc, *CFLAGS, *VENDOR_CFLAGS, f"-I{VENDOR_DIR}", "-c",
             str(vendor_source), "-o", str(vendor_object)]
        )

    keygen_inputs = [TOOL_DIR / "aius_ota_keygen.c", vendor_object]
    if not keygen.exists() or keygen.stat().st_mtime < newest(keygen_inputs):
        run_compiler(
            [cc, *CFLAGS, f"-I{VENDOR_DIR}",
             str(TOOL_DIR / "aius_ota_keygen.c"), str(vendor_object),
             "-o", str(keygen)]
        )

    cli_inputs = [TOOL_DIR / "aius_ota_cli.c", vendor_object, *LIB_SOURCES,
                  *headers]
    if not cli.exists() or cli.stat().st_mtime < newest(cli_inputs):
        run_compiler(
            [cc, *CFLAGS, f"-I{LIB_DIR / 'include'}", f"-I{VENDOR_DIR}",
             str(TOOL_DIR / "aius_ota_cli.c"),
             *[str(source) for source in LIB_SOURCES],
             str(vendor_object), "-o", str(cli)]
        )
    return keygen, cli


def relay(completed: subprocess.CompletedProcess) -> int:
    """Print a helper's evidence lines and map its exit code to this tool's."""
    if completed.stdout:
        sys.stdout.write(completed.stdout)
    if completed.stderr:
        sys.stderr.write(completed.stderr)
    return completed.returncode


def command_keygen(args, keygen: Path) -> int:
    for path in (args.public, args.secret):
        if path.exists():
            return fail(f"{path} exists; keygen never overwrites a key",
                        EXIT_REFUSED)
    if args.seed_hex is not None:
        seed = args.seed_hex.strip()
        try:
            if len(bytes.fromhex(seed)) != 32:
                raise ValueError
        except ValueError:
            return fail("--seed-hex must be 64 hex characters (32 bytes)")
        print("==> seeded generation: this keypair is reproducible from the "
              "seed, so it is a TEST key, never a production one",
              file=sys.stderr)
    else:
        seed = os.urandom(32).hex()
    args.public.parent.mkdir(parents=True, exist_ok=True)
    args.secret.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [str(keygen), str(args.public), str(args.secret)],
        input=seed, text=True, capture_output=True,
    )
    return relay(completed)


def resolve_build_utc(args) -> int | None:
    """`--build-utc` as Unix seconds: an integer, an ISO-8601 date/time, or the
    image's own mtime. The default is the image's mtime rather than `now`, so a
    re-run over an unchanged image reproduces the container byte for byte."""
    if args.build_utc is None:
        return int(args.image.stat().st_mtime)
    text = args.build_utc.strip()
    if text.isdigit():
        return int(text)
    try:
        parsed = dt.datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return int(parsed.timestamp())


def flatten_intel_hex(path: Path) -> bytes | None:
    """Flatten an Intel-hex text file into the raw image bytes the container
    carries, or None when the file is not Intel hex. 0xFF fills alignment
    gaps (erased-flash value); a span over 8 MiB is refused as a parse gone
    wrong rather than padded into a monster.

    This exists because of a real incident (2026-08-16): `sign --image
    firmware.hex` signed the 1.6 MB ASCII TEXT of the hex file as the
    payload, the device faithfully staged, verified and flashed it, and the
    chooser's IVT sanity check was the only thing standing between the
    operator and a dead slot. The signature machinery cannot tell text from
    code -- this loader can."""
    try:
        with path.open("r", encoding="ascii") as handle:
            first = handle.readline()
            if not first.startswith(":"):
                return None
            regions: dict[int, bytes] = {}
            upper = 0
            handle.seek(0)
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                if not line.startswith(":"):
                    return None
                raw = bytes.fromhex(line[1:])
                if (sum(raw) & 0xFF) != 0:
                    return None
                count, kind = raw[0], raw[3]
                address = (raw[1] << 8) | raw[2]
                if kind == 4:
                    upper = ((raw[4] << 8) | raw[5]) << 16
                elif kind == 0:
                    regions[upper + address] = raw[4:4 + count]
    except (OSError, UnicodeDecodeError, ValueError):
        return None
    if not regions:
        return None
    base = min(regions)
    end = max(start + len(data) for start, data in regions.items())
    if end - base > 8 * 1024 * 1024:
        return None
    image = bytearray(b"\xff" * (end - base))
    for start, data in regions.items():
        image[start - base:start - base + len(data)] = data
    return bytes(image)


def command_sign(args, cli: Path) -> int:
    if not args.image.is_file():
        return fail(f"no such image: {args.image}")
    if not args.secret.is_file():
        return fail(f"no such secret key: {args.secret}")
    if args.output.exists():
        return fail(f"{args.output} exists; sign never overwrites a container",
                    EXIT_REFUSED)
    build_utc = resolve_build_utc(args)
    if build_utc is None:
        return fail("--build-utc must be Unix seconds or an ISO-8601 date")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image_path = args.image
    flattened = flatten_intel_hex(args.image)
    if flattened is not None:
        image_path = args.output.with_suffix(".flattened.bin")
        if image_path.exists():
            return fail(f"{image_path} exists; sign never overwrites an image",
                        EXIT_REFUSED)
        image_path.write_bytes(flattened)
        print(f"intel_hex_flattened={len(flattened)} bytes -> {image_path}")
    argv = [str(cli), "sign", str(image_path), str(args.secret),
            args.target_id, args.build_revision, str(build_utc),
            str(args.output)]
    if args.slot:
        # Still honoured so a board that is somehow back on the old per-slot
        # chooser can be served, but the deployed chooser refuses a tagged
        # container at chunk 0 (400 ota_unsupported), so say so every time.
        print("==> --slot is DEPRECATED: the slot-agnostic chooser accepts "
              "only the untagged container; this tagged container is for "
              "the retired per-slot chooser", file=sys.stderr)
        argv.append(args.slot)
    completed = subprocess.run(argv, text=True, capture_output=True)
    return relay(completed)


def command_verify(args, cli: Path) -> int:
    if not args.container.is_file():
        return fail(f"no such container: {args.container}")
    if not args.public.is_file():
        return fail(f"no such public key: {args.public}")
    completed = subprocess.run(
        [str(cli), "verify", str(args.container), str(args.public),
         args.target_id],
        text=True, capture_output=True,
    )
    return relay(completed)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="aius_ota_sign.py",
        description=__doc__.split("\n\n")[0],
        epilog=PRODUCTION_KEYGEN_HELP,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--cc", default=os.environ.get("CC", "cc"),
        help="host C compiler for the native helpers (default: $CC or cc)",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    keygen = commands.add_parser(
        "keygen", help="generate an Ed25519 keypair (refuses to overwrite)",
        epilog=PRODUCTION_KEYGEN_HELP,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    keygen.add_argument("--public", type=Path, required=True)
    keygen.add_argument("--secret", type=Path, required=True)
    keygen.add_argument(
        "--seed-hex",
        help="32-byte seed as hex, for reproducing a TEST key; omit for "
             "os.urandom",
    )

    sign = commands.add_parser(
        "sign", help="sign an image into a container",
    )
    sign.add_argument("--image", type=Path, required=True)
    sign.add_argument("--secret", type=Path, required=True)
    sign.add_argument("--output", type=Path, required=True)
    sign.add_argument("--target-id", default=DEFAULT_TARGET_ID)
    sign.add_argument(
        "--build-revision", required=True,
        help="the image's build identity, as reported by the running firmware",
    )
    sign.add_argument(
        "--build-utc",
        help="Unix seconds or an ISO-8601 date (default: the image's mtime)",
    )
    sign.add_argument(
        "--slot", choices=("a", "b"),
        help="DEPRECATED (aius-ota-slot-agnostic.md Phase 5): the signed A/B "
             "slot tag for the retired per-slot chooser. The slot-agnostic "
             "chooser deployed since 2026-09-03 accepts only the untagged "
             "container -- omit this flag, which is the default",
    )

    verify = commands.add_parser(
        "verify",
        help="verify a container through the device's own state machine",
    )
    verify.add_argument("--container", type=Path, required=True)
    verify.add_argument("--public", type=Path, required=True)
    verify.add_argument("--target-id", default=DEFAULT_TARGET_ID)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        keygen, cli = build_helpers(args.cc)
    except subprocess.CalledProcessError:
        return fail("could not build the native helpers")
    except OSError as error:
        return fail(f"could not build the native helpers: {error}")
    if args.command == "keygen":
        return command_keygen(args, keygen)
    if args.command == "sign":
        return command_sign(args, cli)
    return command_verify(args, cli)


if __name__ == "__main__":
    sys.exit(main())
