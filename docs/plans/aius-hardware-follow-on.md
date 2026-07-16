---
title: Aius hardware follow-on
type: plans
scope: aius
status: draft
tags: [aius, hardware, pcb, teensy, spdif, ethernet]
updated: 2026-07-16
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

H1 begins with eight serial schema-sized checkpoints. Each stage-family task
owns only its closed schema and, where evaluation is already knowable, its
projection oracle. A final coordinator builds the shared validator bundle.
Interface, stub and schematic checkpoints then consume that committed bundle.

The hardware bridge consumes three deterministic verdict artifacts:
`evidence/design/p1d-release.json`, `evidence/cost/h0-verdict.json`, and
`evidence/capability-results/p7-verdict.json`. H1 requires their states to be
`eligible`, `eligible`, and `pass`, respectively, and verifies every recorded
input hash before dispatch. A shared hardware-gate validator and its closed
fixtures land first so later failed physical outcomes can be committed without
weakening their release gates.

```yaml
phase: H1a0a
unit: single-file-implementation
seams: [closed H1 bridge schema]
contract-artifact: spec-only
fan-out:
  - name: h1-bridge-schema
    owned-paths: [evidence/hardware/schemas/h1-interface-schema.json]
review-intensity: active
commit-checkpoint:
  green: [schema meta-validation]
  buildable: [closed H1 gate-input and sheet-interface contract]
  parent-commit: "feat(aius): define H1 bridge schema"
```

```yaml
phase: H1a0b
unit: test-surface
seams: [closed H2a result schema, deterministic mechanical projection oracle]
contract-artifact: both
fan-out:
  - name: h2a-gate-contract
    owned-paths:
      - evidence/hardware/schemas/h2a-schema.json
      - evidence/hardware/oracles/h2a-gate-oracle.json
review-intensity: active
commit-checkpoint:
  green: [schema meta-validation and oracle completeness]
  buildable: [closed deterministic H2a contract]
  parent-commit: "feat(aius): define H2a gate contract"
```

```yaml
phase: H1a0c
unit: test-surface
seams: [closed H2b result schema, deterministic layer projection oracle]
contract-artifact: both
fan-out:
  - name: h2b-gate-contract
    owned-paths:
      - evidence/hardware/schemas/h2b-schema.json
      - evidence/hardware/oracles/h2b-gate-oracle.json
review-intensity: active
commit-checkpoint:
  green: [schema meta-validation and oracle completeness]
  buildable: [closed deterministic H2b contract]
  parent-commit: "feat(aius): define H2b gate contract"
```

```yaml
phase: H1a0d
unit: single-file-implementation
seams: [closed deterministic manufacturing-package contract]
contract-artifact: spec-only
fan-out:
  - name: manufacturing-package-schema
    owned-paths: [evidence/hardware/schemas/manufacturing-package-schema.json]
review-intensity: active
commit-checkpoint:
  green: [schema meta-validation]
  buildable: [closed manifest CSV archive and checksum contract]
  parent-commit: "feat(aius): define manufacturing package schema"
```

```yaml
phase: H1a0e
unit: test-surface
seams: [closed H3 quotation schema, deterministic quotation projection oracle]
contract-artifact: both
fan-out:
  - name: h3c-gate-contract
    owned-paths:
      - evidence/hardware/schemas/h3c-schema.json
      - evidence/hardware/oracles/h3c-gate-oracle.json
review-intensity: active
commit-checkpoint:
  green: [schema meta-validation and oracle completeness]
  buildable: [closed deterministic H3 quotation contract]
  parent-commit: "feat(aius): define H3 quotation gate contract"
```

```yaml
phase: H1a0f
unit: single-file-implementation
seams: [closed H4 structural-result family]
contract-artifact: spec-only
fan-out:
  - name: h4-result-schema
    owned-paths: [evidence/hardware/schemas/h4-schema.json]
review-intensity: active
commit-checkpoint:
  green: [schema meta-validation]
  buildable: [closed H4 result family awaiting fixed EVT criteria]
  parent-commit: "feat(aius): define H4 result schema"
```

```yaml
phase: H1a0g
unit: single-file-implementation
seams: [closed H5 structural-result family]
contract-artifact: spec-only
fan-out:
  - name: h5-result-schema
    owned-paths: [evidence/hardware/schemas/h5-schema.json]
review-intensity: active
commit-checkpoint:
  green: [schema meta-validation]
  buildable: [closed H5 result family awaiting fixed system criteria]
  parent-commit: "feat(aius): define H5 result schema"
```

```yaml
phase: H1a0h
unit: test-surface
seams: [seven committed stage schemas, shared structural validator and fixtures]
contract-artifact: both
depends-on:
  - evidence/hardware/schemas/h1-interface-schema.json
  - evidence/hardware/schemas/h2a-schema.json
  - evidence/hardware/schemas/h2b-schema.json
  - evidence/hardware/schemas/manufacturing-package-schema.json
  - evidence/hardware/schemas/h3c-schema.json
  - evidence/hardware/schemas/h4-schema.json
  - evidence/hardware/schemas/h5-schema.json
  - evidence/hardware/oracles/h2a-gate-oracle.json
  - evidence/hardware/oracles/h2b-gate-oracle.json
  - evidence/hardware/oracles/h3c-gate-oracle.json
fan-out:
  - name: hardware-gate-coordinator
    owned-paths:
      - evidence/hardware/hardware-gate-schema.json
      - scripts/validate_hardware.py
      - scripts/test_validate_hardware.py
      - scripts/fixtures/hardware-gates/cases.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_hardware.py
  buildable: [fixture-proven structural and deterministic early hardware gates]
  parent-commit: "feat(aius): integrate hardware result gates"
```

`hardware-gate-schema.json` is a closed bundle built only from the seven
committed stage-family schemas. The tables below define the generated JSON and
CSV families those stages validate; the bridge, criteria and authorization
shapes are defined in their owning phase sections. All JSON roots begin
`schema_version` with constant `1.0.0`, reject additional properties, use sorted
unique IDs, and end with string `notes`. The ordered artifact contracts are:

