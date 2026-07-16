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
- one selected USB-A male to Micro-B male cable carrying both data and the sole 5 V supply to the Teensy, with its evidence-derived length, bend envelope and service slack;
- keyed expansion for I2C display/encoder/buttons/LEDs, one six-circuit SPI interface carrying SCK, MOSI, MISO, one chip select, 3.3 V and ground, UART/debug, I2S/TDM, USB host, and GPIO/ADC; a trigger or relay output is an optional allocation of `expansion-gpio-adc`, not another interface;
- access to microSD, Program, status LED, and a remote Program/RTC header;
- test points and a manufacturing/bring-up interface.

There is one power source. The carrier receives 5 V from rear USB-C and passes that same rail to the Teensy through the internal USB cable. No second VIN or VUSB feed is permitted.

## PCB and mechanical decision

Use a two-layer PCB by default. Escalate to four layers only for a recorded
electrical return-path, impedance or routing failure that a four-layer stackup
actually resolves. Outline, panel, cable, access or collision failures block
mechanical redesign and cannot be cleared by changing layer count.

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
machine-checked schemas, and committed checkpoints. Only P1Ed's eligible branch
completes H0; its blocked branch preserves evidence without authorizing H1. The
original single-checkpoint outline below is retained as outcome history and is
not executed separately.

Complete the functional topology, connector and control inventory, power and clock domains, operating/fault states, pin/resource map, expansion map, preliminary placement, reference provenance, functional BOM, and risk register.

All saved research artifacts live below `docs/reference/datasheets.md` and follow the evidence and Git LFS policy in the capability plan. Reference/evidence schematics are explicitly not manufacturing schematics.

```yaml
phase: H0
unit: single-file-implementation
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

H1 cannot start until both P1Ed has committed an eligible H0 result and the
software gate's P7 phase has passed. H0 evidence work may proceed before P7
because it does not commit manufacturing effort.

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

H1 is three serial checkpoints. The first lands the reusable gate contract, the
second commits the shared interface before any sheet worker starts, and the
third consumes that immutable baseline.

The hardware bridge consumes three deterministic verdict artifacts:
`evidence/design/p1d-release.json`, `evidence/cost/h0-verdict.json`, and
`evidence/capability-results/p7-verdict.json`. H1 requires their states to be
`eligible`, `eligible`, and `pass`, respectively, and verifies every recorded
input hash before dispatch. A shared hardware-gate validator and its closed
fixtures land first so later failed physical outcomes can be committed without
weakening their release gates.

```yaml
phase: H1a0
unit: test-surface
seams: [closed hardware-result schemas, structural validation, deterministic verdict exits]
contract-artifact: spec-only
fan-out:
  - name: hardware-gate-validator
    owned-paths:
      - evidence/hardware/hardware-gate-schema.json
      - scripts/validate_hardware.py
      - scripts/test_validate_hardware.py
      - scripts/fixtures/hardware-gates/cases.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_hardware.py
  buildable: [fixture-proven structural and verdict gates for H1 through H5]
  parent-commit: "feat(aius): define hardware result gates"
