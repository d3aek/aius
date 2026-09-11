# Aius carrier electronics — generated schematic pipeline

The Aius carrier schematic is **generated from a checked-in connectivity spec**,
ERC-checked and rendered in the digest-pinned KiCad 10.0.4 container
(`../scripts/kicad.sh`). No GUI-only step is the sole way to reproduce it. This
directory implements the H1 foundation of
[`../docs/plans/aius-h1-schematic.md`](../docs/plans/aius-h1-schematic.md): a
**hierarchical multi-sheet project** whose root `aius-carrier.kicad_sch`
instantiates one sub-sheet per functional area. All four sheets are present and
ERC-clean: `power-usb` (H1a), `teensy-expansion` (Teensy 4.1, expansion / service
/ test interfaces), `optical-spdif` (reset-safe ORJ5 → mux → gate → OTJ5 path,
H1b) and `ethernet` (Pulse magjack for the Teensy native-Ethernet header, H1b).
Cross-sheet connectivity is by the global-label net convention below.

The container bind is the Aius checkout itself at `/workspace/aius`, whether
checked out standalone or under `git_modules/aius`. `scripts/kicad.sh` resolves
that directory from its own location; paths passed to KiCad are Aius-relative.
The wrapper runs as the caller's UID/GID so exported files are writable, and
`--python <script> ...` runs the container's `pcbnew` Python environment.
Host user-namespace mapping preserves those IDs on Docker hosts configured with
`userns-remap`; KiCad settings live in the ignored `.scratch/` directory.
`scripts/test_kicad_wrapper.sh` checks both the KiCad and router mounts.
Project library tables name the stock KiCad libraries used by this design;
ERC does not depend on an existing GUI user's global library tables.

## Pipeline

```
connectivity/aius-carrier.json  (project manifest: sheet list)
        |                    connectivity/<sheet>.json  (per-area review surface)
        v                            v
     generate.py --project  -->  aius-carrier.kicad_sch (root)
                                 <sheet>.kicad_sch (one per area)
                                        |  --erc.sh--> ERC gate (whole project)
                                        |  --render.sh-> SVG (per page) + PDF
```

- **`connectivity/aius-carrier.json`** — the project manifest: the ordered sheet
  list (name, spec file, page). `generate.py --project` reads it, emits the
  hierarchical root, and generates each sub-sheet.
- **`connectivity/<sheet>.json`** — one per sheet, the review surface. Every
  component (ref, `lib_id`, value/MPN, footprint) and every net (name → list of
  `[ref, pin]` endpoints), optional component `dnp` (JSON boolean), plus
  `no_connects`, `design_notes`, and a
  `global_nets` list naming the cross-sheet nets (see
  [`connectivity/nets.md`](connectivity/nets.md)).
- **`connectivity/pin-map.md`** — the conflict-checked Teensy 4.1 pad allocation
  (primary review artifact). **`connectivity/nets.md`** — the global cross-sheet
  net registry + the capture-time design decisions (Ethernet PHY, expansion
  power).
- **`generate.py`** — spec → `.kicad_sch`. Connectivity is by **net label**: each
  symbol is placed on a grid, every pin gets a short wire stub and a label
  carrying its net name; nets connect by matching label text. A net listed in the
  spec's `global_nets` becomes a KiCad **global label** (project-wide,
  cross-sheet); all others stay sheet-local. Pin endpoints come from the symbol
  pin geometry (KiCad libs are Y-up, schematics Y-down, so a library pin at
  `(px, py)` lands at schematic `(X + px, Y - py)`; a pin's `angle` points
  tip→body, so the label direction is `angle + 180`). Placement origins are
  snapped to the 1.27 mm grid so every stub endpoint stays on-grid.
