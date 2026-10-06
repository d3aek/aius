#!/usr/bin/env python3
"""Structural regression suite for the Phase 14 optical runtime cutover.

Reads and analyses `src/optical_main.cpp` as text; it never compiles or runs
the firmware (see the network-transport suite's `test_vendor_source.py` for
the sibling idiom this file follows: exact-set/exact-count assertions over
stripped source text, self-tested against synthetic input).

Two independently runnable groups, selected with `--group`:

  --group positive   properties the post-cutover file must HAVE:
                      setup() is construction-only, loop() delegates exactly
                      one runtime epoch, the watchdog is armed once at the
                      pinned 8 s interval and fed exactly once per epoch, all
                      three status channels (UDP, status JSON, USB ID) carry
                      the recovery state, and the replay refill path performs
                      no direct SD primitive.
  --group absence     constructs the post-cutover file must NOT contain: a
                      static-address fallback, raw FNET access, the
                      listener-rebind globals, a blocking socket write loop,
                      or a watchdog feed outside the single per-epoch one.
  --group all         both groups (default).

The split matters: slice S2a lands the bounded runtime while the legacy path
is still present-but-unreachable and gates on `--group positive` only; the
absence group cannot pass until slice S2b deletes the legacy path in the same
commit that wires this suite into the standing guard (`scripts/test.sh`).

Both groups are expected to FAIL against the pre-cutover file -- that is
this slice's acceptance criterion, not a bug in the suite.
"""

import argparse
import re
import sys
from pathlib import Path

FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
OPTICAL_MAIN = FIRMWARE_ROOT / "src" / "optical_main.cpp"
DSP_RUNTIME_CONFIG = (
    FIRMWARE_ROOT
    / "lib"
    / "aius_dsp_runtime_config"
    / "src"
    / "aius_dsp_runtime_config.c"
)

# The replay refill's sizing is spread across three libraries that NOTHING
# compiles together: the portable SD engine caps a job's transfer slice, the
# replay seam sizes its ring and refill against that slice, and optical_main.cpp
# declares the buffer whose extent is the slice. Each builds cleanly on its own
# while disagreeing with the other two, and the disagreement is only observable
# on the card -- which is exactly how a replay that produced NOTHING shipped
# past five green suites and a linking image. These are the files that check
# reads.
REPLAY_SIZING_SOURCES = (
    Path("src") / "optical_main.cpp",
    Path("lib") / "aius_sd" / "src" / "aius_sd.c",
    Path("lib") / "aius_replay" / "include" / "aius_replay.h",
    Path("src") / "aius_runtime_storage.h",
)

SETUP_MARKER = "void setup(void)"
LOOP_MARKER = "void loop(void)"


# --------------------------------------------------------------------------
# Shared text helpers. Mirrors test/network-transport/test_vendor_source.py's
# comment/literal stripping and brace-matched body extraction so this suite
# reads the same way to anyone who has read that one.
# --------------------------------------------------------------------------

def without_comments_and_literals(text):
    pattern = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )
    return pattern.sub(lambda match: " " * len(match.group(0)), text)


def without_comments(text):
    """Comments blanked, string and character literals KEPT. The status-channel
    checks assert over format literals, so they cannot use the stripping helper
    above; they still must not be satisfiable by a prose comment that merely
    names a field. Literal-aware by construction: a literal is matched first and
    returned unchanged, so a `//` inside one is never mistaken for a comment."""
    pattern = re.compile(
        r"(//[^\n]*|/\*.*?\*/)|(\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*')",
        re.DOTALL,
    )

    def replace(match):
        if match.group(1) is not None:
            return " " * len(match.group(0))
        return match.group(0)

    return pattern.sub(replace, text)


def matching_delimiter(text, opening, open_char, close_char):
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == open_char:
            depth += 1
        elif text[index] == close_char:
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError(f"unterminated {open_char!r} ... {close_char!r}")


def function_body(text, marker):
    """The brace-matched body of the function whose signature is `marker`.
    `marker` must appear exactly once and be immediately followed (modulo
    whitespace) by the function's opening brace."""
    start = text.find(marker)
    assert start >= 0, f"missing function marker {marker!r}"
    assert text.find(marker, start + 1) == -1, (
        f"marker {marker!r} is not unique in the file"
    )
    opening = text.find("{", start + len(marker))
    assert opening >= 0, f"missing body for {marker!r}"
    clean = without_comments_and_literals(text)
    closing = matching_delimiter(clean, opening, "{", "}")
    return text[opening + 1: closing]


def conditional_body(text, marker):
    """The brace-matched body of one exact `if (...)` marker."""
    clean = without_comments_and_literals(text)
    start = clean.find(marker)
    assert start >= 0, f"missing conditional marker {marker!r}"
    assert clean.find(marker, start + 1) == -1, (
        f"conditional marker {marker!r} is not unique"
    )
    opening = clean.find("{", start + len(marker))
    assert opening >= 0, f"missing body for conditional {marker!r}"
    assert not clean[start + len(marker):opening].strip(), (
        f"unexpected tokens before body for conditional {marker!r}"
    )
    closing = matching_delimiter(clean, opening, "{", "}")
    return text[opening + 1:closing]


def loop_bodies(text):
    """Every `for (...)` / `while (...)` loop body found anywhere in `text`,
    brace-delimited when the loop has a compound body, single-statement
    (up to the next top-level `;`) otherwise. File-wide, not scoped to one
    function -- the absence checks must see every loop in the file."""
    clean = without_comments_and_literals(text)
    bodies = []
    for match in re.finditer(r"\b(?:for|while)\s*\(", clean):
        opening = clean.find("(", match.start())
        closing = matching_delimiter(clean, opening, "(", ")")
        cursor = closing + 1
        while cursor < len(clean) and clean[cursor] in " \t\r\n":
            cursor += 1
        if cursor < len(clean) and clean[cursor] == "{":
            body_end = matching_delimiter(clean, cursor, "{", "}")
            bodies.append(text[cursor + 1: body_end])
        else:
            end = clean.find(";", cursor)
            if end == -1:
                continue
            bodies.append(text[cursor:end + 1])
    return bodies


def loop_headers(text):
    """Every `for (...)` / `while (...)` CONDITION found in `text`, as the raw
    parenthesised string. The counterpart of `loop_bodies` above: a bound lives
    in the header, not in the body, so a check that a loop terminates on a
    constant has to read the header. Comment/literal stripping is the caller's
    job, because the caller already scopes the text to one function body."""
    headers = []
    for match in re.finditer(r"\b(?:for|while)\s*\(", text):
        opening = text.find("(", match.start())
        closing = matching_delimiter(text, opening, "(", ")")
        headers.append(text[opening + 1: closing])
    return headers


def function_definitions(text):
    """Every file-scope C-style function DEFINITION in `text`, as
    name -> brace-matched body.

    A definition is a line-initial declarator whose closing `)` is followed
    (modulo whitespace) by `{` -- which is what separates it from a prototype, a
    call, or a macro. Comments and literals are blanked for the scan and the
    brace match; the returned body is the original text. Qualified C++ member
    definitions (`Class::member`) are deliberately not matched: nothing this
    suite scopes to is one, and admitting `::` would also admit statements."""
    clean = without_comments_and_literals(text)
    definitions = {}
    for match in re.finditer(
        r"^[A-Za-z_][A-Za-z0-9_ \t*&]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(",
        clean,
        re.MULTILINE,
    ):
        opening = clean.find("(", match.start(1))
        closing = matching_delimiter(clean, opening, "(", ")")
        cursor = closing + 1
        while cursor < len(clean) and clean[cursor] in " \t\r\n":
            cursor += 1
        if cursor >= len(clean) or clean[cursor] != "{":
            continue
        body_end = matching_delimiter(clean, cursor, "{", "}")
        definitions[match.group(1)] = text[cursor + 1: body_end]
    return definitions


def call_count(text, callee):
    """Count of `callee(` outside comments/string literals."""
    return without_comments_and_literals(text).count(callee + "(")


def qualified_call_sites(text, qualified):
    """Indices of every `Class::member(` in `text` that is a CALL rather than
    that member's own out-of-line definition. Definitions are told apart the
    same way function_definitions() tells them from prototypes: the closing `)`
    is followed, modulo whitespace, by `{`. Indices are into `text` itself --
    without_comments_and_literals() blanks with equal-length runs, so the
    stripped and unstripped strings share an offset space."""
    clean = without_comments_and_literals(text)
    needle = qualified + "("
    sites = []
    cursor = 0
    while True:
        index = clean.find(needle, cursor)
        if index == -1:
            return sites
        cursor = index + len(needle)
        closing = matching_delimiter(clean, index + len(qualified), "(", ")")
        after = closing + 1
        while after < len(clean) and clean[after] in " \t\r\n":
            after += 1
        if after < len(clean) and clean[after] == "{":
            continue
        sites.append(index)


def identifier_present(text, name):
    return re.search(
        rf"\b{re.escape(name)}\b", without_comments_and_literals(text)
    ) is not None


# --------------------------------------------------------------------------
# Positive group -- properties the post-cutover file must HAVE.
# --------------------------------------------------------------------------

FORBIDDEN_IN_SETUP = (
    "SD.begin(",
    "Ethernet.begin(",
    "start_network(",
    "load_sd_config(",
    "stage_dsp_sd_override(",
    "activate_network(",
    "g_http_server.begin(",
    "g_udp.begin(",
    "ensure_dhcp_client(",
    "Serial.available(",
)


def assert_setup_is_construction_only(text):
    body = without_comments_and_literals(function_body(text, SETUP_MARKER))
    assert "aius_runtime_teensy_construct(" in body, (
        "setup() must construct the runtime via aius_runtime_teensy_construct()"
    )
    for forbidden in FORBIDDEN_IN_SETUP:
        assert forbidden not in body, (
            f"setup() is not construction-only: found {forbidden!r} (probes "
            "SD, waits for link/DHCP/USB, or starts a peer transaction)"
        )
    loop_match = re.search(r"\b(?:for|while)\s*\(", body)
    assert loop_match is None, (
        "setup() must contain no loop that waits on an external condition"
    )


FORBIDDEN_IN_LOOP = (
    "service_http(",
    "service_network(",
    "service_sd(",
    "service_usb_control(",
    "supervise_network_link(",
    "Ethernet.maintain(",
    "aius_replay_service_refill(",
    "watchdog_feed(",
)


def assert_loop_delegates_one_epoch(text):
    body = without_comments_and_literals(function_body(text, LOOP_MARKER))
    count = body.count("aius_runtime_teensy_epoch(")
    assert count == 1, (
        "loop() must call aius_runtime_teensy_epoch() exactly once per "
        f"iteration, found {count}"
    )
    for forbidden in FORBIDDEN_IN_LOOP:
        assert forbidden not in body, (
            f"loop() calls {forbidden!r} directly instead of delegating the "
            "whole epoch to aius_runtime_teensy_epoch()"
        )


REBOOT_GATE_FUNCTION = "service_reboot_request"
REBOOT_DRAIN_PREDICATE = "reboot_drain_complete"


def assert_requested_reset_waits_for_the_drained_transaction(text):
    """A requested reset waits for its own acknowledgement to reach the wire.

    The retired shape called `reset_application()` straight from `loop()` under
    `if (g_reboot_requested)`, i.e. at the end of the epoch that dispatched. That
    epoch has only retained a pending application response: `format_response()`
    runs in the next transaction step, so not one byte of the 200 exists yet, and
    the first <=512-byte socket write is later still. Measured on the board, the
    board reset and the client always timed out.

    The gate is the transaction reaching `AIUS_HTTP_TX_IDLE`, which is reached
    only through `restart_idle()` and on the response path only after the last
    write was accepted and the close completed -- a real drain signal, not a
    buffered one. It is bounded, because a wedged or vanished peer must not be
    able to veto a reset that was asked for."""
    definitions = function_definitions(text)
    loop_body = without_comments_and_literals(function_body(text, LOOP_MARKER))
    assert "g_reboot_requested" in loop_body, (
        "loop() must remain the reader of the requested-reset flag"
    )
    assert "reset_application(" not in loop_body, (
        "loop() calls reset_application() directly, so the reset lands at the "
        "end of the epoch that dispatched -- before the response has been "
        "formatted, let alone written -- and POST /api/reboot resets the board "
        "and never answers it"
    )
    assert f"{REBOOT_GATE_FUNCTION}(" in loop_body, (
        f"loop() must reach the reset through {REBOOT_GATE_FUNCTION}()"
    )
    for name in (REBOOT_GATE_FUNCTION, REBOOT_DRAIN_PREDICATE):
        assert name in definitions, f"missing {name}() definition"
    gate = without_comments_and_literals(definitions[REBOOT_GATE_FUNCTION])
    drain = without_comments_and_literals(definitions[REBOOT_DRAIN_PREDICATE])
    assert f"{REBOOT_DRAIN_PREDICATE}(" in gate, (
        f"{REBOOT_GATE_FUNCTION}() must gate the reset on "
        f"{REBOOT_DRAIN_PREDICATE}()"
    )
    assert "AIUS_HTTP_TX_IDLE" in drain, (
        f"{REBOOT_DRAIN_PREDICATE}() must read the transaction's drained "
        "terminal state, AIUS_HTTP_TX_IDLE -- a fixed epoch delay would be "
        "fragile against WOULD_BLOCK retries"
    )
    assert "AIUS_REBOOT_DRAIN_MS" in gate, (
        f"{REBOOT_GATE_FUNCTION}() must bound the wait, so a wedged or vanished "
        "peer cannot prevent a requested reset"
    )
    occurrences = call_count(text, "reset_application")
    assert occurrences == 4, (
        "expected exactly four reset_application() occurrences -- its "
        "definition and the THREE gated call sites -- found "
        f"{occurrences}. The invariant is deliberately three gated resets now, "
        "not one: the reboot drain gate above; OTA activation, whose "
        "RAM-resident run-to-completion finalizer resets into the new image "
        "only after the sequencer reaches READY_TO_RESET "
        "(ota_activate_run_and_finalize); and the OTA REHEARSAL, which ends in "
        "the same deliberate reset once its verdict is recorded, because its "
        "quiesce already stopped the audio service one-way and the epoch can "
        "no longer feed the watchdog (assert_rehearsal_ends_in_a_deliberate_"
        "reset pins that one)"
    )
    assert "reset_application(" in gate, (
        "the reboot reset must stay the drained-transaction gated call site"
    )
    finalizer = "ota_activate_run_and_finalize"
    assert finalizer in definitions, (
        f"missing {finalizer}() -- the activation reset must live in the "
        "RAM-resident finalizer, not loop(), because after the head is "
        "programmed the old image's flash is gone and a FLASHMEM reset would "
        "execute new-image bytes"
    )
    assert "reset_application(" in without_comments_and_literals(
        definitions[finalizer]
    ), (
        f"{finalizer}() must be the activation reset site, reached only after "
        "the sequencer reaches READY_TO_RESET"
    )


OTA_QUIESCE_FUNCTION = "ota_quiesce"
OTA_ACTIVATION_SERVICE = "service_ota_activation"
OTA_RESIDENT_RUN = "ota_activate_run_and_finalize"
# The quiesce steps, in the order that leaves no master touching flash during the
# erase window (plan §Item 4: gate closed, audio service stopped, ISR parked).
# The order is load-bearing -- the carrier must go dark before the audio path is
# torn down, and the audio service must stop before the source ISR is parked --
# and no native suite compiles optical_main.cpp, so this is a text property.
OTA_QUIESCE_STEPS = (
    "tx_gate_actuate",        # 1. transmit gate closed (pad 14 low)
    "g_audio_service_ready",  # 2. epoch-driven audio service stopped
    "NVIC_DISABLE_IRQ",       # 3. audio update ISR source parked
    "ota_park_interrupt_sources",  # 4. EVERY remaining source parked (2026-08-01)
)

OTA_PARK_FUNCTION = "ota_park_interrupt_sources"
OTA_RESTORE_FUNCTION = "ota_restore_interrupt_sources"
# Every interrupt SOURCE parked, in order: PRIMASK first so nothing sneaks in
# mid-park, then the whole NVIC bank, then SysTick's TICKINT, then the pended
# system exceptions. The 2026-08-01 incident's mechanism: the core driver's
# flash_wait() ends every erase/program with an unconditional __enable_irq(),
# so PRIMASK alone leaves a taken-interrupt gap inside every primitive -- an
# ISR reading flash-resident .rodata from the currently-erased sector faults
# into the core handler's SYSRESETREQ and the board lands in ROM SDP. With no
# source enabled, the driver's enable takes nothing. The TICKINT and PENDSV
# tokens are the distinctive ones (the save block reads SYST_CSR earlier, so
# the bare register name would false-order).
OTA_PARK_STEPS = (
    "__disable_irq",
    "NVIC_ICER0",
    "NVIC_ICER1",
    "NVIC_ICER2",
    "NVIC_ICER3",
    "NVIC_ICER4",
    "~SYST_CSR_TICKINT",
    "SCB_ICSR_PENDSVCLR",
)
# Restore is membership, not order: re-enable exactly the saved set, then
# SysTick, then PRIMASK.
OTA_RESTORE_TOKENS = (
    "NVIC_ISER0",
    "NVIC_ISER1",
    "NVIC_ISER2",
    "NVIC_ISER3",
    "NVIC_ISER4",
    "SYST_CSR",
    "__enable_irq",
)


def assert_dark_window_parks_every_interrupt_source(text):
    """The dark window runs with every interrupt SOURCE parked, not merely
    PRIMASK-masked, and the park is restorable for the rehearsal path. The
    per-primitive PRIMASK mask cannot carry this alone: eeprom.c's
    flash_wait() unconditionally __enable_irq()s inside every erase/program,
    which is how the 2026-08-01 activation faulted into the core handler's
    SYSRESETREQ with the FCB erased. No native suite compiles
    optical_main.cpp, so this is a text property."""
    definitions = function_definitions(text)
    assert OTA_PARK_FUNCTION in definitions, (
        f"missing {OTA_PARK_FUNCTION}() -- the dark window must park every "
        "interrupt source, or flash_wait()'s unconditional __enable_irq() "
        "takes a pending ISR mid-window (the 2026-08-01 incident)"
    )
    body = without_comments_and_literals(definitions[OTA_PARK_FUNCTION])
    positions = []
    for step in OTA_PARK_STEPS:
        index = body.find(step)
        assert index >= 0, (
            f"{OTA_PARK_FUNCTION}() is missing the {step!r} park step"
        )
        positions.append(index)
    assert positions == sorted(positions), (
        f"{OTA_PARK_FUNCTION}() runs its steps out of order "
        f"{list(zip(OTA_PARK_STEPS, positions))}: PRIMASK first so nothing "
        "sneaks in mid-park, then the whole NVIC bank, then SysTick, then "
        "the pended system exceptions"
    )
    assert OTA_RESTORE_FUNCTION in definitions, (
        f"missing {OTA_RESTORE_FUNCTION}() -- the rehearsal must hand the "
        "board back to service, so the park needs its restoring inverse"
    )
    restore = without_comments_and_literals(definitions[OTA_RESTORE_FUNCTION])
    for token in OTA_RESTORE_TOKENS:
        assert token in restore, (
            f"{OTA_RESTORE_FUNCTION}() is missing {token!r}: restore "
            "re-enables exactly the saved NVIC set, then SysTick, then PRIMASK"
        )


def assert_activation_quiesce_is_ordered(text):
    """OTA activation quiesces before the dark window, in the exact order the
    plan requires, and does so BEFORE it runs the RAM-resident sequencer to
    completion. RM p.279 note 3 ("no FLASH access from any master during
    erase/program") is discharged structurally by this quiesce plus the
    per-primitive PRIMASK mask, not by timing analysis, so the ordering is the
    proof and it is checked here."""
    definitions = function_definitions(text)
    assert OTA_QUIESCE_FUNCTION in definitions, (
        f"missing {OTA_QUIESCE_FUNCTION}() -- activation must quiesce the board "
        "before it erases flash, or a live master touches flash mid-erase"
    )
    body = without_comments_and_literals(definitions[OTA_QUIESCE_FUNCTION])
    positions = []
    for step in OTA_QUIESCE_STEPS:
        index = body.find(step)
        assert index >= 0, (
            f"{OTA_QUIESCE_FUNCTION}() is missing the {step!r} quiesce step"
        )
        positions.append(index)
    assert positions == sorted(positions), (
        f"{OTA_QUIESCE_FUNCTION}() runs its steps out of order "
        f"{list(zip(OTA_QUIESCE_STEPS, positions))}: the gate must close, then "
        "the audio service stop, then the ISR park"
    )
    assert OTA_ACTIVATION_SERVICE in definitions, (
        f"missing {OTA_ACTIVATION_SERVICE}() -- the deferred activation the "
        "activate 200 acknowledges before the board goes dark"
    )
    service = without_comments_and_literals(definitions[OTA_ACTIVATION_SERVICE])
    quiesce_at = service.find(OTA_QUIESCE_FUNCTION + "(")
    run_at = service.find(OTA_RESIDENT_RUN + "(")
    assert quiesce_at >= 0 and run_at > quiesce_at, (
        f"{OTA_ACTIVATION_SERVICE}() must call {OTA_QUIESCE_FUNCTION}() BEFORE "
        f"{OTA_RESIDENT_RUN}(): the dark window runs to completion, so a master "
        "left live before it would touch flash during the erase"
    )
    # begin() validates preconditions with the flash still coherent, so a
    # pre-dark refusal must come before the quiesce -- otherwise a refused
    # activation would have needlessly darkened the board.
    begin_at = service.find("aius_ota_activate_begin(")
    assert 0 <= begin_at < quiesce_at, (
        f"{OTA_ACTIVATION_SERVICE}() must run aius_ota_activate_begin() before "
        f"{OTA_QUIESCE_FUNCTION}(), so a pre-dark refusal leaves the board serving"
    )


OTA_REHEARSAL_SERVICE = "service_ota_rehearsal"
OTA_REHEARSAL_RUN = "ota_rehearse_run"
OTA_REHEARSAL_VERDICT = "ota_rehearsal_finish"


def assert_rehearsal_ends_in_a_deliberate_reset(text):
    """The rehearsal performs its own reset, after the verdict and only there.

    Measured on the board 2026-09-03: both rehearsals rebooted ~8 s after
    `ota_diag.rehearsal "pass"` with `reset_source 16` (SRC_SRSR_WDOG_RST_B)
    and `prev_boot.first_abort 9` (AUDIO). The mechanism is not the sequencer's
    -- it is `ota_quiesce()` step 2, `g_audio_service_ready = false`, which the
    snapshot restore does NOT undo (it restores interrupt SOURCES only). From
    the quiesce on, `platform_audio()` refuses the AUDIO checkpoint, every
    epoch aborts before EPOCH_COMPLETE and none feeds WDOG1, so the board
    reboots either way and the only question is whether it does so
    deliberately. A watchdog bite makes a clean rehearsal indistinguishable in
    `prev_boot` from a crash and inflates the watchdog-reset accounting the
    stall chase reads as evidence.

    So the rehearsal ends in `reset_application()` -- the SAME SYSRESETREQ the
    real activation finalizer performs, which also makes the rehearsal rehearse
    the terminal act. The pins are ordering and count:

      - exactly ONE reset in the function, so the pre-quiesce aborts (geometry,
        PSRAM, verify, stage copy, begin) keep returning WITHOUT resetting --
        the board never darkened there, so it keeps serving and its verdict
        stays readable live in `ota_diag`;
      - it comes after the quiesce, after the RAM-resident run and after the
        LAST `ota_rehearsal_finish()`, so both post-window verdicts (pass and
        fail alike -- the debt is the quiesce's, not the verdict's) are
        recorded first and the reset is out of the dark window with flash
        coherent;
      - nothing follows it, because it never returns.

    No grace window is pinned or wanted: the sequencer's terminal stamp
    (COMPLETED exactly at READY_TO_RESET) lands in the retained
    `.aius_retained_ota` slot with the REHEARSAL flag, survives SYSRESETREQ and
    is served as `/api/status` `ota_dark`, so the verdict outlives the reboot.

    No native suite compiles optical_main.cpp, so this is a text property."""
    definitions = function_definitions(text)
    assert OTA_REHEARSAL_SERVICE in definitions, (
        f"missing {OTA_REHEARSAL_SERVICE}() -- the deferred rehearsal that "
        "drives the scratch-window run and owns its terminal reset"
    )
    body = without_comments_and_literals(definitions[OTA_REHEARSAL_SERVICE])
    for name in (OTA_QUIESCE_FUNCTION, OTA_REHEARSAL_RUN, OTA_REHEARSAL_VERDICT):
        assert f"{name}(" in body, (
            f"{OTA_REHEARSAL_SERVICE}() is missing {name}()"
        )
    resets = call_count(body, "reset_application")
    assert resets == 1, (
        f"expected exactly one reset_application() in {OTA_REHEARSAL_SERVICE}"
        f"(), found {resets}. One: the terminal reset. A second would mean a "
        "PRE-quiesce abort resets too, and those never darkened the board -- "
        "they must hand it back serving with the verdict live in ota_diag"
    )
    quiesce_at = body.find(OTA_QUIESCE_FUNCTION + "(")
    run_at = body.find(OTA_REHEARSAL_RUN + "(")
    verdict_at = body.rfind(OTA_REHEARSAL_VERDICT + "(")
    reset_at = body.find("reset_application(")
    assert quiesce_at < run_at, (
        f"{OTA_REHEARSAL_SERVICE}() must call {OTA_QUIESCE_FUNCTION}() before "
        f"{OTA_REHEARSAL_RUN}()"
    )
    assert run_at < verdict_at, (
        f"{OTA_REHEARSAL_SERVICE}() must record the verdict with "
        f"{OTA_REHEARSAL_VERDICT}() after {OTA_REHEARSAL_RUN}() returns"
    )
    assert reset_at > verdict_at, (
        f"{OTA_REHEARSAL_SERVICE}() must call reset_application() AFTER the "
        f"last {OTA_REHEARSAL_VERDICT}(): the reset is the rehearsal's "
        "terminal act, and the verdict has to be recorded before it"
    )
    tail = body[reset_at + len("reset_application("):]
    assert tail.strip() == ");", (
        f"reset_application() must be the LAST statement of "
        f"{OTA_REHEARSAL_SERVICE}() -- it never returns, so anything after it "
        f"is dead code; found {tail.strip()!r} following it"
    )


def assert_activation_gates_on_psram_presence(text):
    """Activation stages the image into PSRAM and is REFUSED without it, per
    aius-ota-upgrade.md's final-slice architecture: the reworked sequencer reads
    the staged image from a linear PSRAM view during the FlexSPI1 erase (FlexSPI2
    is independent), so a board with no PSRAM cannot do it and the presence gate
    REFUSES (ota_no_psram) rather than darkening the board. Since b76f8fc
    (2026-08-16, OTA resilience Phase 2) the pad-1 staging region deliberately
    ALIASES the flight-recorder ring, to keep every in-window readback off the
    marginal pad-2 chip; the proof is therefore ORDERING, not disjointness --
    the recorder bank must already be parked, and ota_psram_staging() asserts
    that rather than trusting its caller. No native suite compiles
    optical_main.cpp, so this is a text property."""
    definitions = function_definitions(text)

    assert "ota_psram_present" in definitions, (
        "missing ota_psram_present() -- the activation PSRAM presence gate"
    )
    present = without_comments_and_literals(definitions["ota_psram_present"])
    for signal in ("external_psram_size", "g_psram_pad2_mb"):
        assert signal in present, (
            f"ota_psram_present() must read {signal!r}: BOTH PSRAM signals gate "
            "activation, so a pad-2-only board is not falsely refused"
        )

    assert "ota_psram_staging" in definitions, (
        "missing ota_psram_staging() -- the disjoint PSRAM region resolver"
    )
    staging = without_comments_and_literals(definitions["ota_psram_staging"])
    assert "aius_ota_staging_place(" in staging, (
        "ota_psram_staging() must resolve its region through the host-pinned "
        "placement helper, not an open-coded offset"
    )
    assert "RECORDER_STAGING_BYTES" in staging, (
        "ota_psram_staging() must account for the recorder's staging carve"
    )
    # The runtime half of the proof after b76f8fc: pad-1 staging aliases the
    # ring, so the bank must already be parked and the refusal is the assert.
    assert "!placement.on_pad2 && aius_recorder_bank_present(" in staging, (
        "ota_psram_staging() must refuse a pad-1 placement while the recorder "
        "bank still holds the ring that placement aliases, or a mis-ordered "
        "caller races the SD->PSRAM copy against a live producer"
    )
    assert staging.find("aius_recorder_bank_present(") < staging.find("*base ="), (
        "the park assert must run BEFORE the staging base is published, or a "
        "refusable placement still hands the caller a live-ring pointer"
    )
    clean = without_comments_and_literals(text)
    assert "static_assert(RECORDER_STAGING_BYTES" in clean, (
        "missing the static_assert pinning the recorder staging carve -- the "
        "constant half of the OTA/recorder placement proof"
    )
    assert (
        "static_assert(AIUS_OTA_STAGING_PAD_BYTES - RECORDER_STAGING_BYTES >="
        in clean
    ), (
        "missing the static_assert that pad 1 minus the staging carve still "
        "holds a maximum image; without it the marginal-chip fix would "
        "silently shrink the largest deliverable image"
    )

    assert "app_ota_activate" in definitions, "missing app_ota_activate()"
    activate = without_comments_and_literals(definitions["app_ota_activate"])
    present_at = activate.find("ota_psram_present(")
    request_at = activate.find("aius_ota_request_activation(")
    assert 0 <= present_at < request_at, (
        "app_ota_activate() must gate on ota_psram_present() BEFORE requesting "
        "activation, so a no-PSRAM board is refused and no flash is attempted"
    )

    assert OTA_ACTIVATION_SERVICE in definitions, (
        f"missing {OTA_ACTIVATION_SERVICE}()"
    )
    service = without_comments_and_literals(definitions[OTA_ACTIVATION_SERVICE])
    staging_at = service.find("ota_psram_staging(")
    copy_at = service.find("ota_copy_staged_to_psram(")
    begin_at = service.find("aius_ota_activate_begin(")
    quiesce_at = service.find(OTA_QUIESCE_FUNCTION + "(")
    assert 0 <= staging_at < copy_at < begin_at < quiesce_at, (
        f"{OTA_ACTIVATION_SERVICE}() must resolve the PSRAM region, copy "
        "SD->PSRAM, and verify (aius_ota_activate_begin) -- in that order -- "
        "BEFORE the quiesce, so every pre-window refusal leaves the board "
        "bootable and serving"
    )


