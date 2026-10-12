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
                      the recovery state, and the service checkpoint
                      (platform_service) performs no direct SD primitive.
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


def without_attributes(clean):
    """`clean` with every `__attribute__((...))` replaced by spaces of the
    same length, so offsets into the original text still hold."""
    out = clean
    for match in re.finditer(r"\b__attribute__\s*\(", clean):
        opening = clean.find("(", match.start())
        closing = matching_delimiter(clean, opening, "(", ")")
        out = out[:match.start()] + " " * (closing + 1 - match.start()) + out[closing + 1:]
    return out


def function_definitions(text):
    """Every file-scope C-style function DEFINITION in `text`, as
    name -> brace-matched body.

    A definition is a line-initial declarator whose closing `)` is followed
    (modulo whitespace) by `{` -- which is what separates it from a prototype, a
    call, or a macro. Comments and literals are blanked for the scan and the
    brace match; the returned body is the original text. Qualified C++ member
    definitions (`Class::member`) are deliberately not matched: nothing this
    suite scopes to is one, and admitting `::` would also admit statements.
    `__attribute__((...))` specifiers are blanked (length kept) for the scan,
    so `static FLASHMEM __attribute__((noinline)) bool f(` is a definition of
    f (Phase 4f: the render appends are noinline)."""
    clean = without_attributes(without_comments_and_literals(text))
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


# A direct card primitive: an `SD.<method>(` call, or a file-handle method that
# only an owned `File` has.
SD_PRIMITIVE = re.compile(
    r"\bSD\s*\.\s*[A-Za-z_][A-Za-z0-9_]*\s*\("
    r"|\b[A-Za-z_][A-Za-z0-9_]*\s*\.\s*"
    r"(?:open|read|write|seek|close|truncate|size)\s*\("
)

# Checkpoint 7 (SERVICE; REPLAY until Phase 4i, same id 7 and budget row). Its
# services run in this order: the SD lifecycle (with 4g's fill walk) before
# 4j's idle DSP apply, the idle apply before the rate service (closure S1), the
# rate service before the gate judges it, and the recorder drain last, after
# the gate it consumes.
SERVICE_CHECKPOINT_MARKER = (
    "static FLASHMEM bool platform_service(void *, const AiusRuntimeBudget *budget,"
)
SERVICE_ORDER = (
    "admission_sd_window_sample(",
    "sd_lifecycle_service(",
    "dsp_idle_apply_service(",
    "sample_rate_service(",
    "tx_gate_service(",
    "input_fault_service(",
    "input_dark_service(",
    "aius_recorder_bank_drain(",
)


def assert_service_path_has_no_direct_sd(text):
    """The checkpoint-7 driver reaches the card only through the bounded SD
    lifecycle job, never by a direct primitive (the no-direct-SD scan that
    covered platform_replay, now on platform_service)."""
    body = function_body(text, SERVICE_CHECKPOINT_MARKER)
    clean = without_comments_and_literals(body)
    match = SD_PRIMITIVE.search(clean)
    assert match is None, (
        "platform_service() calls a card primitive directly "
        f"({match.group(0).strip() if match else ''!r}); checkpoint 7 reaches "
        "the card only through the bounded SD lifecycle job"
    )
    assert "sd_lifecycle_service(" in clean, (
        "platform_service() never calls sd_lifecycle_service(), so this scan "
        "would pass without the SD path it exists to cover"
    )


def assert_service_checkpoint_runs_in_order(text):
    body = function_body(text, SERVICE_CHECKPOINT_MARKER)
    clean = without_comments_and_literals(body)
    assert "AIUS_BOOT_CHECKPOINT_SERVICE" in clean, (
        "platform_service() does not key on AIUS_BOOT_CHECKPOINT_SERVICE (id 7)"
    )
    assert not re.search(r"replay", clean, re.IGNORECASE), (
        "platform_service() still names replay; Phase 4i retired every replay "
        "service from checkpoint 7"
    )
    positions = []
    for call in SERVICE_ORDER:
        at = clean.find(call)
        assert at >= 0, f"platform_service() never calls {call}...)"
        positions.append(at)
    assert positions == sorted(positions), (
        "platform_service() runs its services out of order; expected "
        + " -> ".join(c.rstrip("(") for c in SERVICE_ORDER)
    )


# Both flags, not one: a pin that sets only one leaves both or neither driving
# update_all() (a double-processed block or a stalled graph).
CLOCK_MASTER_SETTERS = (
    ("AiusSpdifOutputClock::set_master", "false"),
    ("AiusSpdifInputClock::set_master", "true"),
)


def assert_setup_pins_input_master_once(text):
    """Phase 4i (owner Q3): live input is the only source, so setup() pins the
    input as the graph's clock master once, explicitly, and nothing else in
    the file can move it."""
    body = function_body(text, SETUP_MARKER)
    clean = without_comments_and_literals(body)
    guard_open = clean.find("noInterrupts(")
    guard_close = (
        clean.find("interrupts()", guard_open + len("noInterrupts("))
        if guard_open >= 0 else -1
    )
    assert guard_open >= 0 and guard_close > guard_open, (
        "setup() pins the clock master outside a noInterrupts()/interrupts() "
        "critical section; both flags are read from a DMA ISR"
    )
    for setter, value in CLOCK_MASTER_SETTERS:
        whole = qualified_call_sites(text, setter)
        assert len(whole) == 1, (
            f"{setter}() has {len(whole)} call sites in the file; setup() pins "
            "the input once and nothing may move the graph's clock afterwards"
        )
        local = qualified_call_sites(body, setter)
        assert len(local) == 1 and guard_open < local[0] < guard_close, (
            f"{setter}() is not called once inside setup()'s critical section"
        )
        assert re.search(re.escape(setter) + r"\s*\(\s*" + value + r"\s*\)", clean), (
            f"setup() must call {setter}({value}): the input is the master"
        )


TX_GATE_SERVICE_MARKER = "static FLASHMEM void tx_gate_service(void)"