- **`lib/aius.kicad_sym`** — custom symbols for parts KiCad does not ship, plus
  single-unit redraws of a few stock parts (the stock `74xGxx` symbols split their
  power pins into a second unit the single-unit generator cannot place):
  `TPD2EUSB30A` (3-pin shunt USB ESD), `TPS2553` (current-limit load switch),
  `Teensy41` (the socketed module, drawn as the carrier-used pads with correct
  electrical types), `TCA9517A` (I2C level-translating buffer/isolator), the
  generic `Conn_1x02..06` connectors (JST GH headers, pin headers, Tag-Connect),
  the `Cliff_ORJ5` / `Cliff_OTJ5` TOSLINK optical module footprint-class symbols
  (the purchased parts are the RS PRO ORJ-5/OTJ-5 drop-ins), the
  `SN74LVC1G3157` bypass mux and the `SN74LVC1G126` TX gate (single-unit, pin
  numbers verified against the datasheets and the stock KiCad symbols), and the
  Pulse `Pulse_J0011D21BNL` RJ45 magjack. The library also retains
  `SN74LVC1G125` and `SN74LVC1G04` symbols from the pre-review TX gate
  (active-low OE plus an inverter); no sheet instantiates them since the H1
  review collapsed that pair into the single active-high-OE `SN74LVC1G126`.
- **`lib/standard-cache.kicad_sym`** — verbatim copies of the stock KiCad symbols
  the generator uses (`R`, `C`, `USB_C_Receptacle_USB2.0_16P`, `USB_A`,
  `PWR_FLAG`), taken from KiCad 10.0.4 (`/usr/share/kicad/symbols`, digest pinned
  in `../compose.yaml`). Lets the generator embed the `lib_symbols` cache and
  compute pin geometry without a live container.

## Commands

```bash
# regenerate the whole project (root + every sub-sheet) from the manifest
electronics/generate.py --project electronics/connectivity/aius-carrier.json electronics

# (legacy) generate a single flat sheet to an explicit output file
electronics/generate.py electronics/connectivity/power-usb.json /tmp/one-sheet.kicad_sch

# ERC gate (exit 0 = no errors); --all also prints warnings
electronics/scripts/erc.sh

# render SVG (one per page) + PDF into this directory
electronics/scripts/render.sh
```

The KiCad wrapper runs as the invoking user and writes outputs into this
checkout. Direct Compose invocations should set `AIUS_UID` and `AIUS_GID` to the
caller's IDs (defaults are 1000). ERC reports are written to `.scratch/`;
the textual summary and exit code are the result.

## ERC status

The 2026-09-09 recovery corrects the Teensy, optical and magjack library geometry
and the U1 package, and connects magjack pin8 to PCB ground per its datasheet.
The regenerated schematic passes full ERC. Reviewed coordinates and the remaining
stock header/cable assembly and physical fit gates are in
[`carrier-footprint-review.md`](../docs/reference/carrier-footprint-review.md).
Run `scripts/kicad.sh --python electronics/scripts/check_footprints.py` from the
Aius root to check the library interfaces. Add `--board electronics/aius-carrier.kicad_pcb`
to check the embedded board copies; the pre-recovery board fails this check and
must be rebuilt before its routing can be accepted.

The USB-host recovery removes the parallel carrier supply: native Teensy UH5V
is J14's sole power source and is correctly typed `power_out`. Regression:
`python3 electronics/scripts/check_host_power.py --netlist <kicadxml>` after
`scripts/kicad.sh sch export netlist --format kicadxml -o <kicadxml>
electronics/aius-carrier.kicad_sch`. See [connectivity/nets.md](connectivity/nets.md)
for source evidence and the separate expansion-3v3 isolation release blocker.

The stock-cable revision uses 53 U10 main/optional-host underside pads. Ethernet
and service mount on the Teensy top and cable to J18/J19. No custom spacer is
needed. The six expansion headers J10–J15 are DNP for the first build; this
does not approve independently powered expansion. Do not fit or externally
power them until isolation and population are reviewed.

`python3 electronics/scripts/check_assembly.py --netlist <kicadxml>` checks the
actual exported cable pin map, core service/UART paths and DNP set.
`python3 electronics/scripts/test_assembly.py` runs positive and failure-case
regressions, including native DNP generation. See the footprint review for the
stock assembly BOM, J19.4 omitted wire and unkeyed-cable continuity procedure.

ERC checks the global `GND` and `+3V3` connections; optical logic uses the
Teensy's `+3V3` output. `VBUS_5V` now feeds only the internal USB-A/cable path.
ERC alone cannot prove powered-off isolation or correct external-module models.

