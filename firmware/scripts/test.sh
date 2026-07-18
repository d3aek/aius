#!/usr/bin/env bash
# Aius firmware native test runner.
#
# Compiles and runs a host test suite with the portable C code. Foreground-only
# and root-independent: it resolves the firmware root from its own path. The
# Teensy target build is a separate wrapper (added at capability-gate P4).
#
#   scripts/test.sh --environment native --suite <suite>
#
# Suites: arena, codec-api-contract, dts, dts-arena, dts-fuzz, iec61937,
#         iec61937-fuzz, pipeline, dsp.
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

# Shared build inputs for every DTS suite. The adapter and the vendored dcadec
# core allocate through libc malloc/calloc/realloc/free; the aius_alloc_shim
# intercepts those at LINK time via --wrap so the no-heap arena path is provable
# without editing any vendored file. Every DTS suite links the shim + the wrap
# flags: with no arena armed the shim just forwards to __real_* (libc), so the
# differential decode is unaffected.
dts_build() {
	local out="$1"
	shift
	# Include paths: adapter, arena/codec ABI, and the vendored core.
	local dts_inc=("-I$fw_root/lib/aius_dts/include" "-I$inc" \
		"-I$fw_root/vendor/dcadec-core")
	# The vendored dcadec sources use M_PI (needs _GNU_SOURCE under -std=c11) and
	# the AT_LEAST_GCC() feature macro (benign -Wexpansion-to-defined). These
	# accommodations are scoped to the DTS suites so the vendored files stay
	# verbatim. The comma-separated --wrap list is one linker argument.
	# shellcheck disable=SC2054
	local dts_cflags=(-D_GNU_SOURCE -Wno-expansion-to-defined \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)
	local vendor="$fw_root/vendor/dcadec-core"
	"$cc" "${cflags[@]}" "${dts_inc[@]}" "${dts_cflags[@]}" \
		"$@" \
		"$fw_root/lib/aius_dts/src/aius_dts.c" \
		"$fw_root/lib/aius_dts/src/aius_alloc_shim.c" \
		"$src/aius_arena.c" \
		"$vendor/bitstream.c" \
		"$vendor/core_decoder.c" \
		"$vendor/dmix_tables.c" \
		"$vendor/idct_fixed.c" \
		"$vendor/idct_float.c" \
		"$vendor/interpolator.c" \
		"$vendor/interpolator_fixed.c" \
		"$vendor/ta.c" \
		-lm -o "$out"
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
	out="$build_dir/dts"
	dts_build "$out" "$fw_root/test/dts/test_dts.c"
	"$out" "$fw_root/tools/dts_vectors/vectors"
	;;
dts-arena)
	# No-heap arena path + allocation metrics: prove zero steady-state allocations.
	out="$build_dir/dts-arena"
	dts_build "$out" "$fw_root/test/dts/test_dts_arena.c"
	"$out" "$fw_root/tools/dts_vectors/vectors"
	;;
dts-fuzz)
	# Malformed/truncated/fuzz robustness under ASan/UBSan.
	out="$build_dir/dts-fuzz"
	dts_build "$out" "$fw_root/test/dts/fuzz_dts.c"
	"$out" "$fw_root/tools/dts_vectors/vectors" 50000
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
pipeline)
	# S/PDIF pipeline: classifier + IEC 61937 parser chained into the DTS decoder,
	# end-to-end vs a direct raw-AU decode over the committed IEC corpus.
	out="$build_dir/pipeline"
	dts_build "$out" \
		"-I$fw_root/lib/aius_pipeline/include" \
		"-I$fw_root/lib/aius_iec61937/include" \
		"$fw_root/test/pipeline/test_pipeline.c" \
		"$fw_root/lib/aius_pipeline/src/aius_pipeline.c" \
		"$fw_root/lib/aius_iec61937/src/aius_iec61937.c"
	"$out" "$fw_root/tools/iec_corpus/vectors"
	;;
dsp)
	# 5.1->stereo downmix + Direct Form I biquad EQ known-answer tests. -ffp-contract=off
	# keeps the double-precision math bit-identical between host and Cortex-M7 (no FMA).
	dsp_out="$build_dir/dsp"
	"$cc" "${cflags[@]}" -D_GNU_SOURCE -ffp-contract=off \
		"-I$fw_root/lib/aius_dsp/include" \
		"$fw_root/test/dsp/test_dsp.c" \
		"$fw_root/lib/aius_dsp/src/aius_dsp.c" \
		"$src/aius_arena.c" \
		-lm -o "$dsp_out"
	"$dsp_out"
	;;
*)
	echo "unknown suite: $suite" >&2
	exit 2
	;;
esac
