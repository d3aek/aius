"""Round trip: the host signer's containers, through the device's state machine.

This suite is the seam between `tools/ota/aius_ota_sign.py` and `lib/aius_ota`.
The C suites prove each half on its own -- `test_ota.c` the container and the
staged-upgrade machine against fakes, `test_ota_ed25519.c` the real curve behind
the verifier seam -- and neither can catch the failure that matters most here: a
signer whose artifact the device will not accept. So every case below drives the
real tool end to end and judges it by what the firmware's own
offer/absorb/verify/arm sequence says, which is what the `verify` verb runs.

Nothing here re-implements the container layout. The sizes and offsets a test
needs come out of the tool's printed evidence (`header_bytes`, `image_length`),
because a second copy of the format in Python would be a copy that agrees with
the firmware only until somebody edits one of them.

The refusals are the point of the rest: an existing key or container is never
overwritten, a container signed for another key or another board is refused
rather than misread, and a single flipped payload byte fails on the digest
before the signature is ever consulted.
"""

import hashlib
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SIGNER = ROOT / "tools" / "ota" / "aius_ota_sign.py"
sys.path.insert(0, str(SIGNER.parent))
import aius_ota_sign  # noqa: E402  (the module owns the native helper build)
TEST_KEY_DIR = ROOT / "test" / "ota"
COMMITTED_PUBLIC = TEST_KEY_DIR / "test-only-signing-key.pub"
COMMITTED_SECRET = TEST_KEY_DIR / "test-only-signing-key.sec"

# The seed the committed test-only key is generated from, published in the key
# files themselves: ASCII "aius-ota-test-only-key-notsecret".
PUBLISHED_SEED_HEX = (
    "616975732d6f74612d746573742d6f6e6c792d6b65792d6e6f74736563726574"
)

TARGET_ID = "teensy41-optical"
REVISION = "7a66153703d1"
BUILD_UTC = "1753900000"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_REFUSED = 2


def run_signer(*args):
    """Runs the tool and returns (returncode, evidence, stdout, stderr).

    `evidence` is the tool's `key=value` output as a dict -- the same lines an
    operator reads, so a test asserting on them is asserting on the artifact's
    published evidence rather than on an internal.
    """
    completed = subprocess.run(
        [sys.executable, str(SIGNER), *args],
        capture_output=True, text=True, check=False,
    )
    evidence = {}
    for line in completed.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            evidence[key] = value
    return completed.returncode, evidence, completed.stdout, completed.stderr


def key_hex(path):
    """The one hex line of a key file, ignoring its comment banner."""
    for line in path.read_text().splitlines():
        if line and not line.startswith("#"):
            return line.strip()
    raise AssertionError(f"{path} carries no key line")


# The flash windows the device judges a payload's IVT against
# (aius_ota_set_link_window): the slot-agnostic chooser's canonical window and
# the plain single-image build's boot window (lib/aius_ab/include/aius_ab_layout.h).
CANONICAL_BASE = 0x60010000
BOOT_BASE = 0x60000000
IVT_OFFSET = 0x1000
# 57977135e472's bytes at canonical 0x60011B80, as written out independently of
# the tool's constant so the two are compared rather than aliased.
PIN_OFFSET = 0x1B80
PIN_BYTES = bytes([
    0x02, 0x4a, 0x8b, 0x60, 0xc1, 0xe9, 0x00, 0xc2, 0x70, 0x47, 0x00, 0xbf,
    0x00, 0x80, 0x3e, 0x60, 0x00, 0x00, 0x01, 0x60, 0xbb, 0xb1, 0x4f, 0xf0,
    0x00, 0x0c, 0xd3, 0x18, 0x10, 0xb5, 0x4c, 0xeb,
])
LD_SCRIPT = ROOT / "link" / "aius_teensy41.ld"
IVT_ENTRY_OFFSET = 0x15F1  # where both 2026-09-03 builds' ResetHandler sits


