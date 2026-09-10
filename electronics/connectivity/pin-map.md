# Teensy 4.1 pin allocation map

The exact Teensy 4.1 pad each carrier net uses, conflict-checked against the
PJRC pinout card (`https://www.pjrc.com/teensy/pinout.html`, rev 4). This is a
primary H1 review artifact: **no pad serves two functions.** Pads are given as
the Teensy pad number (edge pins) or the PJRC pad label (power / USB-host /
Ethernet / service pads). Realised in `connectivity/teensy-expansion.json`
(symbol `aius:Teensy41`); cross-sheet net names are defined in
[`nets.md`](nets.md).

## Allocated pads

| Net | Teensy pad | Function (PJRC) | Notes |
|---|---|---|---|
| `SPDIF_RX` | 15 | A1 / RX3 / SPDIF_IN | fixed by spec; to optical mux (optical sheet) |
| `SPDIF_TX` | 14 | A0 / TX3 / SPDIF_OUT | fixed by spec; to TX gate (optical sheet) |
| `I2C_SCL_T` | 19 | A5 / SCL0 | I2C clock, isolator A-side, 4.7k pull-up |
| `I2C_SDA_T` | 18 | A4 / SDA0 | I2C data, isolator A-side, 4.7k pull-up |
| `SPI_SCK` | 27 | SCK1 | SPI1 clock, 1k series → J11 |
| `SPI_MOSI` | 26 | MOSI1 | SPI1 MOSI, 1k series → J11 |
| `SPI_MISO` | 1 | MISO1 | SPI1 MISO, 1k series → J11 |
| `SPI_CS` | 38 | A14 / CS1 | SPI1 chip-select, 1k series → J11 |
| `UART_TX` | 29 | TX7 | debug UART TX, 1k series → J12 / Tag-Connect |
| `UART_RX` | 28 | RX7 | debug UART RX, 1k series → J12 / Tag-Connect |
| `I2S_BCLK` | 21 | BCLK1 | I2S bit clock, 1k series → J13 |
| `I2S_LRCLK` | 20 | LRCLK1 | I2S word clock, 1k series → J13 |
| `I2S_TXD` | 7 | OUT1A (I2S1 TX) | I2S data out, 1k series → J13 |
| `I2S_RXD` | 8 | IN1 (I2S1 RX) | I2S data in, 1k series → J13 |
| `PATH_SELECT` | 40 | A16 / GPIO | mux select; 10k pull-down = processed default |
| `TX_ENABLE` | 39 | A15 / GPIO | TX-gate enable; 10k pull-down = disabled default |
| `EXP_GPIO` | 41 | A17 / GPIO | general expansion GPIO, 1k series → J15 |
| `EXP_ADC` | 24 | A10 / ADC | expansion analog input, 1k series → J15 |
| `USBH_DP` | USB Host D+ | USBHS2 D+ | USB-host socket (Samtec SSW-105) → J14 |
| `USBH_DM` | USB Host D- | USBHS2 D- | USB-host socket → J14 |
| `EXP_USBH_VBUS` | USB Host 5V | protected host VBUS output | native onboard TPD3S014 → J14; no parallel carrier supply; 500 mA service budget |
| `ETH_RXP` | Ethernet RX+ | MDI receive + | 6-pin native-Ethernet header → magjack |
| `ETH_RXN` | Ethernet RX- | MDI receive - | (ethernet sheet) |
| `ETH_LED` | Ethernet LED | link/activity LED | |
| `ETH_TXN` | Ethernet TX- | MDI transmit - | |
| `ETH_TXP` | Ethernet TX+ | MDI transmit + | |
| `PROGRAM` | Program | bootloader/program | Program/RTC header J16 + Tag-Connect J17 |
| `ON_OFF` | On/Off | power on-off / reset | Tag-Connect J17 |
| `VBAT` | VBAT | RTC coin-cell backup | via J16; PWR_FLAG-declared external source |
| `+3V3` | 3.3V | 3.3V out (250 mA max) | `teensy-3v3` rail **source** (power_out) |
| `GND` | GND, USBH_GND, ETH_GND | digital ground | common ground; USB-host + Ethernet GND merged |

## Reserved / not-fitted pads

- **`VIN` — no-connect.** No second VIN feed is fitted (single power entry).
- **`VUSB` — no-connect.** The Teensy draws 5 V through its own micro-B jack from
  the internal USB-A cable (power-usb sheet), the sole VUSB feed; the carrier
  socket does not re-feed VUSB.
- **Pad 13 (on-board LED, shared with `SCK0`) — reserved, no-connect.** The status
  LED is on the Teensy itself. SPI is deliberately allocated to **SPI1**
  (`SCK1`=27 / `MOSI1`=26 / `MISO1`=1 / `CS1`=38) so pad 13 is never also an SPI
  clock — this is why the map has no `SCK0` collision with the LED.
- **microSD / SDIO (pads 42–54) — not routed.** The `microsd-access` interface is
  mechanical access to the Teensy's on-board microSD socket; no carrier net.
- **`MCLK1` (pad 23) — not exported.** The expansion I2S header carries
  BCLK/LRCLK/data only; no master clock pin is broken out.
- **Unallocated edge pads** (0, 2–6, 9–12, 16, 17, 22, 25, 30–37) remain free for
  future expansion.

## Conflict check

Every allocated edge pad number (1, 7, 8, 14, 15, 18–21, 24, 26–29, 38–41) is
distinct, and each service/power/USB-host/Ethernet pad label is used once. The
build enforces this indirectly: a pad wired to two nets in the spec would give
ERC a short (two labels on one pin) — the whole-project ERC is error-clean.