Full-severity ERC (`scripts/erc.sh --all`) is now **0 violations** — the whole
project is footprint-clean, down from 13 across H3a
([aius-h2-h3-manufacturing.md](../docs/plans/aius-h2-h3-manufacturing.md) §H3a):
the four expansion JST GH headers and the Tag-Connect map to stock KiCad
`Connector_JST:` / `Connector:` footprints; the GCT `USB4105` USB-C resolves in
the pinned 10.0.4 container's `Connector_USB`; the internal USB-A re-selected to
the KiCad-native `USB_A_TE_292303-7_Horizontal`; and the four custom land patterns
now live in [`lib/aius.pretty/`](lib/aius.pretty) registered by `fp-lib-table`:
`Cliff_ORJ5_TOSLINK` / `Cliff_OTJ5_TOSLINK` (RS PRO ORJ-5/OTJ-5), the Pulse
`Pulse_J0011D21BNL_TabDown` magjack, and the `Teensy41_Sockets_...` socket
composite (generated by [`lib/gen_teensy_footprint.py`](lib/gen_teensy_footprint.py)
from a reviewable pad table). Signal-pad geometry is datasheet-exact; the
optical peg / magjack staggered / Teensy underside-cluster XY are datasheet
best-estimates flagged for pad-by-pad verification against the physical parts at
the pre-order footprint gate. The optical mux/gate use real KiCad `SOT-23-5/6`
footprints, and the earlier `isolated_pin_label` warnings cleared once the
optical-spdif and ethernet sheets connected the seven cross-sheet signal globals.

The purchased optical modules (2026-07-27) are the RS PRO ORJ-5 / OTJ-5
(`805-1671` / `805-1680`), not the Cliff parts the `aius:Cliff_ORJ5_TOSLINK` /
`aius:Cliff_OTJ5_TOSLINK` footprints were drawn from — pin-compatible, but
validating the Gerber against the RS PRO layout drawings is a recorded H2 gate
([H1 plan §Milestones](../docs/plans/aius-h1-schematic.md#milestones)).

J1 (the USB-C receptacle) uses `aius:USB_C_GCT_USB4105_NPTH_Clearance`, not the
stock `Connector_USB:USB_C_Receptacle_GCT_USB4105-xx-A_16P_TopMnt_Horizontal`
land pattern: the stock pattern's four GND pads violate PCBWay's published
NPTH-to-copper minimum against the connector's own locating holes.
[`lib/gen_usb_footprint.py`](lib/gen_usb_footprint.py) derives the adapted
footprint from the pinned stock geometry, retreating only the hole-facing edge
of those four pads by 0.02 mm (0.2138 mm nominal clearance, within GCT's
±0.05 mm layout tolerance); regenerate with
`scripts/kicad.sh --python electronics/lib/gen_usb_footprint.py`. `pcbnew`
reassigns a fresh UUID to every graphic and pad element on each save, so a
regenerated copy is never byte-identical to the checked-in one — compare
geometry (pad positions, sizes, description text), not raw bytes, when
confirming a regeneration matches.

## Scaffold (H3b)

[`scripts/scaffold_pcb.py`](scripts/scaffold_pcb.py) builds
`aius-carrier.kicad_pcb` from a KiCad XML netlist: the initial four-layer
placement + connectivity scaffold that H3c routes, not a routed board. Run
from the Aius root, inside the container:

```bash
scripts/kicad.sh sch export netlist --format kicadxml \
  -o <netlist.xml> electronics/aius-carrier.kicad_sch
scripts/kicad.sh --python electronics/scripts/scaffold_pcb.py \
  --netlist <netlist.xml> --output <candidate.kicad_pcb>
```

Both `--netlist` and `--output` are mandatory — exploration never overwrites
the routed board implicitly. Every footprint needs an explicit
`(x, y, rotation)` entry in the script's `PLACEMENT` table (or a slot in the
`FRONT_ROW` expansion-connector row); a component in neither is a hard error
(`missing placement: [...]`), never a silent grid fallback. A netlist
component with no footprint, or a footprint missing a pad the netlist
references, is rejected the same way before any board is written.

Alongside `<candidate.kicad_pcb>` the script also writes `<candidate>.kicad_pro`
(carrying the project's net classes, below), `<candidate>.kicad_dru` (a copy of
[`aius-carrier.kicad_dru`](aius-carrier.kicad_dru)), and a `fp-lib-table` with
the `aius` library URI rewritten relative to the output directory — so the
candidate board opens standalone, with its footprints and design rules
resolved, wherever it lands.

The emitted board declares the H2a JLCPCB JLC04161H-7628 four-layer stackup
(`docs/plans/aius-h2-h3-manufacturing.md` Phase H2a;
[`feasibility/stackups.json`](feasibility/stackups.json)) as a `(stackup ...)`
block in the saved `.kicad_pcb` — 0.035 mm outer / 0.0152 mm inner copper over
0.2104 mm prepreg and a 1.065 mm core, 1.5862 mm overall — inserted textually
after `pcbnew.SaveBoard()` because KiCad 10.0.4's Python bindings do not expose
`BOARD_STACKUP` for writing. The companion `.kicad_pro` carries four net
classes instead of one default: `Power` (≥0.5 mm track, closing M2's undersized
VBUS traces) for `VBUS_RAW`/`VBUS_5V`/`EXP_USBH_VBUS`, `USB90` (0.251 mm /
0.15 mm) for the device and host USB D+/D- pairs, and `Eth100` (0.201 mm /
0.15 mm) for the Ethernet MDI pairs — the corrected controlled-impedance
geometry from the plan's Recovery audit — with every other net staying on
`Default`; membership is assigned by `netclass_patterns` keyed off each net's
bare (sheet-prefix-stripped) name.

`aius-carrier.kicad_dru` carries two custom rules that `kicad-cli pcb drc`
enforces on both the canonical and every scaffolded board: the J1 GCT USB4105
land pattern's NPTH-to-copper clearance (0.20 mm minimum, scoped to J1's own
pads), and a blanket ban on signal tracks over the `In1.Cu` / `In2.Cu`
reference planes (`disallow track`) — both inner layers are `pcbnew.LT_POWER`
ground references, not routing layers, on every board this script emits.

