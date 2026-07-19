# Cross-sheet net convention

The Aius carrier schematic is a hierarchical KiCad project (`aius-carrier.kicad_sch`
root instantiating per-area sub-sheets). **Connectivity within a sheet is by local
net label; connectivity across sheets is by global label.** A net named in a
sheet spec's `global_nets` array is emitted as a KiCad global label by
`generate.py` and merges by name with the same global label on any other sheet;
every other net stays a sheet-local label. This file is the registry of the
global (cross-sheet) names so the optical-spdif and ethernet sheets connect to
the Teensy and the power tree by matching text.

## Global rails

| Net | Source | Consumers | Notes |
|---|---|---|---|
| `GND` | power-usb (`PWR_FLAG` on the USB-C ground) | every sheet | single digital ground; declared external at power entry |
| `VBUS_5V` | power-usb (`TPS2553` protected 5 V out) | teensy-expansion USB-host switch | protected-5V domain; the only 5 V rail leaving power-usb |
| `+3V3` | teensy-expansion (Teensy 3.3V pin, `power_out`) | optical, ethernet, expansion | `teensy-3v3`; the Teensy sources it, so no `PWR_FLAG` is needed |
| `CHASSIS` | enclosure (USB-C/USB-A shells) | power-usb shells, ethernet magjack shield | chassis-shield domain, isolated from `GND`; passive-only (no `PWR_FLAG`) |

`GND` and `VBUS_5V` originate on the power-usb sheet and are promoted to global
there (`power-usb.json` `global_nets`); `+3V3` originates on teensy-expansion.
`CHASSIS` is also promoted to global on power-usb so the ethernet magjack shield
joins the same chassis-shield domain as the USB shells. These rails are what the
later sheets attach to.

## Global Teensy signals

Consumed by the optical-spdif sheet:

| Net | Teensy pad | Role |
|---|---|---|
| `SPDIF_RX` | 15 | optical receiver → Teensy S/PDIF in |
| `SPDIF_TX` | 14 | Teensy S/PDIF out → TX gate |
| `PATH_SELECT` | 40 | raw-bypass mux select (10k pull-down = processed default) |
| `TX_ENABLE` | 39 | optical TX-gate enable (10k pull-down = disabled default) |

Consumed by the ethernet sheet (6-pin native-Ethernet header → magjack):

| Net | Teensy pad | Role |
|---|---|---|
| `ETH_RXP` | Ethernet RX+ | MDI receive pair + |
| `ETH_RXN` | Ethernet RX- | MDI receive pair - |
| `ETH_LED` | Ethernet LED | link/activity indication |
| `ETH_TXN` | Ethernet TX- | MDI transmit pair - |
| `ETH_TXP` | Ethernet TX+ | MDI transmit pair + |

Both sheets have landed: the optical-spdif sheet drives/consumes `SPDIF_RX`,
`SPDIF_TX`, `PATH_SELECT`, `TX_ENABLE`, and the ethernet sheet terminates the
five `ETH_*` MDI/LED globals at the magjack. Every one of these seven signal
globals now spans at least two pins, so the earlier `isolated_pin_label`
warnings are cleared; full-severity ERC leaves only provisional
`footprint_link_issues`.

## Design decisions recorded during capture

### Ethernet PHY is on the Teensy, not the carrier

**The DP83825 Ethernet PHY is on the Teensy 4.1 itself** (order the "with Ethernet
chip" variant), so the Teensy's 6-pin native-Ethernet header carries **MDI
differential pairs** (`ETH_RX±`, `ETH_TX±`) plus `LED` and `GND` straight to the
magjack. The `ETH_*` globals are therefore MDI signals, and the ethernet sheet
needs only the Pulse magjack + its 100 nF centre-tap capacitor — **no DP83825 and
no RMII on the carrier.**

This is evidence-backed and contradicts the H1 plan's H1b decision (add a
DP83825I to the carrier and replicate the "PJRC Ethernet Kit reference circuit"):

- `https://www.pjrc.com/teensy/card11b_rev4_web.pdf` labels the 6-pin
  Ethernet header `RX+ / RX- / LED / TX- / TX+ / GND` (MDI, post-PHY).
- `https://www.pjrc.com/store/ethernet_kit.html`: the
  kit is "an Ethernet MagJack, bare circuit board, ribbon cable assembly, 2 header
  pins, and a capacitor" — **no PHY chip**.
- `evidence/source-snapshots/PJRC-TEENSY41-PAGE/teensy41.html` sells the board in
  "With Ethernet Chip" / "Without Ethernet Chip" variants and lists Ethernet as
  "10/100 Mbit / DP83825 PHY / (6 pins)".

The BOM already matches this reality (magjack + centre-tap cap, no PHY line item).
**Resolved on the ethernet sheet:** no on-carrier DP83825 and no RMII; the Pulse
`J0011D21BNL` magjack MDI pairs wire straight to the `ETH_*` globals, its two
PHY-side centre taps join through the 100 nF `C30` to `GND`, and its shield +
CHS-ground tie to `CHASSIS`.

### One expansion power switch → USB-host VBUS

The interface doc names two protected expansion domains (`expansion-3v3` and
`expansion-usb-host-vbus`) but the BOM lists a single expansion `TPS2553`. It is
assigned here to the **USB-host VBUS** domain (`EXP_USBH_VBUS`, sourced from
`VBUS_5V`): a reverse-blocking current-limited switch, EN pulled to `+3V3` so the
port is enabled while the core is powered and off (safe) when the core is
unpowered.

`expansion-3v3` is realised as `+3V3` directly (the interface's "protected output
derived from teensy-3v3"), with per-signal backfeed protection instead of a rail
switch — matching each interface's own `protection_method`: the `TCA9517A`
isolator on I2C, and 1k series clamps on the SPI / UART / I2S / GPIO-ADC exported
CMOS signals. This keeps the "no expansion rail back-powers the core" guarantee
via bounded clamp current + isolation rather than a second switch.

## Sheet-local nets

Everything not listed above is sheet-local: the power-usb internals
(`VBUS_RAW`, `CC1/2`, `EN`, `FAULT`, `ILIM`, `USB_DP/DM`), the teensy-expansion
internals (the isolator A/B buses, series-clamp `*_T`/header pairs, `EXP_USBH_*`
control, `USBH_D±`, `PROGRAM`, `ON_OFF`, `VBAT`), the optical-spdif internals
(`MUX_OUT` mux→gate, `TX_OE_N` inverter→gate `~OE`, `TX_GATED` gate→OTJ5), and the
ethernet internal `ETH_CT` (joined magjack centre taps → `C30`). Promote a local
net to global only when a second sheet must reference it, and add it here.