def assert_watchdog_armed_once_fed_once_per_epoch(text):
    clean = without_comments_and_literals(text)
    arm_count = clean.count("aius_boot_teensy_watchdog_arm(")
    feed_count = clean.count("aius_boot_teensy_watchdog_feed(")
    assert arm_count == 1, (
        "expected exactly one aius_boot_teensy_watchdog_arm() call site "
        f"(the pinned 8 s interval), found {arm_count}"
    )
    assert feed_count == 1, (
        "expected exactly one aius_boot_teensy_watchdog_feed() call site "
        f"reachable from loop(), found {feed_count}"
    )
    setup_body = without_comments_and_literals(function_body(text, SETUP_MARKER))
    assert "aius_boot_teensy_watchdog_arm(" in setup_body, (
        "the watchdog must be armed once from setup()"
    )
    assert "aius_boot_teensy_watchdog_feed(" not in setup_body, (
        "the watchdog feed must not happen in setup(); it belongs to the "
        "per-epoch EPOCH_COMPLETE checkpoint reached through loop()"
    )


# The recovery snapshot, paired by index: the UDP/status-JSON field name and the
# abbreviated USB `ID` key naming the same AiusRecoveryStatus member. One tuple
# pair rather than two independent lists, because the three channels sharing one
# snapshot is the property under test -- see the reference doc,
# docs/reference/usb-control-protocol.md, which documents the same mapping.
RECOVERY_JSON_KEYS = (
    "network_state",
    "recovery_safe",
    "network_last_fault",
    "last_checkpoint",
    "network_generation",
    "boot_generation",
    "reset_source",
    "watchdog_early_resets",
    "loop_epoch",
    "network_restarts",
)

RECOVERY_ID_KEYS = (
    "nstate",
    "rsafe",
    "nfault",
    "ckpt",
    "ngen",
    "bgen",
    "rsrc",
    "early",
    "epoch",
    "nrst",
)

UDP_DISPATCH_MARKER = "static FLASHMEM void udp_dispatch(const AiusNetFrame *request)"
RECOVERY_STATUS_MARKER = (
    "static FLASHMEM uint16_t put_recovery_status(uint8_t *payload)"
)
STATUS_JSON_MARKER = (
    "static FLASHMEM size_t http_status_json(uint8_t *body, size_t capacity)"
)
USB_PAYLOAD_MARKER = (
    "static FLASHMEM size_t app_write_usb_payload(void *, AiusUsbCommandKind kind,"
)


def assert_udp_status_carries_recovery_payload(text):
    """The UDP status path emits the 99-byte recovery shape, not only the legacy
    71-byte one. Scoped to the two named functions, deliberately: a file-wide
    absence of put_status() would be an absence assertion, and put_status() is a
    live producer of bytes 0..70 that must stay."""
    dispatch = without_comments(function_body(text, UDP_DISPATCH_MARKER))
    assert "put_recovery_status(" in dispatch, (
        "udp_dispatch() must answer status requests with put_recovery_status(), "
        "the 99-byte recovery payload"
    )
    legacy = re.search(r"\bput_status\s*\(", dispatch)
    assert legacy is None, (
        "udp_dispatch() calls put_status() directly, so at least one arm still "
        "answers with the legacy 71-byte payload instead of the recovery one"
    )
    producer = without_comments(function_body(text, RECOVERY_STATUS_MARKER))
    assert "aius_net_status_encode(" in producer, (
        "put_recovery_status() must build its payload with the portable "
        "aius_net_status_encode(), so the device and tools/net share one layout"
    )
    length_present = (
        re.search(r"\b99u?\b", producer) is not None
        or "AIUS_NET_RECOVERY_STATUS_LEN" in producer
    )
    assert length_present, (
        "put_recovery_status() names no 99-byte recovery length, so nothing "
        "proves it returns the extended payload rather than the legacy one"
    )


def assert_status_json_carries_recovery_keys(text):
    # The keys live inside a C string literal, so the needle carries the source's
    # own backslash escapes: `\"network_state\":`.
    body = without_comments(function_body(text, STATUS_JSON_MARKER))
    missing = [key for key in RECOVERY_JSON_KEYS if f'\\"{key}\\":' not in body]
    assert not missing, (
        "the status JSON literal is missing recovery keys "
        f"{missing!r}; GET /api/status must carry the whole snapshot"
    )


BOOT_FUSE_SERIALIZER = "http_boot_fuse_json"
BOOT_FUSE_READER = "aius_boot_teensy_boot_fuse"
BOOT_FUSE_JSON_KEYS = (
    "boot_fuse_0x6e0",
    "word",
    "read",
    "sec_image_offset",
    "sec_image_size",
)

# An assignment, or any read-modify-write, to an OCOTP symbol. The MAC read in
# derive_auth_token() puts three HW_OCOTP_* names on the right-hand side of an
# assignment, which is why the scan stops at the statement terminator: a symbol
# followed by `;` before any `=` is a read, and a read is exactly what is
# allowed.
OCOTP_WRITE = re.compile(
    r"\b(?:HW_OCOTP_\w+|IMXRT_OCOTP\w*)\b[^;=<>!]*"
    r"(?:\|=|&=|\^=|\+=|-=|>>=|<<=|=(?!=))"
)
# A raw pointer into the OCOTP block, which would bypass the check above.
OCOTP_RAW_ADDRESS = re.compile(r"0x401[Ff]4[0-9A-Fa-f]{3}")


def assert_boot_fuse_is_read_only_and_served(text):
    """The boot-configuration fuse is served on /api/status, and NEVER written.

    Whether PJRC blows 0x6E0[23:16] on a Teensy 4.1 is the single gating fact
    for hardware A/B image selection on this part
    (docs/plans/aius-ota-upgrade.md), it is documented nowhere, and only a board
    can answer it -- so the answer has to be on the status surface or it is not
    obtainable at all.

    The second half is the one that matters more. Blowing an OCOTP fuse is
    irreversible and per board, and this particular word decides where the boot
    ROM looks for a secondary image, so a stray write permanently changes how
    that board boots with no reflash that undoes it. Nothing in this file may
    assign to an OCOTP symbol or reach the block through an address literal, and
    no native suite compiles this file, so this scan is the only thing standing
    between a careless edit and a bricked-to-a-different-boot-config part."""
    clean = without_comments_and_literals(text)
    write = OCOTP_WRITE.search(clean)
    assert write is None, (
        "an OCOTP write is present "
        f"({clean[write.start():write.end()].strip()!r}); the fuse read is "
        "read-only, the burn is irreversible, and 0x6E0 decides where the ROM "
        "looks for a secondary boot image"
    )
    raw = OCOTP_RAW_ADDRESS.search(clean)
    assert raw is None, (
        f"a raw OCOTP address literal is present ({raw.group(0)}); reach the "
        "register through the framework macro so the mapping has one owner"
    )
    definitions = function_definitions(text)
    assert BOOT_FUSE_SERIALIZER in definitions, (
        f"missing {BOOT_FUSE_SERIALIZER}() -- /api/status must carry the fuse "
        "word, because nothing else can answer whether hardware A/B exists"
    )
    serializer = definitions[BOOT_FUSE_SERIALIZER]
    assert f"{BOOT_FUSE_READER}(" in without_comments_and_literals(serializer), (
        f"{BOOT_FUSE_SERIALIZER}() must obtain the word from "
        f"{BOOT_FUSE_READER}(), the one guarded read"
    )
    literal = without_comments(serializer)
    missing = [key for key in BOOT_FUSE_JSON_KEYS if f'\\"{key}\\"' not in literal]
    assert not missing, (
        f"the fuse JSON is missing keys {missing!r}; the raw word, the read "
        "flag and both decoded fields are all load-bearing -- a decoded field "
        "alone cannot be re-checked, and a word without `read` cannot be told "
        "apart from a host sentinel"
    )
    status_body = without_comments_and_literals(
        function_body(text, STATUS_JSON_MARKER)
    )
    assert f"{BOOT_FUSE_SERIALIZER}(" in status_body, (
        f"{BOOT_FUSE_SERIALIZER}() is defined but GET /api/status never calls "
        "it, so the fuse is unreadable from the network after all"
    )


def assert_usb_id_carries_recovery_keys(text):
    body = without_comments(function_body(text, USB_PAYLOAD_MARKER))
    missing = [key for key in RECOVERY_ID_KEYS if f" {key}=" not in body]
    assert not missing, (
        f"the USB ID payload is missing recovery keys {missing!r}; ID is the "
        "only channel that answers in recovery-safe mode, where the stack is down"
    )


# Phase 14c: the replay path holds no card handle of its own.
#
# Nothing else can catch a regression here. No native suite compiles
# optical_main.cpp, and the failure this prevents is silent on the bench: a
# replay that reads the card directly keeps a file open across epochs, and every
# portable SD job begins with SD.begin(BUILTIN_SDCARD), so the next config PUT,
# download or probe re-mounts the volume underneath it. The replay then reads
# from a handle whose volume moved, which sounds like a glitch rather than a
# fault.
#
# The scan is by function NAME rather than by an enumerated list, so a replay
# function added later is covered by construction rather than by remembering to
# add it here.
REPLAY_FUNCTION_NAME = re.compile(r"replay", re.IGNORECASE)

# Replay functions that legitimately still reach the card directly, each with the
# reason. Both are HTTP-context routes that Phase 14c does not own: they stat and
# enumerate rather than stream, and the portable SD engine has no directory job
# to move the listing onto. Naming them keeps the carve-out reviewable -- any
# OTHER replay function reaching the card still fails.
#
# Exempt from the bounded JOB, never from being bounded at all:
# assert_replay_card_walks_are_bounded() below holds exactly these two to the
# rule app_read_directory() establishes. Phase 14d found the exemption had been
# read as a licence -- the listing walked /aius/replays with no cap and no
# exclusion -- which is why the second check exists beside this set rather than
# the set being trusted on its own.
REPLAY_SD_EXEMPT = {
    "app_replay_delete": "DELETE /api/replays stats and removes the file",
    "http_replay_list_json": "GET /api/replays enumerates /aius/replays",
}

# A direct card primitive: an `SD.<method>(` call, or a file-handle method that
# only an owned `File` has.
SD_PRIMITIVE = re.compile(
    r"\bSD\s*\.\s*[A-Za-z_][A-Za-z0-9_]*\s*\("
    r"|\b[A-Za-z_][A-Za-z0-9_]*\s*\.\s*"
    r"(?:open|read|write|seek|close|truncate|size)\s*\("
)

# The refill must be the portable bounded job, not an ad-hoc read loop. Asserted
# positively so the check cannot pass by the replay path having no I/O at all.
REPLAY_REFILL_JOB = "AIUS_SD_JOB_REPLAY_REFILL"
REPLAY_REQUIRED_FUNCTIONS = ("platform_replay",)


def assert_replay_path_has_no_direct_sd(text):
    definitions = function_definitions(text)
    scanned = {
        name: body
        for name, body in definitions.items()
        if REPLAY_FUNCTION_NAME.search(name) and name not in REPLAY_SD_EXEMPT
    }
    missing = [
        name for name in REPLAY_REQUIRED_FUNCTIONS if name not in scanned
    ]
    assert not missing, (
        f"replay functions {missing!r} were not found, so this check would "
        "pass without scanning the refill path at all"
    )
    offenders = {}
    for name, body in scanned.items():
        match = SD_PRIMITIVE.search(without_comments_and_literals(body))
        if match is not None:
            offenders[name] = match.group(0).strip()
    assert not offenders, (
        f"the replay path calls a card primitive directly: {offenders!r}. "
        "A replay read must go through the bounded "
        f"{REPLAY_REFILL_JOB} job, which opens and closes inside itself, so no "
        "replay handle can survive the SD.begin() every other job performs"
    )
    named = "".join(scanned.values())
    assert REPLAY_REFILL_JOB in without_comments_and_literals(named), (
        f"no replay function names {REPLAY_REFILL_JOB}, so nothing shows the "
        "refill goes through the bounded SD job rather than some other read"
    )


# The two card walks the exemption above allows, and the bound each must keep.
#
# `app_read_directory` is the model: it caps its raw reads at
# AIUS_OPTICAL_APP_INDEX_SCAN_MAX and the dispatcher calls it only when
# app_sd_busy() is false. The replay listing had neither -- a `for (;;)
# openNextFile()` that ran to the end of a directory of unknown size inside one
# epoch, on a card whose library is synchronous and uncancellable. That is an 8 s
# watchdog reset waiting for a big enough replays directory, and it is the kind
# of defect nothing else in this repo can catch: no native suite compiles
# optical_main.cpp, so the bound is only observable as text.
#
# Two rules, both asserted:
#   - every loop over the card terminates on the scan cap, not on the directory;
#   - a listing that stopped on either bound SAYS SO, because a truncated
#     listing that reads as a complete one is a wrong answer rather than a
#     refused one.
# The exclusion itself (503 sd_busy) is dispatcher behaviour and is proven for
# real in the optical-app suite, which drives the routes against a fake whose
# sd_busy it controls; only the bound is a property of this file's text.
REPLAY_WALK_SCAN_CAP = "AIUS_OPTICAL_APP_INDEX_SCAN_MAX"
REPLAY_WALK_TRUNCATION_FIELD = r"\"truncated\""
REPLAY_LIST_FUNCTION = "http_replay_list_json"
REPLAY_DELETE_FUNCTION = "app_replay_delete"


def assert_replay_card_walks_are_bounded(text):
    definitions = function_definitions(text)
    missing = [
        name
        for name in (REPLAY_LIST_FUNCTION, REPLAY_DELETE_FUNCTION)
        if name not in definitions
    ]
    assert not missing, (
        f"the exempt card walks {missing!r} were not found, so this check would "
        "pass without reading either bound"
    )

    listing = definitions[REPLAY_LIST_FUNCTION]
    headers = loop_headers(without_comments_and_literals(listing))
    assert headers, (
        f"{REPLAY_LIST_FUNCTION} enumerates the card but contains no loop at "
        "all, so this check has nothing to bound -- the listing has moved "
        "somewhere this check no longer sees"
    )
    unbounded = [
        header.strip() for header in headers if REPLAY_WALK_SCAN_CAP not in header
    ]
    assert not unbounded, (
        f"{REPLAY_LIST_FUNCTION} loops over the card without "
        f"{REPLAY_WALK_SCAN_CAP} in the condition: {unbounded!r}. An unbounded "
        "walk runs to the end of a directory of unknown size inside a single "
        "epoch, and the SD library cannot be cancelled once it has started"
    )
    assert REPLAY_WALK_TRUNCATION_FIELD in without_comments(listing), (
        f"{REPLAY_LIST_FUNCTION} never emits a {REPLAY_WALK_TRUNCATION_FIELD} "
        "field, so a listing its own bound cut short is indistinguishable from "
        "a complete one -- which turns a refusal into a wrong answer"
    )

    delete_headers = loop_headers(
        without_comments_and_literals(definitions[REPLAY_DELETE_FUNCTION])
    )
    assert not delete_headers, (
        f"{REPLAY_DELETE_FUNCTION} has grown a loop: {delete_headers!r}. Its "
        "card cost is bounded by being a constant two primitives, and a loop "
        "makes it a scan with no cap"
    )


# The replay refill's sizing, checked across the three libraries that own it.
#
# WHY THIS CHECK EXISTS. The replay refill was measured on the board running at
# 65 % of realtime. The fix sized the ring, the watermark, the refill and the
# job's transfer buffer together and raised the transfer buffer to 8 KB so one
# refill would be one read primitive. Every native suite stayed green, the image
# linked, all banks fit -- and the replay produced NOTHING on the board, because
# aius_sd.c caps a job's transfer capacity at AIUS_SD_MAX_TRANSFER (512) and
# validate_request() rejects a larger one with AIUS_SD_FAULT_CAPACITY before the
# job emits its first action. No mount, no open, no read, one decoder error, and
# a service parked in ERROR that never asks again.
#
# Nothing could have caught that. The engine's cap is a #define private to
# aius_sd.c, so no translation unit sees it beside the replay constants; the
# replay suite links no SD engine; the runtime-target suite links the engine but
# not optical_main.cpp's buffer; and the target build only proves the image
# fits. The invariant is real and cross-library, so it is checked here, as text,
# because text is the only place all three are visible at once.
#
# It is a genuinely weaker check than a compile-time assertion and it is used
# only where a compile-time one is impossible: the two guards optical_main.cpp
# CAN express (`slice <= AIUS_REPLAY_DELIVER_MAX_BYTES` and
# `slice <= AIUS_RUNTIME_STORAGE_TRANSFER`) stay static_asserts there, per
# §Convention enforcement.
NUMERIC_DEFINE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+"
    r"([0-9]+)[uU]?[ \t]*$",
    re.MULTILINE,
)
REPLAY_TRANSFER_DECL = re.compile(
    r"\bg_replay_transfer\s*\[\s*([A-Za-z_][A-Za-z0-9_]*|[0-9]+)[uU]?\s*\]"
)
SD_TRANSFER_CAP = "AIUS_SD_MAX_TRANSFER"
REPLAY_SIZING_REQUIRED = (
    SD_TRANSFER_CAP,
    "AIUS_REPLAY_WORD_RING_CAPACITY",
    "AIUS_REPLAY_START_WATERMARK_WORDS",
    "AIUS_REPLAY_REFILL_MAX_BYTES",
    "AIUS_REPLAY_DELIVER_MAX_BYTES",
)
# The runtime budget field the refill must NOT read. It is documented in bytes,
# consumed as bytes at the UDP checkpoint, and was consumed as WORDS here --
# which silently halved every planned refill against constants this seam
# believed it owned.
RUNTIME_RX_BUDGET_FIELD = "max_rx_bytes"
REPLAY_BUDGET_FREE_FUNCTIONS = ("replay_refill_service", "platform_replay")


def read_replay_sizing_sources():
    return "\n".join(
        (FIRMWARE_ROOT / relative).read_text()
        for relative in REPLAY_SIZING_SOURCES
    )


def _resolve_extent(token, defines):
    if token.isdigit():
        return int(token)
    assert token in defines, (
        f"g_replay_transfer is declared with extent {token!r}, which is not a "
        "plain numeric #define in any source this check reads, so its real size "
        "cannot be compared against the SD engine's cap"
    )
    return defines[token]


def assert_replay_sizing_matches_the_sd_engine(text):
    defines = {
        name: int(value) for name, value in NUMERIC_DEFINE.findall(text)
    }
    missing = [name for name in REPLAY_SIZING_REQUIRED if name not in defines]
    assert not missing, (
        f"the replay sizing constants {missing!r} were not found, so this check "
        "would pass without comparing anything"
    )

    declaration = REPLAY_TRANSFER_DECL.search(
        without_comments_and_literals(text)
    )
    assert declaration is not None, (
        "no g_replay_transfer[...] declaration was found, so the replay job's "
        "transfer slice -- the thing the SD engine validates -- is invisible "
        "to this check"
    )
    slice_bytes = _resolve_extent(declaration.group(1), defines)
    engine_cap = defines[SD_TRANSFER_CAP]

    # THE ONE THAT WOULD HAVE CAUGHT THE DEAD REPLAY.
    assert slice_bytes <= engine_cap, (
        f"the replay job's transfer slice is {slice_bytes} bytes but "
        f"{SD_TRANSFER_CAP} is {engine_cap}: aius_sd.c's validate_request() "
        "refuses any job whose capacity exceeds that cap with "
        "AIUS_SD_FAULT_CAPACITY, before the job emits its first action. Every "
        "refill would fail without mounting, opening or reading, and the replay "
        "service would park in ERROR after the first one"
    )
    assert slice_bytes <= defines["AIUS_REPLAY_DELIVER_MAX_BYTES"], (
        f"the transfer slice is {slice_bytes} bytes but a delivery is capped at "
        f"{defines['AIUS_REPLAY_DELIVER_MAX_BYTES']}: the service would refuse "
        "every chunk the job read, as a run-time NO_SPACE rather than a build "
        "failure"
    )

    ring = defines["AIUS_REPLAY_WORD_RING_CAPACITY"]
    watermark = defines["AIUS_REPLAY_START_WATERMARK_WORDS"]
    refill = defines["AIUS_REPLAY_REFILL_MAX_BYTES"]
    assert (ring - 1 - watermark) * 2 >= refill, (
        f"the ring holds {ring} words with a {watermark}-word watermark, so a "
        f"plan made just below the watermark gets {(ring - 1 - watermark) * 2} "
        f"bytes of space against a {refill}-byte refill. Every plan would be "
        "silently shrunk, and a shorter plan costs the same per-job overhead "
        "for fewer bytes"
    )
    assert ring & (ring - 1) == 0, (
        f"the word ring capacity {ring} is not a power of two; next_index() "
        "reduces modulo it on the audio pop path, so this is a hardware divide "
        "per sample"
    )

    # And the coupling that made the sizes disagree invisibly in the first
    # place: a transport receive quota gating a disk read.
    definitions = function_definitions(text)
    for name in REPLAY_BUDGET_FREE_FUNCTIONS:
        assert name in definitions, (
            f"{name}() was not found, so this check cannot see whether the "
            "refill still takes its size from the runtime transport budget"
        )
        body = without_comments_and_literals(definitions[name])
        assert RUNTIME_RX_BUDGET_FIELD not in body, (
            f"{name}() reads {RUNTIME_RX_BUDGET_FIELD}, the runtime's "
            "transport receive quota. It is documented and consumed elsewhere "
            "as BYTES and was consumed here as WORDS, which capped every "
            "planned refill at half the size this seam's own constants say -- "
            "invisibly, because the two live in different libraries. A disk "
            "refill takes its bound from AIUS_REPLAY_REFILL_MAX_BYTES and the "
            "ring, not from a network budget"
        )


# The replay clock handover.
#
# AudioStream::update_setup() grants update responsibility to the first claimant
# and g_spdif_in is constructed first, so the INPUT drives update_all() from a
# DMA ISR the incoming carrier clocks. With no carrier that ISR never fires,
# AiusAudioProcessor::update() never runs, and a replay reports `playing` while
# processing exactly zero blocks. The handover moves responsibility to the
# transmitter -- whose DMA free-runs off the audio PLL -- for the duration of a
# replay, and only while the receiver's DPLL is unlocked.
#
# Nothing else can catch a regression here: no native suite compiles
# optical_main.cpp, and losing the handover in a later refactor reproduces
# exactly the silent zero-block failure it exists to fix -- a replay that starts,
# reports `playing`, and makes no sound. So the check is two-sided rather than a
# presence assertion. WIDENING it is the other failure mode: taking the clock
# from a LIVE carrier would run passthrough at the transmitter's free-running
# rate instead of the recovered one, so the handover must stay conditional on
# pllLocked() and must be reachable from nowhere but the replay reconciler.
#
# The reconciler also carries the settled invariant -- THE LIVE INPUT ALWAYS OWNS
# THE AUDIO PATH -- so a carrier arriving mid-replay must STOP the replay, not
# merely take the clock back. A reconciler that moved the clock and left the
# replay running would leave two sources contending for the graph, which is the
# state the invariant exists to make unreachable; it would also report itself as
# a still-`playing` replay that produces nothing, which is where this began.
CLOCK_SERVICE_MARKER = "static FLASHMEM void replay_clock_service(void)"
SOURCE_SERVICE_MARKER = "static FLASHMEM void replay_source_service(void)"
REPLAY_CHECKPOINT_MARKER = (
    "static FLASHMEM bool platform_replay(void *, const AiusRuntimeBudget *budget,"
)
# Both flags, not one: a handover that sets only the taker leaves both driving
# update_all() (a double-processed block), and one that sets only the yielder
# leaves neither (a stalled graph).
CLOCK_MASTER_SETTERS = (
    "AiusSpdifOutputClock::set_master",
    "AiusSpdifInputClock::set_master",
)


def assert_replay_clock_handover_is_bounded(text):
    body = function_body(text, CLOCK_SERVICE_MARKER)
    clean = without_comments_and_literals(body)
    source_body = function_body(text, SOURCE_SERVICE_MARKER)
    source_clean = without_comments_and_literals(source_body)
    assert "AIUS_REPLAY_SOURCE_REPLAY" in clean, (
        "the clock handover is not bounded to the replay path: "
        "replay_clock_service() never reads AIUS_REPLAY_SOURCE_REPLAY, so its "
        "trigger is something other than the source the audio path is reading"
    )
    assert "pllLocked(" in clean, (
        "the clock handover does not consult AudioInputSPDIF3::pllLocked(), so "
        "a replay could take the clock away from a LIVE carrier -- the input's "
        "recovered clock is the correct master for passthrough"
    )
    assert "g_clock_replay_ready" in clean, (
        "replay_clock_service() can give output graph ownership to a replay "
        "whose exact 48 kHz clock plan and invalid hold are not acknowledged"
    )
    assert "AIUS_REPLAY_SOURCE_REPLAY" in source_clean, (
        "replay_source_service() does not bound carrier displacement to replay"
    )
    assert "pllLocked(" in source_clean, (
        "replay_source_service() does not consult the live carrier lock"
    )
    assert "aius_replay_service_stop(" in source_clean, (
        "replay_source_service() observes a carrier but never "
        "stops the replay, so a live input and a replay both hold the audio "
        "path; the live input must win outright"
    )
    assert "record_event(" in source_clean, (
        "a carrier displacing a replay is a transition, not a counter, and "
        "replay_source_service() puts nothing in the event ring -- so the one "
        "channel that could explain a replay ending by itself stays silent"
    )

    guard_open = clean.find("noInterrupts(")
    guard_close = clean.find("interrupts()", guard_open + 1) if guard_open >= 0 else -1
    assert guard_open >= 0 and guard_close > guard_open, (
        "replay_clock_service() has no noInterrupts()/interrupts() critical "
        "section; both responsibility flags are read from a DMA ISR, and an ISR "
        "entering between the two stores would see both or neither peripheral "
        "driving update_all()"
    )
    for setter in CLOCK_MASTER_SETTERS:
        local = qualified_call_sites(body, setter)
        assert len(local) == 1, (
            f"replay_clock_service() calls {setter}() {len(local)} times; the "
            "handover is one paired write, so anything else means a flag is "
            "left behind or written twice"
        )
        assert guard_open < local[0] < guard_close, (
            f"{setter}() is written outside the noInterrupts()/interrupts() "
            "critical section that makes the pair atomic against both DMA ISRs"
        )
        whole = qualified_call_sites(text, setter)
        assert len(whole) == 1, (
            f"{setter}() has {len(whole)} call sites in the file; the clock "
            "handover must be reachable only from replay_clock_service(), so "
            "nothing outside the replay path can move the graph's clock"
        )

    driver = without_comments_and_literals(
        function_body(text, REPLAY_CHECKPOINT_MARKER)
    )
    source_call = driver.find("replay_source_service(")
    clock_call = driver.find("replay_clock_service(")
    assert source_call >= 0 and clock_call > source_call, (
        "platform_replay() must call replay_source_service() before "
        "replay_clock_service(); otherwise source settlement is dead or the "
        "clock reconciles stale ownership"
    )
    assert "replay_clock_service(" in driver, (
        "platform_replay() never calls replay_clock_service(), so the handover "
        "is dead code and a replay still processes zero blocks with no carrier"
    )


