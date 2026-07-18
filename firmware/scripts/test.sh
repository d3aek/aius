#!/usr/bin/env bash
# Aius firmware native test runner.
#
# Compiles and runs a host test suite with the portable C code. Foreground-only
# and root-independent: it resolves the firmware root from its own path. The
# Teensy target build is a separate wrapper (added at capability-gate P4).
#
#   scripts/test.sh --environment native --suite <suite>
#
# Suites: arena, codec-api-contract.
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

case "$suite" in
arena)
	compile_and_run arena "$src/aius_arena.c" "$fw_root/test/arena/test_arena.c"
	;;
codec-api-contract)
	compile_and_run codec-api-contract "$fw_root/test/codec-api/test_codec_api_contract.c"
	;;
*)
	echo "unknown suite: $suite" >&2
	exit 2
	;;
esac