def assert_source_changes_count_carrier_edges(text):
    """audio_diag.source_changes counts carrier gained and lost edges
    (Phase 4i): tx_gate_service() bumps it when the live lock differs from the
    latched carrier, before the latch moves."""
    clean = without_comments_and_literals(
        function_body(text, TX_GATE_SERVICE_MARKER)
    )
    edge = re.search(
        r"if\s*\(\s*carrier\s*!=\s*g_tx_gate_carrier[^)]*\)\s*"
        r"g_audio_ingress\.source_changes\+\+\s*;",
        clean,
    )
    assert edge is not None, (
        "tx_gate_service() does not count a carrier edge into "
        "g_audio_ingress.source_changes against the latched carrier"
    )
    latch = clean.find("g_tx_gate_carrier = carrier;")
    assert latch > edge.start(), (
        "tx_gate_service() moves g_tx_gate_carrier before comparing it, so no "
        "carrier edge can ever be counted"
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
    # 3 was AIUS_EVENT_REPLAY_STOP, retired with replay (Phase 4i).
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
    # Phase 4h (Q2): a pending preset or www entry dropped on a chip CRC fault.
    "AIUS_EVENT_SD_FLUSH_CRC_DROP": 19,
}
# Spent values: retired types whose number may never be allocated again.
RETIRED_EVENT_TYPE_VALUES = {3: "AIUS_EVENT_REPLAY_STOP (Phase 4i)"}
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
    reused = sorted(f"{name} = {value} (was {RETIRED_EVENT_TYPE_VALUES[value]})"
                    for name, value in allocated.items()
                    if value in RETIRED_EVENT_TYPE_VALUES)
    assert not reused, (
        "a retired event type value is allocated again; a reader of a stored "
        "sample cannot tell the old meaning from the new: " + ", ".join(reused)
    )
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
         "the transmitter must still be able to drive update_all(), or a graph "
         "the output masters with no carrier processes zero blocks again"),
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
    the unlocked receiver's DMA storm does with the carrier down (RX pool
    corruption, 2026-09-03, then under replay: the pool leaked down to two free
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
        "stays installed and the out-of-bounds write returns with the next "
        "carrier-down DMA storm"
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
    service_driver = without_comments_and_literals(
        function_body(text, "static FLASHMEM bool platform_service(")
    )
    order = [
        service_driver.find("sample_rate_service(now_ms);"),
        service_driver.find("tx_gate_service();"),
    ]
    assert min(order) >= 0 and order == sorted(order) and len(set(order)) == 2, (
        "checkpoint 7 must update/apply rate truth, then evaluate the "
        "physical gate"
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
    assert "aius_tx_gate_permitted(carrier, g_clock_live_ready)" in gate, (
        "a locked but unaccepted/mismatched live carrier can still open the pad"
    )
    reset = gate.find("request_audio_reset();")
    credit = gate.find("g_clock_relock_reset_nominal_hz =")
    assert 0 <= reset < credit, (
        "the relock reset does not publish the credit that prevents a second "
        "reset at nominal acceptance"
    )


def assert_enqueue_refuses_rate_before_dsp(text):
    body = without_comments_and_literals(
        function_body(text, "AiusStatus enqueue(const AiusPcmBlock *block)")
    )
    support = body.find("supported_program_rate(block->sample_rate_hz)")
    applied = body.find("!g_clock_plan_applied")
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
        conditional_body(body, "if (!g_clock_plan_applied)")
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
    assert "const uint32_t expected_rate_hz = g_clock_nominal_rate_hz;" in body, (
        "enqueue() does not reconcile blocks against the accepted live nominal"
    )
    assert not re.search(r"replay", body, re.IGNORECASE), (
        "enqueue() still names replay; live input is the only source (Phase 4i)"
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
    "centre_writes",
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
    "gate_open_trims",
    "gate_open_trimmed_frames",
    "gate_open_last_trim_at_ms",
    "gate_open_padded_frames",
    # aius-sample-rate-following Phase 4a F3: a pending-boundary open's half.
    "gate_open_pending_padded_frames",
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
        "clock.state reports an unlocked input instead of an active "
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
    fill_call = "clock_fill_service(now_ms);"
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
    ):
        assert re.search(call, service), (
            owner + " does not declare its fault-reset ownership"
        )
    invalid_plan_owners = re.findall(
        r"clock_record_fault\s*\(\s*AIUS_CLOCK_APPLY_INVALID_PLAN\s*,\s*"
        r"(true|false)\s*\)",
        service,
    )
    assert invalid_plan_owners == ["true", "false"], (
        "invalid-plan reset ownership must be nominal-acquisition=true, "
        "same-nominal-target=false"
    )
    quiesce = service.find(
        "if (result.quiesce_requested && carrier)"
    )
    producer_close = service.find(
        "g_clock_live_ingress_open = false;", quiesce
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
        < producer_close
        < close
        < reset
        < credit
        < accepted
    ), (
        "the first locked live changed-rate sample does not close ingress and "
        "issue one nominal-bound reset credit before acceptance"
    )
    refresh = service.find("clock_refresh_applied(now_ms);")
    close_episode = service.find(
        "if (!g_clock_carrier_ready)",
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
        "aius_audio_ingress_reserve",
        "aius_audio_ingress_commit",
        "aius_audio_ingress_cancel_gap",
        "audio_ingress_publish_live",
        "aius_audio_ingress_record_gap",
        # Phase 4e pend stamp: an inline compare and two stores after the
        # ingress commit, so the next producer pass can time the commit.
        "aius_producer_yield_note_pend",
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
    assert not re.search(r"replay", clean, re.IGNORECASE), (
        "AiusAudioProcessor::update() still names replay; live input is the "
        "only source (Phase 4i)"
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


DSP_ORIGIN_VALUES = ("DSP_ORIGIN_DEFAULT", "DSP_ORIGIN_SD", "DSP_ORIGIN_PUT")
DSP_SOURCE_NAME_MARKER = (
    "static FLASHMEM const char *dsp_source_name(DspModelOrigin origin)"
)
DSP_ORIGIN_NAME_MARKER = (
    "static FLASHMEM const char *dsp_origin_name(DspModelOrigin origin)"
)
DSP_CONFIG_JSON_MARKER = (
    "static FLASHMEM size_t dsp_config_json(uint8_t *body, size_t capacity,\n"
    "                                      bool file_form)"
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
    # Phase 4h: the SD stager is settings_load(), which the settings mount
    # policy calls from platform_publish_config.
    for name in ("app_store_dsp_config", "settings_load"):
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
    # Phase 4h (Q1, N7): with the carve the PUT stager arms Apply itself, but
    # only once the overlay put has landed (a refusal applies nothing), and
    # with the origin staged before the flag. The synchronous (8 MiB) path,
    # after the overlay branch, still leaves the flag to the commit arm.
    put_landed = put_body.find("aius_sd_overlay_put(")
    first_arm = put_body.find("g_dsp_apply_pending = true;")
    sync_path = put_body.find("sd_job_active_for_http()")
    assert put_body.count("g_dsp_apply_pending = true;") == 1, (
        "the PUT stager may arm the apply flag once, in the overlay branch"
    )
    assert -1 < put_landed < put_body.find("g_pending_dsp_origin = DSP_ORIGIN_PUT;") \
        < first_arm < sync_path, (
        "the PUT stager arms the apply flag only after the overlay put lands "
        "and the origin is staged, never on the synchronous path, so a failed "
        "SD write cannot reach the audio block boundary"
    )
    assert "g_dsp_apply_pending" not in put_body[sync_path:], (
        "the PUT stager's synchronous path arms the apply flag itself, so a "
        "failed SD write could reach the audio block boundary"
    )
    sd_body = without_comments_and_literals(definitions["settings_load"])
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
    # The serialiser is aius_optical_dsp_config_write (optical-app proves
    # the rendered source); this body hands it the selected origin's name.
    assert "header.source = dsp_source_name(origin);" in dsp_config, (
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


DSP_IDLE_APPLY_MARKER = (
    "FLASHMEM void AiusAudioProcessor::dsp_idle_apply_service(uint32_t now_ms)"
)
# S1 (4j delivery audit): a fresh millis(), never the epoch's now_ms, which a
# nested AUDIO pass's later stamp would make wrap to "due".
DSP_IDLE_APPLY_CALL = "g_audio_processor.dsp_idle_apply_service(millis());"
DSP_BUILD_GUARD = "if (decision.build)"
DSP_COPY_GUARD = "if (decision.copy)"
DSP_BUILD_CALL = "apply_config(g_active_model, decision.build_rate_hz);"
DSP_COPY_STATEMENT = "g_active_model = g_pending_dsp_model;"
DSP_GENERATION_STEP = "g_config_generation += decision.generation_steps;"


def guarded_statement(clean, marker):
    """The statement one exact `if (...)` marker guards in `clean`, brace body
    or single statement, as a (start, end) span."""
    start = clean.find(marker)
    assert start >= 0, f"missing conditional marker {marker!r}"
    assert clean.find(marker, start + 1) == -1, (
        f"conditional marker {marker!r} is not unique"
    )
    return guarded_span_at(clean, start, marker)


def guarded_span_at(clean, start, marker):
    """The statement the `if (...)` `marker` at offset `start` guards."""
    cursor = start + len(marker)
    while cursor < len(clean) and clean[cursor] in " \t\r\n":
        cursor += 1
    if cursor < len(clean) and clean[cursor] == "{":
        return cursor, matching_delimiter(clean, cursor, "{", "}")
    end = clean.find(";", cursor)
    assert end >= 0, f"missing statement for conditional {marker!r}"
    return cursor, end + 1


def assert_arm_executes_the_decision(arm, label):
    """One apply arm: it calls aius_dsp_apply_decide once, builds only under
    the decision's build field at the decision's rate, and steps the
    generation by the decision's count (closure N-new2)."""
    assert arm.count("aius_dsp_apply_decide(") == 1, (
        f"the {label} arm does not route its apply through aius_dsp_apply_decide "
        "exactly once"
    )
    assert arm.count("apply_config(") == 1, (
        f"the {label} arm must call apply_config exactly once, under the "
        "decision's build field"
    )
    guard = guarded_statement(arm, DSP_BUILD_GUARD)
    call = arm.find(DSP_BUILD_CALL)
    assert call >= 0, (
        f"the {label} arm does not build at the decision's rate "
        "(apply_config(g_active_model, decision.build_rate_hz))"
    )
    assert guard[0] <= call < guard[1], (
        f"the {label} arm calls apply_config outside the decision's build field"
    )
    assert arm.count(DSP_GENERATION_STEP) == 1, (
        f"the {label} arm does not step the generation by the decision's count"
    )
    assert "g_config_generation++" not in arm, (
        f"the {label} arm steps the generation outside the decision"
    )
    first_copy = arm.find(DSP_COPY_GUARD)
    copy = arm.rfind(DSP_COPY_GUARD)
    assert first_copy >= 0 and copy > first_copy, (
        f"the {label} arm must test the decision's copy field twice: to copy "
        "the model, then to adopt the origin and release the flag"
    )
    # N1: the model copy sits under the FIRST copy guard.
    assert arm.count(DSP_COPY_STATEMENT) == 1, (
        f"the {label} arm must copy the staged model exactly once"
    )
    copied = guarded_span_at(arm, first_copy, DSP_COPY_GUARD)
    model_copy = arm.find(DSP_COPY_STATEMENT)
    assert copied[0] <= model_copy < copied[1], (
        f"the {label} arm copies the staged model outside the decision's "
        "first copy guard"
    )
    released = guarded_span_at(arm, copy, DSP_COPY_GUARD)
    released_body = arm[released[0]:released[1]]
    for statement in ("g_dsp_model_origin = g_pending_dsp_origin;",
                      "g_dsp_apply_pending = false;"):
        assert statement in released_body, (
            f"the {label} arm does not execute {statement!r} under the "
            "decision's copy field"
        )
    # N4: the generation step is unconditional, outside every decision guard.
    step = arm.find(DSP_GENERATION_STEP)
    for span in (guard, copied, released):
        assert not span[0] <= step < span[1], (
            f"the {label} arm steps the generation inside a decision guard; "
            "it must run outside both, by the decision's count"
        )


def assert_dsp_idle_apply_executes_the_decision(text):
    """Phase 4j: a staged DSP model applies with no audio flowing.

    The idle arm is a noinline AiusAudioProcessor member called in the service
    checkpoint before sample_rate_service; enqueue() stamps the idle clock for
    every block at the consume point, outside any branch (N2); and both arms
    execute aius_dsp_apply_decide's fields, so a caller that drops the build
    fails here even with the decision correct (closure N-new2)."""
    clean = without_comments_and_literals(text)
    assert re.search(
        r"__attribute__\(\(noinline\)\)\s+FLASHMEM\s+void\s+"
        r"dsp_idle_apply_service\(uint32_t now_ms\);",
        clean,
    ), (
        "dsp_idle_apply_service is not declared noinline; producer-isolation "
        "roots on its linked symbol"
    )
    driver = without_comments_and_literals(
        function_body(text, "static FLASHMEM bool platform_service(")
    )
    assert driver.count("dsp_idle_apply_service(") == 1, (
        "the service checkpoint does not call dsp_idle_apply_service exactly once"
    )
    service = driver.find(DSP_IDLE_APPLY_CALL)
    assert service >= 0, (
        "the service checkpoint must pass dsp_idle_apply_service a fresh "
        "millis(), not the epoch's now_ms: a nested AUDIO pass earlier in the "
        "epoch stamps g_dsp_last_block_ms later, and now - last wraps to due"
    )
    assert service < driver.find("sample_rate_service(now_ms);"), (
        "dsp_idle_apply_service must run before sample_rate_service"
    )

    enqueue = without_comments_and_literals(function_body(text, ENQUEUE_MARKER))
    stamp = "g_dsp_last_block_ms = millis();"
    assert enqueue.count(stamp) == 1, (
        "enqueue() does not stamp g_dsp_last_block_ms exactly once"
    )
    at = enqueue.find(stamp)
    assert at < enqueue.find("g_dsp_apply_pending"), (
        "enqueue() stamps g_dsp_last_block_ms after the consume point's flag read"
    )
    assert enqueue[:at].count("{") == enqueue[:at].count("}"), (
        "enqueue() stamps g_dsp_last_block_ms inside a branch; every block "
        "reaching the consume point must stamp it, outside any branch"
    )
    # N2: the stamp is the statement right after the consume point's last
    # refusal arm (the live-ready assignment), so no braceless branch guards
    # it and no refused block stamps it.
    previous = enqueue.rfind(";", 0, at)
    before_previous = enqueue.rfind(";", 0, previous)
    assert (
        previous >= 0
        and not enqueue[previous + 1:at].strip()
        and enqueue[max(before_previous, enqueue.rfind("}", 0, previous)) + 1:
                    previous + 1].strip()
        == "g_clock_live_ready = g_clock_carrier_ready;"
    ), (
        "enqueue() stamps g_dsp_last_block_ms somewhere other than directly "
        "after the g_clock_live_ready assignment at the consume point"
    )
    assert_arm_executes_the_decision(enqueue, "block")
    assert re.search(
        r"aius_dsp_apply_decide\(\s*model_pending,\s*g_applied_stream_rate_hz,"
        r"\s*block->sample_rate_hz\)",
        enqueue,
    ), (
        "the block arm does not decide on the flag, the applied rate and the "
        "block's rate"
    )

    idle = without_comments_and_literals(function_body(text, DSP_IDLE_APPLY_MARKER))
    assert_arm_executes_the_decision(idle, "idle")
    assert re.search(
        r"if\s*\(\s*!\s*aius_dsp_idle_apply_due\(\s*pending,\s*"
        r"g_dsp_last_block_ms,\s*now_ms\s*\)\s*\)\s*return\s*;",
        idle,
    ), (
        "the idle arm does not return unless aius_dsp_idle_apply_due holds"
    )
    assert "aius_dsp_apply_decide(pending, g_applied_stream_rate_hz, 0u)" in idle, (
        "the idle arm does not decide with block rate 0 at the applied rate"
    )
    assert idle.count("g_dsp_apply_pending") == 2, (
        "the idle arm must read g_dsp_apply_pending once and clear it once"
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
        "checkpoint 7 performs no direct SD primitive",
        assert_service_path_has_no_direct_sd,
    ),
    (
        "checkpoint 7 runs its services in order, with no replay",
        assert_service_checkpoint_runs_in_order,
    ),
    (
        "setup() pins the input as clock master once",
        assert_setup_pins_input_master_once,
    ),
    (
        "source_changes counts carrier gained and lost edges",
        assert_source_changes_count_carrier_edges,
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
        "the DSP model's origin is the stager's staged fact, not the swap's",
        assert_dsp_model_origin_is_staged_not_assumed,
    ),
    (
        "a staged DSP model applies with no audio flowing, both arms executing one decision",
        assert_dsp_idle_apply_executes_the_decision,
    ),
    (
        "the flight recorder reports the rate its frames carry",
        assert_recorder_reports_the_streams_rate,
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
        assert_dsp_idle_apply_executes_the_decision,
        assert_recorder_reports_the_streams_rate,
    )
    for check in checks:
        _expect_pass(check, text, "audio-cutover/current")
    # Phase 4j: each mutation keeps aius_dsp_apply_decide correct and breaks
    # one caller, so the check is what catches it (closure N-new2, N2).
    idle_apply_mutations = (
        ("idle-drops-build",
         "\tif (decision.build)\n\t\tapply_config(g_active_model, decision.build_rate_hz);\n",
         "",
         "exactly once"),
        ("idle-builds-unguarded",
         "\tif (decision.build)\n\t\tapply_config(g_active_model, decision.build_rate_hz);\n",
         "\tapply_config(g_active_model, decision.build_rate_hz);\n",
         "missing conditional marker"),
        ("idle-builds-at-another-rate",
         "\t\tapply_config(g_active_model, decision.build_rate_hz);\n\tg_config_generation",
         "\t\tapply_config(g_active_model, AIUS_OPTICAL_SAMPLE_RATE_HZ);\n\tg_config_generation",
         "decision's rate"),
        ("block-builds-unguarded",
         "\t\tif (decision.build) {\n\t\t\tapply_config(",
         "\t\t{\n\t\t\tapply_config(",
         "missing conditional marker"),
        ("block-steps-generation-itself",
         "\t\tg_config_generation += decision.generation_steps;\n",
         "\t\tg_config_generation++;\n",
         "decision's count"),
        ("stamp-inside-branch",
         "\t\tg_dsp_last_block_ms = millis();\n"
         "\t\tconst bool model_pending = g_dsp_apply_pending;\n",
         "\t\tconst bool model_pending = g_dsp_apply_pending;\n"
         "\t\tif (model_pending) {\n\t\t\tg_dsp_last_block_ms = millis();\n\t\t}\n",
         "stamps g_dsp_last_block_ms"),
        ("service-missing",
         "\t" + DSP_IDLE_APPLY_CALL + "\n",
         "",
         "exactly once"),
        ("service-inlinable",
         "__attribute__((noinline)) FLASHMEM void\n\tdsp_idle_apply_service(",
         "FLASHMEM void\n\tdsp_idle_apply_service(",
         "noinline"),
        # 4j delivery audit S1: the epoch's now_ms predates a nested pass's
        # stamp, so now - last wraps to "due" while blocks flow.
        ("service-passes-epoch-now",
         DSP_IDLE_APPLY_CALL,
         "g_audio_processor.dsp_idle_apply_service(now_ms);",
         "fresh millis()"),
        # N1: the model copy escapes the first copy guard, in either arm.
        ("block-copy-unguarded",
         "\t\tif (decision.copy)\n\t\t\tg_active_model = g_pending_dsp_model;\n",
         "\t\tg_active_model = g_pending_dsp_model;\n",
         "copy field twice"),
        ("idle-copy-unguarded",
         "\tif (decision.copy)\n\t\tg_active_model = g_pending_dsp_model;\n"
         "\tif (decision.build)",
         "\tif (decision.copy) {}\n\tg_active_model = g_pending_dsp_model;\n"
         "\tif (decision.build)",
         "first copy guard"),
        # N2: a braceless branch guarding the stamp, and the stamp hoisted
        # above the refusal arms.
        ("stamp-braceless-branch",
         "\t\tg_dsp_last_block_ms = millis();\n",
         "\t\tif (!g_clock_live_ready)\n\t\t\tg_dsp_last_block_ms = millis();\n",
         "directly after"),
        ("stamp-at-top",
         "\t\tg_dsp_last_block_ms = millis();\n"
         "\t\tconst bool model_pending",
         "\t\tconst bool model_pending",
         None),
        # N3: the due gate inverted, or its return dropped.
        ("idle-due-not-negated",
         "if (!aius_dsp_idle_apply_due(",
         "if (aius_dsp_idle_apply_due(",
         "unless aius_dsp_idle_apply_due holds"),
        ("idle-due-no-return",
         "g_dsp_last_block_ms, now_ms))\n\t\treturn;\n",
         "g_dsp_last_block_ms, now_ms))\n\t\t;\n",
         "unless aius_dsp_idle_apply_due holds"),
        # N4: the generation step moved under the build guard.
        ("idle-generation-under-build",
         "\tif (decision.build)\n\t\tapply_config(g_active_model, decision.build_rate_hz);\n"
         "\tg_config_generation += decision.generation_steps;\n",
         "\tif (decision.build) {\n\t\tapply_config(g_active_model, decision.build_rate_hz);\n"
         "\t\tg_config_generation += decision.generation_steps;\n\t}\n",
         "inside a decision guard"),
    )
    for label, old, new, needle in idle_apply_mutations:
        mutated = text.replace(old, new, 1)
        if label == "stamp-at-top":
            mutated = mutated.replace(
                ENQUEUE_MARKER + "\n\t{\n",
                ENQUEUE_MARKER + "\n\t{\n\t\tg_dsp_last_block_ms = millis();\n",
                1,
            )
            assert mutated.count("g_dsp_last_block_ms = millis();") == 1, (
                "dsp-idle-apply/stamp-at-top: mutation anchor moved"
            )
            needle = "directly after"
        assert mutated != text, f"dsp-idle-apply/{label}: mutation anchor moved"
        _expect_fail(
            assert_dsp_idle_apply_executes_the_decision,
            mutated,
            f"dsp-idle-apply/{label}",
            needle,
        )
    after_rate = text.replace(
        "\t" + DSP_IDLE_APPLY_CALL + "\n", "", 1
    ).replace(
        "\tsample_rate_service(now_ms);\n\t/* After the rate service:",
        "\tsample_rate_service(now_ms);\n"
        "\t" + DSP_IDLE_APPLY_CALL + "\n\t/* After the rate service:",
        1,
    )
    assert after_rate.count(DSP_IDLE_APPLY_CALL) == 1, (
        "dsp-idle-apply/service-after-rate: mutation anchor moved"
    )
    _expect_fail(
        assert_dsp_idle_apply_executes_the_decision,
        after_rate,
        "dsp-idle-apply/service-after-rate",
        "before sample_rate_service",
    )
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
        "update/apply rate truth",
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
    # Phase 4i: the gate evaluated before the rate service judges a stale
    # carrier and plan.
    gate_before_rate = text.replace(
        "\tsample_rate_service(now_ms);\n\t/* After the rate service:",
        "\ttx_gate_service();\n\t/* After the rate service:",
        1,
    ).replace(
        "\ttx_gate_service();\n\tinput_fault_service(now_ms);",
        "\tsample_rate_service(now_ms);\n\tinput_fault_service(now_ms);",
        1,
    )
    assert gate_before_rate != text, (
        "audio-cutover/gate-before-rate: mutation anchor moved"
    )
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        gate_before_rate,
        "audio-cutover/gate-before-rate",
        "evaluate the",
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
    never_ingested = text.replace("g_audio_ingest_since_reset = true;", "", 1)
    _expect_fail(
        assert_rate_service_orders_safe_gate,
        never_ingested,
        "audio-cutover/ingest-never-recorded",
        "never records that words",
    )
    no_applied_gate = text.replace("if (!g_clock_plan_applied) {", "if (false) {", 1)
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
    # The one-tab indent is what distinguishes the SD stager (settings_load,
    # Phase 4h) from the PUT stager's indented arms.
    origin_after_flag = text.replace(
        "\n\tg_pending_dsp_origin = DSP_ORIGIN_SD;\n", "\n", 1
    ).replace(
        "\n\tg_dsp_apply_pending = true;",
        "\n\tg_dsp_apply_pending = true;"
        "\n\tg_pending_dsp_origin = DSP_ORIGIN_SD;",
        1,
    )
    assert origin_after_flag != text, "dsp-origin mutation anchor moved"
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
    # The format moved to aius_optical_dsp_config_write (optical-app asserts
    # its document has no model_origin); here the selection body must not
    # add one either.
    origin_on_config_route = text.replace(
        '\theader.source = dsp_source_name(origin);\n',
        '\theader.source = dsp_source_name(origin);\n'
        '\t(void)"\\"model_origin\\":\\"put\\"";\n',
        1,
    )
    assert origin_on_config_route != text, "dsp-origin mutation anchor moved"
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
    entries = [(value, f"\t{name} = {value},\n")
               for name, value in EVENT_TYPE_VALUES.items()]
    # A retired value stays a comment in place of its member, as in the source.
    entries += [(value, f"\t/* {value} retired ({what}); never reused. */\n")
                for value, what in RETIRED_EVENT_TYPE_VALUES.items()]
    enum_body = "".join(line for _, line in sorted(entries, key=lambda e: e[0]))
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
          "static FLASHMEM void sd_overlay_event(void *, AiusSdOverlayEvent event,\n"
          "                                      const char *)\n"
          "{\n"
          "\trecord_event(AIUS_EVENT_SD_FLUSH_CRC_DROP, current_state(), 1u);\n"
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

    # checkpoint 7: no direct SD, services in order.
    good_service = (
        "static FLASHMEM bool platform_service(void *, const AiusRuntimeBudget *budget,\n"
        "                                      uint32_t now_ms)\n"
        "{\n"
        "\tif (budget == nullptr ||\n"
        "\t    budget->checkpoint != AIUS_BOOT_CHECKPOINT_SERVICE)\n"
        "\t\treturn false;\n"
        "\tadmission_sd_window_sample();\n"
        "\tsd_lifecycle_service(now_ms);\n"
        "\tg_audio_processor.dsp_idle_apply_service(millis());\n"
        "\tsample_rate_service(now_ms);\n"
        "\ttx_gate_service();\n"
        "\tinput_fault_service(now_ms);\n"
        "\tinput_dark_service(now_ms);\n"
        "\t(void)aius_recorder_bank_drain(&g_recorder_bank, 1u);\n"
        "\treturn true;\n"
        "}\n"
    )
    _expect_pass(assert_service_path_has_no_direct_sd, good_service, "service-sd/good")
    _expect_fail(
        assert_service_path_has_no_direct_sd,
        good_service.replace("\tsd_lifecycle_service(now_ms);\n",
                             "\tsd_lifecycle_service(now_ms);\n\tSD.exists(\"/x\");\n", 1),
        "service-sd/direct-primitive", "card primitive directly",
    )
    _expect_fail(
        assert_service_path_has_no_direct_sd,
        good_service.replace("\tsd_lifecycle_service(now_ms);\n", "", 1),
        "service-sd/nothing-scanned", "sd_lifecycle_service",
    )
    _expect_pass(assert_service_checkpoint_runs_in_order, good_service, "service-order/good")
    _expect_fail(
        assert_service_checkpoint_runs_in_order,
        good_service.replace(
            "\tg_audio_processor.dsp_idle_apply_service(millis());\n"
            "\tsample_rate_service(now_ms);\n",
            "\tsample_rate_service(now_ms);\n"
            "\tg_audio_processor.dsp_idle_apply_service(millis());\n", 1),
        "service-order/idle-apply-after-rate", "out of order",
    )
    _expect_fail(
        assert_service_checkpoint_runs_in_order,
        good_service.replace("\tsd_lifecycle_service(now_ms);\n",
                             "\tsd_lifecycle_service(now_ms);\n\treplay_refill_service(now_ms);\n", 1),
        "service-order/replay-back", "names replay",
    )
    _expect_fail(
        assert_service_checkpoint_runs_in_order,
        good_service.replace("AIUS_BOOT_CHECKPOINT_SERVICE", "AIUS_BOOT_CHECKPOINT_SD", 1),
        "service-order/wrong-checkpoint", "AIUS_BOOT_CHECKPOINT_SERVICE",
    )

    # setup() pins the input master once.
    good_pin = (
        "void setup(void)\n"
        "{\n"
        "\tAudioMemory(24);\n"
        "\tnoInterrupts();\n"
        "\tAiusSpdifOutputClock::set_master(false);\n"
        "\tAiusSpdifInputClock::set_master(true);\n"
        "\tinterrupts();\n"
        "}\n"
    )
    _expect_pass(assert_setup_pins_input_master_once, good_pin, "master-pin/good")
    _expect_fail(
        assert_setup_pins_input_master_once,
        good_pin.replace("\tnoInterrupts();\n", "", 1),
        "master-pin/unguarded", "critical section",
    )
    _expect_fail(
        assert_setup_pins_input_master_once,
        good_pin.replace("\tAiusSpdifInputClock::set_master(true);\n", "", 1),
        "master-pin/one-sided", "AiusSpdifInputClock::set_master",
    )
    _expect_fail(
        assert_setup_pins_input_master_once,
        good_pin + (
            "static FLASHMEM void clock_service(void)\n"
            "{\n\tAiusSpdifOutputClock::set_master(true);\n}\n"
        ),
        "master-pin/second-call-site", "call sites in the file",
    )
    _expect_fail(
        assert_setup_pins_input_master_once,
        good_pin.replace("AiusSpdifInputClock::set_master(true)",
                         "AiusSpdifInputClock::set_master(false)", 1),
        "master-pin/output-master", "the input is the master",
    )

    # source_changes counts carrier edges.
    good_gate = (
        "static FLASHMEM void tx_gate_service(void)\n"
        "{\n"
        "\tconst bool carrier = AudioInputSPDIF3::pllLocked();\n"
        "\tconst bool open = aius_tx_gate_permitted(carrier, g_clock_live_ready);\n"
        "\tg_tx_gate_open = open;\n"
        "\tif (carrier != g_tx_gate_carrier && g_audio_ingress_ready)\n"
        "\t\tg_audio_ingress.source_changes++;\n"
        "\tg_tx_gate_carrier = carrier;\n"
        "}\n"
    )
    _expect_pass(assert_source_changes_count_carrier_edges, good_gate, "source-edges/good")
    _expect_fail(
        assert_source_changes_count_carrier_edges,
        good_gate.replace("\t\tg_audio_ingress.source_changes++;\n", "\t\t;\n", 1),
        "source-edges/uncounted", "does not count a carrier edge",
    )
    _expect_fail(
        assert_source_changes_count_carrier_edges,
        good_gate.replace("\tg_tx_gate_carrier = carrier;\n", "", 1).replace(
            "\tif (carrier != g_tx_gate_carrier",
            "\tg_tx_gate_carrier = carrier;\n\tif (carrier != g_tx_gate_carrier", 1),
        "source-edges/latch-first", "before comparing it",
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
    # Phase 4i: the retired value 3 handed to a new type, or REPLAY_STOP back.
    retired_comment = "\t/* 3 retired (AIUS_EVENT_REPLAY_STOP (Phase 4i)); never reused. */\n"
    for label, member in (("types/retired-reused", "AIUS_EVENT_HDMI_LOSS"),
                          ("types/retired-restored", "AIUS_EVENT_REPLAY_STOP")):
        reused = good_edges.replace(retired_comment, f"\t{member} = 3,\n", 1)
        assert reused != good_edges, "types fixture lost its retired-value comment"
        _expect_fail(
            assert_event_types_are_allocated_not_renumbered,
            reused.replace(
                "\trecord_event(AIUS_EVENT_STATE, state, 0);\n",
                "\trecord_event(AIUS_EVENT_STATE, state, 0);\n"
                f"\trecord_event({member}, state, 0);\n", 1),
            label, "retired event type value",
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



# --------------------------------------------------------------------------
# Producer-yield source guards (aius-sample-rate-following.md Phase 4e).
# Read over every C/C++ source under firmware/src and firmware/lib, keyed by
# firmware-relative path, because the post-link walk (producer-isolation)
# cannot tell a windowed SD root from an unwindowed one in the binary
# (CR3-B1), and its pinned libc row rests on the name guard below (CR4-B1).
# --------------------------------------------------------------------------

PRODUCER_YIELD_SOURCE_DIRS = ("src", "lib")
PRODUCER_YIELD_SOURCE_SUFFIXES = (".c", ".cpp", ".h")
PRODUCER_YIELD_LIB = "lib/aius_producer_yield/"
PRODUCER_YIELD_SOURCE = "lib/aius_producer_yield/src/aius_producer_yield.c"
SD_ADAPTER_SOURCE = "src/aius_sd_teensy.cpp"
STORAGE_SOURCE = "src/aius_runtime_storage.cpp"
OPTICAL_MAIN_SOURCE = "src/optical_main.cpp"

# W2 windowed rows by source function: the storage execute (route_action,
# which inlines into the W2 row `advance`) and the raw scan (the replay
# listing and delete retired with Phase 4i).
W2_WINDOWED_SOURCES = frozenset({
    (STORAGE_SOURCE, "route_action"),
    (OPTICAL_MAIN_SOURCE, "app_read_directory"),
})
# The accepted unwindowed row: the OTA copy into PSRAM (CR-S9). Named so a
# window there fails with its reason.
W2_UNWINDOWED_SOURCES = frozenset({
    "ota_copy_staged_to_psram",
})
# Every function outside the adapter whose code names SD., File or FsFile
# is the source of a W2 row (W5): the two raw-access functions.
W2_SD_NAMING_SOURCES = frozenset({
    (OPTICAL_MAIN_SOURCE, "ota_copy_staged_to_psram"),
    (OPTICAL_MAIN_SOURCE, "app_read_directory"),
})
WINDOW_TOKEN = re.compile(
    r"\baius_producer_yield_window_(?:open|close)\b|\bProducerSdWindow\b"
)
WINDOW_DECLARATION = re.compile(
    r"\b(?:const\s+)?ProducerSdWindow\s+[A-Za-z_]\w*\s*;"
)
SD_NAMING = re.compile(r"\bSD\s*\.|\bFile\b|\bFsFile\b")
# W5 (2)'s libc row guard: no SD file becomes a stdio descriptor and no event
# responder is attached, so newlib's stream pointers and _write's Print call
# stay the only indirect sites in the allocator and stdio closure.
STDIO_FORBIDDEN_NAMES = (
    "Print", "Stream", "EventResponder", "vdprintf", "dprintf",
    "fopen", "fdopen", "funopen", "fopencookie",
)
STDIO_FORBIDDEN = re.compile(
    r"\b(" + "|".join(STDIO_FORBIDDEN_NAMES) + r")\b"
)
QUALIFIED_DEFINITION = re.compile(
    r"^[A-Za-z_][A-Za-z0-9_ \t*&<>,]*?\b((?:[A-Za-z_]\w*::)+~?[A-Za-z_]\w*)\s*\(",
    re.MULTILINE,
)
YIELD_POINT_CALL = "aius_producer_yield_point"
# Phase 4f: the status render's appends, each ending at a yield point (the
# render-append rows of producer-isolation's yield-root table).
RENDER_APPEND_YIELD_ROOTS = frozenset({
    (OPTICAL_MAIN_SOURCE, "status_append"),
    (STORAGE_SOURCE, "diag_append"),
})
# Phase 4g: the web cache's yield points (G3, G7): the send loop's body
# source and the pre-header verify's yield callback (their yield-root rows).
WEB_CACHE_YIELD_ROOTS = frozenset({
    ("src/aius_optical_app.cpp", "web_cache_body_chunk"),
    (OPTICAL_MAIN_SOURCE, "web_cache_verify_yield"),
})
YIELD_POINT_GUARDED = re.compile(
    r"if\s*\(\s*checkpoint\s*!=\s*AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE\s*\)"
    r"\s*\(void\)\s*aius_producer_yield_point\s*\(\s*\)\s*;"
)
NESTED_PASS_SHAPE = re.compile(
    r"\b(\w+)\s*=\s*aius_producer_yield_plat_ledger_checkpoint\s*\(\s*\)\s*;"
    r".*?aius_producer_yield_plat_ledger_record\s*\(\s*"
    r"AIUS_BOOT_CHECKPOINT_AUDIO\s*\)\s*;"
    r"\s*aius_producer_yield_plat_run_pass\s*\(\s*\)\s*;"
    r"\s*aius_producer_yield_plat_ledger_record\s*\(\s*\1\s*\)\s*;",
    re.DOTALL,
)


def read_producer_yield_sources():
    """firmware-relative path -> text for every C/C++ source the guards read."""
    sources = {}
    for directory in PRODUCER_YIELD_SOURCE_DIRS:
        for path in sorted((FIRMWARE_ROOT / directory).rglob("*")):
            if path.is_file() and path.suffix in PRODUCER_YIELD_SOURCE_SUFFIXES:
                relative = path.relative_to(FIRMWARE_ROOT).as_posix()
                sources[relative] = path.read_text(errors="replace")
    return sources


def all_function_definitions(text):
    """function_definitions() plus qualified `Class::member` definitions, so a
    member function naming SD cannot slip past the W5 source guard."""
    definitions = dict(function_definitions(text))
    clean = without_comments_and_literals(text)
    for match in QUALIFIED_DEFINITION.finditer(clean):
        opening = clean.find("(", match.start(1) + len(match.group(1)))
        closing = matching_delimiter(clean, opening, "(", ")")
        cursor = closing + 1
        while cursor < len(clean) and clean[cursor] in " \t\r\n":
            cursor += 1
        if cursor >= len(clean) or clean[cursor] != "{":
            continue
        body_end = matching_delimiter(clean, cursor, "{", "}")
        definitions[match.group(1)] = text[cursor + 1: body_end]
    return definitions


def _definitions_by_source(sources, skip=()):
    for path, text in sorted(sources.items()):
        if path.startswith(tuple(skip)):
            continue
        for name, body in all_function_definitions(text).items():
            yield path, name, without_comments_and_literals(body)


def assert_sd_window_only_in_w2_windowed_sources(sources):
    misplaced = []
    found = set()
    for path, name, body in _definitions_by_source(
        sources, skip=(PRODUCER_YIELD_LIB,)
    ):
        if not WINDOW_TOKEN.search(body):
            continue
        if (path, name) in W2_WINDOWED_SOURCES:
            found.add((path, name))
            continue
        reason = (
            " (an accepted UNWINDOWED SD root)"
            if name in W2_UNWINDOWED_SOURCES else ""
        )
        misplaced.append(f"{path}:{name}{reason}")
    assert not misplaced, (
        "the SD window flag is set outside the W2 windowed sources: "
        + ", ".join(misplaced)
    )
    missing = sorted(
        f"{path}:{name}" for path, name in W2_WINDOWED_SOURCES - found
    )
    assert not missing, f"W2 windowed sources lost their SD window: {missing}"
    storage = without_comments_and_literals(
        all_function_definitions(sources[STORAGE_SOURCE])["route_action"]
    )
    opened = storage.find("aius_producer_yield_window_open(")
    executed = storage.find("aius_sd_teensy_execute(")
    closed = storage.find("aius_producer_yield_window_close(")
    assert -1 < opened < executed < closed, (
        "route_action must open the SD window before aius_sd_teensy_execute "
        "and close it after"
    )
    main_definitions = all_function_definitions(sources[OPTICAL_MAIN_SOURCE])
    for path, name in sorted(W2_WINDOWED_SOURCES):
        if path != OPTICAL_MAIN_SOURCE:
            continue
        body = without_comments_and_literals(main_definitions[name])
        declaration = WINDOW_DECLARATION.search(body)
        first_sd = SD_NAMING.search(body)
        assert declaration is not None, (
            f"{name} names ProducerSdWindow but declares no window object"
        )
        assert first_sd is None or declaration.start() < first_sd.start(), (
            f"{name} touches the card before its ProducerSdWindow is declared"
        )
    clean_main = without_comments_and_literals(sources[OPTICAL_MAIN_SOURCE])
    assert re.search(
        r"ProducerSdWindow\s*\(\s*\)\s*\{\s*aius_producer_yield_window_open"
        r"\s*\(\s*\)\s*;\s*\}", clean_main
    ) and re.search(
        r"~\s*ProducerSdWindow\s*\(\s*\)\s*\{\s*aius_producer_yield_window_close"
        r"\s*\(\s*\)\s*;\s*\}", clean_main
    ), "ProducerSdWindow must open in its constructor and close in its destructor"


def assert_no_yield_point_at_epoch_complete(sources):
    callers = []
    for path, name, body in _definitions_by_source(
        sources, skip=(PRODUCER_YIELD_LIB,)
    ):
        if YIELD_POINT_CALL + "(" in body:
            callers.append((path, name, body))
    expected = sorted({(OPTICAL_MAIN_SOURCE, "platform_checkpoint")}
                      | RENDER_APPEND_YIELD_ROOTS | WEB_CACHE_YIELD_ROOTS)
    assert sorted((path, name) for path, name, _ in callers) == expected, (
        "aius_producer_yield_point's callers must be exactly "
        "platform_checkpoint, the Phase 4f render appends and the Phase 4g "
        "web cache yields (the yield-root table): "
        + ", ".join(f"{path}:{name}" for path, name, _ in callers)
    )
    for path, name, body in callers:
        assert body.count(YIELD_POINT_CALL + "(") == 1, (
            f"{name} calls the yield point more than once"
        )
    body = next(b for p, n, b in callers if n == "platform_checkpoint")
    assert YIELD_POINT_GUARDED.search(body), (
        "platform_checkpoint's yield point is not guarded by "
        "`checkpoint != AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE` (round 1 B2: "
        "nothing runs after the EPOCH_COMPLETE commit)"
    )


def assert_sd_naming_functions_are_w2_sources(sources):
    offenders = []
    for path, name, body in _definitions_by_source(
        sources, skip=(SD_ADAPTER_SOURCE, PRODUCER_YIELD_LIB)
    ):
        if SD_NAMING.search(body) and (path, name) not in W2_SD_NAMING_SOURCES:
            offenders.append(f"{path}:{name}")
    assert not offenders, (
        "functions naming SD., File or FsFile outside aius_sd_teensy.cpp that "
        "are not W2 row sources (add the row, or route the access through the "
        "adapter): " + ", ".join(offenders)
    )


def assert_no_stdio_stream_names(sources):
    offenders = []
    for path, text in sorted(sources.items()):
        clean = without_comments_and_literals(text)
        for match in STDIO_FORBIDDEN.finditer(clean):
            line = clean.count("\n", 0, match.start()) + 1
            offenders.append(f"{path}:{line} {match.group(1)}")
    assert not offenders, (
        "a firmware source names a stdio stream or event-responder entry "
        "(the W5 (2) pinned libc row rests on none doing so): "
        + ", ".join(offenders)
    )


def assert_nested_pass_ledger_wrapper(sources):
    library = sources[PRODUCER_YIELD_SOURCE]
    definitions = function_definitions(library)
    assert "nested_pass" in definitions, "nested_pass is missing"
    nested = without_comments_and_literals(definitions["nested_pass"])
    assert NESTED_PASS_SHAPE.search(nested), (
        "nested_pass must save the ledger checkpoint, record AUDIO, run the "
        "pass and record the saved checkpoint, adjacently (E5)"
    )
    assert "return" not in nested, "nested_pass has an early exit (E5)"
    runners = []
    for path, name, body in _definitions_by_source(sources):
        count = body.count("aius_producer_yield_plat_run_pass(")
        runners.extend([f"{path}:{name}"] * count)
    assert runners == [f"{PRODUCER_YIELD_SOURCE}:nested_pass"], (
        "the nested pass must run only inside nested_pass's ledger wrapper: "
        + ", ".join(runners)
    )
    record = without_comments_and_literals(
        function_definitions(sources[OPTICAL_MAIN_SOURCE])[
            "aius_producer_yield_plat_ledger_record"
        ]
    )
    recorded = record.find("aius_boot_record_checkpoint(")
    committed = record.find("aius_boot_storage_teensy_commit(")
    assert -1 < recorded < committed, (
        "aius_producer_yield_plat_ledger_record must record and then commit "
        "the retained ledger"
    )


STACK_BASE_MARKER = "uint32_t *stack_paint_base(void)"
STACK_PAINT_MARKER = "void stack_paint(void)"
STACK_SCAN_MARKER = "void stack_scan(uint32_t now_ms)"
STACK_TRAP_BYTES = re.compile(r"\bkStackTrapBytes\s*=\s*32u?\s*;")
STACK_BASE_SHAPE = re.compile(
    r"&\s*_ebss\s*\)\s*\)?\s*&\s*~\s*\(\s*kStackTrapBytes\s*-\s*1u?\s*\)\s*\)"
    r"\s*\+\s*kStackTrapBytes"
)


def assert_stack_paint_skips_the_mpu_trap(sources):
    """The core's configure_cache() maps a 32 B NOACCESS MPU region over the
    32 B block containing _ebss (cores/teensy4/startup.c:313-314), so a paint
    or scan that touches &_ebss faults with PRIMASK set: a boot loop. Every
    &_ebss in optical_main.cpp lives in stack_paint_base(), which rounds down
    to the trap block and steps over it; the paint and the scan start there."""
    text = sources[OPTICAL_MAIN_SOURCE]
    clean = without_comments_and_literals(text)
    assert STACK_TRAP_BYTES.search(clean), "kStackTrapBytes must be 32"
    base = without_comments_and_literals(function_body(text, STACK_BASE_MARKER))
    assert STACK_BASE_SHAPE.search(base), (
        "stack_paint_base() must round _ebss down to the 32 B trap block and "
        "step over it"
    )
    for marker in (STACK_PAINT_MARKER, STACK_SCAN_MARKER):
        body = without_comments_and_literals(function_body(text, marker))
        assert "_ebss" not in body, (
            f"{marker} addresses _ebss directly: the MPU stack trap faults"
        )
        assert "stack_paint_base()" in body, (
            f"{marker} must start at stack_paint_base()"
        )
    uses = len(re.findall(r"&\s*_ebss\b", clean))
    assert uses == len(re.findall(r"&\s*_ebss\b", base)), (
        "&_ebss is used outside stack_paint_base()"
    )


def assert_audio_service_run_is_bracketed(sources):
    """Every regular producer pass is a visit (in-producer flag set, pend
    disarmed); the only unbracketed aius_audio_service_run is the nested pass's
    platform hook, which only nested_pass calls (ledger guard)."""
    offenders = []
    for path, text in sorted(sources.items()):
        if path == OPTICAL_MAIN_SOURCE or path.startswith(
            "lib/aius_audio_service/"
        ):
            continue
        if call_count(text, "aius_audio_service_run"):
            offenders.append(path)
    assert not offenders, (
        "aius_audio_service_run called outside optical_main.cpp: "
        + ", ".join(offenders)
    )
    text = sources[OPTICAL_MAIN_SOURCE]
    total = call_count(text, "aius_audio_service_run")
    seen = 0
    bracketed = re.compile(
        r"aius_producer_yield_visit_begin\s*\(\s*\)\s*;\s*"
        r"(?:const\s+AiusStatus\s+\w+\s*=\s*|\(void\)\s*)?"
        r"aius_audio_service_run\s*\([^;]*\)\s*;\s*"
        r"aius_producer_yield_visit_end\s*\(\s*\)\s*;"
    )
    for name, body in function_definitions(text).items():
        clean = without_comments_and_literals(body)
        calls = clean.count("aius_audio_service_run(")
        if not calls:
            continue
        seen += calls
        if name == "aius_producer_yield_plat_run_pass":
            assert calls == 1, "plat_run_pass runs more than one pass"
            continue
        assert len(bracketed.findall(clean)) == calls, (
            f"{name}: aius_audio_service_run not bracketed by "
            "aius_producer_yield_visit_begin/visit_end"
        )
    assert seen == total, (
        "an aius_audio_service_run call sits outside any recognised function "
        "definition in optical_main.cpp"
    )


# --------------------------------------------------------------------------
# Phase 4f admission source guards (aius-sample-rate-following.md F2, F9,
# F11): every root SD job start consults sd_fifo_defer(), the storage root
# primitive step consults it and a drain does not, OTA staging is exempt
# before any margin test, live_owns_ring() is the one ownership derivation,
# and the status render appends only through its yielding appends.
# --------------------------------------------------------------------------

APP_SOURCE = "src/aius_optical_app.cpp"
ROOT_SD_START_CALLS = ("aius_runtime_teensy_start_storage_job(", "aius_sd_job_start(")
FIFO_DEFER_CALL = "sd_fifo_defer()"
# Phase 4g G8 (owner, 2026-10-10): the fill's one isolated predicate. It must
# be exactly sd_fifo_defer(), and it counts as a consult of it.
FILL_DEFER_NAME = "web_cache_fill_defer"
FILL_DEFER_CALL = FILL_DEFER_NAME + "()"
FILL_STEP_HOOK = "aius_runtime_storage_fill_step_defer"
# Round 4: a root job is held only at its start gate. The storage service's
# ownerless branch (a drain or a ROOT_RUNNING job) advances one primitive every
# epoch, so a started root job never parks as ROOT_RUNNING. Phase 4g G8 is the
# one exception: an uncancelled, non-draining CACHE_FILL root job's step waits
# while the platform's fill hook holds; no other job is ever gated.
# Phase 4h: pw-storage hoisted the ownerless branch into advance_ownerless(),
# a guard-return over the same predicate (the S8 STORE sink steps beside it).
STORAGE_OWNERLESS_ADVANCE = re.compile(
    r"if\s*\(\s*\(\s*!\s*service\s*->\s*draining\s*&&\s*"
    r"service\s*->\s*root_state\s*!=\s*AIUS_RUNTIME_STORAGE_ROOT_RUNNING\s*\)\s*"
    r"\|\|\s*service\s*->\s*terminal_seen\s*\)\s*return\s*;\s*"
    r"const\s+bool\s+fill_paused\s*=\s*"
    r"!\s*service\s*->\s*draining\s*&&\s*"
    r"service\s*->\s*root_state\s*==\s*AIUS_RUNTIME_STORAGE_ROOT_RUNNING\s*&&\s*"
    r"service\s*->\s*job\s*\.\s*kind\s*==\s*AIUS_SD_JOB_CACHE_FILL\s*&&\s*"
    r"!\s*service\s*->\s*root_cancelling\s*&&\s*"
    r"aius_runtime_storage_fill_step_defer\s*\(\s*&\s*service\s*->\s*job\s*\)\s*;\s*"
    r"if\s*\(\s*!\s*fill_paused\s*\)\s*"
    r"\(void\)\s*advance\s*\(\s*service\s*,\s*now_ms\s*,\s*false\s*\)\s*;"
)
OWNERSHIP_BODY = re.compile(
    r"^\s*return\s+g_aius_spdif_tx24\s*\.\s*gate_open\s*!=\s*0u\s*&&\s*"
    r"g_tx_gate_carrier\s*;\s*$"
)
# One statement conjoining the ring's gate flag and the latched carrier.
OWNERSHIP_EXPRESSION = re.compile(
    r"\bgate_open\b[^;{}]*&&[^;{}]*\bg_tx_gate_carrier\b"
    r"|\bg_tx_gate_carrier\b[^;{}]*&&[^;{}]*\bgate_open\b"
)
RENDER_ROOT = "http_status_json"


def _clean_definitions(text):
    return {
        name: without_comments_and_literals(body)
        for name, body in all_function_definitions(text).items()
    }


def assert_root_sd_starts_consult_fifo_defer(sources):
    """F2: each optical_main function that starts a root SD job is reached
    only from callers that consult sd_fifo_defer() before the call."""
    defs = _clean_definitions(sources[OPTICAL_MAIN_SOURCE])
    # G8: the fill's isolated predicate is sd_fifo_defer() and nothing else,
    # and the storage service's fill step hook decides through it.
    fill_defer = defs.get(FILL_DEFER_NAME)
    if fill_defer is not None:
        assert re.fullmatch(r"\s*return\s+sd_fifo_defer\s*\(\s*\)\s*;\s*",
                            fill_defer), (
            f"{FILL_DEFER_NAME}() must be exactly `return sd_fifo_defer();` "
            "(G8, the fill's one predicate)"
        )
    step_hook = defs.get(FILL_STEP_HOOK)
    if step_hook is not None:
        assert re.fullmatch(rf"\s*return\s+{FILL_DEFER_NAME}\s*\(\s*\)\s*;\s*",
                            step_hook), (
            f"{FILL_STEP_HOOK} must return {FILL_DEFER_CALL} (G8: each fill "
            "primitive consults the same predicate as the fill start)"
        )

    def consult_at(body):
        hits = [body.find(token) for token in (FIFO_DEFER_CALL, FILL_DEFER_CALL)
                if token != FILL_DEFER_CALL or fill_defer is not None]
        hits = [h for h in hits if h > -1]
        return min(hits) if hits else -1

    starters = sorted(
        name for name, body in defs.items()
        if any(call in body for call in ROOT_SD_START_CALLS)
    )
    assert starters, "no root SD job start site found in optical_main.cpp"
    for starter in starters:
        # A starter that consults the predicate before its own start call is
        # gated by itself (4g's fill start, G8), whatever its callers do.
        own = defs[starter]
        own_call = min(own.find(c) for c in ROOT_SD_START_CALLS if c in own)
        if -1 < consult_at(own) < own_call:
            continue
        if starter == "sd_flush_start_job":
            # 4h: reached only through g_sd_flush_ops.start, which
            # aius_optical_sd_flush_start calls after storage_ready
            # (sd_flush_storage_ready); sd-overlay-wiring checks that order.
            assert FIFO_DEFER_CALL in defs.get("sd_flush_storage_ready", ""), (
                "sd_flush_storage_ready must consult sd_fifo_defer() (F2)"
            )
            continue
        callers = sorted(
            name for name, body in defs.items()
            if name != starter and re.search(rf"\b{starter}\s*\(", body)
        )
        assert callers, f"root SD start {starter} has no caller in optical_main.cpp"
        for caller in callers:
            body = defs[caller]
            call = re.search(rf"\b{starter}\s*\(", body).start()
            consult = body.find(FIFO_DEFER_CALL)
            assert -1 < consult < call, (
                f"{caller} starts a root SD job ({starter}) without consulting "
                "sd_fifo_defer() first (F2)"
            )
    storage_text = without_comments_and_literals(sources[STORAGE_SOURCE])
    assert "sd_fifo_defer" not in storage_text, (
        "the storage service must not consult sd_fifo_defer(): a root job is "
        "held only at its start gate, a started one steps every epoch (round 4)"
    )
    storage = _clean_definitions(sources[STORAGE_SOURCE])
    stepping = [n for n, b in storage.items() if STORAGE_OWNERLESS_ADVANCE.search(b)]
    assert stepping == ["advance_ownerless"], (
        "advance_ownerless must read `if ((!service->draining && "
        "service->root_state != AIUS_RUNTIME_STORAGE_ROOT_RUNNING) || "
        "service->terminal_seen) return; const bool fill_paused = "
        "!service->draining && service->root_state == "
        "AIUS_RUNTIME_STORAGE_ROOT_RUNNING && service->job.kind == "
        "AIUS_SD_JOB_CACHE_FILL && !service->root_cancelling && "
        "aius_runtime_storage_fill_step_defer(&service->job); if (!fill_paused) "
        "(void)advance(service, now_ms, false);` (a started root job is never "
        "held, its start gate alone defers, except an uncancelled CACHE_FILL "
        "per G8): " + ", ".join(stepping)
    )
    assert "advance_ownerless(" in storage.get("aius_runtime_storage_service", ""), (
        "aius_runtime_storage_service must step the ownerless job through "
        "advance_ownerless()"
    )


def assert_round3_admission_wiring(sources):
    """4f delivery audit round 3. F-B2: bring-up root jobs are exempt from
    sd_fifo_defer(); F-S1: a shed
    SD_START disarms the upload staging it armed; F-S2: the hook tells the
    portable decision whether the consult re-issues the held start; F-S3: the
    mode switch charges the pending SD window sample to the old mode."""
    defs = _clean_definitions(sources[OPTICAL_MAIN_SOURCE])
    admit = defs.get("aius_runtime_http_admit", "")
    assert re.search(r"\bsd_disarm_shed_staging\s*\(\s*query\s*->\s*sd_job\s*\)",
                     admit), (
        "aius_runtime_http_admit must disarm a shed SD_START's staging (F-S1)"
    )
    assert "deferred_start_present" in admit, (
        "aius_runtime_http_admit must pass the held-start state to the "
        "SD-start decision (F-S2)"
    )
    disarm = defs.get("sd_disarm_shed_staging", "")
    for clear in (r"g_dsp_upload_pending\s*=\s*false",
                  r"g_config_upload_pending\s*=\s*false",
                  r"g_sd_commit_target\s*=\s*SD_COMMIT_NONE"):
        assert re.search(clear, disarm), (
            f"sd_disarm_shed_staging must clear `{clear}` (F-S1)"
        )
    mode = defs.get("app_sd_set_mode", "")
    sample = mode.find("admission_sd_window_sample(")
    switch = mode.find("aius_sd_teensy_set_mode(")
    assert -1 < sample < switch, (
        "app_sd_set_mode must take admission_sd_window_sample() before "
        "aius_sd_teensy_set_mode() (F-S3)"
    )


def assert_ota_staging_exempt_before_margin(sources):
    """F9: OTA staging chunk jobs never consult the FIFO deferral."""
    app = _clean_definitions(sources[APP_SOURCE])
    body = app.get("aius_optical_admission_sd_start", "")
    exempt = body.find("aius_optical_app_sd_job_is_ota_staging(")
    margin = body.find("aius_optical_admission_short(")
    assert -1 < exempt < margin, (
        "aius_optical_admission_sd_start must exempt OTA staging before any "
        "margin test (F9)"
    )
    assert FIFO_DEFER_CALL not in without_comments_and_literals(
        sources[APP_SOURCE]
    ), "aius_optical_app.cpp must not consult sd_fifo_defer() (the OTA chunk route)"


def assert_single_ownership_predicate(sources):
    """F11, CR-B1: live_owns_ring() is the one derivation and both consumers
    (admission and sd_fifo_defer) read it through admit_sample()."""
    defs = _clean_definitions(sources[OPTICAL_MAIN_SOURCE])
    assert OWNERSHIP_BODY.match(defs.get("live_owns_ring", "")), (
        "live_owns_ring() must be the ring's gate flag with the latched carrier"
    )
    readers = sorted(n for n, b in defs.items()
                     if n != "live_owns_ring" and re.search(r"\blive_owns_ring\s*\(", b))
    assert readers == ["admit_sample"], (
        f"only admit_sample() may read live_owns_ring(): {readers}"
    )
    for consumer in ("aius_runtime_http_admit", "sd_fifo_defer"):
        body = defs.get(consumer, "")
        assert "admit_sample(" in body, f"{consumer} must read admit_sample()"
        assert "gate_open" not in body and "g_tx_gate_carrier" not in body, (
            f"{consumer} derives ownership itself"
        )
    derived = sorted(
        n for n, b in defs.items()
        if n != "live_owns_ring" and OWNERSHIP_EXPRESSION.search(b)
    )
    assert not derived, (
        "ownership derived outside live_owns_ring() (gate flag with carrier): "
        + ", ".join(derived)
    )


def assert_status_render_appends_yield(sources):
    """F5: every append in the status render ends at a yield point, so no
    render section formats through the plain http_json_append or vsnprintf."""
    defs = _clean_definitions(sources[OPTICAL_MAIN_SOURCE])
    assert RENDER_ROOT in defs, f"{RENDER_ROOT} not found"
    seen, todo = set(), [RENDER_ROOT]
    while todo:
        name = todo.pop()
        if name in seen or name not in defs:
            continue
        seen.add(name)
        todo.extend(c for c in re.findall(r"\b(\w+)\s*\(", defs[name])
                    if c in defs and c not in seen and c != "status_append")
    bare = sorted(n for n in seen
                  if re.search(r"\b(http_json_append|v?snprintf)\s*\(", defs[n]))
    assert not bare, (
        "status render sections append without a yield (use status_append): "
        + ", ".join(bare)
    )
    storage = _clean_definitions(sources[STORAGE_SOURCE])
    diag = storage.get("aius_runtime_storage_diag_json", "")
    assert "diag_append(" in diag and not re.search(r"\bv?snprintf\s*\(", diag), (
        "aius_runtime_storage_diag_json must append only through diag_append"
    )


NOTE_BRINGUP = re.compile(
    r"\baius_optical_admission_note_bringup_loaded\s*\(\s*&\s*g_http_admission\s*\)"
)


def assert_bringup_latch_at_load_dsp_end(sources):
    """F-B2 (round 4): sd_fifo_defer() holds nothing until bring-up's first
    LOAD_DSP step has ended. The latch is set unconditionally (found, missing
    or faulted) in sd_step_complete()'s SD_STEP_LOAD_DSP arm and nowhere else,
    sd_fifo_defer() reads it through the portable predicate on
    g_http_admission, and the predicate returns false while it is clear,
    before any margin test or hold count."""
    defs = _clean_definitions(sources[OPTICAL_MAIN_SOURCE])
    sites = sorted(n for n, b in defs.items() if NOTE_BRINGUP.search(b))
    assert sites == ["sd_step_complete"], (
        "the bring-up latch must be set only in sd_step_complete (F-B2): "
        + ", ".join(sites)
    )
    complete = defs["sd_step_complete"]
    assert len(NOTE_BRINGUP.findall(complete)) == 1, (
        "sd_step_complete must set the bring-up latch exactly once (F-B2)"
    )
    arm_at = complete.find("case SD_STEP_LOAD_DSP:")
    assert arm_at != -1, "sd_step_complete has no SD_STEP_LOAD_DSP arm"
    arm = complete[arm_at + len("case SD_STEP_LOAD_DSP:"):]
    arm = arm[:min(i for i in (arm.find("case "), arm.find("default:"), len(arm))
                   if i != -1)]
    note = NOTE_BRINGUP.search(arm)
    assert note, (
        "the bring-up latch must be set at the end of the LOAD_DSP step "
        "(sd_step_complete's SD_STEP_LOAD_DSP arm) (F-B2)"
    )
    before = arm[:note.start()]
    assert "return" not in before and not re.search(
        r"\b(if|while|for|switch)\b|\?|&&|\|\|", before
    ), (
        "the LOAD_DSP arm must set the bring-up latch unconditionally, so a "
        "missing or faulted dsp.json does not hold it open (F-B2)"
    )
    defer = defs.get("sd_fifo_defer", "")
    assert re.search(
        r"\baius_optical_admission_sd_fifo_defer\s*\(\s*&\s*g_http_admission\s*,", defer
    ), "sd_fifo_defer() must decide through aius_optical_admission_sd_fifo_defer(&g_http_admission, ...) (F-B2)"
    app = _clean_definitions(sources[APP_SOURCE])
    predicate = app.get("aius_optical_admission_sd_fifo_defer", "")
    gate = re.search(
        r"if\s*\([^;{}]*(\bbringup_loaded\s*==\s*0u?\b|!\s*\w+\s*->\s*bringup_loaded\b)"
        r"[^;{}]*\)\s*return\s+false\s*;",
        predicate,
    )
    short = predicate.find("aius_optical_admission_short(")
    count = predicate.find("sd_fifo_defers")
    assert gate and -1 < short and gate.start() < short and gate.start() < count, (
        "aius_optical_admission_sd_fifo_defer must return false while the "
        "bring-up latch is clear, before the margin test and the hold count (F-B2)"
    )
    setter = app.get("aius_optical_admission_note_bringup_loaded", "")
    assert re.search(r"\bbringup_loaded\s*=\s*(1u?|true)\s*;", setter), (
        "aius_optical_admission_note_bringup_loaded must set the latch (F-B2)"
    )


def assert_orphan_staging_dropped(sources):
    """Round 4: an armed upload staging whose request can no longer start (a
    network teardown during a DEFER) is disarmed, right after the commit arm
    settles and before any root start, and never while its job may still
    start: the transaction in DISPATCH, SD_START or SD_TRANSFER, a held or a
    queued start, or the job already seen."""
    defs = _clean_definitions(sources[OPTICAL_MAIN_SOURCE])
    life = defs.get("sd_lifecycle_service", "")
    settle = life.find("sd_settle_http_write(")
    drop = life.find("sd_drop_orphan_staging(")
    start = life.find("sd_start_step(")
    first_return = life.find("return")
    assert -1 < settle < drop < start and (first_return == -1 or drop < first_return), (
        "sd_lifecycle_service must call sd_drop_orphan_staging() right after "
        "sd_settle_http_write() and before any return or root start (round 4)"
    )
    body = defs.get("sd_drop_orphan_staging", "")
    call = re.search(
        r"!\s*aius_optical_app_sd_staging_orphaned\s*\(\s*&\s*g_runtime\s*\.\s*"
        r"network_http\s*\.\s*http\s*,\s*g_runtime\s*\.\s*network_http\s*\.\s*"
        r"deferred_start_present\s*,\s*g_runtime\s*\.\s*storage_action_present\s*\)",
        body,
    )
    guard = re.search(
        r"if\s*\(\s*g_sd_commit_target\s*==\s*SD_COMMIT_NONE\s*\|\|\s*"
        r"g_sd_commit_writing\s*\|\|\s*g_sd_http_job_seen\s*\|\|\s*"
        r"!\s*aius_optical_app_sd_staging_orphaned\s*\([^;]*\)\s*\)\s*return\s*;",
        body,
    )
    assert call and guard, (
        "sd_drop_orphan_staging must return unless a staging is armed, not "
        "writing, its job never seen, and aius_optical_app_sd_staging_orphaned("
        "&g_runtime.network_http.http, deferred_start_present, "
        "storage_action_present) holds (round 4)"
    )
    for clear in (r"g_dsp_upload_pending\s*=\s*false",
                  r"g_config_upload_pending\s*=\s*false",
                  r"g_sd_commit_target\s*=\s*SD_COMMIT_NONE"):
        assert re.search(clear, body[guard.end():]), (
            f"sd_drop_orphan_staging must clear `{clear}` (round 4)"
        )
    app = _clean_definitions(sources[APP_SOURCE])
    orphan = app.get("aius_optical_app_sd_staging_orphaned", "")
    held = re.search(
        r"if\s*\([^;{}]*\bstart_held\b[^;{}]*\bstart_queued\b[^;{}]*\)\s*return\s+false\s*;",
        orphan,
    )
    assert held, (
        "aius_optical_app_sd_staging_orphaned must return false while a start "
        "is held or queued (round 4)"
    )
    for state in ("AIUS_HTTP_TX_DISPATCH", "AIUS_HTTP_TX_SD_START",
                  "AIUS_HTTP_TX_SD_TRANSFER"):
        assert re.search(rf"\bstate\s*!=\s*{state}\b", orphan[held.end():]), (
            f"aius_optical_app_sd_staging_orphaned must keep a staging while "
            f"the transaction is in {state} (round 4)"
        )


HTTP_SERVICE_SOURCE = "src/aius_runtime_network_http.cpp"
DIRECT_CARD_OPS = ("app_read_directory",)
ROUTE_FILL_WAIT = re.compile(
    r"!\s*sd_start\s*&&\s*aius_optical_app_route_reads_card\s*\(\s*query\s*->\s*request\s*\)"
    r"\s*&&\s*aius_optical_web_fill_route\s*\(\s*&\s*g_web_fill\s*,\s*"
    r"web_cache_fill_in_flight\s*\(\s*\)\s*,[^;]*\)\s*\)\s*\{\s*"
    r"if\s*\(\s*fill_cancel\s*\)\s*web_cache_fill_cancel\s*\(\s*\)\s*;\s*"
    r"return\s+AIUS_HTTP_VERDICT_DEFER\s*;\s*\}"
)
FOLLOW_CARD = re.compile(
    r"if\s*\(\s*aius_optical_web_fill_follow_card\s*\(\s*&\s*g_web_fill_walk\s*\.\s*ready_seen\s*,"
    r"\s*&\s*g_web_fill_walk\s*\.\s*mounts_seen\s*,\s*g_sd_ready\s*,\s*storage\s*\.\s*target\s*\.\s*"
    # Phase 4h: the arm may be a block that also sets the card-loss hold.
    r"mounted\s*,\s*storage\s*\.\s*target\s*\.\s*mounts\s*\)\s*\)\s*\{?\s*web_cache_invalidate_all\s*\(\s*\)\s*;"
)


def assert_fill_route_and_card_wiring(sources):
    """Phase 4g delivery audit. Finding 3: a ROUTE DEFER holds the DISPATCH
    (it was an ADMIT): dispatch_route() records the hold from the verdict and
    returns before the application on DEFER, the service re-issues a held
    dispatch before any transaction step, the admission sends a direct-card
    route through the fill wait (one cancel entry) before 4f's route verdict,
    app_dispatch_end() ends it, and the HTTP canceller is never dropped under a
    held dispatch. (Finding 2, the replay start's FILL_BUSY gate, retired
    with replay in Phase 4i.)
    Finding 6: the walk follows the adapter's trusted mount, and each
    direct-card op clears the driver code first and settles it after."""
    http = _clean_definitions(sources[HTTP_SERVICE_SOURCE])
    route = http.get("dispatch_route", "")
    flag = re.search(r"\bservice\s*->\s*deferred_dispatch_present\s*=\s*verdict\s*==\s*"
                     r"AIUS_HTTP_VERDICT_DEFER\s*;", route)
    hold = re.search(r"if\s*\(\s*verdict\s*==\s*AIUS_HTTP_VERDICT_DEFER\s*\)\s*return\s*;", route)
    app_call = route.find("aius_optical_app_http_dispatch(")
    assert flag and hold and -1 < hold.start() < app_call, (
        "dispatch_route must hold a DEFERred DISPATCH: `service->deferred_dispatch_present "
        "= verdict == AIUS_HTTP_VERDICT_DEFER;` and `if (verdict == "
        "AIUS_HTTP_VERDICT_DEFER) return;` before aius_optical_app_http_dispatch (4g F3)"
    )
    service = http.get("aius_runtime_http_service", "")
    held = re.search(r"if\s*\(\s*service\s*->\s*deferred_dispatch_present\s*\)\s*\{[^{}]*"
                     r"\bdispatch_route\s*\([^;]*;\s*return\s+true\s*;\s*\}", service)
    step = service.find("aius_http_transaction_step(")
    fresh = re.search(r"case\s+AIUS_HTTP_ACTION_DISPATCH\s*:[^:]*\bdispatch_route\s*\(", service)
    assert held and fresh and -1 < held.start() < step < fresh.start(), (
        "aius_runtime_http_service must re-issue a held DISPATCH through dispatch_route "
        "before the transaction step, and route a fresh DISPATCH through it (4g F3)"
    )
    defs = _clean_definitions(sources[OPTICAL_MAIN_SOURCE])
    admit = defs.get("aius_runtime_http_admit", "")
    wait = ROUTE_FILL_WAIT.search(admit)
    verdict = admit.find("aius_optical_admission_route(")
    assert wait and -1 < wait.start() < verdict, (
        "aius_runtime_http_admit must DEFER a direct-card route through "
        "aius_optical_web_fill_route(&g_web_fill, web_cache_fill_in_flight(), ...) "
        "and the one cancel entry before 4f's route verdict (4g F3)"
    )
    assert re.search(r"\baius_optical_web_fill_route_end\s*\(\s*&\s*g_web_fill\s*,",
                     defs.get("app_dispatch_end", "")), (
        "app_dispatch_end must end a direct-card route (its completion extends the hold, 4g F3)"
    )
    settle = defs.get("web_cache_fill_settle", "")
    drop = settle.find("aius_optical_web_fill_drop_http_canceller(")
    assert -1 < settle.find("deferred_dispatch_present") < drop, (
        "web_cache_fill_settle must not drop the HTTP canceller under a held DISPATCH (4g F3)"
    )
    assert FOLLOW_CARD.search(settle), (
        "web_cache_fill_settle must follow the card through aius_optical_web_fill_follow_card"
        "(&g_web_fill_walk.ready_seen, &g_web_fill_walk.mounts_seen, g_sd_ready, "
        "storage.target.mounted, storage.target.mounts) and empty the cache (4g F6)"
    )
    assert re.search(r"\baius_sd_teensy_invalidate_mount\s*\(",
                     defs.get("direct_card_settle", "")), (
        "direct_card_settle must drop the adapter's trusted mount (4g F6)"
    )
    for name in DIRECT_CARD_OPS:
        body = defs.get(name, "")
        clear = re.search(r"\bSD\s*\.\s*clearError\s*\(", body)
        first = re.search(r"\bSD\s*\.\s*(open|exists|remove)\s*\(", body)
        assert clear and first and clear.start() < first.start(), (
            f"{name} must clear the driver code before its first card op (4g F6)"
        )
        assert re.search(r"\bdirect_card_settle\s*\(\s*SD\s*\.\s*sdErrorCode\s*\(\s*\)\s*\)",
                         body[first.start():]), (
            f"{name} must settle its driver code through direct_card_settle (4g F6)"
        )


FILL_RESUME_START = re.compile(
    r"\brequest\s*\.\s*initial_offset\s*=\s*aius_web_cache_fill_resume_offset\s*\(\s*"
    r"&\s*g_web_cache\s*,\s*rel\s*,\s*strlen\s*\(\s*rel\s*\)\s*\)\s*;"
)
FILL_RESUME_SINK = re.compile(
    r"\boffset\s*==\s*0u?\s*\?\s*aius_web_cache_fill_begin\s*\([^;]*:\s*"
    r"aius_web_cache_fill_resume\s*\(\s*&\s*g_web_cache\s*,\s*rel\s*,"
)
FILL_RESUMED_FLAG = re.compile(
    r"\bconst\s+bool\s+resumed\s*=\s*storage\s*\.\s*job\s*\.\s*initial_offset\s*!=\s*0u?\s*;"
)
FILL_CANCEL_KEEPS = re.compile(
    r"\}\s*else\s+if\s*\(\s*result\s*==\s*nullptr\s*\|\|\s*result\s*->\s*fault\s*!=\s*"
    r"AIUS_SD_FAULT_CANCELLED\s*\)\s*\{\s*aius_web_cache_fill_abort\s*\(\s*&\s*g_web_cache\s*\)"
    r"\s*;\s*if\s*\(\s*!\s*resumed\s*\)\s*\{\s*g_web_fill_walk\s*\.\s*entry_index\s*\+\+\s*;"
)
FILL_CANCEL_COMPLETE = re.compile(
    r"\}\s*else\s+if\s*\(\s*g_web_fill_walk\s*\.\s*begun\s*&&\s*rel\s*!=\s*nullptr\s*&&\s*"
    r"aius_web_cache_fill_resume_offset\s*\(\s*&\s*g_web_cache\s*,\s*rel\s*,\s*strlen\s*\(\s*rel\s*\)"
    r"\s*\)\s*==\s*storage\s*\.\s*job\s*\.\s*end_offset\s*&&\s*aius_web_cache_publish\s*\("
)
FILL_PAUSE = re.compile(
    r"\baius_optical_web_fill_paused\s*\(\s*&\s*g_web_fill\s*,\s*now_ms\s*\)"
)
FILL_PRESETS_UNREAD = re.compile(
    r"\bbatch\s*\.\s*count\s*=\s*0u?\s*;\s*g_web_fill_walk\s*\.\s*dir_clean\s*=\s*false\s*;"
)


def assert_fill_resume_pause_and_presets(sources):
    """Phase 4g clean-room round. Finding 1 (owner): a cancelled fill file
    resumes at its next chunk: the start asks the cache for the resume
    offset, a job's first chunk continues the partial (dropped on a size
    change), a cancel keeps the partial and the cursor, and a failed resume
    restarts the file from 0 without skipping it. The hold pauses the fill
    start (nit A's replay pause retired with replay, Phase 4i). Nit: a
    failed subdirectory read leaves the directory unclean, so presets/ is
    never published complete from an unread listing."""
    defs = _clean_definitions(sources[OPTICAL_MAIN_SOURCE])
    nxt = defs.get("web_cache_fill_next", "")
    start = nxt.find("aius_runtime_teensy_start_storage_job(")
    offset = FILL_RESUME_START.search(nxt)
    assert offset and -1 < offset.start() < start, (
        "web_cache_fill_next must set request.initial_offset = "
        "aius_web_cache_fill_resume_offset(&g_web_cache, rel, strlen(rel)) before "
        "the start (finding 1: resume)"
    )
    pause = FILL_PAUSE.search(nxt)
    assert pause and -1 < pause.start() < start and "aius_optical_web_fill_held(" not in nxt, (
        "web_cache_fill_next must gate on aius_optical_web_fill_paused(&g_web_fill, "
        "now_ms) before the start"
    )
    assert FILL_PRESETS_UNREAD.search(nxt), (
        "web_cache_fill_next must mark an unread subdirectory unclean: "
        "`batch.count = 0u; g_web_fill_walk.dir_clean = false;` (presets nit)"
    )
    assert FILL_RESUME_SINK.search(defs.get("web_cache_fill_sink", "")), (
        "web_cache_fill_sink must open chunk 0 with aius_web_cache_fill_begin and a "
        "resumed job's first chunk with aius_web_cache_fill_resume (finding 1: resume)"
    )
    settle = defs.get("web_cache_fill_settle", "")
    assert FILL_RESUMED_FLAG.search(settle) and FILL_CANCEL_KEEPS.search(settle), (
        "web_cache_fill_settle must drop the partial only on a non-cancel failure "
        "and keep a failed resume's cursor (finding 1: a cancel keeps the partial)"
    )
    assert settle.count("aius_web_cache_fill_abort(") == 1, (
        "web_cache_fill_settle must drop the partial only on a non-cancel failure "
        "(finding 1: a cancel keeps the partial)"
    )
    assert FILL_CANCEL_COMPLETE.search(settle), (
        "web_cache_fill_settle must publish a file cancelled after its last chunk "
        "(resume offset == storage.job.end_offset): a tail's end is no chunk "
        "boundary to resume at (finding 1)"
    )


def _strong_definition(clean, name):
    return re.search(rf"\b{name}\s*\([^;{{}}]*\)\s*\{{", clean) is not None


# Phase 4h: link-time hooks whose weak defaults (aius_runtime_storage.cpp)
# would silently hide a missing definition in optical_main.cpp.
SD_OVERLAY_STRONG_HOOKS = (
    "aius_runtime_storage_settings_restores",
    "aius_runtime_storage_root_input",
    "aius_runtime_storage_store_open",
    "aius_runtime_storage_store_chunk",
    "aius_runtime_storage_store_commit",
    "aius_runtime_storage_store_abort",
)


def _between_last(body, earlier, later):
    """The text from the last `earlier` before the first `later` up to it."""
    end = body.find(later)
    assert end != -1, f"no {later}"
    start = body.rfind(earlier, 0, end)
    assert start != -1, f"no {earlier} before {later}"
    return body[start:end]


def assert_sd_overlay_wiring(sources):
    """Phase 4h: CR4-B2 (no cacheClear here, no new W2 row), the strong
    hooks, B6 (publish_config consults the overlay before writing g_config or
    g_pending_dsp_model), R5-S1 / G8 (the flush start consults sd_fifo_defer()
    and cancels a fill through web_cache_fill_cancel()), the job-end order
    (pass and read-back notes before _job_end) and R4-B1 (in both OTA
    services the drain gate precedes the park and a second drain query is
    the statement right before aius_recorder_bank_park; in the reboot service
    it immediately precedes tx_gate_actuate and reset_application)."""
    raw = sources[OPTICAL_MAIN_SOURCE]
    clean = without_comments_and_literals(raw)
    assert "cacheClear" not in clean, (
        "optical_main.cpp must not call cacheClear: the cache drop is an "
        "adapter primitive (CR4-B2)"
    )
    # The put's per-chunk yield (lib/aius_web_cache) is a silent no-op until
    # bound: the composition must bind it to the producer yield.
    assert re.search(r"\baius_web_cache_set_put_yield\s*\(\s*web_cache_verify_yield\s*,",
                     clean), (
        "optical_main.cpp must call aius_web_cache_set_put_yield("
        "web_cache_verify_yield, ...): an unbound put yield is a no-op"
    )
    missing = [h for h in SD_OVERLAY_STRONG_HOOKS if not _strong_definition(clean, h)]
    assert not missing, f"optical_main.cpp must define the 4h hooks: {missing}"
    defs = _clean_definitions(raw)
    # B6, H10: every settings load and restore goes through the portable
    # policy (aius_optical_settings_*, aius_optical_app.cpp, proved in
    # optical-app), whose publication skips a shadowed path before the
    # platform's settings_load() writes g_config or g_pending_dsp_model.
    publish = defs["platform_publish_config"]
    assert "aius_optical_settings_publish(" in publish, (
        "platform_publish_config must route the settings loads through "
        "aius_optical_settings_publish (B6, H10)"
    )
    for write in ("g_config = parsed;", "g_pending_dsp_model = parsed;"):
        assert write not in publish, (
            f"platform_publish_config writes {write} itself, around the "
            "overlay's shadow check (B6)"
        )
    # Phase 4l: a first preset save on a card with no presets/ directory, or
    # one holding a non-servable file, is accepted once the whole walk is done:
    # the overlay's count is "known" at walk_complete, never presets_complete.
    put_begin = defs["sd_flush_put_begin"]
    assert "g_web_fill_walk.walk_complete" in put_begin, (
        "sd_flush_put_begin must pass g_web_fill_walk.walk_complete to "
        "aius_sd_overlay_put_begin (4l)"
    )
    assert "presets_complete" not in put_begin, (
        "sd_flush_put_begin must not gate the preset cap on presets_complete: "
        "a card with no presets/ directory would answer sd_busy forever (4l)"
    )
    complete = defs["sd_step_complete"]
    assert complete.count("aius_optical_settings_load_end(") == 3, (
        "sd_step_complete must end LOAD_CONFIG, LOAD_DSP and the restore "
        "steps through aius_optical_settings_load_end (H10)"
    )
    assert "aius_optical_settings_load_begin(" in defs["sd_start_step"], (
        "sd_start_step must reset the load verdict (H10)"
    )
    app_publish = _clean_definitions(sources[APP_SOURCE]).get(
        "aius_optical_settings_publish", "")
    assert -1 < app_publish.find("shadowed(") < app_publish.find("load("), (
        "aius_optical_settings_publish must consult the overlay before the "
        "platform's load writes the running state (B6)"
    )
    # The flush, remount, freeze and put policy is aius_optical_sd_flush_* /
    # aius_optical_sd_idle_step / aius_optical_sd_write_gate (aius_optical_app.cpp,
    # proved in optical-app); optical_main.cpp binds g_sd_flush_ops and routes
    # its call sites through it. R5-S1 / G8: the start consults storage_ready
    # (sd_fifo_defer()) before pinning a job and cancels a fill through
    # fill_cancel (web_cache_fill_cancel()); the job end orders the pass and
    # read-back notes before job_end.
    app_defs = _clean_definitions(sources[APP_SOURCE])
    app_start = app_defs.get("aius_optical_sd_flush_start", "")
    for needle in ("fill_cancel(", "storage_ready(", "start(context"):
        assert needle in app_start, f"aius_optical_sd_flush_start must call {needle}"
    assert app_start.find("storage_ready(") < app_start.find("start(context"), (
        "aius_optical_sd_flush_start must consult storage_ready before pinning a job"
    )
    app_settle = app_defs.get("aius_optical_sd_flush_settle", "")
    app_end = app_settle.find("job_end(")
    for note in ("note_pass(", "note_readback("):
        assert -1 < app_settle.find(note) < app_end, f"{note} must precede job_end"
    for callback, needle in (
        ("sd_flush_storage_ready", "sd_fifo_defer()"),
        ("sd_flush_fill_cancel", "web_cache_fill_cancel()"),
        ("sd_flush_start_job", "aius_sd_overlay_next_job("),
        ("sd_flush_note_pass", "aius_sd_overlay_note_pass_closed("),
        ("sd_flush_note_readback", "aius_sd_overlay_note_readback("),
        ("sd_flush_job_end", "aius_sd_overlay_job_end("),
        ("sd_flush_card_detected", "aius_sd_teensy_card_detected("),
        ("sd_flush_playback", "pllLocked()"),
    ):
        assert needle in defs.get(callback, ""), f"{callback} must call {needle}"
    ops = raw[raw.find("g_sd_flush_ops PROGMEM = {"):]
    ops = ops[:ops.find("};")]
    for callback in ("sd_flush_storage_ready", "sd_flush_fill_cancel",
                     "sd_flush_start_job", "sd_flush_note_pass",
                     "sd_flush_note_readback", "sd_flush_job_end",
                     "sd_flush_card_detected", "sd_flush_playback"):
        assert callback in ops, f"g_sd_flush_ops must bind {callback}"
    routes = (
        ("sd_lifecycle_service", "aius_optical_sd_idle_step("),
        ("sd_flush_settle", "aius_optical_sd_flush_settle("),
        ("service_reboot_request", "aius_optical_sd_service_gate("),
        ("service_ota_activation", "aius_optical_sd_service_gate("),
        ("service_ota_rehearsal", "aius_optical_sd_service_gate("),
        ("sd_overlay_reput", "aius_optical_settings_reput("),
        ("app_sd_ready", "aius_optical_app_card_ops_ready("),
        ("dsp_config_json", "aius_optical_dsp_config_write("),
        # Delivery audit: D-S2 the due predicate is the overlay's own pick;
        # D-S1 card-gone needs DAT3 absent (flush end and the fill's hold);
        # D-N3 REPLACE scratch names are boot-independent.
        ("sd_flush_due", "aius_sd_overlay_job_ready("),
        # D-S5: every card-loss decision (the flush end, the fill's hold, the
        # lifecycle's per-pass observation) goes through the one latched
        # classification, which re-muxes DAT3 only for a new driver fault;
        # direct card ops count theirs; the presence read is the ops callback.
        ("sd_flush_settle", "aius_optical_sd_card_loss("),
        ("web_cache_fill_settle", "aius_optical_sd_card_loss("),
        ("sd_lifecycle_service", "aius_optical_sd_card_loss("),
        # D-S7: the two settle sites hand over the adapter's open file, so
        # a fault is never classified (DAT3 never read) with a file open.
        ("sd_flush_settle", "storage.target.file_open"),
        ("sd_flush_settle", "end.loss_pending"),
        ("web_cache_fill_settle", "storage.target.file_open"),
        ("direct_card_settle", "aius_optical_sd_note_direct_fault("),
        ("sd_flush_card_present_after_fault", "aius_sd_teensy_card_detected("),
        ("sd_flush_card_present_after_fault", "aius_sd_teensy_invalidate_mount("),
        ("sd_flush_start_job", "aius_optical_sd_flush_scratch_id("),
        ("http_dsp_config_json", "dsp_config_json(body, capacity, false)"),
        # NS6: the Q2 re-put writes the file form (base_generation present).
        ("write_dsp_config_file", "dsp_config_json(body, capacity, true)"),
        ("sd_overlay_reput", "write_dsp_config_file"),
        ("sd_drop_orphan_put", "aius_optical_sd_flush_sweep_orphan("),
        ("app_overlay_put_begin", "aius_optical_sd_flush_put_begin("),
        ("sd_flush_note_probe", "aius_optical_sd_flush_note_probe("),
    )
    for caller, needle in routes:
        assert needle in defs.get(caller, ""), f"{caller} must route through {needle}"
    # D-S5: the DAT3 re-mux after a lost mount happens only inside the bound
    # callback (and the adapter's own card-gone paths), never from the
    # card-follow or settle call sites directly; E3's mode change never
    # reaches it (app_sd_set_mode is checked above).
    for caller in ("sd_flush_settle", "web_cache_fill_settle",
                   "sd_lifecycle_service", "app_sd_set_mode"):
        assert "aius_sd_teensy_invalidate_mount(" not in defs.get(caller, ""), (
            f"{caller} must not re-mux DAT3 itself (D-S5, E3)"
        )
    assert "sd_flush_card_present_after_fault" in ops, (
        "g_sd_flush_ops must bind sd_flush_card_present_after_fault"
    )
    # D-S9: one BEGIN gate (aius_optical_sd_begin_blocked), consulted by every
    # path that can start a storage BEGIN. The storage-job start sites are a
    # closed set (a new one fails here until it is gated), there is no direct
    # executor since the replay job retired (Phase 4i), and each start is
    # reached through the gate.
    assert "sd_flush_mounted" in ops, "g_sd_flush_ops must bind sd_flush_mounted"
    assert "aius_optical_sd_begin_blocked(" in defs.get("sd_begin_blocked", "")
    starters = sorted(name for name, body in defs.items()
                      if "aius_runtime_teensy_start_storage_job(" in body)
    assert starters == ["sd_flush_start_job", "sd_start_step",
                        "web_cache_fill_next"], (
        f"storage job start sites changed ({starters}): gate the new one "
        "through sd_begin_blocked (D-S9)"
    )
    executors = sorted(name for name, body in defs.items()
                       if "aius_sd_teensy_execute(" in body)
    assert executors == [], (
        f"direct adapter executors changed ({executors}): gate BEGIN (D-S9)"
    )
    for site, gate in (("sd_lifecycle_service", "sd_step_begin_blocked("),
                       ("aius_runtime_http_admit", "sd_begin_blocked(")):
        assert gate in defs.get(site, ""), (
            f"{site} must consult the BEGIN gate {gate} (D-S9, D-S10, D-N9)"
        )
    # Phase 4k (N1, N2): the freeze answers before the gate; the gate maps
    # NO_CARD to UNAVAILABLE and WAIT to BUSY and disarms the staging.
    admit = defs.get("aius_runtime_http_admit", "")
    assert 0 <= admit.find("sd_freeze_active()") < admit.find("sd_begin_blocked("), (
        "the SD-writes freeze must be tested before the BEGIN gate (N1)"
    )
    gate_tail = admit[admit.find("sd_begin_blocked("):]
    gate_end = "AIUS_HTTP_VERDICT_BUSY;"
    gate_branch = gate_tail[:gate_tail.find(gate_end) + len(gate_end)]
    assert ("sd_disarm_shed_staging(" in gate_branch
            and "AIUS_OPTICAL_SD_BEGIN_NO_CARD" in gate_branch
            and "AIUS_HTTP_VERDICT_UNAVAILABLE" in gate_branch), (
        "the BEGIN gate must map NO_CARD to UNAVAILABLE, WAIT to BUSY, and "
        "disarm the staging on both (N2)"
    )
    assert re.search(
        r"gate\s*==\s*AIUS_OPTICAL_SD_BEGIN_NO_CARD\s*\?\s*"
        r"AIUS_HTTP_VERDICT_UNAVAILABLE\s*:\s*AIUS_HTTP_VERDICT_BUSY\s*;",
        gate_branch), (
        "the BEGIN gate ternary must pair NO_CARD with UNAVAILABLE and WAIT "
        "with BUSY (N2)"
    )
    assert "aius_optical_sd_step_begin_blocked(" in defs.get("sd_step_begin_blocked", "")
    for site in ("aius_optical_sd_flush_start", "aius_optical_sd_idle_step"):
        assert "aius_optical_sd_begin_blocked(" in app_defs.get(site, ""), (
            f"{site} must consult aius_optical_sd_begin_blocked (D-S9)"
        )
    assert "flush->fault_remount" in app_defs.get("aius_optical_sd_flush_start", ""), (
        "aius_optical_sd_flush_start must wait for the fault remount (D-S9 a)"
    )
    # CR-B1: the remount schedule is read only through sd_remount_waiting
    # (an armed flag, lapsed once due) and written only through
    # sd_remount_schedule; the lifecycle lapses the windows every pass and
    # keeps a skipped due probe's stamp fresh.
    for name, body in app_defs.items():
        if name in ("sd_remount_waiting", "sd_remount_schedule"):
            continue
        assert "remount_at_ms" not in body, (
            f"{name} touches remount_at_ms outside the armed schedule (CR-B1)"
        )
    assert "aius_optical_sd_flush_lapse(" in app_defs.get("aius_optical_sd_card_loss", "")
    assert "g_sd_probe_at_ms = now_ms;" in defs.get("sd_lifecycle_service", ""), (
        "sd_lifecycle_service must refresh a skipped due probe's stamp (CR-B1)"
    )
    # R5-S2: both rehearsal abandons (the gate's and the second drain
    # query's) record OTA_REHEARSAL_ABANDONED before the park.
    rehearsal = defs["service_ota_rehearsal"]
    rehearsal_gate = rehearsal[rehearsal.find("aius_optical_sd_service_gate("):
                               rehearsal.find("aius_recorder_bank_park(")]
    assert rehearsal_gate.count("ota_rehearsal_finish(OTA_REHEARSAL_ABANDONED)") == 2, (
        "service_ota_rehearsal must finish OTA_REHEARSAL_ABANDONED on the gate's "
        "abandon and on the final drain query (R5-S2)"
    )
    # GET /api/dsp/config serves the staged model while an Apply is pending,
    # else the running one, under the generation the Apply advances.
    dsp = defs.get("dsp_config_json", "")
    for needle in ("apply_pending ? g_pending_dsp_model : g_active_model",
                   "header.config_pending = apply_pending;",
                   "header.config_generation = g_config_generation;"):
        assert needle in dsp, f"dsp_config_json must carry `{needle}`"
    # E3 mode-only half: POST /api/sd/mode drops the mount through the
    # adapter and never touches DAT3 (the adapter's side, set_mode leaving
    # dat3_remuxed clear, is sd-target's test_card_detected_and_dat3).
    mode = defs.get("app_sd_set_mode", "")
    assert "aius_sd_teensy_set_mode(" in mode and "dat3_remuxed" not in mode, (
        "app_sd_set_mode must switch the mode through the adapter and leave "
        "dat3_remuxed to it (E3)"
    )
    for service in ("service_ota_activation", "service_ota_rehearsal"):
        body = defs[service]
        park = body.find("aius_recorder_bank_park(")
        gate = body.find("aius_optical_sd_service_gate(")
        assert -1 < gate < park, f"{service}: the drain gate must precede the park"
        tail = _between_last(body, "sd_flush_drained()", "aius_recorder_bank_park(")
        assert tail.count("return") == 1 and "aius_optical_sd_service_gate(" not in tail, (
            f"{service}: the second drain query must be the statement right "
            "before aius_recorder_bank_park (R4-B1)"
        )
    reboot = defs["service_reboot_request"]
    tail = _between_last(reboot, "aius_optical_sd_service_final(", "tx_gate_actuate(")
    assert tail.count("return") == 1, (
        "service_reboot_request: the second drain query must immediately "
        "precede tx_gate_actuate and reset_application (R4-B1)"
    )
    assert reboot.find("tx_gate_actuate(") < reboot.find("reset_application("), (
        "service_reboot_request must actuate the gate before the reset"
    )


SD_OVERLAY_CHECKS = (
    ("sd-overlay-wiring", assert_sd_overlay_wiring),
)


def run_sd_overlay_self_test(sources):
    """The 4h check discriminates: each mutation of the real sources fails."""
    mutations = (
        ("extern \"C\" FLASHMEM void aius_runtime_storage_settings_restores(",
         "extern \"C\" FLASHMEM void unrelated_settings_restores("),
        ("aius_sd_overlay_note_readback(&g_sd_overlay,",
         "(void)(&g_sd_overlay,"),
        ("\treturn sd_storage_idle() && !sd_fifo_defer();",
         "\treturn sd_storage_idle();"),
        ("aius_optical_sd_flush_sweep_orphan(\n", "(void)(\n"),
        ("\taius_web_cache_set_put_yield(web_cache_verify_yield, nullptr);",
         "\t(void)0;"),
        ("\n\t\t\tota_rehearsal_finish(OTA_REHEARSAL_ABANDONED);",
         "\n\t\t\tota_rehearsal_finish(OTA_REHEARSAL_FAIL);"),
        ("apply_pending ? g_pending_dsp_model : g_active_model",
         "g_active_model"),
        ("\treturn aius_sd_overlay_job_ready(&g_sd_overlay, now_ms);",
         "\treturn true;"),
        ("\taius_optical_sd_note_direct_fault(&g_sd_flush);\n", "\n"),
        ("\t\tconst AiusOpticalSdBeginGate gate = sd_begin_blocked(sample.now_ms);",
         "\t\tconst AiusOpticalSdBeginGate gate = AIUS_OPTICAL_SD_BEGIN_NONE;"),
        ("? AIUS_HTTP_VERDICT_UNAVAILABLE\n\t\t\t               : AIUS_HTTP_VERDICT_BUSY;",
         "? AIUS_HTTP_VERDICT_BUSY\n\t\t\t               : AIUS_HTTP_VERDICT_UNAVAILABLE;"),
        ("\t\tend.loss_pending = loss == AIUS_OPTICAL_SD_CARD_PENDING;\n",
         "\t\tend.card_detected = true;\n"),
        ("\taius_sd_teensy_set_mode(target, dma);\n",
         "\taius_sd_teensy_set_mode(target, dma);\n\ttarget->dat3_remuxed = true;\n"),
        ("if (!sd_flush_drained()) {\n\t\tg_ota_activation_fault = "
         "OTA_ACTIVATION_FAULT_FLUSH_PENDING;\n\t\tota_activation_recover();",
         "if (false) {\n\t\tg_ota_activation_fault = "
         "OTA_ACTIVATION_FAULT_FLUSH_PENDING;\n\t\tota_activation_recover();"),
    )
    for old, new in mutations:
        mutated = _mutated(sources, OPTICAL_MAIN_SOURCE, old, new)
        try:
            assert_sd_overlay_wiring(mutated)
        except AssertionError:
            continue
        raise AssertionError(f"sd-overlay-wiring passed a source with {old!r} removed")


ADMISSION_CHECKS = (
    ("fill-resume-pause-and-presets", assert_fill_resume_pause_and_presets),
    ("fill-route-and-card-wiring", assert_fill_route_and_card_wiring),
    ("root-sd-starts-consult-fifo-defer", assert_root_sd_starts_consult_fifo_defer),
    ("ota-staging-exempt-before-margin", assert_ota_staging_exempt_before_margin),
    ("single-ownership-predicate", assert_single_ownership_predicate),
    ("status-render-appends-yield", assert_status_render_appends_yield),
    ("round3-admission-wiring", assert_round3_admission_wiring),
    ("bringup-latch-at-load-dsp-end", assert_bringup_latch_at_load_dsp_end),
    ("orphan-staging-dropped", assert_orphan_staging_dropped),
)


def _admission_fixture():
    main = (
        "static FLASHMEM bool live_owns_ring(void)\n{\n"
        "\treturn g_aius_spdif_tx24.gate_open != 0u && g_tx_gate_carrier;\n}\n"
        "static FLASHMEM AiusOpticalAdmitSample admit_sample(void)\n{\n"
        "\tsample.live_owns_ring = live_owns_ring();\n\treturn sample;\n}\n"
        "extern \"C\" FLASHMEM AiusHttpAdmitVerdict aius_runtime_http_admit(\n"
        "\tconst AiusHttpAdmitQuery *query)\n{\n"
        "\tconst AiusOpticalAdmitSample sample = admit_sample();\n"
        "\tv = decide(&sample, g_runtime.network_http.deferred_start_present);\n"
        "\tif (!sd_start && aius_optical_app_route_reads_card(query->request) &&\n"
        "\t    aius_optical_web_fill_route(&g_web_fill, web_cache_fill_in_flight(),\n"
        "\t                                sample.now_ms, &fill_cancel)) {\n"
        "\t\tif (fill_cancel)\n\t\t\tweb_cache_fill_cancel();\n"
        "\t\treturn AIUS_HTTP_VERDICT_DEFER;\n\t}\n"
        "\tv = aius_optical_admission_route(&g_http_admission, query->route);\n"
        "\tif (v == SHED)\n\t\tsd_disarm_shed_staging(query->sd_job);\n"
        "\treturn v;\n}\n"
        "static FLASHMEM void app_dispatch_end(void *, AiusHttpRoute r, AiusHttpMethod m)\n{\n"
        "\taius_optical_admission_note_dispatch(&g_http_admission, r, m, 0u, 0u);\n"
        "\taius_optical_web_fill_route_end(&g_web_fill, millis());\n}\n"
        "static FLASHMEM void web_cache_fill_settle(uint32_t now_ms)\n{\n"
        "\tif (g_web_fill.http_canceller &&\n"
        "\t    !g_runtime.network_http.deferred_start_present &&\n"
        "\t    !g_runtime.network_http.deferred_dispatch_present &&\n"
        "\t    !g_sd_http_job_seen)\n"
        "\t\taius_optical_web_fill_drop_http_canceller(&g_web_fill, now_ms);\n"
        "\tif (g_web_fill_walk.running && storage.root_state != RUNNING) {\n"
        "\t\tconst bool resumed = storage.job.initial_offset != 0u;\n"
        "\t\tg_web_fill_walk.running = false;\n"
        "\t\tif (ok) {\n"
        "\t\t\tconst bool begun = g_web_fill_walk.begun ||\n"
        "\t\t\t\t(resumed ? aius_web_cache_fill_resume(&g_web_cache, rel, n, o, l)\n"
        "\t\t\t\t         : aius_web_cache_fill_begin(&g_web_cache, rel, n, 0u));\n"
        "\t\t\tif (begun && aius_web_cache_publish(&g_web_cache))\n"
        "\t\t\t\tg_web_fill_walk.entry_index++;\n"
        "\t\t} else if (result == nullptr ||\n"
        "\t\t           result->fault != AIUS_SD_FAULT_CANCELLED) {\n"
        "\t\t\taius_web_cache_fill_abort(&g_web_cache);\n"
        "\t\t\tif (!resumed) {\n"
        "\t\t\t\tg_web_fill_walk.entry_index++;\n"
        "\t\t\t\tg_web_fill_walk.dir_clean = false;\n\t\t\t}\n"
        "\t\t} else if (g_web_fill_walk.begun && rel != nullptr &&\n"
        "\t\t           aius_web_cache_fill_resume_offset(&g_web_cache, rel,\n"
        "\t\t                                             strlen(rel)) ==\n"
        "\t\t                   storage.job.end_offset &&\n"
        "\t\t           aius_web_cache_publish(&g_web_cache)) {\n"
        "\t\t\tg_web_fill_walk.entry_index++;\n\t\t}\n"
        "\t\tg_web_fill_walk.begun = false;\n\t}\n"
        "\tif (aius_optical_web_fill_follow_card(&g_web_fill_walk.ready_seen,\n"
        "\t                                      &g_web_fill_walk.mounts_seen,\n"
        "\t                                      g_sd_ready, storage.target.mounted,\n"
        "\t                                      storage.target.mounts))\n"
        "\t\tweb_cache_invalidate_all();\n}\n"
        "static FLASHMEM void direct_card_settle(uint32_t code)\n{\n"
        "\tif (code != 0u && !g_runtime.storage.target.file_open)\n"
        "\t\taius_sd_teensy_invalidate_mount(&g_runtime.storage.target);\n}\n"
        "static FLASHMEM int app_read_directory(void *, const char *p)\n{\n"
        "\tconst ProducerSdWindow window;\n\tSD.clearError();\n"
        "\tFile directory = SD.open(p);\n"
        "\tif (!directory) {\n\t\tdirect_card_settle(SD.sdErrorCode());\n\t\treturn 1;\n\t}\n"
        "\treturn 0;\n}\n"
        "static FLASHMEM bool sd_fifo_defer(void)\n{\n"
        "\tconst AiusOpticalAdmitSample sample = admit_sample();\n"
        "\treturn aius_optical_admission_sd_fifo_defer(&g_http_admission, &sample);\n}\n"
        "static FLASHMEM void sd_step_complete(uint32_t now_ms)\n{\n"
        "\tswitch (step) {\n"
        "\tcase SD_STEP_PROBE: {\n"
        "\t\tif (present && !g_sd_ready) {\n"
        "\t\t\tg_sd_bringup_done = true;\n"
        "\t\t\tg_sd_step = SD_STEP_LOAD_CONFIG;\n\t\t}\n\t\treturn;\n\t}\n"
        "\tcase SD_STEP_LOAD_CONFIG:\n"
        "\t\tif (!ok)\n\t\t\tg_config_error_count++;\n"
        "\t\tg_sd_step = SD_STEP_LOAD_DSP;\n\t\treturn;\n"
        "\tcase SD_STEP_LOAD_DSP:\n"
        "\t\taius_optical_admission_note_bringup_loaded(&g_http_admission);\n"
        "\t\tg_sd_step = SD_STEP_USAGE;\n\t\treturn;\n"
        "\tdefault:\n\t\treturn;\n\t}\n}\n"
        "static FLASHMEM void sd_disarm_shed_staging(const AiusSdJobRequest *job)\n{\n"
        "\tg_dsp_upload_pending = false;\n\tg_config_upload_pending = false;\n"
        "\tg_sd_commit_target = SD_COMMIT_NONE;\n}\n"
        "static FLASHMEM bool app_sd_set_mode(void *, bool dma)\n{\n"
        "\tadmission_sd_window_sample();\n"
        "\taius_sd_teensy_set_mode(target, dma);\n\treturn true;\n}\n"
        "static FLASHMEM __attribute__((noinline)) bool status_append(\n"
        "\tchar *b, size_t c, size_t *l, const char *f, ...)\n{\n"
        "\tint written = vsnprintf(b + *l, c - *l, f, arguments);\n"
        "\t(void)aius_producer_yield_point();\n\treturn true;\n}\n"
        "static FLASHMEM bool http_clock_json(char *o, size_t c, size_t *l)\n{\n"
        "\treturn status_append(o, c, l, \"x\");\n}\n"
        "static FLASHMEM size_t http_status_json(uint8_t *body, size_t capacity)\n{\n"
        "\tif (!status_append(b, c, &l, \"{\"))\n\t\treturn 0;\n"
        "\tif (!http_clock_json(b, c, &l))\n\t\treturn 0;\n"
        "\treturn l;\n}\n"
        "static FLASHMEM bool sd_start_step(SdStep step)\n{\n"
        "\treturn aius_runtime_teensy_start_storage_job(&g_runtime, &request);\n}\n"
        "static FLASHMEM void sd_drop_orphan_staging(void)\n{\n"
        "\tif (g_sd_commit_target == SD_COMMIT_NONE || g_sd_commit_writing ||\n"
        "\t    g_sd_http_job_seen ||\n"
        "\t    !aius_optical_app_sd_staging_orphaned(\n"
        "\t\t    &g_runtime.network_http.http,\n"
        "\t\t    g_runtime.network_http.deferred_start_present,\n"
        "\t\t    g_runtime.storage_action_present))\n\t\treturn;\n"
        "\tg_dsp_upload_pending = false; g_config_upload_pending = false;\n"
        "\tg_sd_commit_target = SD_COMMIT_NONE;\n}\n"
        "static FLASHMEM void sd_lifecycle_service(void)\n{\n"
        "\tsd_settle_http_write();\n\tsd_drop_orphan_staging();\n"
        "\tweb_cache_fill_next(now_ms);\n"
        "\tif (sd_fifo_defer())\n\t\treturn;\n"
        "\tif (sd_start_step(g_sd_step))\n\t\tg_sd_step_running = true;\n}\n"
        "static FLASHMEM bool web_cache_fill_defer(void)\n{\n"
        "\treturn sd_fifo_defer();\n}\n"
        "extern \"C\" FLASHMEM bool aius_runtime_storage_fill_step_defer(const AiusSdJob *)\n{\n"
        "\treturn web_cache_fill_defer();\n}\n"
        "static FLASHMEM bool web_cache_fill_sink(const AiusSdJob *job, uint64_t offset)\n{\n"
        "\tif (!g_web_fill_walk.begun) {\n"
        "\t\tconst bool open = offset == 0u\n"
        "\t\t\t? aius_web_cache_fill_begin(&g_web_cache, rel, n, s)\n"
        "\t\t\t: aius_web_cache_fill_resume(&g_web_cache, rel, n, o, s);\n"
        "\t\tif (!open)\n\t\t\treturn false;\n\t\tg_web_fill_walk.begun = true;\n\t}\n"
        "\treturn aius_web_cache_fill_chunk(&g_web_cache, data, l);\n}\n"
        "static FLASHMEM void web_cache_fill_next(uint32_t now_ms)\n{\n"
        "\tif (g_web_fill_walk.running ||\n"
        "\t    aius_optical_web_fill_paused(&g_web_fill, now_ms) ||\n"
        "\t    !sd_storage_idle())\n\t\treturn;\n"
        "\tif (web_cache_fill_defer())\n\t\treturn;\n"
        "\tif (app_read_directory(nullptr, path, &batch) != DIR_OK) {\n"
        "\t\tif (g_web_fill_walk.dir_ordinal == 0u) {\n"
        "\t\t\tg_web_fill_walk.walk_complete = true;\n\t\t} else {\n"
        "\t\t\tbatch.count = 0u;\n\t\t\tg_web_fill_walk.dir_clean = false;\n\t\t}\n\t}\n"
        "\trequest.initial_offset =\n"
        "\t\taius_web_cache_fill_resume_offset(&g_web_cache, rel, strlen(rel));\n"
        "\tif (aius_runtime_teensy_start_storage_job(&g_runtime, &request))\n"
        "\t\tg_web_fill_walk.running = true;\n}\n"
        "static FLASHMEM void tx_gate_service(void)\n{\n"
        "\tsample.gate_open = g_aius_spdif_tx24.gate_open != 0u ? 1u : 0u;\n"
        "\tsample.carrier = g_tx_gate_carrier ? 1u : 0u;\n}\n"
    )
    storage = (
        # Phase 4h: the ownerless branch as pw-storage hoisted it.
        "FLASHMEM void advance_ownerless(AiusRuntimeStorage *service, uint32_t now_ms)\n{\n"
        "\tif ((!service->draining &&\n"
        "\t     service->root_state != AIUS_RUNTIME_STORAGE_ROOT_RUNNING) ||\n"
        "\t    service->terminal_seen)\n"
        "\t\treturn;\n"
        "\t\tconst bool fill_paused =\n"
        "\t\t\t!service->draining &&\n"
        "\t\t\tservice->root_state == AIUS_RUNTIME_STORAGE_ROOT_RUNNING &&\n"
        "\t\t\tservice->job.kind == AIUS_SD_JOB_CACHE_FILL &&\n"
        "\t\t\t!service->root_cancelling &&\n"
        "\t\t\taius_runtime_storage_fill_step_defer(&service->job);\n"
        "\t\tif (!fill_paused)\n"
        "\t\t\t(void)advance(service, now_ms, false);\n}\n"
        "FLASHMEM bool aius_runtime_storage_service(AiusRuntimeStorage *service)\n{\n"
        "\tif (action) {\n\t\troute(service);\n"
        "\t} else {\n\t\tadvance_ownerless(service, now_ms);\n\t}\n\treturn true;\n}\n"
        "FLASHMEM bool aius_runtime_storage_diag_json(const AiusRuntimeStorage *s)\n{\n"
        "\treturn diag_append(o, c, l, \"x\");\n}\n"
    )
    app = (
        "FLASHMEM AiusOpticalAdmitVerdict aius_optical_admission_sd_start(\n"
        "\tAiusOpticalAdmission *a, const AiusSdJobRequest *job,\n"
        "\tconst AiusOpticalAdmitSample *sample)\n{\n"
        "\tif (aius_optical_app_sd_job_is_ota_staging(job))\n\t\treturn ADMIT;\n"
        "\tif (!aius_optical_admission_short(a, klass, sample))\n\t\treturn ADMIT;\n"
        "\treturn SHED;\n}\n"
        "FLASHMEM bool aius_optical_admission_sd_fifo_defer(\n"
        "\tAiusOpticalAdmission *admission, const AiusOpticalAdmitSample *sample)\n{\n"
        "\tif (admission == NULL || sample == NULL || !sample->fifo_mode ||\n"
        "\t    admission->bringup_loaded == 0u)\n\t\treturn false;\n"
        "\tif (!aius_optical_admission_short(admission, klass, sample))\n"
        "\t\treturn false;\n"
        "\tadmission->sd_fifo_defers++;\n\treturn true;\n}\n"
        "FLASHMEM void aius_optical_admission_note_bringup_loaded(\n"
        "\tAiusOpticalAdmission *admission)\n{\n"
        "\tif (admission != NULL)\n\t\tadmission->bringup_loaded = 1u;\n}\n"
        "FLASHMEM bool aius_optical_app_sd_staging_orphaned(\n"
        "\tconst AiusHttpTransaction *http, bool start_held, bool start_queued)\n{\n"
        "\tif (http == NULL || start_held || start_queued)\n\t\treturn false;\n"
        "\treturn http->state != AIUS_HTTP_TX_DISPATCH &&\n"
        "\t       http->state != AIUS_HTTP_TX_SD_START &&\n"
        "\t       http->state != AIUS_HTTP_TX_SD_TRANSFER;\n}\n"
    )
    http = (
        "namespace {\n"
        "FLASHMEM void dispatch_route(AiusRuntimeNetworkHttp *service,\n"
        "                             AiusOpticalAppDispatcher *app,\n"
        "                             const AiusHttpAction *action)\n{\n"
        "\tconst AiusHttpAdmitVerdict verdict =\n"
        "\t\tconsult(service, AIUS_HTTP_ADMIT_ROUTE, action->request, nullptr);\n"
        "\tservice->deferred_dispatch_present = verdict == AIUS_HTTP_VERDICT_DEFER;\n"
        "\tif (verdict == AIUS_HTTP_VERDICT_DEFER)\n\t\treturn;\n"
        "\tresponse = aius_optical_app_http_dispatch(app, action->request);\n}\n"
        "}\n"
        "FLASHMEM bool aius_runtime_http_service(AiusRuntimeNetworkHttp *service)\n{\n"
        "\tif (service->deferred_dispatch_present) {\n"
        "\t\tdispatch_route(service, app, &held);\n\t\treturn true;\n\t}\n"
        "\taius_http_transaction_step(&service->http, now_ms, event, &action);\n"
        "\tswitch (action.kind) {\n"
        "\tcase AIUS_HTTP_ACTION_DISPATCH:\n"
        "\t\tservice->http_requests++;\n\t\tdispatch_route(service, app, &action);\n"
        "\t\tbreak;\n\tdefault:\n\t\tbreak;\n\t}\n\treturn true;\n}\n"
    )
    return {OPTICAL_MAIN_SOURCE: main, STORAGE_SOURCE: storage, APP_SOURCE: app,
            HTTP_SERVICE_SOURCE: http}


def run_admission_self_test():
    good = _admission_fixture()
    for _, check in ADMISSION_CHECKS:
        _expect_pass(check, good, "admission/good")
    starts = assert_root_sd_starts_consult_fifo_defer
    _expect_fail(starts, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\tif (sd_fifo_defer())\n\t\treturn;\n", "",
    ), "starts/lifecycle-unguarded", "sd_lifecycle_service")
    _expect_fail(starts, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tif (sd_fifo_defer())\n\t\treturn;\n\tif (sd_start_step(g_sd_step))",
        "\tif (sd_start_step(g_sd_step))\n\t\treturn;\n\tif (sd_fifo_defer())",
    ), "starts/consulted-after", "sd_lifecycle_service")
    _expect_fail(starts, _mutated(
        good, STORAGE_SOURCE, "\t\tif (!fill_paused)\n",
        "\t\tif (!fill_paused && !service->event_present)\n",
    ), "starts/ownerless-gated", "never held")
    # G8 (owner, 2026-10-10): the fill step gate is fill-only, skips a
    # cancelled fill, and decides through the fill start's own predicate.
    _expect_fail(starts, _mutated(
        good, STORAGE_SOURCE,
        "\t\t\tservice->job.kind == AIUS_SD_JOB_CACHE_FILL &&\n", "",
    ), "g8/every-root-job-gated", "never held")
    _expect_fail(starts, _mutated(
        good, STORAGE_SOURCE, "\t\t\t!service->root_cancelling &&\n", "",
    ), "g8/cancel-gated", "never held")
    _expect_fail(starts, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\treturn web_cache_fill_defer();\n",
        "\treturn false;\n",
    ), "g8/step-not-deferred", "each fill primitive")
    _expect_fail(starts, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "{\n\treturn sd_fifo_defer();\n}\nextern",
        "{\n\treturn live_owns_ring();\n}\nextern",
    ), "g8/predicate-not-fifo-defer", "exactly")
    _expect_fail(starts, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tif (web_cache_fill_defer())\n\t\treturn;\n\tif (app_read_directory",
        "\tif (app_read_directory",
    ), "g8/fill-start-unguarded", "web_cache_fill_next")
    # Phase 4g clean-room round: finding 1 (resume), the hold's pause, the
    # presets nit.
    resume = assert_fill_resume_pause_and_presets
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\trequest.initial_offset =\n"
        "\t\taius_web_cache_fill_resume_offset(&g_web_cache, rel, strlen(rel));\n", "",
    ), "f1/restarts-at-zero", "initial_offset")
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t\t\t: aius_web_cache_fill_resume(&g_web_cache, rel, n, o, s);\n",
        "\t\t\t: false;\n",
    ), "f1/sink-no-resume", "first chunk")
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t\tg_web_fill_walk.running = false;\n\t\tif (ok) {\n",
        "\t\tg_web_fill_walk.running = false;\n"
        "\t\taius_web_cache_fill_abort(&g_web_cache);\n\t\tif (ok) {\n",
    ), "f1/cancel-drops-partial", "keeps the partial")
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t\t\tif (!resumed) {\n\t\t\t\tg_web_fill_walk.entry_index++;\n",
        "\t\t\tif (true) {\n\t\t\t\tg_web_fill_walk.entry_index++;\n",
    ), "f1/failed-resume-skips", "failed resume")
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\t\t                   storage.job.end_offset &&\n",
        "\t\t                   0u &&\n",
    ), "f1/complete-cancel-unpublished", "last chunk")
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t    aius_optical_web_fill_paused(&g_web_fill, now_ms) ||\n", "",
    ), "hold/unpaused", "aius_optical_web_fill_paused")
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t    aius_optical_web_fill_paused(&g_web_fill, now_ms) ||\n",
        "\t    aius_optical_web_fill_paused(&g_web_fill, now_ms, true) ||\n",
    ), "hold/source-argument", "aius_optical_web_fill_paused")
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t    aius_optical_web_fill_paused(&g_web_fill, now_ms) ||\n",
        "\t    aius_optical_web_fill_held(&g_web_fill, now_ms) ||\n",
    ), "hold/held-not-paused", "aius_optical_web_fill_paused")
    _expect_fail(resume, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t\t\tbatch.count = 0u;\n\t\t\tg_web_fill_walk.dir_clean = false;\n",
        "\t\t\tbatch.count = 0u;\n",
    ), "presets/unread-dir-clean", "unclean")
    ota = assert_ota_staging_exempt_before_margin
    _expect_fail(ota, _mutated(
        good, APP_SOURCE,
        "\tif (aius_optical_app_sd_job_is_ota_staging(job))\n\t\treturn ADMIT;\n", "",
    ), "ota/no-exemption", "OTA staging")
    _expect_fail(ota, dict(good, **{
        APP_SOURCE: good[APP_SOURCE] + "static bool chunk(void)\n{\n"
                                       "\treturn sd_fifo_defer();\n}\n",
    }), "ota/app-defers", "must not consult")
    own = assert_single_ownership_predicate
    _expect_fail(own, _mutated(
        good, OPTICAL_MAIN_SOURCE, " && g_tx_gate_carrier;", ";",
    ), "own/flag-only", "gate flag with the latched carrier")
    _expect_fail(own, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tconst AiusOpticalAdmitSample sample = admit_sample();\n"
        "\treturn aius_optical_admission_sd_fifo_defer(",
        "\treturn g_aius_spdif_tx24.gate_open != 0u && g_tx_gate_carrier &&\n"
        "\t\taius_optical_admission_sd_fifo_defer(",
    ), "own/defer-derives", "sd_fifo_defer")
    _expect_fail(own, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tconst AiusOpticalAdmitSample sample = admit_sample();\n\tv = decide(",
        "\tv = live_owns_ring() ? v : a;\n\tv = decide(",
    ), "own/admit-reads-directly", "only admit_sample")
    _expect_fail(own, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tsample.carrier = g_tx_gate_carrier ? 1u : 0u;\n",
        "\tsample.carrier = g_tx_gate_carrier && g_aius_spdif_tx24.gate_open;\n",
    ), "own/second-derivation", "tx_gate_service")
    render = assert_status_render_appends_yield
    _expect_fail(render, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\treturn status_append(o, c, l, \"x\");",
        "\treturn http_json_append(o, c, l, \"x\");",
    ), "render/bare-append", "http_clock_json")
    _expect_fail(render, _mutated(
        good, STORAGE_SOURCE, "\treturn diag_append(o, c, l, \"x\");",
        "\treturn snprintf(o, c, \"x\") > 0;",
    ), "render/storage-bare", "diag_append")
    _expect_fail(starts, _mutated(
        good, STORAGE_SOURCE, "\t\tif (!fill_paused)\n",
        "\t\tif (!fill_paused &&\n"
        "\t\t    (service->job.phase != AIUS_SD_PHASE_BEGIN ||\n"
        "\t\t     !aius_runtime_storage_sd_fifo_defer()))\n",
    ), "starts/begin-hold-back", "must not consult")
    latch = assert_bringup_latch_at_load_dsp_end
    note_line = "\t\taius_optical_admission_note_bringup_loaded(&g_http_admission);\n"
    _expect_fail(latch, _mutated(
        good, OPTICAL_MAIN_SOURCE, note_line, "",
    ), "r4/latch-never-set", "only in sd_step_complete")
    _expect_fail(latch, _mutated(
        _mutated(good, OPTICAL_MAIN_SOURCE, note_line, ""),
        OPTICAL_MAIN_SOURCE, "\t\t\tg_sd_bringup_done = true;\n",
        "\t\t\tg_sd_bringup_done = true;\n" + "\t" + note_line,
    ), "r4/latch-at-probe", "LOAD_DSP step")
    _expect_fail(latch, _mutated(
        good, OPTICAL_MAIN_SOURCE, note_line, "\t\tif (ok)\n\t" + note_line,
    ), "r4/latch-only-on-success", "unconditionally")
    _expect_fail(latch, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\treturn aius_optical_admission_sd_fifo_defer(&g_http_admission, &sample);",
        "\treturn g_sd_bringup_done && sd_short(&sample);",
    ), "r4/defer-bypasses-latch", "g_http_admission")
    _expect_fail(latch, _mutated(
        good, APP_SOURCE, " ||\n\t    admission->bringup_loaded == 0u)", ")",
    ), "r4/predicate-ignores-latch", "latch is clear")
    _expect_fail(latch, _mutated(
        good, APP_SOURCE, "admission->bringup_loaded = 1u;", "(void)admission;",
    ), "r4/setter-noop", "must set the latch")
    orphan = assert_orphan_staging_dropped
    _expect_fail(orphan, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\tsd_settle_http_write();\n\tsd_drop_orphan_staging();\n",
        "\tsd_drop_orphan_staging();\n\tsd_settle_http_write();\n",
    ), "r4/drop-before-settle", "right after")
    _expect_fail(orphan, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\t    g_sd_http_job_seen ||\n", "",
    ), "r4/drop-seen-job", "never seen")
    _expect_fail(orphan, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t\t    g_runtime.network_http.deferred_start_present,\n", "\t\t    false,\n",
    ), "r4/drop-held-start", "deferred_start_present")
    _expect_fail(orphan, _mutated(
        good, APP_SOURCE, "\treturn http->state != AIUS_HTTP_TX_DISPATCH &&\n\t       ",
        "\treturn ",
    ), "r4/drop-in-dispatch", "AIUS_HTTP_TX_DISPATCH")
    _expect_fail(orphan, _mutated(
        good, APP_SOURCE, "\tif (http == NULL || start_held || start_queued)\n",
        "\tif (http == NULL || start_held)\n",
    ), "r4/drop-queued-start", "held or queued")
    r3 = assert_round3_admission_wiring
    _expect_fail(r3, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tif (v == SHED)\n\t\tsd_disarm_shed_staging(query->sd_job);\n", "",
    ), "r3/no-disarm", "F-S1")
    _expect_fail(r3, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\tg_config_upload_pending = false;\n", "",
    ), "r3/disarm-partial", "g_config_upload_pending")
    _expect_fail(r3, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "&sample, g_runtime.network_http.deferred_start_present", "&sample, false",
    ), "r3/no-reconsult", "F-S2")
    _expect_fail(r3, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tadmission_sd_window_sample();\n\taius_sd_teensy_set_mode(target, dma);",
        "\taius_sd_teensy_set_mode(target, dma);\n\tadmission_sd_window_sample();",
    ), "r3/sample-after-switch", "F-S3")
    # Phase 4g delivery audit: findings 3, 2 and 6.
    wiring = assert_fill_route_and_card_wiring
    _expect_fail(wiring, _mutated(
        good, HTTP_SERVICE_SOURCE, "\tif (verdict == AIUS_HTTP_VERDICT_DEFER)\n\t\treturn;\n", "",
    ), "f3/route-defer-admits", "hold a DEFERred DISPATCH")
    _expect_fail(wiring, _mutated(
        good, HTTP_SERVICE_SOURCE,
        "\tif (service->deferred_dispatch_present) {\n"
        "\t\tdispatch_route(service, app, &held);\n\t\treturn true;\n\t}\n"
        "\taius_http_transaction_step(&service->http, now_ms, event, &action);\n",
        "\taius_http_transaction_step(&service->http, now_ms, event, &action);\n"
        "\tif (service->deferred_dispatch_present) {\n"
        "\t\tdispatch_route(service, app, &held);\n\t\treturn true;\n\t}\n",
    ), "f3/held-after-step", "before the transaction step")
    _expect_fail(wiring, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tif (!sd_start && aius_optical_app_route_reads_card(query->request) &&\n",
        "\tif (!sd_start && false &&\n",
    ), "f3/no-route-wait", "direct-card route")
    _expect_fail(wiring, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t\tif (fill_cancel)\n\t\t\tweb_cache_fill_cancel();\n\t\treturn AIUS_HTTP_VERDICT_DEFER;\n",
        "\t\treturn AIUS_HTTP_VERDICT_DEFER;\n",
    ), "f3/route-wait-no-cancel", "one cancel entry")
    _expect_fail(wiring, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\taius_optical_web_fill_route_end(&g_web_fill, millis());\n", "",
    ), "f3/route-end-missing", "app_dispatch_end")
    _expect_fail(wiring, _mutated(
        good, OPTICAL_MAIN_SOURCE, "\t    !g_runtime.network_http.deferred_dispatch_present &&\n", "",
    ), "f3/drop-under-held-dispatch", "held DISPATCH")
    _expect_fail(wiring, _mutated(
        good, OPTICAL_MAIN_SOURCE, "g_sd_ready, storage.target.mounted,", "g_sd_ready, true,",
    ), "f6/follow-ignores-mount", "follow the card")
    _expect_fail(wiring, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t\tdirect_card_settle(SD.sdErrorCode());\n\t\treturn 1;", "\t\treturn 1;",
    ), "f6/listing-unsettled", "app_read_directory must settle")
    _expect_fail(wiring, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tconst ProducerSdWindow window;\n\tSD.clearError();\n\tFile directory = SD.open(p);",
        "\tconst ProducerSdWindow window;\n\tFile directory = SD.open(p);",
    ), "f6/listing-stale-code", "app_read_directory")


