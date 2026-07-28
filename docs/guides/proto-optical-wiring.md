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

**Every horizontal run below is a direct wire. The capacitors hang BETWEEN two
runs (shunt); nothing is ever in series with a supply or a signal.** A bypass
capacitor in series with Vcc blocks DC entirely and leaves the module
unpowered — a previous revision of this diagram was readable exactly that way
and produced a dark, deaf bench build (2026-07-27); the module survives it
unharmed, but nothing works until the cap moves to shunt.

Receiver:

```
ORJ-5 pin 1 (Vout) ──┬──────────── Teensy pad 15 (SPDIF_IN)
                 22–30 pF
ORJ-5 pin 2 (GND)  ──┴──┬───────── Teensy GND
                     100 nF
ORJ-5 pin 3 (Vcc)  ─────┴───────── Teensy 3V3
```

Transmitter:

```
OTJ-5 pin 1 (GND)  ─────┬───────── Teensy GND
                     100 nF
OTJ-5 pin 2 (Vcc)  ─────┴───────── Teensy 3V3
OTJ-5 pin 3 (Vin)  ─────────────── Teensy pad 14 (SPDIF_OUT)
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

- **the firmware transmit gate substitutes for the electronic one** (the
  carrier-management phase in the prototype plan): the output stays dark
  unless the input carrier is locked or a replay is running, actuated at
  pad 14's mux, observable as `tx_gate` on `GET /api/status`. What firmware
  cannot close: an unavoidable **~0.1–2 ms silence-carrier blip** at static
  construction (~300 ms after every reset, before `setup()` runs), with the
  pad undriven before that. Harmless into a bench sink; the carrier board's
  electronic default closes even that window;
- a crashed-mid-open firmware leaves the pad routed until the watchdog
  resets it (bounded 8 s) — again a bench-only difference;
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

## Troubleshooting with only a DC multimeter

Worked out during the 2026-07-27/28 bench bring-up; every expected value below
was derived from the datasheets plus the firmware's gate behaviour
(`tx_gate_actuate`: gate open muxes pad 14 to SPDIF_OUT, gate closed drives it
low as GPIO — so a closed gate reads a firm 0.00 V, never floating).

**A steady red glow at the TX only proves the LED has power — not that data
reaches it.** The OTJ-5's V_IH is 2.0 V: a drive whose high level sags below
that leaves the LED in one state, emitting *constant* light. A receiver
staring at constant light holds a *constant* Vout (≈2.5 V is a normal static
high for its output stage; spec V_OH is only ≥ 2.4 V) and nothing ever locks.
Light without lock therefore does not clear the TX side.

The S/PDIF peripheral streams biphase continuously whenever the gate is open
(silence still modulates), so a healthy drive reads mid-rail at all times:

| Meter point | Healthy | Meaning of a deviation |
|---|---|---|
| Any Vcc pin to its GND pin | 3.3 V | Supply joint, or a bypass cap wired in series (see §Wiring) |
| OTJ-5 pin 3 (Vin) to pin 1, gate open | ~1.4–1.8 V unsteady | Flat 0/3.3 V: no data arriving; ~0.5 V: weak or asymmetric drive |
| Teensy pad 14 direct, gate open | ~1.4–1.8 V | Same low value as at the jack: pad driver suspect; higher than jack: resistive joint between |
| Teensy pad 14 direct, gate closed | 0.00 V firm | Anything else: mux/actuation not switching |
| ORJ-5 pin 1 (Vout) to pin 2, fibre out | steady high (~2.5–3.3 V) | — |
| ORJ-5 pin 1 (Vout) to pin 2, live carrier in | reading CHANGES vs fibre-out | Same both ways: no modulation is being decoded (upstream, seating, or module) |

Cheap DC meters misread 6 MHz streams (the true average is ~1.65 V; readings
skew), so judge by *contrast* between states — fibre in vs out, gate open vs
closed — rather than absolute numbers. The firmware can toggle the gate on
demand (start/stop a replay over HTTP) while a meter sits on the pad: the
0.00 V closed state is a built-in reference edge.

Findings from this test belong in
[`signal-path.md`](../reference/signal-path.md) (measured-on-glass notes) and
the prototype plan's test-matrix records.
