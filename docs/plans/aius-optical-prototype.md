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
use a small request/response protocol over UDP, with a monotonically increasing
request ID and an explicitly versioned response schema. UDP is acceptable for the
fixture only when the board is on a directly connected or isolated test network;
the protocol must not claim to provide security until an authentication mechanism
is added. TCP is unnecessary for the initial fixture and would add
connection-state failure modes.

Required commands:

| command | purpose |
| --- | --- |
| `get_status` | firmware version, protocol version, state, carrier/PLL state, format, sample rate, counters, last error |
| `discover` | answer a bounded LAN discovery request with identity and network state |
| `set_mode` | processed, raw optical bypass, muted; every transition passes through muted |
| `clear_counters` | reset test counters and error latches |
| `start_test` | start a bounded on-device observation window |
| `stop_test` | end the observation window safely and mute output if requested |
| `reboot` | controlled software restart, with TX disabled first |
| `get_events` | retrieve bounded transition/fault events from a cursor |

The status response must include input carrier loss count, malformed burst count,
PCM/DTS detection counts, output-enable transitions, watchdog/reset reason, and a
sequence/timestamp pair. It must also identify the board instance and boot epoch,
so a response from a rebooted or wrong target cannot satisfy an old request. The
wire format must use explicit little-endian widths and a bounded length, either as
a fixed packed schema with a declared size or as versioned fields; a C struct's
native layout is not a wire contract. A host test runner must be able to fail
closed on a missing, stale, malformed, duplicate, replayed, or out-of-order
response.

## Adversarial review — 2026-07-21

The following findings are resolved in the plan before implementation.

### Control protocol findings

1. **UDP loss and duplication:** every request carries a 32-bit request ID and
   every response echoes it, includes the boot epoch, and carries a response
   status. `get_status`, `clear_counters`, `set_mode`, and `stop_test` are
   idempotent. `start_test` is keyed by request ID and cannot create two active
   windows when a retry arrives. `reboot` is acknowledged before reset and is
   not retried automatically after acknowledgement.
2. **Replay and unauthorized control:** the fixture protocol is explicitly
   *unauthenticated* in its first version. The runner must require a configured
   target IP/MAC and the board must reject packets from unconfigured peers. The
   board must be placed on an isolated test network or direct Ethernet link.
   A later authenticated protocol is a release requirement, not an assumption
   hidden in this prototype.
3. **Identity and stale responses:** `get_status` returns protocol version,
   firmware build ID, hardware identity, MAC, boot epoch, uptime, and the current
   request/response sequence. The runner records these at session start and
   rejects changes except across an explicitly expected reboot.
4. **Network configuration:** DHCP is not a valid sole dependency for a
   USB-disconnected test. Use DHCP first with a bounded startup timeout, then a
   deterministic static fallback. The default fixture fallback is
   `192.168.42.42/24`, with no gateway; the host can use `192.168.42.1/24` on a
   direct or isolated test link. The static address is configuration data and
   must be changed before attaching the fixture to a network where it could
   conflict. Link state, DHCP result, selected address, and fallback/conflict
   failures are separate status fields.
5. **DHCP discovery:** because the DHCP address is not known in advance, the
   runner sends a bounded UDP discovery broadcast. A matching board replies
   with its MAC, identity, IP mode, address, protocol version, and boot epoch.
   The runner then pins the selected IP/MAC for the session and rejects other
   responders. Discovery is a fixture convenience, not authentication.
6. **Lease changes and recovery:** renew an active DHCP lease in the foreground
   without changing the address mid-session. After link loss, increment a
   network epoch and repeat DHCP-first startup; use static fallback only if
   DHCP cannot be restored. The event ring records each transition.
7. **Time correlation:** if DHCP supplies an NTP server, use it to obtain a
   best-effort wall-clock offset for evidence timestamps. NTP validity, sync
   age, and offset are telemetry fields. Monotonic uptime, boot epoch, and
   request IDs remain authoritative for ordering, timeout, and stale-response
   decisions; loss of NTP must never affect the audio path or control safety.
