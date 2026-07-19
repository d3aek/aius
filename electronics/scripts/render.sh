#!/usr/bin/env bash
# Render the generated Aius carrier schematic to SVG + PDF in the pinned KiCad container.
# Usage: electronics/scripts/render.sh
# Env: AIUS_DOCKER_BIN (passed through to ../../scripts/kicad.sh).
# Outputs: electronics/aius-carrier.svg and electronics/aius-carrier.pdf.
# Exit: 0 on success. The KiCad container writes into electronics/, which must be
#       writable by the container uid (the repo checkout normally is; if not, grant
#       the output dir others-write - git does not track that bit).
set -euo pipefail

electronics_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kicad="$electronics_dir/../scripts/kicad.sh"
sch="electronics/aius-carrier.kicad_sch"

echo "==> Exporting SVG"
"$kicad" sch export svg --output electronics "$sch"

echo "==> Exporting PDF"
"$kicad" sch export pdf --output electronics/aius-carrier.pdf "$sch"

echo "==> Rendered electronics/aius-carrier.svg and electronics/aius-carrier.pdf"
