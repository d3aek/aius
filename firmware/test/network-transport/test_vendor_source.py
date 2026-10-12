#!/usr/bin/env python3
"""Source-contract tests for the repository-pinned NativeEthernet transport.

Two contracts live here. The first pins the vendored NativeEthernet snapshot by
hash and by structure. The second, added after the `read_once()` defect, pins the
*fake* FNET header against the real one: `fake/fnet.h` had been derived from the
adapter that calls it rather than from the dependency it stands for, so the suite
could only ever confirm the adapter's beliefs. See
test_fake_fnet_declares_the_dependencys_signatures().
"""

import os
import re
import hashlib
import json
import subprocess
from pathlib import Path, PurePosixPath


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
VENDOR_ROOT = FIRMWARE_ROOT / "vendor" / "NativeEthernet"
SOURCE_ROOT = VENDOR_ROOT / "src"
FAKE_FNET = FIRMWARE_ROOT / "test" / "network-transport" / "fake" / "fnet.h"
# The FNET the target links: a Teensy framework library, not a repository
# snapshot, so it is located rather than vendored.
FNET_RELATIVE = Path("packages/framework-arduinoteensy/libraries/FNET/src")

# The one application-level owner of a vendor transport header, and now the
# only one: Phase 14 slice S2b deleted optical_main.cpp's legacy blocking
# network path together with its NativeEthernet and FNET includes, and removed
# the exact-path exception that had carried it in the same commit.
DESIGNATED_INCLUDE_OWNER = PurePosixPath("src/aius_network_teensy.cpp")

TARGET_FUNCTIONS = (
    ("NativeEthernet.cpp", "bool EthernetClass::beginNoWait(uint8_t *mac)"),
    ("NativeEthernet.cpp", "bool EthernetClass::dhcpStart(unsigned long responseTimeout)"),
    ("NativeEthernet.cpp", "void EthernetClass::dhcpStop()"),
    ("NativeEthernet.cpp", "void EthernetClass::end()"),
    (
        "NativeEthernetClient.cpp",
        "size_t EthernetClient::write(const uint8_t *buf, size_t size)",
    ),
    ("Nativesocket.cpp", "uint16_t EthernetClass::socketSend("),
    ("NativeEthernetServer.cpp", "void EthernetServer::begin()"),
    ("NativeEthernetServer.cpp", "void EthernetServer::end()"),
    ("NativeEthernetServer.cpp", "void EthernetServer::invalidateService()"),
    ("NativeEthernetServer.cpp", "void EthernetServer::closeListener()"),
)


def source(name):
    return (SOURCE_ROOT / name).read_text()


def without_comments_and_literals(text):
    pattern = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )
    return pattern.sub(lambda match: " " * len(match.group(0)), text)


def function_body(text, marker):
    start = text.find(marker)
    assert start >= 0, f"missing function marker {marker!r}"
    opening = text.find("{", start + len(marker))
    assert opening >= 0, f"missing body for {marker!r}"
    clean = without_comments_and_literals(text)
    depth = 0
    for index in range(opening, len(clean)):
        if clean[index] == "{":
            depth += 1
        elif clean[index] == "}":
            depth -= 1
            if depth == 0:
                return text[opening + 1 : index]
    raise AssertionError(f"unterminated body for {marker!r}")


def matching_parenthesis(text, opening):
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError("unterminated loop condition")


def strip_outer_parentheses(condition):
    result = condition.strip()
    while result.startswith("(") and result.endswith(")"):
        if matching_parenthesis(result, 0) != len(result) - 1:
            break
        result = result[1:-1].strip()
    return result


def is_constant_true(condition):
    normalized = strip_outer_parentheses(condition)
    return re.fullmatch(r"(?:true|1[uUlL]*)", normalized) is not None


def for_fields(condition):
    fields = []
    start = 0
    depth = 0
    for index, character in enumerate(condition):
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        elif character == ";" and depth == 0:
            fields.append(condition[start:index])
            start = index + 1
    fields.append(condition[start:])
    return fields


