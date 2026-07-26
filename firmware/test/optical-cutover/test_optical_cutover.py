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
    assert occurrences == 2, (
        "expected exactly two reset_application() occurrences -- its definition "
        f"and the one gated call site -- found {occurrences}"
    )
    assert "reset_application(" in gate, (
        "the single reset_application() call site must be the gated one"
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
    assert "aius_replay_service_stop(" in clean, (
        "replay_clock_service() takes the clock back from a carrier but never "
        "stops the replay, so a live input and a replay both hold the audio "
        "path; the live input must win outright"
    )
    assert "record_event(" in clean, (
        "a carrier displacing a replay is a transition, not a counter, and "
        "replay_clock_service() puts nothing in the event ring -- so the one "
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
}
# The three that predate the counter edges; the rest are the edges themselves.
COUNTER_EDGE_TYPES = tuple(
    name for name, value in EVENT_TYPE_VALUES.items() if value > 3
)
EDGE_SERVICE = "service_event_edges"
EDGE_HELPER = "counter_edge"
AUDIO_CALLBACK_MARKER = "void update(void) override"
ENUM_ENTRY = re.compile(r"\b(AIUS_EVENT_[A-Z0-9_]+)\s*=\s*(\d+)\s*[,}]")
PREVIOUS_STATIC = re.compile(r"^static\b.*\bg_prev_")
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
BEGIN_PIPELINE_MARKER = "void begin_pipeline(void)"
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
    assert "<< 8" not in clean, (
        "the transmit handler shifts by 8, which is the 16-bit reduction it "
        "exists to remove: the DSP's word goes into the 24-bit register unchanged"
    )
    assert "audio_block_t" not in clean, (
        "the transmit handler still touches audio_block_t; bypassing that type "
        "for the final hop is the whole point of the path"
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
)


POSITIVE_CHECKS = (
    ("setup() is construction-only", assert_setup_is_construction_only),
    ("loop() delegates exactly one epoch", assert_loop_delegates_one_epoch),
    (
        "a requested reset waits for the drained transaction, bounded",
        assert_requested_reset_waits_for_the_drained_transaction,
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
    assert "WDOG1_WSR" not in clean, (
        "a raw WDOG1_WSR register write is present; the feed must go "
        "through the boot adapter, not reimplement it here"
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


def _synthetic_counter_edges():
    """A minimal file with the counter-edge shape: the allocated enum, one
    scalar static and one guarded emission per counter, the three transition
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
        "static FLASHMEM void reset_application(void)\n{\n"
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
        "static FLASHMEM void replay_clock_service(void)\n"
        "{\n"
        "\tif ((g_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY ||\n"
        "\t     g_replay_service.source == AIUS_REPLAY_SOURCE_SWITCHING_TO_REPLAY) &&\n"
        "\t    AudioInputSPDIF3::pllLocked()) {\n"
        "\t\treplay_job_cancel();\n"
        "\t\taius_replay_service_stop(&g_replay_service);\n"
        "\t\tg_replay_stop_reason = REPLAY_STOP_CARRIER;\n"
        "\t\trecord_event(3, current_state(), REPLAY_STOP_CARRIER);\n"
        "\t}\n"
        "\tconst bool output_drives =\n"
        "\t\tg_replay_service.source == AIUS_REPLAY_SOURCE_REPLAY &&\n"
        "\t\t!AudioInputSPDIF3::pllLocked();\n"
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
        "clock/never-called", "never calls replay_clock_service()",
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

    # the 24-bit output path.
    good_output = (
        "class AiusAudioProcessor : public AudioStream\n"
        "{\n"
        "\tvoid begin_pipeline(void)\n"
        "\t{\n"
        "\t\taius_out16_init(&g_out16, AIUS_DSP_DITHER_SEED_DEFAULT, 1);\n"
        "\t\taius_spdif_tx24_begin();\n"
        "\t}\n"
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
