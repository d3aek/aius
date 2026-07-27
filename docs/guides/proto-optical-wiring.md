---
title: Proto-board optical wiring — first real signal test
type: guides
scope: component
component: aius
status: current
tags: [aius, hardware, optical-spdif, toslink, bring-up]
updated: 2026-07-27
---

# Proto-board optical wiring — first real signal test

How to wire the purchased RS PRO optical jacks directly to the running Teensy
4.1 on a breadboard/proto board, and what to observe for the first-ever real
S/PDIF signal into the device. This is the bench shape only — the carrier
board adds a bypass mux and a reset-safe TX gate that the bench deliberately
omits (see [§What the bench omits](#what-the-bench-omits)).

## Parts

| Part | Role | Datasheet |
|---|---|---|
| RS PRO ORJ-5 (RS `805-1671`) | TOSLINK receiver → Teensy S/PDIF in | `https://uk.rs-online.com/web/p/fibre-optic-connectors/8051671` |
| RS PRO OTJ-5 (RS `805-1680`) | Teensy S/PDIF out → TOSLINK transmitter | `https://uk.rs-online.com/web/p/fibre-optic-connectors/8051680` |
| 2 × 100 nF ceramic | Vcc bypass, one per jack, **within 7 mm of the jack pins** (datasheet precaution 1) | |
| 1 × 30 pF ceramic | GND-to-Vout on the receiver, "avoid loading effect" (receiver precaution 2). **Value uncritical: 15–100 pF all work** (33 pF is the nearest standard value; 22/47 pF equally fine — **the bench build fits 22 pF**). **Omittable on the bench** with short leads — the test's own signals (lock, `blocks` climbing, zero error counters) reveal ringing if it bites. Never substitute ≥ 1 nF: that smears the ≤25 ns edges into the ~81 ns half-cell and breaks lock. The carrier board keeps the designed 30 pF (C21) regardless. | |
| TOSLINK cables + a real S/PDIF source (TV, DAC, console) | | |

Both jacks run from the Teensy's 3.3 V rail: Vcc range is 2.7–5.5 V, dissipation
is ≤10 mA each (typ. 6.5 mA RX + 5.5 mA TX), far inside the Teensy 3V3 pad's
250 mA budget. The pins are on 2.54 mm pitch and fit a breadboard directly; the
two Ø2 mm pegs overhang a breadboard's edge or sit in a proto-board's peg holes.

**ESD caution (datasheet precaution 3): both modules are ESD-sensitive.**
Handle by the body, touch ground first, wire everything with the power off.

## Pinout — the two jacks are NOT the same order

Pin functions from the RS PRO datasheets (page 4 of each), which match the
schematic symbols in `electronics/connectivity/optical-spdif.json` exactly.
The datasheets number the three signal pins 3-2-1 left-to-right on the
rear-view drawing (the view with the Ø2.50 peg hole top-right); they carry no
front-view numbering, so **identify pin 1 from that drawing with the jack in
hand before powering** — miswiring Vcc onto the receiver's Vout is the one
mistake this table exists to prevent.

| Pin | ORJ-5 receiver (805-1671) | OTJ-5 transmitter (805-1680) |
|---|---|---|
| 1 | **Vout** (signal out) | **GND** |
| 2 | GND | Vcc (+3V3) |
| 3 | Vcc (+3V3) | **Vin** (signal in) |

## Wiring

```
ORJ-5 pin 1 (Vout) ──────────────── Teensy pad 15  (SPDIF_IN)
ORJ-5 pin 2 (GND)  ──┬───────────── Teensy GND
ORJ-5 pin 3 (Vcc)  ──┼── 100 nF ──┐
                     │            ├─ Teensy 3V3
OTJ-5 pin 2 (Vcc)  ──┼── 100 nF ──┘
OTJ-5 pin 1 (GND)  ──┤
OTJ-5 pin 3 (Vin)  ──┼───────────── Teensy pad 14  (SPDIF_OUT)
                     │
     30 pF between ORJ-5 pin 1 (Vout) and pin 2 (GND)
```

- Teensy pads 15/14 are the spec-fixed `SPDIF_IN`/`SPDIF_OUT` pads
  ([pin map](../../electronics/connectivity/pin-map.md)).
- **Keep the Vout → pad 15 and pad 14 → Vin leads short (a few cm) and run a
  ground lead alongside each.** The S/PDIF bitstream is ~6 MHz with 25 ns
  edges (16 Mbps-rated modules); long unpaired breadboard jumpers are the
  first thing to suspect on lock/carrier errors.
- Each 100 nF sits within 7 mm of its jack's Vcc pin, not at the Teensy end.
- Levels need no conversion: receiver V_OH ≥ 2.4 V drives the Teensy's 3.3 V
  CMOS input directly, and the Teensy's 3.3 V output is well above the
  transmitter's V_IH ≥ 2.0 V.

## What the bench omits

On the carrier the receiver feeds a bypass mux (`SN74LVC1G3157`) and the
transmitter sits behind a reset-safe gate (`SN74LVC1G126`) so a booting or
crashed Teensy can never emit data ([optical sheet](../../electronics/connectivity/optical-spdif.json)
§design_notes). The bench wires both jacks **directly**, so:

- the TX may emit an optical carrier from the moment the S/PDIF peripheral
  starts, including during boot — harmless into a bench sink, but it is a
  difference from the carrier's contract, not a bug to "fix" later;
- there is no raw-bypass path; the raw/processed mux tests in the prototype
  plan's test matrix are carrier-only.

## First real signal test

The firmware side is already proven against replay; this is the first time
`blocks` climbs from a *carrier* rather than a replay. Board reachable at
`http://192.0.2.66` as usual.

1. **Electrical bring-up, fibre unplugged:** power on; check 3.3 V at both
   jacks' Vcc pins; ORJ-5 Vout idles at a steady logic level with no fibre.
   `GET /api/status` should show the familiar no-carrier state (`blocks` 0).
2. **Carrier:** connect a PCM source (TV/DAC at 48 kHz) into the ORJ-5.
   **`blocks` on `GET /api/status` must climb continuously from passthrough
   alone with no replay running**, `overruns` staying at or near 0 — the
   exact observation the plan's "Resume here" section has been waiting on.
   `GET /api/meters` publishes non-zero input levels (`sequence` > 0).
3. **Carrier-present refusal:** with the carrier live, `POST
   /api/replays/start` must refuse `409 replay_carrier_present`. This is the
   refusal's first hardware exercise; `stop_reason:"carrier"` stays reachable
   only by unplugging mid-replay.
4. **TX out:** connect the OTJ-5 to a sink (soundbar/DAC); it should lock to
   48 kHz stereo PCM carrying the processed downmix.
5. **Glass loopback (optional but the payoff):** patch OTJ-5 → ORJ-5 with a
   short TOSLINK cable and run the item 3 measurement flow
   (`firmware/tools/measure_vectors/measure_run.py`). Item 3's figures were
   measured on the internal software tap; this repeats them across real
   glass — receiver jitter and all. Expect THD+N at the 16-bit stimulus
   floor as before; a large deviation implicates the optical path, not the
   DSP.
6. **Carrier loss/recovery:** pull the input fibre mid-stream; `blocks`
   freezes and the carrier-loss counters increment once per event; replug
   and confirm recovery.

Findings from this test belong in
[`signal-path.md`](../reference/signal-path.md) (measured-on-glass notes) and
the prototype plan's test-matrix records.