[`scripts/test_scaffold.py`](scripts/test_scaffold.py) regresses the builder
end to end against a live netlist export: valid geometry/nets/DNP plus a
zero-violation `kicad-cli pcb drc` pass on the unrouted candidate (with a
non-zero unconnected-item count, since nothing is routed yet), rejection of a
missing footprint, a missing pad, an unplaced part, and a missing
`--netlist`/`--output`, confirms the two reference-plane design rules actually
fire when a track is added to `In1.Cu`/`In2.Cu`, and (importing the builder
module directly for its constants) that the saved board's `(stackup ...)`
block carries the expected four copper layers and per-layer thicknesses and
that the saved `.kicad_pro` declares all four net classes with the expected
width/clearance and at least one representative net assigned to each of
`Power`/`USB90`/`Eth100`. Run it the same way as any other `pcbnew` script:

```bash
scripts/kicad.sh --python electronics/scripts/test_scaffold.py
```

## Routing (H3c)

The routing pipeline runs as three pcbnew scripts against a board file, all
sharing a `--board <path>` argument that **defaults to the canonical
`electronics/aius-carrier.kicad_pcb`** and, when given a scaffolded
candidate instead, derives every sibling path (the DSN/SES export, and the
saved board itself) from that candidate's own directory and filename stem —
never from the canonical location. `--board` is optional; every command below
also runs with no arguments against the canonical board, unchanged from
before this flag existed.

```bash
# export the Specctra DSN for Freerouting (writes <board-stem>.dsn beside the board)
scripts/kicad.sh --python electronics/scripts/route_export_dsn.py \
  [--board <candidate.kicad_pcb>]

# Freerouting (headless, via the freerouting compose service) reads <board-stem>.dsn
# and writes <board-stem>.ses beside it — see compose.yaml for the invocation shape.

# import the routed session, apply fab-capable DRC minimums, fill GND planes
# (reads <board-stem>.ses beside the board; saves the board in place)
scripts/kicad.sh --python electronics/scripts/route_import_ses.py \
  [--board <candidate.kicad_pcb>]

# bounded post-route cleanup: widen necked tracks, solidify GND pad connections
# (saves the board in place)
scripts/kicad.sh --python electronics/scripts/route_cleanup.py \
  [--board <candidate.kicad_pcb>]
```