def ivt_words(link_base, entry=None):
    """The eight-word Teensy 4 image vector table an image LINKED at
    `link_base` carries at +0x1000: header tag, entry, two reserved, boot-data
    pointer, self pointer, CSF, reserved."""
    return struct.pack(
        "<8I", 0x432000D1,
        link_base + IVT_ENTRY_OFFSET if entry is None else entry,
        0, 0, link_base + IVT_OFFSET + 32, link_base + IVT_OFFSET, 0, 0,
    )


def sample_image(path, length=8192, seed=0, link_base=CANONICAL_BASE,
                 entry=None):
    """A pseudo-random payload that IS an image: it carries the IVT of a
    build linked at `link_base`, since the device's verify (and therefore
    the signer's self-check) refuses anything that does not. A canonical
    image also carries the Phase 4z bridge pin the signer checks."""
    body = bytearray((index * 31 + seed) % 256 for index in range(length))
    body[IVT_OFFSET:IVT_OFFSET + 32] = ivt_words(link_base, entry)
    if link_base == CANONICAL_BASE and length >= PIN_OFFSET + len(PIN_BYTES):
        body[PIN_OFFSET:PIN_OFFSET + 32] = PIN_BYTES
    path.write_bytes(bytes(body))
    return path


def intel_hex_text(payload_length):
    """Intel-hex TEXT long enough to carry an 'IVT' at +0x1000 made of hex
    digits and newlines: the byte-for-byte shape of the 2026-08-16 incident's
    payload."""
    lines = [":020000046001" + "99"]
    address = 0
    while sum(len(line) + 1 for line in lines) + 44 <= payload_length:
        data = bytes((address + index) % 256 for index in range(16))
        raw = bytes([16, (address >> 8) & 0xFF, address & 0xFF, 0]) + data
        lines.append(":" + (raw + bytes([(-sum(raw)) & 0xFF])).hex().upper())
        address += 16
    text = "\n".join(lines) + "\n"
    return text.encode("ascii").ljust(payload_length, b"\n")


def native_cli():
    """The C helper the signer drives, built by the signer's own rule, for
    the one row that must bypass the Python layer's Intel-hex flattening."""
    _, cli = aius_ota_sign.build_helpers(os.environ.get("CC", "cc"))
    return cli


class SignerTestCase(unittest.TestCase):
    def setUp(self):
        self._work = tempfile.TemporaryDirectory()
        self.work = pathlib.Path(self._work.name)
        self.addCleanup(self._work.cleanup)

    def sign_sample(self, output_name="image.aiuo", length=8192, seed=0,
                    secret=None):
        image = sample_image(self.work / "image.bin", length=length, seed=seed)
        container = self.work / output_name
        code, evidence, stdout, stderr = run_signer(
            "sign",
            "--image", str(image),
            "--secret", str(secret or COMMITTED_SECRET),
            "--target-id", TARGET_ID,
            "--build-revision", REVISION,
            "--build-utc", BUILD_UTC,
            "--output", str(container),
        )
        self.assertEqual(code, EXIT_OK, f"sign failed:\n{stdout}\n{stderr}")
        return image, container, evidence


