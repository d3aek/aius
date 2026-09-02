#!/usr/bin/env bash
# Aius firmware native test runner.
#
# Compiles and runs a host test suite with the portable C code. Foreground-only
# and root-independent: it resolves the firmware root from its own path. The
# Teensy target build is a separate wrapper (added at capability-gate P4).
#
#   scripts/test.sh --environment native --suite <suite>
#
# Suites: ab, arena, audio-ingress, audio-service, auth, clock, clock-target, codec-api-contract, config, dsp-config, json, dts, dts-arena,
#         dts-fuzz, dts-lfe, a52, a52-fuzz, eac3, eac3-fuzz, iec61937, iec61937-fuzz, capability-protocol,
#         capability, pipeline, dsp, dsp-vectors, downmix-identity, measure, net, net-runner,
#         rate-runner, rate-gate-capture, latency-runner,
#         recovery-runner, flash, http,
#         usb-control, replay, replay-samples, tx-gate, tx-governor, input-liveness, recorder, boot, boot-target, fault-target, ota,
#         build-metadata, runtime, network-transport, sd, sd-target, optical-app,
#         optical-cutover, runtime-target, memory, itcm-placement, fake-fidelity,
#         registry.
#
# That list is not documentation: the `registry` suite fails when it, the suites
# table in README.md, and the arms of the `case` below disagree with the
# directories under test/.
#
# Every suite is host-only and self-contained except `itcm-placement`, which
# inspects the linked objects of an optical image and therefore needs a current
# cross-build; it exits 3 with the build command to run when one is absent.
#
# Env vars:
#   CC, CXX              host compilers                        (default: gcc, g++)
#   AIUS_ITCM_ENV        image `itcm-placement` inspects       (default: teensy41-optical)
#   PLATFORMIO_CORE_DIR  toolchain cache holding readelf       (default: ~/.platformio)
#   AIUS_A52_BOUND_PROBE AC-3 host reader instrumentation       (default: 1; 0 disables)
#
# Exit codes: 0 ok; 2 usage error or unknown suite; 3 a suite's prerequisite
#             artifact or tool is absent; any other non-zero is the failing
#             suite's own status.
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
cxx="${CXX:-g++}"
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

# Compile the AC-3 adapter and each fork translation unit to its own object.
# Keeping these inputs out of a shared pipeline/parser compilation command
# prevents any fork-local accommodation from weakening sanitizer or warning
# coverage on those shared sources. The fork currently needs no sanitizer or
# warning carve-out: _GNU_SOURCE supplies M_PI and the common strict sanitized
# flags apply to every object.
a52_compile_objects() {
	local out_prefix="$1"
	local bound_probe="$2"
	local a52_inc=("-I$fw_root/lib/aius_a52/include" \
		"-I$fw_root/lib/aius_dts/include" "-I$inc" \
		"-I$fw_root/lib/aius_a52/a52")
	local a52_cflags=(-D_GNU_SOURCE -fno-sanitize-recover=undefined)
	if [ "$bound_probe" = "1" ]; then
		a52_cflags+=(-DAIUS_A52_BOUND_PROBE)
	fi
	local a52="$fw_root/lib/aius_a52/a52"
	a52_objects=(
		"$out_prefix-aius-a52.o"
		"$out_prefix-a52-parse.o"
		"$out_prefix-a52-bit-allocate.o"
		"$out_prefix-a52-bitstream.o"
		"$out_prefix-a52-imdct.o"
	)
	"$cc" "${cflags[@]}" "${a52_inc[@]}" "${a52_cflags[@]}" \
		-c "$fw_root/lib/aius_a52/src/aius_a52.c" \
		-o "${a52_objects[0]}"
	"$cc" "${cflags[@]}" "${a52_inc[@]}" "${a52_cflags[@]}" \
		-c "$a52/parse.c" -o "${a52_objects[1]}"
	"$cc" "${cflags[@]}" "${a52_inc[@]}" "${a52_cflags[@]}" \
		-c "$a52/bit_allocate.c" -o "${a52_objects[2]}"
	"$cc" "${cflags[@]}" "${a52_inc[@]}" "${a52_cflags[@]}" \
		-c "$a52/bitstream.c" -o "${a52_objects[3]}"
	"$cc" "${cflags[@]}" "${a52_inc[@]}" "${a52_cflags[@]}" \
		-c "$a52/imdct.c" -o "${a52_objects[4]}"
}

# Shared build inputs for the AC-3 suites. Mirrors dts_build: the adapter and
# the forked liba52 allocate through libc malloc/free, intercepted at LINK
# time by aius_alloc_shim via --wrap so the no-heap arena path is provable
# in both.
a52_build() {
	local out="$1"
	shift
	local a52_inc=("-I$fw_root/lib/aius_a52/include" \
		"-I$fw_root/lib/aius_dts/include" "-I$inc" \
		"-I$fw_root/lib/aius_a52/a52")
	local a52_test_cflags=(-D_GNU_SOURCE -fno-sanitize-recover=undefined)
	local bound_probe=0
	if [ "${AIUS_A52_BOUND_PROBE:-1}" != "0" ]; then
		a52_test_cflags+=(-DAIUS_A52_BOUND_PROBE)
		bound_probe=1
	fi
	a52_compile_objects "$out" "$bound_probe"
	# The comma-separated --wrap list is one linker argument.
	# shellcheck disable=SC2054
	a52_test_cflags+=(-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)
	"$cc" "${cflags[@]}" "${a52_inc[@]}" "${a52_test_cflags[@]}" \
		"$@" \
		"$fw_root/lib/aius_dts/src/aius_alloc_shim.c" \
		"$src/aius_arena.c" \
		"${a52_objects[@]}" \
		-lm -o "$out"
}