```

`validate_hardware.py` accepts `--stage` plus an optional `--record` path. Every
stage first validates structure and references. Verdict stages return 0 for the
named eligible/pass branch, 3 for a structurally valid blocked branch, and 4
only for H2b's structurally valid `four-layer-required` branch. Diagnostics are
sorted unique `AIUS-HW-BLOCKED:<stage>:<record-id>:<reason-code>` lines only.
Exit 4 instead emits sorted unique
`AIUS-HW-FOUR-LAYER:<criterion-id>:<reason-code>` lines only.
`--record` atomically writes the exact deterministic verdict selected by the
same evaluation; no timestamp or Git identity is permitted.

H1a's `sheet-interfaces.json` is the explicit P1D-to-manufacturing projection.
Its ordered keys are `schema_version`, `reference_release_sha256`,
`reference_sheets`, `manufacturing_sheets`, `symbol_bindings`, `deviations`,
`notes`. It copies every reference sheet ID and ordered hierarchical-pin tuple
byte-semantically from `schematic-interface-v1.json`; manufacturing sheets use
the same IDs and tuples with only the path root changed from
`electronics/reference` to `electronics/carrier/sheets`. Symbol bindings copy
the closed P1D component/source/support-circuit identities and pin counts.
`deviations` is initially empty; a later non-empty deviation requires a new H1a
commit and clean-room electrical review before any sheet resumes. The root and
library are generated only from this mapping, never by inspecting sheet text.

```yaml
phase: H1a
unit: component-behind-interface
seams: [canonical root, local symbol library, hierarchical sheet interfaces]
contract-artifact: spec-only
depends-on:
  - evidence/design/p1d-release.json
  - evidence/cost/h0-verdict.json
  - evidence/capability-results/p7-verdict.json
  - electronics/reference/schematic-interface-v1.json
  - electronics/reference/reference-schematic-contract.json
  - evidence/hardware/hardware-gate-schema.json
fan-out:
  - name: manufacturing-schematic-interface
    owned-paths:
      - electronics/carrier/aius-carrier.kicad_sch
      - electronics/carrier/aius-carrier.kicad_sym
      - electronics/carrier/sheet-interfaces.json
      - electronics/carrier/README.md
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h1-interface
    - scripts/kicad.sh sch erc electronics/carrier/aius-carrier.kicad_sch
  buildable: [committed root library and exact sheet-interface stubs]
  parent-commit: "design(aius): lock carrier schematic interfaces"
```

```yaml
phase: H1b
unit: single-file-implementation
seams: [committed sheet interface, predeclared symbols, power and safe-state contracts]
contract-artifact: stubs-committed
depends-on:
  - electronics/carrier/aius-carrier.kicad_sch
  - electronics/carrier/aius-carrier.kicad_sym
  - electronics/carrier/sheet-interfaces.json
  - evidence/design/electrical-reference.json
  - evidence/design/pin-resource-map.json
fan-out:
  - name: power-usb
    owned-paths: [electronics/carrier/sheets/power-usb.kicad_sch]
  - name: optical-spdif
    owned-paths: [electronics/carrier/sheets/optical-spdif.kicad_sch]
  - name: ethernet
    owned-paths: [electronics/carrier/sheets/ethernet.kicad_sch]
  - name: teensy-expansion
    owned-paths: [electronics/carrier/sheets/teensy-expansion.kicad_sch]
  - name: schematic-root-coordinator
    owned-paths:
      - electronics/carrier/aius-carrier.kicad_sch
coordination: maximum three sheet agents; after all four workers return, the coordinator imports only reviewed sheets and does not edit the immutable interface or shared symbol library; the parent commits the integrated phase
review-intensity: active
commit-checkpoint:
  green:
    - scripts/kicad.sh sch erc electronics/carrier/aius-carrier.kicad_sch
    - python3 scripts/validate_hardware.py --stage h1-schematic
    - clean-room pin-by-pin electrical power and FMEA review
  buildable: [reviewed manufacturing schematic]
  parent-commit: "feat(aius): complete carrier schematic"