PRODUCER_YIELD_CHECKS = (
    ("sd-window-only-in-w2-windowed-sources",
     assert_sd_window_only_in_w2_windowed_sources),
    ("no-yield-point-at-epoch-complete", assert_no_yield_point_at_epoch_complete),
    ("sd-naming-functions-are-w2-sources",
     assert_sd_naming_functions_are_w2_sources),
    ("no-stdio-stream-names", assert_no_stdio_stream_names),
    ("nested-pass-ledger-wrapper", assert_nested_pass_ledger_wrapper),
    ("stack-paint-skips-the-mpu-trap", assert_stack_paint_skips_the_mpu_trap),
    ("audio-service-run-is-bracketed", assert_audio_service_run_is_bracketed),
)


def _producer_yield_fixture():
    storage = (
        "namespace {\n"
        "FLASHMEM bool route_action(AiusRuntimeStorage *service)\n{\n"
        "\taius_producer_yield_window_open();\n"
        "\tservice->pending_event =\n"
        "\t\taius_sd_teensy_execute(&service->target, action);\n"
        "\taius_producer_yield_window_close();\n"
        "\treturn true;\n}\n"
        "FLASHMEM __attribute__((noinline, format(printf, 4, 5))) bool diag_append(\n"
        "\tchar *o, size_t c, size_t *l, const char *f, ...)\n{\n"
        "\twritten = vsnprintf(o + *l, c - *l, f, arguments);\n"
        "\t(void)aius_producer_yield_point();\n\treturn true;\n}\n"
        "}\n"
    )
    main = (
        "static FLASHMEM __attribute__((noinline)) bool status_append(\n"
        "\tchar *b, size_t c, size_t *l, const char *f, ...)\n{\n"
        "\tint written = vsnprintf(b + *l, c - *l, f, arguments);\n"
        "\t(void)aius_producer_yield_point();\n\treturn true;\n}\n"
        "struct ProducerSdWindow {\n"
        "\tProducerSdWindow() { aius_producer_yield_window_open(); }\n"
        "\t~ProducerSdWindow() { aius_producer_yield_window_close(); }\n"
        "};\n"
        "static FLASHMEM int ota_copy_staged_to_psram(uint8_t *d)\n{\n"
        "\tFile staged = SD.open(\"/x\");\n\treturn 0;\n}\n"
        "static FLASHMEM int app_read_directory(void *, const char *p)\n{\n"
        "\tif (!g_sd_ready)\n\t\treturn 1;\n"
        "\tconst ProducerSdWindow window;\n"
        "\tFile directory = SD.open(p);\n\treturn 0;\n}\n"
        "static FLASHMEM void sd_service_step(void)\n{\n"
        "\t/* SD. and File in a comment are not code. */\n"
        "\taius_sd_teensy_execute(&g_runtime.storage.target, action);\n}\n"
        "static FLASHMEM void platform_checkpoint(void *, uint16_t checkpoint)\n{\n"
        "\taius_boot_record_checkpoint(&g_boot_ledger, checkpoint);\n"
        "\tif (checkpoint != AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE)\n"
        "\t\t(void)aius_producer_yield_point();\n}\n"
        "static FLASHMEM __attribute__((noinline)) void web_cache_verify_yield(void *)\n{\n"
        "\t(void)aius_producer_yield_point();\n}\n"
        "static FLASHMEM bool web_cache_seam_verify(void *, uint8_t entry)\n{\n"
        "\treturn aius_web_cache_verify(&g_web_cache, entry,\n"
        "\t                             web_cache_verify_yield, nullptr);\n}\n"
        "static constexpr uintptr_t kStackTrapBytes = 32u;\n"
        "static inline __attribute__((always_inline)) uint32_t *stack_paint_base(void)\n{\n"
        "\treturn (uint32_t *)((((uintptr_t)&_ebss) & ~(kStackTrapBytes - 1u)) +\n"
        "\t                    kStackTrapBytes);\n}\n"
        "static FLASHMEM __attribute__((noinline)) void stack_paint(void)\n{\n"
        "\tuint32_t *word = stack_paint_base();\n\t*word = 0u;\n}\n"
        "static FLASHMEM void stack_scan(uint32_t now_ms)\n{\n"
        "\tconst uint32_t *word = stack_paint_base();\n\t(void)word;\n}\n"
        "static FLASHMEM bool platform_audio(void *, uint32_t now_ms)\n{\n"
        "\taius_producer_yield_visit_begin();\n"
        "\tconst AiusStatus status =\n"
        "\t\taius_audio_service_run(&g_audio_service, g_audio_reset_sequence);\n"
        "\taius_producer_yield_visit_end();\n"
        "\treturn status == AIUS_OK;\n}\n"
        "FLASHMEM void aius_producer_yield_plat_run_pass(void)\n{\n"
        "\t(void)aius_audio_service_run(&g_audio_service, 0u);\n}\n"
        "FLASHMEM void aius_producer_yield_plat_ledger_record(uint16_t c)\n{\n"
        "\taius_boot_record_checkpoint(&g_boot_ledger, c);\n"
        "\t(void)aius_boot_storage_teensy_commit(&g_boot_storage, &g_boot_ledger);\n"
        "}\n"
    )
    library = (
        "static void nested_pass(AiusProducerYieldState *s)\n{\n"
        "\tuint16_t saved = aius_producer_yield_plat_ledger_checkpoint();\n"
        "\ts->in_producer = 1u;\n"
        "\taius_producer_yield_plat_ledger_record(AIUS_BOOT_CHECKPOINT_AUDIO);\n"
        "\taius_producer_yield_plat_run_pass();\n"
        "\taius_producer_yield_plat_ledger_record(saved);\n"
        "\tproducer_leave(s);\n}\n"
        "bool aius_producer_yield_point(void)\n{\n\treturn false;\n}\n"
    )
    adapter = (
        "FLASHMEM AiusSdEvent aius_sd_teensy_execute(AiusSdTeensy *a)\n{\n"
        "\tFile opened = SD.open(\"/x\");\n\treturn ok();\n}\n"
    )
    app = (
        "static FLASHMEM __attribute__((noinline)) bool web_cache_body_chunk(\n"
        "\tvoid *ctx, uint32_t chunk, uint8_t *dst, uint32_t *bytes)\n{\n"
        "\tif ((chunk + 1u) % AIUS_OPTICAL_APP_WEB_YIELD_CHUNKS == 0u)\n"
        "\t\t(void)aius_producer_yield_point();\n\treturn true;\n}\n"
        "static FLASHMEM const void *dispatch_www_cached(void *d)\n{\n"
        "\treturn d;\n}\n"
    )
    return {
        STORAGE_SOURCE: storage,
        OPTICAL_MAIN_SOURCE: main,
        "src/aius_optical_app.cpp": app,
        PRODUCER_YIELD_SOURCE: library,
        SD_ADAPTER_SOURCE: adapter,
        "lib/aius_other/src/other.c": "/* fopen and Print in a comment */\n"
                                      "int other(void)\n{\n\treturn 0;\n}\n",
    }