# Shared build inputs for the E-AC-3 suite: the aius_eac3 adapter over the
# vendored FFmpeg fork (vendor/ffmpeg-eac3), allocator-wrapped like the DTS
# and AC-3 suites so the no-heap arena path is provable. The fork compiles
# under the same strict sanitized flags as first-party code with three
# accommodations scoped here so the vendored files stay verbatim-plus-tagged:
# _GNU_SOURCE (M_PI under -std=c11), HAVE_AV_CONFIG_H (FFmpeg's own
# internal-build switch selecting config.h/internal.h), and the -Wno-* set
# below (style nits -Wall/-Wextra raises in verbatim upstream sources).
eac3_build() {
	local out="$1"
	shift
	local v="$fw_root/vendor/ffmpeg-eac3"
	local eac3_inc=("-I$fw_root/lib/aius_eac3/include" \
		"-I$fw_root/lib/aius_dts/include" "-I$inc" \
		"-I$v/shim" "-I$v" "-I$v/libavcodec")
	# The comma-separated --wrap list is one linker argument.
	# shellcheck disable=SC2054
	# -Wno-unused-function: ac3dec.h carries static prototypes for the
	# fork's internal eac3dec.c/ac3dec.c split; the adapter includes it for
	# the context type and would otherwise trip on the undefined statics.
	# --gc-sections drops the two upstream corners nothing here references
	# (ac3_parser.c's AVCodecParser registration struct and mathematics.c's
	# av_add_stable) instead of stubbing their framework externs.
	local eac3_cflags=(-D_GNU_SOURCE -DHAVE_AV_CONFIG_H \
		-fno-sanitize-recover=undefined \
		-ffunction-sections -fdata-sections -Wl,--gc-sections \
		-Wno-unused-parameter -Wno-sign-compare \
		-Wno-implicit-fallthrough -Wno-pointer-sign -Wno-parentheses \
		-Wno-unused-function \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)
	"$cc" "${cflags[@]}" "${eac3_inc[@]}" "${eac3_cflags[@]}" \
		"$@" \
		"$fw_root/lib/aius_eac3/src/aius_eac3.c" \
		"$fw_root/lib/aius_dts/src/aius_alloc_shim.c" \
		"$src/aius_arena.c" \
		"$v/shim/aius_avshim.c" \
		"$v/libavcodec/aius_eac3_core.c" \
		"$v/libavcodec/ac3.c" \
		"$v/libavcodec/ac3tab.c" \
		"$v/libavcodec/ac3dec_data.c" \
		"$v/libavcodec/eac3_data.c" \
		"$v/libavcodec/ac3dsp.c" \
		"$v/libavcodec/ac3_parser.c" \
		"$v/libavcodec/ac3_channel_layout_tab.c" \
		"$v/libavcodec/kbdwin.c" \
		"$v/libavcodec/fmtconvert.c" \
		"$v/libavcodec/bswapdsp.c" \
		"$v/libavutil/float_dsp.c" \
		"$v/libavutil/crc.c" \
		"$v/libavutil/lfg.c" \
		"$v/libavutil/tx.c" \
		"$v/libavutil/tx_float.c" \
		"$v/libavutil/mem.c" \
		"$v/libavutil/mathematics.c" \
		"$v/libavutil/md5.c" \
		-lm -o "$out"
}

# Pipeline-bearing suites link both resident decoders. A52 stays in dedicated
# objects while dts_build retains the established dcadec build and allocator
# wrapping.
pipeline_build() {
	local out="$1"
	shift
	a52_compile_objects "$out" 0
	dts_build "$out" \
		"-I$fw_root/lib/aius_a52/include" \
		"$@" "${a52_objects[@]}"
}

# Compile the E-AC-3 adapter + FFmpeg-fork translation units to dedicated
# objects (the a52_compile_objects shape): the fork's accommodation flags stay
# scoped to these objects so they never weaken sanitizer or warning coverage
# on the shared pipeline/parser/test sources. Only the `pipeline` suite links
# these (with -DAIUS_PIPELINE_EAC3=1); pipeline_build itself stays untouched,
# so audio-service/downmix-identity/latency keep the flag-off target shape.
# NOTE: linking these objects requires -Wl,--gc-sections on the WHOLE binary
# (a link flag cannot be scoped): it is what drops ac3_parser.c's AVCodecParser
# registration struct and mathematics.c's av_add_stable instead of stubbing
# their framework externs, exactly as eac3_build does.
eac3_compile_objects() {
	local out_prefix="$1"
	local v="$fw_root/vendor/ffmpeg-eac3"
	local eac3_inc=("-I$fw_root/lib/aius_eac3/include" \
		"-I$fw_root/lib/aius_dts/include" "-I$inc" \
		"-I$v/shim" "-I$v" "-I$v/libavcodec")
	local eac3_ocflags=(-D_GNU_SOURCE -DHAVE_AV_CONFIG_H \
		-fno-sanitize-recover=undefined \
		-ffunction-sections -fdata-sections \
		-Wno-unused-parameter -Wno-sign-compare \
		-Wno-implicit-fallthrough -Wno-pointer-sign -Wno-parentheses \
		-Wno-unused-function)
	local sources=(
		"$fw_root/lib/aius_eac3/src/aius_eac3.c"
		"$v/shim/aius_avshim.c"
		"$v/libavcodec/aius_eac3_core.c"
		"$v/libavcodec/ac3.c"
		"$v/libavcodec/ac3tab.c"
		"$v/libavcodec/ac3dec_data.c"
		"$v/libavcodec/eac3_data.c"
		"$v/libavcodec/ac3dsp.c"
		"$v/libavcodec/ac3_parser.c"
		"$v/libavcodec/ac3_channel_layout_tab.c"
		"$v/libavcodec/kbdwin.c"
		"$v/libavcodec/fmtconvert.c"
		"$v/libavcodec/bswapdsp.c"
		"$v/libavutil/float_dsp.c"
		"$v/libavutil/crc.c"
		"$v/libavutil/lfg.c"
		"$v/libavutil/tx.c"
		"$v/libavutil/tx_float.c"
		"$v/libavutil/mem.c"
		"$v/libavutil/mathematics.c"
		"$v/libavutil/md5.c"
	)
	eac3_objects=()
	local source
	for source in "${sources[@]}"; do
		local object
		object="$out_prefix-eac3-$(basename "$source" .c).o"
		"$cc" "${cflags[@]}" "${eac3_inc[@]}" "${eac3_ocflags[@]}" \
			-c "$source" -o "$object"
		eac3_objects+=("$object")
	done
}

case "$suite" in
config)
	# Two halves of "configuration": the build-time optical DSP header generator,
	# and the runtime feature-flag parser in lib/aius_config that optical_main.cpp
	# runs over /aius/config.json at boot, on hot-reinsert, and on an HTTP body.
	python3 "$fw_root/test/config/test_build_config.py"
	# The runtime parser is bounded and heap-free by contract, so the test links
	# --wrap over the libc allocators and asserts zero allocations per parse. Same
	# LINK-time interception the DTS suites use, here with no arena and no
	# vendored core, so the wrap list is spelled out rather than via dts_build.
	config_out="$build_dir/config"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_config/include" \
		"-I$fw_root/lib/aius_json/include" \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free \
		"$fw_root/test/config/test_config.c" \
		"$fw_root/lib/aius_config/src/aius_config.c" \
		"$fw_root/lib/aius_json/src/aius_json.c" \
		-o "$config_out"
	"$config_out"
	;;
json)
	# The shared JSON cursor the three bounded parsers depend on: lib/aius_config,
	# lib/aius_dsp_config, and the replay body reader in src/aius_optical_app.cpp
	# each carried its own copy until they were hoisted here. Allocation-free and
	# non-recursive over untrusted input, so the test links --wrap over the libc
	# allocators and asserts zero allocations, exactly as the config suite does.
	json_out="$build_dir/json"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_json/include" \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free \
		"$fw_root/test/json/test_json.c" \
		"$fw_root/lib/aius_json/src/aius_json.c" \
		-o "$json_out"
	"$json_out"
	;;
