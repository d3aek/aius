#!/usr/bin/env bash
# H3d: run kicad-cli DRC against a fab's DFM capability rule set
# (electronics/dfm/<fab>.kicad_dru), on a disposable scratch project copy of
# the given board -- never the canonical project files by default. Merges
# the fab overlay with the board's own .kicad_dru (KiCad loads one
# .kicad_dru per project) and elevates the DFM-relevant rule severities to
# "error" in the scratch project only, since the canonical project may keep
# some of them at "warning" for day-to-day routing review. Also applies the
# fab's minimum solder-mask bridge (sliver) width to the scratch board's own
# setup block -- KiCad has no `.kicad_dru` constraint for mask-web geometry.
#
# --fix applies the safe DFM fixes (silkscreen reference/value text stroke
# width raised to the fab's minimum line width; per-footprint solder-mask
# overrides from the data table electronics/dfm/overrides.json, e.g. U11's
# fine-pitch mask-bridge margin; then a zone refill against the freshly
# loaded board so the .kicad_dru NPTH hole_clearance rule is honoured by the
# filler) to the scratch copy before running DRC, to prove that each
# violation type reaches zero. --apply-fix applies the same fixes directly,
# in place, to the board named by --board (intended for the canonical board
# once the scratch run above has proven it clean) and does not run DRC.
#
# Deliberately NOT auto-fixed: any footprint's solder-mask margin not
# listed in electronics/dfm/overrides.json, and the NPTH-mounting-hole
# hole_clearance design-rule floor itself (owned by aius-carrier.kicad_dru,
# not this script) -- both are design decisions recorded where they're
# owned, not mechanical text/zone edits.
#
# Usage: electronics/scripts/check_dfm.sh [--board <path>] [--fab jlcpcb-4l] [--fix]
#        electronics/scripts/check_dfm.sh --apply-fix [--board <path>]
# Env:   AIUS_DOCKER_BIN (passed through to ../../scripts/kicad.sh)
# Exit:  0 no DFM violations (or --apply-fix succeeded); 2 DFM violations
#        found; 3 missing fab rule file, missing overrides file, or a
#        required tool; 4 bad argument.
set -euo pipefail

electronics_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
aius_dir="$(cd -- "$electronics_dir/.." && pwd)"
kicad="$aius_dir/scripts/kicad.sh"

board="electronics/aius-carrier.kicad_pcb"
fab="jlcpcb-4l"
fix=0
apply_fix=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --board)
      board="$2"
      shift 2
      ;;
    --fab)
      fab="$2"
      shift 2
      ;;
    --fix)
      fix=1
      shift
      ;;
    --apply-fix)
      apply_fix=1
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 4
      ;;
  esac
done

if ! command -v jq > /dev/null 2>&1; then
  echo "jq is required" >&2
  exit 3
fi