```

## H2: Two-layer feasibility gate

Mechanical placement completes first:

1. Set the exact enclosure-guide board outline and verify board thickness.
2. Place Teensy, Ethernet magjack, USB-C, internal USB-A, both optical modules,
   expansion, and test access.
3. Reserve the selected cable's evidence-derived length, bend envelope, service
   slack, connector insertion path, and retention.
4. Confirm the full board remains no larger than 100 x 100 mm.
5. Run mechanical interference and assembly-access review.

Only after that checkpoint, the routing study:

1. Obtains PCBWay's actual two-layer material and impedance capabilities.
2. Calculates and records manufacturable 90 ohm USB and 100 ohm Ethernet geometries.
3. Routes both high-speed pairs provisionally on top without vias.
4. Demonstrates a continuous bottom-ground return under both pairs and connector transitions.
5. Routes or escapes all remaining nets without plane cuts or unacceptable return-path necks.

Escalate to four layers only if an electrical route, impedance or return-path
criterion fails while every mechanical criterion passes. Record the failed
electrical criterion, proposed four-layer stackup, corrected return-path design,
and cost delta in the plan and cost model before continuing. A mechanical
failure blocks placement redesign instead.

```yaml
phase: H2a
unit: component-behind-interface
seams: [enclosure guide, board outline, component and cable envelopes]
contract-artifact: both
depends-on:
  - electronics/carrier/aius-carrier.kicad_sch
  - electronics/carrier/sheet-interfaces.json
  - evidence/design/two-layer-placement.json
  - https://www.hammfg.com/part/1455N1201BK
  - https://www.hammfg.com/part/1455N1201BK
fan-out:
  - name: mechanical-placement
    owned-paths:
      - mechanical/board-outline.json
      - mechanical/component-placement.json
      - mechanical/cable-envelope.json
      - mechanical/h2a-verdict.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h2a-structural
  branches:
    eligible:
      command: python3 scripts/validate_hardware.py --stage h2a-gate --record mechanical/h2a-verdict.json
      expected-exit: 0
      buildable: [committed mechanically valid board outline and placement]
      parent-commit: "design(aius): approve carrier mechanical placement"
    blocked:
      command: python3 scripts/validate_hardware.py --stage h2a-gate --record mechanical/h2a-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h2a-gate lines only
      buildable: [committed mechanical evidence requiring placement redesign]
      parent-commit: "design(aius): record blocked carrier placement"
```

```yaml
phase: H2b
unit: phase-task
seams: [committed placement, vendor stackup, impedance geometry, layer decision]
contract-artifact: stubs-committed
depends-on:
  - mechanical/h2a-verdict.json
  - mechanical/board-outline.json
  - mechanical/component-placement.json
  - electronics/carrier/aius-carrier.kicad_sch
fan-out:
  - name: stackup-routing-study
    owned-paths:
      - electronics/feasibility/vendor-stackup.json
      - electronics/feasibility/impedance-calculations.json
      - electronics/feasibility/routing-study.json
      - electronics/feasibility/layer-decision.json
      - electronics/feasibility/h2b-verdict.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h2b-structural
  branches:
    two-layer:
      command: python3 scripts/validate_hardware.py --stage h2b-gate --record electronics/feasibility/h2b-verdict.json
      expected-exit: 0
      buildable: [approved two-layer stackup and routing contract]
      parent-commit: "design(aius): approve two-layer carrier feasibility"
    four-layer-required:
      command: python3 scripts/validate_hardware.py --stage h2b-gate --record electronics/feasibility/h2b-verdict.json
      expected-exit: 4
      expected-diagnostics: sorted exact AIUS-HW-FOUR-LAYER lines only
      buildable: [approved four-layer stackup after documented electrical failure]
      parent-commit: "design(aius): require four-layer carrier stackup"
    blocked:
      command: python3 scripts/validate_hardware.py --stage h2b-gate --record electronics/feasibility/h2b-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h2b-gate lines only
      buildable: [committed feasibility evidence requiring redesign]
      parent-commit: "design(aius): record blocked carrier feasibility"