dsp-config)
	# Bounded, allocation-free PUT /api/dsp/config validator: parses the DSP model
	# JSON wire shape against the same bounds build_config.py's validate_config()
	# enforces. -lm for floor()/isfinite()/pow() (JSON number decode + integer
	# and finite-range checks) -- no strtod(): see aius_dsp_config.c for why.
	dsp_config_out="$build_dir/dsp-config"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_dsp_config/include" \
		"-I$fw_root/lib/aius_json/include" \
		"$fw_root/test/dsp-config/test_dsp_config.c" \
		"$fw_root/lib/aius_dsp_config/src/aius_dsp_config.c" \
		"$fw_root/lib/aius_json/src/aius_json.c" \
		-lm -o "$dsp_config_out"
	"$dsp_config_out"
	# The SD editor builds its PUT body by naming each key explicitly rather than
	# round-tripping the GET, so a key added to the validator and not to the editor
	# is a 400 on every save with nothing failing. That shipped once
	# (matrix.lfe_crossover_hz); this compares the two key sets so it cannot again.
	python3 "$fw_root/test/dsp-config/test_editor_keys.py" "$fw_root"
	;;
arena)
	compile_and_run arena "$src/aius_arena.c" "$fw_root/test/arena/test_arena.c"
	;;
auth)
	# The SD maintenance token's derivation. test_auth.c pins the firmware side;
	# tools/aius_token.py recomputes the same token on the host from a board's
	# fused MAC words, so both are run together -- a change to either that did
	# not change the other would silently invalidate every token already
	# recorded off-device, which is the failure this suite exists to catch.
	compile_and_run auth "$fw_root/lib/aius_auth/src/aius_auth.c" \
		"$fw_root/test/auth/test_auth.c" \
		"-I$fw_root/lib/aius_auth/include"
	python3 "$fw_root/tools/aius_token.py" --self-test
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
dts-lfe)
	# DTS Core LFE bit-exact test: AiusDtsDecoder vs a full dcadec core decode
	# (committed reference PCM; running the suite needs no dcadec build).
	out="$build_dir/dts-lfe"
	dts_build "$out" "$fw_root/test/dts-lfe/test_dts_lfe.c"
	"$out" "$fw_root/tools/dts_vectors/vectors" "$fw_root/tools/dcadec_ref"
	;;
a52)
	# AC-3 decode differential vs FFmpeg (committed corpus) + channel identity
	# + arena/metrics contract.
	out="$build_dir/a52"
	a52_build "$out" "$fw_root/test/a52/test_a52.c"
	UBSAN_OPTIONS=halt_on_error=1 "$out" "$fw_root/tools/ac3_vectors/vectors"
	;;
a52-fuzz)
	# AC-3 adapter robustness under ASan/UBSan: deterministic seam hunt +
	# random corruption, every decode under the defect-31 termination oracle.
	out="$build_dir/a52-fuzz"
	a52_build "$out" "$fw_root/test/a52/fuzz_a52.c"
	UBSAN_OPTIONS=halt_on_error=1 \
		"$out" "$fw_root/tools/ac3_vectors/vectors" 50000
	;;
eac3)
	# E-AC-3 decode differential vs FFmpeg (committed corpus) + regression
	# hash + arena/no-alloc steady state + malformed family.
	out="$build_dir/eac3"
	eac3_build "$out" "$fw_root/test/eac3/test_eac3.c"
	UBSAN_OPTIONS=halt_on_error=1 "$out" "$fw_root/tools/eac3_vectors/vectors"
	;;
eac3-fuzz)
	# E-AC-3 adapter robustness under ASan/UBSan: deterministic seam hunt +
	# random corruption, every decode under the defect-31 termination oracle.
	out="$build_dir/eac3-fuzz"
	eac3_build "$out" "$fw_root/test/eac3/fuzz_eac3.c"
	UBSAN_OPTIONS=halt_on_error=1 \
		"$out" "$fw_root/tools/eac3_vectors/vectors" 20000
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
capability-protocol)
	# USB capability wire protocol: CRC-32, framing/resync, golden vectors (no decoder).
	out="$build_dir/capability-protocol"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_capability/include" \
		"$fw_root/test/capability/test_capability_protocol.c" \
		"$fw_root/lib/aius_capability/src/aius_cap_protocol.c" \
		"$fw_root/lib/aius_capability/src/aius_cap_payload.c" \
		-o "$out"
	"$out" "$fw_root/test/capability/vectors"
	;;
capability)
	# USB capability round-trip: sim device + host runner decoding the corpus.
	out="$build_dir/capability"
	inc_cap=("-I$fw_root/lib/aius_capability/include" "-I$fw_root/tools/device/include")
	dts_build "$out" "${inc_cap[@]}" \
		"$fw_root/test/capability/test_capability.c" \
		"$fw_root/tools/device/src/aius_sim_device.c" \
		"$fw_root/tools/device/src/aius_host_runner.c" \
		"$fw_root/lib/aius_capability/src/aius_cap_protocol.c" \
		"$fw_root/lib/aius_capability/src/aius_cap_payload.c" \
		"$fw_root/lib/aius_capability/src/aius_cap_device.c"
	"$out" "$fw_root/tools/dts_vectors/vectors"
	;;
audio-ingress)
	audio_ingress_out="$build_dir/audio-ingress"
	"$cc" "${cflags[@]}" \
		"-I$fw_root/lib/aius_audio_ingress/include" \
		-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free \
		"$fw_root/test/audio-ingress/test_audio_ingress.c" \
		"$fw_root/lib/aius_audio_ingress/src/aius_audio_ingress.c" \
		-o "$audio_ingress_out"
	"$audio_ingress_out"
	;;
audio-service)
	# The real portable controller over the real ingress and deferred pipeline.
	out="$build_dir/audio-service"
	pipeline_build "$out" \
		"-I$fw_root/lib/aius_audio_ingress/include" \
		"-I$fw_root/lib/aius_audio_service/include" \
		"-I$fw_root/lib/aius_pipeline/include" \
		"-I$fw_root/lib/aius_iec61937/include" \
		"$fw_root/test/audio-service/test_audio_service.c" \
		"$fw_root/lib/aius_audio_ingress/src/aius_audio_ingress.c" \
		"$fw_root/lib/aius_audio_service/src/aius_audio_service.c" \
		"$fw_root/lib/aius_pipeline/src/aius_pipeline.c" \
		"$fw_root/lib/aius_iec61937/src/aius_iec61937.c"
	"$out" "$fw_root/tools/iec_corpus/vectors"
	;;
clock)
	compile_and_run clock \
		"-I$fw_root/lib/aius_clock/include" \
		"$fw_root/test/clock/test_clock.c" \
		"$fw_root/lib/aius_clock/src/aius_clock.c"
	;;
clock-target)
	clock_target_out="$build_dir/clock-target"
	"$cxx" -std=c++17 -Wall -Wextra -Werror -O1 -g \
		-fsanitize=address,undefined -DAIUS_AUDIO_CLOCK_FAKE_TARGET \
		"-I$fw_root/lib/aius_audio/include" \
		"-I$fw_root/lib/aius_clock/include" \
		"$fw_root/test/clock-target/test_clock_target.cpp" \
		"$fw_root/lib/aius_audio/src/aius_audio_clock.cpp" \
		"$fw_root/lib/aius_audio/src/aius_spdif_sources.cpp" \
		-o "$clock_target_out"
	"$clock_target_out"
	;;
