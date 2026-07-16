---
title: Aius hardware follow-on
type: plans
scope: aius
status: draft
tags: [aius, hardware, pcb, teensy, spdif, ethernet]
updated: 2026-07-15
toc: true
---

# Aius hardware follow-on

Design and validate a three-unit Teensy 4.1 carrier that receives optical DTS Core or stereo PCM, performs the Aius software processing, and emits 48 kHz stereo PCM over optical S/PDIF to the existing DAC. The user supplies the Teensy used for capability testing and performs rear-panel machining by hand or through friends with CNC capability.

This plan follows the [DTS capability gate](aius-capability-gate.md). It is committed before that gate runs so a positive result has a decision-complete hardware path and a negative result preserves the intended product topology for processor reassessment. The [cost model](aius-cost-model.md) tracks both two-layer and four-layer outcomes.

## Locked product topology

The signal path is:

`TOSLINK RX -> S/PDIF capture -> IEC 61937/PCM classification -> DTS Core decoder -> native-channel PCM -> custom downmix/EQ -> 48 kHz stereo PCM S/PDIF -> gated TOSLINK TX`

The carrier contains:

- socketed Teensy 4.1;
- integrated Cliff-class TOSLINK receiver and transmitter;
- GPIO-controlled optical-transmitter enable, disabled by hardware during reset;
- diagnostic raw S/PDIF bypass mux with a hardware default that prevents raw DTS reaching the stereo DAC;
- native Teensy Ethernet connection to an integrated magjack;
- rear PCB-mounted USB-C device receptacle with CC pull-downs and ESD protection;
- a short controlled 90 ohm D+/D- route to an adjacent internal PCB-mounted USB-A receptacle;
- one standard approximately 100 mm USB-A male to Micro-B male cable carrying both data and the sole 5 V supply to the Teensy;
- keyed expansion for I2C display/encoder/buttons/LEDs, SPI with two chip-selects, UART/debug, I2S/TDM, USB host, GPIO/ADC, and a possible trigger/relay output;
- access to microSD, Program, status LED, and a remote Program/RTC header;
- test points and a manufacturing/bring-up interface.

There is one power source. The carrier receives 5 V from rear USB-C and passes that same rail to the Teensy through the internal USB cable. No second VIN or VUSB feed is permitted.

## PCB and mechanical decision

Use a two-layer PCB by default and escalate to four layers only when the H2 feasibility gate records a failed criterion.

The two-layer contract is:

- target outline no larger than 100 x 100 mm, subject to the official Hammond enclosure drawing and guide-slot geometry;
- standard 1.6 mm material only if the enclosure guides accept it;
- USB and Ethernet pairs routed on the top layer without vias;
- impedance geometry calculated from PCBWay's actual stackup, targeting 90 ohm differential USB and 100 ohm differential Ethernet;
- grounded coplanar routing where the stackup requires it;
- no high-speed pair crossing a ground split, plane neck, connector-return discontinuity, or layer change;
- bottom copper retained as an effectively continuous ground plane;
- low-speed signals routed primarily on top;
- zero-ohm links used in preference to cutting the ground return;
- USB-C and internal USB-A kept adjacent;
- Ethernet magjack kept immediately local to the Teensy Ethernet pins;
- all connector ESD paths returned to the intended chassis/shield network without crossing the signal return path.

Two layers reduce bare-board cost materially, especially if the board remains inside the 100 x 100 mm prototype tier. The cost benefit does not override USB or Ethernet integrity. H2 records the actual stackup calculation and routing proof before the layer count is final.

The stock enclosure is Hammond 1455N1201BK. The front panel remains blank. Rear-panel machining is excluded from vendor work and project costing. CAD-only fit validation retains a recorded residual risk until the first physical assembly.

## Quantity and manufacturing boundary

- Target three assembled carrier boards.
- Accept a vendor minimum of five bare PCBs; surplus boards remain unpopulated spares.
- Unit one is the bring-up and rework board.
- Unit two is the regression and A/B board.
- Unit three remains untouched until unit one passes power and interface checks, then becomes the spare or destructive-test board.
- PCBWay turnkey mixed assembly is the initial manufacturing assumption.
- Teensy modules are customer-supplied or separately priced and are not part of the headline carrier cost.
- No order occurs until H2 passes, H3 produces an actual quote, and the updated landed estimate is accepted.

## H0: Requirements, evidence, and architecture gate

H0 is implemented by P1A through P1E in the normative [P1 evidence, topology,
and cost implementation plan](aius-p1-evidence-topology-cost.md). That contract
adds the connector/control and expansion inventories, staged source handoff,
machine-checked schemas, and committed checkpoints. Its P1E checkpoint completes
H0; the original single-checkpoint outline below is retained as outcome history
and is not executed separately.