# Materialize the safe-fix pcbnew script into $1 (a scratch path). Kept as
# an embedded heredoc rather than a standalone committed .py file so the one
# committed entry point (this script) stays the reproducible source for the
# whole DFM workflow per the subtree CLAUDE.md.
write_fix_script() {
  cat > "$1" << 'PYEOF'
"""Apply the safe DFM fixes to a board in place, on a freshly loaded pcbnew
board: (1) raise silkscreen reference/value text stroke width to the fab's
minimum line width -- a text-property edit only; (2) apply per-footprint
solder-mask-margin overrides from the data table (electronics/dfm/
overrides.json) -- e.g. U11's fine-pitch mask-bridge margin -- without
touching the board-global pad_to_mask_clearance every other pad relies on;
(3) refill every copper zone (ZONE_FILLER, as route_import_ses.py does) so
the .kicad_dru NPTH hole_clearance rule (parsed at board load) is honoured
by the filler. Idempotent: re-running against an already-fixed board bumps
zero silk texts and applies zero overrides (the refill always runs, but
against unchanged geometry and settings it produces the same fill).
Deliberately does not touch any footprint's solder-mask margin absent from
the overrides table, or the NPTH hole_clearance design-rule floor itself
(owned by aius-carrier.kicad_dru, not this script) -- both are design
decisions recorded where they're owned.
"""
import argparse
import json

import pcbnew

parser = argparse.ArgumentParser()
parser.add_argument("--board", required=True)
parser.add_argument("--min-silk-thickness-mm", type=float, required=True)
parser.add_argument("--overrides", required=True)
args = parser.parse_args()

board = pcbnew.LoadBoard(args.board)

min_silk_thickness = pcbnew.FromMM(args.min_silk_thickness_mm)
silk_layers = {pcbnew.F_SilkS, pcbnew.B_SilkS}

bumped_texts = 0
for footprint in board.GetFootprints():
    for text in (footprint.Reference(), footprint.Value()):
        if text.GetLayer() in silk_layers and text.GetTextThickness() < min_silk_thickness:
            text.SetTextThickness(min_silk_thickness)
            bumped_texts += 1
for drawing in board.GetDrawings():
    if isinstance(drawing, pcbnew.PCB_TEXT) and drawing.GetLayer() in silk_layers:
        if drawing.GetTextThickness() < min_silk_thickness:
            drawing.SetTextThickness(min_silk_thickness)
            bumped_texts += 1
print(f"bumped {bumped_texts} silk text items to >= {args.min_silk_thickness_mm} mm thickness", flush=True)

with open(args.overrides, encoding="utf-8") as overrides_file:
    overrides = json.load(overrides_file)

footprints_by_ref = {footprint.GetReference(): footprint for footprint in board.GetFootprints()}
applied_overrides = 0
for ref, override in overrides.items():
    footprint = footprints_by_ref.get(ref)
    if footprint is None:
        continue
    margin_mm = override.get("solder_mask_margin_mm")
    if margin_mm is None:
        continue
    target_margin = pcbnew.FromMM(margin_mm)
    if footprint.GetLocalSolderMaskMargin() == target_margin:
        continue
    footprint.SetLocalSolderMaskMargin(target_margin)
    applied_overrides += 1
    why = override.get("why", "")
    print(f"set {ref} local solder mask margin to {margin_mm} mm ({why})", flush=True)
print(f"applied {applied_overrides} footprint DFM override(s) from {args.overrides}", flush=True)

filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())
print(f"refilled {len(board.Zones())} zones", flush=True)

pcbnew.SaveBoard(args.board, board)
print(f"saved {args.board}", flush=True)
PYEOF
}

fab_dru_rel="electronics/dfm/$fab.kicad_dru"
if [[ ! -f "$aius_dir/$fab_dru_rel" ]]; then
  echo "no fab rule file: $fab_dru_rel" >&2
  exit 3
fi

overrides_rel="electronics/dfm/overrides.json"
if [[ ! -f "$aius_dir/$overrides_rel" ]]; then
  echo "no DFM overrides file: $overrides_rel" >&2
  exit 3
fi

# JLCPCB 4L min silk line (text stroke) width is 0.153 mm
# (electronics/dfm/jlcpcb-4l.kicad_dru); a small margin over the published
# floor clears the sub-micron measurement rounding kicad-cli reports
# against a value set exactly at the floor.
min_silk_thickness_mm=0.16

if [[ "$apply_fix" -eq 1 ]]; then
  fix_script_rel=".scratch/dfm/apply-fixes.py"
  mkdir -p -- "$aius_dir/.scratch/dfm"
  write_fix_script "$aius_dir/$fix_script_rel"
  echo "==> Applying safe DFM fixes in place to $board"
  "$kicad" --python "$fix_script_rel" --board "$board" \
    --min-silk-thickness-mm "$min_silk_thickness_mm" \
    --overrides "$overrides_rel"
  exit 0
fi

board_dir="$(dirname -- "$board")"
board_stem="$(basename -- "$board" .kicad_pcb)"
scratch_rel=".scratch/dfm/$fab-$board_stem"
scratch_dir="$aius_dir/$scratch_rel"

rm -rf -- "$scratch_dir"
mkdir -p -- "$scratch_dir"