pipeline)
	# S/PDIF pipeline: classifier + IEC 61937 parser chained into all three
	# decoders, end-to-end over the committed IEC corpus. This is the ONE
	# flag-on (-DAIUS_PIPELINE_EAC3=1) pipeline build; the other
	# pipeline-bearing suites keep the target's flag-off shape. --gc-sections
	# is required by the fork objects (see eac3_compile_objects) and applies
	# to the whole binary.
	out="$build_dir/pipeline"
	eac3_compile_objects "$out"
	pipeline_build "$out" \
		-DAIUS_PIPELINE_TEST_HOOKS \
		-DAIUS_PIPELINE_EAC3=1 \
		-Wl,--gc-sections \
		"-I$fw_root/lib/aius_pipeline/include" \
		"-I$fw_root/lib/aius_iec61937/include" \
		"-I$fw_root/lib/aius_eac3/include" \
		"$fw_root/test/pipeline/test_pipeline.c" \
		"$fw_root/lib/aius_pipeline/src/aius_pipeline.c" \
		"$fw_root/lib/aius_iec61937/src/aius_iec61937.c" \
		"${eac3_objects[@]}"
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
	# The model-to-runtime adapter is the one coefficient/bypass/state-reset
	# authority shared by firmware and the native latency renderer.
	dsp_runtime_config_out="$build_dir/dsp-runtime-config"
	"$cc" "${cflags[@]}" -D_GNU_SOURCE -ffp-contract=off \
		"-I$fw_root/lib/aius_dsp/include" \
		"-I$fw_root/lib/aius_dsp_config/include" \
		"-I$fw_root/lib/aius_dsp_runtime_config/include" \
		"$fw_root/test/dsp/test_dsp_runtime_config.c" \
		"$fw_root/lib/aius_dsp_runtime_config/src/aius_dsp_runtime_config.c" \
		"$fw_root/lib/aius_dsp/src/aius_dsp.c" \
		"$src/aius_arena.c" \
		-lm -o "$dsp_runtime_config_out"
	"$dsp_runtime_config_out"
	;;
dsp-vectors)
	# Decode each committed DTS vector through the portable DTS interface, then
	# drive the decoded PCM through the PUBLIC DSP interface (downmix + biquad EQ)
	# and check routing/gain/latency known answers. -ffp-contract=off keeps the
	# double-precision downmix/EQ bit-identical to the Cortex-M7 target (as the
	# dsp suite). The dts_build machinery links the vendored dcadec core + adapter
	# + --wrap alloc shim; the extra flags/sources add the DSP module.
	out="$build_dir/dsp-vectors"
	dts_build "$out" -ffp-contract=off \
		"-I$fw_root/lib/aius_dsp/include" \
		"$fw_root/test/dsp/test_dsp_vectors.c" \
		"$fw_root/lib/aius_dsp/src/aius_dsp.c"
	"$out" "$fw_root/tools/dts_vectors/vectors" "$fw_root/tools/dts_vectors/golden"
	;;
downmix-identity)
	# The COMPOSED half of defect 30's chase: one real per-channel tone
	# programme per codec, through the real decoder and the real downmix, read
	# at the two stereo outputs. `dts`/`a52` convict the decode side only and
	# `dsp-vectors` the matrix side only, on synthetic isolated blocks; neither
	# composes, and the hardware capture that raised the defect was a statement
	# about the composition. Both resident decoders, so pipeline_build; the DSP
	# module and liba52's syncinfo header are the extra inputs.
	# -ffp-contract=off keeps the downmix bit-identical to the Cortex-M7, as the
	# other dsp suites.
	out="$build_dir/downmix-identity"
	pipeline_build "$out" -ffp-contract=off \
		"-I$fw_root/lib/aius_dsp/include" \
		"-I$fw_root/lib/aius_a52/a52" \
		"$fw_root/test/dsp/test_downmix_identity.c" \
		"$fw_root/lib/aius_dsp/src/aius_dsp.c"
	"$out" "$fw_root/tools/dts_vectors/vectors" \
		"$fw_root/tools/ac3_vectors/vectors"
	;;
measure)
	# The measurement loopback's portable core (lib/aius_dsp/src/aius_measure.c):
	# capture state machine + sliced sine-fit/Goertzel analysis known answers.
	# -ffp-contract=off for the same host/target bit-identity the dsp suites pin;
	# aius_dsp.c is not linked -- the module uses aius_dsp.h for the sample scale.
	measure_out="$build_dir/measure"
	"$cc" "${cflags[@]}" -D_GNU_SOURCE -ffp-contract=off \
		"-I$fw_root/lib/aius_dsp/include" \
		"$fw_root/test/measure/test_measure.c" \
		"$fw_root/lib/aius_dsp/src/aius_measure.c" \
		-lm -o "$measure_out"
	"$measure_out"
	# The committed PCM stimulus vectors carry two generation-time obligations
	# (no IEC 61937 preamble by coincidence, per-vector loop coherence); this
	# re-asserts them against the committed bytes, plus tone content/residual.
	python3 "$fw_root/tools/measure_vectors/measure_vectors.py" validate
	;;
net)
	# UDP codec plus the pure generation/deadline network supervisor oracle.
	net_out="$build_dir/net"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_net/include" \
		"-I$fw_root/lib/aius_boot/include" \
		"$fw_root/test/net/test_net.c" \
		"$fw_root/lib/aius_net/src/aius_net.c" \
		-o "$net_out"
	"$net_out"
	supervisor_out="$build_dir/net-supervisor"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_net/include" \
		"-I$fw_root/lib/aius_boot/include" \
		"$fw_root/test/net/test_net_supervisor.c" \
		"$fw_root/lib/aius_net/src/aius_net.c" \
		-o "$supervisor_out"
	"$supervisor_out"
	;;
net-runner)
	python3 "$fw_root/test/net/test_net_runner.py"
	python3 "$fw_root/test/net/test_aius_http_runner.py"
	;;
rate-runner)
	python3 "$fw_root/test/rate-runner/test_rate_bench.py"
	# The recorder pull tool's slot selection, exit codes and header sidecar
	# (aius-recorder-rolling-buffer.md Phase C): pure functions, no board.
	python3 "$fw_root/test/rate-runner/test_recorder_pull.py"
	;;
rate-gate-capture)
	# The host-side optical capture reducers that produce the
	# --external-observation JSON gates 2, 4 and 7 read: the rate model, the
	# capture<->status time origins, the amplitude-derived discontinuity
	# detector at the documented +-410 stimulus, the fiducial-based
	# old_rate_output_frames, and the capture-health faults. Synthetic
	# waveforms only -- no board, no ALSA device.
	python3 "$fw_root/test/rate-gate-capture/test_capture.py"
	;;