```

H3a accepts only an H2b verdict of `two-layer` or `four-layer-required`; it
rejects `blocked`. H4 accepts only `order-authorization.json` state `authorized`.
H5a and H5b accept only an H4 verdict of `pass`. Every dependency check is
content- and hash-based, not file-presence-only.

## H3: PCB, DFM, and quotation

Complete routing under the approved layer contract. Run ERC, DRC, impedance, clearance, return-path, creepage, test-access, DFM, DFA, enclosure, cable, and independent electrical reviews.

Generate the manufacturing package twice from clean checkouts. Compare normalized manifests and checksums. Request:

- the vendor-minimum bare-board quantity;
- assembly and component procurement for three boards only;
- an equivalent four-layer comparison price;
- shipping to M20 5PG with taxes and duties identified.

Update the cost model from the actual BOM, fabrication settings, assembly quotation, component substitutions, freight, VAT, and duty. Review PCBWay's rendered preview and every substitution before order approval.

H3 is serial because manufacturing outputs consume the finished PCB, and the
quotation consumes the frozen manufacturing package.

```yaml
phase: H3a
unit: phase-task
seams: [approved schematic, board outline, stackup, routed PCB]
contract-artifact: both
depends-on:
  - electronics/carrier/aius-carrier.kicad_sch
  - electronics/carrier/sheet-interfaces.json
  - mechanical/board-outline.json
  - mechanical/component-placement.json
  - electronics/feasibility/h2b-verdict.json
fan-out:
  - name: pcb-layout
    owned-paths:
      - electronics/carrier/aius-carrier.kicad_pcb
      - electronics/carrier/aius-carrier.kicad_dru
      - electronics/carrier/pcb-notes.md
review-intensity: active
commit-checkpoint:
  green:
    - scripts/kicad.sh sch erc electronics/carrier/aius-carrier.kicad_sch
    - scripts/kicad.sh pcb drc electronics/carrier/aius-carrier.kicad_pcb
    - python3 scripts/validate_hardware.py --stage h3-pcb
    - clean-room electrical mechanical DFM and test-access review
  buildable: [finished fabrication-candidate PCB]
  parent-commit: "design(aius): complete carrier PCB"
```

```yaml
phase: H3b
unit: phase-task
seams: [committed PCB, normalized fabrication outputs, assembly BOM]
contract-artifact: stubs-committed
depends-on:
  - electronics/carrier/aius-carrier.kicad_pcb
  - electronics/carrier/aius-carrier.kicad_dru
  - electronics/feasibility/h2b-verdict.json
fan-out:
  - name: manufacturing-output
    owned-paths:
      - manufacturing/aius-carrier-manufacturing.zip
      - manufacturing/assembly-bom.csv
      - manufacturing/positions.csv
      - manufacturing/manifest.json
      - manufacturing/checksums.sha256
review-intensity: active
commit-checkpoint:
  green: [DFM, DFA, two-clean-checkout reproducibility, normalized manifest and checksum parity, clean-room release audit]
  buildable: [frozen unordered manufacturing package]
  parent-commit: "design(aius): generate carrier manufacturing package"
```

```yaml
phase: H3c
unit: phase-task
seams: [frozen manufacturing package, vendor quotation, landed cost model]
contract-artifact: stubs-committed
depends-on:
  - manufacturing/aius-carrier-manufacturing.zip
  - manufacturing/assembly-bom.csv
  - manufacturing/positions.csv
  - manufacturing/manifest.json
  - manufacturing/checksums.sha256
fan-out:
  - name: quotation
    owned-paths:
      - docs/reference/datasheets.mdAIUS-PCBWAY-EVT-QUOTE/quote.pdf
      - docs/reference/datasheets.mdAIUS-PCBWAY-EVT-QUOTE/preview.pdf
      - docs/reference/datasheets.mdAIUS-PCBWAY-EVT-QUOTE/order-configuration.json
      - evidence/cost/prototype-quotation.json
      - evidence/cost/landed-prototype-cost.csv
      - evidence/cost/should-cost.csv
      - evidence/cost/risk-register.csv
      - evidence/cost/h3c-verdict.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h3c-structural
    - python3 scripts/validate_hardware.py --stage h3c-quote --record evidence/cost/h3c-verdict.json
  buildable: [quoted but unordered three-unit prototype package]
  parent-commit: "docs(aius): record prototype quotation"