def unbounded_loop(body):
    clean = without_comments_and_literals(body)
    for match in re.finditer(r"\bwhile\s*\(", clean):
        opening = clean.find("(", match.start())
        closing = matching_parenthesis(clean, opening)
        if is_constant_true(clean[opening + 1 : closing]):
            return match
    for match in re.finditer(r"\bfor\s*\(", clean):
        opening = clean.find("(", match.start())
        closing = matching_parenthesis(clean, opening)
        fields = for_fields(clean[opening + 1 : closing])
        if len(fields) == 3 and (
            not fields[1].strip() or is_constant_true(fields[1])
        ):
            return match
    return None


def assert_target_functions_are_one_shot(sources):
    for filename, marker in TARGET_FUNCTIONS:
        body = function_body(sources[filename], marker)
        assert not unbounded_loop(body), (
            f"unbounded loop in target primitive {filename}:{marker}"
        )


def forbidden_transport_includes(source_text):
    include = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
    forbidden = []
    for header in include.findall(source_text):
        basename = PurePosixPath(header).name
        if basename.startswith("NativeEthernet") or basename.startswith("fnet"):
            forbidden.append(header)
        elif "/fnet_" in f"/{header}":
            forbidden.append(header)
    return forbidden


# Build output, never authored source. `.pio/` in particular can contain whole
# copies of the repository: the removed evidence apparatus left
# `.pio/source-snapshots/<sha>/…/repository/…` trees behind, and scanning those
# reports every snapshotted `optical_main.cpp` and vendored NativeEthernet file
# as a fresh violation. Both directories are gitignored.
BUILD_OUTPUT_DIRS = {".pio", ".build"}


def repository_transport_include_violations():
    violations = {}
    vendor_prefix = VENDOR_ROOT.relative_to(FIRMWARE_ROOT)
    for path in FIRMWARE_ROOT.rglob("*"):
        if path.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp", ".ino"}:
            continue
        relative = PurePosixPath(path.relative_to(FIRMWARE_ROOT).as_posix())
        if relative == vendor_prefix or vendor_prefix in relative.parents:
            continue
        if relative.parts[0] == "test":
            continue
        if relative.parts[0] in BUILD_OUTPUT_DIRS:
            continue
        headers = forbidden_transport_includes(path.read_text())
        if headers:
            violations[relative] = headers
    return violations


def test_provenance_and_snapshot_shape():
    notice = (VENDOR_ROOT / "NOTICE.md").read_text()
    license_text = (VENDOR_ROOT / "LICENSE").read_text()
    properties = (VENDOR_ROOT / "library.properties").read_text()

    assert "framework-arduinoteensy` version\n`1.162.0`" in notice
    assert "teensy@5.2.0" in notice
    assert "AIUS_NET_POLL_SINGLE_CONTEXT" in notice
    assert "MIT License" in license_text
    assert "Permission is hereby granted, free of charge" in license_text
    assert "name=AiusNativeEthernet" in properties
    assert not (VENDOR_ROOT / "examples").exists()

    expected_sources = {
        "NativeDns.cpp",
        "NativeDns.h",
        "NativeEthernet.cpp",
        "NativeEthernet.h",
        "NativeEthernetClient.cpp",
        "NativeEthernetClient.h",
        "NativeEthernetServer.cpp",
        "NativeEthernetServer.h",
        "NativeEthernetUdp.cpp",
        "NativeEthernetUdp.h",
        "NativeMdns.cpp",
        "NativeMdns.h",
        "Nativesocket.cpp",
    }
    actual_sources = {
        path.name for path in SOURCE_ROOT.iterdir() if path.is_file()
    }
    assert actual_sources == expected_sources
    assert (SOURCE_ROOT / "utility" / "NativeW5100.h").is_file()