# Counter-edge events.
#
# record_event() had two reachable call sites, so the ring reported a lock
# change and nothing else: every other observable fact -- overruns, dropped
# datagrams, budget overruns, config errors, network restarts, DHCP updates,
# early watchdog resets -- was a counter in the status payload, which tells a
# poller that a number changed but never when, nor in what order against
# anything else. service_event_edges() turns those counters into ring entries.
#
# Four properties are structural, and each is a real failure mode:
#
#   1. The emission sites are in the EPOCH LOOP. A counter edge detected from
#      the audio callback would put ring writes, millis() and the S/PDIF
#      peripheral read in AiusAudioProcessor::update(), which is the one
#      context this whole cutover exists to keep clear -- and the ITCM guard
#      would then be policing a section that must not exist.
#   2. The per-epoch bound EXISTS. g_overruns advances once per refused audio
#      block and is unbounded against an epoch that stalls; one event per
#      increment would blow the eight-entry ring and bury everything rare in
#      it. One `counter_edge()` per counter, one guarded `record_event()` per
#      counter, and no loop in the body IS that bound, stated in a form this
#      file can read.
#   3. FLASHMEM is on each definition. The prototype cannot carry it (GCC does
#      not merge a section attribute from a declaration), and a cold function
#      without it emits .text from the one object the ITCM allowlist pins.
#   4. The type values are ALLOCATED, never renumbered. `type` is served
#      verbatim by GET /api/events and read by tools/net/aius_net_runner.py, so
#      a renumber silently changes what every stored sample means.
#
# What this file CANNOT reach, stated plainly rather than faked: that a counter
# advancing actually produces an event, that the delta is arithmetically right,
# and that the ring drops the oldest entry while g_event_overflows stays
# truthful. No native suite compiles optical_main.cpp, so those are device
# facts, observed through GET /api/events on a board.
EVENT_TYPE_VALUES = {
    "AIUS_EVENT_STATE": 1,
    "AIUS_EVENT_TEST_TONE": 2,
    "AIUS_EVENT_REPLAY_STOP": 3,
    "AIUS_EVENT_OVERRUN": 4,
    "AIUS_EVENT_NETWORK_DROP": 5,
    "AIUS_EVENT_NETWORK_BUDGET": 6,
    "AIUS_EVENT_CONFIG_ERROR": 7,
    "AIUS_EVENT_NETWORK_RESTART": 8,
    "AIUS_EVENT_DHCP_UPDATE": 9,
    "AIUS_EVENT_WATCHDOG_EARLY_RESET": 10,
    "AIUS_EVENT_RATE_CHANGE": 11,
    "AIUS_EVENT_RATE_UNSUPPORTED": 12,
    "AIUS_EVENT_CLOCK_FAULT": 13,
    "AIUS_EVENT_UNDERRUN": 14,
    "AIUS_EVENT_GOVERNOR_TRIM": 15,
    "AIUS_EVENT_CLOCK_TRIM": 16,
    "AIUS_EVENT_TX_MISSED_FILL": 17,
    "AIUS_EVENT_AC3_CRC_REJECT": 18,
}
# Values 4..10 are the fixed counter-edge range; later episode events are not.
COUNTER_EDGE_TYPES = tuple(
    name for name, value in EVENT_TYPE_VALUES.items() if 4 <= value <= 10
)
EDGE_SERVICE = "service_event_edges"
EDGE_HELPER = "counter_edge"
AUDIO_CALLBACK_MARKER = "void update(void) override"
ENUM_ENTRY = re.compile(r"\b(AIUS_EVENT_[A-Z0-9_]+)\s*=\s*(\d+)\s*[,}]")
# uint32_t-typed only: the counter-edge scalars are all uint32_t, and the
# retained boot-ledger latches (g_prev_boot_checkpoint uint16_t,
# g_prev_first_abort uint8_t -- the defect-25 fa= instrument, 521ba20bc) are a
# different, legitimate previous-value pattern this check must not count.
PREVIOUS_STATIC = re.compile(r"^static uint32_t\b.*\bg_prev_")
PREVIOUS_SCALAR = re.compile(r"static uint32_t g_prev_[a-z0-9_]+;")


def assert_event_types_are_allocated_not_renumbered(text):
    clean = without_comments_and_literals(text)
    allocated = {}
    for name, value in ENUM_ENTRY.findall(clean):
        assert name not in allocated, (
            f"event type {name} is allocated twice in the enum"
        )
        allocated[name] = int(value)
    assert allocated == EVENT_TYPE_VALUES, (
        "the event-type allocation moved. `type` is a wire contract: GET "
        "/api/events serves it verbatim and tools/net/aius_net_runner.py reads "
        "it, so a value may be ADDED but never renumbered or reused. Expected "
        f"{EVENT_TYPE_VALUES}, found {allocated}"
    )
    values = list(allocated.values())
    assert len(set(values)) == len(values), (
        f"two event types share one value: {sorted(values)}"
    )
    for name in EVENT_TYPE_VALUES:
        assert re.search(rf"record_event\(\s*{name}\b", clean) is not None, (
            f"{name} is allocated but never recorded. An allocated type with no "
            "producer is a value no reader can ever see -- and the value is "
            "then spent, because it may not be reused"
        )


def assert_counter_edges_are_epoch_loop_bound(text):
    clean = without_comments_and_literals(text)
    definitions = function_definitions(text)

    for name in (EDGE_SERVICE, EDGE_HELPER):
        assert name in definitions, (
            f"{name}() is not defined; the counters that explain a fault are "
            "back to being numbers with no time and no order"
        )
        assert re.search(
            rf"^static\s+FLASHMEM\s+\w+\s+{name}\s*\(", clean, re.MULTILINE
        ) is not None, (
            f"{name}() carries no FLASHMEM on its DEFINITION. A prototype "
            "cannot carry it -- GCC does not merge a section attribute from a "
            "declaration -- so this cold function would emit .text from the "
            "one object the ITCM allowlist pins"
        )

    loop_body = without_comments_and_literals(function_body(text, LOOP_MARKER))
    assert loop_body.count(EDGE_SERVICE + "(") == 1, (
        f"loop() must call {EDGE_SERVICE}() exactly once per epoch; found "
        f"{loop_body.count(EDGE_SERVICE + '(')}. Zero is dead code and the "
        "defect unfixed; more than one is more than one event per counter per "
        "epoch, which is the burst the eight-entry ring cannot absorb"
    )
    reachable = clean.count(EDGE_SERVICE + "(")
    assert reachable == 2, (
        f"{EDGE_SERVICE}() has {reachable - 1} call sites; exactly one, in the "
        "epoch loop, is what makes the per-epoch bound a property of the code "
        "rather than of whoever calls it"
    )

    audio_body = without_comments_and_literals(
        function_body(text, AUDIO_CALLBACK_MARKER)
    )
    for needle in ("record_event(", EDGE_SERVICE + "(", EDGE_HELPER + "("):
        assert needle not in audio_body, (
            f"the audio callback contains {needle!r}. Event emission is epoch "
            "work: a ring write, a millis() read and a peripheral read do not "
            "belong in AiusAudioProcessor::update()"
        )

    body = without_comments_and_literals(definitions[EDGE_SERVICE])
    expected = len(COUNTER_EDGE_TYPES)
    edges = body.count(EDGE_HELPER + "(")
    assert edges == expected, (
        f"{EDGE_SERVICE}() performs {edges} counter comparisons, expected "
        f"{expected} -- one per instrumented counter, no more and no fewer"
    )
    emissions = body.count("record_event(")
    assert emissions == expected, (
        f"{EDGE_SERVICE}() has {emissions} record_event() sites against "
        f"{expected} counters. More than one per counter is one event per "
        "increment, which is the storm this bound exists to refuse"
    )
    assert re.search(r"\b(?:for|while)\s*\(", body) is None, (
        f"{EDGE_SERVICE}() contains a loop, so one epoch can emit an unbounded "
        "number of events -- the bound is one event per counter per epoch, "
        "carrying the delta"
    )
    guards = len(re.findall(r"\bif\s*\(", body))
    assert guards == expected, (
        f"{EDGE_SERVICE}() has {guards} conditionals guarding {emissions} "
        "emissions; an unguarded record_event() fires every epoch forever"
    )
    recorded = re.findall(r"record_event\(\s*(AIUS_EVENT_[A-Z0-9_]+)", body)
    assert sorted(recorded) == sorted(COUNTER_EDGE_TYPES), (
        f"{EDGE_SERVICE}() records {sorted(recorded)}, expected "
        f"{sorted(COUNTER_EDGE_TYPES)}"
    )
    for name in COUNTER_EDGE_TYPES:
        sites = len(re.findall(rf"record_event\(\s*{name}\b", clean))
        assert sites == 1, (
            f"{name} is recorded from {sites} call sites in the file; a "
            "counter edge has exactly one, inside the bounded service"
        )

    helper = without_comments_and_literals(definitions[EDGE_HELPER])
    store = re.search(r"\*\s*previous\s*=\s*current\s*;", helper)
    assert store is not None, (
        f"{EDGE_HELPER}() never writes back the previous value, so every epoch "
        "re-reports the same edge forever"
    )
    first_return = helper.find("return")
    assert first_return == -1 or store.start() < first_return, (
        f"{EDGE_HELPER}() writes back the previous value only on some paths. "
        "It must resynchronise unconditionally: POST /api/counters clears "
        "g_overruns, and a clear has to cost one missed edge rather than a "
        "permanent four-billion delta on the next real one"
    )
    assert "0xFFFF" in helper, (
        f"{EDGE_HELPER}() does not clamp to the ring's 16-bit detail field, so "
        "a large delta is reported as a small one"
    )

    previous = [
        line.strip()
        for line in clean.splitlines()
        if PREVIOUS_STATIC.match(line.strip())
    ]
    assert len(previous) == expected, (
        f"found {len(previous)} previous-value statics against {expected} "
        "counters; the edge state is one scalar per counter"
    )
    for line in previous:
        assert PREVIOUS_SCALAR.fullmatch(line) is not None, (
            f"previous-value static {line!r} is not a plain scalar. DTCM's "
            "free-for-locals zone is the whole stack for the main loop AND the "
            "audio ISR; an array here is paid for out of it"
        )


# The 24-bit output path.
#
# `audio_block_t` is `int16_t data[]`, so every sample that reached the wire
# through the AudioStream engine had been reduced to 16 bits and then shifted
# back up into the 24-bit transmit register by the retained snapshot's ISR
# (`*dest++ = (*src_left++) << 8`, output_spdif3.cpp:130). The 24-bit path
# replaces that final hop with our own: enqueue() writes the DSP's 24-bit words
# into a ring in DMAMEM and the transmit DMA interrupt drains it straight into
# SPDIF_tx_buffer.
#
# Nothing else can catch a regression here, and the failure modes are all quiet:
#
#   - Reconnecting the processor to g_spdif_out would restore the 16-bit hop AND
#     leak the pool, because nothing releases what AudioOutputSPDIF3::update()
#     queues once our ISR is installed. Audibly it is 8 lost bits; on the
#     counters it is a rising `overruns`.
#   - Losing the aius_spdif_tx24_begin() call leaves the vendor ISR installed,
#     which transmits `block_silent` forever: a board that decodes, counts
#     blocks, reports no overruns and makes no sound.
#   - Leaving the transmit channel-status registers at reset zero advertises
#     44.1 kHz and copyright asserted on a 48 kHz PCM carrier. Strict receivers
#     reject it while permissive DACs make the same firmware appear healthy.
#   - Dropping DMAMEM from the ring definition moves 16 KB into DTCM's
#     free-for-locals zone -- which is 25 KB -- and the only symptom is a stack
#     that overflows somewhere else entirely.
#   - Patching the retained snapshot instead of siting a sibling path beside it
#     would be invisible in every suite, because no suite compiles it twice.
#
# No native suite compiles optical_main.cpp or the transmit ISR, so these are
# text properties. What the text CANNOT reach -- that the words on the wire
# really carry their low 8 bits -- is proven for real by the dsp suite's
# negative control (test/dsp/test_dsp.c,
# test_tx24_carries_low_bits_the_old_path_discarded), which drives one signal
# through both paths.
AIUS_ROOT = FIRMWARE_ROOT.parent
TX_SNAPSHOT = (
    AIUS_ROOT / "evidence" / "source-snapshots" / "PJRC-SPDIF-TX-SOURCE"
    / "output_spdif3.cpp"
)
TX_MODULE = FIRMWARE_ROOT / "lib" / "aius_audio" / "src" / "aius_spdif_sources.cpp"

ENQUEUE_MARKER = "AiusStatus enqueue(const AiusPcmBlock *block)"
# The out-of-line definition (begin_pipeline moved out of the class body with
# FLASHMEM when the DSP staging carve grew it past the inliner -- the ITCM
# placement gate's requirement). The in-class line is now a declaration ending
# in `;`, which a body extractor must not match.
BEGIN_PIPELINE_MARKER = "FLASHMEM void AiusAudioProcessor::begin_pipeline(void)"
OUTPUT_WIDTH_CONSTANT = "OUTPUT_WIDTH_BITS"
TX24_BEGIN = "aius_spdif_tx24_begin"
# The line that proves the old path put 16-bit data in a 24-bit register, and
# that the snapshot still says so because we did not touch it.
SNAPSHOT_SHIFT_LINE = "*dest++ = (*src_left++) << 8;"


def assert_enqueue_writes_24_bit_words(text):
    clean = without_comments_and_literals(text)
    width = re.search(
        rf"{OUTPUT_WIDTH_CONSTANT}\s*=\s*(AIUS_DSP_OUT_WIDTH_\d+)", clean
    )
    assert width is not None, (
        f"no {OUTPUT_WIDTH_CONSTANT} selection was found; the output word width "
        "must be named in one place rather than implied by whichever conversion "
        "the ring write happens to call"
    )
    assert width.group(1) == "AIUS_DSP_OUT_WIDTH_24", (
        f"{OUTPUT_WIDTH_CONSTANT} is {width.group(1)}, not AIUS_DSP_OUT_WIDTH_24. "
        "The S/PDIF transmit register is 24 bits wide and the DSP already "
        "produces 24-bit words, so anything narrower throws bits away for nothing"
    )

    body = without_comments_and_literals(function_body(text, ENQUEUE_MARKER))
    # Whitespace-normalised, because the call spans lines in the source.
    flat = " ".join(body.split())
    needle = f"aius_out_stage_run(&g_out16, {OUTPUT_WIDTH_CONSTANT},"
    assert needle in flat, (
        "enqueue() does not run the output stage at the named width; the ring "
        f"write must go through `{needle} ...` so the width is a selection and "
        "not a coincidence of whichever conversion was called"
    )
    assert "aius_out16_run(" not in body, (
        "enqueue() still calls aius_out16_run(), so the 24 -> 16 bit reduction is "
        "back on the shipped path. It is not deleted -- it is the other arm of "
        "aius_out_stage_run() -- but a 24-bit output must bypass it"
    )
    stages = body.count("aius_spdif_tx24_stage(")
    assert stages == 2, (
        f"enqueue() calls aius_spdif_tx24_stage() {stages} times, expected 2: the "
        "destination is a ring a 512-frame block can wrap, and one call cannot "
        "cover both sides of the wrap"
    )
    commits = body.count("aius_spdif_tx24_commit(")
    assert commits == 1, (
        f"enqueue() calls aius_spdif_tx24_commit() {commits} times, expected 1. "
        "The commit is what publishes the block to the higher-priority transmit "
        "interrupt; committing per staging call would expose a half-written block"
    )
    assert "aius_spdif_tx24_free(" in body, (
        "enqueue() never checks aius_spdif_tx24_free(), so a block would be "
        "staged over frames the transmitter has not sent yet -- a wrap that "
        "sounds like a click and counts as nothing"
    )

    begin = without_comments_and_literals(
        function_body(text, BEGIN_PIPELINE_MARKER)
    )
    assert TX24_BEGIN + "(" in begin, (
        f"begin_pipeline() never calls {TX24_BEGIN}(), so the retained snapshot's "
        "16-bit ISR keeps the transmit DMA interrupt and the board transmits "
        "block_silent forever -- decoding, counting blocks, reporting no "
        "overruns, and making no sound"
    )
    sites = clean.count(TX24_BEGIN + "(")
    assert sites == 1, (
        f"{TX24_BEGIN}() has {sites} call sites; installing the transmit handler "
        "is bring-up work that happens exactly once"
    )


def assert_no_audio_block_output_hop(text):
    clean = without_comments_and_literals(text)
    assert re.search(r"\bAudioOutputSPDIF3\s+g_spdif_out\s*;", clean) is not None, (
        "g_spdif_out is no longer constructed. Its constructor is what runs "
        "config_spdif3() -- the audio PLL, the whole transmit clock tree, "
        "CORE_PIN14 as SPDIF_OUT, the DMA channel and SPDIF_tx_buffer -- so "
        "removing it silences the board rather than bypassing 16 bits"
    )
    connected = re.findall(
        r"AudioConnection\s+[A-Za-z_][A-Za-z0-9_]*\s*\(([^;]*)\)", clean
    )
    into_output = [args for args in connected if "g_spdif_out" in args]
    assert not into_output, (
        f"the processor is still connected to g_spdif_out: {into_output!r}. "
        "AudioConnection::connect() is the only thing that sets "
        "AudioStream::active, so a connection puts AudioOutputSPDIF3::update() "
        "back in the graph -- which restores the 16-bit audio_block_t hop AND "
        "leaks the pool, because nothing releases the blocks it queues once the "
        "24-bit ISR is installed"
    )
    audio_body = without_comments_and_literals(
        function_body(text, AUDIO_CALLBACK_MARKER)
    )
    for needle in ("emit_block(", "allocate(", "transmit("):
        assert needle not in audio_body, (
            f"the audio callback contains {needle!r}. The final hop is the "
            "transmit DMA interrupt draining the 24-bit ring, so this callback "
            "allocates no pool block and transmits nothing"
        )


def assert_no_int16_playback_ring(text):
    clean = without_comments_and_literals(text)
    assert re.search(r"\bemit_block\b", clean) is None, (
        "emit_block() is still present; the 16-bit drain into audio_block_t is "
        "what the 24-bit path replaces"
    )
    ring = re.search(r"\bint16_t\s+g_(?:left|right)\s*\[", clean)
    assert ring is None, (
        f"an int16 playback ring is still declared ({ring.group(0)!r} ...). The "
        "ring is 24-bit and lives in DMAMEM as g_aius_spdif_tx24"
    )
    assert re.search(r"\baius_out16_run\s*\(", clean) is None, (
        "optical_main.cpp still calls aius_out16_run(); the width selection goes "
        "through aius_out_stage_run() so that 24 bits is a bypass rather than a "
        "second code path"
    )


# --------------------------------------------------------------------------
# Output-path group -- properties of the transmit module and the snapshot it
# sits beside. Own source set, because the whole claim is that the two files
# relate in one specific way: ours includes theirs and changes nothing in it.
# --------------------------------------------------------------------------

def assert_snapshot_is_not_patched(text):
    """`text` here is the retained PJRC transmit snapshot."""
    assert SNAPSHOT_SHIFT_LINE in text, (
        f"the retained snapshot no longer contains {SNAPSHOT_SHIFT_LINE!r}. That "
        "line IS the 16-bit defect, and its presence is what proves the 24-bit "
        "path was sited beside the snapshot rather than patched into it. The "
        "snapshot is the upstream implementation boundary; a sibling path is the "
        "only sanctioned way past it"
    )
    ours = re.findall(r"\b(?:[Aa]ius|AIUS)[A-Za-z0-9_]*", text)
    assert not ours, (
        f"Aius identifiers have leaked into the retained snapshot: {sorted(set(ours))!r}"
    )


def assert_transmit_isr_drains_the_24_bit_ring(text):
    """`text` here is lib/aius_audio/src/aius_spdif_sources.cpp."""
    includes = re.findall(r'^\s*#\s*include\s*"([^"]+)"', text, re.MULTILINE)
    snapshot_includes = [h for h in includes if "source-snapshots" in h]
    assert len(snapshot_includes) == 2, (
        f"expected exactly the two retained snapshot includes, found "
        f"{snapshot_includes!r}. Compiling the snapshot through this file is the "
        "seam: SPDIF_tx_buffer is a file static of output_spdif3.cpp, so the "
        "filler is only addressable from inside this translation unit"
    )
    clean = without_comments_and_literals(text)
    assert re.search(r"\bDMAMEM\s+AiusSpdifTx24Ring\b", clean) is not None, (
        "the 24-bit ring is not declared DMAMEM. It is 16 KB; DMAMEM puts it in "
        "OCRAM (293 KB free) and without it the linker puts it in DTCM, whose "
        "free-for-locals zone -- shared by the main stack and the audio ISR -- is "
        "25 KB. The symptom would be a stack overflow somewhere unrelated"
    )
    for needle, why in (
        ("aius_spdif_tx24_fill(",
         "the handler must fill the transmit buffer from the 24-bit ring"),
        ("dma.clearInterrupt(",
         "an unacknowledged DMA interrupt re-enters immediately"),
        ("dma.attachInterrupt(",
         "nothing redirects the transmit DMA vector, so the vendor 16-bit ISR "
         "stays installed"),
        ("arm_dcache_flush(",
         "SPDIF_tx_buffer is cached OCRAM and the DMA reads it, so the fill must "
         "be flushed or the transmitter sends stale words"),
        ("update_responsibility",
         "the transmitter must still be able to drive update_all(), or a replay "
         "with no carrier processes zero blocks again"),
    ):
        assert needle in clean, f"the transmit handler omits {needle!r}: {why}"
    # The handler's own body, not the file: the receive hook beside it
    # (assert_receive_isr_stages_only_into_an_empty_pair) legitimately handles
    # audio_block_t, because the receive side still feeds the AudioStream graph.
    handler = without_comments_and_literals(
        function_body(text, "void AiusSpdifTx24Hook::isr(void)")
    )
    assert "<< 8" not in handler, (
        "the transmit handler shifts by 8, which is the 16-bit reduction it "
        "exists to remove: the DSP's word goes into the 24-bit register unchanged"
    )
    assert "audio_block_t" not in handler, (
        "the transmit handler still touches audio_block_t; bypassing that type "
        "for the final hop is the whole point of the path"
    )
    begin = without_comments_and_literals(
        function_body(text, "FLASHMEM void aius_spdif_tx24_begin(void)")
    )
    for assignment, meaning in (
        ("SPDIF_STCSCH = UINT32_C(0x00200000);",
         "consumer PCM with copyright not asserted"),
        ("SPDIF_STCSCL = UINT32_C(0x00400000);",
         "48 kHz sample-frequency status"),
    ):
        assert begin.count(assignment) == 1, (
            f"aius_spdif_tx24_begin() must set {assignment!r} exactly once "
            f"({meaning}). The snapshot leaves both channel-status registers "
            "at reset zero, which disagrees with its 48 kHz carrier and strict "
            "receivers substitute silence"
        )


def assert_receive_isr_stages_only_into_an_empty_pair(text):
    """`text` here is lib/aius_audio/src/aius_spdif_sources.cpp.

    The retained receive snapshot's isr() copies at `data[offset]` under a guard
    that admits offset == 2 * AUDIO_BLOCK_SAMPLES, 512 bytes past the block
    pair, whenever two DMA halves land between update() calls -- which is what
    the unlocked receiver's DMA storm does while replay drives the graph (RX
    pool corruption under replay, 2026-09-03: the pool leaked down to two free
    blocks and the pipeline never typed the stream again). Our handler stages
    through aius_spdif_rx_stage(), which refuses a non-empty pair."""
    handler = without_comments_and_literals(
        function_body(text, "void AiusSpdifRxHook::isr(void)")
    )
    for needle, why in (
        ("aius_spdif_rx_stage(",
         "the handler must stage through the guarded copy; that guard is what "
         "refuses a pair the graph has not consumed"),
        ("dma.clearInterrupt(",
         "an unacknowledged DMA interrupt re-enters immediately"),
        ("arm_dcache_delete(",
         "spdif_rx_buffer is cached OCRAM the DMA writes, so the half must be "
         "invalidated or the copy reads stale words"),
        ("update_responsibility",
         "the receiver must still drive update_all() while it is the graph "
         "master, or live passthrough processes zero blocks"),
    ):
        assert needle in handler, f"the receive handler omits {needle!r}: {why}"
    assert "data[offset]" not in handler and "block_offset +" not in handler, (
        "the receive handler indexes the pair by the snapshot's running offset; "
        "the guarded stage is the only sanctioned copy into a block"
    )
    install = without_comments_and_literals(
        function_body(text, "FLASHMEM void AiusSpdifRxHook::install(void)")
    )
    assert "dma.attachInterrupt(AiusSpdifRxHook::isr)" in install, (
        "nothing redirects the receive DMA vector, so the snapshot's copy loop "
        "stays installed and the out-of-bounds write returns with the next replay"
    )


OUTPUT_PATH_CHECKS = (
    (
        "the retained S/PDIF snapshot is unpatched",
        assert_snapshot_is_not_patched,
    ),
)

TRANSMIT_MODULE_CHECKS = (
    (
        "the transmit ISR drains the 24-bit ring into SPDIF_tx_buffer",
        assert_transmit_isr_drains_the_24_bit_ring,
    ),
    (
        "the receive ISR stages only into an empty block pair",
        assert_receive_isr_stages_only_into_an_empty_pair,
    ),
)


APPLY_CONFIG_MARKER = "void AiusAudioProcessor::apply_config("
RUNTIME_CONFIG_APPLY_MARKER = "AiusStatus aius_dsp_runtime_config_apply("

FEATURE_FLAGS = ("input_eq", "matrix", "output_eq", "limiter", "meters")


def assert_apply_config_reads_every_feature_flag(text):
    """All five bypass flags must be READ, not merely stored.

    `features.matrix` and `features.output_eq` were parsed, stored, round-tripped
    through GET/PUT and serialised into status JSON, and `apply_config()` read
    neither -- so two of the five documented bypasses had no mechanism at all.
    Output-EQ bypass appeared to work only because a config carrying no sections
    bypasses by accident, which is agreement rather than function.

    Read from the COMMENT-STRIPPED body deliberately. This file explains itself
    at length and now discusses both flags at some length too; a check satisfied
    by prose would pass against a firmware that only talked about the flag.
    """
    wrapper = without_comments_and_literals(
        function_body(text, APPLY_CONFIG_MARKER)
    )
    assert "aius_dsp_runtime_config_apply(" in wrapper, (
        "firmware bypasses the shared production-model resolver"
    )
    runtime_text = DSP_RUNTIME_CONFIG.read_text()
    body = without_comments_and_literals(
        function_body(runtime_text, RUNTIME_CONFIG_APPLY_MARKER)
    )
    missing = [
        flag for flag in FEATURE_FLAGS
        if f"features.{flag}" not in body
    ]
    if missing:
        raise AssertionError(
            "the shared runtime resolver never reads features."
            + ", features.".join(missing)
            + " -- a bypass flag the config accepts and the audio path ignores "
            "is a promise the device does not keep"
        )


def assert_input_stage_skips_an_exact_identity(text):
    """A unity-gain, no-EQ input stage must not run.

    The shipped default is every channel at gain_db 0 with eq [] and
    features.input_eq true, which copied six planes to scratch and spent an
    int-to-double convert plus an aius_quantize per sample -- 3,072 quantizes
    per block -- reproducing the input exactly.

    Asserted as the conjunction rather than by naming a helper, so the check
    survives the guard being written any reasonable way but still fails if
    activation goes back to reading the flag alone.
    """
    runtime_text = DSP_RUNTIME_CONFIG.read_text()
    body = without_comments_and_literals(
        function_body(runtime_text, RUNTIME_CONFIG_APPLY_MARKER)
    )
    if "input_stage_active" not in body:
        raise AssertionError(
            "the shared runtime resolver never assigns input_stage_active"
        )
    if "model->features.input_eq && input_changes_audio" not in body:
        raise AssertionError(
            "input_stage_active is set from features.input_eq alone, so the "
            "default identity config still pays 3,072 quantizes per block"
        )
    if "gain_linear" not in body or "eq_sections" not in body:
        raise AssertionError(
            "nothing in apply_config() tests channel gain and section count, so "
            "the identity it claims to detect is not actually being detected"
        )


def assert_apply_config_uses_stream_rate(text):
    wrapper = without_comments_and_literals(
        function_body(text, APPLY_CONFIG_MARKER)
    )
    assert (
        "&g_dsp_runtime, &cfg, stream_rate_hz, &g_meter" in wrapper
    ), "apply_config() does not pass the accepted stream rate to the resolver"
    assert "cfg.sample_rate_hz" not in wrapper, (
        "apply_config() passes the model's generated/default rate instead of "
        "the accepted stream rate"
    )
    runtime_text = DSP_RUNTIME_CONFIG.read_text()
    body = without_comments_and_literals(
        function_body(runtime_text, RUNTIME_CONFIG_APPLY_MARKER)
    )
    assert "model->sample_rate_hz" not in body, (
        "the shared runtime resolver designs a rate-dependent stage from the model's "
        "generated/default rate"
    )
    for call in (
        "resolve_eq(&model->output_eq[section], stream_rate_hz)",
        "&runtime->lfe_fold, (double)stream_rate_hz,",
        "resolve_eq(&input->eq[section], stream_rate_hz)",
        "aius_meter_init(meter, stream_rate_hz)",
    ):
        assert call in body, (
            f"the shared runtime resolver does not bind {call} to the accepted stream rate"
        )


