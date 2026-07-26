"""Source-blind oracle for the image-identity reader and the flash sequence.

This suite is written against the CONTRACT the flash tool must satisfy, not
against how it satisfies it: an image states its own identity, the tool refuses
to touch the board when it cannot read that identity, and the board must come
back claiming exactly it. The Intel HEX cases are the sharp ones -- a stamped
string straddling a record boundary is the case that a naive "concatenate every
record's data" reader loses, and it is the case a real 385 KB image presents
constantly.
"""

import importlib.util
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
MODULE = ROOT / "tools" / "net" / "aius_cdc_runner.py"
SPEC = importlib.util.spec_from_file_location("aius_cdc_runner", MODULE)
CDC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CDC)

sys.path.insert(0, str(pathlib.Path(__file__).parent / "fake"))
import fake_cdc  # noqa: E402


REVISION = b"abc123def456+teensy41-optical\x00"
BUILD_UTC = b"2026-07-26T21:07:31Z\x00"


def write_hex(payload, per_record=16):
    handle = tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False)
    handle.write(fake_cdc.hex_file(payload, per_record=per_record))
    handle.close()
    return handle.name


class ImageIdentityTests(unittest.TestCase):
    def test_identity_is_read_from_the_image(self):
        path = write_hex(b"\xff" * 8 + REVISION + b"\xff" * 8 + BUILD_UTC)
        found = CDC.image_identity(path)
        self.assertEqual(found["build_revision"], "abc123def456+teensy41-optical")
        self.assertEqual(found["build_utc"], "2026-07-26T21:07:31Z")

    def test_a_revision_straddling_record_boundaries_is_still_found(self):
        """Four-byte records guarantee the 29-byte revision spans eight of them.

        This is the case that separates address-reassembly from concatenation.
        Concatenation happens to work when records are emitted in address order,
        so the assertion that matters is the one below it: out-of-order records
        must not fuse two unrelated runs into a false match.
        """
        path = write_hex(b"\xff" * 3 + REVISION + b"\xff" * 3 + BUILD_UTC,
                         per_record=4)
        found = CDC.image_identity(path)
        self.assertEqual(found["build_revision"], "abc123def456+teensy41-optical")

    def test_discontiguous_records_do_not_fuse_into_a_false_identity(self):
        """Two halves of a revision at unrelated addresses are not an identity.

        Reassembly is by absolute address precisely so that a gap stays a gap.
        A reader that concatenated record payloads would splice these into a
        well-formed revision string that exists nowhere in the image.
        """
        first = fake_cdc.hex_records(b"abc123def456", address=0x0000)
        second = fake_cdc.hex_records(b"+teensy41-optical\x00" + BUILD_UTC,
                                      address=0x4000)
        handle = tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False)
        handle.write("\n".join(first + second + [":00000001FF"]) + "\n")
        handle.close()
        with self.assertRaises(CDC.CdcError):
            CDC.image_identity(handle.name)

    def test_an_image_with_no_identity_is_refused(self):
        path = write_hex(b"\x00" * 64)
        with self.assertRaises(CDC.CdcError):
            CDC.image_identity(path)

    def test_an_image_with_a_revision_but_no_timestamp_is_refused(self):
        path = write_hex(b"\xff" * 8 + REVISION)
        with self.assertRaises(CDC.CdcError):
            CDC.image_identity(path)

    def test_a_corrupt_checksum_is_refused(self):
        good = fake_cdc.hex_file(REVISION + BUILD_UTC)
        lines = good.splitlines()
        # Flip the final checksum nibble of the first data record.
        lines[0] = lines[0][:-1] + ("0" if lines[0][-1] != "0" else "1")
        handle = tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False)
        handle.write("\n".join(lines) + "\n")
        handle.close()
        with self.assertRaises(CDC.CdcError):
            CDC.image_identity(handle.name)

    def test_a_non_hex_file_is_refused(self):
        handle = tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False)
        handle.write("this is not an image\n")
        handle.close()
        with self.assertRaises(CDC.CdcError):
            CDC.image_identity(handle.name)


class FlashSequenceTests(unittest.TestCase):
    def setUp(self):
        self.path = write_hex(b"\xff" * 8 + REVISION + b"\xff" * 8 + BUILD_UTC)
        self.id_line = ("AIUS/1 OK {id} ID protocol=1 "
                        "revision=abc123def456+teensy41-optical "
                        "build=2026-07-26T21:07:31Z mode=optical")

    def link(self, transport):
        ticks = iter(range(0, 100000))
        return CDC.CdcLink(port="/dev/fake", transport=transport,
                           sleep=lambda _s: None,
                           clock=lambda: next(ticks) * 0.1)

    def test_an_unreadable_image_never_reaches_the_loader(self):
        """Fail closed BEFORE the board is touched.

        An image whose identity cannot be read cannot be verified afterwards, so
        writing it would leave the board in a state the tool could not report
        on. The assertion is that the loader was never invoked at all.
        """
        empty = write_hex(b"\x00" * 64)
        runner = fake_cdc.RecordingRunner()
        transport = fake_cdc.FakeTransport(answers={"ID": self.id_line},
                                           presence=[True, False])
        with self.assertRaises(CDC.CdcError):
            CDC.flash(empty, self.link(transport), "/bin/true", runner=runner,
                      log=lambda _m: None)
        self.assertEqual(runner.calls, [])

    def test_a_failing_loader_is_not_reported_as_a_flash(self):
        runner = fake_cdc.RecordingRunner(returncode=1)
        transport = fake_cdc.FakeTransport(answers={"ID": self.id_line},
                                           presence=[True, False])
        with self.assertRaises(CDC.CdcError):
            CDC.flash(self.path, self.link(transport), "/bin/true",
                      runner=runner, log=lambda _m: None)

    def test_a_board_that_will_not_answer_before_flashing_is_still_flashed(self):
        """A crash-looping board is exactly the one worth reflashing.

        The pre-flash identity read is diagnostic, so its failure must not abort
        the cycle -- a board that cannot answer AIUS ID is the case the whole
        tool exists for.
        """
        runner = fake_cdc.RecordingRunner()
        transport = fake_cdc.FakeTransport(
            answers={"ID": None, "BOOTLOADER": None},
            presence=[True, False],
            force_reboot_effect=[False])
        # ID answers nothing before the flash; after it, verification needs a
        # real answer, so swap the script once the loader has run.
        original = runner.__call__

        def run_then_answer(argv, timeout=None):
            transport.answers["ID"] = self.id_line
            transport.presence = [True]
            transport._presence_index = 0
            return original(argv, timeout=timeout)

        CDC.flash(self.path, self.link(transport), "/bin/true",
                  runner=run_then_answer, log=lambda _m: None)
        self.assertEqual(len(runner.calls), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