class KeygenTests(SignerTestCase):
    def test_keygen_writes_a_usable_pair(self):
        public = self.work / "fresh.pub"
        secret = self.work / "fresh.sec"
        code, evidence, stdout, stderr = run_signer(
            "keygen", "--public", str(public), "--secret", str(secret)
        )
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        self.assertTrue(public.is_file())
        self.assertTrue(secret.is_file())
        # The secret key's tail is the public key: that is TweetNaCl's layout,
        # and a pair that failed it would sign unverifiably.
        self.assertEqual(len(bytes.fromhex(key_hex(public))), 32)
        self.assertEqual(len(bytes.fromhex(key_hex(secret))), 64)
        self.assertTrue(key_hex(secret).endswith(key_hex(public)))
        self.assertEqual(evidence["public_key"], key_hex(public))

        # A fresh key is a fresh key: two runs must not agree.
        second_public = self.work / "second.pub"
        second_secret = self.work / "second.sec"
        run_signer("keygen", "--public", str(second_public),
                   "--secret", str(second_secret))
        self.assertNotEqual(key_hex(public), key_hex(second_public))

    def test_keygen_refuses_to_overwrite_either_file(self):
        public = self.work / "keep.pub"
        secret = self.work / "keep.sec"
        run_signer("keygen", "--public", str(public), "--secret", str(secret))
        before = (public.read_text(), secret.read_text())

        for existing in (public, secret):
            other = self.work / f"other-{existing.suffix}"
            code, _, _, stderr = run_signer(
                "keygen",
                "--public", str(public if existing is public else other),
                "--secret", str(secret if existing is secret else other),
            )
            self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertEqual((public.read_text(), secret.read_text()), before)

    def test_the_committed_test_key_is_the_published_seed(self):
        """The committed pair is regenerable, and the files say how.

        This is what keeps the test key honest: it is not a mystery blob that
        happens to be in the tree, it is the keypair of a seed printed in both
        key files, so anyone can confirm the secret half is public knowledge and
        therefore that this key can never be mistaken for a production one.
        """
        public = self.work / "regenerated.pub"
        secret = self.work / "regenerated.sec"
        code, _, stdout, stderr = run_signer(
            "keygen", "--public", str(public), "--secret", str(secret),
            "--seed-hex", PUBLISHED_SEED_HEX,
        )
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        self.assertEqual(key_hex(public), key_hex(COMMITTED_PUBLIC))
        self.assertEqual(key_hex(secret), key_hex(COMMITTED_SECRET))


