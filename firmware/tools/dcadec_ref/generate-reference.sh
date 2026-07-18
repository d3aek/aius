#!/usr/bin/env bash
# (Re)generate the dcadec bit-exact reference PCM for the dts-lfe test.
#
# Decodes each committed DTS Core vector with the SAME dcadec core path the Aius
# adapter drives — core-only, fixed-point, bit-exact (`dcadec -c -b`) — and emits
# native per-channel output (`-m`, native-layout mono WAVs). wav_to_s32 then
# interleaves the mono WAVs, in AIUS canonical channel order, into one
# `<name>.dcadec.s32` reference committed beside this script. The committed
# dts-lfe suite consumes those .s32 files and NEVER needs dcadec; only
# regenerating them needs the scratch build.
#
# The dcadec flags mirror lib/aius_dts/src/aius_dts.c exactly:
#   DCADEC_FLAG_CORE_ONLY  (-c) + DCADEC_FLAG_CORE_BIT_EXACT (-b)
# so the LFE runs through interpolate_lfe_fixed_fir on both sides.
#
# Usage:
#   tools/dcadec_ref/generate-reference.sh
#   DCADEC_SRC=/path/to/dcadec tools/dcadec_ref/generate-reference.sh
#
# Env:
#   DCADEC_SRC   dcadec clone dir (default: $CLAUDE_SESSION_SCRATCH/dcadec)
#   OUT          reference output dir (default: <script dir>)
#
# Exit: 0 success, 3 clone/tooling missing.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/../.." && pwd)"
dcadec_src="${DCADEC_SRC:-${CLAUDE_SESSION_SCRATCH:-.scratch}/dcadec}"
out="${OUT:-$here}"
vectors="$fw_root/tools/dts_vectors/vectors"
scratch="${CLAUDE_SESSION_SCRATCH:-.scratch}/dcadec_ref_work"

"$here/build-dcadec.sh"
dcadec="$dcadec_src/dcadec"

cc="${CC:-gcc}"
conv="$scratch/wav_to_s32"
mkdir -p "$scratch" "$out"
echo "==> Building wav_to_s32 helper"
"$cc" -std=c11 -Wall -Wextra -Werror -O2 "$here/wav_to_s32.c" -o "$conv"

# gen_ref <name> <aius-ordered dcadec speaker names...>
# Decodes <name>.dts to native mono WAVs, then interleaves the named speakers
# (already in AIUS order) into <name>.dcadec.s32.
gen_ref() {
	local name="$1"
	shift
	echo "==> $name"
	"$dcadec" -c -b -m -q "$vectors/$name.dts" "$scratch/$name.%s.wav"
	local wavs=()
	local spk
	for spk in "$@"; do
		wavs+=("$scratch/$name.$spk.wav")
	done
	"$conv" "$out/$name.dcadec.s32" "${wavs[@]}"
}

# AIUS canonical order is FL,FR,FC,LFE,SL,SR; the dcadec-native speaker names for
# those are L,R,C,LFE,Ls,Rs. Each vector lists only its present channels.
gen_ref stereo_tones        L R
gen_ref surround51_tones    L R C LFE Ls Rs
gen_ref stereo_impulse      L R
gen_ref surround51_impulse  L R C LFE Ls Rs

echo "==> references written to $out"
