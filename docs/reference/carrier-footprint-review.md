---
title: Carrier footprint coordinates and interface review
type: reference
scope: component
component: aius
status: current
updated: 2026-09-10
tags: [aius, pcb, footprints, mechanical]
---

# Carrier footprint coordinates and interface review

All coordinates below are **PCB component-side**, X right, Y down. Rotating a
footprint never repairs a reflected pad pattern. These corrections supersede
the estimated geometry and the 2026-08-16 review's assurances in the H2/H3 plan.
The physical parts/socket seating and panel assembly still require the pre-order
fit check. `electronics/scripts/check_footprints.py` checks library pad interfaces;
`--board` checks the actual board's embedded copies and changed net assignments.

## Teensy 4.1

Source: [PJRC dimensions](https://www.pjrc.com/teensy/dimensions.html), archived as
`docs/reference/datasheets.mdPJRC-TEENSY41-DIMENSIONS/dimensions_teensy41.png`; names from
the archived PJRC card11a/card11b revision 4. Downloaded 2026-09-09.

VIN is (0,0), the USB connector is left and microSD right. The VIN row is Y=0;
the GND/pin-0 row is Y=15.24. Columns advance 2.54 mm, ending at X=58.42.
The module body spans X=-1.27..59.69, Y=-1.27..16.51.

| Group | Component-side positions (mm) |
|---|---|
| USB host | +5V, D−, D+, GND, GND at X=6.35, 8.89, 11.43, 13.97, 16.51; Y=12.19 |
| Ethernet | RX+, LED, TX− at X=12.97; RX−, GND, TX+ at X=14.97; Y=2.45, 4.45, 6.45 |
| Service | On/Off, Program, GND, 3V3, VBAT at X=45.72; Y=2.54, 5.08, 7.62, 10.16, 12.70 |
| VUSB | (2.54, 2.54) |

Derivation: the Ethernet center column is X=13.97 and its middle row Y=4.45;
the 2 mm pitch extends ±1 mm in X and ±2 mm in Y. USB host is 3.05 mm above
the lower main row. Card11b mixes views; its USB-host and Ethernet photographs
show the component side. Do not mirror the coordinate table again.
The old footprint mirrored the main rows, reversed host D+/D−, and estimated
all auxiliary coordinates. Its apparent VBAT clearance issue was a wrong hole
location, not permission to nudge a socket hole. The courtyard extends left to
X=-3.10 to include the USB metal, not just the module PCB.

Drills follow the selected connector recommendations: SSW single-row 1.04 mm
([Samtec print](https://suddendocs.samtec.com/prints/ssw-s.pdf)), MMS-DV 0.79 mm
([Samtec footprint](https://suddendocs.samtec.com/prints/mms-1xx-01-xxx-dv-xxx-footprint.pdf)).
The service row uses the same 2.54 mm socket hole pattern; VUSB remains an
unpopulated optional hole. Mating hardware is not yet a released assembly BOM.

**Open assembly blocker:** the specified SSW body width is 2.41 mm and the
MMS-103 body length 6.56 mm, leaving a nominal 0.035 mm overlap between the
VIN-row socket and Ethernet socket (inner edges Y=1.205 and Y=1.170).
SSW and MMS bodies also have unequal heights, 8.51 and 4.45 mm respectively.
Socket bodies, mating pin lengths and tolerances must be resolved together;
moving electrically fixed Teensy holes cannot solve this. Sources:
[SSW catalog](https://suddendocs.samtec.com/catalog_english/ssw_th.pdf),
[MMS catalog](https://suddendocs.samtec.com/catalog_english/mms.pdf).
The coordinate checker does not clear this assembly blocker.

### Matched replacement candidate (2026-09-10, not yet adopted)

The owner permits a fresh bare Teensy and new mating hardware. A housing-free
candidate avoids the main, Ethernet and service-row plastic collisions:

| Board | Quantity | Mill-Max part |
|---|---:|---|
| Carrier | 64 | `0461-3-15-15-21-27-04-0` receptacle |
| Teensy | 64 | `4964-0-00-15-00-00-33-0` round nail-head pin |

VUSB stays unpopulated. Pin heads sit on the Teensy's top PCB surface.
The current manufacturer drawings are archived in
`evidence/source-snapshots/MILLMAX-TEENSY-SOCKETS/`:
[0461 drawing](https://www.mill-max.com/sites/default/files/external/products/fullsize/2023-03/0461.svg),
[4964 drawing](https://www.mill-max.com/sites/default/files/external/products/fullsize/2024-05/4964.svg).
Downloaded and visually reviewed 2026-09-10. Unlike the old catalog, the current
male drawing directly dimensions the **under-head length** as 0.345 inch; do not
subtract the head thickness from that dimension again.

Socket above-board height is 4.445 ±0.127 mm; male under-head length is
8.763 ±0.127 mm. Holding Teensy **top PCB surface** at 10.30 ±0.10 mm above
carrier top gives insertion `4.445 + 8.763 - 10.30 = 2.908 mm`, worst-case
2.554–3.262 mm. This leaves 0.243 mm beyond the deepest contact finger after
allowing a hemispherical tip of maximum pin radius, and 0.421 mm before the
shallowest cavity bottom. These are geometric margins, not mating qualification.

Proposed carrier finished holes are Ø0.70 ±0.05 mm, pads Ø1.40 mm. Minimum
hole-to-maximum-tail diameter clearance is 0.142 mm. Maximum socket diameter
1.0414 mm and pin-head diameter 1.3208 mm clear the 2 mm minimum pitch.
The current footprint still has the old socket drills and is **not** this candidate.

**Unresolved assembly condition:** discrete contacts do not establish board
separation. A permanent insulating spacing stop and soldering alignment fixture
are required. The owner is being asked whether a custom printed/machined spacer
is acceptable. Its support locations must clear Teensy underside parts and the
USB plug, and survive card insertion without loading the contacts. Also verify
actual Teensy hole/head fit, top-side solder access, configured-part availability
and sample mating. Do not release this candidate solely from the insertion sum.

## Optical ORJ-5 / OTJ-5

Sources: archived RS PRO `DATASHEET_805-1671.pdf` and `DATASHEET_805-1680.pdf`, p4;
[Cliff FCR684205R](https://www.cliffuk.co.uk/products/optical/FCR684205R.pdf),
2023-06-29 v2.0, package drawing p3. Both give the same mechanical datums.

Port faces −Y. Signal pins 1,2,3 are (-2.54,0), (0,0), (+2.54,0).
The front-view order 3,2,1 reverses when projected to PCB top view: a person
looking into the port from −Y has their right pointing toward PCB −X.
Receiver functions are Vout/GND/Vcc; transmitter functions are GND/Vcc/Vin.

The center peg is (0,-3.90), rear pegs (±4.60,+2.10), with 2.00 mm holes.
**6.00 mm is peg-row separation**, so the rear row is 6.00−3.90=2.10.
The housing front face is Y=-8.50, rear Y=+3.50, width 11.50 mm; the optical
nose reaches Y=-10.80. The optical axis is 6.00 mm above the PCB and the upper
mounting hole center 16.00 mm. The courtyard includes the nose.

## Pulse J0011D21BNL

Source: archived `https://productfinder.pulseeng.com/products/datasheets/J403.pdf`,
Pulse J403.L (11/23), p2 drawing B, p5 J0011 schematic and p6 LED configuration.
[Manufacturer copy](https://yageogroup.com/content/datasheet/asset/file/DATASHEET_J403).

Port faces −Y; pin8=(0,0). Even pins 8,6,4,2 run X=0,2.54,5.08,7.62 at Y=0.
Odd pins 7,5,3,1 run X=1.27,3.81,6.35,8.89 at Y=-2.54.

| Feature | X mm | Y mm |
|---|---:|---:|
| LED9 | 10.77 | -13.79 |
| LED10 | 8.23 | -12.27 |
| LED11 | 0.66 | -13.79 |
| LED12 | -1.88 | -12.27 |
| Posts | -1.27, 10.16 | -8.89 |
| Shield tabs | -3.43, 12.32 | -5.84 |

The connector axis is X=4.445. Housing face Y=-19.685 follows the 0.425 inch
post-to-face dimension. Maximum housing width/depth is 16.256×21.590 mm;
recommended panel opening 17.018×14.605 mm. Positions are converted from the
drawing's inch dimensions and rounded to 0.01 mm.

Electrical: pins1/2 are TX+/TX−, pins3/6 RX+/RX−, pins4/5 center taps, pin7 NC.
**Pin8 goes to PCB GND**, as the manufacturer explicitly specifies; it is the
return through the internal 1 nF isolation capacitor, not the physical shield.
SH remains CHASSIS. The shield domain is deliberately isolated from digital
GND; routing must not invent a net tie. LED9/10 are anode/cathode; LED12/11 are
anode/cathode. J0011 has no internal LED resistors, but the
[PJRC schematic](https://www.pjrc.com/teensy/schematic.html) already places R3,
150 ohms, between PHY LED0 and the exposed header LED signal.

## USB entry

TI SLVSAC2G, June 2021, [TPD2EUSB30A datasheet](https://www.ti.com/lit/ds/symlink/tpd2eusb30a.pdf),
archived under `docs/reference/datasheets.mdTI-TPD2EUSB30A/`, assigns pins1/2/3 to D+/D−/GND
in the DRT package. Use `Package_TO_SOT_SMD:Texas_DRT-3`, not SOT-23-3.

The [GCT USB4105 drawing](https://gct.co/files/drawings/usb4105.pdf), archived under
`docs/reference/datasheets.mdGCT-USB4105/` on 2026-09-09, matches the stock footprint's
pin layout. The schematic symbol stacks VBUS/GND pins; KiCad netlist export
expands them correctly. The existing board's eight power/ground pads were checked
and already have the correct nets; no speculative JSON pin duplication is needed.
