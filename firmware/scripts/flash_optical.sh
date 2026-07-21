#!/usr/bin/env bash
# Flash the Ethernet optical image through the current RawHID application.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/.." && pwd)"
loader="${TEENSY_LOADER:-$HOME/.platformio/packages/tool-teensy/teensy_loader_cli}"
runner="$fw_root/scripts/run_hid.sh"

if [ "$#" -ne 1 ]; then
	echo "usage: $0 firmware.hex" >&2
	exit 2
fi
[ -x "$loader" ] || { echo "Teensy loader not executable: $loader" >&2; exit 3; }

find_rawhid()
{
	for path in /dev/hidraw*; do
		[ -e "$path" ] || continue
		props="$(udevadm info --query=property --name="$path" 2>/dev/null || true)"
		devpath="$(printf '%s\n' "$props" | sed -n 's/^DEVPATH=//p')"
		case "$devpath" in
			*:1.0/*16C0:0486.*)
				echo "$path"
				return 0
				;;
		esac
	done
	return 1
}

if hidraw="$(find_rawhid)"; then
	echo "==> Requesting software reboot from $hidraw"
	"$runner" "$hidraw" --reboot
fi

echo "==> Waiting for Teensy bootloader and flashing $1"
timeout 90 "$loader" --mcu=imxrt1062 -w -v "$1"
echo "==> Optical image flashed"
