#!/usr/bin/env bash
# H3d: export the Aius carrier manufacturing package from the routed board.
# Runs DRC (report only), the JLCPCB 4L DFM rule set (report only, via
# check_dfm.sh), and the routing-geometry gate (report only, via
# check_routing_geometry.py -- straight-segments-only / 15-degree-bend Owner
# ruling), then writes Gerbers, drill, placement (CPL), STEP, and a BOM into
# electronics/manufacturing/ via the pinned KiCad 10.0.4 container. The
# Gerber/drill set is fabricator-neutral (accepted by both PCBWay and JLCPCB); the
# 2-vs-4-layer stackup + impedance geometry live in the H2/H3 plan.
#
# Usage: electronics/scripts/export_fab.sh [--board <path>]
# Env:   AIUS_DOCKER_BIN (passed through to ../../scripts/kicad.sh)
# Exit:  0 on successful export (DRC, DFM, and routing-geometry violations
#        are reported, not fatal here - every gate is enforced separately
#        before order); 4 on a bad argument.
#
# --board defaults to the canonical electronics/aius-carrier.kicad_pcb. Pass a
# scaffolded candidate's .kicad_pcb to export its own DRC report + manufacturing
# package into a sibling "manufacturing/" directory next to that candidate,
# rather than into the canonical one.
set -euo pipefail

electronics_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kicad="$electronics_dir/../scripts/kicad.sh"
board="electronics/aius-carrier.kicad_pcb"
sch="electronics/aius-carrier.kicad_sch"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --board)
      board="$2"
      shift 2
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 4
      ;;
  esac
done

board_dir="$(dirname -- "$board")"
out="$board_dir/manufacturing"
board_stem="$(basename -- "$board" .kicad_pcb)"

echo "==> DRC report"
"$kicad" pcb drc --format json -o "$out/drc-report.json" "$board" || true

echo "==> DFM report (JLCPCB 4L rule set, non-fatal here)"
"$electronics_dir/scripts/check_dfm.sh" --board "$board" || true

echo "==> Routing geometry report (straight segments / 15-degree bends, non-fatal here)"
"$kicad" --python electronics/scripts/check_routing_geometry.py --board "$board" || true

echo "==> Gerbers"
"$kicad" pcb export gerbers -o "$out/gerbers/" "$board"

echo "==> Drill files"
"$kicad" pcb export drill -o "$out/gerbers/" "$board"

echo "==> Placement (CPL)"
"$kicad" pcb export pos --format csv --units mm -o "$out/$board_stem-cpl.csv" "$board"

echo "==> STEP model"
"$kicad" pcb export step --no-dnp -o "$out/$board_stem.step" "$board"

echo "==> BOM"
"$kicad" sch export bom -o "$out/$board_stem-bom.csv" "$sch"

echo "==> Manufacturing package written to $out/"
