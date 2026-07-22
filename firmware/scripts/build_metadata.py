"""Inject source revision and UTC build time into firmware binaries."""

from datetime import datetime, timezone
from pathlib import Path
import subprocess


Import("env")

project_dir = Path(env.subst("$PROJECT_DIR"))
try:
    revision = subprocess.check_output(
        ["git", "rev-parse", "--short=12", "HEAD"],
        cwd=project_dir,
        text=True,
    ).strip()
except (OSError, subprocess.CalledProcessError):
    revision = "unknown"

build_time = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
env.Append(
    CPPDEFINES=[
        ("AIUS_BUILD_GIT_REV", env.StringifyMacro(revision)),
        ("AIUS_BUILD_UTC", env.StringifyMacro(build_time)),
    ]
)
