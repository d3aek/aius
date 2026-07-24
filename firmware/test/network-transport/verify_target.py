"""PlatformIO post-link proof for the pinned NativeEthernet target library."""

from pathlib import Path
import subprocess

Import("env")


def verify_nativeethernet(source, target, env):
    project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
    expected = (project_dir / "vendor" / "NativeEthernet" / "src").resolve()
    resolved = []
    for builder in env.GetLibBuilders():
        if builder.name == "AiusNativeEthernet":
            resolved.append(Path(str(builder.src_dir)).resolve())
        if builder.name == "NativeEthernet":
            raise RuntimeError(
                f"framework NativeEthernet was not ignored: {builder.src_dir}"
            )
    if resolved != [expected]:
        raise RuntimeError(
            f"NativeEthernet resolved from {resolved}, expected {[expected]}"
        )

    elf = Path(str(target[0])).resolve()
    nm = env.WhereIs("arm-none-eabi-nm")
    if not nm:
        raise RuntimeError("arm-none-eabi-nm is unavailable")
    result = subprocess.run(
        [nm, "-C", str(elf)],
        check=True,
        text=True,
        capture_output=True,
    )
    archive_matches = list(
        Path(env.subst("$BUILD_DIR")).glob("lib*/libNativeEthernet.a")
    )
    if len(archive_matches) != 1:
        raise RuntimeError(
            f"expected one pinned NativeEthernet archive, found {archive_matches}"
        )
    archive_result = subprocess.run(
        [nm, "-C", str(archive_matches[0])],
        check=True,
        text=True,
        capture_output=True,
    )
    archive_required = (
        "EthernetClass::beginNoWait(unsigned char*)",
        "EthernetClass::dhcpStart(unsigned long)",
        "EthernetClass::end()",
        "EthernetClient::write(unsigned char const*, unsigned int)",
        "EthernetServer::end()",
    )
    missing = [
        symbol for symbol in archive_required
        if symbol not in archive_result.stdout
    ]
    if missing:
        raise RuntimeError(f"pinned archive lacks required symbols: {missing}")
    linked_required = tuple(
        symbol for symbol in archive_required
        if symbol != "EthernetClass::end()"
    )
    missing = [symbol for symbol in linked_required if symbol not in result.stdout]
    if missing:
        raise RuntimeError(f"linked firmware lacks pinned symbols: {missing}")
    print("NativeEthernet target resolution and linked symbols: PASS")


if env.subst("$PIOENV") == "teensy41-optical":
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", verify_nativeethernet)
