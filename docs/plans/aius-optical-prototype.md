---
title: Aius optical-only prototype and remote validation
type: plans
scope: aius
subtype: experiment
status: proposed
tags: [aius, optical-spdif, prototype, ethernet, validation]
updated: 2026-07-21
toc: true
---

# Aius optical-only prototype and remote validation

## Objective

Validate the complete physical optical path with a real S/PDIF/DTS source and
sink while retaining a control and telemetry channel after USB is disconnected.
The Ethernet link is a service interface only; DTS and PCM remain on the optical
S/PDIF path.

The prototype uses the existing Aius signal contract:

```
TOSLINK RX → ORJ-5 logic output → Teensy SPDIF input
Teensy SPDIF output → OTJ-5 logic input → TOSLINK TX
```

The reset-safe transmit gate and raw-bypass mux remain fitted because they make
the physical test safe and provide an independent optical loop-through mode.
The RS PRO ORJ-5/OTJ-5 pair replaces the unavailable Cliff modules, subject to
recording their final datasheet pinout and package variant in the BOM.

## Control boundary

Ethernet carries only framed control requests, state, counters, and test results.
It must not carry or replay the audio stream. The first implementation should
use a small authenticated-by-local-network request/response protocol over UDP,
with a monotonically increasing request ID and a fixed-size status response.
TCP is unnecessary for the initial fixture and would add connection-state failure
modes.

Required commands:

| command | purpose |
| --- | --- |
| `get_status` | firmware version, protocol version, state, carrier/PLL state, format, sample rate, counters, last error |
| `set_mode` | processed, raw optical bypass, muted; every transition passes through muted |
| `clear_counters` | reset test counters and error latches |
| `start_test` | start a bounded on-device observation window |
| `stop_test` | end the observation window safely and mute output if requested |
| `reboot` | controlled software restart, with TX disabled first |

The status response must include input carrier loss count, malformed burst count,
PCM/DTS detection counts, output-enable transitions, watchdog/reset reason, and a
sequence/timestamp pair. A host test runner must be able to fail closed on a
missing, stale, malformed, or out-of-order response.

## Hardware prototype scope

### Required

- Socketed Teensy 4.1 with Ethernet-populated PHY.
- RS PRO ORJ-5 optical receiver at 3.3 V.
- RS PRO OTJ-5 optical transmitter at 3.3 V.
- Existing `SN74LVC1G3157` raw-bypass mux.
- Existing `SN74LVC1G126` reset-safe TX gate.
- Existing pull-downs for processed-path selection and TX disable.
- Per-device decoupling and the ORJ-5 output loading capacitor specified by its
  datasheet.
- TOSLINK input and output connectors, power entry, and test points for
  `SPDIF_RX`, `SPDIF_TX`, `TX_GATED`, `3V3`, and `GND`.
- PJRC Ethernet Kit or equivalent native-Ethernet magjack and its documented
  centre-tap capacitor.
- USB connection retained as a bring-up/programming fallback, but not required
  during source/sink validation.

### Deliberately excluded

- RCA/coax S/PDIF input or output.
- Analogue audio circuitry.
- Ethernet audio transport.
- Production enclosure and final connector mechanics.
- Manufacturing release or PCB order authorization.

## Firmware phases

### Phase 1: remote-control transport

**unit:** Ethernet control/telemetry adapter and protocol tests  
**seams:** Ethernet service loop ↔ portable pipeline state interface  
**contract-artifact:** protocol header, request/response examples, status-field definitions  
**fan-out:** one owner for protocol framing and one owner for the Teensy Ethernet adapter  
**review-intensity:** adversarial review of framing, timeout, replay, malformed-packet, and fail-closed behavior  
**commit-checkpoint:** host protocol tests and instrumented Teensy build green

Add the Ethernet service behind a narrow C-compatible interface. Keep the codec,
IEC 61937 parser, and DTS decoder independent of Arduino/Ethernet types. The host
must have a deterministic protocol test that covers truncation, invalid lengths,
unknown commands, duplicate IDs, wraparound, and stale responses.

### Phase 2: optical runtime integration

**unit:** real S/PDIF capture, classification, processing/bypass selection, and gated output  
**seams:** Teensy S/PDIF ISR/DMA ↔ Aius pipeline; pipeline state ↔ Ethernet status snapshot  
**contract-artifact:** state-transition table plus source/sink acceptance record  
**fan-out:** one owner for runtime integration; no parallel edits to the same firmware entry point  
**review-intensity:** electrical reset-state review and transition fault injection  
**commit-checkpoint:** clean PlatformIO build, native tests, and USB-controlled optical smoke test green

The firmware must boot with TX disabled, expose the physical carrier/lock state,
classify PCM versus IEC 61937 DTS Core, and never emit raw compressed data in the
processed mode. Raw bypass is diagnostic-only and must require the same muted
transition already defined in the Aius state contract.

### Phase 3: remote fixture runner

**unit:** host-side Ethernet validation runner  
**seams:** UDP protocol ↔ test oracle; real source/sink observation ↔ recorded evidence  
**contract-artifact:** machine-readable test manifest and JSONL result schema  
**fan-out:** one owner for the runner and evidence schema  
**review-intensity:** independent review of pass/fail criteria and evidence completeness  
**commit-checkpoint:** runner dry-run against simulator and real-board smoke test green

The runner must support a fixed target address, a bounded command timeout, status
polling, test labels, and append-only JSONL evidence. It must remain useful with
USB absent and must report loss of Ethernet control as a test failure rather than
silently continuing.

## Representative test matrix

1. **Electrical bring-up:** verify 3.3 V rail, module current, ORJ-5 logic idle,
   TX gate idle, and no optical output while reset/booted.
2. **Optical carrier:** connect a known PCM optical source; confirm lock, format,
   sample-rate report, stable counters, and output optical carrier.
3. **PCM integrity:** play a deterministic stereo test file through the source,
   compare sink output lock/rate/channel behavior, and record drop/error counters.
4. **DTS Core path:** play a known DTS Core source; verify DTS classification,
   decoder activity, processed PCM output, and absence of raw DTS at the output.
5. **Raw bypass:** select bypass remotely while muted; verify source optical
   frames reach the sink without enabling the processed path.
6. **Carrier loss/recovery:** remove and restore the optical input; verify TX is
   disabled or muted according to the state contract, counters increment once per
   qualified event, and recovery returns to the correct mode.
7. **Remote operation without USB:** disconnect USB, retain Ethernet, repeat
   status, mode, carrier-loss, and reboot tests.
8. **Control-link failure:** unplug Ethernet during active audio; verify audio
   behavior is unchanged and the host records the control-plane outage.
9. **Long observation:** run the existing soak-style repeated status and optical
   observation workload for at least eight hours, recording resets, carrier
   losses, malformed bursts, output transitions, and missed responses.
10. **Power/reset safety:** assert reset and cycle power while source and sink are
    connected; verify no unintended optical transmission during boot or reset.

## Acceptance gates

- Native parser/decoder and protocol tests pass with zero allocation violations.
- Clean PlatformIO build succeeds for the Ethernet-enabled target.
- USB-controlled optical smoke test passes before USB removal.
- All remote commands pass with USB physically disconnected.
- DTS input produces processed PCM at the optical output, not a raw DTS bypass.
- Carrier loss, reset, watchdog, and Ethernet-control-loss behavior matches the
  documented state table.
- Eight-hour observation completes with no unexplained reset, malformed packet,
  buffer overrun, or output-enable fault.

No manufacturing release follows automatically from these gates. The result is a
prototype validation record that informs the later schematic, layout, and DRC
release gates.
