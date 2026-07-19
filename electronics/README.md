# Aius carrier electronics — generated schematic pipeline

The Aius carrier schematic is **generated from a checked-in connectivity spec**,
ERC-checked and rendered in the digest-pinned KiCad 10.0.4 container
(`../scripts/kicad.sh`). No GUI-only step is the sole way to reproduce it. This
directory implements the H1 foundation of
[`../docs/plans/aius-h1-schematic.md`](../docs/plans/aius-h1-schematic.md): a
**hierarchical multi-sheet project** whose root `aius-carrier.kicad_sch`
instantiates one sub-sheet per functional area. Present and ERC-clean today: the
`power-usb` sheet (H1a) and the `teensy-expansion` sheet (Teensy 4.1, expansion /
service / test interfaces). The `optical-spdif` and `ethernet` sheets are later
H1b milestones and attach via the global-label net convention below.

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
- **`lib/aius.kicad_sym`** — custom symbols for parts KiCad does not ship:
  `TPD2EUSB30A` (3-pin shunt USB ESD), `TPS2553` (current-limit load switch),
  `Teensy41` (the socketed module, drawn as the carrier-used pads with correct
  electrical types), `TCA9517A` (I2C level-translating buffer/isolator), and the
  generic `Conn_1x02..06` connectors (JST GH headers, pin headers, Tag-Connect).
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
labels, and `+3V3` is driven by the Teensy 3.3 V `power_out`.

Full-severity ERC (`scripts/erc.sh --all`) leaves 17 non-electrical warnings, all
expected at this milestone:

- 7× `isolated_pin_label` ("label connected to only one pin") — the cross-sheet
  signal globals `SPDIF_RX`, `SPDIF_TX`, `ETH_RXP/RXN/LED/TXN/TXP` terminate on
  the Teensy alone until the optical-spdif and ethernet sheets land. They clear
  when those sheets connect. See [`connectivity/nets.md`](connectivity/nets.md).
- 10× `footprint_link_issues` — provisional footprints (the power-usb `J2` TE
  `292303-1`, plus the custom `aius:` land patterns for the Teensy sockets, JST
  GH headers and Tag-Connect). Land-pattern verification is H2, not H1.
