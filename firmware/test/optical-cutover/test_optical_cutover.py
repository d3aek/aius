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


POSITIVE_CHECKS = (
    ("setup() is construction-only", assert_setup_is_construction_only),
    ("loop() delegates exactly one epoch", assert_loop_delegates_one_epoch),
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
    if args.group in ("absence", "all"):
        run_checks("absence", ABSENCE_CHECKS, text)


if __name__ == "__main__":
    sys.exit(main())