def test_upstream_hash_baselines_and_explicit_patch_set():
    upstream_manifest = {}
    for line in (VENDOR_ROOT / "UPSTREAM_SHA256").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        digest, relative = line.split("  ", 1)
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        upstream_manifest[PurePosixPath(relative)] = digest

    patched_manifest = {}
    for line in (VENDOR_ROOT / "PATCHED_SHA256").read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        digest, relative = line.split("  ", 1)
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        patched_manifest[PurePosixPath(relative)] = digest

    copied = {
        PurePosixPath("LICENSE"),
        PurePosixPath("library.json"),
        PurePosixPath("library.properties"),
    }
    copied.update(
        PurePosixPath(path.relative_to(VENDOR_ROOT).as_posix())
        for path in SOURCE_ROOT.rglob("*")
        if path.is_file()
    )
    assert set(upstream_manifest) == copied

    patched = {
        PurePosixPath("src/NativeEthernet.cpp"),
        PurePosixPath("src/NativeEthernet.h"),
        PurePosixPath("src/NativeEthernetClient.cpp"),
        PurePosixPath("src/NativeEthernetServer.cpp"),
        PurePosixPath("src/Nativesocket.cpp"),
        PurePosixPath("library.json"),
        PurePosixPath("library.properties"),
    }
    notice = (VENDOR_ROOT / "NOTICE.md").read_text()
    assert set(patched_manifest) == patched
    for relative in copied:
        actual = hashlib.sha256((VENDOR_ROOT / relative).read_bytes()).hexdigest()
        if relative in patched:
            assert actual != upstream_manifest[relative]
            assert actual == patched_manifest[relative], relative
            assert f"`{relative}`" in notice
        else:
            assert actual == upstream_manifest[relative], relative


def test_platformio_selects_repository_snapshot():
    config = (FIRMWARE_ROOT / "platformio.ini").read_text()
    optical = config.split("[env:teensy41-optical]", 1)[1]
    assert re.search(
        r"(?m)^lib_deps\s*=\s*symlink://vendor/NativeEthernet\s*$",
        optical,
    )
    assert re.search(r"(?m)^lib_ignore\s*=.*NativeEthernet", optical)
    assert "+<aius_network_teensy.cpp>" in optical
    common = config.split("[common]", 1)[1].split("[env:", 1)[0]
    assert "post:test/network-transport/verify_target.py" in common

    metadata = subprocess.run(
        ["pio", "project", "config", "--json-output"],
        cwd=FIRMWARE_ROOT,
        check=True,
        text=True,
        capture_output=True,
    )
    resolved = json.loads(metadata.stdout)
    optical_env = next(
        section for section in resolved
        if section[0] == "env:teensy41-optical"
    )
    values = dict(optical_env[1])
    # Phase 4e (E4) vendored the SD stack the same way; NativeEthernet stays first.
    assert values["lib_deps"] == [
        "symlink://vendor/NativeEthernet",
        "symlink://vendor/SdFat",
        "symlink://vendor/SD",
    ]
    assert "NativeEthernet" in values["lib_ignore"]


def defined_flag_values(flags, macro):
    """Every value `flags` gives `macro`, in order; a bare -Dmacro is 1 in C.

    An empty list means the macro is not defined on that command line at all,
    which is the shape of the original defect: FNET then applies its own
    `#ifndef` default and nobody reading platformio.ini can see what it is.
    """
    values = []
    for flag in flags:
        if flag == f"-D{macro}":
            values.append("1")
        elif flag.startswith(f"-D{macro}="):
            values.append(flag.split("=", 1)[1])
    return values


def suite_arm_flag_values(runner_text, suite, macro):
    """The same, read out of one `case` arm of scripts/test.sh."""
    arm = runner_text.split(f"\n{suite})", 1)[1].split("\n\t;;", 1)[0]
    return [
        found or "1"
        for found in re.findall(rf"-D{macro}(?:=(\S+))?", arm)
    ]