| Artifact | Exact ordered fields |
|---|---|
| `board-outline.json` | `schema_version`, `board`, `evidence_ids`, `notes`; board is the exact P1D board row |
| `component-placement.json` | `schema_version`, `placements`, `keepouts`, `evidence_ids`, `notes`; rows are the exact P1D placement/keepout rows |
| `cable-envelope.json` | `schema_version`, `cables`, `collision_checks`, `criteria`, `evidence_ids`, `notes`; rows are the exact P1D cable/collision/mechanical-criterion rows |
| `vendor-stackup.json` | `schema_version`, `vendor`, `layer_count`, `material`, `finished_thickness_um`, `copper_weight_um`, `dielectric_height_um`, `dielectric_constant`, `minimum_trace_um`, `minimum_gap_um`, `status`, `evidence_ids`, `notes`; status is `available` or `unavailable`, and unavailable retains evidence while numeric capability fields are null |
| `impedance-calculations.json` | `schema_version`, `calculations`, `evidence_ids`, `notes`; rows key `calculation_id`, `interface`, `target_mohm`, `tolerance_mohm`, `trace_width_um`, `gap_um`, `reference_plane`, `calculated_mohm`, `method`, `evidence_ids`, `status`, `notes` |
| `routing-study.json` | `schema_version`, `routes`, `ground_corridors`, `criteria`, `evidence_ids`, `notes`; rows reuse exact P1D route/corridor/electrical-criterion shapes |
| `layer-decision.json` | `schema_version`, followed once by the exact P1D layer-decision keys |
| `order-configuration.json` | `schema_version`, `vendor`, `board_configuration`, `assembly_configuration`, `shipping_postcode`, `quote_artifact_sha256`, `preview_artifact_sha256`, `notes`; both configuration objects are closed lexically keyed scalar maps and postcode is `M20 5PG` |
| `prototype-quotation.json` | `schema_version`, `vendor`, `configuration_sha256`, `bare_board_quantity`, `assembled_quantity`, `two_layer_gbp_ex_vat`, `four_layer_gbp_ex_vat`, `assembly_gbp_ex_vat`, `components_gbp_ex_vat`, `freight_gbp`, `vat_gbp`, `duty_gbp`, `landed_gbp`, `currency`, `quotation_status`, `substitutions`, `evidence_ids`, `notes`; status is `complete` or `incomplete`; substitution rows key `substitution_id`, `requested_mpn`, `proposed_mpn`, `decision`, `evidence_ids`, `notes`, with decision `approved`, `rejected` or `pending` |
| `evt-plan.json` | `schema_version`, `procedure_contract_sha256`, `gate_oracle_sha256`, `procedure_ids`, `acceptance_ids`, `firmware_sha256`, `equipment_ids`, `notes` |
| EVT unit file | `schema_version`, `unit_id`, `board_serial`, `inspection_status`, `procedure_results`, `defect_ids`, `eco_ids`, `notes`; procedure rows key `procedure_id`, `status`, `measurement_ids`, `evidence_ids`, `notes` |
| `defects.json` | `schema_version`, `defects`, `notes`; rows key `defect_id`, `unit_ids`, `severity`, `description`, `evidence_ids`, `status`, `notes` |
| either `eco-log.json` | `schema_version`, `ecos`, `notes`; rows key `eco_id`, `source_defect_ids`, `description`, `affected_artifact_ids`, `verification_status`, `evidence_ids`, `notes` |
| `test-matrix.json` | `schema_version`, `test_contract_sha256`, `gate_oracle_sha256`, `cases`, `notes`; rows key `case_id`, `input_format`, `fault_mode`, `expected_state`, `measurement_ids`, `status`, `notes` |
| `thermal-fault-results.json` | `schema_version`, `results`, `notes`; rows key `result_id`, `case_id`, `metric`, `value`, `unit`, `minimum`, `maximum`, `status`, `evidence_ids`, `notes` |
| `soak-results.json` | `schema_version`, `duration_seconds`, `sample_count`, `reset_count`, `error_count`, `maximum_temperature_c`, `status`, `evidence_ids`, `notes` |
| `precompliance-results.json` | `schema_version`, `tests`, `notes`; rows key `test_id`, `method`, `limit`, `measured`, `unit`, `status`, `evidence_ids`, `notes` |

CSV headers are exact:
`measurements.csv` is
`measurement_id,unit_id,procedure_id,metric,value,unit,minimum,maximum,status,evidence_ids,notes`;
`audio-measurements.csv` is
`measurement_id,case_id,channel,metric,value,unit,minimum,maximum,status,evidence_ids,notes`;
`timing-memory.csv` is
`measurement_id,case_id,metric,value,unit,limit,status,evidence_ids,notes`;
and `landed-prototype-cost.csv` is
`cost_id,category,quantity,unit_gbp_ex_vat,extended_gbp_ex_vat,vat_gbp,freight_gbp,duty_gbp,landed_gbp,evidence_ids,notes`.

The manufacturing-package schema is also closed. `assembly-bom.csv` has exact
RFC-4180 header
`reference,quantity,value,manufacturer,mpn,footprint,dnp,substitute_policy,notes`;
references are unique, sorted naturally, and every fitted schematic reference
appears exactly once. `positions.csv` has exact header
`reference,value,footprint,side,x_mm,y_mm,rotation_deg,notes`; fitted placement
references match the BOM exactly, side is `top` or `bottom`, and decimal fields
use the fixed KiCad-origin projection recorded in the manifest.
`manufacturing/manifest.json` keys `schema_version`, `board_sha256`,
`design_rules_sha256`, `schematic_sha256`, `layer_decision_sha256`,
`generator_sha256`, `archive_members`, `outputs`, `notes`. Archive-member rows
key `path`, `sha256`, `bytes`, `media_type`, `role`, sort by path, and enumerate
every normalized ZIP member. Output rows have the same keys and enumerate only
`assembly-bom.csv` and `positions.csv`. The manifest cannot hash itself or the
containing ZIP. `checksums.sha256` instead contains exactly four lowercase
SHA-256 lines, in path order, for `aius-carrier-manufacturing.zip`,
`assembly-bom.csv`, `manifest.json`, and `positions.csv`; it does not hash
itself. ZIP member order, permissions, compression method and timestamps are
fixed by the checked-in generator, so two clean generations are byte-identical.

