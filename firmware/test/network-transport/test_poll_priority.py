"""Guard: FNET's poll timer must not preempt the audio path.

WHY A SOURCE GUARD. `stack_start()` is target-only and the thing being asserted
is an NVIC priority relationship between this firmware and two files in the
Teensy framework, so there is no host execution that could observe it. What can
be checked is that the relationship holds in the sources that produce the image.

THE ASSERTION THAT MATTERS IS NOT `224 > 208`. The Cortex-M7 on the i.MX RT1062
implements four priority bits, so the NVIC discards the low nibble of whatever
is written: 209 and 208 are the SAME level, and a fix that moved the poll timer
from 128 to 209 would read as correct arithmetic and change nothing at all. The
contract is therefore expressed in implemented levels, which is the form the
hardware actually enforces.

Exits 3 when the framework is not present, following the itcm-placement suite's
precedent: an absent prerequisite is not a failing contract.
"""

import os
import pathlib
import re
import sys


FW_ROOT = pathlib.Path(__file__).parents[2]
ADAPTER = FW_ROOT / "src" / "aius_network_teensy.cpp"

# The NVIC on this part implements the top four bits of the priority byte.
IMPLEMENTED_PRIORITY_BITS = 4


def implemented_level(priority):
    """The level the hardware actually enforces for a written priority byte."""
    return priority >> (8 - IMPLEMENTED_PRIORITY_BITS)


def framework_audio_stream():
    core = os.environ.get("PLATFORMIO_CORE_DIR",
                          os.path.expanduser("~/.platformio"))
    path = (pathlib.Path(core) / "packages" / "framework-arduinoteensy"
            / "cores" / "teensy4" / "AudioStream.cpp")
    if not path.is_file():
        print("framework AudioStream.cpp not found at %s; install the Teensy "
              "platform or set PLATFORMIO_CORE_DIR" % path, file=sys.stderr)
        raise SystemExit(3)
    return path


def audio_software_isr_priority():
    """Read the audio software interrupt's priority from the framework itself.

    Read rather than pinned: a framework bump that moved this value would
    silently invert the relationship this guard exists to hold, and a hard-coded
    208 here would keep passing while it did.
    """
    text = framework_audio_stream().read_text(encoding="utf-8", errors="replace")
    found = re.search(r"NVIC_SET_PRIORITY\s*\(\s*IRQ_SOFTWARE\s*,\s*(\d+)\s*\)",
                      text)
    if not found:
        print("no NVIC_SET_PRIORITY(IRQ_SOFTWARE, N) in AudioStream.cpp",
              file=sys.stderr)
        raise SystemExit(3)
    return int(found.group(1))


def adapter_text():
    return ADAPTER.read_text(encoding="utf-8")


def poll_priority(text):
    found = re.search(r"#define\s+AIUS_FNET_POLL_PRIORITY\s+(\d+)u?", text)
    assert found, "AIUS_FNET_POLL_PRIORITY is not defined in the adapter"
    return int(found.group(1))


def stack_start_body(text):
    start = text.index("bool stack_start()")
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise AssertionError("stack_start() has no closing brace")


def test_poll_is_a_weaker_implemented_level_than_the_audio_isr():
    audio = audio_software_isr_priority()
    poll = poll_priority(adapter_text())

    assert poll > audio, (
        "fnet_poll priority %d must be numerically greater (weaker) than the "
        "audio software ISR's %d" % (poll, audio))
    assert implemented_level(poll) > implemented_level(audio), (
        "fnet_poll at %d and the audio ISR at %d land on the SAME implemented "
        "NVIC level (%d): the M7 here implements %d priority bits, so this "
        "demotion does nothing on the hardware"
        % (poll, audio, implemented_level(poll), IMPLEMENTED_PRIORITY_BITS))
    return audio, poll


def test_the_priority_is_applied_where_the_timer_is_armed():
    """It must sit in stack_start(), not in setup().

    IntervalTimer::beginCycles() re-applies the instance's own priority on every
    begin(), and beginNoWait() runs once per network generation -- so an
    override applied once at boot is undone by the first network restart. The
    guard is that the write lives beside the call that arms the timer.
    """
    body = stack_start_body(adapter_text())
    assert "beginNoWait" in body, "stack_start() no longer arms the stack"
    assert re.search(r"NVIC_SET_PRIORITY\s*\(\s*IRQ_PIT\s*,\s*"
                     r"AIUS_FNET_POLL_PRIORITY\s*\)", body), (
        "stack_start() must set IRQ_PIT to AIUS_FNET_POLL_PRIORITY")


def test_guard_sensitivity():
    """Each check must fail on the defect it exists to catch."""
    audio = audio_software_isr_priority()

    # The original defect: IntervalTimer's default, which outranks the DSP.
    assert not implemented_level(128) > implemented_level(audio), (
        "sensitivity: the pre-fix default 128 must not satisfy the ordering")

    # The subtle non-fix: numerically greater, same implemented level.
    same_level = audio + 1
    assert same_level > audio
    assert not implemented_level(same_level) > implemented_level(audio), (
        "sensitivity: a same-level demotion must not satisfy the ordering")

    # A body with no priority write must be rejected.
    try:
        stack_start_body("bool stack_start()\n{\n\treturn begin();\n}\n")
    except AssertionError:
        raise AssertionError("sensitivity fixture should parse")
    assert not re.search(r"NVIC_SET_PRIORITY", "bool stack_start()\n{\n}\n")


if __name__ == "__main__":
    audio, poll = test_poll_is_a_weaker_implemented_level_than_the_audio_isr()
    test_the_priority_is_applied_where_the_timer_is_armed()
    test_guard_sensitivity()
    print("fnet poll priority: PASS (audio ISR %d = level %d, fnet_poll %d = "
          "level %d, applied in stack_start; 3 sensitivity checks)"
          % (audio, implemented_level(audio), poll, implemented_level(poll)))