def assert_rate_service_orders_safe_gate(text):
    replay = without_comments_and_literals(
        function_body(text, "static FLASHMEM bool platform_replay(")
    )
    order = [
        replay.find("replay_source_service();"),
        replay.find("sample_rate_service(now_ms);"),
        replay.find("replay_clock_service();"),
        replay.find("tx_gate_service();"),
    ]
    assert min(order) >= 0 and order == sorted(order) and len(set(order)) == 4, (
        "checkpoint 7 must settle the source, update/apply rate truth, reconcile "
        "graph ownership, then evaluate the physical gate"
    )
    service = without_comments_and_literals(
        function_body(text, "static FLASHMEM void sample_rate_service(")
    )
    for call in (
        "aius_spdif_input_rate_millihz(",
        "aius_rate_tracker_step(",
        "pipeline_pcm_rate_service(",
        "aius_spdif_clock_plan(",
        "clock_refresh_applied(",
    ):
        assert call in service, f"sample_rate_service() never calls {call}"
    assert re.search(
        r"reset_for_acceptance\s*=\s*"
        r"g_clock_relock_reset_nominal_hz\s*!=\s*"
        r"result\.nominal_rate_hz",
        service,
    ), (
        "nominal acceptance does not reset partial state whenever its relock "
        "reset credit is absent or bound to a different nominal rate"
    )
    # Decision (b), 2026-09-03: the credit test alone re-typed a pipeline
    # nothing had been ingested into (an aplay blip whose quiesce fired on an
    # unsupported measured rate), one extra PCM-confirm mute per rate change.
    assert re.search(
        r"reset_for_acceptance\s*=\s*"
        r"g_clock_relock_reset_nominal_hz\s*!=\s*"
        r"result\.nominal_rate_hz\s*&&\s*"
        r"g_audio_ingest_since_reset\s*;",
        service,
    ), (
        "nominal acceptance resets even when nothing was ingested since the "
        "last reset request; the second reset only adds a PCM-confirm mute"
    )
    assert "g_audio_ingest_since_reset = true;" in service, (
        "sample_rate_service() never records that words may have been ingested, "
        "so the acceptance reset can never fire again after the first request"
    )
    assert re.search(
        r"observed_nominal_hz\s*!=\s*g_clock_relock_reset_nominal_hz\)\s*"
        r"g_clock_relock_reset_nominal_hz\s*=\s*0u;",
        service,
    ), (
        "a changed or unsupported acquisition candidate can inherit a relock "
        "reset credit issued for a different nominal rate"
    )
    assert service.count(
        "g_clock_relock_reset_nominal_hz = 0u;"
    ) >= 2, (
        "nominal acceptance does not consume its rate-bound relock-reset credit"
    )
    refresh = without_comments_and_literals(
        function_body(text, "static FLASHMEM void clock_refresh_applied(")
    )
    for call in (
        "aius_audio_clock_get_boundary_status(",
        "aius_audio_clock_get_applied_plan(",
        "aius_rate_tracker_note_target_applied(",
    ):
        assert call in refresh, f"clock_refresh_applied() never calls {call}"
    apply = without_comments_and_literals(
        function_body(text, "static FLASHMEM bool clock_apply_desired_plan(")
    )
    order = [
        apply.find("close_stream_rate_gate();"),
        apply.find("aius_audio_clock_begin_dark_transition();"),
        apply.find("clock_flush_ring_preserve_diag();"),
        apply.find("aius_audio_clock_apply("),
    ]
    assert min(order) >= 0 and order == sorted(order) and len(set(order)) == 4, (
        "a whole-rate apply must close the pad, begin the invalid transition, "
        "flush stale words, then touch the clock"
    )
    pipeline_rate = without_comments_and_literals(
        function_body(text, "static FLASHMEM void pipeline_pcm_rate_service(")
    )
    assert "aius_pipeline_set_pcm_sample_rate(" in pipeline_rate, (
        "the rate service never updates PCM passthrough metadata"
    )
    gate = without_comments_and_literals(
        function_body(text, "static FLASHMEM void tx_gate_service(")
    )
    assert (
        "aius_tx_gate_permitted(carrier, g_clock_live_ready,"
        in gate
        and "g_clock_replay_ready, source)" in gate
    ), (
        "a locked but unaccepted/mismatched live carrier can still open the pad"
    )
    reset = gate.find("request_audio_reset();")
    credit = gate.find("g_clock_relock_reset_nominal_hz =")
    assert 0 <= reset < credit, (
        "the relock reset does not publish the credit that prevents a second "
        "reset at nominal acceptance"
    )
    replay_clock = without_comments_and_literals(
        function_body(text, "static FLASHMEM void replay_clock_service(")
    )
    assert "&& g_clock_replay_ready" in replay_clock, (
        "replay can take graph ownership before its exact 48 kHz plan and "
        "four-period invalid hold are acknowledged"
    )


def assert_enqueue_refuses_rate_before_dsp(text):
    body = without_comments_and_literals(
        function_body(text, "AiusStatus enqueue(const AiusPcmBlock *block)")
    )
    support = body.find("supported_program_rate(block->sample_rate_hz)")
    applied = body.find("!output_plan_ready")
    config = body.find("g_dsp_apply_pending")
    dsp = body.find("aius_dsp_process(")
    assert min(support, applied, config, dsp) >= 0, (
        "enqueue() is missing the supported/applied rate gate or DSP seam"
    )
    assert support < config and applied < config and config < dsp, (
        "enqueue() advances staged config or DSP before refusing an unsupported "
        "or unapplied stream rate"
    )
    assert body.count("return AIUS_REFUSED;") >= 2, (
        "enqueue() does not have distinct unsupported and mismatch refusal arms"
    )
    plan_wait = without_comments_and_literals(
        conditional_body(body, "if (!output_plan_ready)")
    )
    assert "close_stream_rate_gate();" in plan_wait, (
        "a block can wait for a clock transition while the physical pad stays open"
    )
    assert "return AIUS_NO_SPACE;" in plan_wait, (
        "an intentional clock transition drops/abandons decoded output instead "
        "of retaining it until the four-period hold is acknowledged"
    )
    assert "g_clock_mismatch_blocks++;" not in plan_wait, (
        "an intentional clock transition is misreported as a stream-rate mismatch"
    )
    assert body.count("close_stream_rate_gate();") >= 2, (
        "a refused stream waits until the next epoch to close the physical pad"
    )
    assert "replay ? AIUS_OPTICAL_SAMPLE_RATE_HZ" in body, (
        "replay is not reconciled against its fixed 48 kHz rate contract"
    )
    assert "replay ? g_clock_replay_ready" in body, (
        "replay blocks can enter DSP before the exact 48 kHz output plan is ready"
    )


CLOCK_JSON_KEYS = (
    "clock",
    "carrier_locked",
    "carrier_ready",
    "carrier_rate_millihz",
    "nominal_rate_hz",
    "stream_rate_hz",
    "output_rate_millihz",
    "trim_ppm",
    "master",
    "state",
    "rate_changes",
    "trim_updates",
    "trim_last_at_ms",
    "trim_applied_at_ms",
    # Phase 4 fill-referenced trim.
    "fill_offset_millihz",
    "fill_setpoint_frames",
    "fill_open_boundary_pending",
    "fill_trims",
    "fill_trim_last_at_ms",
    "fill_applied_at_ms",
    "structural_replans",
    "invalid_windows",
    "clock_faults",
    "last_clock_fault",
    "mismatch_blocks",
    "unsupported_blocks",
    "unsupported_episodes",
    "tx_level_frames",
    "tx_underrun_frames",
    "tx_missed_fills",
    "tx_missed_fill_frames",
    "tx_missed_fill_at_ms",
    "tx_missed_fill_flexspi_intr",
    "tx_missed_fill_flexspi2_intr",
    "tx_missed_fill_flexspi2_sts1",
    # Output-jitter phases D/E/F: gate-open TX-ring trim (cec2dfa), the
    # latency governor (fecd2e6), and the relock ring pad.
    "gate_open_target_frames",
    # 2026-09-03: the replay gate-open floor, sized to the SD primitive.
    "gate_open_replay_target_frames",
    "gate_open_trims",
    "gate_open_trimmed_frames",
    "gate_open_last_trim_at_ms",
    "gate_open_padded_frames",
    "governor_trims",
    "governor_trimmed_frames",
    "governor_last_trim_at_ms",
    "governor_last_trim_frames",
    "ingress_level_words",
    "ingress_high_water_words",
    "underrun_episode_count",
    # Phase 3 fill sensor.
    "fill_level_q4",
    "fill_subwindow_spread_frames",
    "fill_windows",
    "fill_subwindows_excluded",
    "fill_torn_reads",
    # Phase 4 sensor window sequence and last exclusion bits.
    "fill_window_seq",
    "fill_window_exclusions",
    "underrun_episodes",
    # Episode record entries.
    "start_ms",
    "ms",
    "frames",
    "gate_open",
)
CLOCK_COUNTERS = (
    "g_clock_rate_changes",
    "g_clock_trim_updates",
    "g_clock_fill_trims",
    "g_clock_structural_replans",
    "g_clock_invalid_windows",
    "g_clock_faults",
    "g_clock_last_fault",
    "g_clock_mismatch_blocks",
    "g_clock_unsupported_blocks",
    "g_clock_unsupported_episodes",
)
RATE_UNSUPPORTED_EVENT = "record_event(AIUS_EVENT_RATE_UNSUPPORTED"


# The trailing ")\n" excludes the `;`-terminated forward declaration.
TX_GATE_EXECUTE_OPEN_DEFINITION = (
    "static inline void tx_gate_execute_open(bool replay, bool first_in_unit,\n"
    "                                        uint32_t unit_frames,\n"
    "                                        uint32_t block_frames)\n"
)


def assert_replay_gate_open_pads_to_its_own_floor(text):
    """A replay's gate-open floor is sized to the SD primitive its producer
    blocks on, chosen at the pre-commit instant from enqueue()'s own source
    fact (register: "Replay start can open the gate on an empty ring",
    2026-09-03)."""
    clean = without_comments_and_literals(text)
    assert "#define AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES" in clean, (
        "no replay gate-open floor macro; the live target's one block of "
        "margin loses the race against the refill job's OPEN_READ"
    )
    assert re.search(
        r"#if\s+AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES\s*<\s*"
        r"AIUS_TX_GATE_OPEN_TARGET_FRAMES",
        clean,
    ), "the replay floor is not guarded against sinking below the live target"
    # The trailing newline excludes the `;`-terminated forward declaration.
    execute = without_comments_and_literals(
        function_body(text, TX_GATE_EXECUTE_OPEN_DEFINITION)
    )
    assert re.search(
        r"replay\s*\?\s*AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES\s*:\s*"
        r"AIUS_TX_GATE_OPEN_TARGET_FRAMES",
        execute,
    ), "tx_gate_execute_open() does not select the floor by source"
    assert re.search(
        r"aius_spdif_tx24_set_level\(\s*&g_aius_spdif_tx24,\s*target_frames,",
        execute,
    ), "the gate-open pad is not set to the selected floor"
    assert "AIUS_TX_GATE_OPEN_TARGET_FRAMES," not in execute, (
        "the gate-open pad still hard-codes the live target"
    )
    enqueue = without_comments_and_literals(
        function_body(text, "AiusStatus enqueue(const AiusPcmBlock *block)")
    )
    assert enqueue.count("tx_gate_execute_open(") == 1, (
        "enqueue() must execute the armed open exactly once per block"
    )
    fact = re.search(
        r"const bool replay\s*=\s*g_replay_service\.source\s*==\s*"
        r"AIUS_REPLAY_SOURCE_REPLAY;",
        enqueue,
    )
    call = enqueue.find("tx_gate_execute_open(replay,")
    assert fact is not None and call >= 0 and fact.start() < call, (
        "enqueue() does not pass its own replay fact to the gate-open edge"
    )


def assert_clock_status_clear_and_episode_contract(text):
    """Clock observability is complete, clearable and episode-bounded."""
    clean = without_comments_and_literals(text)
    status = without_comments_and_literals(
        function_body(text, "static FLASHMEM size_t http_status_json(")
    )
    assert status.count("http_clock_json(") == 1, (
        "GET /api/status does not append exactly one authoritative clock object"
    )

    clock_json = without_comments(
        function_body(text, "static FLASHMEM bool http_clock_json(")
    )
    keys = tuple(re.findall(r'\\"([a-z0-9_]+)\\":', clock_json))
    assert keys == CLOCK_JSON_KEYS, (
        "the clock status object field set or order changed. Expected "
        f"{CLOCK_JSON_KEYS}, found {keys}"
    )
    for counter in CLOCK_COUNTERS:
        rendered = f"(unsigned long){counter}"
        assert clock_json.count(rendered) == 1, (
            f"clock status does not publish {counter} exactly once"
        )
    assert "g_clock_fault_episode ? " in clock_json, (
        "clock.state derives active fault truth from clearable fault history; "
        "clearing counters can hide a still-bypassed failed apply"
    )
    assert (
        clock_json.find("g_clock_fault_episode ? ") <
        clock_json.find("!g_tx_gate_carrier ? ")
    ), (
        "clock.state reports an unlocked input instead of an active replay "
        "clock-apply fault"
    )

    clear = without_comments_and_literals(
        function_body(text, "void clear_counters(void)")
    )
    for counter in CLOCK_COUNTERS:
        assignment = counter + " = 0u;"
        assert clear.count(assignment) == 1, (
            f"counter clear does not reset {counter} exactly once"
        )

    enqueue = without_comments_and_literals(
        function_body(text, "AiusStatus enqueue(const AiusPcmBlock *block)")
    )
    stream_episode = without_comments_and_literals(
        conditional_body(
            enqueue, "if (!g_clock_stream_unsupported_episode)"
        )
    )
    for needle in (
        "g_clock_stream_unsupported_episode = true;",
        "g_clock_unsupported_episodes++;",
        RATE_UNSUPPORTED_EVENT,
    ):
        assert stream_episode.count(needle) == 1, (
            "unsupported stream-rate episode guard is incomplete: "
            f"{needle!r}"
        )
    assert enqueue.count(RATE_UNSUPPORTED_EVENT) == 1, (
        "stream-rate refusal has an unguarded or duplicate unsupported event"
    )
    assert enqueue.count("g_clock_stream_unsupported_episode = false;") == 1, (
        "a supported stream does not re-arm exactly one later unsupported event"
    )

    rate_service = without_comments_and_literals(
        function_body(text, "static FLASHMEM void sample_rate_service(")
    )
    carrier_episode = without_comments_and_literals(
        conditional_body(rate_service, "if (result.unsupported_accepted)")
    )
    for needle in (
        "g_clock_carrier_unsupported = true;",
        "g_clock_unsupported_episodes++;",
        RATE_UNSUPPORTED_EVENT,
    ):
        assert carrier_episode.count(needle) == 1, (
            "unsupported carrier-rate episode edge is incomplete: "
            f"{needle!r}"
        )
    assert rate_service.count(RATE_UNSUPPORTED_EVENT) == 1, (
        "carrier-rate tracking has an unguarded or duplicate unsupported event"
    )
    assert "if (!carrier)" in rate_service
    assert "g_clock_carrier_unsupported = false;" in rate_service
    assert clean.count(RATE_UNSUPPORTED_EVENT) == 2, (
        "rate-unsupported events must have exactly two producers: one guarded "
        "stream episode and one tracker-accepted carrier episode"
    )
    select_plan = without_comments_and_literals(
        function_body(text, "static FLASHMEM void clock_select_desired_plan(")
    )
    assert "g_clock_fault_episode" not in select_plan, (
        "selecting a new target clears an unresolved clock-fault episode, so "
        "continued lock failures can emit one event every target cadence"
    )
    apply_plan = without_comments_and_literals(
        function_body(text, "static FLASHMEM bool clock_apply_desired_plan(")
    )
    failure = apply_plan.find("if (applied != AIUS_CLOCK_APPLY_OK)")
    recovered = apply_plan.find("g_clock_fault_episode = false;")
    assert 0 <= failure < recovered, (
        "a successful apply is not the only operation that clears the active "
        "clock-fault episode"
    )


def assert_rate_acquisition_quiesces_ingress(text):
    update = without_comments_and_literals(
        function_body(text, "void update(void) override")
    )
    closed = update.find("!g_clock_live_ingress_open")
    live_publish = update.find("audio_ingress_publish_live(")
    missing_gap = update.find("aius_audio_ingress_record_gap(")
    assert 0 <= closed < live_publish and closed < missing_gap, (
        "AUDIO does not discard live blocks without gap accounting while the "
        "physical-rate candidate is unaccepted"
    )

    close_gate = without_comments_and_literals(
        function_body(text, "static inline void close_stream_rate_gate(")
    )
    assert "g_clock_live_ingress_open" not in close_gate, (
        "the general stream-rate gate also closes producer ingress, breaking "
        "same-nominal structural replans that preserve decoder generation"
    )
    fault = without_comments_and_literals(
        function_body(text, "static FLASHMEM void clock_record_fault(")
    )
    assert "g_clock_live_ingress_open = false;" in fault, (
        "a persistent clock fault leaves producer ingress filling while no "
        "plan can consume it"
    )
    episode = fault.find("if (g_clock_fault_episode)")
    conditional_reset = fault.find("if (!reset_already_requested)", episode)
    reset = fault.find("request_audio_reset();", conditional_reset)
    assert 0 <= episode < conditional_reset < reset, (
        "the first clock-fault episode does not own one coalesced audio reset "
        "before producer admission can recover"
    )
    apply_plan = without_comments_and_literals(
        function_body(text, "static FLASHMEM bool clock_apply_desired_plan(")
    )
    assert re.search(
        r"clock_record_fault\s*\(\s*applied\s*,\s*"
        r"reset_audio\s*\|\|\s*fault_reset_owned\s*\)",
        apply_plan,
    ), (
        "a failed apply ignores reset ownership established before the current "
        "apply call"
    )

    service = without_comments_and_literals(
        function_body(text, "static FLASHMEM void sample_rate_service(")
    )
    # Phase 4 moved the same-nominal centre path into clock_fill_service(),
    # called once from sample_rate_service(): splice its body in at the call
    # so the ownership order below still reads in execution order.
    fill_call = "clock_fill_service(source, now_ms);"
    if fill_call in service:
        assert service.count(fill_call) == 1, (
            "sample_rate_service() must call clock_fill_service() exactly once"
        )
        service = service.replace(
            fill_call,
            without_comments_and_literals(
                function_body(text, "static FLASHMEM void clock_fill_service(")
            ),
        )
    for call, owner in (
        (
            r"clock_apply_desired_plan\s*\(\s*now_ms\s*,\s*true\s*,\s*"
            r"reset_for_acceptance\s*,\s*true\s*\)",
            "nominal acceptance",
        ),
        (
            r"clock_apply_desired_plan\s*\(\s*now_ms\s*,\s*false\s*,\s*"
            r"false\s*,\s*false\s*\)",
            "same-nominal structural replan",
        ),
        (
            r"clock_apply_desired_plan\s*\(\s*now_ms\s*,\s*rate_changed\s*,\s*"
            r"rate_changed\s*,\s*true\s*\)",
            "replay acquisition",
        ),
    ):
        assert re.search(call, service), (
            owner + " does not declare its fault-reset ownership"
        )
    invalid_plan_owners = re.findall(
        r"clock_record_fault\s*\(\s*AIUS_CLOCK_APPLY_INVALID_PLAN\s*,\s*"
        r"(true|false)\s*\)",
        service,
    )
    assert invalid_plan_owners == ["true", "false", "true"], (
        "invalid-plan reset ownership must be nominal-acquisition=true, "
        "same-nominal-target=false, replay-acquisition=true"
    )
    quiesce = service.find(
        "if (result.quiesce_requested && carrier &&"
    )
    live_only = service.find("!replay_rate_requested(source)", quiesce)
    producer_close = service.find(
        "g_clock_live_ingress_open = false;", live_only
    )
    close = service.find("close_stream_rate_gate();", quiesce)
    reset = service.find("request_audio_reset();", quiesce)
    credit = service.find(
        "g_clock_relock_reset_nominal_hz = observed_nominal_hz;", quiesce
    )
    accepted = service.find("if (result.nominal_accepted)")
    assert (
        0
        <= quiesce
        < live_only
        < producer_close
        < close
        < reset
        < credit
        < accepted
    ), (
        "the first locked live changed-rate sample does not close ingress and "
        "issue one nominal-bound reset credit before acceptance"
    )
    refresh = service.find("clock_refresh_applied(source, now_ms);")
    close_episode = service.find(
        "if (replay_rate_requested(source) || !g_clock_carrier_ready)",
        refresh,
    )
    close_store = service.find(
        "g_clock_live_ingress_open = false;", close_episode
    )
    applied = service.find("else if (g_clock_plan_applied)", close_store)
    open_store = service.find("g_clock_live_ingress_open = true;", applied)
    assert 0 <= refresh < close_episode < close_store < applied < open_store, (
        "producer admission is not episode-owned after clock boundary refresh"
    )
    assert "g_clock_live_ingress_open = g_clock_plan_applied" not in service, (
        "a same-nominal structural hold closes producer admission even though "
        "it preserves decoder generation"
    )


