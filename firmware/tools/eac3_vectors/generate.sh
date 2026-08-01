#!/usr/bin/env bash
# Deterministically (re)generate the E-AC-3 differential-test corpus.
#
# Synthesizes known PCM with FFmpeg lavfi sources, encodes each to raw E-AC-3
# with FFmpeg's native `eac3` encoder (independent substream only -- the
# encoder emits no dependent substreams, matching the box's 5.1-only scope),
# and decodes the differential vectors back with FFmpeg to produce the
# reference PCM the host test compares against. The exact FFmpeg command
# lines are the provenance record; re-running reproduces the committed output
# bit-for-bit on a given FFmpeg build.
#
# Usage:
#   tools/eac3_vectors/generate.sh            # write into ./vectors
#   OUT=/path tools/eac3_vectors/generate.sh  # write elsewhere
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

# Pinned bitrates so committed bytes do not drift with FFmpeg's defaults.
# 192k stereo / 384k 5.1 are customary DD+ broadcast rates.
br_mono=96k
br_stereo=192k
br_51=384k

# Channel identity is authored with explicit `map=` from the first commit --
# the un-mapped `join` guess is defect 30's anatomy (see
# ../ac3_vectors/generate.sh).

# encode_mono <name> <freq>
encode_mono() {
	local name="$1" f="$2"
	echo "==> $name (mono tone ${f} Hz)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f lavfi -i "sine=frequency=$f:duration=$dur:sample_rate=$sr" \
		-filter_complex "[0]volume=0.6[a]" \
		-map "[a]" -c:a eac3 -b:a "$br_mono" -ar "$sr" "$out/$name.eac3"
}

# encode_stereo_rate <name> <sample_rate> <freq_l> <freq_r>
encode_stereo_rate() {
	local name="$1" rate="$2" fl="$3" fr="$4"
	echo "==> $name (${rate} Hz stereo tones ${fl}/${fr} Hz)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f lavfi -i "sine=frequency=$fl:duration=$dur:sample_rate=$rate" \
		-f lavfi -i "sine=frequency=$fr:duration=$dur:sample_rate=$rate" \
		-filter_complex "[0][1]join=inputs=2:channel_layout=stereo:map=0.0-FL|1.0-FR,volume=0.6[a]" \
		-map "[a]" -c:a eac3 -b:a "$br_stereo" -ar "$rate" "$out/$name.eac3"
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
		-map "[a]" -c:a eac3 -b:a "$br_51" -ar "$sr" "$out/$name.eac3"
}

# A click train (impulse every 1024 samples) exercises transient/broadband
# behaviour where a pure tone does not.
click="if(eq(mod(n\,1024)\,0)\,0.8\,0)"

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
		-map "[a]" -c:a eac3 -b:a "$br_51" -ar "$sr" "$out/$name.eac3"
}

# decode_ref <name> <sample_rate> : FFmpeg's own decode, interleaved s32le.
# `-f eac3` pins the demuxer: raw-stream probing misidentifies some of the
# short stereo vectors.
decode_ref() {
	local name="$1" rate="$2"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-f eac3 -i "$out/$name.eac3" -f s32le -acodec pcm_s32le -ar "$rate" \
		"$out/$name.ref.s32"
}

encode_mono           mono_tone          440
encode_stereo_rate    stereo_tones       "$sr" 440 660
encode_stereo_rate    stereo_tones_44100 44100 440 660
encode_stereo_rate    stereo_tones_32000 32000 440 660
encode_51_impulse     surround51_impulse
# Mirrors the hardware bench tone programme (defect 30), correctly assigned.
encode_51_ident       surround51_ident   300 600 900 60 1200 1500

for name in mono_tone stereo_tones surround51_impulse surround51_ident; do
	echo "==> decode reference: $name"
	decode_ref "$name" "$sr"
done

for rate in 44100 32000; do
	name="stereo_tones_$rate"
	echo "==> decode reference: $name"
	decode_ref "$name" "$rate"
done

echo "==> corpus written to $out"
