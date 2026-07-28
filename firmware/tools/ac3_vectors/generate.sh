#!/usr/bin/env bash
# Deterministically (re)generate the AC-3 differential-test corpus.
#
# Synthesizes known PCM with FFmpeg lavfi sources, encodes each to raw AC-3
# at 48 kHz with FFmpeg's native `ac3` encoder, and decodes each back with
# FFmpeg to produce the reference PCM the host test compares against. The exact
# FFmpeg command lines are the provenance record; re-running reproduces the
# committed .ac3 / .ref.s32 bit-for-bit on a given FFmpeg build.
#
# Usage:
#   tools/ac3_vectors/generate.sh            # write into ./vectors
#   OUT=/path tools/ac3_vectors/generate.sh  # write elsewhere
#
# Env:
#   FFMPEG   ffmpeg binary (default: ffmpeg)
#   OUT      output directory (default: <script dir>/vectors)
#
# Exit: 0 success, 3 ffmpeg missing.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ffmpeg="${FFMPEG:-ffmpeg}"
out="${OUT:-$here/vectors}"

if ! command -v "$ffmpeg" >/dev/null 2>&1; then
	echo "==> ffmpeg not found (set FFMPEG=...)" >&2
	exit 3
fi

mkdir -p "$out"

sr=48000
dur=0.1

# Bitrates are pinned so the committed bytes do not drift with FFmpeg's
# defaults: 640k is AC-3's maximum for 5.1 (the corpus wants encoder fidelity,
# not rate realism), 192k the customary stereo rate.
br_stereo=192k
br_51=640k

# EVERY 5.1 recipe below states `map=` explicitly. The un-mapped `join` guess
# rotated the front three channels of the DTS corpus's legacy vectors -- that
# is defect 30 (see ../dts_vectors/generate.sh for the full anatomy). This
# corpus is new, so no committed bytes depend on the rotated recipe and none
# of it is carried over: channel identity is authored correctly from the
# first commit, and the a52 suite's per-channel tone check pins it.

# encode_stereo <name> <freq_l> <freq_r>
encode_stereo() {
	local name="$1" fl="$2" fr="$3"
	echo "==> $name (stereo tones ${fl}/${fr} Hz)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f lavfi -i "sine=frequency=$fl:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$fr:duration=$dur:sample_rate=$sr" \
		-filter_complex "[0][1]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,volume=0.6[a]" \
		-map "[a]" -c:a ac3 -b:a "$br_stereo" -ar "$sr" "$out/$name.ac3"
}

# encode_51_ident <name> <f_fl> <f_fr> <f_fc> <f_lfe> <f_bl> <f_br>
# Explicitly mapped: input k lands on the channel its argument position names.
encode_51_ident() {
	local name="$1"
	echo "==> $name (5.1 channel-identity tones, explicit map)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f lavfi -i "sine=frequency=$2:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$3:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$4:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$5:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$6:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$7:duration=$dur:sample_rate=$sr" \
		-filter_complex "[0][1][2][3][4][5]join=inputs=6:channel_layout=5.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR,volume=0.6[a]" \
		-map "[a]" -c:a ac3 -b:a "$br_51" -ar "$sr" "$out/$name.ac3"
}

# A click train (impulse every 1024 samples) exercises transient/broadband
# behaviour where a pure tone does not.
click="if(eq(mod(n\,1024)\,0)\,0.8\,0)"

encode_stereo_impulse() {
	local name="$1"
	echo "==> $name (stereo click train)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f lavfi -i "aevalsrc=exprs=$click:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "aevalsrc=exprs=$click:duration=$dur:sample_rate=$sr" \
		-filter_complex "[0][1]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR[a]" \
		-map "[a]" -c:a ac3 -b:a "$br_stereo" -ar "$sr" "$out/$name.ac3"
}

encode_51_impulse() {
	local name="$1"
	echo "==> $name (5.1 click train)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f lavfi -i "aevalsrc=exprs=$click:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "aevalsrc=exprs=$click:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "aevalsrc=exprs=$click:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "aevalsrc=exprs=$click:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "aevalsrc=exprs=$click:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "aevalsrc=exprs=$click:duration=$dur:sample_rate=$sr" \
		-filter_complex "[0][1][2][3][4][5]join=inputs=6:channel_layout=5.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR[a]" \
		-map "[a]" -c:a ac3 -b:a "$br_51" -ar "$sr" "$out/$name.ac3"
}

# decode_ref <name> : FFmpeg's own decode, interleaved s32le, the comparison ref.
decode_ref() {
	local name="$1"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-i "$out/$name.ac3" -f s32le -acodec pcm_s32le -ar "$sr" \
		"$out/$name.ref.s32"
}

encode_stereo         stereo_tones       440 660
encode_stereo_impulse stereo_impulse
encode_51_impulse     surround51_impulse
# Mirrors the hardware bench tone programme (defect 30), correctly assigned.
encode_51_ident       surround51_ident   300 600 900 60 1200 1500

for name in stereo_tones stereo_impulse surround51_impulse surround51_ident; do
	echo "==> decode reference: $name"
	decode_ref "$name"
done

echo "==> corpus written to $out"
