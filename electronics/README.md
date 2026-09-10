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
  `[ref, pin]` endpoints), plus `no_connects`, `design_notes`, and a
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
socket assembly blocker are in
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

## Board (H3 recovery pending)

`aius-carrier.kicad_pcb` still contains the rejected 2026-08-16 autoroute: a
96 × 95 mm board with incorrect embedded footprints, signals through its ground
planes, uncoupled differential pairs and 15 DRC violations plus 3 unconnected
items. The corrected library does not automatically update those embedded copies.
The rebuild must establish enclosure datums, use the corrected interfaces, reserve
the reference planes and explicitly route the critical pairs. The owning plan is
[`aius-h2-h3-manufacturing.md`](../docs/plans/aius-h2-h3-manufacturing.md).