latency-runner)
	# Exact-carrier native reference: public IEC parser -> resident production
	# pipeline/decoders -> shared runtime-config adapter -> public DSP.
	latency_renderer_out="$build_dir/latency-native-renderer"
	pipeline_build "$latency_renderer_out" -ffp-contract=off \
		"-I$fw_root/lib/aius_pipeline/include" \
		"-I$fw_root/lib/aius_iec61937/include" \
		"-I$fw_root/lib/aius_dsp/include" \
		"-I$fw_root/lib/aius_dsp_config/include" \
		"-I$fw_root/lib/aius_dsp_runtime_config/include" \
		"-I$fw_root/lib/aius_json/include" \
		"$fw_root/tools/latency/native_renderer.c" \
		"$fw_root/lib/aius_pipeline/src/aius_pipeline.c" \
		"$fw_root/lib/aius_iec61937/src/aius_iec61937.c" \
		"$fw_root/lib/aius_dsp/src/aius_dsp.c" \
		"$fw_root/lib/aius_dsp_config/src/aius_dsp_config.c" \
		"$fw_root/lib/aius_dsp_runtime_config/src/aius_dsp_runtime_config.c" \
		"$fw_root/lib/aius_json/src/aius_json.c"
	AIUS_LATENCY_RENDERER="$latency_renderer_out" \
		python3 "$fw_root/test/latency-runner/test_latency_runner.py"
	;;
flash)
	# The button-free flash path, driven against a scripted CDC transport and
	# synthesised Intel HEX images: bootloader entry judged by the port going
	# away rather than by an acknowledgement, the image-identity oracle, and
	# every fail-closed arm (unreadable image, failing loader, stale revision,
	# a board that never re-enumerates). No board and no serial port.
	python3 "$fw_root/test/flash/test_cdc_runner.py"
	python3 "$fw_root/test/flash/test_flash_optical.py"
	;;
recovery-runner)
	# The live recovery smoke's host runner, driven against a scripted device on
	# a virtual clock: every gate on all three result paths (pass, fail, and the
	# unresolved/unreachable one), plus its decode of the recovery status fields.
	# No board, no socket, no serial port -- the 30-minute soak gate runs here in
	# milliseconds because the runner takes its clock from the device link.
	python3 "$fw_root/test/recovery-runner/test_recovery_smoke.py"
	;;
http)
	http_out="$build_dir/http"
	http_inc=("-I$fw_root/lib/aius_http/include" \
		"-I$fw_root/lib/aius_net/include" \
		"-I$fw_root/lib/aius_sd/include" \
		"-I$fw_root/lib/aius_config/include")
	"$cc" "${cflags[@]}" "${http_inc[@]}" \
		"$fw_root/test/http/test_http.c" \
		"$fw_root/lib/aius_http/src/aius_http.c" \
		-o "$http_out"
	"$http_out"
	http_transaction_out="$build_dir/http-transaction"
	"$cc" "${cflags[@]}" "${http_inc[@]}" \
		"$fw_root/test/http/test_http_transaction.c" \
		"$fw_root/lib/aius_http/src/aius_http.c" \
		"$fw_root/lib/aius_http/src/aius_http_transaction.c" \
		"$fw_root/lib/aius_sd/src/aius_sd.c" \
		-o "$http_transaction_out"
	"$http_transaction_out"
	# The home-page emitter proves the extracted generator on the host: it must
	# produce a non-empty document whose skeleton and dynamic fields render.
	http_home_out="$build_dir/http-home-page"
	"$cxx" -std=c++17 -Wall -Wextra -Werror -O1 -g \
		-fsanitize=address,undefined \
		"$fw_root/test/http/emit_home_page.cpp" \
		"$fw_root/src/http_home_page.cpp" \
		-o "$http_home_out"
	http_home_doc="$build_dir/http-home-page.html"
	"$http_home_out" > "$http_home_doc"
	grep -q "<!doctype html>" "$http_home_doc"
	grep -q "test-revision" "$http_home_doc"
	grep -q "getElementById('dsp')" "$http_home_doc"
	echo "http home page: emitted $(wc -c < "$http_home_doc") bytes"
	;;
usb-control)
	usb_out="$build_dir/usb-control"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_usb_control/include" \
		"-I$fw_root/lib/aius_net/include" \
		"$fw_root/test/usb-control/test_usb_control.c" \
		"$fw_root/lib/aius_usb_control/src/aius_usb_control.c" \
		-o "$usb_out"
	"$usb_out"
	;;
replay)
	replay_out="$build_dir/replay"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_pipeline/include" \
		"-I$fw_root/lib/aius_dts/include" \
		"-I$fw_root/lib/aius_iec61937/include" \
		"-I$fw_root/lib/aius_replay/include" \
		"$fw_root/test/replay/test_replay.c" \
		"$fw_root/lib/aius_replay/src/aius_replay.c" \
		-o "$replay_out"
	"$replay_out"
	;;
replay-samples)
	replay_samples_generated="$build_dir/replay-samples-generated"
	OUT="$replay_samples_generated" DURATION=1 \
		"$fw_root/tools/replay_samples/generate.sh"
	python3 "$fw_root/tools/replay_samples/validate.py" \
		"$replay_samples_generated"
	python3 "$fw_root/test/replay-samples/test_validate.py" \
		"$fw_root/tools/replay_samples/validate.py" \
		"$replay_samples_generated" \
		"$build_dir/replay-samples-negative"
	replay_samples_out="$build_dir/replay-samples"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_iec61937/include" \
		"$fw_root/test/replay-samples/test_replay_samples.c" \
		"$fw_root/lib/aius_iec61937/src/aius_iec61937.c" \
		-o "$replay_samples_out"
	"$replay_samples_out" \
		"$replay_samples_generated/channel_id_51.ac3" \
		"$replay_samples_generated/channel_id_51_ac3.iec"
	"$replay_samples_out" \
		"$replay_samples_generated/channel_impulse_51.ac3" \
		"$replay_samples_generated/channel_impulse_51_ac3.iec"
	"$replay_samples_out" \
		"$replay_samples_generated/cinematic_program_51.ac3" \
		"$replay_samples_generated/cinematic_program_51_ac3.iec"
	;;
tx-gate)
	# Header-only predicate: the test compiles against the replay lib's
	# include set (aius_replay.h pulls codec + pipeline types) but links no
	# implementation -- the whole policy is the inline in aius_tx_gate.h.
	tx_gate_out="$build_dir/tx-gate"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_pipeline/include" \
		"-I$fw_root/lib/aius_dts/include" \
		"-I$fw_root/lib/aius_iec61937/include" \
		"-I$fw_root/lib/aius_replay/include" \
		"$fw_root/test/tx-gate/test_tx_gate.c" \
		-o "$tx_gate_out"
	"$tx_gate_out"
	;;
tx-governor)
	# Header-only decision rule: the latency governor's window-minimum floor
	# estimator and trim decision (aius-output-jitter Phase E). Pure stdint
	# header, no implementation linked.
	tx_governor_out="$build_dir/tx-governor"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_audio/src" \
		"$fw_root/test/tx-governor/test_tx_governor.c" \
		-o "$tx_governor_out"
	"$tx_governor_out"
	;;
input-liveness)
	# Header-only decision rule: why nothing is reaching the transmit sink and
	# for how long, over the gate-closed region the production-gap watermark
	# discards by design (aius-input-recovery Phase 1). Pure stdint header, no
	# implementation linked.
	input_liveness_out="$build_dir/input-liveness"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_audio/src" \
		"$fw_root/test/input-liveness/test_input_liveness.c" \
		-o "$input_liveness_out"
	"$input_liveness_out"
	;;