def test_target_and_suite_pin_the_same_fnet_cfg_tls():
    """The suite must compile the transport the image contains.

    FNET defaults FNET_CFG_TLS to 2 behind an `#ifndef`
    (fnet_user_config.h:443-445). platformio.ini overrode nothing, so the target
    compiled the vendor transport's `#if FNET_CFG_TLS` branches -- the TLS
    members of EthernetClient/EthernetServer, socketSend's tls/plain fork,
    EthernetServer::begin()'s fnet_tls_init() arm -- while scripts/test.sh
    compiled this suite with -DFNET_CFG_TLS=0. Every assertion the suite made
    about the send path was made against a function the image did not have.

    Nothing here is TLS-specific in spirit: it is the general rule that a
    configuration macro the fake fixes must be pinned on the target too, and the
    only way to keep it is to compare the two build descriptions. The value is
    read from the *resolved* environment rather than the [common] section, so
    inheritance breaking counts as drift.
    """
    resolved = json.loads(
        subprocess.run(
            ["pio", "project", "config", "--json-output"],
            cwd=FIRMWARE_ROOT,
            check=True,
            text=True,
            capture_output=True,
        ).stdout
    )
    optical = dict(
        next(
            section[1] for section in resolved
            if section[0] == "env:teensy41-optical"
        )
    )
    target_values = defined_flag_values(optical["build_flags"], "FNET_CFG_TLS")
    assert target_values == ["0"], (
        "env:teensy41-optical must pin -DFNET_CFG_TLS=0 exactly once; resolved "
        f"{target_values or 'nothing, so FNET_CFG_TLS defaults to 2'}"
    )

    runner = (FIRMWARE_ROOT / "scripts" / "test.sh").read_text()
    suite_values = suite_arm_flag_values(
        runner, "network-transport", "FNET_CFG_TLS")
    assert suite_values == ["0"], (
        "the network-transport arm of scripts/test.sh must pass "
        f"-DFNET_CFG_TLS=0 exactly once; found {suite_values}"
    )
    assert target_values == suite_values

    # Both readers must see the drift they exist for: the original defect was an
    # *absent* target flag against a present suite one, and a future one is just
    # as likely to be a non-zero value.
    assert defined_flag_values(["-O2", "-DFNET_CFG_IP6=0"], "FNET_CFG_TLS") == []
    assert defined_flag_values(["-DFNET_CFG_TLS=2"], "FNET_CFG_TLS") == ["2"]
    assert defined_flag_values(["-DFNET_CFG_TLS"], "FNET_CFG_TLS") == ["1"]
    assert suite_arm_flag_values(
        "\nfoo)\n\t\"$cxx\" -DFNET_CFG_TLS=2 \\\n\t\t-o x\n\t;;\n",
        "foo", "FNET_CFG_TLS") == ["2"]
    assert suite_arm_flag_values(
        "\nfoo)\n\t\"$cxx\" -O1 -o x\n\t;;\n", "foo", "FNET_CFG_TLS") == []

    # The fake declares no fnet_tls_* function, so a target compiled with TLS on
    # is not something this suite could link even if it wanted to. Comments are
    # stripped first: fake/fnet.h discusses the flag at length and must be free
    # to keep doing so.
    tls_declarations = [
        name for name in declared_signatures(FAKE_FNET.read_text())
        if name.startswith("fnet_tls")
    ]
    assert tls_declarations == [], tls_declarations


def test_one_shot_source_guard_and_guard_sensitivity():
    sources = {path.name: path.read_text() for path in SOURCE_ROOT.glob("*.cpp")}
    assert_target_functions_are_one_shot(sources)

    mutated = dict(sources)
    marker = "void EthernetClass::end()"
    body = function_body(mutated["NativeEthernet.cpp"], marker)
    mutated["NativeEthernet.cpp"] = mutated["NativeEthernet.cpp"].replace(
        body, body + "\nwhile (true) {}", 1
    )
    try:
        assert_target_functions_are_one_shot(mutated)
    except AssertionError as error:
        assert "EthernetClass::end" in str(error)
    else:
        raise AssertionError("source guard accepted a synthetic unbounded wait")

    mutated = dict(sources)
    body = function_body(mutated["NativeEthernet.cpp"], marker)
    mutated["NativeEthernet.cpp"] = mutated["NativeEthernet.cpp"].replace(
        body, body + "\ndo {} while (true);", 1
    )
    try:
        assert_target_functions_are_one_shot(mutated)
    except AssertionError as error:
        assert "EthernetClass::end" in str(error)
    else:
        raise AssertionError("source guard accepted a synthetic do-while wait")

    mutated = dict(sources)
    body = function_body(mutated["NativeEthernet.cpp"], marker)
    mutated["NativeEthernet.cpp"] = mutated["NativeEthernet.cpp"].replace(
        body, body + "\nfor (;;) {}", 1
    )
    try:
        assert_target_functions_are_one_shot(mutated)
    except AssertionError as error:
        assert "EthernetClass::end" in str(error)
    else:
        raise AssertionError("source guard accepted a synthetic for(;;) wait")

    mutated = dict(sources)
    body = function_body(mutated["NativeEthernet.cpp"], marker)
    mutated["NativeEthernet.cpp"] = mutated["NativeEthernet.cpp"].replace(
        body, body + "\nfor (; true;) {}", 1
    )
    try:
        assert_target_functions_are_one_shot(mutated)
    except AssertionError as error:
        assert "EthernetClass::end" in str(error)
    else:
        raise AssertionError("source guard accepted a synthetic for(;true;) wait")

    bounded_examples = (
        "\nunsigned i = 0; while (i < 3) { ++i; }",
        "\nunsigned i = 0; do { ++i; } while (i < 3);",
        "\nfor (unsigned i = 0; i < 3; ++i) {}",
    )
    for example in bounded_examples:
        bounded = dict(sources)
        body = function_body(bounded["NativeEthernet.cpp"], marker)
        bounded["NativeEthernet.cpp"] = bounded["NativeEthernet.cpp"].replace(
            body, body + example, 1
        )
        assert_target_functions_are_one_shot(bounded)


