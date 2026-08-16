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
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SIGNER = ROOT / "tools" / "ota" / "aius_ota_sign.py"
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


def sample_image(path, length=5096, seed=0):
    path.write_bytes(bytes((index * 31 + seed) % 256 for index in range(length)))
    return path


class SignerTestCase(unittest.TestCase):
    def setUp(self):
        self._work = tempfile.TemporaryDirectory()
        self.work = pathlib.Path(self._work.name)
        self.addCleanup(self._work.cleanup)

    def sign_sample(self, output_name="image.aiuo", length=5096, seed=0,
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

        lines = [ela(0x6001)]
        lines.append(record(0x0000, payload[:16]))
        lines.append(record(0x0010, payload[16:32]))
        # A 4-byte alignment gap, as the real slot images carry.
        lines.append(record(0x0024, payload[32:48]))
        lines.append(":00000001FF")
        image = self.work / "image.hex"
        image.write_text("\n".join(lines) + "\n")
        expected = payload[:32] + b"\xff" * 4 + payload[32:48]

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


class VerifyTests(SignerTestCase):
    def verify(self, container, public=None, target_id=TARGET_ID):
        return run_signer(
            "verify",
            "--container", str(container),
            "--public", str(public or COMMITTED_PUBLIC),
            "--target-id", target_id,
        )

    def test_a_signed_container_reaches_armed(self):
        image, container, sign_evidence = self.sign_sample()
        code, evidence, stdout, stderr = self.verify(container)
        self.assertEqual(code, EXIT_OK, f"{stdout}\n{stderr}")
        # ARMED is the whole claim: the header parsed, the target matched, the
        # payload digested to what the header promised, and the signature over
        # the authentication digest checked out.
        self.assertEqual(evidence["state"], "ARMED")
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