recorder)
	# The flight recorder's portable core (state machine, triggers, staging
	# drain, chunk arithmetic; -lm for the one cold powf in set_config), plus
	# the contract's firmware-wide extmem-heap ban as a source guard.
	recorder_out="$build_dir/recorder"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_recorder/include" \
		"-I$fw_root/lib/aius_codec_api/include" \
		"$fw_root/test/recorder/test_recorder.c" \
		"$fw_root/lib/aius_recorder/src/aius_recorder.c" \
		-lm -o "$recorder_out"
	"$recorder_out"
	python3 "$fw_root/test/recorder/test_no_extmem_heap.py" "$fw_root"
	# The rolling multi-slot bank wrapper (aius-recorder-rolling-buffer.md
	# Phase B): auto-advance on freeze, recycle counting, the read-lease
	# deferral, park() reaching every slot, the config clamp. Links the
	# same portable core unmodified.
	recorder_bank_out="$build_dir/recorder-bank"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_recorder/include" \
		"-I$fw_root/lib/aius_codec_api/include" \
		"$fw_root/test/recorder/test_recorder_bank.c" \
		"$fw_root/lib/aius_recorder/src/aius_recorder_bank.c" \
		"$fw_root/lib/aius_recorder/src/aius_recorder.c" \
		-lm -o "$recorder_bank_out"
	"$recorder_bank_out"
	;;
ab)
	# The A/B boot state (lib/aius_ab): flash map arithmetic, the boot-record
	# and trial-ledger codecs, the ping-pong pick, the chooser decision rule
	# and the slot IVT sanity rule -- the logic the first-stage chooser, the
	# slot application and the tooling all share verbatim.
	ab_out="$build_dir/ab"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_ab/include" \
		"$fw_root/test/ab/test_ab.c" \
		"$fw_root/lib/aius_ab/src/aius_ab.c" \
		-o "$ab_out"
	"$ab_out"
	;;
boot)
	boot_out="$build_dir/boot"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_boot/include" \
		"$fw_root/test/boot/test_boot.c" \
		"$fw_root/lib/aius_boot/src/aius_boot.c" \
		-o "$boot_out"
	"$boot_out"
	;;
boot-target)
	python3 "$fw_root/test/boot-target/test_source_guard.py" \
		"$fw_root/src/aius_boot_teensy.cpp"
	python3 "$fw_root/test/boot-target/test_verify_target.py"
	# The optical envs link through our own copy of the framework's
	# imxrt1062_t41.ld (board_build.ldscript) so the retained OCRAM region can be
	# carved out ahead of .bss.dma. Prove the copy is still the framework body
	# plus exactly the Aius additions. Exit 3 = framework package absent; the
	# native suite must stay runnable without the Teensy toolchain installed.
	ldscript_drift_status=0
	python3 "$fw_root/test/boot-target/test_ldscript_drift.py" \
		"$fw_root/link/aius_teensy41.ld" || ldscript_drift_status=$?
	if [[ $ldscript_drift_status -ne 0 && $ldscript_drift_status -ne 3 ]]; then
		exit "$ldscript_drift_status"
	fi
	boot_target_core="$build_dir/boot-target-aius-boot.o"
	"$cc" "${cflags[@]}" -Wpedantic \
		"-I$fw_root/lib/aius_boot/include" \
		-c "$fw_root/lib/aius_boot/src/aius_boot.c" \
		-o "$boot_target_core"
	boot_target_out="$build_dir/boot-target"
	"$cxx" -std=c++17 -Wall -Wextra -Werror -Wpedantic -O1 -g \
		-fsanitize=address,undefined \
		"-I$fw_root/test/boot-target/fake" \
		"-I$fw_root/lib/aius_boot/include" \
		"-I$fw_root/src" \
		"$fw_root/test/boot-target/test_aius_boot_teensy.cpp" \
		"$fw_root/src/aius_boot_teensy.cpp" \
		"$boot_target_core" \
		-o "$boot_target_out"
	"$boot_target_out"
	;;
fault-target)
	python3 "$fw_root/test/fault-target/test_source_guard.py"
	python3 "$fw_root/test/fault-target/test_fault_target.py"
	;;
build-metadata)
	python3 "$fw_root/test/build-metadata/test_build_metadata.py"
	;;
runtime)
	runtime_out="$build_dir/runtime"
	"$cc" "${cflags[@]}" \
		"-I$fw_root/lib/aius_boot/include" \
		"-I$fw_root/lib/aius_runtime/include" \
		"$fw_root/test/runtime/test_runtime.c" \
		"$fw_root/lib/aius_runtime/src/aius_runtime.c" \
		-o "$runtime_out"
	"$runtime_out"
	;;
network-transport)
	python3 "$fw_root/test/network-transport/test_vendor_source.py"
	# FNET polls from an IntervalTimer, not from the epoch loop, so its NVIC
	# priority is an audio-path contract: at the default 128 it preempts the
	# DSP's software interrupt at 208, a thousand times a second.
	python3 "$fw_root/test/network-transport/test_poll_priority.py"
	transport_out="$build_dir/network-transport"
	"$cxx" -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g \
		-fsanitize=address,undefined -ffunction-sections -fdata-sections \
		-Wl,--gc-sections -DFNET_CFG_TLS=0 \
		"-I$fw_root/test/network-transport/fake" \
		"-I$fw_root/vendor/NativeEthernet/src" \
		"-I$fw_root/lib/aius_net/include" \
		"-I$fw_root/lib/aius_boot/include" \
		"-I$fw_root/src" \
		"$fw_root/test/network-transport/test_nativeethernet_behavior.cpp" \
		"$fw_root/src/aius_network_teensy.cpp" \
		"$fw_root/vendor/NativeEthernet/src/NativeEthernet.cpp" \
		"$fw_root/vendor/NativeEthernet/src/NativeEthernetClient.cpp" \
		"$fw_root/vendor/NativeEthernet/src/NativeEthernetServer.cpp" \
		"$fw_root/vendor/NativeEthernet/src/NativeEthernetUdp.cpp" \
		"$fw_root/vendor/NativeEthernet/src/Nativesocket.cpp" \
		-o "$transport_out"
	"$transport_out" fnet-init-failure
	"$transport_out" netif-init-failure
	"$transport_out" link-init-failure
	"$transport_out" lifecycle
	"$transport_out" adapter-io
	adapter_out="$build_dir/network-teensy-adapter"
	"$cxx" -std=c++17 -Wall -Wextra -Werror -O1 -g \
		-fsanitize=address,undefined -fno-sanitize=enum \
		-DAIUS_NETWORK_TEENSY_TESTING \
		"-I$fw_root/lib/aius_net/include" \
		"-I$fw_root/src" \
		"$fw_root/test/network-transport/test_network_teensy.cpp" \
		"$fw_root/src/aius_network_teensy.cpp" \
		-o "$adapter_out"
	"$adapter_out"
	;;
sd)
	sd_out="$build_dir/sd"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_sd/include" \
		"-I$fw_root/lib/aius_config/include" \
		"$fw_root/test/sd/test_aius_sd.c" \
		"$fw_root/lib/aius_sd/src/aius_sd.c" \
		-o "$sd_out"
	"$sd_out"
	;;