def assert_audio_update_is_ingress_only(text):
    body = function_body(text, "void update(void) override")
    clean = without_comments_and_literals(body)
    assert not loop_headers(clean), (
        "AiusAudioProcessor::update() contains a loop; the exact sample walk "
        "belongs only in audio_ingress_publish_live()"
    )
    allowed_calls = {
        "if",
        "receiveReadOnly",
        "release",
        "aius_audio_ingress_record_source_change",
        "aius_audio_ingress_reserve",
        "aius_replay_service_pop_words",
        "aius_audio_ingress_commit",
        "aius_audio_ingress_cancel_gap",
        "aius_replay_service_discard_words",
        "audio_ingress_publish_live",
        "aius_audio_ingress_record_gap",
        # The input-fault cadence watermark (micros() between update() calls,
        # aius-output-jitter.md): one timestamp read and a max, no movement
        # of samples, so it is instrumentation rather than work.
        "micros",
    }
    calls = set(re.findall(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", clean))
    unexpected = sorted(calls - allowed_calls)
    assert not unexpected, (
        "AiusAudioProcessor::update() performs work beyond ingress movement: "
        + ", ".join(unexpected)
    )
    assert clean.count("audio_ingress_publish_live(") == 1, (
        "live input must use exactly one named bounded producer helper"
    )
    assert re.search(
        r"aius_replay_service_pop_words\s*\(\s*&g_replay_service\s*,\s*"
        r"slot\s*,\s*AIUS_AUDIO_INGRESS_CHUNK_WORDS\s*\)",
        clean,
    ), "replay does not pop its exact 256 words directly into the reserved slot"
    assert re.search(
        r"aius_replay_service_discard_words\s*\(\s*&g_replay_service\s*,\s*"
        r"AIUS_AUDIO_INGRESS_CHUNK_WORDS\s*\)",
        clean,
    ), "full ingress does not discard one bounded 256-word replay chunk"
    source_change = clean.find("aius_audio_ingress_record_source_change(")
    first_reserve = clean.find("aius_audio_ingress_reserve(")
    assert 0 <= source_change < first_reserve, (
        "the effective-source transition must publish before the first "
        "commit, overflow gap, or missing-input gap"
    )
    helper = function_body(
        text,
        "audio_ingress_publish_live(uint16_t *slot, const audio_block_t *left,",
    )
    headers = loop_headers(without_comments_and_literals(helper))
    assert len(headers) == 1 and "i < AUDIO_BLOCK_SAMPLES" in headers[0], (
        "audio_ingress_publish_live() must contain the one exact "
        "AUDIO_BLOCK_SAMPLES interleave walk"
    )


def assert_audio_checkpoint_owns_service(text):
    body = function_body(text, "static FLASHMEM bool platform_audio")
    clean = without_comments_and_literals(body)
    assert clean.count("aius_audio_service_run(") == 1, (
        "the AUDIO checkpoint must run the portable service exactly once"
    )
    assert "noInterrupts(" not in clean and "interrupts(" not in clean, (
        "platform_audio() masks the whole reset/service path; only the service "
        "critical-section hooks may mask the ingress producer"
    )
    assert "ARM_DWT_CYCCNT" in clean, (
        "the full AUDIO checkpoint path is not measured in target cycles"
    )
    begin = function_body(
        text, "FLASHMEM void AiusAudioProcessor::begin_pipeline(void)"
    )
    begin_clean = without_comments_and_literals(begin)
    call = begin_clean.find("aius_audio_service_init(")
    assert call >= 0, "begin_pipeline() never initialises the AUDIO service"
    opening = begin_clean.find("(", call)
    closing = matching_delimiter(begin_clean, opening, "(", ")")
    args = begin_clean[opening + 1: closing]
    assert len([part for part in args.split(",") if part.strip()]) == 8, (
        "aius_audio_service_init() must receive all eight arguments, including "
        "the narrow reset critical-section hooks"
    )
    assert "audio_enter_reset_critical" in args
    assert "audio_exit_reset_critical" in args
    enter = without_comments_and_literals(
        function_body(text, "static FLASHMEM void audio_enter_reset_critical")
    )
    leave = without_comments_and_literals(
        function_body(text, "static FLASHMEM void audio_exit_reset_critical")
    )
    assert enter.strip() == "NVIC_DISABLE_IRQ(IRQ_SOFTWARE);", (
        "the reset enter hook must mask only the software-audio ingress producer"
    )
    assert leave.strip() == "NVIC_ENABLE_IRQ(IRQ_SOFTWARE);", (
        "the reset exit hook must unmask only the software-audio ingress producer"
    )
    assert "noInterrupts(" not in enter and "interrupts(" not in leave, (
        "the narrow reset hooks must never globally mask every target IRQ"
    )
    clear = without_comments_and_literals(
        function_body(text, "void clear_counters(void)")
    )
    status = without_comments_and_literals(
        function_body(text, "static FLASHMEM bool http_audio_diag_json")
    )
    for label, producer_snapshot in (
        ("counter clear", clear),
        ("status snapshot", status),
    ):
        assert "NVIC_DISABLE_IRQ(IRQ_SOFTWARE);" in producer_snapshot
        assert "NVIC_ENABLE_IRQ(IRQ_SOFTWARE);" in producer_snapshot
        assert "noInterrupts(" not in producer_snapshot, (
            f"{label} globally masks every target IRQ"
        )
    loop = without_comments_and_literals(function_body(text, LOOP_MARKER))
    assert "AIUS_RUNTIME_EPOCH_BUDGET_US = 25000u" in text
    assert "completed && !g_runtime.recovery_safe" in loop
    assert loop.count("epoch_us > AIUS_RUNTIME_EPOCH_BUDGET_US") == 1, (
        "completed normal epochs are not checked exactly once against 25 ms"
    )


def assert_enqueue_preflights_before_effects(text):
    body = function_body(text, "AiusStatus enqueue(const AiusPcmBlock *block)")
    clean = without_comments_and_literals(body)
    order = (
        clean.find("block == nullptr"),
        clean.find("aius_spdif_tx24_free("),
        clean.find("g_dsp_apply_pending"),
        clean.find("aius_dsp_process("),
        clean.find("aius_spdif_tx24_commit("),
        clean.find("aius_measure_producer_block("),
        clean.find("aius_recorder_bank_producer_block("),
        clean.find("g_blocks++"),
    )
    assert all(position >= 0 for position in order), (
        "enqueue() is missing validation, capacity preflight, or one committed "
        "writer effect"
    )
    assert tuple(sorted(order)) == order, (
        "enqueue() must validate and check TX capacity before config/DSP state, "
        "then commit TX before measurement, recorder and counters"
    )


def assert_audio_storage_and_diagnostics(text):
    clean = without_comments_and_literals(text)
    assert re.search(
        r"\bDMAMEM\s+static\s+uint16_t\s+"
        r"g_audio_ingress_words\s*\[\s*AIUS_AUDIO_INGRESS_CAPACITY_WORDS\s*\]",
        clean,
    ), "the exact target ingress storage is not a DMAMEM uint16_t array"
    assert "AIUS_AUDIO_INGRESS_CAPACITY_WORDS == 8192u" in clean, (
        "the target does not pin ingress capacity to exactly 8192 words"
    )
    assert "sizeof(g_audio_ingress_words) == 16384u" in clean, (
        "the target does not pin ingress storage to exactly 16 KiB"
    )
    assert re.search(r"\bg_recorder_staging\s*\[", clean) is None, (
        "the obsolete OCRAM recorder sample array still exists"
    )
    assert "RECORDER_STAGING_SLOTS 8u" in clean
    assert "aius_recorder_ring_bytes_after_staging(" in clean
    required_fields = (
        "ingress_drop_overflow",
        "ingress_drop_missing_live",
        "ingress_drop_short_replay",
        "source_flushes",
        "gap_flushes",
        "service_steps_attempted",
        "service_steps_committed",
        "service_max_us",
        "service_max_cycles",
        "service_budget_overruns",
        "decode_units",
        "parse_rejects",
        "decode_rejects",
        "transmit_stalls",
        "retained_retries",
        "output_drops",
        "au_abandons",
        "tx_high_water_frames",
        "runtime_epoch_max_us",
        "runtime_epoch_budget_overruns",
    )
    missing = [field for field in required_fields if field not in text]
    assert not missing, "audio diagnostics are missing: " + ", ".join(missing)
    overrun = without_comments_and_literals(
        function_body(text, "uint32_t overrun_count(void) const")
    )
    assert (
        "__atomic_load_n(&g_audio_ingress.dropped_chunks,"
        in overrun
        and "__ATOMIC_ACQUIRE" in overrun
    ), (
        "legacy overruns no longer projects the ingress dropped-chunk count"
    )
    assert "noInterrupts(" not in overrun, (
        "the per-block overrun projection globally masks every target IRQ"
    )


def assert_noload_diagnostics_are_initialized(text):
    setup = without_comments_and_literals(function_body(text, SETUP_MARKER))
    clear = "memset(&g_replay_diag, 0, sizeof(g_replay_diag));"
    assert setup.count(clear) == 1, (
        "setup() must clear the NOLOAD replay diagnostics exactly once"
    )
    assert setup.find(clear) < setup.find("AudioMemory("), (
        "NOLOAD replay diagnostics must be initialized before audio/runtime "
        "startup can serve status"
    )


DSP_ORIGIN_VALUES = ("DSP_ORIGIN_DEFAULT", "DSP_ORIGIN_SD", "DSP_ORIGIN_PUT")
DSP_SOURCE_NAME_MARKER = (
    "static FLASHMEM const char *dsp_source_name(DspModelOrigin origin)"
)
DSP_ORIGIN_NAME_MARKER = (
    "static FLASHMEM const char *dsp_origin_name(DspModelOrigin origin)"
)
DSP_CONFIG_JSON_MARKER = (
    "static FLASHMEM size_t http_dsp_config_json(uint8_t *body, size_t capacity)"
)


def assert_dsp_model_origin_is_staged_not_assumed(text):
    """WHICH stager produced the running DSP model is the stager's fact, carried
    across the existing block-boundary handoff -- never a constant the swap
    assumes.

    The defect: a single `g_dsp_source_sd` bool that the swap set true for ANY
    applied model, so a model uploaded by PUT /api/dsp/config reported itself as
    having come off the card. One bool cannot carry three origins, so the swap
    had nothing to report but the one it was hard-coded to.

    The wire half is a compatibility rule, not decoration. `source` is a shipped
    two-word vocabulary ("default", or "sd" meaning an override is applied) and
    stays that way, because widening it in place hands every existing client a
    value it has no arm for. And `model_origin` is served on /api/status ONLY:
    aius_dsp_config_parse_put() rejects any top-level key outside its nine, so a
    tenth key on GET /api/dsp/config would 400 every GET-modify-PUT that echoes
    the document back."""
    clean = without_comments_and_literals(text)
    assert not identifier_present(text, "g_dsp_source_sd"), (
        "the g_dsp_source_sd bool is still present -- it cannot distinguish an "
        "SD-staged model from a PUT-staged one, which is the whole defect"
    )
    for name in DSP_ORIGIN_VALUES:
        assert identifier_present(text, name), (
            f"the staged-origin vocabulary is missing {name!r}"
        )

    # Staged beside the model it describes, ahead of the volatile flag that
    # publishes both. The handoff rule is positional, so the declaration is too.
    declarations = (
        clean.find("g_pending_dsp_model;"),
        clean.find("DspModelOrigin g_pending_dsp_origin"),
        clean.find("volatile bool g_dsp_apply_pending"),
    )
    assert all(position >= 0 for position in declarations), (
        "the staged model, its origin, or the volatile apply flag is missing"
    )
    assert tuple(sorted(declarations)) == declarations, (
        "g_pending_dsp_origin must be declared with the staged payload, ahead "
        "of the volatile g_dsp_apply_pending flag that publishes it"
    )

    # The swap: observe the flag, copy the model, ADOPT the staged origin, then
    # release the flag.
    enqueue = without_comments_and_literals(function_body(text, ENQUEUE_MARKER))
    # Checked before the ordering below, because a hard-coded origin IS the
    # original defect and deserves to be named as one rather than reported as a
    # missing statement.
    for value in DSP_ORIGIN_VALUES:
        assert f"g_dsp_model_origin = {value}" not in enqueue, (
            "the block-boundary swap assigns a CONSTANT origin; that is the "
            "g_dsp_source_sd defect with a wider type"
        )
    order = (
        enqueue.find("const bool model_pending = g_dsp_apply_pending;"),
        enqueue.find("g_active_model = g_pending_dsp_model;"),
        enqueue.find("g_dsp_model_origin = g_pending_dsp_origin;"),
        enqueue.find("g_dsp_apply_pending = false;"),
    )
    assert all(position >= 0 for position in order), (
        "the block-boundary swap does not read the flag, copy the model, adopt "
        "the staged origin and release the flag"
    )
    assert tuple(sorted(order)) == order, (
        "the swap must adopt the staged origin after copying the model it "
        "describes and before releasing the handoff flag"
    )

    # Both stagers, each writing the origin with the model and before any flag.
    definitions = function_definitions(text)
    for name in ("app_store_dsp_config", "platform_publish_config"):
        assert name in definitions, f"missing DSP model stager {name!r}"
    put_body = without_comments_and_literals(definitions["app_store_dsp_config"])
    put_order = (
        put_body.find("g_pending_dsp_model = parsed;"),
        put_body.find("g_pending_dsp_origin = DSP_ORIGIN_PUT;"),
    )
    assert all(position >= 0 for position in put_order), (
        "the PUT stager does not stage the model and DSP_ORIGIN_PUT beside it"
    )
    assert tuple(sorted(put_order)) == put_order, (
        "the PUT stager must stage its origin with the model"
    )
    assert "g_dsp_apply_pending" not in put_body, (
        "the PUT stager arms the apply flag itself, so a failed SD write could "
        "reach the audio block boundary and the staged origin's "
        "before-the-flag ordering is no longer the commit arm's to keep"
    )
    sd_body = without_comments_and_literals(
        definitions["platform_publish_config"]
    )
    sd_order = (
        sd_body.find("g_pending_dsp_model = parsed;"),
        sd_body.find("g_pending_dsp_origin = DSP_ORIGIN_SD;"),
        sd_body.find("g_dsp_apply_pending = true;"),
    )
    assert all(position >= 0 for position in sd_order), (
        "the SD stager does not stage the model, its origin and the apply flag"
    )
    assert tuple(sorted(sd_order)) == sd_order, (
        "the SD stager must write the staged origin BEFORE the volatile apply "
        "flag publishes the staged model, or AUDIO can read an origin that "
        "belongs to the previous model"
    )

    # The two renderings, and the vocabulary each is allowed.
    source_name = without_comments(function_body(text, DSP_SOURCE_NAME_MARKER))
    assert '"default"' in source_name and '"sd"' in source_name, (
        "the `source` rendering no longer emits both shipped values"
    )
    assert '"put"' not in source_name, (
        "the shipped two-value `source` vocabulary grew a third value; an "
        "existing client has no arm for it"
    )
    origin_name = without_comments(function_body(text, DSP_ORIGIN_NAME_MARKER))
    for value in ('"default"', '"sd"', '"put"'):
        assert value in origin_name, (
            f"the `model_origin` rendering is missing {value}"
        )

    status = without_comments(function_body(text, STATUS_JSON_MARKER))
    assert '\\"source\\":\\"%s\\"' in status, (
        "GET /api/status dropped or retyped its `source` field"
    )
    assert '\\"model_origin\\":\\"%s\\"' in status, (
        "GET /api/status does not carry `model_origin`, so nothing on the "
        "device says whether the running model arrived by PUT or off the card"
    )
    # Both renderings went staged-aware in d2d058c, matching the selection
    # http_dsp_config_json already made: while a commit is pending they report
    # the STAGED origin, not the still-applied one.
    assert status.count(
        "dsp_apply_pending ? g_pending_dsp_origin : g_dsp_model_origin"
    ) == 1, (
        "the status origin is not the staged-aware selection, so a pending "
        "commit is reported under the origin it is about to replace"
    )
    assert status.count("dsp_source_name(reported_origin)") == 1, (
        "the status `source` value is not derived from the reported origin"
    )
    assert status.count("dsp_origin_name(reported_origin)") == 1, (
        "the status `model_origin` value is not the reported origin verbatim"
    )

    dsp_config = without_comments(function_body(text, DSP_CONFIG_JSON_MARKER))
    assert '\\"source\\":\\"%s\\"' in dsp_config, (
        "GET /api/dsp/config dropped or retyped its `source` field"
    )
    # Staged-aware since fc674c6 (defect 22): the served document describes the
    # staged model while a commit is pending, so `source` must describe it too.
    assert (
        "apply_pending ? g_pending_dsp_origin : g_dsp_model_origin"
        in dsp_config
    ), (
        "the config origin is not the staged-aware selection, so `source` can "
        "describe a different model than the body around it"
    )
    assert "dsp_source_name(origin)" in dsp_config, (
        "the config `source` value is not derived from the selected origin"
    )
    # The KEY, not the substring: `g_dsp_model_origin` is read here legitimately
    # to derive `source`, and it contains this name.
    assert '\\"model_origin\\"' not in dsp_config, (
        "GET /api/dsp/config emits `model_origin`, but this document's key set "
        "is a round-trip contract: aius_dsp_config_parse_put() answers "
        "UNKNOWN_KEY for a tenth top-level key, so every GET-modify-PUT that "
        "echoes the document back would 400. Accept-and-ignore it in "
        "lib/aius_dsp_config first"
    )


RECORDER_JSON_MARKER = (
    "static FLASHMEM size_t app_write_recorder(void *, uint8_t *body,"
)
RECORDER_SLOT_ARRAY_MARKER = (
    "static FLASHMEM bool recorder_slot_array(char *output, size_t capacity,"
)
BEGIN_PIPELINE_MARKER = (
    "FLASHMEM void AiusAudioProcessor::begin_pipeline(void)"
)


def assert_recorder_reports_the_streams_rate(text):
    """GET /api/recorder reports the rate of the frames actually in the ring,
    not the build's compile-time nominal.

    AIUS_OPTICAL_SAMPLE_RATE_HZ is what the build was configured for; the
    accepted live nominal may be 44.1 kHz. Serving the constant made every
    pulled WAV of a 44.1 kHz capture play ~8.8% fast, and no field on the route
    contradicted it. The producer tap is where the block's own rate is in
    scope, so that is where the capture learns it."""
    enqueue = without_comments_and_literals(function_body(text, ENQUEUE_MARKER))
    # Rolling-bank rename (72196d1, Phase B): the recorder seam is now the bank.
    opening = enqueue.find("aius_recorder_bank_producer_block(")
    assert opening >= 0, "the enqueue flight-recorder tap is missing"
    arguments = enqueue[
        enqueue.find("(", opening) + 1:
        matching_delimiter(enqueue, enqueue.find("(", opening), "(", ")")
    ]
    assert "block->sample_rate_hz" in arguments, (
        "the flight-recorder producer tap does not pass the block's own sample "
        "rate, so the capture cannot know which rate its frames carry"
    )

    begin = without_comments_and_literals(
        function_body(text, BEGIN_PIPELINE_MARKER)
    )
    arms = []
    for match in re.finditer(r"aius_recorder_bank_init\s*\(", begin):
        arm_open = begin.find("(", match.start())
        arms.append(
            begin[arm_open + 1: matching_delimiter(begin, arm_open, "(", ")")]
        )
    assert len(arms) == 2, (
        "expected the fitted-PSRAM and bare-board recorder init arms, found "
        f"{len(arms)}"
    )
    for index, arm in enumerate(arms):
        assert "AIUS_OPTICAL_SAMPLE_RATE_HZ" in arm, (
            f"recorder init arm {index} does not pass the build's configured "
            "nominal, so its pre-capture reported rate is not the device's"
        )

    # Rolling bank (Phase C): the per-slot rate facts moved out of the bank
    # document into recorder_slot_array, one entry per slot. Both halves of the
    # served document must stay free of the compile-time nominal.
    body = without_comments(function_body(text, RECORDER_JSON_MARKER))
    slots = without_comments(function_body(text, RECORDER_SLOT_ARRAY_MARKER))
    assert "recorder_slot_array(" in body, (
        "the bank document no longer appends the per-slot array, so nothing "
        "on the route carries the observed rate"
    )
    for where, document in (("the bank document", body),
                            ("the slot array", slots)):
        assert "AIUS_OPTICAL_SAMPLE_RATE_HZ" not in document, (
            f"{where} still serves the compile-time nominal; the "
            "snapshot's observed rate is the only honest one"
        )
    assert '\\"sample_rate_hz\\":' in slots and "s.sample_rate_hz" in slots, (
        "GET /api/recorder does not serve the snapshot's observed rate"
    )
    assert '\\"rate_changes\\":' in slots and "s.rate_changes" in slots, (
        "GET /api/recorder does not report rate_changes, so a downloader "
        "cannot tell a single-rate ring from a mixed-rate one it must reject"
    )


def assert_ota_seam_composed_under_option(text):
    """The OTA composition root's contract (docs/plans/aius-ota-upgrade.md slice
    3): the whole staged-upgrade surface sits behind the AIUS_OTA compile-time
    option; the six-callback seam is installed on the runtime's own dispatcher
    AFTER it is constructed (aius_optical_app_init zeroes the OTA slot); the
    feature latch is armed at composition because none of the five routes toggles
    it and the slice must reach ARMED; the ARM verb arms the IMAGE
    (aius_ota_arm_image, verified -> armed) rather than the latch; and ota_diag
    rides GET /api/status. The routes themselves and the dispatcher's
    absent-then-token gate live outside this file."""
    assert "#if AIUS_OTA" in text, (
        "the OTA surface must sit behind the AIUS_OTA compile-time option"
    )
    assert "aius_optical_app_set_ota(&g_runtime.app" in text, (
        "the OTA seam must be installed on the runtime's constructed dispatcher"
    )
    assert "aius_ota_latch_arm(" in text, (
        "the composition must arm the OTA feature latch: no route toggles it "
        "and this slice must reach ARMED"
    )
    assert "aius_ota_arm_image(" in text, (
        "the ARM verb must arm the image (verified -> armed), not the latch"
    )
    assert "http_ota_diag_json" in text, (
        "ota_diag must ride GET /api/status"
    )
    construct = text.find(
        "aius_runtime_teensy_construct(&g_runtime, &g_aius_optical_app"
    )
    compose = text.find("ota_compose();")
    assert construct >= 0, "the runtime must be constructed in setup()"
    assert compose >= 0, "setup() must call ota_compose()"
    assert compose > construct, (
        "ota_compose() must run AFTER aius_runtime_teensy_construct(): the "
        "runtime zeroes the OTA slot as it builds the dispatcher"
    )


POSITIVE_CHECKS = (
    ("setup() is construction-only", assert_setup_is_construction_only),
    ("loop() delegates exactly one epoch", assert_loop_delegates_one_epoch),
    (
        "apply_config() reads all five feature bypass flags",
        assert_apply_config_reads_every_feature_flag,
    ),
    (
        "an exact-identity input stage is skipped, not run",
        assert_input_stage_skips_an_exact_identity,
    ),
    (
        "rate-dependent DSP configuration uses the accepted stream rate",
        assert_apply_config_uses_stream_rate,
    ),
    (
        "checkpoint 7 updates rate truth before evaluating the gate",
        assert_rate_service_orders_safe_gate,
    ),
    (
        "rate acquisition quiesces producer ingress until the plan is applied",
        assert_rate_acquisition_quiesces_ingress,
    ),
    (
        "enqueue refuses unsupported and unapplied rates before DSP",
        assert_enqueue_refuses_rate_before_dsp,
    ),
    (
        "a replay's gate-open edge pads to its own primitive-sized floor",
        assert_replay_gate_open_pads_to_its_own_floor,
    ),
    (
        "clock status, clears and unsupported episodes remain observable",
        assert_clock_status_clear_and_episode_contract,
    ),
    (
        "a requested reset waits for the drained transaction, bounded",
        assert_requested_reset_waits_for_the_drained_transaction,
    ),
    (
        "OTA activation quiesces (gate, audio, ISR) in order before the dark window",
        assert_activation_quiesce_is_ordered,
    ),
    (
        "the dark window parks every interrupt source, restorably for rehearsal",
        assert_dark_window_parks_every_interrupt_source,
    ),
    (
        "the OTA rehearsal ends in a deliberate reset, after its verdict",
        assert_rehearsal_ends_in_a_deliberate_reset,
    ),
    (
        "OTA activation gates on PSRAM presence and stages only over a parked recorder ring",
        assert_activation_gates_on_psram_presence,
    ),
    (
        "watchdog armed once at 8 s, fed exactly once per epoch",
        assert_watchdog_armed_once_fed_once_per_epoch,
    ),
    (
        "UDP status carries the 99-byte recovery payload",
        assert_udp_status_carries_recovery_payload,
    ),
    (
        "status JSON carries all ten recovery keys",
        assert_status_json_carries_recovery_keys,
    ),
    (
        "USB ID carries all ten recovery keys",
        assert_usb_id_carries_recovery_keys,
    ),
    (
        "boot fuse 0x6E0 is served on /api/status and never written",
        assert_boot_fuse_is_read_only_and_served,
    ),
    (
        "the OTA seam is composed under the option, after construction",
        assert_ota_seam_composed_under_option,
    ),
    (
        "the replay path performs no direct SD primitive",
        assert_replay_path_has_no_direct_sd,
    ),
    (
        "the exempt replay card walks are bounded and report truncation",
        assert_replay_card_walks_are_bounded,
    ),
    (
        "the clock handover exists and is bounded to the replay path",
        assert_replay_clock_handover_is_bounded,
    ),
    (
        "event types are allocated, never renumbered",
        assert_event_types_are_allocated_not_renumbered,
    ),
    (
        "counter edges emit from the epoch loop, one per counter per epoch",
        assert_counter_edges_are_epoch_loop_bound,
    ),
    (
        "enqueue() writes 24-bit words into the transmit ring",
        assert_enqueue_writes_24_bit_words,
    ),
    (
        "the final hop bypasses audio_block_t entirely",
        assert_no_audio_block_output_hop,
    ),
    (
        "the software audio callback performs ingress movement only",
        assert_audio_update_is_ingress_only,
    ),
    (
        "the AUDIO checkpoint owns deferred service and narrow reset masking",
        assert_audio_checkpoint_owns_service,
    ),
    (
        "enqueue preflights before every writer effect",
        assert_enqueue_preflights_before_effects,
    ),
    (
        "audio storage and cause-separated diagnostics remain exact",
        assert_audio_storage_and_diagnostics,
    ),
    (
        "NOLOAD diagnostics are initialized before runtime startup",
        assert_noload_diagnostics_are_initialized,
    ),
    (
        "the DSP model's origin is the stager's staged fact, not the swap's",
        assert_dsp_model_origin_is_staged_not_assumed,
    ),
    (
        "the flight recorder reports the rate its frames carry",
        assert_recorder_reports_the_streams_rate,
    ),
)


# Checks whose subject spans more than optical_main.cpp. Run over the
# concatenated REPLAY_SIZING_SOURCES rather than the one file, because the
# invariant they assert is precisely that those files agree with each other.
CROSS_SOURCE_CHECKS = (
    (
        "the replay sizing agrees with the SD engine's transfer cap",
        assert_replay_sizing_matches_the_sd_engine,
    ),
)


# --------------------------------------------------------------------------
# Absence group -- constructs the post-cutover file must NOT contain.
# --------------------------------------------------------------------------

FORBIDDEN_STATIC_FALLBACK_IDENTIFIERS = (
    "g_static_ip",
    "g_static_mask",
    "AIUS_STATIC_IP",
    "AIUS_STATIC_MASK",
)


def assert_no_static_fallback(text):
    for name in FORBIDDEN_STATIC_FALLBACK_IDENTIFIERS:
        assert not identifier_present(text, name), (
            f"static-address fallback identifier {name!r} is still present"
        )


def assert_no_raw_fnet_access(text):
    clean = without_comments_and_literals(text)
    match = re.search(r"\bfnet_[A-Za-z0-9_]*\s*\(", clean)
    if match is not None:
        raise AssertionError(
            "a raw FNET call is present in optical_main.cpp; only "
            f"aius_network_teensy.cpp may reach FNET (matched {match.group(0)!r})"
        )


FORBIDDEN_REBIND_GLOBALS = (
    "g_dhcp",
    "g_dhcp_pending",
    "g_network_retry_at",
    "g_ethernet_initialized",
    "g_http_server_rebinds",
)


def assert_no_rebind_globals(text):
    for name in FORBIDDEN_REBIND_GLOBALS:
        assert not identifier_present(text, name), (
            f"listener-rebind global {name!r} is still present"
        )


def assert_no_blocking_socket_write_loop(text):
    for body in loop_bodies(text):
        clean_body = without_comments_and_literals(body)
        if ".write(" in clean_body and "millis(" in clean_body:
            raise AssertionError(
                "a loop body writes to a socket while checking millis() -- "
                "a blocking socket write loop, not the portable ABI's "
                "single-attempt write_once() per epoch: "
                f"{clean_body.strip()[:80]!r}..."
            )


def assert_no_extra_watchdog_feed(text):
    clean = without_comments_and_literals(text)
    assert call_count(text, "aius_boot_teensy_watchdog_feed") <= 1, (
        "more than one aius_boot_teensy_watchdog_feed() call site"
    )
    assert re.search(r"\bwatchdog_feed\s*\(", clean) is None, (
        "the legacy local watchdog_feed() is still present"
    )
    assert re.search(r"\bwatchdog_init\s*\(", clean) is None, (
        "the legacy local watchdog_init() is still present"
    )
    # The ONE legitimate raw WDOG1_WSR writer is ota_wdt_feed(), the OTA
    # activation's RAM-resident watchdog feed: aius_boot_teensy_watchdog_feed() is
    # FLASHMEM and cannot run in the flash-erase dark window (its code is in the
    # flash being overwritten), and there is no epoch loop there to reach it, so
    # the sequencer feeds per primitive through this distinct RAM-resident writer.
    # Every OTHER raw WDOG1_WSR write must go through the boot adapter.
    ota_feed = without_comments_and_literals(
        function_definitions(text).get("ota_wdt_feed", "")
    )
    outside = clean.replace(ota_feed, "", 1) if ota_feed else clean
    assert "WDOG1_WSR" not in outside, (
        "a raw WDOG1_WSR register write is present outside ota_wdt_feed(); the "
        "feed must go through the boot adapter, not reimplement it here"
    )
    # Lives here, not in the positive group: a file-wide absence assertion is
    # only satisfiable once the legacy arming is deleted, which is S2b's job.
    # Asserting it positively forced S2a to gut a legacy function body it was
    # otherwise told not to touch.
    assert "WDOG_WCR_WT(" not in clean, (
        "a raw WDOG_WCR_WT(...) interval literal is present; the pinned 8 s "
        "interval must come from the boot adapter, not be reimplemented here"
    )


ABSENCE_CHECKS = (
    ("no static-address fallback path", assert_no_static_fallback),
    ("no raw FNET access", assert_no_raw_fnet_access),
    ("no listener-rebind globals", assert_no_rebind_globals),
    ("no blocking socket write loop", assert_no_blocking_socket_write_loop),
    (
        "no watchdog feed outside the single per-epoch one",
        assert_no_extra_watchdog_feed,
    ),
    ("no int16 playback ring or 16-bit drain", assert_no_int16_playback_ring),
)


# --------------------------------------------------------------------------
# Self-test: prove every helper and every check discriminates, against
# synthetic input, the way test_vendor_source.py proves its own rejection
# logic. This runs unconditionally, regardless of --group, because a check
# that can never fail is worse than no check.
# --------------------------------------------------------------------------

def _expect_pass(check, text, label):
    check(text)


def _expect_fail(check, text, label, needle=None):
    try:
        check(text)
    except AssertionError as error:
        if needle is not None:
            assert needle in str(error), f"{label}: wrong failure reason: {error}"
        return
    raise AssertionError(f"{label}: expected the check to reject synthetic input")


def run_helper_self_test():
    text = "int foo(void)\n{\n\tif (x) { return 1; }\n\treturn 0;\n}\n"
    body = function_body(text, "int foo(void)")
    assert "return 0;" in body and "int foo" not in body, "function_body extraction is broken"
    branch = conditional_body(body, "if (x)")
    assert "return 1;" in branch, "conditional_body extraction is broken"

    loop_text = (
        "void bar(void) {\n"
        "\tfor (int i = 0; i < 3; i++) { x += i; }\n"
        "\twhile (y) single_stmt();\n"
        "}\n"
    )
    bodies = loop_bodies(loop_text)
    assert len(bodies) == 2, f"expected 2 loop bodies, found {len(bodies)}"
    assert "x += i;" in bodies[0], "for-loop brace body extraction is broken"
    assert "single_stmt();" in bodies[1], "while single-statement body extraction is broken"

    headers = loop_headers(loop_text)
    assert headers == ["int i = 0; i < 3; i++", "y"], (
        f"loop_headers must return the raw conditions, found {headers}"
    )

    commented = without_comments(
        '\tconst char *f = "a // b"; /* gone */ // gone\n\tint x = 1;\n'
    )
    assert '"a // b"' in commented, "without_comments must keep string literals"
    assert "gone" not in commented, "without_comments must blank both comment forms"
    assert "int x = 1;" in commented, "without_comments must keep code"

    assert len(RECOVERY_JSON_KEYS) == len(RECOVERY_ID_KEYS) == 10, (
        "the recovery snapshot is ten fields, and the JSON and ID key tuples "
        "pair by index"
    )

    definition_text = (
        "static int prototype_only(void);\n"
        "static FLASHMEM bool one(void *ctx,\n"
        "                         int wide)\n"
        "{\n"
        "\tif (wide) { return false; }\n"
        "\treturn one_helper(ctx);\n"
        "}\n"
        "static void two(void) { call(); }\n"
    )
    definitions = function_definitions(definition_text)
    assert set(definitions) == {"one", "two"}, (
        f"function_definitions must find definitions only, found "
        f"{sorted(definitions)}"
    )
    assert "one_helper(ctx);" in definitions["one"], (
        "multi-line signature body extraction is broken"
    )
    assert "call();" in definitions["two"], "single-line body extraction is broken"

    qualified_text = (
        "FLASHMEM void Shim::set(bool on)\n{\n\tflag = on;\n}\n"
        "static void caller(void) { Shim::set(true); }\n"
        "/* Shim::set(false) in a comment */\n"
    )
    sites = qualified_call_sites(qualified_text, "Shim::set")
    assert len(sites) == 1, (
        f"qualified_call_sites must see the call and neither the out-of-line "
        f"definition nor the comment, found {len(sites)}"
    )
    assert qualified_text[sites[0]:].startswith("Shim::set(true)"), (
        "qualified_call_sites returned an offset into the wrong string"
    )


def run_audio_cutover_self_test():
    text = OPTICAL_MAIN.read_text()
    checks = (
        assert_audio_update_is_ingress_only,
        assert_audio_checkpoint_owns_service,
        assert_enqueue_preflights_before_effects,
        assert_audio_storage_and_diagnostics,
        assert_apply_config_uses_stream_rate,
        assert_rate_service_orders_safe_gate,
        assert_enqueue_refuses_rate_before_dsp,
        assert_clock_status_clear_and_episode_contract,
        assert_dsp_model_origin_is_staged_not_assumed,
        assert_recorder_reports_the_streams_rate,
    )
    for check in checks:
        _expect_pass(check, text, "audio-cutover/current")
    looped = text.replace(
        "\t\taudio_block_t *left = receiveReadOnly(0);",
        "\t\tfor (;;) {}\n\t\taudio_block_t *left = receiveReadOnly(0);",
        1,
    )
    _expect_fail(
        assert_audio_update_is_ingress_only,
        looped,
        "audio-cutover/loop-in-update",
        "contains a loop",
    )
    masked = text.replace(
        "\tconst uint32_t cycles_started = ARM_DWT_CYCCNT;",
        "\tnoInterrupts();\n\tconst uint32_t cycles_started = ARM_DWT_CYCCNT;",
        1,
    )
    _expect_fail(
        assert_audio_checkpoint_owns_service,
        masked,
        "audio-cutover/outer-mask",
        "masks the whole",
    )
    clock_status_unappended = text.replace(
        "if (!http_clock_json((char *)body, capacity, &length))",
        "if (!http_pipeline_diag_json((char *)body, capacity, &length))",
        1,
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        clock_status_unappended,
        "clock-status/unappended",
        "does not append exactly one",
    )
    clock_status_key_removed = text.replace(
        '"\\"carrier_locked\\":%s,"',
        '"\\"carrier_missing\\":%s,"',
        1,
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        clock_status_key_removed,
        "clock-status/key-removed",
        "field set or order changed",
    )
    clock_counter_not_cleared = text.replace(
        "\t\tg_clock_unsupported_episodes = 0u;\n", "", 1
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        clock_counter_not_cleared,
        "clock-status/not-cleared",
        "does not reset g_clock_unsupported_episodes",
    )
    trim_counter_not_cleared = text.replace(
        "\t\tg_clock_trim_updates = 0u;\n", "", 1
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        trim_counter_not_cleared,
        "clock-status/trim-not-cleared",
        "does not reset g_clock_trim_updates",
    )
    trim_counter_not_published = text.replace(
        "(unsigned long)g_clock_trim_updates,",
        "UINT32_C(0),",
        1,
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        trim_counter_not_published,
        "clock-status/trim-not-published",
        "does not publish g_clock_trim_updates",
    )
    fault_state_from_history = text.replace(
        "g_clock_fault_episode ? \"fault\" :",
        "g_clock_last_fault != 0u ? \"fault\" :",
        1,
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        fault_state_from_history,
        "clock-status/fault-from-clearable-history",
        "derives active fault truth from clearable fault history",
    )
    fault_cleared_by_target = text.replace(
        "\tg_clock_apply_attempted = false;\n",
        "\tg_clock_apply_attempted = false;\n"
        "\tg_clock_fault_episode = false;\n",
        1,
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        fault_cleared_by_target,
        "clock-status/fault-cleared-by-target",
        "selecting a new target clears",
    )
    stream_event_unguarded = text.replace(
        "if (!g_clock_stream_unsupported_episode)", "if (true)", 1
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        stream_event_unguarded,
        "clock-event/stream-unguarded",
        "missing conditional marker",
    )
    carrier_event_not_edge_guarded = text.replace(
        "if (result.unsupported_accepted)",
        "if (result.carrier_ready)",
        1,
    )
    _expect_fail(
        assert_clock_status_clear_and_episode_contract,
        carrier_event_not_edge_guarded,
        "clock-event/carrier-not-edge-guarded",
        "missing conditional marker",
    )
    global_reset_mask = text.replace(
        "static FLASHMEM void audio_enter_reset_critical(void *)\n"
        "{\n\tNVIC_DISABLE_IRQ(IRQ_SOFTWARE);\n}",
        "static FLASHMEM void audio_enter_reset_critical(void *)\n"
        "{\n\tnoInterrupts();\n}",
        1,
    )
    _expect_fail(
        assert_audio_checkpoint_owns_service,
        global_reset_mask,
        "audio-cutover/global-reset-mask",
        "must mask only",
    )
    reordered = text.replace(
        "\t\tconst bool model_pending = g_dsp_apply_pending;",
        "\t\t(void)aius_dsp_process(nullptr, nullptr, nullptr);\n"
        "\t\tconst bool model_pending = g_dsp_apply_pending;",
        1,
    )
    _expect_fail(
        assert_enqueue_preflights_before_effects,
        reordered,
        "audio-cutover/effect-before-config",
        "must validate",
    )
    model_rate = text.replace(
        "&g_dsp_runtime, &cfg, stream_rate_hz, &g_meter",
        "&g_dsp_runtime, &cfg, cfg.sample_rate_hz, &g_meter",
        1,
    )
    _expect_fail(
        assert_apply_config_uses_stream_rate,
        model_rate,
        "audio-cutover/model-rate",
        "accepted stream rate",
    )
    no_rate_service = text.replace("\tsample_rate_service(now_ms);\n", "", 1)
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        no_rate_service,
        "audio-cutover/rate-service-after-gate",
        "settle the source",
    )
    ingress_gate_removed = text.replace(
        "!g_clock_live_ingress_open",
        "false",
        1,
    )
    _expect_fail(
        assert_rate_acquisition_quiesces_ingress,
        ingress_gate_removed,
        "audio-cutover/acquisition-ingress-left-open",
        "discard live blocks",
    )
    source_after_rate = text.replace(
        "\treplay_source_service();\n"
        "\t/* Apply or verify the settled source's live plan or exact 48 kHz replay\n"
        "\t * plan before graph ownership can move. */\n"
        "\tsample_rate_service(now_ms);",
        "\tsample_rate_service(now_ms);\n"
        "\t/* Apply or verify the settled source's live plan or exact 48 kHz replay\n"
        "\t * plan before graph ownership can move. */\n"
        "\treplay_source_service();",
        1,
    )
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        source_after_rate,
        "audio-cutover/source-after-rate",
        "settle the source",
    )
    replay_without_ready = text.replace(
        "!AudioInputSPDIF3::pllLocked() && g_clock_replay_ready;",
        "!AudioInputSPDIF3::pllLocked();",
        1,
    )
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        replay_without_ready,
        "audio-cutover/replay-without-ready",
        "before its exact 48 kHz plan",
    )
    acceptance_reset = (
        "g_clock_relock_reset_nominal_hz !=\n"
        "\t\t\t\tresult.nominal_rate_hz &&\n"
        "\t\t\tg_audio_ingest_since_reset;"
    )
    assert text.count(acceptance_reset) == 1, "acceptance reset text moved"
    duplicate_rate_reset = text.replace(
        acceptance_reset,
        "g_clock_relock_reset_nominal_hz == 0u;",
        1,
    )
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        duplicate_rate_reset,
        "audio-cutover/double-rate-reset",
        "absent or bound to a different nominal",
    )
    first_acquisition_without_credit_reset = text.replace(
        acceptance_reset,
        "old_nominal != 0u &&\n"
        "\t\t\tg_clock_relock_reset_nominal_hz !=\n"
        "\t\t\t\tresult.nominal_rate_hz &&\n"
        "\t\t\tg_audio_ingest_since_reset;",
        1,
    )
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        first_acquisition_without_credit_reset,
        "audio-cutover/first-acquisition-reset-credit",
        "absent or bound to a different nominal",
    )
    # The redundant acceptance reset back (pre-(b) text).
    ungated_acceptance_reset = text.replace(
        acceptance_reset,
        "g_clock_relock_reset_nominal_hz !=\n"
        "\t\t\t\tresult.nominal_rate_hz;",
        1,
    )
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        ungated_acceptance_reset,
        "audio-cutover/acceptance-reset-without-ingest",
        "nothing was ingested",
    )
    never_ingested = text.replace("g_audio_ingest_since_reset = true;", "", 2)
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        never_ingested,
        "audio-cutover/ingest-never-recorded",
        "never records that words",
    )
    no_applied_gate = text.replace("!output_plan_ready", "false", 1)
    _expect_fail(
        assert_enqueue_refuses_rate_before_dsp,
        no_applied_gate,
        "audio-cutover/no-applied-rate-gate",
        "missing the supported/applied",
    )
    unpinned = text.replace(
        "static_assert(sizeof(g_audio_ingress_words) == 16384u,",
        "static_assert(sizeof(g_audio_ingress_words) > 0u,",
        1,
    )
    _expect_fail(
        assert_audio_storage_and_diagnostics,
        unpinned,
        "audio-cutover/unpinned-size",
        "exactly 16 KiB",
    )
    uncleared_diag = text.replace(
        "\tmemset(&g_replay_diag, 0, sizeof(g_replay_diag));\n",
        "",
        1,
    )
    _expect_fail(
        assert_noload_diagnostics_are_initialized,
        uncleared_diag,
        "audio-cutover/uncleared-noload-diag",
        "must clear",
    )
    # The regression itself: the swap deciding the origin instead of adopting
    # the one the stager staged.
    origin_assumed = text.replace(
        "\t\t\tg_dsp_model_origin = g_pending_dsp_origin;",
        "\t\t\tg_dsp_model_origin = DSP_ORIGIN_SD;",
        1,
    )
    _expect_fail(
        assert_dsp_model_origin_is_staged_not_assumed,
        origin_assumed,
        "dsp-origin/swap-assumes",
        "CONSTANT origin",
    )
    # Anchored on the two statements, not the block between them: d2d058c put
    # g_staged_generation++ and its comment in the middle of the old anchor.
    # The two-tab indent is what distinguishes the SD stager's apply-pending
    # arm from the three-tab PUT one.
    origin_after_flag = text.replace(
        "\t\tg_pending_dsp_origin = DSP_ORIGIN_SD;\n", "", 1
    ).replace(
        "\n\t\tg_dsp_apply_pending = true;",
        "\n\t\tg_dsp_apply_pending = true;"
        "\n\t\tg_pending_dsp_origin = DSP_ORIGIN_SD;",
        1,
    )
    _expect_fail(
        assert_dsp_model_origin_is_staged_not_assumed,
        origin_after_flag,
        "dsp-origin/staged-after-flag",
        "BEFORE the volatile apply",
    )
    # Both shipped values kept, a third bolted on: the plausible mistake, and
    # the one a bare presence check would miss.
    source_widened = text.replace(
        '\treturn origin == DSP_ORIGIN_DEFAULT ? "default" : "sd";',
        '\treturn origin == DSP_ORIGIN_DEFAULT ? "default"\n'
        '\t     : origin == DSP_ORIGIN_PUT ? "put" : "sd";',
        1,
    )
    _expect_fail(
        assert_dsp_model_origin_is_staged_not_assumed,
        source_widened,
        "dsp-origin/source-vocabulary-widened",
        "grew a third value",
    )
    origin_on_config_route = text.replace(
        '\t                      "{\\"source\\":\\"%s\\",'
        '\\"config_generation\\":%lu,',
        '\t                      "{\\"source\\":\\"%s\\",'
        '\\"model_origin\\":\\"put\\",\\"config_generation\\":%lu,',
        1,
    )
    _expect_fail(
        assert_dsp_model_origin_is_staged_not_assumed,
        origin_on_config_route,
        "dsp-origin/config-route-round-trip",
        "round-trip contract",
    )
    status_origin_dropped = text.replace(
        '\\"source\\":\\"%s\\",\\"model_origin\\":\\"%s\\",', '\\"source\\":\\"%s\\",', 1
    )
    _expect_fail(
        assert_dsp_model_origin_is_staged_not_assumed,
        status_origin_dropped,
        "dsp-origin/status-field-dropped",
        "does not carry `model_origin`",
    )
    # The rate argument now sits in recorder_slot_array, one line per field.
    recorder_nominal_rate = text.replace(
        "\t\t\t(unsigned long)s.sample_rate_hz,",
        "\t\t\t(unsigned long)AIUS_OPTICAL_SAMPLE_RATE_HZ,",
        1,
    )
    _expect_fail(
        assert_recorder_reports_the_streams_rate,
        recorder_nominal_rate,
        "recorder-rate/compile-time-nominal",
        "compile-time nominal",
    )
    recorder_tap_unstamped = text.replace(
        "\t\t\t\tblock->frames, block->sample_rate_hz,",
        "\t\t\t\tblock->frames,",
        1,
    )
    _expect_fail(
        assert_recorder_reports_the_streams_rate,
        recorder_tap_unstamped,
        "recorder-rate/tap-unstamped",
        "does not pass the block's own sample rate",
    )


