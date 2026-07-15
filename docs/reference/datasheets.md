# Datasheet and CAD sources

Aius references manufacturer datasheets, drawings and CAD models for the parts
it uses. Those documents are third-party copyrighted material and are **not**
redistributed in this repository — this page links to each vendor source
instead. Retrieve any you need directly from the vendor.

Part numbers in the design are authoritative; where an archived document covered
a sibling part, that is noted.

## Optical (S/PDIF TOSLINK)

- **RS PRO ORJ-5 TOSLINK receiver** (RS stock `805-1671`) — https://uk.rs-online.com/web/p/fibre-optic-connectors/8051671
- **RS PRO OTJ-5 TOSLINK transmitter** (RS stock `805-1680`) — https://uk.rs-online.com/web/p/fibre-optic-connectors/8051680
- **Cliff Electronics FCR684205R** (ORJ-5 receiver) — https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf
- **Cliff Electronics FCR684205T** (OTJ-5 transmitter) — https://www.cliffuk.co.uk/products/optical/FCR684205T.pdf

STEP models for the RS PRO optical parts were obtained from TraceParts via the
RS product pages' CAD tab.

## S/PDIF routing

- **TI SN74LVC1G3157** SPDT analog switch (raw-bypass mux) — https://www.ti.com/lit/ds/symlink/sn74lvc1g3157.pdf
- **TI SN74LVC1G125** single 3-state buffer — https://www.ti.com/lit/ds/symlink/sn74lvc1g125.pdf
  (the reset-safe TX gate in the BOM specifies the active-high-enable sibling
  **SN74LVC1G126** — https://www.ti.com/lit/ds/symlink/sn74lvc1g126.pdf)

## Processor (Teensy 4.1 / i.MX RT1060)

- **PJRC Teensy 4.1 pinout card** — front https://www.pjrc.com/teensy/card11a_rev4_web.pdf , back https://www.pjrc.com/teensy/card11b_rev4_web.pdf
- **PJRC Teensy 4.1 schematic** — https://www.pjrc.com/teensy/schematic.html
- **NXP i.MX RT1060 datasheet** (`IMXRT1060CEC`, PJRC mirror) — https://www.pjrc.com/teensy/IMXRT1060CEC_rev4.pdf
- **NXP i.MX RT1060 reference manual** (`IMXRT1060RM`, PJRC-annotated) — https://www.pjrc.com/teensy/IMXRT1060RM_rev3_annotations.pdf

## Ethernet

- **PJRC Ethernet Kit for Teensy 4.1** — https://www.pjrc.com/store/ethernet_kit.html
- **Pulse Electronics J0011D21BNL** magjack (doc `J403`) — https://productfinder.pulseeng.com/products/datasheets/J403.pdf
- **Samtec MMS-103-01-L-DV** 2.00 mm socket (Ethernet socket) — https://www.samtec.com/products/mms-103-01-l-dv

## Teensy sockets / headers

- **Samtec SSW-124-02-G-S** side socket — https://www.samtec.com/products/ssw-124-02-g-s
- **Samtec SSW-105-02-G-S** USB-host socket — https://www.samtec.com/products/ssw-105-02-g-s
- **Samtec SSW series** (through-hole socket strip) — https://www.samtec.com/products/ssw
- **Samtec TSW series** (mating terminal strip) — https://www.samtec.com/products/tsw
- **Samtec TMM series** (mating terminal strip) — https://www.samtec.com/products/tmm

## Expansion headers

- **JST GH series** 1.25 mm connectors (all expansion headers) — https://www.jst-mfg.com/product/pdf/eng/eGH.pdf

## Power / protection / USB

- **TI TPS2553** current-limited power switch (USB + expansion) — https://www.ti.com/lit/ds/symlink/tps2553.pdf
- **TI TPD2EUSB30A** USB ESD protection — https://www.ti.com/lit/ds/symlink/tpd2eusb30a.pdf
- **TI TCA9517A** level-translating I2C repeater — https://www.ti.com/lit/ds/symlink/tca9517a.pdf
- **StarTech UUSBHAUB6IN** internal USB-A→Micro-B cable — https://www.startech.com/en-us/cables/uusbhaub6in

## Mechanical / test

- **Hammond 1455N1201BK** extruded enclosure (drawing + STEP) — https://www.hammfg.com/part/1455N1201BK
- **HellermannTyton 151-30300** (CTM0) cable-tie mount — https://www.hellermanntyton.com/products/cable-tie-mounts/ctm0/151-30300
- **Tag-Connect TC2030-IDC** programming/test cable — https://www.tag-connect.com/product/tc2030-idc-6-pin-no-legs