Every ordinary verdict record keys `schema_version`, `stage`, `verdict`,
`input_sha256`, `failures`, `notes`. `input_sha256` is a lexically keyed object
of lowercase hashes; failure rows key `record_id`, `reason_code`, sort by both
fields, and project one diagnostic each. Exact stage/hash keys/verdicts/reason
codes are:

| Stage | Exact `input_sha256` keys | Verdict enum | Reason-code enum |
|---|---|---|---|
| `h2a-gate` | `board-outline`, `cable-envelope`, `component-placement` | `eligible`, `blocked` | `assembly-access`, `cable-bend`, `guide-fit`, `insertion-access`, `outline-over-limit`, `placement-collision` |
| `h2b-gate` | `h2a-verdict`, `impedance-calculations`, `layer-decision`, `routing-study`, `vendor-stackup` | `two-layer`, `four-layer-required`, `blocked` | `impedance-unmet`, `mechanical-input-blocked`, `plane-cut`, `return-path`, `route-length`, `vendor-stackup-unavailable`, `via-count` |
| `h3c-quote` | `h2b-verdict`, `landed-prototype-cost`, `manufacturing-manifest`, `order-configuration`, `prototype-quotation`, `risk-register`, `should-cost` | `quoted`, `blocked` | `landed-cost-unreconciled`, `quantity-mismatch`, `quote-incomplete`, `substitution-unapproved` |
| `h4-gate` | `defects`, `eco-log`, `evt-plan`, `h4-gate-oracle`, `h4-procedure-contract`, `manufacturing-checksums`, `manufacturing-manifest`, `measurements`, `order-authorization`, `unit-01`, `unit-02`, `unit-03` | `pass`, `blocked` | `eco-open`, `ethernet`, `inspection`, `optical`, `power-rail`, `recovery`, `regression`, `reset-safe-state`, `short-circuit`, `spdif`, `thermal`, `usb` |
| `h5-gate` | `audio-measurements`, `eco-log`, `firmware-release`, `h4-verdict`, `h5-gate-oracle`, `h5-test-contract`, `precompliance-results`, `soak-results`, `test-matrix`, `thermal-fault-results`, `timing-memory` | `pass`, `blocked` | `audio-matrix`, `buffer-recovery`, `clock-drift`, `eco-open`, `emc`, `fault`, `headroom`, `latency`, `production-test`, `repeatability`, `soak`, `thermal`, `word-integrity` |

Every projection oracle keys `schema_version`, `stage`, `rules`,
`verdict_rules`, `notes`. Rule rows key `order`, `reason_code`, `source_key`,
`row_collection`, `record_id_field`, `predicate`, `arguments`, `notes`.
`row_collection` and `record_id_field` are JSON Pointer strings or the literal
`$record`; arguments are a closed scalar object. The only predicates are
`fixed-inequality`, `field-not-equal`, `field-nonempty`, `status-not-pass`,
`inclusive-range-fail`, `sum-mismatch`, `count-mismatch`, and
`hash-mismatch`; their evaluation is defined by the fixture-proven coordinator,
not arbitrary expressions. Rules evaluate in numeric order, all matches are
collected, exact duplicate `(record_id, reason_code)` pairs retain the first
match, and the retained set sorts by record ID then reason code. Structure and
reference failures exit 1 before any rule. There is no hidden reason-code
precedence.

The H2a oracle contains these exact source projections:

| Reason | Source rows and condition | Record ID |
|---|---|---|
| `outline-over-limit` | board row; width or height greater than 100000 um | `board_id` |
| `guide-fit` | mechanical criterion with metric `guide-fit` and status not `pass` | `criterion_id` |
| `insertion-access` | mechanical criterion with metric `insertion-access` and status not `pass` | `criterion_id` |
| `assembly-access` | keepout kind `tool-access` or `test-access` with status not `pass` | `keepout_id` |
| `cable-bend` | cable row, or keepout kind `cable-bend`, with status not `pass` | `cable_id` or `keepout_id` |
| `placement-collision` | placement, keepout other than the three kinds above, or collision-check row with status not `pass` | the row's canonical ID |

H2a is `eligible` only for an empty failure set and otherwise `blocked`. The
H2b oracle first projects `mechanical-input-blocked` with fixed record ID
`h2a-verdict` when H2a is not eligible, and
`vendor-stackup-unavailable` with fixed record ID `vendor-stackup` when vendor
status is unavailable. It then maps failed impedance-calculation rows to
`impedance-unmet`; route length above maximum to `route-length`; route via
count above zero to `via-count`; ground-corridor split count above zero to
`plane-cut`; and a false `continuous`, non-zero `neck_count`, or non-zero
`via_transition_count` to `return-path`. Record IDs are respectively
`calculation_id`, `route_id`, or `corridor_id`. Mechanical-input or unavailable
vendor failures select `blocked`. Otherwise any electrical failure selects
`four-layer-required`; an empty set selects `two-layer`. All electrical
failures remain persisted in the four-layer verdict.

The H3c oracle uses fixed IDs `quotation`, `quotation-quantity`, and
`landed-cost`. It projects `quote-incomplete` when `quotation_status` is not
`complete`; `quantity-mismatch` when assembled quantity is not exactly 3 or
bare-board quantity is below 3; and `landed-cost-unreconciled` when the quote
landed value differs from the decimal half-up sum of its component, assembly,
selected-layer board, freight, VAT and duty fields or from
`landed-prototype-cost.csv`. Each substitution whose decision is not
`approved` projects `substitution-unapproved` with its `substitution_id`.
An empty set selects `quoted`; otherwise the result is `blocked`.

