# Aius carrier electronics — generated schematic pipeline

The Aius carrier schematic is **generated from a checked-in connectivity spec**,
ERC-checked and rendered in the digest-pinned KiCad 10.0.4 container
(`../scripts/kicad.sh`). No GUI-only step is the sole way to reproduce it. This
directory implements the H1a milestone of
[`../docs/plans/aius-h1-schematic.md`](../docs/plans/aius-h1-schematic.md): the
`power-usb` sheet, proven ERC-clean and rendered.

## Pipeline

```
connectivity/<sheet>.json  --generate.py-->  aius-carrier.kicad_sch  --erc.sh-->  ERC gate
                                                                      --render.sh-> SVG + PDF
```

- **`connectivity/power-usb.json`** — the review surface. Every component (ref,
  `lib_id`, value/MPN, footprint) and every net (name → list of `[ref, pin]`
  endpoints), plus `no_connects` and human `design_notes` (EN default, ILIM
  value, FAULT pull-up, decoupling, shield domain).
- **`generate.py`** — spec → `.kicad_sch`. Connectivity is by **net label**: each
  symbol is placed on a grid, every pin gets a short wire stub and a local label
  carrying its net name; nets connect by matching label text. Pin endpoints are
  computed from the symbol pin geometry (KiCad symbol libs are Y-up, schematics
  Y-down, so a library pin at `(px, py)` lands at schematic `(X + px, Y - py)`;
  a pin's `angle` points tip→body, so the label direction is `angle + 180`).
- **`lib/aius.kicad_sym`** — custom symbols for parts KiCad does not ship:
  `TPD2EUSB30A` (3-pin shunt USB ESD) and `TPS2553` (current-limit load switch,
  EN active-high, open-drain FAULT).
- **`lib/standard-cache.kicad_sym`** — verbatim copies of the stock KiCad symbols
  the generator uses (`R`, `C`, `USB_C_Receptacle_USB2.0_16P`, `USB_A`,
  `PWR_FLAG`), taken from KiCad 10.0.4 (`/usr/share/kicad/symbols`, digest pinned
  in `../compose.yaml`). Lets the generator embed the `lib_symbols` cache and
  compute pin geometry without a live container.

## Commands

```bash
# regenerate the schematic from the spec
electronics/generate.py electronics/connectivity/power-usb.json electronics/aius-carrier.kicad_sch

# ERC gate (exit 0 = no errors); --all also prints warnings
electronics/scripts/erc.sh

# render SVG + PDF into this directory
electronics/scripts/render.sh
```

The KiCad container runs as uid 1000 and writes outputs into this directory, so
it must be writable by that uid (a normal checkout is; otherwise grant the dir
others-write — git does not track that bit). ERC reports are written inside the
container home volume; the textual summary and exit code are the result.

## ERC status

`power-usb` is **ERC-error-clean** (`--severity-error` exits 0). Full-severity
ERC leaves three non-electrical warnings, all expected at H1:

- 2× `lib_symbol_issues` "configuration does not include the symbol library
  'aius'" — a `kicad-cli` quirk (it ignores the project `sym-lib-table`); the
  symbols are embedded and fully resolved, and the GUI resolves `aius` via
  `sym-lib-table`.
- 1× `footprint_link_issues` for `J2` (TE `292303-1`) — a provisional footprint;
  land-pattern verification is H2, not H1.
