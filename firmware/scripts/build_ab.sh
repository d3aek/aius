#!/usr/bin/env bash
# Build the whole A/B release set (aius-ota-resilience.md Phase 3):
# both slot images, the first-stage chooser, and the merged HalfKay
# recovery image (chooser + slot A).
#
#   scripts/build_ab.sh                # build everything
#   scripts/build_ab.sh --chooser-only # just the chooser + size check
#
# Outputs, under .pio/build/:
#   teensy41-optical-slot-a/firmware.hex   slot A image (linked at 0x60010000)
#   teensy41-optical-slot-b/firmware.hex   slot B image (linked at 0x603E8000)
#   aius-chooser/chooser.hex               the first stage (0x60000000)
#   aius-ab/recovery.hex                   chooser + slot A, for HalfKay
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
if [[ "${1:-}" == "--chooser-only" ]]; then
	chooser_only=1
elif [[ $# -gt 0 ]]; then
	echo "usage: build_ab.sh [--chooser-only]" >&2
	exit 4
fi

slot_a_dir="$fw_root/.pio/build/teensy41-optical-slot-a"
if [[ "$chooser_only" -eq 0 ]]; then
	echo "==> Building slot A image..."
	"$here/build.sh" teensy41-optical-slot-a
	echo "==> Building slot B image..."
	"$here/build.sh" teensy41-optical-slot-b
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

echo "==> Merging the HalfKay recovery image (chooser + slot A)..."
ab_dir="$fw_root/.pio/build/aius-ab"
mkdir -p "$ab_dir"
python3 "$here/merge_hex.py" "$ab_dir/recovery.hex" \
	"$chooser_dir/chooser.hex" \
	"$slot_a_dir/firmware.hex"

echo "==> A/B release set complete:"
echo "    chooser:  $chooser_dir/chooser.hex"
echo "    slot A:   $slot_a_dir/firmware.hex"
echo "    slot B:   $fw_root/.pio/build/teensy41-optical-slot-b/firmware.hex"
echo "    recovery: $ab_dir/recovery.hex"