8. **Audio-loop interference:** Ethernet polling and packet parsing run in a
   bounded foreground slice. No network call may block the S/PDIF interrupt,
   DMA service, or audio sink. Incoming packets are copied into a bounded queue;
   queue overflow is a telemetry fault and never a reason to drop audio safety
   behavior.

### Telemetry findings

6. **Unknown is not zero:** carrier lock, format, rate, and output state each
   have validity flags. Before the S/PDIF runtime exists, those fields are
   reported as unknown, not as a fabricated stable value.
7. **Counters need definitions:** all counters are monotonic `uint64_t` values
   since boot, with a separate clearable test epoch. Definitions are fixed:
   carrier-loss events are qualified transitions, malformed bursts are parser
   rejects, decode failures are DTS access-unit failures, and output transitions
   count actual gate commands. The response includes audio input words/frames,
   output frames, ring-buffer overruns, and network packet drops.
8. **Snapshots must be coherent:** telemetry is published through a double
   buffer or a short interrupt-safe snapshot section. Multiword counters must
   never be read half-updated by the Ethernet service.
9. **Counters are insufficient evidence:** the firmware maintains a small
   bounded event ring containing timestamp, event type, state, and detail. The
   protocol adds `get_events` with a cursor so the host can reconstruct carrier,
   format, reset, and fault transitions after a polling gap.
10. **External sink observability:** a downstream DAC lock alone cannot prove
    DTS was decoded or that raw DTS was not forwarded. The test fixture must
    record both board telemetry and the sink behavior; raw-versus-processed
    claims require either a second optical capture point or a sink/test receiver
    that identifies IEC 61937 versus PCM.

### Stretch-goal review: basic mix/EQ

The repository already contains allocation-free, tested downmix and biquad EQ
primitives. The stretch goal is therefore feasible as a separate phase, but the
first optical pass must run with a flat/bypass DSP configuration.

The bounded stretch goal is:

- route decoded DTS blocks and PCM blocks through `aius_dsp_process`;
- use the existing default 5.1-to-stereo coefficients;
- expose only a small fixed set of compile-time or protocol-selected presets:
  flat, speech, and bass-reduced;
- report the active preset, clipping count, DSP block count, and processing
  overrun count;
- prove block-boundary EQ state continuity and zero steady-state allocations;
- measure CPU time and memory on the Ethernet-enabled target before enabling it
  for the physical DTS test.

Arbitrary remote biquad coefficients, live coefficient mutation, resampling, and
per-channel user EQ are out of scope. If the DSP path cannot meet the measured
audio deadline or causes any safety-state regression, the prototype remains on
flat downmix/bypass and the stretch goal is recorded as deferred.

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
- The optional 8 MB Teensy PSRAM is deliberately not used for the real-time
  optical path. Internal RAM is sufficient for audio blocks, decoder state,
  Ethernet buffers, and bounded telemetry; PSRAM is reserved for a later
  diagnostic capture feature if required.

### Deliberately excluded

- RCA/coax S/PDIF input or output.
- Analogue audio circuitry.
- Ethernet audio transport.
- Production enclosure and final connector mechanics.
- Manufacturing release or PCB order authorization.

## Firmware phases

Current implementation status: the portable UDP framing, DHCP-first Ethernet
bring-up target, real Teensy S/PDIF input/output objects, IEC 61937 DTS/PCM
pipeline, flat 5.1-to-stereo downmix, versioned status snapshot, and bounded
cursor-based event retrieval are implemented and build-tested. Physical optical
acceptance, fixed EQ presets, NTP synchronization, and authenticated control
remain validation or follow-on work.

### RawHID soak investigation

The first long run was not a valid soak result: after an initially clean
period, failures rose until nearly every submission failed, reaching about
19.2 million failures out of 19.3 million submissions. The audit path was
`/dev/null`, and the host transport previously collapsed device removal,
write/read errors, and timeout into the same failure code. The diagnostic
runner now records the first transport `errno`, runner error, and result status.
The portable simulator soak remains green, so the leading hypotheses were
RawHID/device reset or transport loss, followed by a target-only allocation or
memory-corruption fault. A controlled P6 reproduction reached the same boundary
at approximately 60 iterations: the audit contained only `status=0` frame
results, while the host then stopped progressing in hidraw I/O and the board
still enumerated as `16c0:0486`. This materially favors a USB-over-fibre/KVM
RawHID transport stall over a decoder failure. The optical image was restored
after the reproduction.