optical-cutover)
	# Structural regression suite for the Phase 14 runtime cutover: reads
	# src/optical_main.cpp as text and asserts both what the post-cutover file
	# must have (construction-only setup, one-epoch loop, one watchdog feed) and
	# what it must not contain (static fallback, raw FNET, rebind globals,
	# blocking socket writes, a second watchdog feed). Both groups run.
	python3 "$fw_root/test/optical-cutover/test_optical_cutover.py"
	;;
optical-app)
	# Optical application dispatch: one bounded decision per HTTP route and USB
	# command against a fake application. The dispatcher is a target .cpp (it
	# carries FLASHMEM on every definition), so the C11 oracle and the real
	# parser compile with gcc and link through g++.
	optical_app_inc=("-I$fw_root/lib/aius_http/include" \
		"-I$fw_root/lib/aius_net/include" \
		"-I$fw_root/lib/aius_sd/include" \
		"-I$fw_root/lib/aius_usb_control/include" \
		"-I$fw_root/lib/aius_json/include" \
		"-I$fw_root/src")
	optical_app_oracle="$build_dir/optical-app-oracle.o"
	optical_app_parser="$build_dir/optical-app-http.o"
	optical_app_json="$build_dir/optical-app-json.o"
	"$cc" "${cflags[@]}" -Wpedantic "${optical_app_inc[@]}" \
		-c "$fw_root/test/optical-app/test_optical_app.c" \
		-o "$optical_app_oracle"
	"$cc" "${cflags[@]}" -Wpedantic "${optical_app_inc[@]}" \
		-c "$fw_root/lib/aius_http/src/aius_http.c" \
		-o "$optical_app_parser"
	"$cc" "${cflags[@]}" -Wpedantic "${optical_app_inc[@]}" \
		-c "$fw_root/lib/aius_json/src/aius_json.c" \
		-o "$optical_app_json"
	optical_app_out="$build_dir/optical-app"
	"$cxx" -std=c++17 -Wall -Wextra -Werror -Wpedantic -O1 -g \
		-fsanitize=address,undefined \
		"${optical_app_inc[@]}" \
		"$fw_root/src/aius_optical_app.cpp" \
		"$optical_app_oracle" \
		"$optical_app_parser" \
		"$optical_app_json" \
		-o "$optical_app_out"
	"$optical_app_out"
	python3 "$fw_root/test/optical-app/test_bitstream_ui.py"
	;;
ota)
	# Portable OTA core: the image container's field validation, the staged
	# upgrade state machine (strict APPEND_PART-shaped chunk contract,
	# incremental SHA-256, injected signature seam), the power-loss record
	# and the slot-independent image-health rules. Pure logic: no flash, no
	# transport, no firmware wiring, so it is host-only and self-contained.
	ota_out="$build_dir/ota"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_ota/include" \
		"$fw_root/test/ota/test_ota.c" \
		"$fw_root/lib/aius_ota/src/aius_ota.c" \
		"$fw_root/lib/aius_ota/src/aius_ota_staging.c" \
		"$fw_root/lib/aius_ota/src/aius_sha256.c" \
		-o "$ota_out"
	"$ota_out"
	# The activation sequencer over a fake flash device: the erase/program
	# ordering (FCB erased first, reprogrammed last), the verify-then-head
	# rule, power loss truncated at every step boundary, each primitive's
	# fault path and the bounds the shipped driver does not have. Its own
	# binary because it drives a different seam -- injected flash/staging
	# primitives -- than the container and state-machine suite above.
	ota_activate_out="$build_dir/ota-activate"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_ota/include" \
		"$fw_root/test/ota/test_ota_activate.c" \
		"$fw_root/lib/aius_ota/src/aius_ota.c" \
		"$fw_root/lib/aius_ota/src/aius_ota_activate.c" \
		"$fw_root/lib/aius_ota/src/aius_ota_dark_stamp.c" \
		"$fw_root/lib/aius_ota/src/aius_sha256.c" \
		-o "$ota_activate_out"
	"$ota_activate_out"
	# The Ed25519 verifier adapter behind that seam, over vendored TweetNaCl.
	# The vendored file is compiled to its OWN object so its two
	# accommodations stay scoped to it and the adapter, the OTA core and the
	# suite keep full strictness -- the same shape the dts and a52 arms use:
	#   -Wno-sign-compare      its FOR(i,n) macro compares a signed counter
	#                          against an unsigned bound throughout.
	#   -fno-sanitize=shift-base  its carry propagation left-shifts negative
	#                          i64 intermediates (tweetnacl.c:281, :685),
	#                          which is UB by the letter of C11 and exactly
	#                          what the algorithm's radix-2^16 representation
	#                          does on every two's-complement target. The
	#                          file is verbatim (vendor/tweetnacl/NOTICE.md),
	#                          so the carve-out lives here rather than in it,
	#                          and it is the narrowest UBSan check that
	#                          covers it -- every other check still applies.
	ota_tweetnacl="$build_dir/ota-tweetnacl.o"
	"$cc" "${cflags[@]}" -Wno-sign-compare -fno-sanitize=shift-base \
		"-I$fw_root/vendor/tweetnacl" \
		-c "$fw_root/vendor/tweetnacl/tweetnacl.c" \
		-o "$ota_tweetnacl"
	ota_ed25519_out="$build_dir/ota-ed25519"
	"$cc" "${cflags[@]}" "-I$fw_root/lib/aius_ota/include" \
		"-I$fw_root/vendor/tweetnacl" \
		"$fw_root/test/ota/test_ota_ed25519.c" \
		"$fw_root/lib/aius_ota/src/aius_ota.c" \
		"$fw_root/lib/aius_ota/src/aius_sha256.c" \
		"$fw_root/lib/aius_ota/src/aius_ota_ed25519.c" \
		"$ota_tweetnacl" \
		-o "$ota_ed25519_out"
	# The committed TEST-ONLY keypair lives beside the suite; the argument
	# is the directory holding it, so the C suite, the host signer and the
	# round-trip test below all sign with the same bytes.
	"$ota_ed25519_out" "$fw_root/test/ota"
	# The host signer end to end: keygen, sign, and verify by driving the
	# firmware's own offer/absorb/verify/arm sequence over the container.
	# It builds its own native helpers on demand (tools/ota/*.c), so it is
	# runnable by an operator without this script.
	python3 "$fw_root/test/ota/test_ota_signer.py"
	# The operator flash client (tools/ota/aius_ota_flash.py) against a
	# state-machine fake: resumable chunk faults (landed and not), the
	# verify -> arm order, and the --activate gate.
	python3 "$fw_root/test/ota/test_ota_flash_client.py"
	;;