Complete the functional topology, connector and control inventory, power and clock domains, operating/fault states, pin/resource map, expansion map, preliminary placement, reference provenance, functional BOM, and risk register.

All saved research artifacts live below `docs/reference/datasheets.md` and follow the evidence and Git LFS policy in the capability plan. Reference/evidence schematics are explicitly not manufacturing schematics.

```yaml
phase: H0
unit: documentation-contract
seams: [product topology, electrical evidence, functional BOM, risk register]
contract-artifact: spec-only
fan-out:
  - name: requirements
    owned-paths: [docs/reference]
  - name: evidence
    owned-paths:
      - docs/reference/datasheets.md
      - evidence/manifest
  - name: cost-baseline
    owned-paths: [evidence/cost]
review-intensity: active
commit-checkpoint:
  green: [source audit, topology audit, cost arithmetic, risk review]
  buildable: [locked carrier architecture]
  parent-commit: "docs(aius): lock carrier architecture"
```

H1 cannot start until the software gate's P7 phase passes. H0 evidence work may proceed earlier because it does not commit manufacturing effort.

## H1: Electrical system schematic

Create the manufacturing schematic from the audited sources. It includes:

- optical receiver and transmitter logic levels, current, timing, filtering, and decoupling;
- S/PDIF RX/TX routing, diagnostic mux, output buffer or gate, and contention analysis;
- USB-C CC pull-downs, USB 2.0 ESD, 5 V protection, 90 ohm route, and internal USB-A connector;
- Teensy native Ethernet pairs, selected magjack, Bob Smith/chassis termination where required by its reference, LEDs, and 100 ohm routing contract;
- USB current budget for Teensy, PHY, optics, logic, indicators, and bounded expansion allowance;
- socketed Teensy, keyed expansion, current limiting or protection, remote controls, and reserved pins;
- chassis/shield network suitable for metal or insulated mounting;
- test points for every rail, reset, optical data, mux state, transmitter enable, USB, and Ethernet bring-up;
- a fixture header or equivalent manufacturing diagnostic seam.

The schematic contains a reset and failure-state truth table for Teensy absent, boot, reset, brownout, firmware crash, optical loss, unsupported burst, malformed input, USB unplug, raw-bypass request, and unpowered expansion. Hardware pulls keep the transmitter disabled and select the processed Teensy path during reset. Expansion outputs are high impedance or current-limited until firmware configures them.

```yaml
phase: H1
unit: schematic-components
seams: [power tree, optical interface, USB bridge, Ethernet interface, safe-state controls]
contract-artifact: both
fan-out:
  - name: power-usb
    owned-paths: [electronics/sheets/power-usb]
  - name: optical-spdif
    owned-paths: [electronics/sheets/optical-spdif]
  - name: ethernet
    owned-paths: [electronics/sheets/ethernet]
  - name: teensy-expansion
    owned-paths: [electronics/sheets/teensy-expansion]
review-intensity: active
commit-checkpoint:
  green: [KiCad ERC, pin-by-pin datasheet audit, power calculations, clean-room electrical and FMEA review]
  buildable: [reviewed manufacturing schematic]
  parent-commit: "feat(aius): complete carrier schematic"
```

The schematic coordinator imports reviewed hierarchical sheets into the canonical Aius KiCad project only after their owned-path reviews pass.

## H2: Two-layer feasibility gate

Before detailed routing:

1. Obtain PCBWay's actual two-layer material and impedance capabilities.
2. Set the exact enclosure-guide board outline and verify board thickness.
3. Place Teensy, Ethernet magjack, USB-C, internal USB-A, both optical modules, expansion, and test access.
4. Reserve the approximately 100 mm internal USB cable volume, bend radius, connector insertion path, and retention.
5. Calculate and record manufacturable 90 ohm USB and 100 ohm Ethernet geometries.
6. Route both high-speed pairs provisionally on top without vias.
7. Demonstrate a continuous bottom-ground return under both pairs and their connector transitions.
8. Route or escape all remaining nets without plane cuts or unacceptable return-path necks.
9. Confirm the full board remains no larger than 100 x 100 mm.
10. Run a mechanical interference and assembly-access review.

Escalate to four layers if any criterion fails. Record the failed criterion, proposed four-layer stackup, corrected return-path design, and cost delta in the plan and cost model before continuing.