class SignTests(SignerTestCase):
    def test_sign_emits_the_container_and_its_evidence(self):
        image, container, evidence = self.sign_sample()
        payload = image.read_bytes()

        header_bytes = int(evidence["header_bytes"])
        self.assertEqual(int(evidence["image_length"]), len(payload))
        self.assertEqual(int(evidence["container_bytes"]),
                         header_bytes + len(payload))
        self.assertEqual(container.stat().st_size, header_bytes + len(payload))
        # The container carries the image verbatim after the header.
        self.assertEqual(container.read_bytes()[header_bytes:], payload)

        # Every printed digest independently recomputed here.
        self.assertEqual(evidence["payload_sha256"],
                         hashlib.sha256(payload).hexdigest())
        self.assertEqual(evidence["container_sha256"],
                         hashlib.sha256(container.read_bytes()).hexdigest())
        # The authenticated prefix is the header up to the signature slot, and
        # its length comes from the tool rather than from a constant repeated
        # here. Recomputing it independently is what proves the signature
        # covers the image's CLAIMS (target, revision, build time, length) and
        # not only its payload digest.
        self.assertEqual(evidence["auth_digest"],
                         hashlib.sha256(container.read_bytes()[
                             :int(evidence["auth_bytes"])]).hexdigest())
        self.assertEqual(evidence["public_key"], key_hex(COMMITTED_PUBLIC))
        self.assertEqual(evidence["signature_scheme"], "1")
        self.assertEqual(len(bytes.fromhex(evidence["signature"])), 64)

    def test_signing_is_deterministic(self):
        """Same image, key, target, revision and build-utc -> same bytes.

        Ed25519 signs deterministically and the container has no other varying
        field, so this is checkable rather than aspirational -- and it is what
        lets two machines compare an artifact by hash instead of by trust.
        """
        _, first, first_evidence = self.sign_sample(output_name="a.aiuo")
        _, second, second_evidence = self.sign_sample(output_name="b.aiuo")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(first_evidence["container_sha256"],
                         second_evidence["container_sha256"])
        self.assertEqual(first_evidence["signature"],
                         second_evidence["signature"])

    def test_an_intel_hex_image_is_flattened_not_signed_as_text(self):
        """The 2026-08-16 incident row: `--image firmware.hex` used to sign
        the hex file's ASCII as the payload; the device staged, verified and
        flashed 1.6 MB of text and only the chooser's IVT sanity check kept
        the slot bootable. Hex input must land as the flattened binary."""
        payload = bytes((index * 7 + 3) % 256 for index in range(48))

        def record(address, data):
            raw = bytes([len(data), (address >> 8) & 0xFF, address & 0xFF, 0])
            raw += data
            checksum = (-sum(raw)) & 0xFF
            return ":" + (raw + bytes([checksum])).hex().upper()

        def ela(upper):
            raw = bytes([2, 0, 0, 4, (upper >> 8) & 0xFF, upper & 0xFF])
            checksum = (-sum(raw)) & 0xFF
            return ":" + (raw + bytes([checksum])).hex().upper()

        ivt = ivt_words(CANONICAL_BASE)
        lines = [ela(0x6001)]
        lines.append(record(0x0000, payload[:16]))
        lines.append(record(0x0010, payload[16:32]))
        # A 4-byte alignment gap, as the real slot images carry.
        lines.append(record(0x0024, payload[32:48]))
        # The IVT at +0x1000, so the flattened result IS an image the
        # signer's self-check (the device's own verify) accepts; the erased
        # span between is 0xFF exactly as flash would read it.
        lines.append(record(0x1000, ivt[:16]))
        lines.append(record(0x1010, ivt[16:32]))
        lines.append(record(PIN_OFFSET, PIN_BYTES[:16]))
        lines.append(record(PIN_OFFSET + 16, PIN_BYTES[16:]))
        lines.append(":00000001FF")
        image = self.work / "image.hex"
        image.write_text("\n".join(lines) + "\n")
        expected = (payload[:32] + b"\xff" * 4 + payload[32:48]
                    + b"\xff" * (IVT_OFFSET - 0x34) + ivt
                    + b"\xff" * (PIN_OFFSET - IVT_OFFSET - 32) + PIN_BYTES)

        container = self.work / "image.aiuo"
        code, evidence, stdout, stderr = run_signer(
            "sign",
            "--image", str(image),
            "--secret", str(COMMITTED_SECRET),
            "--target-id", TARGET_ID,
            "--build-revision", REVISION,
            "--build-utc", BUILD_UTC,
            "--output", str(container),
        )
        self.assertEqual(code, EXIT_OK, f"sign failed:\n{stdout}\n{stderr}")
        self.assertEqual(int(evidence["image_length"]), len(expected))
        header_bytes = int(evidence["header_bytes"])
        self.assertEqual(container.read_bytes()[header_bytes:], expected)
        self.assertEqual(evidence["payload_sha256"],
                         hashlib.sha256(expected).hexdigest())

    def test_default_container_is_untagged_for_the_slot_agnostic_chooser(self):
        """One build, one signature (aius-ota-slot-agnostic.md Phase 5): the
        deployed chooser accepts only a `slot_id = NONE` container, so the
        signer's default must be the untagged one, and it must say so in the
        evidence rather than leave the operator to infer it."""
        _, container, evidence = self.sign_sample()
        self.assertEqual(evidence["slot_id"], "none")
        code, verify_evidence, stdout, stderr = run_signer(
            "verify", "--container", str(container),
            "--public", str(COMMITTED_PUBLIC), "--target-id", TARGET_ID,
        )
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        self.assertEqual(verify_evidence["slot_id"], "none")

    def test_slot_tag_is_deprecated_but_still_honoured(self):
        """`--slot` serves only the retired per-slot chooser. It keeps
        working, so a board somehow back on that chooser can be served, but
        every use is announced as deprecated on stderr and the tag lands in
        the evidence, because the deployed chooser refuses the tagged
        container at chunk 0 and the operator must be able to see why."""
        image = sample_image(self.work / "image.bin")
        container = self.work / "tagged.aiuo"
        code, evidence, stdout, stderr = run_signer(
            "sign",
            "--image", str(image),
            "--secret", str(COMMITTED_SECRET),
            "--target-id", TARGET_ID,
            "--build-revision", REVISION,
            "--build-utc", BUILD_UTC,
            "--slot", "a",
            "--output", str(container),
        )
        self.assertEqual(code, EXIT_OK, f"sign failed:\n{stdout}\n{stderr}")
        self.assertEqual(evidence["slot_id"], "a")
        self.assertIn("DEPRECATED", stderr)
        code, verify_evidence, stdout, stderr = run_signer(
            "verify", "--container", str(container),
            "--public", str(COMMITTED_PUBLIC), "--target-id", TARGET_ID,
        )
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        self.assertEqual(verify_evidence["slot_id"], "a")

    def test_sign_refuses_an_image_whose_ivt_is_out_of_window(self):
        """The signer's self-check is the device's own verify, IVT bounds
        included: an image whose reset vector points into RAM is refused at
        sign time with the device's reason, and no container is written."""
        image = sample_image(self.work / "image.bin", entry=0x20200000)
        container = self.work / "image.aiuo"
        code, evidence, stdout, stderr = run_signer(
            "sign",
            "--image", str(image),
            "--secret", str(COMMITTED_SECRET),
            "--target-id", TARGET_ID,
            "--build-revision", REVISION,
            "--build-utc", BUILD_UTC,
            "--output", str(container),
        )
        self.assertEqual(code, EXIT_ERROR, f"{stdout}\n{stderr}")
        self.assertEqual(evidence["refusal"], "verify")
        self.assertEqual(evidence["status"], "IVT")
        self.assertEqual(evidence["verifier_fault"], "0")
        self.assertEqual(evidence["state"], "FAILED")
        self.assertFalse(container.exists())

    def test_the_incident_payload_signed_raw_is_refused_as_not_an_image(self):
        """The 2026-08-16 shape at the device's seam: the pre-fix signer
        handed the C helper the hex file's ASCII text as the payload, and
        the device accepted it. The Python layer now flattens hex, so this
        row drives the C helper directly with the text -- exactly what the
        old signer did -- and the device's verify (which the helper's
        self-check IS) refuses it as not an image, before any container
        exists to stage."""
        text = self.work / "firmware-as-text.bin"
        text.write_bytes(intel_hex_text(5096))
        self.assertEqual(text.read_bytes()[0:1], b":")
        container = self.work / "text.aiuo"
        completed = subprocess.run(
            [str(native_cli()), "sign", str(text), str(COMMITTED_SECRET),
             TARGET_ID, REVISION, BUILD_UTC, str(container)],
            capture_output=True, text=True, check=False,
        )
        evidence = dict(line.partition("=")[::2]
                        for line in completed.stdout.splitlines() if "=" in line)
        self.assertEqual(completed.returncode, EXIT_ERROR, completed.stderr)
        self.assertEqual(evidence["refusal"], "verify")
        self.assertEqual(evidence["status"], "IVT")
        self.assertEqual(evidence["verifier_fault"], "0")
        self.assertEqual(evidence["state"], "FAILED")
        self.assertIn("cannot verify", completed.stderr)
        self.assertFalse(container.exists())

    def test_a_slot_tag_and_a_link_window_together_are_refused(self):
        """A slot tag names that slot's link window; naming another beside it
        is a contradiction the signer refuses as a usage error, before the
        native helper runs, and nothing is written."""
        image = sample_image(self.work / "image.bin")
        container = self.work / "contradiction.aiuo"
        code, _, stdout, stderr = run_signer(
            "sign",
            "--image", str(image),
            "--secret", str(COMMITTED_SECRET),
            "--target-id", TARGET_ID,
            "--build-revision", REVISION,
            "--build-utc", BUILD_UTC,
            "--slot", "a",
            "--link-window", "boot",
            "--output", str(container),
        )
        self.assertEqual(code, EXIT_ERROR, f"{stdout}\n{stderr}")
        self.assertIn("do not also pass --link-window", stderr)
        self.assertFalse(container.exists())
        # The helper enforces the same rule on its own argv.
        completed = subprocess.run(
            [str(native_cli()), "sign", str(image), str(COMMITTED_SECRET),
             TARGET_ID, REVISION, BUILD_UTC, str(container), "a", "boot"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(completed.returncode, EXIT_ERROR, completed.stderr)
        self.assertIn("do not also name a link window", completed.stderr)
        self.assertFalse(container.exists())

    def test_sign_refuses_to_overwrite_a_container(self):
        image, container, _ = self.sign_sample()
        before = container.read_bytes()
        code, _, _, stderr = run_signer(
            "sign",
            "--image", str(image),
            "--secret", str(COMMITTED_SECRET),
            "--target-id", TARGET_ID,
            "--build-revision", REVISION,
            "--build-utc", BUILD_UTC,
            "--output", str(container),
        )
        self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertEqual(container.read_bytes(), before)


class BridgePinTests(SignerTestCase):
    """Phase 4z: a canonical image must carry 57977135e472's bytes at payload
    offset 0x1B80 (Phase 4za removes the check)."""

    def sign(self, image, *extra):
        container = self.work / "pin.aiuo"
        code, evidence, stdout, stderr = run_signer(
            "sign", "--image", str(image), "--secret", str(COMMITTED_SECRET),
            "--target-id", TARGET_ID, "--build-revision", REVISION,
            "--build-utc", BUILD_UTC, "--output", str(container), *extra,
        )
        return code, container, stdout, stderr

    def test_the_tool_constant_is_the_reference_bytes(self):
        self.assertEqual(aius_ota_sign.BRIDGE_PIN_OFFSET, PIN_OFFSET)
        self.assertEqual(aius_ota_sign.BRIDGE_PIN_BYTES, PIN_BYTES)

    def test_a_payload_carrying_the_pin_signs(self):
        image = sample_image(self.work / "ok.bin")
        code, container, stdout, stderr = self.sign(image)
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        self.assertTrue(container.exists())

    def test_one_differing_byte_is_refused(self):
        for position in (PIN_OFFSET, PIN_OFFSET + 31):
            image = sample_image(self.work / f"bad{position}.bin")
            body = bytearray(image.read_bytes())
            body[position] ^= 0x01
            image.write_bytes(bytes(body))
            code, container, stdout, stderr = self.sign(image)
            self.assertEqual(code, EXIT_REFUSED, f"{stdout}\n{stderr}")
            self.assertIn("bridge pin mismatch", stderr)
            self.assertFalse(container.exists())

    def test_a_payload_too_short_for_the_pin_is_refused(self):
        image = sample_image(self.work / "short.bin", length=0x1B90)
        code, container, _, stderr = self.sign(image)
        self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertIn("bridge pin mismatch", stderr)

    def test_the_offset_is_payload_not_container(self):
        """The pin at CONTAINER offset 0x1B80 (header-shifted) is not the pin."""
        header_bytes = int(self.sign_header_bytes())
        image = sample_image(self.work / "shifted.bin")
        body = bytearray(image.read_bytes())
        body[PIN_OFFSET:PIN_OFFSET + 32] = b"\x00" * 32
        body[PIN_OFFSET - header_bytes:PIN_OFFSET - header_bytes + 32] = PIN_BYTES
        image.write_bytes(bytes(body))
        code, _, _, stderr = self.sign(image)
        self.assertEqual(code, EXIT_REFUSED, stderr)
        # And the signed container really does hold the pin at payload offset.
        good = sample_image(self.work / "good.bin")
        code, container, stdout, stderr = self.sign(good)
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        raw = container.read_bytes()
        self.assertEqual(raw[header_bytes + PIN_OFFSET:
                             header_bytes + PIN_OFFSET + 32], PIN_BYTES)
        self.assertNotEqual(raw[PIN_OFFSET:PIN_OFFSET + 32], PIN_BYTES)

    def sign_header_bytes(self):
        _, _, evidence = self.sign_sample(output_name="probe.aiuo")
        return evidence["header_bytes"]

    def test_the_plain_image_path_is_not_checked(self):
        image = sample_image(self.work / "plain.bin", link_base=BOOT_BASE)
        code, _, stdout, stderr = self.sign(image, "--link-window", "boot")
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")

    def test_the_ld_pin_words_equal_the_tool_constant(self):
        text = LD_SCRIPT.read_text()
        begin = text.find("/* aius-bridge-pin begin")
        end = text.find("/* aius-bridge-pin end */")
        if begin < 0 or end < 0:
            self.skipTest("cross-check pending ld pin")
        import re
        block = text[begin:end]
        words = re.findall(r"LONG\(\s*(0[xX][0-9a-fA-F]+)\s*\)", block)
        self.assertEqual(len(words), 8)
        decoded = b"".join(struct.pack("<I", int(w, 16)) for w in words)
        self.assertEqual(decoded, aius_ota_sign.BRIDGE_PIN_BYTES)


class VerifyTests(SignerTestCase):
    def verify(self, container, public=None, target_id=TARGET_ID,
               link_window=None):
        args = [
            "verify",
            "--container", str(container),
            "--public", str(public or COMMITTED_PUBLIC),
            "--target-id", target_id,
        ]
        if link_window:
            args += ["--link-window", link_window]
        return run_signer(*args)

    def test_a_signed_container_reaches_armed(self):
        image, container, sign_evidence = self.sign_sample()
        code, evidence, stdout, stderr = self.verify(container)
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        # ARMED is the whole claim: the header parsed, the target matched, the
        # payload digested to what the header promised, the signature over the
        # authentication digest checked out, and the IVT lies in the canonical
        # window the default posture judges against.
        self.assertEqual(evidence["state"], "ARMED")
        self.assertEqual(evidence["link_window"], "0x60010000+0x003d8000")
        self.assertEqual(evidence["target_id"], TARGET_ID)
        self.assertEqual(evidence["build_revision"], REVISION)
        self.assertEqual(evidence["build_utc"], BUILD_UTC)
        for field in ("payload_sha256", "auth_digest", "signature",
                      "public_key"):
            self.assertEqual(evidence[field], sign_evidence[field], field)
        self.assertEqual(evidence["payload_sha256"],
                         hashlib.sha256(image.read_bytes()).hexdigest())

    def test_another_signer_is_refused(self):
        _, container, _ = self.sign_sample()
        other_public = self.work / "other.pub"
        other_secret = self.work / "other.sec"
        run_signer("keygen", "--public", str(other_public),
                   "--secret", str(other_secret))

        code, evidence, _, stderr = self.verify(container, public=other_public)
        self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertEqual(evidence["refusal"], "verify")
        self.assertEqual(evidence["status"], "SIGNATURE")
        # 3 is AIUS_OTA_ED25519_FAULT_SIGNATURE, latched verbatim from the
        # adapter so an operator sees why rather than only that.
        self.assertEqual(evidence["verifier_fault"], "3")
        self.assertEqual(evidence["state"], "FAILED")

    def test_an_image_for_another_board_is_refused_before_staging(self):
        _, container, _ = self.sign_sample()
        code, evidence, _, stderr = self.verify(
            container, target_id="teensy41-something-else"
        )
        self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertEqual(evidence["refusal"], "offer")
        self.assertEqual(evidence["status"], "UNSUPPORTED")

    def test_one_flipped_payload_byte_fails_on_the_digest(self):
        _, container, evidence = self.sign_sample()
        header_bytes = int(evidence["header_bytes"])
        bytes_out = bytearray(container.read_bytes())
        bytes_out[header_bytes + 17] ^= 0x01
        tampered = self.work / "tampered.aiuo"
        tampered.write_bytes(bytes(bytes_out))

        code, evidence, _, stderr = self.verify(tampered)
        self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertEqual(evidence["refusal"], "verify")
        # DIGEST, not SIGNATURE: the staged bytes are checked against the
        # header's promise before the curve is consulted at all.
        self.assertEqual(evidence["status"], "DIGEST")
        self.assertEqual(evidence["verifier_fault"], "0")

    def test_one_flipped_header_byte_fails_the_container(self):
        _, container, evidence = self.sign_sample()
        header_bytes = int(evidence["header_bytes"])
        bytes_out = bytearray(container.read_bytes())
        bytes_out[header_bytes - 8] ^= 0x01
        tampered = self.work / "tampered-header.aiuo"
        tampered.write_bytes(bytes(bytes_out))

        code, evidence, _, stderr = self.verify(tampered)
        self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertEqual(evidence["refusal"], "header_parse")
        self.assertEqual(evidence["status"], "MALFORMED")

    def test_a_truncated_container_is_refused(self):
        _, container, evidence = self.sign_sample()
        truncated = self.work / "truncated.aiuo"
        truncated.write_bytes(container.read_bytes()[:-64])

        code, evidence, _, stderr = self.verify(truncated)
        self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertEqual(evidence["refusal"], "container_length")

    def test_a_plain_image_is_judged_by_posture(self):
        """Posture-aware: a plain single-image build (linked at the flash
        base) signs and verifies against the boot window, and the same
        container is refused at verify against the canonical window with the
        IVT reason -- the signature is fine, so no verifier fault -- leaving
        the device machine FAILED and nothing armed."""
        image = sample_image(self.work / "plain.bin", link_base=BOOT_BASE)
        container = self.work / "plain.aiuo"
        code, evidence, stdout, stderr = run_signer(
            "sign",
            "--image", str(image),
            "--secret", str(COMMITTED_SECRET),
            "--target-id", TARGET_ID,
            "--build-revision", REVISION,
            "--build-utc", BUILD_UTC,
            "--link-window", "boot",
            "--output", str(container),
        )
        self.assertEqual(code, EXIT_OK, f"sign failed:\n{stdout}\n{stderr}")

        code, evidence, stdout, stderr = self.verify(container)
        self.assertEqual(code, EXIT_REFUSED, f"{stdout}\n{stderr}")
        self.assertEqual(evidence["refusal"], "verify")
        self.assertEqual(evidence["status"], "IVT")
        self.assertEqual(evidence["verifier_fault"], "0")
        self.assertEqual(evidence["state"], "FAILED")
        self.assertEqual(evidence["link_window"], "0x60010000+0x003d8000")

        code, evidence, stdout, stderr = self.verify(container,
                                                     link_window="boot")
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        self.assertEqual(evidence["state"], "ARMED")
        self.assertEqual(evidence["link_window"], "0x60000000+0x007c0000")

    def test_a_tampered_ivt_fails_on_the_digest_first(self):
        """Ordering pinned: a flipped byte inside the signed container's IVT
        is a payload change, so it reads as DIGEST -- the bounds check judges
        only words the signature has already vouched for."""
        _, container, evidence = self.sign_sample()
        header_bytes = int(evidence["header_bytes"])
        bytes_out = bytearray(container.read_bytes())
        bytes_out[header_bytes + IVT_OFFSET + 4] ^= 0x80
        tampered = self.work / "tampered-ivt.aiuo"
        tampered.write_bytes(bytes(bytes_out))

        code, evidence, _, stderr = self.verify(tampered)
        self.assertEqual(code, EXIT_REFUSED, stderr)
        self.assertEqual(evidence["refusal"], "verify")
        self.assertEqual(evidence["status"], "DIGEST")
        self.assertEqual(evidence["verifier_fault"], "0")


if __name__ == "__main__":
    unittest.main(verbosity=2)