def _synthetic_counter_edges():
    """A minimal file with the counter-edge shape: the allocated enum, one
    scalar static and one guarded emission per counter, the five transition
    events that predate them, an audio callback, and a loop() that reaches the
    service exactly once. Generated from EVENT_TYPE_VALUES so the fixture cannot
    drift from the contract the checks assert."""
    enum_body = "".join(
        f"\t{name} = {value},\n" for name, value in EVENT_TYPE_VALUES.items()
    )
    statics = "".join(
        f"static uint32_t g_prev_{index};\n"
        for index in range(len(COUNTER_EDGE_TYPES))
    )
    edges = "".join(
        f"\tdelta = counter_edge(source_{index}(), &g_prev_{index});\n"
        f"\tif (delta != 0u)\n"
        f"\t\trecord_event({name}, current_state(), delta);\n"
        for index, name in enumerate(COUNTER_EDGE_TYPES)
    )
    return (
        "enum AiusEventType : uint8_t {\n" + enum_body + "};\n"
        + statics
        + "static FLASHMEM void record_state(void)\n"
          "{\n"
          "\trecord_event(AIUS_EVENT_STATE, state, 0);\n"
          "}\n"
          "static FLASHMEM void app_set_test_running(void *, bool running)\n"
          "{\n"
          "\trecord_event(AIUS_EVENT_TEST_TONE, current_state(), running ? 1u : 0u);\n"
          "}\n"
          "static FLASHMEM void replay_clock_service(void)\n"
          "{\n"
          "\trecord_event(AIUS_EVENT_REPLAY_STOP, current_state(), REPLAY_STOP_CARRIER);\n"
          "}\n"
          "static FLASHMEM void sample_rate_service(void)\n"
          "{\n"
          "\trecord_event(AIUS_EVENT_RATE_CHANGE, current_state(), 48000u);\n"
          "\trecord_event(AIUS_EVENT_RATE_UNSUPPORTED, current_state(), 0u);\n"
          "\trecord_event(AIUS_EVENT_CLOCK_FAULT, current_state(), 1u);\n"
          "\trecord_event(AIUS_EVENT_UNDERRUN, current_state(), 1u);\n"
          "\trecord_event(AIUS_EVENT_GOVERNOR_TRIM, current_state(), 1u);\n"
          "\trecord_event(AIUS_EVENT_CLOCK_TRIM, current_state(), 1u);\n"
          "\trecord_event(AIUS_EVENT_TX_MISSED_FILL, current_state(), 1u);\n"
          "\trecord_event(AIUS_EVENT_AC3_CRC_REJECT, current_state(), 1u);\n"
          "}\n"
          "static FLASHMEM uint16_t counter_edge(uint32_t current, uint32_t *previous)\n"
          "{\n"
          "\tconst uint32_t delta = current - *previous;\n"
          "\t*previous = current;\n"
          "\tif (delta == 0u || delta > 0x7FFFFFFFu)\n"
          "\t\treturn 0u;\n"
          "\treturn delta > 0xFFFFu ? (uint16_t)0xFFFFu : (uint16_t)delta;\n"
          "}\n"
          "static FLASHMEM void service_event_edges(void)\n"
          "{\n"
          "\tuint16_t delta;\n"
        + edges
        + "}\n"
          "class AiusAudioProcessor : public AudioStream\n"
          "{\n"
          "\tvoid update(void) override\n"
          "\t{\n"
          "\t\temit_block();\n"
          "\t}\n"
          "};\n"
          "FLASHMEM void loop(void)\n"
          "{\n"
          "\taius_runtime_teensy_epoch(&g_runtime, millis());\n"
          "\tservice_event_edges();\n"
          "}\n"
    )


