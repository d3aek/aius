#!/usr/bin/env bash
# Flash the P6 image and resolve the re-enumerated RawHID interface by identity.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/.." && pwd)"
loader="${TEENSY_LOADER:-$HOME/.platformio/packages/tool-teensy/teensy_loader_cli}"
runner="$fw_root/scripts/run_hid.sh"

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	echo "usage: $0 firmware.hex vectors-dir [audit-file]" >&2
	exit 2
fi
[ -x "$loader" ] || { echo "Teensy loader not executable: $loader" >&2; exit 3; }

hex="$1"
vectors="$2"
audit="${3:-$fw_root/.build/native/aius-p6.audit}"

echo "==> Waiting for Teensy bootloader and flashing $hex"
timeout 90 "$loader" --mcu=imxrt1062 -w -v "$hex"

find_rawhid()
{
	for path in /dev/hidraw*; do
		[ -e "$path" ] || continue
		props="$(udevadm info --query=property --name="$path" 2>/dev/null || true)"
		case "$props" in
			*ID_VENDOR_ID=16c0*ID_MODEL_ID=0486*ID_USB_INTERFACE_NUM=00*)
				echo "$path"
				return 0
				;;
		esac
		devpath="$(printf '%s\n' "$props" | sed -n 's/^DEVPATH=//p')"
		case "$devpath" in
			*'/16C0:0486.'*':1.0/'*|*'/16C0:0486.'*':1.0/hidraw/'*)
				echo "$path"
				return 0
				;;
		esac
	done
	return 1
}

echo "==> Waiting for application RawHID re-enumeration"
for attempt in $(seq 1 60); do
	if hidraw="$(find_rawhid)"; then
		echo "==> Running P6 proof through $hidraw"
		"$runner" "$hidraw" "$vectors" "$audit"
		exit $?
	fi
	sleep 1
done

echo "Timed out waiting for Teensy application RawHID (16c0:0486)" >&2
exit 4
