#!/usr/bin/env bash
# Aius firmware Teensy 4.1 cross-build + per-bank memory report (capability-gate P4).
#
# Cross-compiles a PlatformIO environment for the Teensy 4.1 (ARM Cortex-M7) to a
# firmware.elf + firmware.hex and, on success, prints the per-bank memory report
# (FLASH / ITCM / DTCM / OCRAM). Foreground-only and root-independent: it resolves
# the firmware root from its own path.
#
#   scripts/build.sh                              # builds teensy41-release (default)
#   scripts/build.sh teensy41-instrument          # builds a named env
#   scripts/build.sh teensy41-optical-fault 0     # builds fault checkpoint 0
#   PIO=/path/to/venv/bin/pio scripts/build.sh
#
# PlatformIO Core is NOT vendored. Provision it once (pinned) into a venv, e.g.
#   python3 -m venv --without-pip <venv> && \
#     PYTHONPATH=/usr/lib/python3/dist-packages <venv>/bin/python -m pip \
#       install --ignore-installed pip && <venv>/bin/pip install platformio==6.1.16
# then point PIO at <venv>/bin/pio. The first build downloads the teensy platform
# and the arm-none-eabi toolchain into PLATFORMIO_CORE_DIR.
#
# Env vars:
#   PIO                  PlatformIO Core executable            (default: pio on PATH)
#   PLATFORMIO_CORE_DIR  platform + toolchain cache            (default: ~/.platformio)
#
# Exit codes: 0 ok; 1 build failed; 2 a memory bank is over capacity;
#             3 required build tool not found; 4 usage error;
#             75 optical build lock contention.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/.." && pwd)"

env_name="teensy41-release"
fault_checkpoint=""
case "${1:-}" in
-h | --help)
	if [[ $# -ne 1 ]]; then
		echo "error: --help does not accept arguments" >&2
		exit 4
	fi
	echo "usage: build.sh [ENV]"
	echo "       build.sh teensy41-optical-fault CHECKPOINT"
	echo "  default ENV: teensy41-release"
	echo "  CHECKPOINT: one digit from 0 through 9"
	echo "  cross-builds a Teensy 4.1 PlatformIO env and prints the per-bank memory report"
	echo "  env: PIO=<pio exe> PLATFORMIO_CORE_DIR=<cache dir>"
	exit 0
	;;
"") ;;
-*)
	echo "error: unknown flag $1" >&2
	exit 4
	;;
*)
	env_name="$1"
	;;
esac

if [[ ${AIUS_FAULT_CHECKPOINT+x} ]]; then
	echo "error: AIUS_FAULT_CHECKPOINT is build-script controlled; pass the checkpoint as an argument" >&2
	exit 4
fi

if [[ "$env_name" == "teensy41-optical-fault" ]]; then
	if [[ $# -ne 2 || ! ${2:-} =~ ^[0-9]$ ]]; then
		echo "error: teensy41-optical-fault requires one checkpoint digit from 0 through 9" >&2
		exit 4
	fi
	fault_checkpoint="$2"
elif [[ $# -ne 0 && $# -ne 1 ]]; then
	echo "error: a checkpoint selector is valid only for teensy41-optical-fault" >&2
	exit 4
fi

pio="${PIO:-pio}"
if ! command -v "$pio" >/dev/null 2>&1; then
	echo "==> PlatformIO Core not found (PIO='$pio')." >&2
	echo "    Provision it per this script's header, then re-run with PIO set." >&2
	exit 3
fi

core_dir="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"

mkdir -p "$fw_root/.pio"
build_log="$fw_root/.pio/build-$env_name.log"
# Serialize optical builds so two agents sharing this tree cannot clobber the
# same .pio/build/<env> output. Build hygiene, not an evidence contract.
if [[ "$env_name" == "teensy41-optical" || "$env_name" == "teensy41-optical-fault" ]]; then
	if ! command -v flock >/dev/null 2>&1; then
		echo "==> flock not found; cannot serialize optical builds." >&2
		exit 3
	fi
	exec {optical_build_lock_fd}>"$fw_root/.pio/optical-build.lock"
	if ! flock -n "$optical_build_lock_fd"; then
		echo "==> Another production or fault optical build holds the build lock." >&2
		exit 75
	fi
fi

echo "==> Building $env_name for Teensy 4.1 (PlatformIO Core $("$pio" --version 2>/dev/null | awk '{print $NF}'))"
set +e
if [[ -n "$fault_checkpoint" ]]; then
	AIUS_FAULT_CHECKPOINT="$fault_checkpoint" "$pio" run -e "$env_name" -d "$fw_root" 2>&1 | tee "$build_log"
else
	"$pio" run -e "$env_name" -d "$fw_root" 2>&1 | tee "$build_log"
fi
rc=${PIPESTATUS[0]}
set -e
if [[ $rc -ne 0 ]]; then
	echo "==> Build FAILED for $env_name (see output above)." >&2
	exit 1
fi

# Report the toolchain version alongside the pins, for reproducibility.
tc_bin="$core_dir/packages/toolchain-gccarmnoneeabi-teensy/bin"
if [[ -x "$tc_bin/arm-none-eabi-gcc" ]]; then
	echo "==> Toolchain: $("$tc_bin/arm-none-eabi-gcc" --version | head -1)"
fi

# The decoder's static arena is an uninitialised .bss symbol (g_arena_memory),
# so its size comes straight from the ELF; feed it to the report for the DTCM
# annotation. (The pattern read `g_arena_mem$` for a while and silently never
# matched -- the annotation was absent, not zero.)
elf="$fw_root/.pio/build/$env_name/firmware.elf"
report_args=(--size-log "$build_log")
nm="$tc_bin/arm-none-eabi-nm"
if [[ -x "$nm" && -f "$elf" ]]; then
	# g_arena_memory is a class-static in a .cpp, so nm shows it name-mangled;
	# -C demangles it back to the qualified name the awk pattern matches.
	# No early `exit` in the awk: under pipefail it would SIGPIPE nm (141)
	# the moment the pattern actually matches.
	arena_hex="$("$nm" -SC "$elf" | awk '/::g_arena_memory$/ && !found { print $2; found = 1 }')"
	if [[ -n "$arena_hex" ]]; then
		report_args+=(--arena-bytes "$((16#$arena_hex))")
	fi
fi

echo
python3 "$fw_root/tools/memory/mem_report.py" "${report_args[@]}"