def run_self_test():
    # The dark-window interrupt-source park (2026-08-01 root-cause fix).
    good_park = (
        "FASTRUN static void ota_park_interrupt_sources(OtaIrqSnapshot *saved)\n"
        "{\n"
        "\tuint32_t primask;\n"
        '\t__asm__ volatile("mrs %0, primask" : "=r"(primask));\n'
        "\t__disable_irq();\n"
        "\tif (saved != NULL) {\n"
        "\t\tsaved->primask = primask;\n"
        "\t\tsaved->syst_csr = SYST_CSR;\n"
        "\t\tsaved->iser[0] = NVIC_ISER0;\n"
        "\t\tsaved->iser[1] = NVIC_ISER1;\n"
        "\t\tsaved->iser[2] = NVIC_ISER2;\n"
        "\t\tsaved->iser[3] = NVIC_ISER3;\n"
        "\t\tsaved->iser[4] = NVIC_ISER4;\n"
        "\t}\n"
        "\tNVIC_ICER0 = 0xFFFFFFFFu;\n"
        "\tNVIC_ICER1 = 0xFFFFFFFFu;\n"
        "\tNVIC_ICER2 = 0xFFFFFFFFu;\n"
        "\tNVIC_ICER3 = 0xFFFFFFFFu;\n"
        "\tNVIC_ICER4 = 0xFFFFFFFFu;\n"
        "\tSYST_CSR &= ~SYST_CSR_TICKINT;\n"
        "\tSCB_ICSR = SCB_ICSR_PENDSVCLR | SCB_ICSR_PENDSTCLR;\n"
        "}\n"
        "FASTRUN static void ota_restore_interrupt_sources(const OtaIrqSnapshot *saved)\n"
        "{\n"
        "\tNVIC_ISER0 = saved->iser[0];\n"
        "\tNVIC_ISER1 = saved->iser[1];\n"
        "\tNVIC_ISER2 = saved->iser[2];\n"
        "\tNVIC_ISER3 = saved->iser[3];\n"
        "\tNVIC_ISER4 = saved->iser[4];\n"
        "\tSYST_CSR = saved->syst_csr;\n"
        "\tif ((saved->primask & 1u) == 0u)\n"
        "\t\t__enable_irq();\n"
        "}\n"
    )
    _expect_pass(
        assert_dark_window_parks_every_interrupt_source, good_park, "park/good"
    )
    # One NVIC bank left live is one live source bank: the exact defect class.
    _expect_fail(
        assert_dark_window_parks_every_interrupt_source,
        good_park.replace("\tNVIC_ICER3 = 0xFFFFFFFFu;\n", "", 1),
        "park/missing-bank", "NVIC_ICER3",
    )
    # PRIMASK set after the bank writes leaves a taken-interrupt gap mid-park.
    _expect_fail(
        assert_dark_window_parks_every_interrupt_source,
        good_park.replace("\t__disable_irq();\n", "", 1).replace(
            "\tSCB_ICSR = SCB_ICSR_PENDSVCLR | SCB_ICSR_PENDSTCLR;\n",
            "\tSCB_ICSR = SCB_ICSR_PENDSVCLR | SCB_ICSR_PENDSTCLR;\n"
            "\t__disable_irq();\n",
            1,
        ),
        "park/primask-last", "out of order",
    )
    # SysTick is not NVIC: forgetting TICKINT leaves the 1 kHz source live.
    _expect_fail(
        assert_dark_window_parks_every_interrupt_source,
        good_park.replace("\tSYST_CSR &= ~SYST_CSR_TICKINT;\n", "", 1),
        "park/systick-live", "~SYST_CSR_TICKINT",
    )
    # A restore that forgets a bank strands the rehearsal's board half-deaf.
    _expect_fail(
        assert_dark_window_parks_every_interrupt_source,
        good_park.replace("\tNVIC_ISER2 = saved->iser[2];\n", "", 1),
        "park/restore-missing-bank", "NVIC_ISER2",
    )
    # No restore at all: the park would be one-way and rehearsal impossible.
    _expect_fail(
        assert_dark_window_parks_every_interrupt_source,
        good_park[: good_park.index("FASTRUN static void ota_restore")],
        "park/no-restore", "ota_restore_interrupt_sources",
    )

    # setup() construction-only.
    good_setup = (
        "void setup(void)\n{\n"
        "\taius_runtime_teensy_construct(&g_runtime, &g_app, &g_cdc, "
        "&g_platform, millis(), false);\n"
        "}\n"
    )
    _expect_pass(assert_setup_is_construction_only, good_setup, "setup/good")
    bad_setup_probe = good_setup.replace(
        "}\n", "\tg_sd_ready = SD.begin(BUILTIN_SDCARD);\n}\n", 1
    )
    _expect_fail(
        assert_setup_is_construction_only, bad_setup_probe, "setup/sd-probe",
        "SD.begin(",
    )
    bad_setup_loop = good_setup.replace("}\n", "\twhile (!Serial) {}\n}\n", 1)
    _expect_fail(assert_setup_is_construction_only, bad_setup_loop, "setup/loop")

    # loop() one-epoch delegation.
    good_loop = "void loop(void)\n{\n\taius_runtime_teensy_epoch(&g_runtime, millis());\n}\n"
    _expect_pass(assert_loop_delegates_one_epoch, good_loop, "loop/good")
    bad_loop_extra_call = good_loop.replace("}\n", "\tservice_sd();\n}\n", 1)
    _expect_fail(
        assert_loop_delegates_one_epoch, bad_loop_extra_call,
        "loop/extra-service", "service_sd(",
    )
    bad_loop_double_epoch = good_loop.replace(
        "aius_runtime_teensy_epoch(&g_runtime, millis());\n",
        "aius_runtime_teensy_epoch(&g_runtime, millis());\n"
        "\taius_runtime_teensy_epoch(&g_runtime, millis());\n",
        1,
    )
    _expect_fail(
        assert_loop_delegates_one_epoch, bad_loop_double_epoch, "loop/double-epoch"
    )

    # A requested reset waits for the drained transaction, bounded. The `bad`
    # variant below is the shape that shipped: measured on the board, two
    # attempts advanced boot_generation 21 -> 22 -> 23 and both timed out
    # client-side, because the reset landed at the end of the epoch that
    # dispatched -- before format_response() had run.
    good_reboot = (
        "static FASTRUN void reset_application(void)\n{\n"
        "\tSCB_AIRCR = 0x05FA0004;\n}\n"
        "static FLASHMEM bool reboot_drain_complete(void)\n{\n"
        "\treturn http->state == AIUS_HTTP_TX_IDLE &&\n"
        "\t       http->outstanding_action == AIUS_HTTP_ACTION_NONE;\n}\n"
        "static FLASHMEM void service_reboot_request(uint32_t now_ms)\n{\n"
        "\tif (!g_reboot_deadline_armed) {\n"
        "\t\tg_reboot_deadline_ms = now_ms + AIUS_REBOOT_DRAIN_MS;\n"
        "\t\tg_reboot_deadline_armed = true;\n"
        "\t\treturn;\n"
        "\t}\n"
        "\tif (reboot_drain_complete() ||\n"
        "\t    (int32_t)(now_ms - g_reboot_deadline_ms) >= 0)\n"
        "\t\treset_application();\n}\n"
        "static void ota_activate_run_and_finalize(void)\n{\n"
        "\tif (phase == AIUS_OTA_ACTIVATE_READY_TO_RESET)\n"
        "\t\treset_application();\n"
        "\tfor (;;)\n\t\t;\n}\n"
        # The third gated site: the rehearsal's terminal reset. Present here
        # only so the occurrence count this check pins is the real one.
        "static FLASHMEM void service_ota_rehearsal(uint32_t now_ms)\n{\n"
        "\tota_quiesce(&snapshot);\n"
        "\tota_rehearse_run();\n"
        "\tota_rehearsal_finish(OTA_REHEARSAL_PASS);\n"
        "\treset_application();\n}\n"
        "void loop(void)\n{\n"
        "\tconst uint32_t now_ms = millis();\n"
        "\tif (g_reboot_requested)\n"
        "\t\tservice_reboot_request(now_ms);\n}\n"
    )
    _expect_pass(
        assert_requested_reset_waits_for_the_drained_transaction,
        good_reboot, "reboot/good",
    )
    bad_reboot_immediate = good_reboot.replace(
        "\tif (g_reboot_requested)\n\t\tservice_reboot_request(now_ms);\n",
        "\tif (g_reboot_requested)\n\t\treset_application();\n",
        1,
    )
    _expect_fail(
        assert_requested_reset_waits_for_the_drained_transaction,
        bad_reboot_immediate, "reboot/reset-in-loop",
        "never answers it",
    )
    bad_reboot_unbounded = good_reboot.replace(
        "\t\tg_reboot_deadline_ms = now_ms + AIUS_REBOOT_DRAIN_MS;\n", "", 1
    )
    _expect_fail(
        assert_requested_reset_waits_for_the_drained_transaction,
        bad_reboot_unbounded, "reboot/unbounded", "must bound the wait",
    )
    bad_reboot_epoch_delay = good_reboot.replace(
        "\treturn http->state == AIUS_HTTP_TX_IDLE &&\n"
        "\t       http->outstanding_action == AIUS_HTTP_ACTION_NONE;\n",
        "\treturn g_reboot_epochs_waited >= 3u;\n",
        1,
    )
    _expect_fail(
        assert_requested_reset_waits_for_the_drained_transaction,
        bad_reboot_epoch_delay, "reboot/fixed-epoch-delay", "AIUS_HTTP_TX_IDLE",
    )

    # The rehearsal's own terminal reset. The `bad` variants below are the four
    # ways it goes wrong: no reset at all (the shipped shape, which rebooted by
    # watchdog ~8 s later with reset_source 16 / first_abort 9), a reset that
    # beats the verdict, a PRE-quiesce abort that resets a board that never
    # darkened, and dead code after a call that never returns.
    good_rehearsal = (
        "static FLASHMEM void service_ota_rehearsal(uint32_t now_ms)\n{\n"
        "\tif (aius_ota_activate_check_window(&g_ota_rehearsal_window) !=\n"
        "\t    AIUS_OTA_OK) {\n"
        "\t\tg_ota_activation_fault = OTA_ACTIVATION_FAULT_GEOMETRY;\n"
        "\t\tota_rehearsal_finish(OTA_REHEARSAL_FAIL);\n"
        "\t\treturn;\n"
        "\t}\n"
        "\tota_quiesce(&snapshot);\n"
        "\tota_rehearse_run();\n"
        "\tota_restore_interrupt_sources(&snapshot);\n"
        "\tota_rehearsal_finish(progress.phase ==\n"
        "\t\t\t\t     AIUS_OTA_ACTIVATE_READY_TO_RESET\n"
        "\t\t\t     ? OTA_REHEARSAL_PASS\n"
        "\t\t\t     : OTA_REHEARSAL_FAIL);\n"
        "\treset_application();\n}\n"
    )
    _expect_pass(
        assert_rehearsal_ends_in_a_deliberate_reset, good_rehearsal,
        "rehearsal/good",
    )
    bad_rehearsal_no_reset = good_rehearsal.replace(
        "\treset_application();\n", "", 1
    )
    _expect_fail(
        assert_rehearsal_ends_in_a_deliberate_reset, bad_rehearsal_no_reset,
        "rehearsal/emulated-reset", "found 0",
    )
    bad_rehearsal_reset_before_verdict = good_rehearsal.replace(
        "\tota_restore_interrupt_sources(&snapshot);\n",
        "\tota_restore_interrupt_sources(&snapshot);\n\treset_application();\n",
        1,
    ).replace("\treset_application();\n}\n", "}\n", 1)
    _expect_fail(
        assert_rehearsal_ends_in_a_deliberate_reset,
        bad_rehearsal_reset_before_verdict, "rehearsal/reset-before-verdict",
        "AFTER the last",
    )
    bad_rehearsal_pre_quiesce_reset = good_rehearsal.replace(
        "\t\tota_rehearsal_finish(OTA_REHEARSAL_FAIL);\n",
        "\t\tota_rehearsal_finish(OTA_REHEARSAL_FAIL);\n"
        "\t\treset_application();\n",
        1,
    )
    _expect_fail(
        assert_rehearsal_ends_in_a_deliberate_reset,
        bad_rehearsal_pre_quiesce_reset, "rehearsal/pre-quiesce-reset",
        "found 2",
    )
    bad_rehearsal_trailing_code = good_rehearsal.replace(
        "\treset_application();\n}\n",
        "\treset_application();\n\tg_ota_rehearsal_requested = false;\n}\n",
        1,
    )
    _expect_fail(
        assert_rehearsal_ends_in_a_deliberate_reset,
        bad_rehearsal_trailing_code, "rehearsal/dead-code-after-reset",
        "LAST statement",
    )

    # The boot fuse is served and never written. Raw string: the synthetic C
    # must keep the backslash escapes a JSON key carries inside a C literal.
    good_boot_fuse = r'''static FLASHMEM bool http_boot_fuse_json(char *output, size_t capacity,
	                                     size_t *length)
{
	const AiusBootFuse fuse = aius_boot_teensy_boot_fuse();

	return http_json_append(output, capacity, length,
	                        ",\"boot_fuse_0x6e0\":{\"word\":\"0x%08lx\","
	                        "\"read\":%s,\"sec_image_offset\":%u,"
	                        "\"sec_image_size\":%u}",
	                        (unsigned long)fuse.word,
	                        fuse.read ? "true" : "false",
	                        (unsigned)fuse.sec_image_offset,
	                        (unsigned)fuse.sec_image_size);
}
static FLASHMEM size_t http_status_json(uint8_t *body, size_t capacity)
{
	uint32_t mac0 = HW_OCOTP_MAC0;
	size_t length = mac0;

	if (!http_boot_fuse_json((char *)body, capacity, &length))
		return 0;
	return length;
}
'''
    _expect_pass(
        assert_boot_fuse_is_read_only_and_served, good_boot_fuse,
        "boot-fuse/good",
    )
    # The MAC read above is the reason the write scan stops at `;`: it puts
    # three OCOTP symbols on the right of an assignment and must stay legal.
    bad_fuse_write = good_boot_fuse.replace(
        "\tuint32_t mac0 = HW_OCOTP_MAC0;\n",
        "\tHW_OCOTP_MISC_CONF1 = 0u;\n\tuint32_t mac0 = 0u;\n",
        1,
    )
    _expect_fail(
        assert_boot_fuse_is_read_only_and_served, bad_fuse_write,
        "boot-fuse/write", "an OCOTP write is present",
    )
    bad_fuse_rmw = good_boot_fuse.replace(
        "\tuint32_t mac0 = HW_OCOTP_MAC0;\n",
        "\tHW_OCOTP_CTRL |= 1u;\n\tuint32_t mac0 = 0u;\n",
        1,
    )
    _expect_fail(
        assert_boot_fuse_is_read_only_and_served, bad_fuse_rmw,
        "boot-fuse/read-modify-write", "an OCOTP write is present",
    )
    bad_fuse_raw_address = good_boot_fuse.replace(
        "aius_boot_teensy_boot_fuse()",
        "aius_boot_teensy_decode_boot_fuse("
        "*(volatile uint32_t *)0x401F46E0)",
        1,
    )
    _expect_fail(
        assert_boot_fuse_is_read_only_and_served, bad_fuse_raw_address,
        "boot-fuse/raw-address", "raw OCOTP address literal",
    )
    bad_fuse_unserved = good_boot_fuse.replace(
        "\tif (!http_boot_fuse_json((char *)body, capacity, &length))\n"
        "\t\treturn 0;\n",
        "",
        1,
    )
    _expect_fail(
        assert_boot_fuse_is_read_only_and_served, bad_fuse_unserved,
        "boot-fuse/unserved", "never calls",
    )
    bad_fuse_no_read_flag = good_boot_fuse.replace(
        r'\"read\":%s,', "", 1
    )
    _expect_fail(
        assert_boot_fuse_is_read_only_and_served, bad_fuse_no_read_flag,
        "boot-fuse/no-read-flag", "missing keys",
    )
    bad_fuse_decoded_only = good_boot_fuse.replace(
        r'{\"word\":\"0x%08lx\",', "{", 1
    )
    _expect_fail(
        assert_boot_fuse_is_read_only_and_served, bad_fuse_decoded_only,
        "boot-fuse/no-raw-word", "missing keys",
    )

    # OTA seam composed under the option, after construction.
    good_ota = (
        "#if AIUS_OTA\n"
        "static FLASHMEM AiusOpticalAppOtaResult app_ota_arm(void *)\n{\n"
        "\treturn ota_map_result(aius_ota_arm_image(&g_ota_session));\n}\n"
        "static FLASHMEM bool http_ota_diag_json(char *o, size_t c, size_t *l)\n"
        "{\n\treturn http_json_append(o, c, l, \"ota_diag\");\n}\n"
        "static FLASHMEM void ota_compose(void)\n{\n"
        "\taius_ota_latch_arm(&g_ota_session);\n"
        "\taius_optical_app_set_ota(&g_runtime.app, &g_ota_seam);\n}\n"
        "#endif\n"
        "void setup(void)\n{\n"
        "\taius_runtime_teensy_construct(&g_runtime, &g_aius_optical_app, &cdc,\n"
        "\t                              &platform, millis(), g_recovery_safe);\n"
        "#if AIUS_OTA\n\tota_compose();\n#endif\n}\n"
    )
    _expect_pass(assert_ota_seam_composed_under_option, good_ota, "ota/good")
    _expect_fail(
        assert_ota_seam_composed_under_option,
        good_ota.replace("\taius_ota_latch_arm(&g_ota_session);\n", "", 1),
        "ota/no-latch-arm", "feature latch",
    )
    _expect_fail(
        assert_ota_seam_composed_under_option,
        good_ota.replace(
            "aius_ota_arm_image(&g_ota_session)",
            "aius_ota_latch_arm(&g_ota_session)", 1,
        ),
        "ota/arm-is-latch", "arm the image",
    )
    _expect_fail(
        assert_ota_seam_composed_under_option,
        good_ota.replace("#if AIUS_OTA\n\tota_compose();\n#endif\n", "", 1),
        "ota/no-compose-call", "call ota_compose()",
    )
    _expect_fail(
        assert_ota_seam_composed_under_option,
        good_ota.replace(
            "\taius_runtime_teensy_construct(&g_runtime, &g_aius_optical_app, "
            "&cdc,\n"
            "\t                              &platform, millis(), "
            "g_recovery_safe);\n"
            "#if AIUS_OTA\n\tota_compose();\n#endif\n",
            "#if AIUS_OTA\n\tota_compose();\n#endif\n"
            "\taius_runtime_teensy_construct(&g_runtime, &g_aius_optical_app, "
            "&cdc,\n"
            "\t                              &platform, millis(), "
            "g_recovery_safe);\n",
            1,
        ),
        "ota/compose-before-construct", "AFTER",
    )

    # watchdog arm-once / feed-once-per-epoch.
    good_watchdog = (
        "void setup(void)\n{\n\taius_boot_teensy_watchdog_arm(&primitives);\n}\n"
        "void loop(void)\n{\n\taius_boot_teensy_watchdog_feed(&primitives);\n}\n"
    )
    _expect_pass(
        assert_watchdog_armed_once_fed_once_per_epoch, good_watchdog, "watchdog/good"
    )
    bad_watchdog_extra_feed = good_watchdog + (
        "void other(void) { aius_boot_teensy_watchdog_feed(&primitives); }\n"
    )
    _expect_fail(
        assert_watchdog_armed_once_fed_once_per_epoch, bad_watchdog_extra_feed,
        "watchdog/extra-feed",
    )
    bad_watchdog_feed_in_setup = (
        "void setup(void)\n{\n\taius_boot_teensy_watchdog_arm(&primitives);\n"
        "\taius_boot_teensy_watchdog_feed(&primitives);\n}\n"
        "void loop(void)\n{\n}\n"
    )
    _expect_fail(
        assert_watchdog_armed_once_fed_once_per_epoch, bad_watchdog_feed_in_setup,
        "watchdog/feed-in-setup", "must not happen in setup",
    )

    # UDP status carries the 99-byte recovery payload. Raw strings throughout
    # this block: the synthetic C must keep its own backslash escapes.
    good_recovery_producer = r'''static FLASHMEM uint16_t put_recovery_status(uint8_t *payload)
{
	if (aius_net_status_encode(payload, AIUS_NET_MAX_PAYLOAD, legacy,
	                           &recovery) == 99u)
		return 99u;
	memcpy(payload, legacy, legacy_length);
	return legacy_length;
}
'''
    good_udp_dispatch = r'''static FLASHMEM void udp_dispatch(const AiusNetFrame *request)
{
	switch (request->type) {
	case AIUS_NET_CMD_DISCOVER:
	case AIUS_NET_CMD_GET_STATUS:
		length = put_recovery_status(g_udp_payload);
		break;
	}
}
'''
    _expect_pass(
        assert_udp_status_carries_recovery_payload,
        good_udp_dispatch + good_recovery_producer,
        "udp-recovery/good",
    )
    legacy_only_dispatch = good_udp_dispatch.replace(
        "put_recovery_status(g_udp_payload)", "put_status(g_udp_payload, &length)", 1
    )
    _expect_fail(
        assert_udp_status_carries_recovery_payload,
        legacy_only_dispatch + good_recovery_producer,
        "udp-recovery/legacy-only", "put_recovery_status()",
    )
    mixed_dispatch = good_udp_dispatch.replace(
        "\t\tlength = put_recovery_status(g_udp_payload);\n",
        "\t\tlength = put_recovery_status(g_udp_payload);\n"
        "\t\tput_status(g_udp_payload, &length);\n",
        1,
    )
    _expect_fail(
        assert_udp_status_carries_recovery_payload,
        mixed_dispatch + good_recovery_producer,
        "udp-recovery/one-legacy-arm", "calls put_status() directly",
    )
    unencoded_producer = r'''static FLASHMEM uint16_t put_recovery_status(uint8_t *payload)
{
	memcpy(payload, legacy, legacy_length);
	return legacy_length;
}
'''
    _expect_fail(
        assert_udp_status_carries_recovery_payload,
        good_udp_dispatch + unencoded_producer,
        "udp-recovery/no-encoder", "aius_net_status_encode()",
    )
    unlengthed_producer = r'''static FLASHMEM uint16_t put_recovery_status(uint8_t *payload)
{
	if (aius_net_status_encode(payload, AIUS_NET_MAX_PAYLOAD, legacy,
	                           &recovery) == 71u)
		return 71u;
	return legacy_length;
}
'''
    _expect_fail(
        assert_udp_status_carries_recovery_payload,
        good_udp_dispatch + unlengthed_producer,
        "udp-recovery/legacy-length", "no 99-byte recovery length",
    )

    # status JSON carries all ten recovery keys.
    good_status_json = r'''static FLASHMEM size_t http_status_json(uint8_t *body, size_t capacity)
{
	int written = snprintf((char *)body, capacity,
		"{\"ip\":\"%u.%u.%u.%u\",\"network_restarts\":%lu,\"reset_source\":%lu,"
		"\"network_state\":%u,\"recovery_safe\":%u,\"network_last_fault\":%u,"
		"\"last_checkpoint\":%u,\"network_generation\":%lu,"
		"\"boot_generation\":%lu,\"watchdog_early_resets\":%u,"
		"\"loop_epoch\":%lu}",
		address[0]);
	return written > 0 ? (size_t)written : 0u;
}
'''
    _expect_pass(
        assert_status_json_carries_recovery_keys, good_status_json, "status-json/good"
    )
    status_json_missing_key = good_status_json.replace(
        r'"\"loop_epoch\":%lu}",', r'"}",', 1
    )
    _expect_fail(
        assert_status_json_carries_recovery_keys, status_json_missing_key,
        "status-json/missing-key", "loop_epoch",
    )
    status_json_comment_only = status_json_missing_key.replace(
        "\treturn written", "\t/* loop_epoch is served elsewhere */\n\treturn written", 1
    )
    _expect_fail(
        assert_status_json_carries_recovery_keys, status_json_comment_only,
        "status-json/comment-is-not-a-field", "loop_epoch",
    )

    # USB ID carries all ten recovery keys.
    good_usb_id = r'''static FLASHMEM size_t app_write_usb_payload(void *, AiusUsbCommandKind kind,
                                             char *payload, size_t capacity)
{
	int written = snprintf(payload, capacity,
		"protocol=1 revision=%s build=%s mode=optical ip=%u.%u.%u.%u rsafe=%u nstate=%u nfault=%u ckpt=%u early=%u ngen=%lu bgen=%lu rsrc=%lu epoch=%lu nrst=%lu",
		AIUS_BUILD_GIT_REV, AIUS_BUILD_UTC);
	return written > 0 ? (size_t)written : 0u;
}
'''
    _expect_pass(assert_usb_id_carries_recovery_keys, good_usb_id, "usb-id/good")
    usb_id_missing_key = good_usb_id.replace(" nrst=%lu", "", 1)
    _expect_fail(
        assert_usb_id_carries_recovery_keys, usb_id_missing_key,
        "usb-id/missing-key", "nrst",
    )
    usb_id_comment_only = usb_id_missing_key.replace(
        "\treturn written", "\t/* nrst= is on GET /api/status */\n\treturn written", 1
    )
    _expect_fail(
        assert_usb_id_carries_recovery_keys, usb_id_comment_only,
        "usb-id/comment-is-not-a-field", "nrst",
    )

    # the replay path performs no direct SD primitive.
    good_replay = (
        "static FLASHMEM void replay_refill_service(size_t budget, uint32_t now)\n"
        "{\n"
        "\tAiusReplayRefill refill;\n"
        "\tif (!aius_replay_service_plan_refill(&g_replay_service, budget, &refill))\n"
        "\t\treturn;\n"
        "\trequest.kind = AIUS_SD_JOB_REPLAY_REFILL;\n"
        "\taius_sd_job_start(&g_replay_job, &request, now, 0u);\n"
        "}\n"
        "static FLASHMEM bool platform_replay(void *, const AiusRuntimeBudget *b,\n"
        "                                     uint32_t now)\n"
        "{\n"
        "\treplay_refill_service(b->max_rx_bytes, now);\n"
        "\treturn true;\n"
        "}\n"
    )
    _expect_pass(assert_replay_path_has_no_direct_sd, good_replay, "replay-sd/good")

    # The pre-14c shape: a reader that owns a File and reads it inline. This is
    # the regression the check exists to prevent, so it must fail on exactly it.
    legacy_reader = good_replay + (
        "static FLASHMEM AiusStatus replay_reader_read(void *, uint32_t offset,\n"
        "                                             uint8_t *buffer, size_t cap)\n"
        "{\n"
        "\tif (!g_replay_file.seek(offset)) return AIUS_MALFORMED;\n"
        "\tint count = g_replay_file.read(buffer, cap);\n"
        "\treturn count < 0 ? AIUS_MALFORMED : AIUS_OK;\n"
        "}\n"
    )
    _expect_fail(
        assert_replay_path_has_no_direct_sd, legacy_reader, "replay-sd/legacy-reader",
        "replay_reader_read",
    )
    mounted_in_callback = good_replay.replace(
        "\treplay_refill_service(b->max_rx_bytes, now);\n",
        "\tg_replay_file = SD.open(g_replay_path, FILE_READ);\n",
        1,
    )
    _expect_fail(
        assert_replay_path_has_no_direct_sd, mounted_in_callback,
        "replay-sd/callback-opens", "platform_replay",
    )
    # Vacuous-pass guard: a replay path with no card access at all, and no
    # bounded job either, is not a pass.
    no_job = good_replay.replace(
        "\trequest.kind = AIUS_SD_JOB_REPLAY_REFILL;\n", "", 1
    )
    _expect_fail(
        assert_replay_path_has_no_direct_sd, no_job, "replay-sd/no-bounded-job",
        "AIUS_SD_JOB_REPLAY_REFILL",
    )
    # And a check that scans nothing is not a pass either.
    _expect_fail(
        assert_replay_path_has_no_direct_sd,
        "static void unrelated(void) { return; }\n",
        "replay-sd/nothing-scanned", "platform_replay",
    )
    # The two named carve-outs really are exempt, and only they are.
    exempt_delete = good_replay + (
        "static FLASHMEM AiusOpticalAppReplayResult app_replay_delete(void *,\n"
        "                                                             const char *id)\n"
        "{\n"
        "\tif (!SD.exists(path)) return AIUS_OPTICAL_APP_REPLAY_NOT_FOUND;\n"
        "\treturn SD.remove(path) ? AIUS_OPTICAL_APP_REPLAY_OK : AIUS_OPTICAL_APP_REPLAY_FAILED;\n"
        "}\n"
    )
    _expect_pass(
        assert_replay_path_has_no_direct_sd, exempt_delete, "replay-sd/exempt"
    )

    # the replay sizing agrees with the SD engine's transfer cap.
    #
    # The synthetic sources stand in for the four real files, in the shape they
    # have on disk: the engine's private cap, the replay seam's constants, the
    # storage slice, the buffer declaration, and the two functions that must not
    # read the runtime's transport budget.
    def sizing_sources(engine_cap=512, deliver=512, ring=65536, watermark=32768,
                       refill=32768, slice_name="AIUS_RUNTIME_STORAGE_TRANSFER",
                       storage_transfer=512, budget_field=""):
        return (
            f"#define {SD_TRANSFER_CAP} {engine_cap}u\n"
            f"#define AIUS_RUNTIME_STORAGE_TRANSFER {storage_transfer}u\n"
            f"#define AIUS_REPLAY_WORD_RING_CAPACITY {ring}u\n"
            f"#define AIUS_REPLAY_START_WATERMARK_WORDS {watermark}u\n"
            f"#define AIUS_REPLAY_REFILL_MAX_BYTES {refill}u\n"
            f"#define AIUS_REPLAY_DELIVER_MAX_BYTES {deliver}u\n"
            f"DMAMEM static uint8_t g_replay_transfer[{slice_name}];\n"
            "static FLASHMEM void replay_refill_service(uint32_t now_ms)\n"
            "{\n"
            f"\tAiusReplayRefill refill;{budget_field}\n"
            "\tif (!aius_replay_service_plan_refill(&g_replay_service,\n"
            "\t                                     "
            "AIUS_REPLAY_REFILL_MAX_BYTES / 2u, &refill))\n"
            "\t\treturn;\n"
            "}\n"
            "static FLASHMEM bool platform_replay(void *ctx, int budget,\n"
            "                                     uint32_t now_ms)\n"
            "{\n"
            "\treplay_refill_service(now_ms);\n"
            "\treturn true;\n"
            "}\n"
        )

    _expect_pass(
        assert_replay_sizing_matches_the_sd_engine, sizing_sources(),
        "sizing/good",
    )
    # THE FLASHED REGRESSION, in its exact shape: an 8 KB transfer slice against
    # the engine's 512-byte cap. Every refill job is refused at validation and
    # the replay produces nothing.
    _expect_fail(
        assert_replay_sizing_matches_the_sd_engine,
        sizing_sources(slice_name="AIUS_REPLAY_REFILL_MAX_BYTES", refill=8192,
                       deliver=8192, ring=16384, watermark=8192),
        "sizing/slice-exceeds-engine-cap", "AIUS_SD_FAULT_CAPACITY",
    )
    # A slice the service could not accept: a run-time NO_SPACE per chunk.
    _expect_fail(
        assert_replay_sizing_matches_the_sd_engine,
        sizing_sources(deliver=256),
        "sizing/delivery-smaller-than-slice", "refuse every chunk",
    )
    # No room above the watermark for a full plan: every refill silently shrinks.
    _expect_fail(
        assert_replay_sizing_matches_the_sd_engine,
        sizing_sources(watermark=57344),
        "sizing/plan-cannot-be-full-size", "silently shrunk",
    )
    # A modulo per sample on the audio pop path.
    _expect_fail(
        assert_replay_sizing_matches_the_sd_engine,
        sizing_sources(ring=48000, watermark=16384),
        "sizing/ring-not-power-of-two", "power of two",
    )
    # The unit bug: a transport receive quota gating a disk refill.
    _expect_fail(
        assert_replay_sizing_matches_the_sd_engine,
        sizing_sources(budget_field="\n\tsize_t words = budget->max_rx_bytes;"),
        "sizing/refill-reads-transport-budget", RUNTIME_RX_BUDGET_FIELD,
    )
    # Vacuous-pass guards: a missing constant and a missing declaration.
    _expect_fail(
        assert_replay_sizing_matches_the_sd_engine,
        sizing_sources().replace(f"#define {SD_TRANSFER_CAP} 512u\n", "", 1),
        "sizing/no-engine-cap", SD_TRANSFER_CAP,
    )
    _expect_fail(
        assert_replay_sizing_matches_the_sd_engine,
        sizing_sources().replace(
            "DMAMEM static uint8_t g_replay_transfer"
            "[AIUS_RUNTIME_STORAGE_TRANSFER];\n",
            "",
            1,
        ),
        "sizing/no-transfer-buffer", "g_replay_transfer",
    )
    # A comment naming the cap is not the cap.
    _expect_fail(
        assert_replay_sizing_matches_the_sd_engine,
        sizing_sources(
            budget_field="\n\t/* bounded by max_rx_bytes */\n"
                         "\tsize_t words = budget->max_rx_bytes;"
        ),
        "sizing/budget-read-not-hidden-by-a-comment", RUNTIME_RX_BUDGET_FIELD,
    )

    # the exempt replay card walks are bounded and report truncation.
    good_walks = (
        "static FLASHMEM size_t http_replay_list_json(uint8_t *body, size_t cap)\n"
        "{\n"
        "\tunsigned scanned = 0u;\n"
        "\tbool cut = false;\n"
        "\twhile (scanned < AIUS_OPTICAL_APP_INDEX_SCAN_MAX) {\n"
        "\t\tFile entry = directory.openNextFile();\n"
        "\t\tif (!entry) break;\n"
        "\t\tscanned++;\n"
        "\t\tentry.close();\n"
        "\t}\n"
        "\tdirectory.close();\n"
        "\twritten = snprintf((char *)body + length, cap - length,\n"
        "\t                   \"],\\\"truncated\\\":%s}\", cut ? \"true\" : \"false\");\n"
        "\treturn length + (size_t)written;\n"
        "}\n"
        "static FLASHMEM AiusOpticalAppReplayResult app_replay_delete(void *,\n"
        "                                                             const char *id)\n"
        "{\n"
        "\tif (!SD.exists(path)) return AIUS_OPTICAL_APP_REPLAY_NOT_FOUND;\n"
        "\treturn SD.remove(path) ? AIUS_OPTICAL_APP_REPLAY_OK :\n"
        "\t                         AIUS_OPTICAL_APP_REPLAY_FAILED;\n"
        "}\n"
    )
    _expect_pass(assert_replay_card_walks_are_bounded, good_walks, "walks/good")

    # THE DEFECT ITSELF: the pre-fix walk, verbatim in shape. This is the
    # negative control the bound exists for, so it must fail on exactly it.
    walk_unbounded = good_walks.replace(
        "\twhile (scanned < AIUS_OPTICAL_APP_INDEX_SCAN_MAX) {\n",
        "\tfor (;;) {\n",
        1,
    )
    _expect_fail(
        assert_replay_card_walks_are_bounded, walk_unbounded,
        "walks/unbounded", REPLAY_WALK_SCAN_CAP,
    )
    # Bounded but silent about it: a short listing that reads as a complete one.
    walk_no_truncation = good_walks.replace(r"\"truncated\"", r"\"ok\"", 1)
    _expect_fail(
        assert_replay_card_walks_are_bounded, walk_no_truncation,
        "walks/silent-truncation", "truncated",
    )
    # A cap named only in a comment is not a cap.
    walk_comment_cap = good_walks.replace(
        "\twhile (scanned < AIUS_OPTICAL_APP_INDEX_SCAN_MAX) {\n",
        "\t/* capped at AIUS_OPTICAL_APP_INDEX_SCAN_MAX */\n\twhile (true) {\n",
        1,
    )
    _expect_fail(
        assert_replay_card_walks_are_bounded, walk_comment_cap,
        "walks/comment-is-not-a-cap", REPLAY_WALK_SCAN_CAP,
    )
    # Delete is bounded by being a constant; a loop there is a new scan.
    walk_delete_loop = good_walks.replace(
        "\tif (!SD.exists(path)) return AIUS_OPTICAL_APP_REPLAY_NOT_FOUND;\n",
        "\tfor (;;) { if (!SD.exists(path)) break; }\n",
        1,
    )
    _expect_fail(
        assert_replay_card_walks_are_bounded, walk_delete_loop,
        "walks/delete-grew-a-loop", REPLAY_DELETE_FUNCTION,
    )
    # Vacuous-pass guards: neither function present, and a listing with no walk.
    _expect_fail(
        assert_replay_card_walks_are_bounded,
        "static void unrelated(void) { return; }\n",
        "walks/nothing-scanned", REPLAY_LIST_FUNCTION,
    )
    walk_no_loop = (
        "static FLASHMEM size_t http_replay_list_json(uint8_t *body, size_t cap)\n"
        "{\n\treturn 0u;\n}\n"
        "static FLASHMEM AiusOpticalAppReplayResult app_replay_delete(void *,\n"
        "                                                             const char *id)\n"
        "{\n\treturn AIUS_OPTICAL_APP_REPLAY_OK;\n}\n"
    )
    _expect_fail(
        assert_replay_card_walks_are_bounded, walk_no_loop,
        "walks/no-loop-at-all", "no loop at all",
    )

    # the clock handover exists and is bounded to the replay path.
    good_clock = (
        "FLASHMEM void AiusSpdifInputClock::set_master(bool master)\n"
        "{\n\tupdate_responsibility = master;\n}\n"
        "FLASHMEM void AiusSpdifOutputClock::set_master(bool master)\n"
        "{\n\tupdate_responsibility = master;\n}\n"
        "static FLASHMEM void replay_source_service(void)\n"
        "{\n"
        "\tif ((g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY ||\n"
        "\t     g_replay_service.source == AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY) &&\n"
        "\t    AudioInputSPDIF3::pllLocked()) {\n"
        "\t\treplay_job_cancel();\n"
        "\t\taius_replay_service_stop(&g_replay_service);\n"
        "\t\tg_replay_stop_reason = REPLAY_STOP_CARRIER;\n"
        "\t\trecord_event(3, current_state(), REPLAY_STOP_CARRIER);\n"
        "\t}\n"
        "}\n"
        "static FLASHMEM void replay_clock_service(void)\n"
        "{\n"
        "\tconst bool output_drives =\n"
        "\t\tg_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY &&\n"
        "\t\t!AudioInputSPDIF3::pllLocked() && g_clock_replay_ready;\n"
        "\tif (output_drives == AiusSpdifOutputClock::is_master())\n"
        "\t\treturn;\n"
        "\tnoInterrupts();\n"
        "\tAiusSpdifOutputClock::set_master(output_drives);\n"
        "\tAiusSpdifInputClock::set_master(!output_drives);\n"
        "\tinterrupts();\n"
        "}\n"
        "static FLASHMEM bool platform_replay(void *, const AiusRuntimeBudget *budget,\n"
        "                                     uint32_t now_ms)\n"
        "{\n"
        "\treplay_refill_service(budget->max_rx_bytes, now_ms);\n"
        "\treplay_source_service();\n"
        "\treplay_clock_service();\n"
        "\treturn true;\n"
        "}\n"
    )
    _expect_pass(
        assert_replay_clock_handover_is_bounded, good_clock, "clock/good"
    )
    # Unbounded: the handover no longer keys on the replay source, so it could
    # move the clock for a reason the audio path knows nothing about. Replaced
    # file-wide, not once -- the reconciler reads the source twice and either
    # occurrence alone would satisfy a naive presence check.
    clock_unbounded = good_clock.replace(
        "AIUS_REPLAY_SOURCE_REPLAY", "g_replay_armed"
    )
    _expect_fail(
        assert_replay_clock_handover_is_bounded, clock_unbounded,
        "clock/unbounded", "AIUS_REPLAY_SOURCE_REPLAY",
    )
    # Would take the clock away from a live carrier, and would never displace a
    # replay when one arrived.
    clock_unconditional = good_clock.replace("AudioInputSPDIF3::pllLocked()", "false")
    _expect_fail(
        assert_replay_clock_handover_is_bounded, clock_unconditional,
        "clock/ignores-lock", "pllLocked()",
    )
    # Moves the clock but leaves the replay running: a live carrier and a replay
    # both holding the audio path, which the invariant forbids.
    clock_keeps_replay = good_clock.replace(
        "\t\taius_replay_service_stop(&g_replay_service);\n", "", 1
    )
    _expect_fail(
        assert_replay_clock_handover_is_bounded, clock_keeps_replay,
        "clock/carrier-does-not-stop-replay", "never stops the replay",
    )
    # Silent: the replay ends by itself and nothing says why.
    clock_no_event = good_clock.replace(
        "\t\trecord_event(3, current_state(), REPLAY_STOP_CARRIER);\n", "", 1
    )
    _expect_fail(
        assert_replay_clock_handover_is_bounded, clock_no_event,
        "clock/unrecorded-transition", "event ring",
    )
    # Half a handover: both peripherals end up driving update_all().
    clock_one_sided = good_clock.replace(
        "\tAiusSpdifInputClock::set_master(!output_drives);\n", "", 1
    )
    _expect_fail(
        assert_replay_clock_handover_is_bounded, clock_one_sided,
        "clock/one-sided", "AiusSpdifInputClock::set_master",
    )
    # Torn: a DMA ISR can land between the two stores.
    clock_unguarded = good_clock.replace("\tnoInterrupts();\n", "", 1)
    _expect_fail(
        assert_replay_clock_handover_is_bounded, clock_unguarded,
        "clock/no-critical-section", "critical section",
    )
    # A second call site outside the reconciler -- the widening this check
    # exists to refuse.
    clock_second_site = good_clock + (
        "static FLASHMEM AiusOpticalAppReplayResult app_replay_start(void *,\n"
        "                                                            const char *id)\n"
        "{\n\tAiusSpdifOutputClock::set_master(true);\n\treturn ok;\n}\n"
    )
    _expect_fail(
        assert_replay_clock_handover_is_bounded, clock_second_site,
        "clock/second-call-site", "call sites in the file",
    )
    # Dead code: present, correct, and never reached.
    clock_unreached = good_clock.replace("\treplay_clock_service();\n", "", 1)
    _expect_fail(
        assert_replay_clock_handover_is_bounded, clock_unreached,
        "clock/never-called", "before replay_clock_service()",
    )

    # counter edges: allocated types, epoch-loop emission, one per epoch.
    good_edges = _synthetic_counter_edges()
    _expect_pass(
        assert_event_types_are_allocated_not_renumbered, good_edges, "types/good"
    )
    _expect_pass(
        assert_counter_edges_are_epoch_loop_bound, good_edges, "edges/good"
    )
    # The wire contract broken the way it actually gets broken: a value reused
    # for a new meaning, and an existing one shifted to make room.
    types_renumbered = good_edges.replace(
        "AIUS_EVENT_OVERRUN = 4,", "AIUS_EVENT_OVERRUN = 11,", 1
    )
    _expect_fail(
        assert_event_types_are_allocated_not_renumbered, types_renumbered,
        "types/renumbered", "never renumbered or reused",
    )
    types_collided = good_edges.replace(
        "AIUS_EVENT_NETWORK_DROP = 5,", "AIUS_EVENT_NETWORK_DROP = 4,", 1
    )
    _expect_fail(
        assert_event_types_are_allocated_not_renumbered, types_collided,
        "types/collision", "never renumbered or reused",
    )
    # Dead code: the service exists, is correct, and nothing reaches it.
    edges_unreached = good_edges.replace("\tservice_event_edges();\n", "", 1)
    _expect_fail(
        assert_counter_edges_are_epoch_loop_bound, edges_unreached,
        "edges/never-called", "exactly once per epoch",
    )
    # A second call site: two passes over the counters in one epoch.
    edges_second_site = good_edges.replace(
        "\taius_runtime_teensy_epoch(&g_runtime, millis());\n",
        "\taius_runtime_teensy_epoch(&g_runtime, millis());\n"
        "\tservice_event_edges();\n",
        1,
    )
    _expect_fail(
        assert_counter_edges_are_epoch_loop_bound, edges_second_site,
        "edges/second-call-site", "exactly once per epoch",
    )
    # Cold code without FLASHMEM on the definition: .text out of the one object
    # the ITCM allowlist pins.
    edges_no_flashmem = good_edges.replace(
        "static FLASHMEM void service_event_edges(void)",
        "static void service_event_edges(void)",
        1,
    )
    _expect_fail(
        assert_counter_edges_are_epoch_loop_bound, edges_no_flashmem,
        "edges/no-flashmem", "no FLASHMEM on its DEFINITION",
    )
    # One event per increment rather than one per epoch.
    edges_per_increment = good_edges.replace(
        "\tif (delta != 0u)\n"
        "\t\trecord_event(AIUS_EVENT_OVERRUN, current_state(), delta);\n",
        "\tfor (uint16_t i = 0; i < delta; i++)\n"
        "\t\trecord_event(AIUS_EVENT_OVERRUN, current_state(), 1u);\n",
        1,
    )
    _expect_fail(
        assert_counter_edges_are_epoch_loop_bound, edges_per_increment,
        "edges/per-increment", "contains a loop",
    )
    # An emission nothing guards: every epoch, forever.
    edges_unguarded = good_edges.replace(
        "\tif (delta != 0u)\n"
        "\t\trecord_event(AIUS_EVENT_OVERRUN, current_state(), delta);\n",
        "\trecord_event(AIUS_EVENT_OVERRUN, current_state(), delta);\n",
        1,
    )
    _expect_fail(
        assert_counter_edges_are_epoch_loop_bound, edges_unguarded,
        "edges/unguarded", "an unguarded record_event()",
    )
    # The emission migrated into the audio callback.
    edges_in_isr = good_edges.replace(
        "\t\temit_block();\n",
        "\t\temit_block();\n"
        "\t\trecord_event(AIUS_EVENT_OVERRUN, current_state(), 1u);\n",
        1,
    )
    _expect_fail(
        assert_counter_edges_are_epoch_loop_bound, edges_in_isr,
        "edges/audio-callback", "the audio callback contains",
    )
    # Edge state as an array: DTCM out of the free-for-locals zone.
    edges_array_state = good_edges.replace(
        "static uint32_t g_prev_0;\n", "static uint32_t g_prev_all[8];\n", 1
    )
    _expect_fail(
        assert_counter_edges_are_epoch_loop_bound, edges_array_state,
        "edges/array-state", "not a plain scalar",
    )
    # A resync only on the emitting path: a cleared counter would then re-report
    # the same edge every epoch.
    edges_conditional_resync = good_edges.replace(
        "\t*previous = current;\n\tif (delta == 0u",
        "\tif (delta == 0u",
        1,
    ).replace("\t\treturn 0u;\n", "\t\treturn 0u;\n\t*previous = current;\n", 1)
    _expect_fail(
        assert_counter_edges_are_epoch_loop_bound, edges_conditional_resync,
        "edges/conditional-resync", "resynchronise unconditionally",
    )

    # no static fallback.
    _expect_pass(
        assert_no_static_fallback, "static IPAddress g_dynamic_ip;\n", "static/good"
    )
    _expect_fail(
        assert_no_static_fallback,
        "static IPAddress g_static_ip(192, 168, 42, 42);\n",
        "static/bad", "g_static_ip",
    )

    # no raw FNET access.
    _expect_pass(
        assert_no_raw_fnet_access,
        "aius_network_teensy_poll(&adapter, now, &event);\n",
        "fnet/good",
    )
    _expect_fail(
        assert_no_raw_fnet_access,
        "fnet_netif_desc_t netif = fnet_netif_get_default();\n",
        "fnet/bad",
    )

    # no listener-rebind globals.
    _expect_pass(assert_no_rebind_globals, "static bool g_link_up;\n", "rebind/good")
    _expect_fail(
        assert_no_rebind_globals, "static bool g_dhcp_pending;\n",
        "rebind/bad", "g_dhcp_pending",
    )

    # no blocking socket write loop.
    _expect_pass(
        assert_no_blocking_socket_write_loop,
        "static void send(void) {\n\tg_http_client.write(buffer, length);\n}\n",
        "write-loop/good-no-loop",
    )
    _expect_pass(
        assert_no_blocking_socket_write_loop,
        "static void copy(void) {\n"
        "\tfor (unsigned i = 0; i < 4; i++) { buffer[i] = source[i]; }\n}\n",
        "write-loop/good-bounded",
    )
    bad_blocking_loop = (
        "static void send(void) {\n"
        "\tfor (; offset < length;) {\n"
        "\t\tif ((millis() - last) > 100) break;\n"
        "\t\tg_http_client.write(buffer + offset, remaining);\n"
        "\t}\n}\n"
    )
    _expect_fail(
        assert_no_blocking_socket_write_loop, bad_blocking_loop, "write-loop/bad"
    )

    # no watchdog feed outside the single per-epoch one.
    good_no_extra_feed = (
        "void loop(void) { aius_boot_teensy_watchdog_feed(&primitives); }\n"
    )
    _expect_pass(assert_no_extra_watchdog_feed, good_no_extra_feed, "extra-feed/good")
    bad_extra_feed_count = good_no_extra_feed + (
        "void other(void) { aius_boot_teensy_watchdog_feed(&primitives); }\n"
    )
    _expect_fail(
        assert_no_extra_watchdog_feed, bad_extra_feed_count, "extra-feed/bad-count"
    )
    bad_legacy_feed = (
        "static void watchdog_feed(void) { WDOG1_WSR = 0x5555; }\n"
    )
    _expect_fail(
        assert_no_extra_watchdog_feed, bad_legacy_feed, "extra-feed/legacy",
    )
    bad_raw_interval = good_no_extra_feed + (
        "static const int x = WDOG_WCR_WT(15);\n"
    )
    _expect_fail(
        assert_no_extra_watchdog_feed, bad_raw_interval,
        "extra-feed/raw-interval", "WDOG_WCR_WT",
    )
    # The OTA activation's RAM-resident feed is the one legitimate raw WDOG1_WSR
    # writer; the same write outside ota_wdt_feed() is not.
    good_ota_feed = good_no_extra_feed + (
        "FASTRUN static void ota_wdt_feed(void *)\n{\n"
        "\tWDOG1_WSR = 0x5555;\n\tWDOG1_WSR = 0xAAAA;\n}\n"
    )
    _expect_pass(assert_no_extra_watchdog_feed, good_ota_feed, "extra-feed/ota-resident")
    bad_raw_wsr_elsewhere = good_ota_feed + (
        "static void sneaky(void) { WDOG1_WSR = 0x5555; }\n"
    )
    _expect_fail(
        assert_no_extra_watchdog_feed, bad_raw_wsr_elsewhere,
        "extra-feed/raw-wsr-elsewhere", "outside ota_wdt_feed",
    )

    # the 24-bit output path.
    good_output = (
        "class AiusAudioProcessor : public AudioStream\n"
        "{\n"
        "\tFLASHMEM void begin_pipeline(void);\n"
        "\tvoid update(void) override\n"
        "\t{\n"
        "\t\taius_pipeline_push(g_pipeline, words, count);\n"
        "\t}\n"
        "\tstatic constexpr int OUTPUT_WIDTH_BITS = AIUS_DSP_OUT_WIDTH_24;\n"
        "\tAiusStatus enqueue(const AiusPcmBlock *block)\n"
        "\t{\n"
        "\t\tif (block->frames > aius_spdif_tx24_free(&g_aius_spdif_tx24))\n"
        "\t\t\treturn AIUS_NO_SPACE;\n"
        "\t\tuint32_t first = aius_spdif_tx24_stage(&g_aius_spdif_tx24, 0,\n"
        "\t\t                                      &ring_left, &ring_right);\n"
        "\t\tif (first > block->frames)\n"
        "\t\t\tfirst = block->frames;\n"
        "\t\taius_out_stage_run(&g_out16, OUTPUT_WIDTH_BITS, processed_left,\n"
        "\t\t                   processed_right, ring_left, ring_right, first);\n"
        "\t\tif (first < block->frames) {\n"
        "\t\t\t(void)aius_spdif_tx24_stage(&g_aius_spdif_tx24, first,\n"
        "\t\t\t                            &ring_left, &ring_right);\n"
        "\t\t\taius_out_stage_run(&g_out16, OUTPUT_WIDTH_BITS,\n"
        "\t\t\t                   processed_left + first, processed_right + first,\n"
        "\t\t\t                   ring_left, ring_right, block->frames - first);\n"
        "\t\t}\n"
        "\t\taius_spdif_tx24_commit(&g_aius_spdif_tx24, block->frames);\n"
        "\t\treturn AIUS_OK;\n"
        "\t}\n"
        "};\n"
        # Out-of-line, matching the real source's shape (FLASHMEM belongs on
        # the definition; the in-class line above is a declaration a body
        # extractor must skip). Two-tab body indentation is synthetic-input
        # convention here, shared with the mutation splices below.
        "FLASHMEM void AiusAudioProcessor::begin_pipeline(void)\n"
        "{\n"
        "\t\taius_out16_init(&g_out16, AIUS_DSP_DITHER_SEED_DEFAULT, 1);\n"
        "\t\taius_spdif_tx24_begin();\n"
        "}\n"
        "static AiusAudioProcessor g_audio_processor;\n"
        "static AudioOutputSPDIF3 g_spdif_out;\n"
        "static AudioConnection g_spdif_left(g_spdif_in, 0, g_audio_processor, 0);\n"
        "static AudioConnection g_spdif_right(g_spdif_in, 1, g_audio_processor, 1);\n"
    )
    _expect_pass(assert_enqueue_writes_24_bit_words, good_output, "out24/good")
    _expect_pass(assert_no_audio_block_output_hop, good_output, "out24/no-hop")
    _expect_pass(assert_no_int16_playback_ring, good_output, "out24/no-int16-ring")

    # THE REGRESSION: the width selection quietly narrowed back to 16.
    narrowed = good_output.replace("AIUS_DSP_OUT_WIDTH_24", "AIUS_DSP_OUT_WIDTH_16", 1)
    _expect_fail(
        assert_enqueue_writes_24_bit_words, narrowed, "out24/narrowed",
        "AIUS_DSP_OUT_WIDTH_24",
    )
    # The reduction back on the shipped path beside the stage.
    reduced = good_output.replace(
        "\t\taius_spdif_tx24_commit(&g_aius_spdif_tx24, block->frames);\n",
        "\t\taius_out16_run(&g_out16, processed_left, processed_right,\n"
        "\t\t               ring_left, ring_right, first);\n"
        "\t\taius_spdif_tx24_commit(&g_aius_spdif_tx24, block->frames);\n",
        1,
    )
    _expect_fail(
        assert_enqueue_writes_24_bit_words, reduced, "out24/out16-on-the-path",
        "aius_out16_run()",
    )
    # One staging call cannot cover both sides of the wrap.
    unwrapped = good_output.replace(
        "\t\t\t(void)aius_spdif_tx24_stage(&g_aius_spdif_tx24, first,\n"
        "\t\t\t                            &ring_left, &ring_right);\n",
        "",
        1,
    )
    _expect_fail(
        assert_enqueue_writes_24_bit_words, unwrapped, "out24/one-stage-call",
        "expected 2",
    )
    # Committing per staging call publishes a half-written block to the ISR.
    torn = good_output.replace(
        "\t\tif (first < block->frames) {\n",
        "\t\taius_spdif_tx24_commit(&g_aius_spdif_tx24, first);\n"
        "\t\tif (first < block->frames) {\n",
        1,
    )
    _expect_fail(
        assert_enqueue_writes_24_bit_words, torn, "out24/commit-per-stage",
        "expected 1",
    )
    # Staging with no free-space check overwrites untransmitted frames.
    unchecked = good_output.replace(
        "\t\tif (block->frames > aius_spdif_tx24_free(&g_aius_spdif_tx24))\n"
        "\t\t\treturn AIUS_NO_SPACE;\n",
        "",
        1,
    )
    _expect_fail(
        assert_enqueue_writes_24_bit_words, unchecked, "out24/no-space-check",
        "aius_spdif_tx24_free()",
    )
    # THE SILENT ONE: the handler is never installed, so the vendor ISR keeps the
    # vector and the board transmits block_silent while counting blocks.
    uninstalled = good_output.replace("\t\taius_spdif_tx24_begin();\n", "", 1)
    _expect_fail(
        assert_enqueue_writes_24_bit_words, uninstalled, "out24/not-installed",
        "block_silent",
    )
    reinstalled = good_output.replace(
        "\t\taius_spdif_tx24_begin();\n",
        "\t\taius_spdif_tx24_begin();\n\t\taius_spdif_tx24_begin();\n",
        1,
    )
    _expect_fail(
        assert_enqueue_writes_24_bit_words, reinstalled, "out24/installed-twice",
        "exactly once",
    )
    # THE OTHER SILENT ONE: the 16-bit hop reconnected, which also leaks the pool.
    reconnected = good_output + (
        "static AudioConnection g_audio_left(g_audio_processor, 0, g_spdif_out, 0);\n"
    )
    _expect_fail(
        assert_no_audio_block_output_hop, reconnected, "out24/reconnected",
        "g_spdif_out",
    )
    # ...and the opposite mistake: deleting the peripheral along with its
    # connections, which takes config_spdif3() and the whole clock tree with it.
    unconstructed = good_output.replace(
        "static AudioOutputSPDIF3 g_spdif_out;\n", "", 1
    )
    _expect_fail(
        assert_no_audio_block_output_hop, unconstructed, "out24/no-peripheral",
        "config_spdif3()",
    )
    re_emitted = good_output.replace(
        "\t\taius_pipeline_push(g_pipeline, words, count);\n",
        "\t\taius_pipeline_push(g_pipeline, words, count);\n\t\temit_block();\n",
        1,
    )
    _expect_fail(
        assert_no_audio_block_output_hop, re_emitted, "out24/emit-in-callback",
        "emit_block(",
    )
    _expect_fail(
        assert_no_int16_playback_ring,
        good_output + "\tint16_t g_left[RING_FRAMES];\n",
        "out24/int16-ring-back", "int16 playback ring",
    )

    # the retained snapshot is unpatched.
    good_snapshot = (
        "\tdo {\n"
        "\t\t*dest++ = (*src_left++) << 8;\n"
        "\t\t*dest++ = (*src_right++) << 8;\n"
        "\t} while (dest < end);\n"
    )
    _expect_pass(assert_snapshot_is_not_patched, good_snapshot, "snapshot/good")
    _expect_fail(
        assert_snapshot_is_not_patched,
        good_snapshot.replace("(*src_left++) << 8", "*src_left24++", 1),
        "snapshot/patched", "sited beside the snapshot",
    )
    _expect_fail(
        assert_snapshot_is_not_patched,
        good_snapshot + "\taius_spdif_tx24_fill(&g_aius_spdif_tx24, dest, 128);\n",
        "snapshot/ours-leaked", "leaked into the retained snapshot",
    )

    # the transmit ISR drains the 24-bit ring.
    good_isr = (
        '#include "../../../../evidence/source-snapshots/'
        'PJRC-SPDIF-TX-SOURCE/output_spdif3.cpp"\n'
        '#include "../../../../evidence/source-snapshots/'
        'PJRC-SPDIF-RX-SOURCE/input_spdif3.cpp"\n'
        '#include "aius_spdif_tx24.h"\n'
        "DMAMEM AiusSpdifTx24Ring g_aius_spdif_tx24;\n"
        "void AiusSpdifTx24Hook::isr(void)\n"
        "{\n"
        "\tconst uint32_t saddr = (uint32_t)(dma.TCD->SADDR);\n"
        "\tdma.clearInterrupt();\n"
        "\taius_spdif_tx24_fill(&g_aius_spdif_tx24, dest, AUDIO_BLOCK_SAMPLES);\n"
        "\tarm_dcache_flush(dest, AUDIO_BLOCK_SAMPLES * 2 * sizeof *dest);\n"
        "\tif (update_responsibility)\n"
        "\t\tupdate_all();\n"
        "}\n"
        "FLASHMEM void AiusSpdifTx24Hook::install(void)\n"
        "{\n"
        "\tdma.attachInterrupt(AiusSpdifTx24Hook::isr);\n"
        "}\n"
        "FLASHMEM void aius_spdif_tx24_begin(void)\n"
        "{\n"
        "\tAiusSpdifTx24Hook::install();\n"
        "\tSPDIF_STCSCH = UINT32_C(0x00200000);\n"
        "\tSPDIF_STCSCL = UINT32_C(0x00400000);\n"
        "}\n"
    )
    _expect_pass(
        assert_transmit_isr_drains_the_24_bit_ring, good_isr, "tx-isr/good"
    )
    # THE MEMORY REGRESSION: 16 KB out of a 25 KB stack budget, symptom elsewhere.
    _expect_fail(
        assert_transmit_isr_drains_the_24_bit_ring,
        good_isr.replace("DMAMEM AiusSpdifTx24Ring", "AiusSpdifTx24Ring", 1),
        "tx-isr/ring-in-dtcm", "DMAMEM",
    )
    for removed, needle in (
        ("\tdma.clearInterrupt();\n", "dma.clearInterrupt("),
        ("\tdma.attachInterrupt(AiusSpdifTx24Hook::isr);\n", "dma.attachInterrupt("),
        ("\tarm_dcache_flush(dest, AUDIO_BLOCK_SAMPLES * 2 * sizeof *dest);\n",
         "arm_dcache_flush("),
        ("\tif (update_responsibility)\n\t\tupdate_all();\n",
         "update_responsibility"),
        ("\tSPDIF_STCSCH = UINT32_C(0x00200000);\n",
         "SPDIF_STCSCH = UINT32_C(0x00200000);"),
        ("\tSPDIF_STCSCL = UINT32_C(0x00400000);\n",
         "SPDIF_STCSCL = UINT32_C(0x00400000);"),
    ):
        _expect_fail(
            assert_transmit_isr_drains_the_24_bit_ring,
            good_isr.replace(removed, "", 1),
            f"tx-isr/missing-{needle}", needle,
        )
    # The reduction reintroduced inside our own handler.
    _expect_fail(
        assert_transmit_isr_drains_the_24_bit_ring,
        good_isr.replace(
            "\taius_spdif_tx24_fill(&g_aius_spdif_tx24, dest, AUDIO_BLOCK_SAMPLES);\n",
            "\taius_spdif_tx24_fill(&g_aius_spdif_tx24, dest, AUDIO_BLOCK_SAMPLES);\n"
            "\tfor (unsigned i = 0; i < 256u; i++)\n"
            "\t\tdest[i] = (int16_t)dest[i] << 8;\n",
            1,
        ),
        "tx-isr/shifts-again", "shifts by 8",
    )
    # A third include, or a copy of the snapshot pulled in some other way.
    _expect_fail(
        assert_transmit_isr_drains_the_24_bit_ring,
        good_isr.replace(
            '#include "aius_spdif_tx24.h"\n',
            '#include "aius_spdif_tx24.h"\n'
            '#include "../../../../evidence/source-snapshots/'
            'PJRC-SPDIF-TX-SOURCE/output_spdif3_patched.cpp"\n',
            1,
        ),
        "tx-isr/extra-snapshot-include", "two retained snapshot includes",
    )
    # audio_block_t in a SIBLING function is fine; in the transmit handler it
    # is the 16-bit hop the path exists to remove.
    _expect_pass(
        assert_transmit_isr_drains_the_24_bit_ring,
        good_isr + "void other(void)\n{\n\taudio_block_t *b = nullptr;\n}\n",
        "tx-isr/audio-block-elsewhere",
    )
    _expect_fail(
        assert_transmit_isr_drains_the_24_bit_ring,
        good_isr.replace(
            "\tdma.clearInterrupt();\n",
            "\tdma.clearInterrupt();\n\taudio_block_t *b = nullptr;\n",
            1,
        ),
        "tx-isr/audio-block-in-handler", "touches audio_block_t",
    )

    # the receive ISR stages only into an empty block pair.
    good_rx = (
        "void AiusSpdifRxHook::isr(void)\n"
        "{\n"
        "\tdma.clearInterrupt();\n"
        "\tif (update_responsibility)\n"
        "\t\tupdate_all();\n"
        "\tarm_dcache_delete(src, AUDIO_BLOCK_SAMPLES * 2 * sizeof *src);\n"
        "\tif (!aius_spdif_rx_stage(src, AUDIO_BLOCK_SAMPLES, left->data,\n"
        "\t                         right->data, &block_offset, AUDIO_BLOCK_SAMPLES))\n"
        "\t\tdropped_halves++;\n"
        "}\n"
        "FLASHMEM void AiusSpdifRxHook::install(void)\n"
        "{\n"
        "\tdma.attachInterrupt(AiusSpdifRxHook::isr);\n"
        "}\n"
    )
    _expect_pass(
        assert_receive_isr_stages_only_into_an_empty_pair, good_rx, "rx-isr/good"
    )
    for removed, needle in (
        ("\tdma.clearInterrupt();\n", "dma.clearInterrupt("),
        ("\tarm_dcache_delete(src, AUDIO_BLOCK_SAMPLES * 2 * sizeof *src);\n",
         "arm_dcache_delete("),
        ("\tif (update_responsibility)\n\t\tupdate_all();\n",
         "update_responsibility"),
    ):
        _expect_fail(
            assert_receive_isr_stages_only_into_an_empty_pair,
            good_rx.replace(removed, "", 1),
            f"rx-isr/missing-{needle}", needle,
        )
    # THE REGRESSION: the snapshot's copy loop back in our handler.
    _expect_fail(
        assert_receive_isr_stages_only_into_an_empty_pair,
        good_rx.replace(
            "\tif (!aius_spdif_rx_stage(src, AUDIO_BLOCK_SAMPLES, left->data,\n"
            "\t                         right->data, &block_offset, AUDIO_BLOCK_SAMPLES))\n"
            "\t\tdropped_halves++;\n",
            "\tdest_left = &(left->data[offset]);\n"
            "\tblock_offset = offset + AUDIO_BLOCK_SAMPLES * 2;\n",
            1,
        ),
        "rx-isr/snapshot-copy-loop", "guarded copy",
    )
    _expect_fail(
        assert_receive_isr_stages_only_into_an_empty_pair,
        good_rx.replace(
            "\t\tdropped_halves++;\n",
            "\t\tdropped_halves++;\n\tdest_left = &(left->data[offset]);\n",
            1,
        ),
        "rx-isr/running-offset", "running offset",
    )
    _expect_fail(
        assert_receive_isr_stages_only_into_an_empty_pair,
        good_rx.replace("\tdma.attachInterrupt(AiusSpdifRxHook::isr);\n", "", 1),
        "rx-isr/vector-not-redirected", "redirects the receive DMA vector",
    )

    # the replay gate-open edge pads to its own floor.
    good_floor = (
        "#define AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES 768u\n"
        "#if AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES < AIUS_TX_GATE_OPEN_TARGET_FRAMES\n"
        "#error \"below\"\n"
        "#endif\n"
        "static inline void tx_gate_execute_open(bool replay, bool first_in_unit,\n"
        "                                        uint32_t unit_frames,\n"
        "                                        uint32_t block_frames)\n"
        "{\n"
        "\tif (!g_tx_gate_open_pending)\n"
        "\t\treturn;\n"
        "\tconst uint32_t target_frames =\n"
        "\t\treplay ? AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES\n"
        "\t\t       : AIUS_TX_GATE_OPEN_TARGET_FRAMES;\n"
        "\taius_spdif_tx24_set_level(&g_aius_spdif_tx24, target_frames,\n"
        "\t                          &dropped, &padded);\n"
        "}\n"
        "AiusStatus enqueue(const AiusPcmBlock *block)\n"
        "{\n"
        "\tconst bool replay =\n"
        "\t\tg_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY;\n"
        "\ttx_gate_execute_open(replay, block->first_in_unit != 0u,\n"
        "\t                     unit_frames, block->frames);\n"
        "\treturn AIUS_OK;\n"
        "}\n"
    )
    _expect_pass(
        assert_replay_gate_open_pads_to_its_own_floor, good_floor, "replay-floor/good"
    )
    _expect_fail(
        assert_replay_gate_open_pads_to_its_own_floor,
        good_floor.replace(
            "#if AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES < "
            "AIUS_TX_GATE_OPEN_TARGET_FRAMES\n#error \"below\"\n#endif\n",
            "",
            1,
        ),
        "replay-floor/unguarded", "sinking below",
    )
    # THE REGRESSION: one floor for both sources.
    _expect_fail(
        assert_replay_gate_open_pads_to_its_own_floor,
        good_floor.replace(
            "\tconst uint32_t target_frames =\n"
            "\t\treplay ? AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES\n"
            "\t\t       : AIUS_TX_GATE_OPEN_TARGET_FRAMES;\n"
            "\taius_spdif_tx24_set_level(&g_aius_spdif_tx24, target_frames,\n",
            "\taius_spdif_tx24_set_level(&g_aius_spdif_tx24,\n"
            "\t                          AIUS_TX_GATE_OPEN_TARGET_FRAMES,\n",
            1,
        ),
        "replay-floor/one-floor", "select the floor by source",
    )
    _expect_fail(
        assert_replay_gate_open_pads_to_its_own_floor,
        good_floor.replace("\ttx_gate_execute_open(replay, ",
                           "\ttx_gate_execute_open(false, ", 1),
        "replay-floor/fact-not-passed", "replay fact",
    )


