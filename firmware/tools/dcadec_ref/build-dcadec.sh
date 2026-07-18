#!/usr/bin/env bash
# Build the vendored-lineage dcadec CLI from the scratch clone.
#
# The clone MUST be the same upstream commit we vendored (b93deed1); its core
# decode is what the dts-lfe reference is generated from. This is a thin driver
# over the clone's own Makefile — no logic lives here.
#
# Usage:
#   tools/dcadec_ref/build-dcadec.sh              # build in $DCADEC_SRC
#   DCADEC_SRC=/path/to/dcadec tools/dcadec_ref/build-dcadec.sh
#
# Env:
#   DCADEC_SRC   dcadec clone dir (default: $CLAUDE_SESSION_SCRATCH/dcadec)
#
# Exit: 0 success, 3 clone missing.
set -euo pipefail

dcadec_src="${DCADEC_SRC:-${CLAUDE_SESSION_SCRATCH:-.scratch}/dcadec}"

if [ ! -f "$dcadec_src/Makefile" ]; then
	echo "==> dcadec clone not found at $dcadec_src (set DCADEC_SRC=...)" >&2
	exit 3
fi

echo "==> Building dcadec in $dcadec_src"
make -C "$dcadec_src"

if [ ! -x "$dcadec_src/dcadec" ]; then
	echo "==> dcadec binary not produced" >&2
	exit 3
fi

echo "==> dcadec CLI ready: $dcadec_src/dcadec"