The failure mode is actionable: a dropped fragment leaves the device cursor
mid-frame while the host waits for that fragment's ACK. Subsequent frames start
at offset zero and are rejected forever. The P6 receiver now resets its cursor
on an out-of-order fragment, and the host retries the complete access unit once
after a fragment timeout or NACK. Native capability tests and the P6 build pass;
the hardware retry path remains to be re-run when the board is next placed in
bootloader mode.

### Phase 1: remote-control transport

**unit:** Ethernet control/telemetry adapter and protocol tests  
**seams:** Ethernet service loop ↔ portable pipeline state interface  
**contract-artifact:** protocol header, request/response examples, status-field definitions, identity/boot-epoch rules  
**fan-out:** one owner for protocol framing and one owner for the Teensy Ethernet adapter  
**review-intensity:** adversarial review of framing, timeout, retry, replay, identity, malformed-packet, and fail-closed behavior  
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

The physical S/PDIF runtime is a prerequisite for non-unknown carrier, format,
rate, and audio-counter telemetry. The control service must not infer those fields
from command traffic.

### Phase 3: remote fixture runner

**unit:** host-side Ethernet validation runner  
**seams:** UDP protocol ↔ test oracle; real source/sink observation ↔ recorded evidence  
**contract-artifact:** machine-readable test manifest and JSONL result schema  
**fan-out:** one owner for the runner and evidence schema  
**review-intensity:** independent review of pass/fail criteria and evidence completeness  
**commit-checkpoint:** runner dry-run against simulator and real-board smoke test green

The runner must support DHCP discovery, the documented static fallback address,
a bounded command timeout, status polling, test labels, and append-only JSONL
evidence. It must remain useful with USB absent and must report loss of Ethernet
control as a test failure rather than silently continuing.

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
8. **DHCP startup:** attach the board to a DHCP-enabled isolated network; verify
   discovery, lease-selected mode, address reporting, lease renewal, NTP
   correlation when offered, and control after the lease is refreshed.
9. **Static fallback:** remove DHCP service, power-cycle the board, verify the
   bounded fallback address and discovery response, then run remote tests over
   the direct/isolated static subnet.
10. **Control-link failure:** unplug Ethernet during active audio; verify audio
   behavior is unchanged and the host records the control-plane outage.
11. **Long observation:** run the existing soak-style repeated status and optical
   observation workload for at least eight hours, recording resets, carrier
   losses, malformed bursts, output transitions, event-ring gaps, and missed
   responses.
12. **Power/reset safety:** assert reset and cycle power while source and sink are
   connected; verify no unintended optical transmission during boot or reset.
13. **DSP stretch:** with a deterministic PCM/DTS fixture, compare flat output
    against the existing host DSP oracle, then verify each fixed preset, clipping
    behavior, block-boundary continuity, and measured processing margin.

## Acceptance gates

- Native parser/decoder and protocol tests pass with zero allocation violations.
- Clean PlatformIO build succeeds for the Ethernet-enabled target.
- DHCP-first startup, discovery, lease renewal, NTP correlation when available,
  and deterministic static fallback all pass on the isolated fixture network.
- USB-controlled optical smoke test passes before USB removal.
- All remote commands pass with USB physically disconnected.
- DTS input produces processed PCM at the optical output, not a raw DTS bypass.
- Carrier loss, reset, watchdog, and Ethernet-control-loss behavior matches the
  documented state table.
- Eight-hour observation completes with no unexplained reset, malformed packet,
  buffer overrun, or output-enable fault.
- The DSP stretch goal is separately accepted only if its host oracle, target
  timing, state continuity, and allocation gates pass; otherwise it remains
  disabled without blocking the optical prototype.

No manufacturing release follows automatically from these gates. The result is a
prototype validation record that informs the later schematic, layout, and DRC
release gates.
