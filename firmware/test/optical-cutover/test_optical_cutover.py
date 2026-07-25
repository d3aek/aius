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
                      pinned 8 s interval and fed exactly once per epoch, and
                      all three status channels (UDP, status JSON, USB ID)
                      carry the recovery state.
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
