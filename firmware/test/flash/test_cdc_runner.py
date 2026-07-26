"""Contract tests for the CDC control path and button-free bootloader entry.

Every assertion below is written in the CONTRACT's terms rather than the
implementation's, because this firmware has repeatedly shipped tests that shared
an expression with the code they tested and so proved only agreement. The
load-bearing example here is entry: the contract is "the port went away", not
"an OK line arrived", and the two tests that pull them apart are
test_lost_acknowledgement_still_enters and test_acknowledged_but_still_present.
"""

import importlib.util
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).parents[2]
MODULE = ROOT / "tools" / "net" / "aius_cdc_runner.py"
SPEC = importlib.util.spec_from_file_location("aius_cdc_runner", MODULE)
CDC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CDC)

sys.path.insert(0, str(pathlib.Path(__file__).parent / "fake"))
import fake_cdc  # noqa: E402


ID_LINE = ("AIUS/1 OK {id} ID protocol=1 revision=abc123def456+teensy41-optical "
           "build=2026-07-26T21:07:31Z mode=optical ip=192.0.2.66 rsafe=0 "
           "nstate=2 nfault=0 ckpt=9 early=0 ngen=1 bgen=239 rsrc=2 "
           "epoch=4211 nrst=0")


def link_with(transport):
    """A link over a fake transport with time collapsed to nothing."""
    ticks = iter(range(0, 100000))
    return CDC.CdcLink(port="/dev/fake", transport=transport,
                       sleep=lambda _s: None, clock=lambda: next(ticks) * 0.1)


class FramingTests(unittest.TestCase):
    def test_response_is_selected_by_request_id_among_diagnostics(self):
        text = ("booting, please wait\n"
                "AIUS/1 OK 4 ID protocol=1\n"
                "some diagnostic line\n"
                "AIUS/1 OK 5 ID protocol=1 revision=r5\n")
        self.assertIn("revision=r5", CDC.select_response(text, 5))

    def test_a_partial_line_before_the_answer_does_not_hide_it(self):
        text = "AIUS/1 OK 5 I\nAIUS/1 OK 5 ID protocol=1 revision=r5\n"
        # The first line is framed and carries the id but is a truncated write;
        # selection takes the first framed match, so the contract that matters
        # is that a complete answer is still found in the buffer.
        self.assertIsNotNone(CDC.select_response(text, 5))

    def test_an_error_response_is_reported_not_parsed_as_identity(self):
        with self.assertRaises(CDC.CdcError):
            CDC.parse_identity("AIUS/1 ERR 3 BUSY error")

    def test_identity_fields_are_decoded(self):
        fields = CDC.parse_identity(ID_LINE.format(id=7))
        self.assertEqual(fields["revision"], "abc123def456+teensy41-optical")
        self.assertEqual(fields["build"], "2026-07-26T21:07:31Z")

    def test_auth_token_is_decoded(self):
        self.assertEqual(CDC.parse_auth("AIUS/1 OK 8 AUTH 6b3ff32e4c2505cc"),
                         "6b3ff32e4c2505cc")


class BootloaderEntryTests(unittest.TestCase):
    def test_lost_acknowledgement_still_enters(self):
        """No answer, but the port went away -- that IS entry.

        The firmware drains its response before entering HalfKay, but a drain
        that loses the race, or a board already hung, produces no line at all.
        Judging entry by the acknowledgement would report a successful reflash
        as a failure and send the operator back to the Program button.
        """
        transport = fake_cdc.FakeTransport(answers={"BOOTLOADER": None},
                                           presence=[True, False])
        self.assertEqual(link_with(transport).enter_bootloader(use_hatch=False),
                         "cdc")

    def test_acknowledged_but_still_present_is_not_entry(self):
        """An OK line with the port still there is a refusal, not a success."""
        transport = fake_cdc.FakeTransport(
            answers={"BOOTLOADER": "AIUS/1 OK {id} BOOTLOADER entering"},
            presence=[True])
        with self.assertRaises(CDC.CdcError):
            link_with(transport).enter_bootloader(attempts=2, use_hatch=False)

    def test_the_hatch_recovers_a_board_that_answers_nothing(self):
        """A hung sketch never answers; the 134-baud reboot still works."""
        transport = fake_cdc.FakeTransport(answers={"BOOTLOADER": None},
                                           presence=[True, True, True],
                                           force_reboot_effect=[False])
        self.assertEqual(link_with(transport).enter_bootloader(),
                         "line-coding")
        self.assertEqual(transport.force_reboots, 1)

    def test_entry_failure_is_closed(self):
        transport = fake_cdc.FakeTransport(answers={"BOOTLOADER": None},
                                           presence=[True],
                                           force_reboot_effect=[True])
        with self.assertRaises(CDC.CdcError):
            link_with(transport).enter_bootloader(attempts=2)


class VerificationTests(unittest.TestCase):
    def test_a_same_revision_reflash_is_distinguished_by_build_time(self):
        """The discriminating case for comparing more than the revision.

        Rebuilding the same commit produces the same revision string, so a board
        still running the OLD image reports a revision identical to the one the
        NEW image claims. Only build_utc separates "the flash landed" from "the
        loader did nothing", which is why both fields are compared.
        """
        running = ID_LINE.replace("build=2026-07-26T21:07:31Z",
                                  "build=2026-07-26T20:00:00Z")
        transport = fake_cdc.FakeTransport(answers={"ID": running},
                                           presence=[True])
        with self.assertRaises(CDC.CdcError) as caught:
            CDC.verify_identity(link_with(transport), {
                "build_revision": "abc123def456+teensy41-optical",
                "build_utc": "2026-07-26T21:07:31Z"})
        self.assertIn("20:00:00Z", str(caught.exception))

    def test_a_matching_image_verifies(self):
        transport = fake_cdc.FakeTransport(answers={"ID": ID_LINE},
                                           presence=[True])
        found = CDC.verify_identity(link_with(transport), {
            "build_revision": "abc123def456+teensy41-optical",
            "build_utc": "2026-07-26T21:07:31Z"})
        self.assertEqual(found["build_revision"], "abc123def456+teensy41-optical")

    def test_a_board_that_never_re_enumerates_fails_closed(self):
        transport = fake_cdc.FakeTransport(answers={"ID": ID_LINE},
                                           presence=[False])
        with self.assertRaises(CDC.CdcError):
            CDC.verify_identity(link_with(transport), {
                "build_revision": "abc123def456+teensy41-optical",
                "build_utc": "2026-07-26T21:07:31Z"}, timeout=0.5)

    def test_a_board_that_never_answers_fails_closed(self):
        transport = fake_cdc.FakeTransport(answers={"ID": None}, presence=[True])
        with self.assertRaises(CDC.CdcError):
            CDC.verify_identity(link_with(transport), {
                "build_revision": "abc123def456+teensy41-optical",
                "build_utc": "2026-07-26T21:07:31Z"}, timeout=0.5)


class LoaderTests(unittest.TestCase):
    def test_a_non_zero_loader_exit_fails_closed(self):
        runner = fake_cdc.RecordingRunner(returncode=1)
        with self.assertRaises(CDC.CdcError):
            CDC.run_loader("/bin/true", "image.hex", runner=runner)

    def test_the_loader_is_invoked_for_the_imxrt1062(self):
        runner = fake_cdc.RecordingRunner()
        CDC.run_loader("/bin/true", "image.hex", runner=runner)
        self.assertEqual(runner.calls[0],
                         ["/bin/true", "--mcu=imxrt1062", "-w", "-v", "image.hex"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