H4 and H5 do not invent limits in this early bundle. Their later committed
criteria contracts use the same closed oracle-row shape and bind each allowed
reason code to an exact source artifact, record ID, unit, inclusive bounds and
required status. A missing required result is a failure, not a structural
omission. H4/H5 evaluation is unavailable until those criteria and their
generated projection oracles have committed in H4b and H5b1 respectively.

Structural stages consume the identically named artifact family without writing
a verdict and return 1 on schema/reference failure. `h1-interface`, `h1-stubs`,
`h1-schematic`, `h3-pcb`, `h3-manufacturing`, `h3c-quotation`,
`h3c-structural`, `h4-contract`, `h5-contract`, `h5-firmware-interface`,
`h5-firmware-components`, and `h5-firmware` have no
committable failing verdict;
they must be corrected before their design commit. `h1-stubs --lock` writes only
the exact stub-lock shape below. `h3d-authorization --decision` writes only the
exact authorization shape defined in H3d. The H1a0h fixture corpus exercises
every early-stage field, reason code, verdict, hash mismatch, diagnostic and
exit plus H4/H5 structural shapes. H4b and H5b1 extend it with every fixed
physical-test reason code before those gates become executable.

`validate_hardware.py` accepts `--stage` plus an optional `--record` path. Every
stage first validates structure and references. Verdict stages return 0 for the
named eligible/pass branch, 3 for a structurally valid blocked branch, and 4
only for H2b's structurally valid `four-layer-required` branch. Diagnostics are
sorted unique `AIUS-HW-BLOCKED:<stage>:<record-id>:<reason-code>` lines only.
Exit 4 instead emits sorted unique
`AIUS-HW-FOUR-LAYER:<record-id>:<reason-code>` lines only.
`--record` atomically writes the exact deterministic verdict selected by the
same evaluation; no timestamp or Git identity is permitted.
The validator rejects `h4-gate` until the H4 criteria and oracle hashes resolve,
and rejects `h5-gate` until the equivalent H5 inputs resolve.

H1a's `sheet-interfaces.json` is the explicit P1D-to-manufacturing projection.
Its ordered keys are `schema_version`, `p1d_release_sha256`,
`h0_verdict_sha256`, `p7_verdict_sha256`, `schematic_interface_sha256`,
`reference_schematic_contract_sha256`,
`reference_sheets`, `manufacturing_sheets`, `symbol_bindings`, `deviations`,
`notes`. The validator requires the three gate states, recomputes each gate
hash, and transitively verifies every input hash inside those verdicts before
writing the bridge. It copies every reference sheet ID and ordered hierarchical-pin tuple
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
  buildable: [committed root library and exact manufacturing sheet-interface contract]
  parent-commit: "design(aius): lock carrier schematic interfaces"
```

```yaml
phase: H1a1
unit: single-file-implementation
seams: [committed sheet interface, interface-only KiCad sheets, one-way stub lock]
contract-artifact: stubs-committed
depends-on:
  - electronics/carrier/aius-carrier.kicad_sch
  - electronics/carrier/aius-carrier.kicad_sym
  - electronics/carrier/sheet-interfaces.json
fan-out:
  - name: power-usb-sheet-stub
    owned-paths: [electronics/carrier/sheets/power-usb.kicad_sch]
  - name: optical-spdif-sheet-stub
    owned-paths: [electronics/carrier/sheets/optical-spdif.kicad_sch]
  - name: ethernet-sheet-stub
    owned-paths: [electronics/carrier/sheets/ethernet.kicad_sch]
  - name: teensy-expansion-sheet-stub
    owned-paths: [electronics/carrier/sheets/teensy-expansion.kicad_sch]
coordination-owned-paths:
  - electronics/carrier/sheet-stub-lock.json
coordination: maximum three stub workers; parent runs ERC and h1-stubs validation, then writes the deterministic lock only after both pass
review-intensity: active
commit-checkpoint:
  green:
    - scripts/kicad.sh sch erc electronics/carrier/aius-carrier.kicad_sch
    - python3 scripts/validate_hardware.py --stage h1-stubs --lock electronics/carrier/sheet-stub-lock.json
  buildable: [committed interface-complete ERC-clean manufacturing sheet stubs]
  parent-commit: "design(aius): commit carrier sheet stubs"
```

`sheet-stub-lock.json` keys are `schema_version`, `root_sha256`,
`library_sha256`, `interface_sha256`, `sheet_sha256`, `notes`; the sheet hash
object has exact lexical keys `ethernet`, `optical-spdif`, `power-usb`,
`teensy-expansion`. H1b validation requires the first three hashes unchanged,
all four implemented sheets different from their locked stub hashes, and every
ordered hierarchical pin unchanged. It rejects partial replacement.

```yaml
phase: H1b
unit: single-file-implementation
seams: [committed sheet interface, predeclared symbols, power and safe-state contracts]
contract-artifact: stubs-committed
depends-on:
  - electronics/carrier/aius-carrier.kicad_sch
  - electronics/carrier/aius-carrier.kicad_sym
  - electronics/carrier/sheet-interfaces.json
  - electronics/carrier/sheet-stub-lock.json
  - electronics/carrier/sheets/power-usb.kicad_sch
  - electronics/carrier/sheets/optical-spdif.kicad_sch
  - electronics/carrier/sheets/ethernet.kicad_sch
  - electronics/carrier/sheets/teensy-expansion.kicad_sch
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
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h2a-gate:<record-id>:<reason-code> lines only
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
      expected-diagnostics: sorted exact AIUS-HW-FOUR-LAYER:<record-id>:<reason-code> lines only
      buildable: [approved four-layer stackup after documented electrical failure]
      parent-commit: "design(aius): require four-layer carrier stackup"
    blocked:
      command: python3 scripts/validate_hardware.py --stage h2b-gate --record electronics/feasibility/h2b-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h2b-gate:<record-id>:<reason-code> lines only
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
phase: H3b1
unit: test-surface
seams: [committed PCB, deterministic package-generator interface and fixture proof]
contract-artifact: both
depends-on:
  - electronics/carrier/aius-carrier.kicad_pcb
  - electronics/carrier/aius-carrier.kicad_dru
  - electronics/feasibility/h2b-verdict.json
  - evidence/hardware/schemas/manufacturing-package-schema.json
