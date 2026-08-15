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

The KiCad container runs as uid 1000 and writes outputs into this directory, so
it must be writable by that uid (a normal checkout is; otherwise grant the dir
others-write — git does not track that bit). ERC reports are written inside the
container home volume; the textual summary and exit code are the result.

## ERC status

The whole project is **ERC-error-clean** (`scripts/erc.sh`, `--severity-error`
exits 0 — `Found 0 violations`). The error-clean gate also proves the global
rails merge across sheets: teensy-expansion's `GND` / `VBUS_5V` power-input pins
are driven by the power-usb `PWR_FLAG` / `TPS2553` output through the global
labels, `+3V3` is driven by the Teensy 3.3 V `power_out`, and the optical logic
`VCC` pins ride that same `+3V3`.

Full-severity ERC (`scripts/erc.sh --all`) leaves **5** `footprint_link_issues`
and nothing else, down from 13 after the H3a map-to-standard renames
([aius-h2-h3-manufacturing.md](../docs/plans/aius-h2-h3-manufacturing.md) §H3a).
The four expansion JST GH headers and the Tag-Connect now use stock KiCad
`Connector_JST:` / `Connector:` footprints, and the GCT `USB4105` USB-C resolves
in the pinned 10.0.4 container's `Connector_USB` library. The 5 remaining are the
four custom `aius:` land patterns still to build (`Cliff_ORJ5`, `Cliff_OTJ5`,
`Pulse_J0011D21BNL`, `Teensy41_Sockets_...` — awaiting `lib/aius.pretty` +
`fp-lib-table`) and the internal USB-A `J2` (`USB_A_TE_292303-1_Horizontal`,
absent from KiCad's `Connector_USB` — re-select or build-custom at H3a). The
optical mux/gate use real KiCad `SOT-23-5/6` footprints, so they add no warnings,
and the earlier `isolated_pin_label` warnings cleared once the optical-spdif and
ethernet sheets connected the seven cross-sheet signal globals.

The purchased optical modules (2026-07-27) are the RS PRO ORJ-5 / OTJ-5
(`805-1671` / `805-1680`), not the Cliff parts the `aius:Cliff_ORJ5_TOSLINK` /
`aius:Cliff_OTJ5_TOSLINK` footprints were drawn from — pin-compatible, but
validating the Gerber against the RS PRO layout drawings is a recorded H2 gate
([H1 plan §Milestones](../docs/plans/aius-h1-schematic.md#milestones)).