```

H3c never places an order. After the quoted checkpoint, H3d pauses for explicit
user acceptance of the exact landed amount and substitutions. The parent then
records the answer in `order-authorization.json`, with ordered keys
`schema_version`, `decision`, `quotation_sha256`, `authorized_quantity`,
`shipping_postcode`, `notes`; decision is `authorized` or `cancelled`, quantity
is 3 only when authorized, and postcode is `M20 5PG`. No automation infers this
decision.

```yaml
phase: H3d
unit: parent-only
seams: [committed quote, explicit user decision, purchase authority]
contract-artifact: spec-only
depends-on:
  - evidence/cost/h3c-verdict.json
  - evidence/cost/prototype-quotation.json
  - docs/reference/datasheets.mdAIUS-PCBWAY-EVT-QUOTE/order-configuration.json
coordination-owned-paths:
  - evidence/cost/order-authorization.json
review-intensity: active
commit-checkpoint:
  branches:
    authorized:
      command: python3 scripts/validate_hardware.py --stage h3d-authorization --decision authorized --record evidence/cost/order-authorization.json
      expected-exit: 0
      buildable: [explicit authority to order exactly three quoted EVT assemblies]
      parent-commit: "docs(aius): authorize three-unit EVT order"
    cancelled:
      command: python3 scripts/validate_hardware.py --stage h3d-authorization --decision cancelled --record evidence/cost/order-authorization.json
      expected-exit: 3
      expected-diagnostics: ["AIUS-HW-BLOCKED:h3d-authorization:order:cancelled"]
      buildable: [quoted package retained without purchase authority]
      parent-commit: "docs(aius): decline EVT order"
```

## H4: Three-unit EVT

Order only after H3 approval. Preserve the PCBWay preview, order configuration, approved substitutions, invoice, fabrication data, and received-board identifiers as evidence.

Bring-up order:

1. Visual inspection, continuity, shorts, and unpowered resistance.
2. Current-limited power, rails, thermal scan, and consumption.
3. Reset-safe states with Teensy absent, present, held reset, and rebooting.
4. Internal USB cable retention under the insertion, handling, and vibration
   loads defined in P1D/H2; inspect mount torque, cable-tie restraint, service
   slack, PCB strain, and load transfer into the Teensy, connectors, and
   enclosure.
5. Rear USB-C to internal USB bridge.
6. Native Ethernet link and traffic.
7. Optical electrical loopback and module gating.
8. Diagnostic raw S/PDIF bypass.
9. Teensy S/PDIF RX and TX independently, then simultaneously.
10. Carrier loss-of-lock, malformed-input, and recovery behaviour.

```yaml
phase: H4
unit: test-surface
seams: [bring-up procedure, per-board record, defect and ECO log]
contract-artifact: both
depends-on:
  - evidence/cost/order-authorization.json
  - manufacturing/manifest.json
  - manufacturing/checksums.sha256
fan-out:
  - name: evt-execution
    owned-paths:
      - evidence/evt-results/evt-plan.json
      - evidence/evt-results/unit-01.json
      - evidence/evt-results/unit-02.json
      - evidence/evt-results/unit-03.json
      - evidence/evt-results/measurements.csv
      - evidence/evt-results/defects.json
      - evidence/evt-results/eco-log.json
      - evidence/evt-results/h4-verdict.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h4-structural
  branches:
    pass:
      command: python3 scripts/validate_hardware.py --stage h4-gate --record evidence/evt-results/h4-verdict.json
      expected-exit: 0
      buildable: [working EVT carrier with three inspections and one repeated full bring-up pass]
      parent-commit: "test(aius): approve carrier EVT result"
    blocked:
      command: python3 scripts/validate_hardware.py --stage h4-gate --record evidence/evt-results/h4-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h4-gate lines only
      buildable: [complete EVT evidence with defects and ECO gate]
      parent-commit: "test(aius): record blocked carrier EVT result"
