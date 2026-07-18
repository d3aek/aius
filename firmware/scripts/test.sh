#!/usr/bin/env bash
# Aius firmware native test runner.
#
# Compiles and runs a host test suite with the portable C code. Foreground-only
# and root-independent: it resolves the firmware root from its own path. The
# Teensy target build is a separate wrapper (added at capability-gate P4).
#
#   scripts/test.sh --environment native --suite <suite>
#
# Suites: arena, codec-api-contract, dts, iec61937, iec61937-fuzz.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/.." && pwd)"

usage() {
	echo "usage: test.sh --environment native --suite <suite>" >&2
	exit 2
}

environment=""
suite=""
while [ $# -gt 0 ]; do
	case "$1" in
	--environment)
		environment="${2:-}"
		shift 2
		;;
	--suite)
		suite="${2:-}"
		shift 2
		;;
	*)
		usage
		;;
	esac
done

[ "$environment" = "native" ] || usage
[ -n "$suite" ] || usage

cc="${CC:-gcc}"
inc="$fw_root/lib/aius_codec_api/include"
src="$fw_root/lib/aius_codec_api/src"
build_dir="$fw_root/.build/native"
mkdir -p "$build_dir"

# The comma in -fsanitize=address,undefined is part of one gcc flag, not a separator.
# shellcheck disable=SC2054
cflags=(-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -I"$inc")

compile_and_run() {
	local name="$1"
	shift
	local out="$build_dir/$name"
	"$cc" "${cflags[@]}" "$@" -o "$out"
	"$out"
}

case "$suite" in
arena)
	compile_and_run arena "$src/aius_arena.c" "$fw_root/test/arena/test_arena.c"
	;;
codec-api-contract)
	compile_and_run codec-api-contract "$fw_root/test/codec-api/test_codec_api_contract.c"
	;;
dts)
	# DTS Core differential test: vendored dcadec fixed-point core + AiusDtsDecoder
	# adapter vs the FFmpeg reference decode of the committed corpus.
	dts_inc=("-I$fw_root/lib/aius_dts/include" "-I$fw_root/vendor/dcadec-core")
	# The vendored dcadec sources use M_PI (needs _GNU_SOURCE under -std=c11) and
	# the AT_LEAST_GCC() feature macro (benign -Wexpansion-to-defined). These
	# accommodations are scoped to this suite so the vendored files stay verbatim.
	dts_cflags=(-D_GNU_SOURCE -Wno-expansion-to-defined)
	vendor="$fw_root/vendor/dcadec-core"
	out="$build_dir/dts"
	"$cc" "${cflags[@]}" "${dts_inc[@]}" "${dts_cflags[@]}" \
		"$fw_root/test/dts/test_dts.c" \
		"$fw_root/lib/aius_dts/src/aius_dts.c" \
		"$vendor/bitstream.c" \
		"$vendor/core_decoder.c" \
		"$vendor/dmix_tables.c" \
		"$vendor/idct_fixed.c" \
		"$vendor/idct_float.c" \
		"$vendor/interpolator.c" \
		"$vendor/interpolator_fixed.c" \
		"$vendor/ta.c" \
		-lm -o "$out"
	"$out" "$fw_root/tools/dts_vectors/vectors"
	;;
iec61937)
	# IEC 61937 burst parser known-answer test against the committed corpus.
	iec_out="$build_dir/iec61937"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_iec61937/include" \
		"$fw_root/test/iec61937/test_iec61937.c" \
		"$fw_root/lib/aius_iec61937/src/aius_iec61937.c" \
		-o "$iec_out"
	"$iec_out" "$fw_root/tools/iec_corpus/vectors"
	;;
iec61937-fuzz)
	# IEC 61937 parser fuzz: random/adversarial streams under ASan/UBSan.
	fuzz_out="$build_dir/iec61937-fuzz"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_iec61937/include" \
		"$fw_root/test/iec61937/fuzz_iec61937.c" \
		"$fw_root/lib/aius_iec61937/src/aius_iec61937.c" \
		-o "$fuzz_out"
	"$fuzz_out" 200000
	;;
*)
	echo "unknown suite: $suite" >&2
	exit 2
	;;
esac