def test_begin_and_dhcp_ownership_contract():
    implementation = source("NativeEthernet.cpp")
    header = source("NativeEthernet.h")
    begin = function_body(
        implementation, "bool EthernetClass::beginNoWait(uint8_t *mac)"
    )
    dhcp_start = function_body(
        implementation,
        "bool EthernetClass::dhcpStart(unsigned long responseTimeout)",
    )
    dhcp_stop = function_body(implementation, "void EthernetClass::dhcpStop()")
    link_callback = function_body(
        implementation,
        "void EthernetClass::link_callback(",
    )
    dhcp_callback = function_body(
        implementation,
        "void EthernetClass::dhcp_cln_callback_updated(",
    )

    assert "static bool beginNoWait(uint8_t *mac);" in header
    assert "static bool dhcpStart(" in header
    assert "static void dhcpStop();" in header
    assert begin.count("fnet_init(") == 1
    assert begin.count("fnet_netif_init(") == 1
    assert begin.count("fnet_link_init(") == 1
    assert "dhcpStart(" not in begin
    assert "fnet_dhcp" not in begin
    assert "millis(" not in begin and "delay(" not in begin
    assert "if (stack_initialized)" in begin
    assert "if (!storage_initialized)" in begin

    assert dhcp_start.count("fnet_dhcp_cln_init(") == 1
    assert "dhcp_descriptor = fnet_dhcp_cln_init" in dhcp_start
    assert dhcp_stop.count("fnet_dhcp_cln_release(") == 1
    assert "dhcp_descriptor = nullptr" in dhcp_stop
    assert "fnet_dhcp" not in link_callback
    assert link_callback.find("if (!stack_initialized)") < link_callback.find(
        "link_status = connected"
    )
    assert "user_link_callback(connected, user_link_context)" in link_callback
    assert "fnet_dhcp" not in dhcp_callback
    assert "user_dhcp_callback(user_dhcp_context)" in dhcp_callback


def test_client_write_is_capacity_bounded_and_single_attempt():
    client_write = function_body(
        source("NativeEthernetClient.cpp"),
        "size_t EthernetClient::write(const uint8_t *buf, size_t size)",
    )
    socket_send = function_body(
        source("Nativesocket.cpp"),
        "uint16_t EthernetClass::socketSend(",
    )
    socket_setup = function_body(
        source("Nativesocket.cpp"),
        "uint8_t EthernetClass::socketBegin(uint8_t protocol, uint16_t port)",
    )

    assert client_write.count("socketSendAvailable(") == 1
    assert client_write.count("socketSend(") == 1
    assert "if (capacity == 0)" in client_write
    assert "if (size > capacity)" in client_write
    # The plain send appears once in the TLS build's else branch and once in
    # the preprocessor's non-TLS build; either compiled path executes one call.
    assert socket_send.count("fnet_socket_send(") == 2
    assert socket_send.count("fnet_tls_socket_send(") == 1
    assert "socketSendAvailable(" not in socket_send
    assert not unbounded_loop(socket_send)
    assert ".l_onoff = FNET_FALSE" in socket_setup
    assert ".l_linger = 0" in socket_setup


