#!/usr/bin/env bash
# Build the A/B release set (aius-ota-slot-agnostic.md Phase 5): the one
# canonical image (the slot-A env, linked at AIUS_AB_CANONICAL_BASE), the
# first-stage chooser, and the merged HalfKay recovery image (chooser +
# canonical image). The canonical image is what every OTA release signs,
# WITHOUT --slot; the chooser remaps it into whichever physical slot it lands
# in. The slot-B-linked image is the retired per-slot chooser's second build
# (aius-ota-resilience.md Phase 3); it is no longer in the default set and
# builds only on request, until Phase 7 retires the env itself.
#
#   scripts/build_ab.sh                # canonical image + chooser + recovery
#   scripts/build_ab.sh --with-slot-b  # also the retired slot-B-linked image
#   scripts/build_ab.sh --chooser-only # just the chooser + size check
#
# Outputs, under .pio/build/:
#   teensy41-optical-slot-a/firmware.hex   canonical image (0x60010000)
#   aius-chooser/chooser.hex               the first stage (0x60000000)
#   aius-ab/recovery.hex                   chooser + canonical image, HalfKay
#   teensy41-optical-slot-b/firmware.hex   --with-slot-b only (0x603E8000)
#
# The chooser is a freestanding image built directly with the platform
# toolchain: no framework, no libc, just chooser/chooser.c + lib/aius_ab.
# Its FCB is EXTRACTED from the slot-A build's own bootdata object, so the
# ROM boots the chooser with byte-identical FlexSPI configuration to every
# image this board has ever run.
#
# Env vars:
#   PLATFORMIO_CORE_DIR  toolchain cache            (default: ~/.platformio)
#
# Exit codes: 0 ok; 1 a build or merge failed; 3 a required tool is absent.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/.." && pwd)"

toolchain="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/toolchain-gccarmnoneeabi-teensy/bin"
gcc="$toolchain/arm-none-eabi-gcc"
objcopy="$toolchain/arm-none-eabi-objcopy"
size_tool="$toolchain/arm-none-eabi-size"
if [[ ! -x "$gcc" || ! -x "$objcopy" ]]; then
	echo "==> build_ab needs the PlatformIO teensy toolchain at $toolchain" >&2
	exit 3
fi

chooser_only=0
with_slot_b=0
for argument in "$@"; do
	case "$argument" in
	--chooser-only)
		chooser_only=1
		;;
	--with-slot-b)
		with_slot_b=1
		;;
	*)
		echo "usage: build_ab.sh [--chooser-only] [--with-slot-b]" >&2
		exit 4
		;;
	esac
done
if [[ "$chooser_only" -eq 1 && "$with_slot_b" -eq 1 ]]; then
	echo "usage: --chooser-only and --with-slot-b are exclusive" >&2
	exit 4
fi

slot_a_dir="$fw_root/.pio/build/teensy41-optical-slot-a"
if [[ "$chooser_only" -eq 0 ]]; then
	echo "==> Building the canonical image (slot-A env)..."
	"$here/build.sh" teensy41-optical-slot-a
	if [[ "$with_slot_b" -eq 1 ]]; then
		echo "==> Building the retired slot-B-linked image (--with-slot-b)..."
		"$here/build.sh" teensy41-optical-slot-b
	fi
fi

bootdata_obj="$slot_a_dir/FrameworkArduino/bootdata.c.o"
if [[ ! -f "$bootdata_obj" ]]; then
	echo "==> build_ab needs the slot-A build for the FCB extraction." >&2
	echo "    Not found: $bootdata_obj (run without --chooser-only first)" >&2
	exit 3
fi

echo "==> Building the first-stage chooser..."
chooser_dir="$fw_root/.pio/build/aius-chooser"
mkdir -p "$chooser_dir"

# The FCB, byte-for-byte from the application build's own bootdata object.
"$objcopy" -O binary --only-section=.flashconfig "$bootdata_obj" \
	"$chooser_dir/fcb.bin"
fcb_bytes=$(stat -c %s "$chooser_dir/fcb.bin")
if [[ "$fcb_bytes" -ne 512 ]]; then
	echo "==> FCB extraction produced $fcb_bytes bytes, expected 512." >&2
	exit 1
fi
"$objcopy" -I binary -O elf32-littlearm -B arm \
	--rename-section .data=.flashconfig,alloc,load,readonly,data,contents \
	"$chooser_dir/fcb.bin" "$chooser_dir/fcb.o"

# SOFT float, deliberately: the ROM hands over with the FPU disabled and the
# chooser must not own enabling it -- the slot image's startup does. Integer
# code only, but the ABI flag is what makes that a compiler guarantee.
chooser_cflags=(-mcpu=cortex-m7 -mthumb -mfloat-abi=soft
	-Os -g -ffreestanding -fno-builtin -nostdlib
	-Wall -Wextra -Werror
	"-I$fw_root/lib/aius_ab/include")
"$gcc" "${chooser_cflags[@]}" -c "$fw_root/chooser/chooser.c" \
	-o "$chooser_dir/chooser.o"
"$gcc" "${chooser_cflags[@]}" -c "$fw_root/lib/aius_ab/src/aius_ab.c" \
	-o "$chooser_dir/aius_ab.o"
"$gcc" -mcpu=cortex-m7 -mthumb -nostdlib -Wl,--gc-sections \
	-T "$fw_root/chooser/chooser.ld" \
	"$chooser_dir/chooser.o" "$chooser_dir/aius_ab.o" "$chooser_dir/fcb.o" \
	-o "$chooser_dir/chooser.elf"
"$objcopy" -O ihex "$chooser_dir/chooser.elf" "$chooser_dir/chooser.hex"
"$size_tool" "$chooser_dir/chooser.elf"

if [[ "$chooser_only" -eq 1 ]]; then
	echo "==> Chooser built: $chooser_dir/chooser.hex"
	exit 0
fi

echo "==> Merging the HalfKay recovery image (chooser + canonical image)..."
ab_dir="$fw_root/.pio/build/aius-ab"
mkdir -p "$ab_dir"
python3 "$here/merge_hex.py" "$ab_dir/recovery.hex" \
	"$chooser_dir/chooser.hex" \
	"$slot_a_dir/firmware.hex"

echo "==> A/B release set complete:"
echo "    chooser:   $chooser_dir/chooser.hex"
echo "    canonical: $slot_a_dir/firmware.hex  (sign WITHOUT --slot)"
if [[ "$with_slot_b" -eq 1 ]]; then
	echo "    slot B:    $fw_root/.pio/build/teensy41-optical-slot-b/firmware.hex  (retired per-slot chooser only)"
fi
echo "    recovery:  $ab_dir/recovery.hex"
