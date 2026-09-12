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
must be rebuilt before its routing can be accepted. Board mode also asserts
pad-number -> net-name identity for every J20 (Pulse magjack) and J1 (USB-C)
pin the connectivity JSON defines a net or `no_connects` entry for, parsing
`connectivity/ethernet.json` / `connectivity/power-usb.json` directly rather
than hardcoding a second copy of the mapping (bare, sheet-prefix-stripped net
names, matching `scripts/scaffold_pcb.py`'s own net-class matching); a pin
present on the footprint but absent from both tables (e.g. the magjack's
floating pin 7, or the USB-C receptacle's stacked VBUS/GND mirror pins) is
printed as `UNCOVERED` rather than asserted against an invented expectation.

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

The routing pipeline runs as four pcbnew scripts against a board file, all
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

# import the routed session, apply fab-capable DRC minimums, tighten GND
# zone clearance/thermal-relief gap, fill GND planes
# (reads <board-stem>.ses beside the board; saves the board in place)
scripts/kicad.sh --python electronics/scripts/route_import_ses.py \
  [--board <candidate.kicad_pcb>]

# bounded post-route cleanup: widen necked tracks, solidify GND pad connections
# (saves the board in place)
scripts/kicad.sh --python electronics/scripts/route_cleanup.py \
  [--board <candidate.kicad_pcb>]

# stitch every remaining isolated GND island (SMD pads/stub tracks Freerouting
# never tied to a via) to the filled In1/In2 planes; refills zones and saves
# the board in place
scripts/kicad.sh --python electronics/scripts/route_gnd_stitch.py \
  [--board <candidate.kicad_pcb>]
```

`route_export_dsn.py`'s Specctra export already marks the `In1.Cu`/`In2.Cu`
reference planes as DSN `(type power)` layers with `(plane GND ...)` entries,
distinct from the `(type signal)` `F.Cu`/`B.Cu` layers. `ExportSpecctraDSN`
reads the plane-layer type (`pcbnew.LT_POWER`, set by
[`scripts/scaffold_pcb.py`](scripts/scaffold_pcb.py)) straight off the board,
so no post-export DSN rewrite is needed to keep Freerouting off the planes.

[`scripts/route_import_ses.py`](scripts/route_import_ses.py) sets every GND
zone's local clearance and thermal-relief gap to 0.25 mm (spoke width stays
0.5 mm, a fabricable solder joint) before filling — KiCad's 0.5/0.5 mm zone
defaults merge the anti-pads on the Teensy 2.54 mm pin rows (a 1.7 mm pad's
keepout ring grows by the clearance on every side, and two neighbouring
pads' keepouts overlap well before DRC's 0.127 mm minimum would require),
slotting the In1.Cu/In2.Cu return planes under those rows. Measured on a
scratch re-route: `In1.Cu` copper along the two Teensy pin-row columns
(x = 59.0 mm, x = 74.24 mm) was 91.5 % / 99.6 % void at the 0.5 mm defaults
and 78.5 % / 86.0 % void at 0.25 mm — the residual void is the pin pads'
own footprint area, not adjustable via zone settings.
[`scripts/scaffold_pcb.py`](scripts/scaffold_pcb.py)'s own zone creation sets
no clearance/thermal-relief parameters (`SetLayer`/`SetNetCode`/
`SetIsFilled(False)` only), so those values come entirely from `pcbnew`'s
`ZONE` class defaults, not from the scaffold — nothing to change there.

[`scripts/route_gnd_stitch.py`](scripts/route_gnd_stitch.py) closes the gap
`route_cleanup.py` cannot: a GND pad's automatic zone bond only fires for a
PTH pad that itself spans the `In1.Cu`/`In2.Cu` planes, so an SMD GND pad (or
a short GND stub track Freerouting routes between neighbouring SMD pads
without ever reaching a via) stays its own electrically isolated island. The
script groups every GND pad/track/zone into connectivity islands, and for
each island not already touching the filled planes either drops a via at a
non-pad kink point on that island's existing stub-track chain (reusing the
routed copper, no new track) or, for a bare pad with no routed copper at all,
adds a via beside it plus a short Default-class connecting track — searching
a widening ring of candidate offsets around the anchor and validating the
whole prospective via-plus-track path's exact geometry (not axis-aligned
bounding boxes, which false-positive against a long diagonal track) against
every other net's copper and every drilled hole's clearance before committing
it. A via never lands inside a pad. Any GND island still left as bare track
with no pad at all after stitching (a stub with nothing left to reach) is
deleted rather than left dangling.

Four further passes close gaps invisible to the island walk above, because
the affected pad's island is already bonded to the plane through *someone
else's* via: every SMD GND pad still more than 1.0 mm from any via gets one
(joined by a short connecting track); the J1 GND pad group specifically gets
a second via (its four GND pads collapse to two physical solder-pad
locations, and one location was observed reaching the plane only through
the other's via, 6+ mm away); each USB/Ethernet differential pair at
J1/J2/J18/J20 gets a flanking GND via within 1.5 mm of its connector
break-in (the H2b return-path argument — stitching vias flanking each
pair's connector break-in), searched beside the pair rather than between
its two pads; and any Power-class (≥0.5 mm track) net riding a single
0.3 mm-drill via gets a parallel same-net via, bridged on both copper
layers (not just the layer the search anchored on), so no supply path
depends on one plated hole. On a scratch re-route of the canonical board's
known punch-list (37 SMD GND pads with no nearby via, J1's four GND pads on
one via, VBUS_RAW's two single-via hops, zero stitching vias flanking any
differential pair): the script added 20 of 27 SMD-pad vias (7 residual,
blocked by genuine congestion — dense connector pin fields at J1/J2, or the
isolated CHASSIS domain routing within clearance of the GND pad), 3 of 6
differential-pair flanking vias (J1 and one J20 pair residual, again
connector-pin-field congestion), and a parallel via for one of VBUS_RAW's
two single-via hops (the other found no collision-free site and stays a
residual); `kicad-cli pcb drc --severity-all` on the result is 0
violations, 0 unconnected items.

[`scripts/route_diff_pairs.py`](scripts/route_diff_pairs.py) is the H3e
diff-pair fix: it rips all eight USB/Ethernet pair nets' existing copper and
re-routes each pair as two genuinely coupled polylines at the netclass width
and a 0.15 mm edge gap (not the independent single-ended traces Freerouting
produces), length-matched to <=0.5 mm skew and locked (`SetLocked(True)`) so
a later Freerouting pass treats them as fixed — confirmed by inspecting the
DSN `route_export_dsn.py` exports afterward, where a locked track or via
prints `(type fix)` instead of `(type route)`. Every segment is straight
with every bend a multiple of 45 degrees; a real `kicad-cli`-visible
geometry check (not just this script's own sampling) against both this
script's own output and the full pipeline's Freerouting-produced output
found 0 tracks off that grid and 0 arcs. It reads every pad position from
the board itself (never hardcoded mm coordinates), rips any foreign F.Cu
copper its planned corridor would otherwise violate clearance against, and
avoids every other foreign pad it would otherwise clip (a growing lateral
detour into the nearest corridor segment, per-side so P also dodges N's own
destination pad and vice versa).

Rotating U1 (the device-USB ESD shunt, a symmetric chip with no directional
meaning) 180 degrees in `scaffold_pcb.py`'s `PLACEMENT` lines its D+/D- pad
order up with both J1's A6/A7 mirror pins and J2's fixed pad order, so the
whole USB_DP/USB_DM J1-to-U1-to-J2 chain now routes at 0 vias — down from
the two disclosed via pairs this pair needed before. Two crossings remain
genuinely forced by fixed connector/cable-pin topology, not by placement:
J1's own B6/A7/A6/B7 USB-C mirror-pin interleaving (inherent to the
connector's reversible design), and ETH_RXP/ETH_RXN's corridor to J18 (the
"1 RX+, 2 RX-, ... 6 TX+" cable-pin contract fixes ETH_TX and ETH_RX to
*opposite* handedness conventions at J18's own pin field — measured
directly off pad positions — so no J18 rotation clears RX without breaking
TX's own already-0-via route). Both print a `DISCLOSED EXCEPTION` line.
USB host and Ethernet TX route at the specified 0 vias. J2 (the USB-A jack)
may be relocated/rotated freely per an design decision, but the U1 rotation
above already clears the order reversal without touching it. Residual
`kicad-cli pcb drc` findings on the full pipeline (35 violations / 7
unconnected) concentrate in the same small set of genuinely congested
footprints this script's own retry loops report as bounded — U1's GND pin
within ~1 mm of its own D+/D- pins, J1's and J2's dense multi-pin rows,
U10's UHDM lead, C30's centre-tap pad near the ETH_RX crossing, and the
ETH_RXN via/pad hole-to-hole at J18 — reported rather than chased through
further constant-tuning, the same "bounded iterations, stop at diminishing
returns" stopping point as the GND punch-list above.

### Routing-geometry gate (H3f)

[`scripts/check_routing_geometry.py`](scripts/check_routing_geometry.py) enforces
the Owner ruling in
[aius-h2-h3-manufacturing.md](../docs/plans/aius-h2-h3-manufacturing.md)
§Owner rulings (2026-09-12): every track is straight segments only, no curves
or arcs, with bends at multiples of 15 degrees and straight sections between.
It inspects `board.GetTracks()` only -- zone fills and footprint graphics are
separate board containers, never visited -- and reports any `PCB_ARC` and any
segment or bend angle off the 15-degree grid (exit 1 on either), plus
sub-0.2 mm straight runs between bends and off-grid bend pairs as report-only
findings that never affect the exit code.

```bash
scripts/kicad.sh --python electronics/scripts/check_routing_geometry.py [--board <candidate.kicad_pcb>]
scripts/kicad.sh --python electronics/scripts/check_routing_geometry.py --board <candidate.kicad_pcb> --fix
```

Run against the routed canonical board (`f1b4168`): 0 arcs, 0 off-grid-angle,
0 off-grid-bend (Freerouting's own 45/90-degree mitred output already sits on
the 15-degree grid), 33 sub-0.2 mm report-only short segments. Run against the
H3e diff-pair rework scratch candidate
(`.scratch/diffpair/aius-carrier.kicad_pcb`, `route_diff_pairs.py`'s coupled-pair
output layered on the Freerouting base): 8 off-grid-angle findings (all
~7.0-7.5 degrees off the nearest 15-degree axis, on `USBH_DP`/`USBH_DM`/
`ETH_TXP`/`ETH_TXN`/`USB_DP`) plus 13 derived off-grid-bend findings and 39
short-segment notes -- confirming `route_diff_pairs.py`'s true-mitre offset
joins do not stay on the 15-degree grid the Owner ruling requires.

`--fix` tried all 8 off-grid-angle findings on the diff-pair candidate and
snapped none: 4 sit at a junction shared with another track (both endpoints
occupied, no free point to rotate), and the other 4 would need 1.3-1.4 mm of
endpoint movement to correct a several-degree deviation over an 11+ mm run,
far past the 0.05 mm move budget -- both classes are reported, not silently
dropped, and neither the canonical board's own `kicad-cli pcb drc
--severity-all` violation count nor the geometry findings changed as a result
(0 endpoints moved). `--fix` is a bounded snap, not a re-router: closing these
findings needs `route_diff_pairs.py`'s own centerline construction to bend on
the 15-degree grid from the start, not a post-hoc nudge.

## Fabrication export (H3d)

[`scripts/export_fab.sh`](scripts/export_fab.sh) runs a DRC report, then a DFM
report against the JLCPCB 4-layer capability rule set, then the
routing-geometry gate (§Routing-geometry gate above — all three non-fatal
here, every gate is enforced separately before order), then exports the full
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
`route_import_ses.py`) still exports cleanly: DRC, DFM, and the
routing-geometry gate report their violation/finding counts without failing
the script (`|| true` on all three steps), and every other export stage runs
unconditionally against the
board as given.

### DFM gate (JLCPCB 4-layer)

[`scripts/check_dfm.sh`](scripts/check_dfm.sh) runs `kicad-cli pcb drc
--severity-all --all-track-errors` against a fab's DFM capability rule set
(`electronics/dfm/<fab>.kicad_dru`, default `jlcpcb-4l`), on a disposable
scratch copy of the given board — never the canonical project files by
default. It concatenates the fab overlay with the board's own
`.kicad_dru` (KiCad loads one `.kicad_dru` per project; the fab overlay goes
first so the canonical board's own narrower, documented per-footprint
exceptions, e.g. J1's adapted-land-pattern NPTH clearance, still win), and
elevates the DFM-relevant rule severities (`silk_over_copper`,
`solder_mask_bridge`, `copper_edge_clearance`, `annular_width`,
`drill_out_of_range`, `hole_clearance`, `hole_to_hole`, `courtyards_overlap`,
footprint/library-mismatch checks) to `error` in the scratch project's own
copy, since the canonical project may keep some of them at `warning` for
day-to-day routing review. It also sets the fab's minimum solder-mask bridge
(sliver) width on the scratch board's own setup block — KiCad has no
`.kicad_dru` constraint for mask-web geometry.

```bash
electronics/scripts/check_dfm.sh [--board <path>] [--fab jlcpcb-4l] [--fix]
electronics/scripts/check_dfm.sh --apply-fix [--board <path>]
```

`--fix` applies the safe DFM fixes to the scratch copy before running DRC:
silkscreen reference/value text stroke width raised to the fab's minimum
line width; per-footprint solder-mask-margin overrides from the data table
[`electronics/dfm/overrides.json`](dfm/overrides.json) (`{"<ref>":
{"solder_mask_margin_mm": <mm>, "why": "<one-line rationale>"}}` — e.g. U11's
fine-pitch mask-bridge margin, without touching the board-global
`pad_to_mask_clearance` every other pad relies on); then a zone refill
(`ZONE_FILLER`, as `route_import_ses.py` does) against the freshly loaded
board, so a `.kicad_dru` hole-clearance rule (parsed at board load, e.g. the
canonical board's `npth-plane` rule) is honoured by the filler — KiCad's
zone filler consults custom design rules directly when computing the
keep-out around a pad or hole, not just the board's global design-settings
floor. `--apply-fix` applies the same three fixes directly, in place, to the
board named by `--board`, with no DRC run. Both are idempotent: re-running
against an already-fixed board bumps 0 silk texts and applies 0 overrides
(the refill always runs, but against unchanged geometry and settings it
produces the same fill).

**2026-09-11 JLCPCB 4L run against the routed board: 90 violations before the
silk fix, 31 after** (59 `text_thickness`, 26 `hole_clearance`, 5
`solder_mask_bridge`; the silk fix clears all 59 `text_thickness`). The
remaining 31 needed an owner decision, not a mechanical fix, at the time:

- **26 `hole_clearance`** — the NPTH mounting holes of J1, U20, U24, J17, J20
  sit 0.2505 mm from the In1.Cu/In2.Cu GND plane fill, 0.0035 mm short of
  JLCPCB's 0.254 mm minimum. Raising the board's global hole-clearance design
  *floor* and re-filling zones did not move this specific gap (verified by
  hand: identical 0.2505 mm actual before and after a floor bump to 0.26 mm).
  The fix that did work: a scoped `.kicad_dru` rule (`npth-plane`,
  `hole_clearance (min 0.3mm)`, condition `A.Pad_Type == 'NPTH, mechanical'`,
  ordered before the existing J1 exception so J1's own narrower relaxation
  still wins inside J1) plus a zone refill against the freshly loaded board —
  the custom rule is what the filler actually consults, not the
  design-settings floor. `check_dfm.sh --apply-fix` / `--fix` now performs
  that refill as its third step.
- **5 `solder_mask_bridge`** — U11's fine-pitch adjacent pads (0.65 mm pitch)
  leave less than JLCPCB's 0.2 mm minimum solder-mask web at
  `pad_to_mask_clearance = 0`. Fixed via the `overrides.json` per-footprint
  table: a `-0.03 mm` local solder-mask margin on U11 (TI TCA9517ADGKR,
  `Package_SO:VSSOP-8_3x3mm_P0.65mm`) widens the web to 0.21 mm without
  touching the board-global mask-expansion setting (`-0.02 mm` was tried and
  does not clear it).

**2026-09-12 proof, scratch copy of `f1b4168` + the current working-tree
`aius-carrier.kicad_dru` (carrying the `npth-plane` rule) + the U11
`overrides.json` entry:** `check_dfm.sh --apply-fix` then `check_dfm.sh` →
**DFM 0 violations**; base `kicad-cli pcb drc --severity-all` → **0
violations, 0 unconnected items**; `check_footprints.py --board` →
**158/158**; a second `--apply-fix` run reports **0 silk texts bumped, 0
overrides applied** (zone refill still runs, unchanged geometry).

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

DRC against the fab rule set (`--severity-all`) is now **0 violations, 0
unconnected items** at `7aed113`, after `route_gnd_stitch.py` closed the
17-item GND punch-list this section previously reported (17 new stitching
vias, 11 connecting tracks). The board carries 578 tracks and 73 vias, and
0 tracks land on `In1.Cu`/`In2.Cu` — confirmed both by the `.kicad_dru`
blanket ban (0 violations) and a direct layer-membership check.
`route_export_dsn.py`'s Specctra DSN correctly carries each net's assigned
class (`Power`/`USB90`/`Eth100`, not one collapsed 0.20 mm class):
`grep '(class'`/`(rule (width` on the exported DSN shows `Power,Default` at
500/150, `USB90,Default` at 251/150 and `Eth100,Default` at 201/150
(thousandths of a mm), plus `kicad_default` at 200/150 for everything else —
no fix was needed to the script.

DRC-clean does not mean order-ready: the H3e second review pass (2026-09-11)
measured the routed geometry directly and found every differential pair
single-ended rather than coupled, and both GND planes heavily slotted by the
0.5 mm zone clearance — neither shows up in a connectivity-only DRC pass. The
fixes in progress for both are tracked in the owning plan's H3e section.

The manufacturing package (`export_fab.sh`) exports cleanly against the routed
board (Gerbers, drill, CPL, STEP, BOM); this is a review/quote snapshot, not
order-ready. The owning plan is
[`aius-h2-h3-manufacturing.md`](../docs/plans/aius-h2-h3-manufacturing.md).
