"""Inject deterministic source, target, and fault identity into firmware."""

from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import re
import subprocess


FAULT_ENVIRONMENT = "teensy41-optical-fault"
FAULT_SELECTOR_PATTERN = re.compile(r"[0-9]")


def source_revision(project_dir):
    """Return the twelve-character Git revision used in the image identity."""
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short=12", "HEAD"],
            cwd=project_dir,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def validate_fault_selector(environment, selector):
    """Return the numeric selector, rejecting leaked or malformed selectors."""
    if environment == FAULT_ENVIRONMENT:
        if (
            not isinstance(selector, str)
            or FAULT_SELECTOR_PATTERN.fullmatch(selector) is None
        ):
            raise ValueError(
                f"{FAULT_ENVIRONMENT} requires AIUS_FAULT_CHECKPOINT in 0..9"
            )
        return int(selector)

    if selector is not None:
        raise ValueError(
            "AIUS_FAULT_CHECKPOINT is valid only for "
            f"{FAULT_ENVIRONMENT}"
        )
    return -1


def derive_metadata(project_dir, environment, selector=None, build_time=None):
    """Derive the complete compile-time metadata for one target image."""
    revision = source_revision(project_dir)
    checkpoint = validate_fault_selector(environment, selector)
    # The fault environment's full PlatformIO name makes the USB ID response
    # exceed its protocol-wide 256-byte payload limit once every retained
    # diagnostic is present. The separate AIUS_BUILD_TARGET define still
    # carries the canonical environment; keep the human-facing revision
    # unique but compact enough for the control protocol.
    identity_target = "fault" if environment == FAULT_ENVIRONMENT else environment
    identity = f"{revision}+{identity_target}"
    if checkpoint >= 0:
        identity = f"{identity}.cp{checkpoint}"
    build_id = int.from_bytes(
        hashlib.sha256(identity.encode("utf-8")).digest()[:4],
        byteorder="little",
    )
    if build_time is None:
        build_time = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {
        "source_revision": revision,
        "target": environment,
        "fault_checkpoint": checkpoint,
        "fault_stall_checkpoint": checkpoint if checkpoint >= 0 else None,
        "build_revision": identity,
        "build_id": build_id,
        "build_utc": build_time,
    }


def _run_platformio():
    try:
        Import("env")
    except NameError:
        return

    metadata = derive_metadata(
        Path(env.subst("$PROJECT_DIR")),
        env.subst("$PIOENV"),
        os.environ.get("AIUS_FAULT_CHECKPOINT"),
    )
    defines = [
        (
            "AIUS_BUILD_SOURCE_REV",
            env.StringifyMacro(metadata["source_revision"]),
        ),
        ("AIUS_BUILD_TARGET", env.StringifyMacro(metadata["target"])),
        ("AIUS_BUILD_FAULT_CHECKPOINT", metadata["fault_checkpoint"]),
        (
            "AIUS_BUILD_GIT_REV",
            env.StringifyMacro(metadata["build_revision"]),
        ),
        ("AIUS_BUILD_ID", metadata["build_id"]),
        ("AIUS_BUILD_UTC", env.StringifyMacro(metadata["build_utc"])),
    ]
    if metadata["fault_stall_checkpoint"] is not None:
        defines.append(
            (
                "AIUS_FAULT_STALL_CHECKPOINT",
                metadata["fault_stall_checkpoint"],
            )
        )
    env.Append(CPPDEFINES=defines)


_run_platformio()