```yaml
phase: H2
unit: placement-and-stackup
seams: [board outline, placement contract, impedance geometry, layer decision]
contract-artifact: both
fan-out:
  - name: mechanical-placement
    owned-paths: [mechanical]
  - name: stackup-routing-study
    owned-paths: [electronics/feasibility]
review-intensity: active
commit-checkpoint:
  green: [mechanical review, impedance calculation, return-path review, two-layer feasibility verdict]
  buildable: [approved board outline placement and stackup]
  parent-commit: "design(aius): approve carrier stackup and placement"
```

## H3: PCB, DFM, and quotation

Complete routing under the approved layer contract. Run ERC, DRC, impedance, clearance, return-path, creepage, test-access, DFM, DFA, enclosure, cable, and independent electrical reviews.

Generate the manufacturing package twice from clean checkouts. Compare normalized manifests and checksums. Request:

- the vendor-minimum bare-board quantity;
- assembly and component procurement for three boards only;
- an equivalent four-layer comparison price;
- shipping to M20 5PG with taxes and duties identified.

Update the cost model from the actual BOM, fabrication settings, assembly quotation, component substitutions, freight, VAT, and duty. Review PCBWay's rendered preview and every substitution before order approval.

```yaml
phase: H3
unit: pcb-and-release-package
seams: [routed PCB, manufacturing manifest, assembly BOM, landed quotation]
contract-artifact: both
fan-out:
  - name: pcb-layout
    owned-paths: [electronics/aius-carrier]
  - name: manufacturing-output
    owned-paths: [manufacturing]
  - name: quotation
    owned-paths: [evidence/cost]
review-intensity: active
commit-checkpoint:
  green: [ERC, DRC, DFM, DFA, mechanical review, reproducibility, clean-room release audit]
  buildable: [quoted but unordered three-unit prototype package]
  parent-commit: "design(aius): complete prototype release package"
```

## H4: Three-unit EVT

Order only after H3 approval. Preserve the PCBWay preview, order configuration, approved substitutions, invoice, fabrication data, and received-board identifiers as evidence.

Bring-up order:

1. Visual inspection, continuity, shorts, and unpowered resistance.
2. Current-limited power, rails, thermal scan, and consumption.
3. Reset-safe states with Teensy absent, present, held reset, and rebooting.
4. Rear USB-C to internal USB bridge.
5. Native Ethernet link and traffic.
6. Optical electrical loopback and module gating.
7. Diagnostic raw S/PDIF bypass.
8. Teensy S/PDIF RX and TX independently, then simultaneously.
9. Carrier loss-of-lock, malformed-input, and recovery behaviour.

```yaml
phase: H4
unit: hardware-test-surface
seams: [bring-up procedure, per-board record, defect and ECO log]
contract-artifact: both
fan-out:
  - name: evt-execution
    owned-paths: [evidence/evt-results]
review-intensity: active
commit-checkpoint:
  green: [three-board inspection, one full bring-up pass, regression repeat, ECO review]
  buildable: [working EVT carrier]
  parent-commit: "test(aius): record carrier EVT result"
```

## H5: Full audio integration and release decision

Prove the complete path:

`TV/source -> TOSLINK -> IEC 61937 -> DTS Core -> custom downmix/EQ -> 48 kHz PCM TOSLINK -> existing DAC`

Test stereo PCM input, DTS Core 5.1, format transitions, optical loss, source sample-rate changes, unsupported bursts, reset, brownout, Ethernet activity, and user-control changes. Measure S/PDIF word integrity, recovered-clock behaviour, clock drift, buffering, underrun and overrun recovery, end-to-end latency, carrier detection and gating, jitter sensitivity, output channel mapping, and sustained decoder/DSP headroom.

Complete thermal, fault, long-soak, enclosure, basic pre-compliance EMC, production-test, and repeatability checks. Record every ECO and refresh the cost model with actual yield, parts, assembly, rework, freight, and enclosure results.

```yaml
phase: H5
unit: system-test-surface
seams: [end-to-end audio contract, DVT report, release decision]
contract-artifact: both
fan-out:
  - name: audio-integration
    owned-paths: [evidence/system-results]
  - name: firmware-integration
    owned-paths: [firmware/src/product]
review-intensity: active
commit-checkpoint:
  green: [audio matrix, timing and drift tests, thermal and fault tests, soak, release audit]
  buildable: [validated personal-build design]
  parent-commit: "test(aius): complete system validation"
```

## Release assumptions

- This is a personal, non-commercial project. Licence concerns are provenance footnotes, not engineering gates; preserve third-party notices.
- Passing the software capability gate does not approve PCB manufacture.
- Two layers are preferred for cost but cannot compromise USB, Ethernet, chassis return, or reset safety.
- Panel machining and CNC work are customer-supplied.
- Compliance certification and production commercialization are outside scope.
