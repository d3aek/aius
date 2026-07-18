#!/usr/bin/env bash
# Deterministically (re)generate the IEC 61937 parser known-answer corpus.
#
# Compiles gen_iec_corpus.c and runs it to wrap a real DTS Core access unit (from
# the committed dts_vectors corpus) in IEC 61937 framing across the packing/endian
# matrix, plus the synthesized edge cases. The C generator is the authoritative
# provenance record; re-running reproduces the committed vectors bit-for-bit.
#
# Usage:
#   tools/iec_corpus/generate.sh            # write into ./vectors
#   OUT=/path tools/iec_corpus/generate.sh  # write elsewhere
#
# Env:
#   CC   compiler (default: gcc)
#   OUT  output directory (default: <script dir>/vectors)
#
# Exit: 0 success, 1 build/run failure.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/../.." && pwd)"
cc="${CC:-gcc}"
out="${OUT:-$here/vectors}"
dts="$fw_root/tools/dts_vectors/vectors"

mkdir -p "$out"

build_dir="$fw_root/.build/native"
mkdir -p "$build_dir"
gen="$build_dir/gen_iec_corpus"

echo "==> compiling generator"
"$cc" -std=c11 -Wall -Wextra -Werror -O1 -g \
	-I"$fw_root/lib/aius_iec61937/include" \
	-I"$fw_root/lib/aius_codec_api/include" \
	"$here/gen_iec_corpus.c" -o "$gen"

echo "==> generating corpus into $out"
"$gen" "$dts" "$out"

echo "==> corpus written"
