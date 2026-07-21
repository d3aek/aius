#!/usr/bin/env bash
# Aius P6 host proof over Teensy RawHID (/dev/hidrawN).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/.." && pwd)"
cc="${CC:-gcc}"
out="$fw_root/.build/native/aius-hid-runner"

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
	echo "usage: $0 /dev/hidrawN vectors-dir [audit-file]" >&2
	exit 2
fi

mkdir -p "$(dirname "$out")"
inc=("-I$fw_root/lib/aius_codec_api/include" "-I$fw_root/lib/aius_dts/include"
	"-I$fw_root/lib/aius_capability/include" "-I$fw_root/tools/device/include"
	"-I$fw_root/vendor/dcadec-core")
flags=(-std=c11 -Wall -Wextra -Werror -O1 -g -D_GNU_SOURCE
	-Wno-expansion-to-defined -fsanitize=address,undefined
	-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)
vendor="$fw_root/vendor/dcadec-core"

"$cc" "${flags[@]}" "${inc[@]}" \
	"$fw_root/tools/device/src/aius_hid_main.c" "$fw_root/tools/device/src/aius_hid_runner.c" \
	"$fw_root/tools/device/src/aius_host_runner.c" "$fw_root/tools/device/src/aius_sim_device.c" \
	"$fw_root/lib/aius_capability/src/aius_cap_protocol.c" \
	"$fw_root/lib/aius_capability/src/aius_cap_payload.c" \
	"$fw_root/lib/aius_capability/src/aius_cap_device.c" \
	"$fw_root/lib/aius_dts/src/aius_dts.c" "$fw_root/lib/aius_dts/src/aius_alloc_shim.c" \
	"$fw_root/lib/aius_codec_api/src/aius_arena.c" \
	"$vendor/bitstream.c" "$vendor/core_decoder.c" "$vendor/dmix_tables.c" \
	"$vendor/idct_fixed.c" "$vendor/idct_float.c" "$vendor/interpolator.c" \
	"$vendor/interpolator_fixed.c" "$vendor/ta.c" -lm -o "$out"

"$out" "$@"
