"""Fakes for the CDC flash path: a scriptable transport and a HEX builder.

The transport models the three things that actually decide the flash contract --
whether an exchange answers, whether the port is present, and what the 134-baud
hatch does to that presence -- and nothing else. It is deliberately NOT derived
from SerialTransport: a fake written from the caller's beliefs can only ever
confirm the caller, which is the exact failure this firmware's network-transport
fake shipped for months (open defect 10). Presence here is a scripted timeline,
so a test states "the port goes away on the second look" as data rather than by
patching the code under test.
"""


class FakeTransport:
    """A CDC port whose answers and presence are scripted.

    `answers` maps a verb to either a response-line template (`{id}` is
    substituted) or None for "no answer at all". `presence` is a list of
    booleans consumed by successive present() calls; once exhausted the last
    value repeats forever.
    """

    def __init__(self, answers=None, presence=None, force_reboot_effect=None):
        self.answers = answers or {}
        self.presence = list(presence) if presence else [True]
        self.force_reboot_effect = force_reboot_effect
        self.exchanges = []
        self.force_reboots = 0
        self._presence_index = 0

    def present(self):
        if self._presence_index < len(self.presence):
            value = self.presence[self._presence_index]
            self._presence_index += 1
        else:
            value = self.presence[-1]
        return value

    def exchange(self, command, request_id):
        self.exchanges.append(command)
        verb = command.split(" ")[1]
        template = self.answers.get(verb)
        if template is None:
            return None
        return template.format(id=request_id)

    def force_reboot(self):
        self.force_reboots += 1
        if self.force_reboot_effect is not None:
            # Replace the remaining timeline: the hatch reboots the board, so
            # what the port does afterwards is a different sequence.
            self.presence = list(self.force_reboot_effect)
            self._presence_index = 0


class RecordingRunner:
    """subprocess.run stand-in that records argv and returns a scripted code."""

    class Completed:
        def __init__(self, returncode):
            self.returncode = returncode

    def __init__(self, returncode=0):
        self.returncode = returncode
        self.calls = []

    def __call__(self, argv, timeout=None):
        self.calls.append(argv)
        return self.Completed(self.returncode)


def hex_records(payload, address=0, per_record=16):
    """Emit Intel HEX data records for `payload` starting at `address`.

    `per_record` is a parameter so a test can force a stamped string to straddle
    a record boundary -- the case that makes naive record concatenation lose it.
    """
    lines = []
    for offset in range(0, len(payload), per_record):
        chunk = payload[offset:offset + per_record]
        here = address + offset
        record = bytes([len(chunk), (here >> 8) & 0xFF, here & 0xFF, 0x00]) + chunk
        checksum = (-sum(record)) & 0xFF
        lines.append(":" + (record + bytes([checksum])).hex().upper())
    return lines


def hex_file(payload, address=0, per_record=16):
    """A complete Intel HEX file for `payload`, terminated by an EOF record."""
    return "\n".join(hex_records(payload, address, per_record) + [":00000001FF"]) + "\n"