`route_export_dsn.py`'s Specctra export already marks the `In1.Cu`/`In2.Cu`
reference planes as DSN `(type power)` layers with `(plane GND ...)` entries,
distinct from the `(type signal)` `F.Cu`/`B.Cu` layers. `ExportSpecctraDSN`
reads the plane-layer type (`pcbnew.LT_POWER`, set by
[`scripts/scaffold_pcb.py`](scripts/scaffold_pcb.py)) straight off the board,
so no post-export DSN rewrite is needed to keep Freerouting off the planes.

## Fabrication export (H3d)

[`scripts/export_fab.sh`](scripts/export_fab.sh) runs a DRC report (non-fatal
here — the DRC gate is enforced separately before order) then exports the full
manufacturing package — Gerbers, drill, CPL, STEP, BOM — via `kicad-cli`.
Takes the same `--board <path>` argument as the routing scripts, defaulting to
the canonical board; the output directory (`<board-dir>/manufacturing/`) and
every exported filename's stem are derived from the given board's own
directory and filename, so a scaffolded candidate's package lands beside the
candidate rather than in `electronics/manufacturing/`.

```bash
electronics/scripts/export_fab.sh [--board <candidate.kicad_pcb>]
```

An unrouted candidate (e.g. straight out of `scaffold_pcb.py`, before
`route_import_ses.py`) still exports cleanly: DRC reports its unconnected-item
count without failing the script (`|| true` on the DRC step), and every other
export stage runs unconditionally against the board as given.

## Board (H3 recovery — regenerated and routed, 2026-09-11)

`aius-carrier.kicad_pcb` is regenerated from the netlist scaffold
(`scaffold_pcb.py` → `route_export_dsn.py` → Freerouting → `route_import_ses.py`
→ `route_cleanup.py`) and no longer carries the rejected 2026-08-16 autoroute.
The companion `aius-carrier.kicad_pro` keeps its schematic-side settings
(`sheets`, `schematic.top_level_sheets`, etc.) and gains the scaffold's four net
classes (`Power`/`USB90`/`Eth100`/`Default`) merged in rather than replaced
wholesale; a subsequent pcbnew save through the routing pipeline round-trips the
file to KiCad 10.0.4's full project schema (adds default keys such as
`rule_severities`, `teardrop_options`; drops nothing).

DRC against the fab rule set (`--severity-all`) is **0 violations, 17
unconnected items** — all on net `GND` (SMD/PTH pads and short GND stub tracks
that never reached a stitching via to the L2/L3 planes), matching Freerouting's
own report of 17 unrouted `GND` connections after 18 auto-route passes (started
147 unrouted, plateaued at score 890.79 with 4 in-router violations that do not
appear in the post-cleanup KiCad DRC). No track lands on `In1.Cu`/`In2.Cu` —
confirmed both by the `.kicad_dru` blanket ban (0 violations) and a direct
layer-membership check. `route_export_dsn.py`'s Specctra DSN correctly carries
each net's assigned class (`Power`/`USB90`/`Eth100`, not one collapsed
0.20 mm class): `grep '(class'`/`(rule (width` on the exported DSN shows
`Power,Default` at 500/150, `USB90,Default` at 251/150 and `Eth100,Default` at
201/150 (thousandths of a mm), plus `kicad_default` at 200/150 for everything
else — no fix was needed to the script.

The residual GND punch-list is the honest H3c stopping point (owner guidance:
scripted grind stops at diminishing returns, `route_cleanup.py` does not
reroute) — stitching-via placement for the listed pads is interactive-judgment
work for the next pass. The manufacturing package (`export_fab.sh`) exports
cleanly against the routed board (Gerbers, drill, CPL, STEP, BOM); this is a
review/quote snapshot, not order-ready. The owning plan is
[`aius-h2-h3-manufacturing.md`](../docs/plans/aius-h2-h3-manufacturing.md).
