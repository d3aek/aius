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

# render schematic SVG (one per page) + PDF, plus PCB SVGs/PNGs, into this directory
electronics/scripts/render.sh
```

The KiCad wrapper runs as the invoking user and writes outputs into this
checkout. Direct Compose invocations should set `AIUS_UID` and `AIUS_GID` to the
caller's IDs (defaults are 1000). ERC reports are written to `.scratch/`;
the textual summary and exit code are the result.

`electronics/scripts/render.sh` also renders the canonical
`aius-carrier.kicad_pcb`: `aius-carrier-pcb-top.svg` /
`aius-carrier-pcb-bottom.svg` (2D `kicad-cli pcb export svg`, page sized to
the board — top is `F.Cu`/`F.SilkS`/`F.Mask`/`F.Fab`/`Edge.Cuts`, bottom is
`B.Cu`/`B.SilkS`/`B.Mask`/`Edge.Cuts` mirrored), `aius-carrier-pcb-in1.svg` /
`aius-carrier-pcb-in2.svg` (the `In1.Cu`/`In2.Cu` reference planes plus
`Edge.Cuts`, for routing review), and `aius-carrier-pcb-top.png` /
`aius-carrier-pcb-bottom.png` (3D raytraced `kicad-cli pcb render`, 1600x1200,
default preset — the container renders headlessly without a GPU; a run that
fails is skipped with a warning rather than failing the script). Missing 3D
models on unpurchased parts are acceptable render warnings, not errors.

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
placement + connectivity scaffold that H3c routes, not a routed board. It also
emits three no-copper/no-footprint keepout rule areas per the H3e owner
rulings (2026-09-12): a 1.5 mm band along both long edges (the Hammond
1455N1201BK slot faces at `x=0` and `x=BOARD_W`) and an internal-USB-cable
envelope from J2's own connector opening toward the Teensy micro-B port on
U10 (a 16x12 mm plug-body rectangle plus a 10 mm-wide corridor, both F.Cu-only
since the cable runs above the board's top side) — see `_usb_cable_envelope_polygon`
and the `USB_CABLE_*` constants. Run from the Aius root, inside the
container:

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
`Power`/`USB90`/`Eth100`, and that the saved board carries exactly the three
H3e keepout rule areas (both edge bands on every copper layer at their exact
bounds, the USB cable envelope F.Cu-only and enclosing J2's own plug-body
rectangle while staying clear of U10's own already-placed footprint). Run it
the same way as any other `pcbnew` script:

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

**H3h DSN footprint-keepout fix (2026-09-12)** closed a Freerouting-only
residual: `I2S_TXD_T`/`I2S_RXD_T` (R28/R29 to U10 pins 7/8) stayed unrouted
across the whole pipeline even though `scaffold_pcb.py`'s USB cable envelope
rule area forbids footprints only (`DoNotAllowTracks`/`DoNotAllowVias` both
`False` — the cable rests on solder mask, so only parts must avoid it).
`ExportSpecctraDSN` writes every rule area as a plain DSN `(keepout ...)`
block regardless of its own per-item flags — confirmed by grepping the
exported DSN, where the envelope (its polygon on `F.Cu` only, unlike the two
all-layer edge keepouts which repeat their polygon on every copper layer)
landed as an ordinary `(keepout "" (polygon F.Cu ...))`, indistinguishable
from a copper-forbidding one — so Freerouting always treated it as a
track/via keepout too. The fix detaches every
footprint-only rule area (`DoNotAllowTracks`/`DoNotAllowVias` both `False`)
from the in-memory board copy the script builds for export, right before
calling `ExportSpecctraDSN`; the board on disk is never saved by this script,
so the canonical/candidate `.kicad_pcb` keeps the rule area (still enforced
by `kicad-cli pcb drc` against footprint placement) unchanged. Against a
fresh `scaffold_pcb.py` → `route_diff_pairs.py` → `route_export_dsn.py` →
Freerouting `-mp 40` → `route_import_ses.py` → `route_cleanup.py` →
`route_gnd_stitch.py` run: both I2S nets show full connectivity in
Freerouting's own unrouted-connections report and in `kicad-cli pcb drc
--severity-all`'s `unconnected_items`.

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

**H3h parallel-via bridging fix (2026-09-12)** closed a `via_dangling`
residual on the Power-class redundancy pass above: the B.Cu bridging track
between a Power-class via and its new parallel via used the plain
`_path_collides` check, which has no exclusion at all, so it always reported
a collision at the two samples landing exactly on the vias' own centers (the
unconditional hole-to-hole check `_colliders` runs against every via yields
any via whose center lies within its own drill radius of the sample point) —
the B.Cu bridge was therefore never added, leaving each new parallel via
connected on F.Cu only, a genuine DRC finding rather than a false positive.
The dedicated `_via_bridge_collides` check excludes both vias' own keys at
every sample, including the endpoints, and a via whose bridge still
genuinely collides with foreign copper is now removed (with its F.Cu
connecting track) rather than left dangling. On a fresh full-pipeline
candidate: `via_dangling` went from 2 (both on `VBUS_RAW`'s parallel vias) to
0, with no new violation introduced (`clearance`/`shorting_items`/
`solder_mask_bridge` unchanged, all pre-existing `route_diff_pairs.py`
residuals).

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

**H3i scope ruling (2026-09-12):** `--pairs` (comma-separated slugs;
default `usb-device`) scopes which pairs this script rips and re-routes,
hand-routing only the USB device pair by default and leaving USB host and
both Ethernet pairs' existing copper untouched. This follows the owner's
relaxed accepted-pair contract (nominal class widths, intra-pair skew
<=5 mm, vias and uncoupled sections allowed wherever DRC-clean), under
which Freerouting's own USB host and Ethernet results (0.15, 2.24 and
3.37 mm skew) already stand without hand-routing. `--measure` separately
reports P/N length, skew, via count and layers for all four pairs from the
board exactly as currently routed, touching no copper, independent of the
`--pairs` selection.

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
above already clears the order reversal without touching it.

**H3e placement pass 2 (2026-09-12)** moved U1 further from J1 (`y=10.0` to
`y=13.0`, leaving ~4.5 mm for the J1 mirror-pin bridge detour instead of
~1.5 mm), re-aligned J2 onto J1/U1's own D+/D- centreline (`x=43.0` to
`x=45.0`), and moved C30 (the ETH_CT centre-tap cap) out of the J20-to-J18
TX/RX corridor entirely (`(88.0, 23.0)` to `(93.5, 26.0)`) — plus the three
keepout rule areas above. Rotating J18 90/270 degrees to split its TX/RX rows
into separate x-corridors was tried and reverted: either rotation collapses
one pair's own P/N pads onto identical x (a 2-row header has no orientation
keeping both rows non-degenerate), which `route_diff_pairs.py`'s handedness
check cannot route through at all (crashes rather than routing with a
disclosed exception); J18 stays unrotated, since TX already routes at 0 vias
there. Net effect on the full pipeline (`scaffold_pcb.py` → `route_diff_pairs.py`
→ `route_export_dsn.py` → Freerouting `-mp 20` → `route_import_ses.py` →
`route_cleanup.py` → `route_gnd_stitch.py`): **29 violations / 6 unconnected**,
down from 35/7 — every finding involving C30 is gone, and the residual
concentrates entirely in the same three genuinely-congested, connector-fixed
spots this script's own retry loops already report as bounded: J1's own
B6/B7 mirror-pin bridge via against its A6/A7/CC1 neighbours (`hole_clearance`
0.18–0.22 mm vs the 0.25 mm floor, unchanged by placement since the bridge's
1 mm span is entirely J1's own row geometry), U1's GND pin against its own
D+/D- pins (`shorting_items`, an SOT-23-3-class chip's own pad
layout — U1's rotation already faces GND away from J1, per the placement
comment above, but does not increase the GND-to-D+/D- pad-to-pad distance
itself), and ETH_TXN/ETH_TXP's own self-clearance plus ETH_RXN's via
hole-to-hole at J18 (0.372 mm vs the 0.4495 mm floor — J18's 2 mm pin pitch,
unaffected by any component this pass moved). `check_routing_geometry.py`
stays 0 arcs / 0 off-grid, `check_footprints.py --board` stays 158/158, and
`check_dfm.sh --apply-fix` then `check_dfm.sh` reports 51 DFM violations, all
in the same three clusters at the DFM rule set's stricter thresholds — no new
cluster introduced by this pass. A scoped `.scratch/placement2/final/`
candidate (board, `.kicad_pro`, `.kicad_dru`, `fp-lib-table`, DRC/DFM/geometry
reports) carries this result; it is not the canonical board.

**H3e via-clearance pass (2026-09-12)** re-ran `route_diff_pairs.py` against
that same `.scratch/placement2/final/` candidate after fixing the script's
own bridge-crossing geometry: the mirror-pin bridge's disclosed via pair now
detours P (the net staying on F.Cu) to a *deeper* bow than N (the net
carrying the via) — P's own bow is unavoidably centred on N's destination
pad (J1's exact 0.5 mm pin pitch), so P has to clear N's crossing geometry
the same way it clears a row pad, not just sit at the same depth (an equal
-depth bow measured a literal `kicad-cli pcb drc`-visible copper overlap,
independent of how deep both went together). `kicad-cli pcb drc
--severity-all` against the resulting candidate: **16 violations / 6
unconnected** (down from 29/6; both counts exclude the 6
`lib_footprint_issues` findings present in both runs, unrelated to this
script's own net geometry). The J1 bridge via's `hole_clearance` against
A5/A6/B6 (0.18-0.22 mm vs the 0.25 mm floor) and ETH_RXN's crossing-via
`hole_to_hole` against J18 (0.372 mm vs 0.4495 mm, fixed via a corrected
`MIN_VIA_STANDOFF_FROM_PAD_MM`) are both fully closed. Two residuals remain
disclosed rather than chased further per this script's own H3c "bounded
iterations" convention: the same bridge via's own crossing now falls
~0.18-0.20 mm short of clearing the coupled P net's track (the same 0.5 mm
pin pitch that centres P's bow on N's own pad also caps the best-achievable
via-to-track separation at ~0.50 mm against the ~0.5755 mm a standard
0.6/0.3 mm via really needs), and it lands close enough to a pre-existing
Freerouting-placed TX_ENABLE track on B.Cu to short against it (this
script's own foreign-copper rip pass only ever tracks F.Cu candidates, so a
B.Cu collision is never seen or ripped, only found by DRC). U1's own GND-pin
clearance and ETH_TXN/ETH_TXP's own self-clearance are unchanged from the
prior pass — several exit-vector approaches were tried for U1 (see
`PAD_ROW_EXIT_DIRECTION`'s own comment) and reverted: `_exit_point`'s
straight-line displacement only holds for a short inset before `_build_leg`
re-converges onto the raw pad-to-pad centreline, and U1's GND pin sits far
enough along the leg that the re-convergence undoes the shift before it
clears the pin, matching the existing retry loop's own convergence with no
improvement at any vector tried.

**H3g U1 fan-through (2026-09-12)** closes the U1 GND-vs-D+/D- `shorting_items`
residual from the H3e passes above: `route_diff_pairs.py`'s
`_route_usb_device_pair` now treats U1 as a straight electrical pass-through
rather than an ordinary `_build_leg` hop, since U1 sits on the exact same x
centreline as J1's and J2's own D+/D- midpoints (measured: 45.0 mm on all
three). It fans the coupled USB90 pair out to U1's own pin 1/2 pitch (0.70 mm,
read off the board every run — not the 0.95 mm first assumed), each line's
centreline running straight through its own pad, widens further to 0.90 mm
past pin 3 (GND, opposite side; `padwidth + 2*0.15 + 0.251` rounded up to a
0.05 mm step) for 0.175 mm of measured clearance on both edges, then fans back
to the coupled pitch — 0 vias, an uncoupled span of 1.549 mm (inside the
recipe's 3.5 mm ESD-fan-through allowance). Against a fresh `scaffold_pcb.py`
candidate run through the full pipeline (`scaffold_pcb.py` → `route_diff_pairs.py`
→ `route_export_dsn.py` → Freerouting `-mp 40` → `route_import_ses.py` →
`route_cleanup.py` → `route_gnd_stitch.py`), `kicad-cli pcb drc --severity-all`
reports **14 violations / 3 unconnected** (down from 16/6): U1's own
GND-vs-D+/D- `shorting_items` finding is gone, and `USB_DP`/`USB_DM` show full
connectivity from J1 through U1 to J2. `check_routing_geometry.py` stays 0
arcs / 0 off-grid; `check_footprints.py --board` stays 158/158. Every residual
violation is pre-existing and untouched by this pass: the J1 mirror-pin
bridge via's own crossing still falls ~0.12-0.15 mm short of clearing the
coupled P track (see the H3e via-clearance pass above), ETH_TXN/ETH_TXP's own
self-clearance, U10's UHDM lead against USBH_DP/USBH_DM, and two
`route_gnd_stitch.py` VBUS_RAW parallel vias left single-layer by design. A
scoped `.scratch/pairs4/final/` candidate (board, `.kicad_pro`, `.kicad_dru`,
`fp-lib-table`, DRC/geometry/footprint reports) carries this result; it is
not the canonical board.

**H3h J1 via-free mirror-pin bridge, "Recipe C" (2026-09-12)** replaces J1's
own via-based bridge (`_route_bridge_pair`/`_route_bridge_pair_avoiding_
self_cross`, both removed) with two independent, genuinely via-free ties:
D+'s bridge (A6↔B6) dips to the *board* side (+Y), D-'s bridge (B7↔A7) dips
to the *connector* side (-Y) — `_route_j1_via_free_bridge`, reusing
`_detour_around_pad_row`'s existing bow shape. Putting the two dips on
opposite sides of the row means neither bridge's own copper ever enters the
other's territory (each only reaches ~1.025 mm past the row on its own
side), so the B6/A7/A6/B7 interleaving that forced the old bridge's own
via never gets the chance to — measured 0 vias for both ties, real pad
centres read off the board (B6 44.25, A7 44.75, A6 45.25, B7 45.75, all at
y=7.355; tips at y=6.78/7.93): the D- dip's flat clears A6's own
connector-side tip by ≈0.32 mm, the D+ dip's flat clears A7's own
board-side tip by ≈0.32 mm, both above the 0.15 mm netclass gap + 0.1255 mm
track half-width (0.2755 mm) floor.

This does not make the *whole* USB device pair via-free, though — A6's own
bridge, going above the row, encloses A7 on three sides (its own two "legs"
plus the flat top, all real F.Cu copper), a fact confirmed the hard way: a
plain vertical A7 exit, a 45-degree sideways dodge either direction, and a
B.Cu dip at every via size and standoff swept all landed a real `kicad-cli
pcb drc` `clearance`/`hole_clearance`/`hole_to_hole` finding against either
the bridge itself or J1's own 0.5 mm-pitch neighbours (A6/B6) — the
enclosure's own inner width is provably narrower than twice the clearance
any board-legal via needs from either wall. A7 is already bonded to B7 by
its own via-free bridge, so `_route_usb_device_pair`'s own leg to U1 now
launches from B7 instead, at the cost of the same kind of disclosed via
pair (`_route_crossing_leg_avoiding_self_cross`) ETH_RXP/ETH_RXN already
needs, for the same underlying reason: B7 sits on the *wrong* side of the
coupled pair for this corridor (J1's own interleaving again), and swapping
which side a signal enters that corridor from does not change that this
connector's physical order reversal must cross somewhere in the plane.
`J1_CROSSING_VIA_STANDOFF_MM` (an SMD-sized standoff for `_route_crossing_
leg`'s own transition via, swept empirically at 0.1/0.2/0.5 mm) narrows but
does not close a residual clearance shortfall against A6 at this specific,
extremely tight pitch (measured ≈0.04–0.05 mm short of the 0.15 mm floor at
the best value found, 0.2 mm) — disclosed rather than chased further per
this module's own H3c "bounded iterations" convention.

Net result on `.scratch/pairs5/final/` (`route_diff_pairs.py` + zone refill
+ `kicad-cli pcb drc --severity-all`, not the full downstream pipeline):
USB device now shows 2 vias (down from a single via *pair* before, but
relocated from the bridge itself to the B7 leg) and a **different** small
set of residual findings, all against J1's own A6 pad and a nearby
B.Cu-only `VBUS_RAW` stitch track/via this script's own F.Cu-only rip pass
never sees (same disclosed limitation the pre-existing TX_ENABLE B.Cu
residual already documented). ETH_TXP/ETH_TXN's own self-clearance
(0.047 mm) and USBH_DP/USBH_DM vs U10's UHDM lead (0.10 mm) are unchanged
residuals from before this pass — not addressed by it.

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

**H3e third-pass silkscreen tidy (2026-09-12)** closes the reviewed
reference-vs-neighbour collisions: [`scripts/tidy_silk_refs.py`](scripts/tidy_silk_refs.py)
relocates a violating `F.SilkS` reference to the first collision-free position
among eight fixed offsets around its own courtyard, touching only position/
rotation, never text size or stroke. Against the canonical board it moved 9
references (`C20`, `C21`, `C22`, `C25`, `R17`, `C1`, `C14`, `R4`, plus a
genuine `U2`-vs-`C1` text overlap this pass also caught) with 0 residuals,
`kicad-cli pcb drc --severity-all` and `check_dfm.sh` stay 0/0, and
`check_routing_geometry.py` is unchanged since no copper is touched.

## Fabrication export (H3d)

[`scripts/export_fab.sh`](scripts/export_fab.sh) runs a DRC report, then a DFM
report against the JLCPCB 4-layer capability rule set, then the
routing-geometry gate (§Routing-geometry gate above — all three non-fatal
here, every gate is enforced separately before order), then exports the full
manufacturing package — Gerbers, a fabrication drawing PDF, drill, CPL, STEP,
BOM — via `kicad-cli`. Takes the same `--board <path>` argument as the
routing scripts, defaulting to the canonical board; the output directory
(`<board-dir>/manufacturing/`) and every exported filename's stem are derived
from the given board's own directory and filename, so a scaffolded
candidate's package lands beside the candidate rather than in
`electronics/manufacturing/`.

```bash
electronics/scripts/export_fab.sh [--board <candidate.kicad_pcb>]
```

[`scripts/fab_drawing.py`](scripts/fab_drawing.py), run right after the
Gerber step, renders `<board-stem>-fab-drawing.pdf`
(`Edge.Cuts` + `User.Drawings` + `F.Fab`). It shows the dimensioned board
outline, the two Hammond slot-edge keepout bands and the rear (`y = 0`) datum
called out in place, and a notes block covering the 4-layer JLC04161H-7628
stackup, the 2026-09-12 controlled-impedance tolerance ruling, min
trace/space and via, NPTH count/diameters, solder mask/silk colour, and
nominal board thickness. Every value is read from the board itself or its
sidecar `.kicad_pro` / [`feasibility/stackups.json`](feasibility/stackups.json)
rather than hardcoded, on a disposable scratch copy it never saves back over
the canonical/candidate board.

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

## Board (H3g U1 fan-through, 2026-09-12)

`aius-carrier.kicad_pcb` regenerates from the netlist scaffold at `d3a38a5` via
the routing pipeline, closing the U1-pin-3 GND residual the prior pass left open.
USB device routes hand-scripted through U1 at 0 vias. USB host, ETH RX and ETH TX
stand on Freerouting's own result (skew 3.058/3.368/2.238 mm, within the 5 mm
accepted-pair contract). Gates: `kicad-cli pcb drc --severity-all` **0/0**,
`check_dfm.sh` **0**, `check_routing_geometry.py` **0 arcs/0 off-grid**,
`check_footprints.py --board` **158/158**, `export_fab.sh` clean. Plan:
[aius-h2-h3-manufacturing.md](../docs/plans/aius-h2-h3-manufacturing.md).