def test_server_accept_initialises_fromlen():
    """The accept callback must still initialise `fromlen`, and say why.

    Defect 20: upstream's `EthernetServer::poll()` passes an *uninitialised*
    `fnet_size_t fromlen` to `fnet_socket_accept()`. FNET's
    `_fnet_socket_addr_check_len()` refuses any length below
    `sizeof(struct fnet_sockaddr_in)` -- zero included, via its trailing else --
    and both pointers are non-null at that call site, so the accept succeeded or
    failed on whatever the stack slot held, leaving the refused connection
    QUEUED. Every inbound connection this device accepts goes through it.

    PATCHED_SHA256 already notices ANY byte change to this file, but it cannot
    say which change matters: a re-sync that drops the one-liner and refreshes
    the baseline reads as an ordinary vendor bump. This asserts the fix itself,
    inside the accept callback rather than anywhere in the file, plus the patch
    marker that tells the next re-syncer it is deliberate.
    """
    marker = "void EthernetServer::poll(void* cookie)"
    poll_body = function_body(source("NativeEthernetServer.cpp"), marker)
    # The patch comment names fnet_socket_accept() several lines above the call,
    # so the ordering is read off the comment-stripped body. The stripper pads
    # with spaces, so offsets stay comparable between the two.
    code = without_comments_and_literals(poll_body)
    accept = code.find("fnet_socket_accept(")
    assert accept >= 0, "the accept call left EthernetServer::poll()"
    initialiser = code.find("fnet_size_t fromlen = sizeof(from);")
    assert 0 <= initialiser < accept, (
        "EthernetServer::poll() must initialise `fnet_size_t fromlen = "
        "sizeof(from);` before fnet_socket_accept(); an uninitialised fromlen "
        "is upstream's defect 20 and fails accepts against stack remnants"
    )
    assert "&fromlen" in code[accept : accept + 200]
    assert "Aius patch" in poll_body[:initialiser], (
        "the fromlen initialiser must keep its `Aius patch:` comment, so a "
        "vendor re-sync sees a deliberate fix rather than a stray local edit"
    )

    # The guard must see the defect it exists for: strip the initialiser and it
    # has to fail, not pass on some other `sizeof(from)` elsewhere in the file.
    mutated = function_body(
        source("NativeEthernetServer.cpp").replace(
            "fnet_size_t fromlen = sizeof(from);", "fnet_size_t fromlen;", 1
        ),
        marker,
    )
    assert (
        without_comments_and_literals(mutated).find(
            "fnet_size_t fromlen = sizeof(from);"
        )
        == -1
    )


def test_server_and_stack_teardown_are_idempotent_and_bounded():
    server_source = source("NativeEthernetServer.cpp")
    server_begin = function_body(server_source, "void EthernetServer::begin()")
    server_end = function_body(server_source, "void EthernetServer::end()")
    invalidate = function_body(
        server_source, "void EthernetServer::invalidateService()"
    )
    close_listener = function_body(
        server_source, "void EthernetServer::closeListener()"
    )
    stack_end = function_body(source("NativeEthernet.cpp"), "void EthernetClass::end()")

    idempotent_return = server_begin.find(
        "service_descriptor != 0"
    )
    socket_open = server_begin.find("Ethernet.socketBegin(")
    assert 0 <= idempotent_return < socket_open
    assert server_begin.count("Ethernet.socketBegin(") == 1
    assert server_begin.count("fnet_service_register(") == 1
    assert server_end.find("invalidateService()") < server_end.find("closeListener()")
    assert invalidate.count("fnet_service_unregister(") == 1
    assert close_listener.count("Ethernet.socketClose(") == 1
    assert "service_descriptor = 0" in invalidate
    assert "_listener = MAX_SOCK_NUM" in close_listener
    assert not unbounded_loop(server_begin)
    assert not unbounded_loop(server_end)
    assert not unbounded_loop(invalidate)
    assert not unbounded_loop(close_listener)

    assert stack_end.count("dhcpStop(") == 1
    assert stack_end.count("socketClose(") == 1
    assert "i < socket_num" in stack_end
    assert stack_end.find("stack_initialized = false") < stack_end.find(
        "fnet_link_release("
    )
    assert stack_end.find("link_status = false") < stack_end.find(
        "fnet_link_release("
    )
    assert not unbounded_loop(stack_end)


