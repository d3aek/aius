#!/usr/bin/env bash
# Render the generated Aius carrier schematic and the canonical PCB in the
# pinned KiCad container.
# Usage: electronics/scripts/render.sh
# Env: AIUS_DOCKER_BIN (passed through to ../../scripts/kicad.sh).
# Outputs:
#   electronics/aius-carrier.svg, electronics/aius-carrier.pdf (schematic)
#   electronics/aius-carrier-pcb-top.svg, electronics/aius-carrier-pcb-bottom.svg
#     (2D copper/silk/mask/fab layer stacks, page sized to the board)
#   electronics/aius-carrier-pcb-in1.svg, electronics/aius-carrier-pcb-in2.svg
#     (inner-layer review renders, In1.Cu/In2.Cu + Edge.Cuts)
#   electronics/aius-carrier-pcb-top.png, electronics/aius-carrier-pcb-bottom.png
#     (3D raytraced renders, skipped with a warning if the container has no GL)
# Exit: 0 on success. The KiCad container writes into electronics/, which must be
#       writable by the container uid (the repo checkout normally is; if not, grant
#       the output dir others-write - git does not track that bit).
set -euo pipefail

electronics_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kicad="$electronics_dir/../scripts/kicad.sh"
sch="electronics/aius-carrier.kicad_sch"
pcb="electronics/aius-carrier.kicad_pcb"

echo "==> Exporting SVG"
"$kicad" sch export svg --output electronics "$sch"

echo "==> Exporting PDF"
"$kicad" sch export pdf --output electronics/aius-carrier.pdf "$sch"

echo "==> Rendered electronics/aius-carrier.svg and electronics/aius-carrier.pdf"

echo "==> Exporting PCB top-side SVG"
"$kicad" pcb export svg \
  --layers F.Cu,F.SilkS,F.Mask,F.Fab,Edge.Cuts \
  --mode-single --page-size-mode 2 \
  --output electronics/aius-carrier-pcb-top.svg "$pcb"

echo "==> Exporting PCB bottom-side SVG"
"$kicad" pcb export svg \
  --layers B.Cu,B.SilkS,B.Mask,Edge.Cuts \
  --mode-single --mirror --page-size-mode 2 \
  --output electronics/aius-carrier-pcb-bottom.svg "$pcb"

echo "==> Exporting PCB inner-layer review SVGs"
"$kicad" pcb export svg \
  --layers In1.Cu,Edge.Cuts \
  --mode-single --page-size-mode 2 \
  --output electronics/aius-carrier-pcb-in1.svg "$pcb"
"$kicad" pcb export svg \
  --layers In2.Cu,Edge.Cuts \
  --mode-single --page-size-mode 2 \
  --output electronics/aius-carrier-pcb-in2.svg "$pcb"

echo "==> Rendered electronics/aius-carrier-pcb-top.svg, -bottom.svg, -in1.svg, -in2.svg"

echo "==> Rendering PCB 3D top view"
if "$kicad" pcb render --side top --width 1600 --height 1200 \
  --output electronics/aius-carrier-pcb-top.png "$pcb"; then
  echo "==> Rendering PCB 3D bottom view"
  "$kicad" pcb render --side bottom --width 1600 --height 1200 \
    --output electronics/aius-carrier-pcb-bottom.png "$pcb"
  echo "==> Rendered electronics/aius-carrier-pcb-top.png and -bottom.png"
else
  echo "==> Skipping 3D PCB PNG renders (kicad-cli pcb render failed - no GL in this container)"
fi
