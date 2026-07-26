#!/usr/bin/env bash
# Flash the Ethernet optical image with no Program-button press.
#
# A thin dispatcher over tools/net/aius_cdc_runner.py, which owns the CDC
# control protocol, HalfKay entry, the loader invocation and the post-flash
# identity verification. The logic and its tests live there (test/flash/);
# this script exists so the operator entry point stays where it always was.
#
#   scripts/flash_optical.sh                       # flash the built optical image
#   scripts/flash_optical.sh firmware.hex          # flash a named image
#   scripts/flash_optical.sh --port /dev/ttyACM0 firmware.hex
#   scripts/flash_optical.sh --via rawhid firmware.hex   # legacy RawHID handoff
#
# The CDC route needs group access to the port and the loader. When the login is
# stale, wrap it:
#   sg dialout -c "sg plugdev -c 'scripts/flash_optical.sh'"
#
# Env vars:
#   TEENSY_LOADER  teensy_loader_cli path  (default: ~/.platformio/packages/tool-teensy/teensy_loader_cli)
#
# Exit codes: 0 flashed and verified; 1 the cycle failed; 3 a required tool is
#             absent; 4 usage error.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/.." && pwd)"
runner="$fw_root/tools/net/aius_cdc_runner.py"
loader="${TEENSY_LOADER:-$HOME/.platformio/packages/tool-teensy/teensy_loader_cli}"

via="cdc"
port_args=()
hex=""

while [[ $# -gt 0 ]]; do
	case "$1" in
	-h | --help)
		echo "usage: flash_optical.sh [--port PORT] [--via cdc|rawhid] [firmware.hex]"
		exit 0
		;;
	--port)
		[[ $# -ge 2 ]] || { echo "error: --port needs a value" >&2; exit 4; }
		port_args=(--port "$2")
		shift 2
		;;
	--via)
		[[ $# -ge 2 ]] || { echo "error: --via needs a value" >&2; exit 4; }
		via="$2"
		shift 2
		;;
	-*)
		echo "error: unknown flag $1" >&2
		exit 4
		;;
	*)
		[[ -z "$hex" ]] || { echo "error: one image at a time" >&2; exit 4; }
		hex="$1"
		shift
		;;
	esac
done

[[ -n "$hex" ]] || hex="$fw_root/.pio/build/teensy41-optical/firmware.hex"
[[ -f "$hex" ]] || { echo "error: no image at $hex" >&2; exit 3; }
[[ -x "$loader" ]] || { echo "error: loader not executable: $loader" >&2; exit 3; }

case "$via" in
cdc)
	exec python3 "$runner" "${port_args[@]}" flash "$hex" --loader "$loader"
	;;
rawhid)
	# The pre-CDC handoff, kept because it is the only route on an image whose
	# CDC control path is not present. It cannot verify what it wrote -- that
	# is the whole reason the CDC route is the default.
	echo "==> RawHID handoff (no post-flash verification)"
	hid_runner="$fw_root/scripts/run_hid.sh"
	for path in /dev/hidraw*; do
		[ -e "$path" ] || continue
		props="$(udevadm info --query=property --name="$path" 2>/dev/null || true)"
		case "$(printf '%s\n' "$props" | grep '^DEVPATH=' || true)" in
		*:1.0/*16C0:0486.*)
			echo "==> Requesting software reboot from $path"
			"$hid_runner" "$path" --reboot
			break
			;;
		esac
	done
	timeout 90 "$loader" --mcu=imxrt1062 -w -v "$hex"
	echo "==> Flashed $hex (unverified)"
	;;
*)
	echo "error: --via must be cdc or rawhid" >&2
	exit 4
	;;
esac