# --------------------------------------------------------------------------
# Runner.
# --------------------------------------------------------------------------

def run_checks(label, checks, text):
    failures = []
    for name, check in checks:
        try:
            check(text)
        except AssertionError as error:
            failures.append(f"  - {name}: {error}")
    if failures:
        raise AssertionError(
            f"optical-cutover {label} group FAILED "
            f"({len(failures)}/{len(checks)}):\n" + "\n".join(failures)
        )
    print(f"optical-cutover {label} group: PASS ({len(checks)}/{len(checks)})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--group",
        choices=("positive", "absence", "all"),
        default="all",
        help="run only the positive group, only the absence group, or both (default)",
    )
    args = parser.parse_args()

    run_helper_self_test()
    run_audio_cutover_self_test()
    run_self_test()
    print("optical-cutover self-test (checks discriminate against synthetic input): PASS")

    text = OPTICAL_MAIN.read_text()

    if args.group in ("positive", "all"):
        run_checks("positive", POSITIVE_CHECKS, text)
        # Grouped with the positive checks because it asserts a property the
        # file must HAVE, but given its own source set: its whole point is that
        # optical_main.cpp alone cannot state it.
        run_checks(
            "cross-source", CROSS_SOURCE_CHECKS, read_replay_sizing_sources()
        )
        # The 24-bit output path's other two files, each read on its own: the
        # claim is that ours includes the snapshot and changes nothing in it, so
        # neither file can be checked through the other.
        run_checks("snapshot", OUTPUT_PATH_CHECKS, TX_SNAPSHOT.read_text())
        run_checks("transmit-module", TRANSMIT_MODULE_CHECKS, TX_MODULE.read_text())
    if args.group in ("absence", "all"):
        run_checks("absence", ABSENCE_CHECKS, text)


if __name__ == "__main__":
    sys.exit(main())