fan-out:
  - name: manufacturing-package-generator
    owned-paths:
      - scripts/build_manufacturing_package.py
      - scripts/test_build_manufacturing_package.py
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_build_manufacturing_package.py
  buildable: [fixture-proven deterministic manufacturing-package generator]
  parent-commit: "feat(aius): add manufacturing package generator"
```

```yaml
phase: H3b2
unit: component-behind-interface
seams: [committed deterministic generator, normalized fabrication outputs]
contract-artifact: stubs-committed
depends-on:
  - scripts/build_manufacturing_package.py
  - electronics/carrier/aius-carrier.kicad_pcb
  - electronics/carrier/aius-carrier.kicad_dru
  - electronics/feasibility/h2b-verdict.json
  - evidence/hardware/schemas/manufacturing-package-schema.json
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
  green:
    - python3 scripts/build_manufacturing_package.py --write
    - python3 scripts/build_manufacturing_package.py --check
    - python3 scripts/validate_hardware.py --stage h3-manufacturing
    - clean-room DFM DFA and release audit
  buildable: [deterministic frozen manufacturing package]
  parent-commit: "design(aius): generate carrier manufacturing package"
```

```yaml
phase: H3c1
unit: phase-task
seams: [frozen manufacturing package, vendor quotation capture, normalized quotation]
contract-artifact: stubs-committed
depends-on:
  - manufacturing/aius-carrier-manufacturing.zip
  - manufacturing/assembly-bom.csv
  - manufacturing/positions.csv
  - manufacturing/manifest.json
  - manufacturing/checksums.sha256
fan-out:
  - name: quotation-acquisition
    owned-paths:
      - docs/reference/datasheets.mdAIUS-PCBWAY-EVT-QUOTE/quote.pdf
      - docs/reference/datasheets.mdAIUS-PCBWAY-EVT-QUOTE/preview.pdf
      - evidence/cost/order-configuration.json
      - evidence/cost/prototype-quotation.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h3c-quotation
  buildable: [retained normalized vendor quotation without purchase authority]
  parent-commit: "docs(aius): retain prototype quotation"
```

```yaml
phase: H3c2
unit: phase-task
seams: [committed quotation, landed-cost normalization, H0 cost and risk refresh]
contract-artifact: stubs-committed
depends-on:
  - electronics/feasibility/h2b-verdict.json
  - evidence/cost/order-configuration.json
  - evidence/cost/prototype-quotation.json
  - evidence/cost/functional-bom.csv
fan-out:
  - name: quotation-cost-normalization
    owned-paths:
      - evidence/cost/landed-prototype-cost.csv
      - evidence/cost/should-cost.csv
      - evidence/cost/risk-register.csv
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h3c-structural
  buildable: [quotation-reconciled landed cost should-cost and risk]
  parent-commit: "docs(aius): reconcile prototype landed cost"
```

```yaml
phase: H3c3
unit: single-file-implementation
seams: [retained quotation, reconciled cost and risk, deterministic quote verdict]
contract-artifact: stubs-committed
depends-on:
  - electronics/feasibility/h2b-verdict.json
  - evidence/cost/order-configuration.json
  - evidence/cost/prototype-quotation.json
  - evidence/cost/landed-prototype-cost.csv
  - evidence/cost/should-cost.csv
  - evidence/cost/risk-register.csv
  - manufacturing/manifest.json
fan-out:
  - name: quotation-verdict
    owned-paths: [evidence/cost/h3c-verdict.json]
review-intensity: active
commit-checkpoint:
  branches:
    quoted:
      command: python3 scripts/validate_hardware.py --stage h3c-quote --record evidence/cost/h3c-verdict.json
      expected-exit: 0
      buildable: [quoted three-unit prototype package without purchase authority]
      parent-commit: "docs(aius): approve prototype quotation for user review"
    blocked:
      command: python3 scripts/validate_hardware.py --stage h3c-quote --record evidence/cost/h3c-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h3c-quote:<record-id>:<reason-code> lines only
      buildable: [complete quotation evidence blocked from user acceptance]
      parent-commit: "docs(aius): record blocked prototype quotation"
```

H3c never places an order. After the quoted checkpoint, H3d pauses for explicit
user acceptance of the exact landed amount and substitutions. One bounded
recorder receives that explicit answer and writes `order-authorization.json`,
with ordered keys
`schema_version`, `decision`, `quotation_sha256`, `authorized_quantity`,
`shipping_postcode`, `notes`; decision is `authorized` or `cancelled`, quantity
is 3 only when authorized, and postcode is `M20 5PG`. No automation infers this
decision.

```yaml
phase: H3d
unit: single-file-implementation
seams: [committed quote, explicit user decision, purchase authority]
contract-artifact: spec-only
depends-on:
  - evidence/cost/h3c-verdict.json
  - evidence/cost/prototype-quotation.json
  - evidence/cost/order-configuration.json
fan-out:
  - name: order-authorization-recorder
    owned-paths: [evidence/cost/order-authorization.json]
coordination: dispatch pauses until the user supplies the literal authorized or cancelled decision; the recorder cannot choose or broaden it
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
phase: H4a
unit: single-file-implementation
seams: [frozen manufacturing package, fixed EVT procedures and acceptance limits]
contract-artifact: spec-only
depends-on:
  - evidence/cost/order-authorization.json
  - manufacturing/manifest.json
  - manufacturing/checksums.sha256
  - electronics/carrier/aius-carrier.kicad_sch
  - electronics/carrier/aius-carrier.kicad_pcb
