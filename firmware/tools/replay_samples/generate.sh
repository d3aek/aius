#!/usr/bin/env bash
# Generate the synthetic 5.1 replay sample pack (WAV + raw DTS Core + IEC 61937).
#
# Usage:
#   tools/replay_samples/generate.sh                 # 30 s pack into ./generated
#   OUT=/path DURATION=60 tools/replay_samples/generate.sh
#
# Env:
#   FFMPEG    ffmpeg binary       (default: ffmpeg)
#   OUT       output directory    (default: <script dir>/generated)
#   DURATION  seconds per item    (default: 30)
#
# Exit: 0 success, 3 ffmpeg missing.
#
# EVERY `join` HERE STATES `map=`, and that is load-bearing. Without it `join`
# guesses the input-to-output assignment by channel NAME; each mono `aevalsrc`
# declares layout `mono`, whose one channel is named FC, so all six inputs claim
# FC and the guess rotates the front three (input 0 to FC, input 1 to FL, input 2
# to FR) while leaving LFE and the surrounds alone. Nothing errors.
#
# This pack's `channel_id_51` IS the hardware bench tone programme, and the
# un-mapped form is what produced defect 30 in
# ../../../docs/plans/aius-network-hardening.md: the decoded capture showed FR's
# tone in the left output and read as a decoder channel permutation. The decoder
# was correct; the material was rotated before it was ever encoded. Regenerate
# and re-upload any pack produced before 2026-07-28.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ffmpeg="${FFMPEG:-ffmpeg}"
out="${OUT:-$here/generated}"
duration="${DURATION:-30}"
sr=48000

if ! command -v "$ffmpeg" >/dev/null 2>&1; then
	echo "==> ffmpeg not found (set FFMPEG=...)" >&2
	exit 3
fi

mkdir -p "$out"

encode() {
	local name="$1"
	local filter="$2"
	echo "==> $name (${duration}s 5.1 WAV, DTS Core, IEC 61937)"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-filter_complex "$filter" \
		-map "[mix]" -t "$duration" -ar "$sr" -c:a pcm_s24le "$out/$name.wav"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-i "$out/$name.wav" -map 0:a -ar "$sr" -c:a dca -strict -2 \
		"$out/$name.dts"
	"$ffmpeg" -hide_banner -loglevel error -y \
		-i "$out/$name.dts" -map 0:a -c:a copy -f spdif "$out/$name.iec"
}

encode channel_id_51 \
	"aevalsrc=0.45*sin(2*PI*300*t):s=$sr:d=${duration}[fl]; \
	 aevalsrc=0.45*sin(2*PI*600*t):s=$sr:d=${duration}[fr]; \
	 aevalsrc=0.45*sin(2*PI*900*t):s=$sr:d=${duration}[c]; \
	 aevalsrc=0.45*sin(2*PI*60*t):s=$sr:d=${duration}[lfe]; \
	 aevalsrc=0.45*sin(2*PI*1200*t):s=$sr:d=${duration}[sl]; \
	 aevalsrc=0.45*sin(2*PI*1500*t):s=$sr:d=${duration}[sr]; \
	 [fl][fr][c][lfe][sl][sr]join=inputs=6:channel_layout=5.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR[mix]"

encode channel_impulse_51 \
	"aevalsrc=if(eq(mod(n\\,28800)\\,0)\\,0.8\\,0):s=$sr:d=${duration}[fl]; \
	 aevalsrc=if(eq(mod(n\\,28800)\\,4800)\\,0.8\\,0):s=$sr:d=${duration}[fr]; \
	 aevalsrc=if(eq(mod(n\\,28800)\\,9600)\\,0.8\\,0):s=$sr:d=${duration}[c]; \
	 aevalsrc=if(eq(mod(n\\,28800)\\,14400)\\,0.8\\,0):s=$sr:d=${duration}[lfe]; \
	 aevalsrc=if(eq(mod(n\\,28800)\\,19200)\\,0.8\\,0):s=$sr:d=${duration}[sl]; \
	 aevalsrc=if(eq(mod(n\\,28800)\\,24000)\\,0.8\\,0):s=$sr:d=${duration}[sr]; \
	 [fl][fr][c][lfe][sl][sr]join=inputs=6:channel_layout=5.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR[mix]"

encode cinematic_program_51 \
	"aevalsrc=0.18*sin(2*PI*(220+35*sin(2*PI*0.25*t))*t)+0.08*sin(2*PI*440*t):s=$sr:d=${duration}[fl]; \
	 aevalsrc=0.18*sin(2*PI*(277+29*sin(2*PI*0.21*t))*t)+0.08*sin(2*PI*554*t):s=$sr:d=${duration}[fr]; \
	 aevalsrc=(0.24+0.12*sin(2*PI*0.7*t))*sin(2*PI*(520+35*sin(2*PI*0.11*t))*t):s=$sr:d=${duration}[c]; \
	 aevalsrc=0.28*sin(2*PI*(55+4*sin(2*PI*0.17*t))*t):s=$sr:d=${duration}[lfe]; \
	 aevalsrc=0.07*sin(2*PI*180*t)+0.035*sin(2*PI*320*t):s=$sr:d=${duration}[sl]; \
	 aevalsrc=0.06*sin(2*PI*197*t)+0.03*sin(2*PI*337*t):s=$sr:d=${duration}[sr]; \
	 [fl][fr][c][lfe][sl][sr]join=inputs=6:channel_layout=5.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR[mix]"

echo "==> generated in $out"