def _mutated(sources, path, old, new):
    assert sources[path].count(old) == 1, f"fixture mutation anchor {old!r}"
    mutated = dict(sources)
    mutated[path] = sources[path].replace(old, new)
    return mutated


def run_producer_yield_self_test():
    good = _producer_yield_fixture()
    for _, check in PRODUCER_YIELD_CHECKS:
        _expect_pass(check, good, "producer-yield/good")

    window = assert_sd_window_only_in_w2_windowed_sources
    _expect_fail(window, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tFile staged = SD.open",
        "\tconst ProducerSdWindow window;\n\tFile staged = SD.open",
    ), "window/ota-copy", "ota_copy_staged_to_psram")
    _expect_fail(window, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tconst ProducerSdWindow window;\n\tFile directory = SD.open(p);",
        "\tFile directory = SD.open(p);",
    ), "window/scan-lost", "lost their SD window")
    _expect_fail(window, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tconst ProducerSdWindow window;\n\tFile directory = SD.open(p);",
        "\tif (!SD.exists(p))\n\t\treturn 0;\n"
        "\tconst ProducerSdWindow window;\n\tFile directory = SD.open(p);",
    ), "window/declared-late", "before its ProducerSdWindow")
    _expect_fail(window, _mutated(
        good, STORAGE_SOURCE,
        "\taius_producer_yield_window_close();\n\treturn true;",
        "\treturn true;",
    ), "window/storage-close-missing", "close it after")
    _expect_fail(window, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "~ProducerSdWindow() { aius_producer_yield_window_close(); }",
        "~ProducerSdWindow() {}",
    ), "window/destructor-no-close", "destructor")

    epoch = assert_no_yield_point_at_epoch_complete
    _expect_fail(epoch, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tif (checkpoint != AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE)\n\t\t",
        "\t",
    ), "epoch/unguarded", "EPOCH_COMPLETE")
    _expect_fail(epoch, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "checkpoint != AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE",
        "checkpoint != AIUS_BOOT_CHECKPOINT_AUDIO",
    ), "epoch/wrong-guard", "EPOCH_COMPLETE")
    _expect_fail(epoch, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\taius_sd_teensy_execute(&g_runtime.storage.target, action);\n}",
        "\taius_sd_teensy_execute(&g_runtime.storage.target, action);\n"
        "\t(void)aius_producer_yield_point();\n}",
    ), "epoch/second-yield-root", "callers must be exactly")
    _expect_fail(epoch, _mutated(
        good, STORAGE_SOURCE,
        "\t(void)aius_producer_yield_point();\n\treturn true;\n}\n",
        "\treturn true;\n}\n",
    ), "epoch/render-append-lost", "callers must be exactly")
    _expect_fail(epoch, _mutated(
        good, "src/aius_optical_app.cpp",
        "\t\t(void)aius_producer_yield_point();\n\treturn true;\n}\n",
        "\treturn true;\n}\n",
    ), "epoch/web-cache-send-yield-lost", "callers must be exactly")
    _expect_fail(epoch, _mutated(
        good, "src/aius_optical_app.cpp",
        "\treturn d;\n}\n",
        "\t(void)aius_producer_yield_point();\n\treturn d;\n}\n",
    ), "epoch/web-cache-dispatch-yield", "callers must be exactly")

    naming = assert_sd_naming_functions_are_w2_sources
    _expect_fail(naming, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\taius_sd_teensy_execute(&g_runtime.storage.target, action);\n}",
        "\taius_sd_teensy_execute(&g_runtime.storage.target, action);\n"
        "\t(void)SD.exists(\"/aius\");\n}",
    ), "naming/raw-sd-in-service", "sd_service_step")
    _expect_fail(naming, _mutated(
        good, "lib/aius_other/src/other.c",
        "int other(void)\n{\n\treturn 0;",
        "int other(void)\n{\n\tFsFile f;\n\treturn 0;",
    ), "naming/fsfile-in-lib", "other")
    _expect_fail(naming, dict(good, **{
        "src/member.cpp": "FLASHMEM bool Owner::probe(void)\n{\n"
                          "\treturn SD.mediaPresent();\n}\n",
    }), "naming/member-function", "Owner::probe")

    stdio = assert_no_stdio_stream_names
    for name in STDIO_FORBIDDEN_NAMES:
        _expect_fail(stdio, _mutated(
            good, "lib/aius_other/src/other.c",
            "\treturn 0;", f"\t(void){name};\n\treturn 0;",
        ), f"stdio/{name}", name)

    ledger = assert_nested_pass_ledger_wrapper
    _expect_fail(ledger, _mutated(
        good, PRODUCER_YIELD_SOURCE,
        "\taius_producer_yield_plat_ledger_record(saved);\n", "",
    ), "ledger/no-restore", "E5")
    _expect_fail(ledger, _mutated(
        good, PRODUCER_YIELD_SOURCE,
        "\taius_producer_yield_plat_ledger_record(AIUS_BOOT_CHECKPOINT_AUDIO);\n"
        "\taius_producer_yield_plat_run_pass();\n",
        "\taius_producer_yield_plat_run_pass();\n"
        "\taius_producer_yield_plat_ledger_record(AIUS_BOOT_CHECKPOINT_AUDIO);\n",
    ), "ledger/record-after-pass", "E5")
    _expect_fail(ledger, _mutated(
        good, PRODUCER_YIELD_SOURCE,
        "bool aius_producer_yield_point(void)\n{\n\treturn false;",
        "bool aius_producer_yield_point(void)\n{\n"
        "\taius_producer_yield_plat_run_pass();\n\treturn false;",
    ), "ledger/unwrapped-pass", "only inside nested_pass")
    _expect_fail(ledger, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t(void)aius_boot_storage_teensy_commit(&g_boot_storage, &g_boot_ledger);\n",
        "",
    ), "ledger/no-commit", "commit")

    trap = assert_stack_paint_skips_the_mpu_trap
    _expect_fail(trap, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tuint32_t *word = stack_paint_base();",
        "\tuint32_t *word = (uint32_t *)&_ebss;",
    ), "trap/paint-at-ebss", "_ebss directly")
    _expect_fail(trap, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\tconst uint32_t *word = stack_paint_base();",
        "\tconst uint32_t *word = (const uint32_t *)&_ebss;",
    ), "trap/scan-at-ebss", "_ebss directly")
    _expect_fail(trap, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        " +\n\t                    kStackTrapBytes);",
        ");",
    ), "trap/base-not-stepped", "step over it")
    _expect_fail(trap, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "kStackTrapBytes = 32u;", "kStackTrapBytes = 4u;",
    ), "trap/wrong-size", "must be 32")

    visit = assert_audio_service_run_is_bracketed
    _expect_fail(visit, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\taius_producer_yield_visit_end();\n\treturn status",
        "\treturn status",
    ), "visit/end-missing", "platform_audio")
    _expect_fail(visit, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\taius_producer_yield_visit_begin();\n\tconst AiusStatus",
        "\tconst AiusStatus",
    ), "visit/begin-missing", "platform_audio")
    _expect_fail(visit, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\taius_sd_teensy_execute(&g_runtime.storage.target, action);\n}",
        "\taius_sd_teensy_execute(&g_runtime.storage.target, action);\n"
        "\t(void)aius_audio_service_run(&g_audio_service, 0u);\n}",
    ), "visit/bare-call", "sd_service_step")
    _expect_fail(visit, _mutated(
        good, OPTICAL_MAIN_SOURCE,
        "\t(void)aius_audio_service_run(&g_audio_service, 0u);\n}\n"
        "FLASHMEM void aius_producer_yield_plat_ledger_record",
        "\t(void)aius_audio_service_run(&g_audio_service, 0u);\n"
        "\t(void)aius_audio_service_run(&g_audio_service, 0u);\n}\n"
        "FLASHMEM void aius_producer_yield_plat_ledger_record",
    ), "visit/double-nested-pass", "more than one pass")
    _expect_fail(visit, dict(good, **{
        "src/other.cpp": "FLASHMEM void other(void)\n{\n"
                         "\t(void)aius_audio_service_run(&s, 0u);\n}\n",
    }), "visit/other-file", "src/other.cpp")


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
    run_producer_yield_self_test()
    run_admission_self_test()
    print("optical-cutover self-test (checks discriminate against synthetic input): PASS")

    text = OPTICAL_MAIN.read_text()

    if args.group in ("positive", "all"):
        run_checks("positive", POSITIVE_CHECKS, text)
        # The 24-bit output path's other two files, each read on its own: the
        # claim is that ours includes the snapshot and changes nothing in it, so
        # neither file can be checked through the other.
        run_checks("snapshot", OUTPUT_PATH_CHECKS, TX_SNAPSHOT.read_text())
        run_checks("transmit-module", TRANSMIT_MODULE_CHECKS, TX_MODULE.read_text())
        # Phase 4e: read over every source under firmware/src and firmware/lib.
        run_checks(
            "producer-yield", PRODUCER_YIELD_CHECKS, read_producer_yield_sources()
        )
        # Phase 4f: admission and render-yield guards over the same sources.
        run_checks("admission", ADMISSION_CHECKS, read_producer_yield_sources())
        # Phase 4h: the write-behind wiring, over the real sources only (its
        # discrimination is proved by mutating them).
        run_sd_overlay_self_test(read_producer_yield_sources())
        run_checks("sd-overlay", SD_OVERLAY_CHECKS, read_producer_yield_sources())
    if args.group in ("absence", "all"):
        run_checks("absence", ABSENCE_CHECKS, text)


if __name__ == "__main__":
    sys.exit(main())
