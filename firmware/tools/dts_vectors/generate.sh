#!/usr/bin/env bash
# Deterministically (re)generate the DTS Core differential-test corpus.
#
# Synthesizes known PCM with FFmpeg lavfi sources, encodes each to raw DTS Core
# at 48 kHz with the experimental `dca` encoder, and decodes each back with
# FFmpeg to produce the reference PCM the host test compares against. The exact
# FFmpeg command lines are the provenance record; re-running reproduces the
# committed .dts / .ref.s32 bit-for-bit on a given FFmpeg build.
#
# Usage:
#   tools/dts_vectors/generate.sh            # write into ./vectors
#   OUT=/path tools/dts_vectors/generate.sh  # write elsewhere
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

# encode_stereo <name> <freq_l> <freq_r>
encode_stereo() {
	local name="$1" fl="$2" fr="$3"
	echo "==> $name (stereo tones ${fl}/${fr} Hz)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f lavfi -i "sine=frequency=$fl:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$fr:duration=$dur:sample_rate=$sr" \
		-filter_complex "[0][1]join=inputs=2:channel_layout=stereo,volume=0.6[a]" \
		-map "[a]" -c:a dca -strict -2 -ar "$sr" "$out/$name.dts"
}

# encode_51 <name> <f_fl> <f_fr> <f_fc> <f_lfe> <f_bl> <f_br>
encode_51() {
	local name="$1"
	echo "==> $name (5.1 tones)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f lavfi -i "sine=frequency=$2:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$3:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$4:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$5:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$6:duration=$dur:sample_rate=$sr" \
		-f lavfi -i "sine=frequency=$7:duration=$dur:sample_rate=$sr" \
		-filter_complex "[0][1][2][3][4][5]join=inputs=6:channel_layout=5.1,volume=0.6[a]" \
		-map "[a]" -c:a dca -strict -2 -ar "$sr" "$out/$name.dts"
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
		-filter_complex "[0][1]join=inputs=2:channel_layout=stereo[a]" \
		-map "[a]" -c:a dca -strict -2 -ar "$sr" "$out/$name.dts"
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
		-filter_complex "[0][1][2][3][4][5]join=inputs=6:channel_layout=5.1[a]" \
		-map "[a]" -c:a dca -strict -2 -ar "$sr" "$out/$name.dts"
}

# decode_ref <name> : FFmpeg's own decode, interleaved s32le, the comparison ref.
decode_ref() {
	local name="$1"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-i "$out/$name.dts" -f s32le -acodec pcm_s32le -ar "$sr" \
		"$out/$name.ref.s32"
}

encode_stereo         stereo_tones       440 660
encode_51             surround51_tones   300 600 900 60 1200 1500
encode_stereo_impulse stereo_impulse
encode_51_impulse     surround51_impulse

for name in stereo_tones surround51_tones stereo_impulse surround51_impulse; do
	echo "==> decode reference: $name"
	decode_ref "$name"
done

echo "==> corpus written to $out"