# --- the fake-FNET declaration contract ------------------------------------
#
# Qualifiers that carry no type information and appear on one side only.
DROPPED_QUALIFIERS = frozenset(
    {"extern", "FNET_COMP_PACKED_VAR", "FNET_COMP_PACKED", "register"}
)
# Tokens that are part of a type rather than a parameter name.
TYPE_TOKENS = frozenset(
    {
        "void", "char", "short", "int", "long", "float", "double",
        "unsigned", "signed", "const", "volatile", "struct", "union",
        "enum", "size_t", "ssize_t",
    }
)
DECLARATION_RE = re.compile(
    r"(?P<returns>[A-Za-z_][A-Za-z_0-9 \t*]*?)\s*"
    r"\b(?P<name>fnet_[A-Za-z_0-9]+)\s*"
    r"\(\s*(?P<params>[^();]*?)\s*\)\s*;",
    re.DOTALL,
)


def header_text(path):
    """FNET ships at least one header with a stray non-UTF-8 byte."""
    return path.read_bytes().decode("utf-8", errors="replace")


def tokens_of(text):
    return [
        token
        for token in re.findall(r"[A-Za-z_][A-Za-z_0-9]*|\*", text)
        if token not in DROPPED_QUALIFIERS
    ]


def normalise_type(text):
    return " ".join(tokens_of(text))


def normalise_parameter(text):
    tokens = tokens_of(text)
    if not tokens:
        return "void"
    # Drop a trailing parameter name, but never the type itself.
    if (
        len(tokens) > 1
        and re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", tokens[-1])
        and tokens[-1] not in TYPE_TOKENS
        and any(token not in TYPE_TOKENS for token in tokens[:-1])
    ):
        tokens = tokens[:-1]
    return " ".join(tokens)


def split_parameters(text):
    if not text.strip():
        return ("void",)
    parts = tuple(normalise_parameter(part) for part in text.split(","))
    return ("void",) if parts == ("void",) else parts


def declared_signatures(text):
    """{name: {(return type, (parameter types, ...))}} for every fnet_* function."""
    signatures = {}
    for match in DECLARATION_RE.finditer(without_comments_and_literals(text)):
        returns = normalise_type(match.group("returns"))
        if not returns:
            continue
        signature = (returns, split_parameters(match.group("params")))
        signatures.setdefault(match.group("name"), set()).add(signature)
    return signatures


def fnet_source_root():
    core = os.environ.get("PLATFORMIO_CORE_DIR") or str(Path.home() / ".platformio")
    root = Path(core) / FNET_RELATIVE
    assert root.is_dir(), (
        f"the real FNET headers are not at {root}.\n"
        "      This contract compares test/network-transport/fake/fnet.h against\n"
        "      the dependency it stands for, so it needs the Teensy framework\n"
        "      package. Set PLATFORMIO_CORE_DIR, or install it with\n"
        "      `pio pkg install -g -p teensy`."
    )
    return root


def real_signatures():
    signatures = {}
    for path in sorted(fnet_source_root().rglob("*.h")):
        if "third_party" in path.parts:
            continue
        for name, found in declared_signatures(header_text(path)).items():
            signatures.setdefault(name, set()).update(found)
    return signatures


def fake_signature_mismatches(fake_text, real=None):
    real = real_signatures() if real is None else real
    fake = declared_signatures(fake_text)
    assert len(fake) > 30, f"parsed only {len(fake)} declarations from the fake"
    mismatches = []
    for name, found in sorted(fake.items()):
        assert len(found) == 1, f"{name} is declared more than once in the fake"
        signature = next(iter(found))
        if name not in real:
            mismatches.append(
                f"{name}: the fake declares it, FNET's headers do not."
            )
        elif signature not in real[name]:
            mismatches.append(
                f"{name}:\n"
                f"      fake  {signature[0]} ({', '.join(signature[1])})\n"
                + "".join(
                    f"      FNET  {candidate[0]} ({', '.join(candidate[1])})\n"
                    for candidate in sorted(real[name])
                )
            )
    return mismatches