fan-out:
  - name: evt-procedure-contract
    owned-paths: [evidence/hardware/criteria/h4-procedure-contract.json]
review-intensity: active
commit-checkpoint:
  green: [closed-schema validation and clean-room electrical acceptance-limit audit]
  buildable: [immutable pre-execution EVT procedure and acceptance contract]
  parent-commit: "test(aius): lock EVT acceptance contract"
```

`h4-procedure-contract.json` keys `schema_version`, `manufacturing_sha256`,
`schematic_sha256`, `pcb_sha256`, `procedures`, `acceptance`, `notes`.
Procedure IDs are exactly `evt-01-visual-continuity`,
`evt-02-current-limited-power`, `evt-03-reset-safe-states`,
`evt-04-usb-cable-mechanical`, `evt-05-usb-bridge`,
`evt-06-native-ethernet`, `evt-07-optical-loopback-gating`,
`evt-08-raw-spdif-bypass`, `evt-09-spdif-rx-tx-simultaneous`, and
`evt-10-carrier-recovery`, in that order. Procedure rows key `procedure_id`,
`steps`, `equipment_ids`, `acceptance_ids`, `notes`. Acceptance rows key
`acceptance_id`, `procedure_id`, `source_artifact`, `metric`, `unit`,
`minimum`, `maximum`, `required_status`, `reason_code`, `evidence_ids`,
`rationale`, `notes`; exactly one or both inclusive decimal bounds are present,
or a non-numeric required status is present. Reason codes are limited to the
H4 enum. Every procedure has at least one acceptance row, every permitted H4
reason code except the aggregate `eco-open` and `regression` codes has at least
one row, and every numerical limit cites the schematic,
datasheet or manufacturing record from which it was derived. The contract has
no result or pass/fail observations and becomes immutable before any unit test.

```yaml
phase: H4b
unit: test-surface
seams: [immutable EVT criteria, deterministic H4 projection oracle]
contract-artifact: both
depends-on:
  - evidence/hardware/criteria/h4-procedure-contract.json
  - evidence/hardware/schemas/h4-schema.json
fan-out:
  - name: h4-gate-oracle
    owned-paths:
      - evidence/hardware/oracles/h4-gate-oracle.json
      - scripts/fixtures/hardware-gates/h4-contract-cases.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_hardware.py --stage h4-contract
  buildable: [fixture-proven H4 projection with no authored observations]
  parent-commit: "test(aius): bind EVT gate projections"
```

The H4 oracle contains exactly one rule per acceptance row. Its source key and
record-ID field come from that row; missing results, status mismatch, or a value
outside either inclusive bound projects the row's fixed reason code and
`acceptance_id`. It adds `eco-open` for each ECO whose verification status is
not closed and `regression` for each required procedure absent from any of the
three unit records. All other reason codes arise only from an acceptance row.

```yaml
phase: H4c1
unit: single-file-implementation
seams: [immutable procedure and projection oracle, executable EVT plan]
contract-artifact: spec-only
depends-on:
  - evidence/cost/order-authorization.json
  - manufacturing/manifest.json
  - manufacturing/checksums.sha256
  - evidence/hardware/criteria/h4-procedure-contract.json
  - evidence/hardware/oracles/h4-gate-oracle.json
fan-out:
  - name: evt-plan
    owned-paths: [evidence/evt-results/evt-plan.json]
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h4-plan
  buildable: [hash-bound executable EVT plan with no observations]
  parent-commit: "test(aius): establish EVT execution plan"
```

```yaml
phase: H4c2
unit: test-surface
seams: [committed EVT plan, three unit records and shared measurements]
contract-artifact: stubs-committed
depends-on:
  - evidence/evt-results/evt-plan.json
  - evidence/hardware/criteria/h4-procedure-contract.json
  - evidence/hardware/oracles/h4-gate-oracle.json
fan-out:
  - name: evt-unit-execution
    owned-paths:
      - evidence/evt-results/unit-01.json
      - evidence/evt-results/unit-02.json
      - evidence/evt-results/unit-03.json
      - evidence/evt-results/measurements.csv
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h4-units
  buildable: [three structurally complete EVT unit and measurement records]
  parent-commit: "test(aius): record EVT unit measurements"
```

```yaml
phase: H4c3
unit: component-behind-interface
seams: [committed unit evidence, normalized defect and ECO disposition]
contract-artifact: stubs-committed
depends-on:
  - evidence/evt-results/evt-plan.json
  - evidence/evt-results/unit-01.json
  - evidence/evt-results/unit-02.json
  - evidence/evt-results/unit-03.json
  - evidence/evt-results/measurements.csv
fan-out:
  - name: evt-defect-disposition
    owned-paths:
      - evidence/evt-results/defects.json
      - evidence/evt-results/eco-log.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h4-structural
  buildable: [complete EVT evidence and explicit defect disposition]
  parent-commit: "test(aius): classify EVT defects and ECOs"
```

```yaml
phase: H4c4
unit: single-file-implementation
seams: [complete EVT evidence, deterministic H4 verdict]
contract-artifact: stubs-committed
depends-on:
  - evidence/evt-results/evt-plan.json
  - evidence/evt-results/unit-01.json
  - evidence/evt-results/unit-02.json
  - evidence/evt-results/unit-03.json
  - evidence/evt-results/measurements.csv
  - evidence/evt-results/defects.json
  - evidence/evt-results/eco-log.json
  - evidence/hardware/criteria/h4-procedure-contract.json
  - evidence/hardware/oracles/h4-gate-oracle.json
  - manufacturing/manifest.json
  - manufacturing/checksums.sha256
  - evidence/cost/order-authorization.json
fan-out:
  - name: h4-verdict
    owned-paths: [evidence/evt-results/h4-verdict.json]
