#!/usr/bin/env python3
"""Source-contract tests for the repository-pinned NativeEthernet transport."""

import re
import hashlib
import json
import subprocess
from pathlib import Path, PurePosixPath


FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
VENDOR_ROOT = FIRMWARE_ROOT / "vendor" / "NativeEthernet"
SOURCE_ROOT = VENDOR_ROOT / "src"

TEMPORARY_INCLUDE_EXCEPTIONS = {
    PurePosixPath("src/optical_main.cpp"),
}
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
    assert values["lib_deps"] == ["symlink://vendor/NativeEthernet"]
    assert "NativeEthernet" in values["lib_ignore"]


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


def test_application_transport_header_guard_and_temporary_exception():
    violations = repository_transport_include_violations()
    assert set(violations) == (
        TEMPORARY_INCLUDE_EXCEPTIONS | {DESIGNATED_INCLUDE_OWNER}
    ), violations
    assert violations[DESIGNATED_INCLUDE_OWNER] == [
        "NativeEthernet.h",
        "NativeEthernetUdp.h",
        "fnet.h",
    ]
    assert violations[PurePosixPath("src/optical_main.cpp")] == [
        "NativeEthernet.h",
        "NativeEthernetUdp.h",
        "service/dhcp/fnet_dhcp_cln.h",
        "stack/fnet_netif.h",
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
    test_one_shot_source_guard_and_guard_sensitivity()
    test_begin_and_dhcp_ownership_contract()
    test_client_write_is_capacity_bounded_and_single_attempt()
    test_server_and_stack_teardown_are_idempotent_and_bounded()
    test_application_transport_header_guard_and_temporary_exception()
    print("NativeEthernet source contract: PASS")


if __name__ == "__main__":
    main()