def test_fake_fnet_declares_the_dependencys_signatures():
    """Every function the fake declares must have FNET's own signature.

    This is the guard the `read_once()` defect did not have. `fake/fnet.h` was
    written from the adapter's call sites, so it published exactly the names the
    adapter referenced, with whatever types made the adapter compile --
    fnet_socket_close() as void, fnet_error_get() as int8_t, and no
    FNET_ERR_CONNCLOSED at all. With no closure code in scope the only
    expressible way to write a "peer closed" test row was `recv_result = 0`,
    which is the POSIX convention rather than FNET's, and the suite then
    confirmed the adapter against a header derived from it. Comparing the fake
    to the dependency closes that loop; the numeric constants and structure
    layouts are pinned by the static_assert block in
    test_nativeethernet_behavior.cpp, which the compiler enforces.
    """
    mismatches = fake_signature_mismatches(FAKE_FNET.read_text())
    assert not mismatches, (
        f"{len(mismatches)} fake-FNET declaration(s) disagree with FNET:\n  - "
        + "\n  - ".join(mismatches)
    )


def test_fake_fnet_contract_discriminates():
    """The guard above is worthless if it cannot see the defect it exists for."""
    real = real_signatures()
    fake_text = FAKE_FNET.read_text()

    regressions = (
        # The original shape: a return type narrowed until the caller compiled.
        (
            "fnet_return_t fnet_socket_close(fnet_socket_t);",
            "void fnet_socket_close(fnet_socket_t);",
            "fnet_socket_close",
        ),
        (
            "fnet_error_t fnet_error_get(void);",
            "int8_t fnet_error_get(void);",
            "fnet_error_get",
        ),
        # A parameter type quietly relaxed to whatever the caller passes.
        (
            "fnet_return_t fnet_socket_listen(fnet_socket_t, fnet_size_t);",
            "fnet_return_t fnet_socket_listen(fnet_socket_t, int);",
            "fnet_socket_listen",
        ),
        # A function invented by the caller and never present in FNET.
        (
            "void fnet_poll(void);",
            "void fnet_poll(void);\nvoid fnet_socket_drain(fnet_socket_t);",
            "fnet_socket_drain",
        ),
    )
    for original, mutation, expected in regressions:
        assert original in fake_text, f"missing declaration to mutate: {original}"
        mismatches = fake_signature_mismatches(
            fake_text.replace(original, mutation, 1), real
        )
        assert any(entry.startswith(f"{expected}:") for entry in mismatches), (
            f"the fake-FNET contract accepted a synthetic regression in "
            f"{expected}: {mismatches}"
        )

    assert not fake_signature_mismatches(fake_text, real)


def test_application_transport_header_guard_has_one_owner():
    violations = repository_transport_include_violations()
    assert set(violations) == {DESIGNATED_INCLUDE_OWNER}, violations
    assert violations[DESIGNATED_INCLUDE_OWNER] == [
        "NativeEthernet.h",
        "NativeEthernetUdp.h",
        "fnet.h",
    ]

    synthetic = '#include "service/dhcp/fnet_dhcp_cln.h"\n'
    assert forbidden_transport_includes(synthetic) == [
        "service/dhcp/fnet_dhcp_cln.h"
    ]

    public_header = (FIRMWARE_ROOT / "src" / "aius_network_teensy.h").read_text()
    assert forbidden_transport_includes(public_header) == []


def main():
    test_provenance_and_snapshot_shape()
    test_upstream_hash_baselines_and_explicit_patch_set()
    test_platformio_selects_repository_snapshot()
    test_target_and_suite_pin_the_same_fnet_cfg_tls()
    test_one_shot_source_guard_and_guard_sensitivity()
    test_begin_and_dhcp_ownership_contract()
    test_client_write_is_capacity_bounded_and_single_attempt()
    test_server_accept_initialises_fromlen()
    test_server_and_stack_teardown_are_idempotent_and_bounded()
    test_application_transport_header_guard_has_one_owner()
    test_fake_fnet_declares_the_dependencys_signatures()
    test_fake_fnet_contract_discriminates()
    print("NativeEthernet source contract: PASS")
    print("fake-FNET declaration contract: PASS")


if __name__ == "__main__":
    main()