review-intensity: active
commit-checkpoint:
  branches:
    pass:
      command: python3 scripts/validate_hardware.py --stage h4-gate --record evidence/evt-results/h4-verdict.json
      expected-exit: 0
      buildable: [working EVT carrier with three inspections and one repeated full bring-up pass]
      parent-commit: "test(aius): approve carrier EVT result"
    blocked:
      command: python3 scripts/validate_hardware.py --stage h4-gate --record evidence/evt-results/h4-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h4-gate:<record-id>:<reason-code> lines only
      buildable: [complete EVT evidence with defects and ECO gate]
      parent-commit: "test(aius): record blocked carrier EVT result"
```

## H5: Full audio integration and release decision

Prove the complete path:

`TV/source -> TOSLINK -> IEC 61937 -> DTS Core -> custom downmix/EQ -> 48 kHz PCM TOSLINK -> existing DAC`

Test stereo PCM input, DTS Core 5.1, format transitions, optical loss, source sample-rate changes, unsupported bursts, reset, brownout, Ethernet activity, and user-control changes. Measure S/PDIF word integrity, recovered-clock behaviour, clock drift, buffering, underrun and overrun recovery, end-to-end latency, carrier detection and gating, jitter sensitivity, output channel mapping, and sustained decoder/DSP headroom.

Complete thermal, fault, long-soak, enclosure, basic pre-compliance EMC, production-test, and repeatability checks. Record every ECO and refresh the cost model with actual yield, parts, assembly, rework, freight, and enclosure results.

```yaml
phase: H5a0
unit: component-behind-interface
seams: [proven decoder/DSP interfaces, carrier pin/state contract, product state-machine interface]
contract-artifact: both
depends-on:
  - evidence/evt-results/h4-verdict.json
  - evidence/capability-results/p7-verdict.json
  - evidence/capability-results/p8-verdict.json
  - electronics/carrier/sheet-interfaces.json
fan-out:
  - name: product-firmware-interface
    owned-paths:
      - firmware/src/product/audio_pipeline.h
      - firmware/src/product/carrier_control.h
      - firmware/src/product/product_state_machine.h
      - firmware/contracts/product-contract.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h5-firmware-interface
  buildable: [committed immutable product firmware interfaces]
  parent-commit: "feat(aius): define product firmware interfaces"
```

`product-contract.json` keys are `schema_version`, `audio_pipeline`,
`carrier_control`, `product_state_machine`, `evidence_ids`, `notes`. Each of the
three interface objects keys `header_sha256`, `input_types`, `output_types`,
`state_ids`, `fault_ids`, `notes`; type and ID arrays are sorted projections of
the capability protocol, H1 sheet interface and P1C operating/fault states.
Headers and contract are immutable through H5a1/H5a2.

```yaml
phase: H5a1
unit: component-behind-interface
seams: [immutable product headers, three disjoint implementations]
contract-artifact: stubs-committed
depends-on:
  - firmware/src/product/audio_pipeline.h
  - firmware/src/product/carrier_control.h
  - firmware/src/product/product_state_machine.h
  - firmware/contracts/product-contract.json
fan-out:
  - name: audio-pipeline-implementation
    owned-paths: [firmware/src/product/audio_pipeline.cpp]
  - name: carrier-control-implementation
    owned-paths: [firmware/src/product/carrier_control.cpp]
  - name: product-state-machine-implementation
    owned-paths: [firmware/src/product/product_state_machine.cpp]
coordination: maximum three component agents; none edits an interface header or another component
review-intensity: active
commit-checkpoint:
  green:
    - platformio test -d firmware -e native
    - platformio run -d firmware -e teensy41-release
    - python3 scripts/validate_hardware.py --stage h5-firmware-components
  buildable: [three buildable product components behind immutable interfaces]
  parent-commit: "feat(aius): implement product firmware components"
```

```yaml
phase: H5a2
unit: phase-task
seams: [three committed product components, firmware entry point, integration regression, release manifest]
contract-artifact: stubs-committed
depends-on:
  - firmware/src/product/audio_pipeline.cpp
  - firmware/src/product/carrier_control.cpp
  - firmware/src/product/product_state_machine.cpp
  - firmware/contracts/product-contract.json
fan-out:
  - name: product-firmware-integration
    owned-paths:
      - firmware/src/main.cpp
      - firmware/test/product/test_product.cpp
      - firmware/build/release-manifest.json
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

`release-manifest.json` keys are `schema_version`, `firmware_sha256`,
`build_environment`, `source_sha256`, `product_contract_sha256`,
`h4_verdict_sha256`, `p7_verdict_sha256`, `p8_verdict_sha256`,
`sheet_interface_sha256`, `notes`; source hashes are a lexically keyed object and
the manifest contains no timestamp.

```yaml
phase: H5b0
unit: single-file-implementation
seams: [committed product firmware, fixed system cases and measurement limits]
contract-artifact: spec-only
depends-on:
  - evidence/evt-results/h4-verdict.json
  - firmware/build/release-manifest.json
  - firmware/contracts/product-contract.json
  - evidence/hardware/schemas/h5-schema.json
fan-out:
  - name: system-test-contract
    owned-paths: [evidence/hardware/criteria/h5-test-contract.json]
review-intensity: active
commit-checkpoint:
  green: [closed-schema validation and clean-room audio timing thermal and fault-limit audit]
  buildable: [immutable pre-execution system test and acceptance contract]
  parent-commit: "test(aius): lock system acceptance contract"
```

`h5-test-contract.json` keys `schema_version`, `firmware_release_sha256`,
`h4_verdict_sha256`, `product_contract_sha256`, `cases`, `acceptance`, `notes`.
Case IDs are exactly `sys-01-pcm-stereo`, `sys-02-dts-5-1`,
`sys-03-format-transition`, `sys-04-optical-loss`,
`sys-05-sample-rate-change`, `sys-06-unsupported-burst`,
`sys-07-reset-brownout`, `sys-08-ethernet-concurrency`,
`sys-09-control-change`, `sys-10-buffer-recovery`,
`sys-11-clock-jitter`, `sys-12-thermal-fault`, `sys-13-soak`,
`sys-14-precompliance`, and `sys-15-production-repeatability`, in that order.
Case rows key `case_id`, `input_format`, `fault_mode`, `expected_state`,
`acceptance_ids`, `notes`. Acceptance rows use the H4 acceptance-row shape with
`case_id` replacing `procedure_id`; their reason codes are limited to the H5
enum. Every case and every permitted H5 reason code except the aggregate
`eco-open` code has at least one acceptance row. Each numerical word-integrity,
drift, latency, headroom, thermal, soak,
EMC and repeatability limit cites its engineering source and states its unit and
inclusive range. The contract has no observations and is immutable before any
H5 measurement.

