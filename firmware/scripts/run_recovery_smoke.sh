#!/usr/bin/env bash
# Aius live recovery smoke -- the reproducible entry point for plan Phase 16a.
#
# Drives tools/net/aius_recovery_smoke.py against one deployed optical board:
# the read-only probe, then the selected gates in one serialized order. The
# stall gate is the only step that needs the build and flash tools, because
# each injectable checkpoint is its own fault image and every image is restored
# to teensy41-optical before the next one is built.
#
#   scripts/run_recovery_smoke.sh --target 192.0.2.66 --dry-run
#   scripts/run_recovery_smoke.sh --target 192.0.2.66 --gates soak
#   scripts/run_recovery_smoke.sh --target 192.0.2.66 --serial /dev/ttyACM0 \
#       --gates reboots,stalls,soak,dsp --allow-mutation
#
# Without --allow-mutation the run is read-only: the reboot cycles, the fault
# images and the DSP config round-trip are reported unresolved rather than
# driven, which is exit code 3 and not a pass.
#
# Env vars:
#   AIUS_SD_AUTH   SD maintenance token for the DSP config PUT  (default: unset;
#                  with --serial the runner reads it from the board instead)
#   PYTHON         host Python interpreter                      (default: python3)
#   TEENSY_LOADER  loader used by scripts/flash_optical.sh      (default: see it)
#
# Exit codes: 0 every selected gate passed; 2 a gate failed; 3 an unresolved
#             gate, an unreachable board, or a missing tool; 4 usage error.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/.." && pwd)"
runner="$fw_root/tools/net/aius_recovery_smoke.py"
python="${PYTHON:-python3}"

target=""
serial=""
gates="reboots,soak,dsp"
checkpoints="0 1 2 3 4 5 6 7 8"
soak_duration="1800"
allow_mutation=0
dry_run=0

print_usage() {
	echo "usage: run_recovery_smoke.sh --target HOST [--serial PATH]"
	echo "       [--gates reboots,stalls,soak,dsp] [--checkpoints '0 1 ... 8']"
	echo "       [--soak-duration SECONDS] [--allow-mutation] [--dry-run]"
	echo "  default gates: $gates; the stall gate builds and flashes fault images."
	echo "  exit codes: 0 ok, 2 a gate failed, 3 unresolved or environmental, 4 usage."
}

usage() {
	print_usage >&2
	exit 4
}

while [ $# -gt 0 ]; do
	case "$1" in
	--target) target="${2:-}"; shift 2 ;;
	--serial) serial="${2:-}"; shift 2 ;;
	--gates) gates="${2:-}"; shift 2 ;;
	--checkpoints) checkpoints="${2:-}"; shift 2 ;;
	--soak-duration) soak_duration="${2:-}"; shift 2 ;;
	--allow-mutation) allow_mutation=1; shift ;;
	--dry-run) dry_run=1; shift ;;
	-h | --help)
		print_usage
		exit 0
		;;
	*) usage ;;
	esac
done

if [ -z "$target" ]; then
	usage
fi
if ! command -v "$python" >/dev/null 2>&1; then
	echo "==> $python is not on PATH; the runner is a host Python script." >&2
	exit 3
fi
if [ ! -f "$runner" ]; then
	echo "==> missing runner: $runner" >&2
	exit 3
fi

serial_args=()
if [ -n "$serial" ]; then
	serial_args=(--serial "$serial")
fi
mutation_args=()
if [ "$allow_mutation" -eq 1 ]; then
	mutation_args=(--allow-mutation)
fi

# 2 (a gate failed) outranks 3 (unresolved), which outranks 0.
worst=0
record() {
	local code="$1"
	if [ "$code" -eq 2 ]; then
		worst=2
		return 0
	fi
	if [ "$code" -ne 0 ] && [ "$worst" -eq 0 ]; then
		worst="$code"
	fi
	return 0
}

echo "==> Plan of work"
"$python" "$runner" plan --target "$target" --gates "$gates" \
	--soak-duration "$soak_duration"
if [ "$dry_run" -eq 1 ]; then
	echo "==> Dry run: nothing was driven."
	exit 0
fi

echo "==> Probe (read-only)"
code=0
"$python" "$runner" probe --target "$target" "${serial_args[@]}" || code=$?
if [ "$code" -ne 0 ]; then
	echo "==> Board did not answer USB, HTTP or UDP at $target." >&2
	echo "    Confirm the deployed image and the link before the smoke." >&2
	exit 3
fi

# The stall gate is driven per fault image below, so it is stripped from the
# single-pass gate list rather than run against the production image.
selected=""
IFS=',' read -r -a requested <<<"$gates"
for gate in "${requested[@]}"; do
	if [ -z "$gate" ] || [ "$gate" = "stalls" ]; then
		continue
	fi
	if [ -z "$selected" ]; then
		selected="$gate"
	else
		selected="$selected,$gate"
	fi
done

if [ -n "$selected" ]; then
	echo "==> Gates $selected"
	code=0
	"$python" "$runner" run --target "$target" --gates "$selected" \
		--soak-duration "$soak_duration" \
		"${serial_args[@]}" "${mutation_args[@]}" || code=$?
	record "$code"
fi

case ",$gates," in
*,stalls,*) ;;
*) exit "$worst" ;;
esac

if [ "$allow_mutation" -ne 1 ]; then
	echo "==> Stall gate skipped: flashing a fault image needs --allow-mutation." >&2
	record 3
	exit "$worst"
fi
if [ ! -x "$fw_root/scripts/build.sh" ] || [ ! -x "$fw_root/scripts/flash_optical.sh" ]; then
	echo "==> The stall gate needs scripts/build.sh and scripts/flash_optical.sh." >&2
	exit 3
fi

# One board and one USB enumeration: each fault image is built, flashed,
# observed and restored before the next checkpoint starts. A failed restore
# stops the run rather than carrying a fault image into the next checkpoint.
IFS=' ' read -r -a checkpoint_list <<<"$checkpoints"
for checkpoint in "${checkpoint_list[@]}"; do
	echo "==> Fault image for checkpoint $checkpoint"
	"$fw_root/scripts/build.sh" teensy41-optical-fault "$checkpoint"
	"$fw_root/scripts/flash_optical.sh" \
		"$fw_root/.pio/build/teensy41-optical-fault/firmware.hex"
	echo "==> Gate stalls, checkpoint $checkpoint"
	code=0
	"$python" "$runner" run --target "$target" --gates stalls \
		--checkpoint "$checkpoint" --allow-mutation \
		"${serial_args[@]}" || code=$?
	record "$code"
	echo "==> Restoring teensy41-optical after checkpoint $checkpoint"
	"$fw_root/scripts/build.sh" teensy41-optical
	"$fw_root/scripts/flash_optical.sh" \
		"$fw_root/.pio/build/teensy41-optical/firmware.hex"
	code=0
	"$python" "$runner" probe --target "$target" "${serial_args[@]}" >/dev/null ||
		code=$?
	if [ "$code" -ne 0 ]; then
		echo "==> Production image did not answer after checkpoint $checkpoint." >&2
		echo "    Restore it before the next checkpoint; the run stops here." >&2
		exit 3
	fi
done

exit "$worst"
