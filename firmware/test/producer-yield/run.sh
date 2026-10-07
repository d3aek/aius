#!/usr/bin/env bash
# Build and run the producer-yield host suite natively.
# Usage: firmware/test/producer-yield/run.sh
# Env: CC (default gcc), BUILD_DIR (default: a mktemp dir).
# Exit: 0 pass, nonzero on compile or check failure.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/../.." && pwd)"
cc="${CC:-gcc}"
build_dir="${BUILD_DIR:-$(mktemp -d)}"
mkdir -p "$build_dir"

"$cc" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
	"-I$fw_root/lib/aius_producer_yield/include" \
	"-I$fw_root/lib/aius_boot/include" \
	"$here/test_producer_yield.c" \
	"$fw_root/lib/aius_producer_yield/src/aius_producer_yield.c" \
	-o "$build_dir/producer-yield"
"$build_dir/producer-yield"
