#!/usr/bin/env bash
# Build and run the web-cache host suite natively.
# Usage: firmware/test/web-cache/run.sh
# Env: CC (default gcc), BUILD_DIR (default: a mktemp dir).
# Exit: 0 pass, nonzero on compile or check failure.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$here/../.." && pwd)"
cc="${CC:-gcc}"
build_dir="${BUILD_DIR:-$(mktemp -d)}"
mkdir -p "$build_dir"

# The placement cases link the real recorder ring sizing and OTA staging
# placement, so disjointness is proved against the code that places them.
"$cc" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
	"-I$fw_root/lib/aius_web_cache/include" \
	"-I$fw_root/lib/aius_ota/include" \
	"-I$fw_root/lib/aius_recorder/include" \
	"-I$fw_root/lib/aius_codec_api/include" \
	"$here/test_web_cache.c" \
	"$fw_root/lib/aius_web_cache/src/aius_web_cache.c" \
	"$fw_root/lib/aius_ota/src/aius_ota_staging.c" \
	"$fw_root/lib/aius_recorder/src/aius_recorder.c" \
	-lm -o "$build_dir/web-cache"
"$build_dir/web-cache"

# 4h's store cases (pending states, puts, tombstones, safe copy, folding).
"$cc" -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
	"-I$fw_root/lib/aius_web_cache/include" \
	"$here/test_web_cache_store.c" \
	"$fw_root/lib/aius_web_cache/src/aius_web_cache.c" \
	-o "$build_dir/web-cache-store"
"$build_dir/web-cache-store"