```

## H5: Full audio integration and release decision

Prove the complete path:

`TV/source -> TOSLINK -> IEC 61937 -> DTS Core -> custom downmix/EQ -> 48 kHz PCM TOSLINK -> existing DAC`

Test stereo PCM input, DTS Core 5.1, format transitions, optical loss, source sample-rate changes, unsupported bursts, reset, brownout, Ethernet activity, and user-control changes. Measure S/PDIF word integrity, recovered-clock behaviour, clock drift, buffering, underrun and overrun recovery, end-to-end latency, carrier detection and gating, jitter sensitivity, output channel mapping, and sustained decoder/DSP headroom.

Complete thermal, fault, long-soak, enclosure, basic pre-compliance EMC, production-test, and repeatability checks. Record every ECO and refresh the cost model with actual yield, parts, assembly, rework, freight, and enclosure results.

```yaml
phase: H5a
unit: phase-task
seams: [proven decoder/DSP modules, EVT carrier drivers, product state machine]
contract-artifact: both
depends-on:
  - evidence/evt-results/h4-verdict.json
  - evidence/capability-results/p7-verdict.json
  - evidence/capability-results/p8-verdict.json
  - electronics/carrier/sheet-interfaces.json
fan-out:
  - name: firmware-integration
    owned-paths:
      - firmware/src/product/audio_pipeline.cpp
      - firmware/src/product/audio_pipeline.h
      - firmware/src/product/carrier_control.cpp
      - firmware/src/product/carrier_control.h
      - firmware/src/product/product_state_machine.cpp
      - firmware/src/product/product_state_machine.h
      - firmware/src/main.cpp
      - firmware/test/product/test_product.cpp
review-intensity: active
commit-checkpoint:
  green:
    - platformio test -d firmware -e native
    - platformio run -d firmware -e teensy41-release
    - python3 scripts/validate_hardware.py --stage h5-firmware
    - hardware-in-loop smoke matrix and reset/fault-state recovery
  buildable: [committed end-to-end product firmware candidate]
  parent-commit: "feat(aius): integrate product firmware"
```

```yaml
phase: H5b
unit: test-surface
seams: [committed product firmware, end-to-end audio contract, DVT report, release decision]
contract-artifact: stubs-committed
depends-on:
  - evidence/evt-results/h4-verdict.json
  - firmware/src/product/audio_pipeline.cpp
  - firmware/src/product/carrier_control.cpp
  - firmware/src/product/product_state_machine.cpp
  - firmware/src/main.cpp
fan-out:
  - name: audio-system-validation
    owned-paths:
      - evidence/system-results/test-matrix.json
      - evidence/system-results/audio-measurements.csv
      - evidence/system-results/timing-memory.csv
      - evidence/system-results/thermal-fault-results.json
      - evidence/system-results/soak-results.json
      - evidence/system-results/precompliance-results.json
      - evidence/system-results/eco-log.json
      - evidence/system-results/h5-verdict.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h5-structural
  branches:
    pass:
      command: python3 scripts/validate_hardware.py --stage h5-gate --record evidence/system-results/h5-verdict.json
      expected-exit: 0
      buildable: [validated personal-build design]
      parent-commit: "test(aius): approve system validation"
    blocked:
      command: python3 scripts/validate_hardware.py --stage h5-gate --record evidence/system-results/h5-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h5-gate lines only
      buildable: [complete system evidence with release blocked]
      parent-commit: "test(aius): record blocked system validation"
```

## Release assumptions

- This is a personal, non-commercial project. Licence concerns are provenance footnotes, not engineering gates; preserve third-party notices.
- Passing the software capability gate does not approve PCB manufacture.
- Two layers are preferred for cost but cannot compromise USB, Ethernet, chassis return, or reset safety.
- Panel machining and CNC work are customer-supplied.
- Compliance certification and production commercialization are outside scope.
