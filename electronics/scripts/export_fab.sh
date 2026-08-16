#!/usr/bin/env bash
# H3d: export the Aius carrier manufacturing package from the routed board.
# Runs DRC (report only) then writes Gerbers, drill, placement (CPL), STEP, and a
# BOM into electronics/manufacturing/ via the pinned KiCad 10.0.4 container. The
# Gerber/drill set is fabricator-neutral (accepted by both PCBWay and JLCPCB); the
# 2-vs-4-layer stackup + impedance geometry live in the H2/H3 plan.
#
# Usage: electronics/scripts/export_fab.sh
# Env:   AIUS_DOCKER_BIN (passed through to ../../scripts/kicad.sh)
# Exit:  0 on successful export (DRC violations are reported, not fatal here -
#        the DRC gate is enforced separately before order).
set -euo pipefail

electronics_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kicad="$electronics_dir/../scripts/kicad.sh"
board="electronics/aius-carrier.kicad_pcb"
sch="electronics/aius-carrier.kicad_sch"
out="electronics/manufacturing"

echo "==> DRC report"
"$kicad" pcb drc --format json -o "$out/drc-report.json" "$board" || true

echo "==> Gerbers"
"$kicad" pcb export gerbers -o "$out/gerbers/" "$board"

echo "==> Drill files"
"$kicad" pcb export drill -o "$out/gerbers/" "$board"

echo "==> Placement (CPL)"
"$kicad" pcb export pos --format csv --units mm -o "$out/aius-carrier-cpl.csv" "$board"

echo "==> STEP model"
"$kicad" pcb export step --no-dnp -o "$out/aius-carrier.step" "$board"

echo "==> BOM"
"$kicad" sch export bom -o "$out/aius-carrier-bom.csv" "$sch"

echo "==> Manufacturing package written to $out/"