```yaml
phase: H5b1
unit: test-surface
seams: [immutable system criteria, deterministic H5 projection oracle]
contract-artifact: both
depends-on:
  - evidence/hardware/criteria/h5-test-contract.json
  - evidence/hardware/schemas/h5-schema.json
fan-out:
  - name: h5-gate-oracle
    owned-paths:
      - evidence/hardware/oracles/h5-gate-oracle.json
      - scripts/fixtures/hardware-gates/h5-contract-cases.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/test_validate_hardware.py --stage h5-contract
  buildable: [fixture-proven H5 projection with no authored observations]
  parent-commit: "test(aius): bind system gate projections"
```

The H5 oracle contains exactly one rule per acceptance row. Missing cases or
measurements, status mismatch, or values outside inclusive bounds project the
fixed acceptance ID and reason code. It additionally maps each non-closed ECO
to `eco-open` using its ECO ID. An empty failure set selects `pass`; any failure
selects `blocked`. No execution phase may add a case, reason code or limit.

```yaml
phase: H5b2a
unit: test-surface
seams: [immutable end-to-end criteria and oracle, audio and timing measurements]
contract-artifact: stubs-committed
depends-on:
  - evidence/evt-results/h4-verdict.json
  - firmware/src/product/audio_pipeline.cpp
  - firmware/src/product/carrier_control.cpp
  - firmware/src/product/product_state_machine.cpp
  - firmware/src/main.cpp
  - firmware/build/release-manifest.json
  - evidence/hardware/criteria/h5-test-contract.json
  - evidence/hardware/oracles/h5-gate-oracle.json
fan-out:
  - name: audio-timing-validation
    owned-paths:
      - evidence/system-results/test-matrix.json
      - evidence/system-results/audio-measurements.csv
      - evidence/system-results/timing-memory.csv
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h5-audio-timing
  buildable: [complete audio timing clock buffer and headroom observations]
  parent-commit: "test(aius): record system audio and timing results"
```

```yaml
phase: H5b2b
unit: test-surface
seams: [immutable end-to-end criteria and oracle, environmental and precompliance results]
contract-artifact: stubs-committed
depends-on:
  - evidence/system-results/test-matrix.json
  - evidence/hardware/criteria/h5-test-contract.json
  - evidence/hardware/oracles/h5-gate-oracle.json
fan-out:
  - name: environmental-validation
    owned-paths:
      - evidence/system-results/thermal-fault-results.json
      - evidence/system-results/soak-results.json
      - evidence/system-results/precompliance-results.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h5-environmental
  buildable: [complete thermal fault soak and precompliance observations]
  parent-commit: "test(aius): record system environmental results"
```

```yaml
phase: H5b2c
unit: single-file-implementation
seams: [complete system observations, explicit ECO disposition]
contract-artifact: stubs-committed
depends-on:
  - evidence/system-results/test-matrix.json
  - evidence/system-results/audio-measurements.csv
  - evidence/system-results/timing-memory.csv
  - evidence/system-results/thermal-fault-results.json
  - evidence/system-results/soak-results.json
  - evidence/system-results/precompliance-results.json
fan-out:
  - name: system-eco-disposition
    owned-paths:
      - evidence/system-results/eco-log.json
review-intensity: active
commit-checkpoint:
  green:
    - python3 scripts/validate_hardware.py --stage h5-structural
  buildable: [complete system evidence and explicit ECO disposition]
  parent-commit: "test(aius): classify system ECOs"
```

```yaml
phase: H5b2d
unit: single-file-implementation
seams: [complete system evidence, deterministic release verdict]
contract-artifact: stubs-committed
depends-on:
  - evidence/evt-results/h4-verdict.json
  - firmware/build/release-manifest.json
  - evidence/hardware/criteria/h5-test-contract.json
  - evidence/hardware/oracles/h5-gate-oracle.json
  - evidence/system-results/test-matrix.json
  - evidence/system-results/audio-measurements.csv
  - evidence/system-results/timing-memory.csv
  - evidence/system-results/thermal-fault-results.json
  - evidence/system-results/soak-results.json
  - evidence/system-results/precompliance-results.json
  - evidence/system-results/eco-log.json
fan-out:
  - name: h5-verdict
    owned-paths: [evidence/system-results/h5-verdict.json]
review-intensity: active
commit-checkpoint:
  branches:
    pass:
      command: python3 scripts/validate_hardware.py --stage h5-gate --record evidence/system-results/h5-verdict.json
      expected-exit: 0
      buildable: [validated personal-build design]
      parent-commit: "test(aius): approve system validation"
    blocked:
      command: python3 scripts/validate_hardware.py --stage h5-gate --record evidence/system-results/h5-verdict.json
      expected-exit: 3
      expected-diagnostics: sorted exact AIUS-HW-BLOCKED:h5-gate:<record-id>:<reason-code> lines only
      buildable: [complete system evidence with release blocked]
      parent-commit: "test(aius): record blocked system validation"
```

## Release assumptions

- This is a personal, non-commercial project. Licence concerns are provenance footnotes, not engineering gates; preserve third-party notices.
- Passing the software capability gate does not approve PCB manufacture.
- Two layers are preferred for cost but cannot compromise USB, Ethernet, chassis return, or reset safety.
- Panel machining and CNC work are customer-supplied.
- Compliance certification and production commercialization are outside scope.
