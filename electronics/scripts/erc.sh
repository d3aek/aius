#!/usr/bin/env bash
# Run the ERC gate on the generated Aius carrier schematic in the pinned KiCad container.
# Usage: electronics/scripts/erc.sh
# Usage: electronics/scripts/erc.sh --all   # include warnings + exclusions
# Env: AIUS_DOCKER_BIN (passed through to ../../scripts/kicad.sh).
# Exit: 0 = no ERC errors (gate pass); non-zero = ERC errors present (see docs/plans/aius-h1-schematic.md).
set -euo pipefail

electronics_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kicad="$electronics_dir/../scripts/kicad.sh"
sch="electronics/aius-carrier.kicad_sch"

# The wrapper preserves caller ownership; keep the report in checkout scratch.
severity=(--severity-error)
report=".scratch/aius-carrier-erc-$$.rpt"
if [[ "${1:-}" == "--all" ]]; then
	severity=(--severity-all)
fi

echo "==> Running ERC on $sch"
exec "$kicad" sch erc --exit-code-violations "${severity[@]}" -o "$report" "$sch"