runtime-target)
	# Composed Teensy runtime: the composition root, the three bounded target
	# services and the CDC adapter, driven through the real portable engines
	# against fake network/SD/CDC/clock/checkpoint/watchdog primitives.
	#
	# The portable engines compile as strict C11 objects; the target .cpp
	# files and the oracle compile as C++17 and link through g++. The fake
	# include dir must come first so <Arduino.h> and <SD.h> resolve to the
	# host stand-ins, and aius_network_teensy.cpp needs its TESTING define so
	# the primitive table is injectable instead of NativeEthernet-bound.
	runtime_target_inc=("-I$fw_root/test/runtime-target/fake" \
		"-I$fw_root/lib/aius_boot/include" \
		"-I$fw_root/lib/aius_net/include" \
		"-I$fw_root/lib/aius_http/include" \
		"-I$fw_root/lib/aius_sd/include" \
		"-I$fw_root/lib/aius_config/include" \
		"-I$fw_root/lib/aius_dsp_config/include" \
		"-I$fw_root/lib/aius_json/include" \
		"-I$fw_root/lib/aius_runtime/include" \
		"-I$fw_root/lib/aius_usb_control/include" \
		"-I$fw_root/src")
	# aius_json is here because aius_optical_app.cpp's replay body reader is one
	# of its three callers; the two config parsers are the others, and neither
	# links into this suite.
	runtime_target_portable=()
	for portable in aius_net/src/aius_net aius_http/src/aius_http \
		aius_http/src/aius_http_transaction aius_sd/src/aius_sd \
		aius_usb_control/src/aius_usb_control \
		aius_json/src/aius_json \
		aius_runtime/src/aius_runtime; do
		object="$build_dir/runtime-target-$(basename "$portable").o"
		"$cc" "${cflags[@]}" -Wpedantic "${runtime_target_inc[@]}" \
			-c "$fw_root/lib/$portable.c" -o "$object"
		runtime_target_portable+=("$object")
	done
	runtime_target_out="$build_dir/runtime-target"
	"$cxx" -std=c++17 -Wall -Wextra -Werror -Wpedantic -O1 -g \
		-fsanitize=address,undefined -fno-sanitize=enum \
		-DAIUS_NETWORK_TEENSY_TESTING \
		"${runtime_target_inc[@]}" \
		"$fw_root/test/runtime-target/test_runtime_target.cpp" \
		"$fw_root/src/aius_cdc_teensy.cpp" \
		"$fw_root/src/aius_network_teensy.cpp" \
		"$fw_root/src/aius_sd_teensy.cpp" \
		"$fw_root/src/aius_optical_app.cpp" \
		"$fw_root/src/aius_runtime_network_http.cpp" \
		"$fw_root/src/aius_runtime_storage.cpp" \
		"$fw_root/src/aius_runtime_usb.cpp" \
		"$fw_root/src/aius_runtime_teensy.cpp" \
		"${runtime_target_portable[@]}" \
		-o "$runtime_target_out"
	"$runtime_target_out"
	;;
sd-target)
	python3 "$fw_root/test/sd-target/test_source_guard.py"
	sd_target_out="$build_dir/sd-target"
	"$cxx" -std=c++17 -Wall -Wextra -Werror -O1 -g \
		-fsanitize=address,undefined -fno-sanitize=enum \
		"-I$fw_root/test/sd-target/fake" \
		"-I$fw_root/lib/aius_sd/include" \
		"-I$fw_root/src" \
		"$fw_root/test/sd-target/test_aius_sd_teensy.cpp" \
		"$fw_root/src/aius_sd_teensy.cpp" \
		-o "$sd_target_out"
	"$sd_target_out"
	;;
memory)
	# The per-bank report plus the optical audio-memory post-link verifier.
	# Host tests drive the generic gate over a committed real-ELF reduction and
	# the target verifier over synthetic symbol/section snapshots, including
	# every refusal. No cross-build or cross toolchain is needed here.
	python3 "$fw_root/test/memory/test_mem_report.py"
	python3 "$fw_root/test/memory/test_target_memory.py"
	;;
itcm-placement)
	# The post-link ITCM placement guard: six checks -- the environment carries
	# no -flto, cold objects emit no ITCM, the optical and mixed-ingress objects
	# emit exactly their pinned producer allowlists, every LDF-resolved library
	# is either gated to zero ITCM or named in the allowlist, and recorder
	# bring-up geometry stays cold. Named here so it is reachable like its
	# siblings -- boot-target runs the mutation tests for its own post-link
	# verifier, and network-transport runs the vendor-header source contract,
	# while this guard was reachable only through platformio.ini's post-link
	# hook.
	#
	# The guard's own tests are host-only and run first, so this suite is never
	# a complete no-op; only the target half below needs a cross-build.
	python3 "$fw_root/test/itcm-placement/test_verify_target.py"
	# The rest re-runs the guard over the objects a build already left behind;
	# it does not build, and it cannot substitute for a build. A green result is
	# only as current as .pio/build/$AIUS_ITCM_ENV.
	itcm_env="${AIUS_ITCM_ENV:-teensy41-optical}"
	itcm_build="$fw_root/.pio/build/$itcm_env"
	if [ ! -f "$itcm_build/firmware.elf" ]; then
		echo "==> itcm-placement needs a linked $itcm_env image." >&2
		echo "    Not found: $itcm_build/firmware.elf" >&2
		echo "    Build one first: scripts/build.sh $itcm_env" >&2
		exit 3
	fi
	# The guard reads ARM object files, so it needs the cross readelf. That is
	# not on PATH here; build.sh resolves the same toolchain the same way.
	itcm_readelf="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
	itcm_readelf="$itcm_readelf/packages/toolchain-gccarmnoneeabi-teensy/bin/arm-none-eabi-readelf"
	if [ ! -x "$itcm_readelf" ]; then
		itcm_readelf="arm-none-eabi-readelf"
	fi
	if ! command -v "$itcm_readelf" >/dev/null 2>&1; then
		echo "==> itcm-placement needs arm-none-eabi-readelf." >&2
		echo "    Neither the PlatformIO toolchain nor PATH provides it." >&2
		echo "    Provision PlatformIO per scripts/build.sh's header." >&2
		exit 3
	fi
	python3 "$fw_root/test/itcm-placement/verify_target.py" \
		"$itcm_build" --readelf "$itcm_readelf"
	;;
fake-fidelity)
	# Every host fake's CONSTANTS against the real framework headers. A fake with
	# a wrong constant does not fail -- it agrees with itself, and with the tests
	# that assert against the same macros. Only an external reference catches it.
	# Found all three FILE_* modes off by one in both SD fakes (so their FILE_READ
	# was the real FILE_WRITE) and SRC_SRSR_WARM_BOOT, a name imxrt.h does not
	# have, at the bit imxrt.h gives WDOG3. Exits 3 when the framework package is
	# absent, so the native suite stays runnable off a toolchain host.
	python3 "$fw_root/test/fake-fidelity/test_fake_fidelity.py"
	;;
registry)
	# The guard on this file. Every other arm is hand-maintained, so a suite
	# directory only ever runs if somebody remembered to add its arm here, its
	# name to the `# Suites:` header above and its row to README.md -- and
	# twice they did not: test/auth/ and test/config/test_config.c were
	# committed, compiling code that had never once executed. This suite parses
	# the arms out of this file and fails when they, the two catalogues and the
	# directories under test/ disagree. It compiles nothing and needs no
	# toolchain.
	python3 "$fw_root/test/registry/test_registry.py"
	;;
*)
	echo "unknown suite: $suite" >&2
	exit 2
	;;
esac