cp -- "$aius_dir/$board" "$scratch_dir/$board_stem.kicad_pcb"
cp -- "$aius_dir/$board_dir/$board_stem.kicad_pro" "$scratch_dir/$board_stem.kicad_pro"
cp -- "$electronics_dir/fp-lib-table" "$scratch_dir/fp-lib-table"
cp -- "$electronics_dir/sym-lib-table" "$scratch_dir/sym-lib-table"
cp -r -- "$electronics_dir/lib" "$scratch_dir/lib"

# One .kicad_dru per project: the fab overlay goes first so the canonical
# board's own narrower, documented per-footprint exceptions (e.g. J1's
# adapted-land-pattern NPTH clearance) win -- KiCad applies the last
# matching rule for a given constraint.
cat -- "$aius_dir/$fab_dru_rel" "$aius_dir/$board_dir/$board_stem.kicad_dru" \
  > "$scratch_dir/$board_stem.kicad_dru"

# Elevate the DFM-relevant checks to "error" in the scratch project only.
severities_tmp="$scratch_dir/severities.tmp.json"
jq '
  .board.design_settings.rule_severities.silk_over_copper = "error"
  | .board.design_settings.rule_severities.solder_mask_bridge = "error"
  | .board.design_settings.rule_severities.copper_edge_clearance = "error"
  | .board.design_settings.rule_severities.annular_width = "error"
  | .board.design_settings.rule_severities.drill_out_of_range = "error"
  | .board.design_settings.rule_severities.hole_clearance = "error"
  | .board.design_settings.rule_severities.hole_to_hole = "error"
  | .board.design_settings.rule_severities.courtyards_overlap = "error"
  | .board.design_settings.rule_severities.footprint_symbol_mismatch = "error"
  | .board.design_settings.rule_severities.footprint_symbol_field_mismatch = "error"
  | .board.design_settings.rule_severities.lib_footprint_mismatch = "error"
' "$scratch_dir/$board_stem.kicad_pro" > "$severities_tmp"
mv -- "$severities_tmp" "$scratch_dir/$board_stem.kicad_pro"

# JLCPCB 4L min solder-mask bridge (sliver) width: 0.2 mm, on the scratch
# board's own setup block (see electronics/dfm/jlcpcb-4l.kicad_dru).
if grep -q '(pad_to_mask_clearance ' "$scratch_dir/$board_stem.kicad_pcb"; then
  board_tmp="$scratch_dir/board.tmp.kicad_pcb"
  sed -E 's/\(pad_to_mask_clearance [^)]*\)/(pad_to_mask_clearance 0)\n\t\t(solder_mask_min_width 0.2)/' \
    "$scratch_dir/$board_stem.kicad_pcb" > "$board_tmp"
  mv -- "$board_tmp" "$scratch_dir/$board_stem.kicad_pcb"
fi

if [[ "$fix" -eq 1 ]]; then
  fix_script_rel="$scratch_rel/apply-fixes.py"
  write_fix_script "$aius_dir/$fix_script_rel"
  echo "==> Applying safe DFM fixes to the scratch copy"
  "$kicad" --python "$fix_script_rel" --board "$scratch_rel/$board_stem.kicad_pcb" \
    --min-silk-thickness-mm "$min_silk_thickness_mm" \
    --overrides "$overrides_rel"
fi

echo "==> DFM DRC ($fab) against $board"
"$kicad" pcb drc --format json --severity-all --all-track-errors \
  -o "$scratch_rel/dfm-report.json" "$scratch_rel/$board_stem.kicad_pcb" || true

report="$scratch_dir/dfm-report.json"
if [[ ! -f "$report" ]]; then
  echo "kicad-cli did not write a DRC report" >&2
  exit 3
fi

violation_count="$(jq '.violations | length' "$report")"

echo "==> Violations by type"
jq -r '
  .violations
  | group_by(.type)
  | map({type: .[0].type, count: length})
  | sort_by(-.count)
  | .[]
  | "\(.count)\t\(.type)"
' "$report"

echo "==> Report: $scratch_rel/dfm-report.json"

if [[ "$violation_count" -gt 0 ]]; then
  echo "==> DFM violations: $violation_count" >&2
  exit 2
fi

echo "==> DFM clean: 0 violations"
