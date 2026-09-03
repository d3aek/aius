---
title: Aius optical-only prototype and remote validation
type: plans
scope: component
component: aius
subtype: experiment
status: in-progress
tags: [aius, optical-spdif, prototype, ethernet, validation]
updated: 2026-09-03
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

**Sourcing decision (2026-07-27): the RS PRO ORJ-5/OTJ-5 pair replaces the
unavailable Cliff modules, chosen for availability.** Purchased parts: receiver
[RS 805-1671](https://uk.rs-online.com/web/p/fibre-optic-connectors/8051671),
transmitter
[RS 805-1680](https://uk.rs-online.com/web/p/fibre-optic-connectors/8051680);
datasheets at `https://uk.rs-online.com/web/p/fibre-optic-connectors/8051671`
and `https://uk.rs-online.com/web/p/fibre-optic-connectors/8051680`. Verified
against the datasheets: pin functions match the schematic symbols exactly
(receiver 1 = Vout, 2 = GND, 3 = Vcc; transmitter 1 = GND, 2 = Vcc, 3 = Vin —
note the two orders are NOT the same), Vcc 2.7–5.5 V so the 3.3 V rail is in
range, 16 Mbps NRZ, receiver V_OH ≥ 2.4 V and transmitter V_IH ≥ 2.0 V — the
exact figures `connectivity/optical-spdif.json` §levels already assumes — and
the same application precautions the carrier already carries (0.1 µF within
7 mm of each jack, 30 pF GND-to-Vout on the receiver, ESD-sensitive).
**Open gate (H2/H3): validate the Gerber against the RS PRO PCB layout
drawings** (3 × Ø0.8 mm pins on 2.54 mm pitch, 3 × Ø2.0 mm pegs, 9.20/6.00/3.90
geometry, page 4 of each datasheet) before any board order — the
`aius:Cliff_ORJ5_TOSLINK` / `aius:Cliff_OTJ5_TOSLINK` footprints were drawn
from the Cliff drawings and must be re-checked against the RS PRO ones. The
cost-model anchor rows name the RS PRO parts since 2026-09-03 (price not
re-quoted in-repo); bench wiring for the first
real signal test: [`../guides/proto-optical-wiring.md`](../guides/proto-optical-wiring.md).

## Firmware carrier management — the bench TX gate

Contracted 2026-07-27 (user-confirmed). **The bench build must hold the
optical output dark until there is something to transmit, preserving the
carrier board's electronically-enforced policy in firmware: no input carrier
and no replay → no output carrier.**

**Shipped 2026-07-27** (`9114418c4`): the predicate lives behind
`firmware/lib/aius_replay/include/aius_tx_gate.h`, the `tx-gate` native suite
covers the truth table, `tx_gate` is served on `GET /api/status`, and the
bench guide's boot caveat was narrowed to the static-construction blip as
contracted. The contract below is retained as the record of what was built.

Prebuild-gated 2026-07-27; the contract below carries the gate's forced
adjustments.

- **Policy (portable, one predicate):** TX is permitted iff the S/PDIF input
  carrier is locked (`SPDIF_SRPC` LOCK bit — the same single volatile read
  that feeds the status surface) OR the replay source is
  `SWITCHING_TO_REPLAY` or `REPLAY` (prebuffering counts, so the sink gets
  lock lead time before audio; `ERROR` does not count). Evaluated per epoch
  in thread context; no hysteresis in v1 (the electronic gate sees the same
  flapping). **Accepted residue, stated:** nothing auto-stops an EOF'd
  replay — its source stays `REPLAY` until an operator stop or carrier
  displacement — so a drained replay keeps a silence carrier up. That is the
  source-predicate's honest behaviour; the state-based alternative would cut
  the final ~43 ms still draining the 2048-frame tx24 ring. The carrier
  board's electronic gate will face the identical policy question.
- **Actuator now (bench):** pad 14's IOMUXC mux. The SPDIF_OUT routing is
  `CORE_PIN14_CONFIG = 3`, written by `AudioOutputSPDIF3::begin()` (NOT by
  `config_spdif3()`, which touches clocks only). Dark = GPIO low via direct
  register writes — `CORE_PIN14_PORTCLEAR`, `CORE_PIN14_DDRREG` set, then
  `CORE_PIN14_CONFIG = 5` — and lit = one store of `3`. **Bare
  `pinMode(14, OUTPUT)` is forbidden:** it rewrites the pad's PADCONFIG
  (DSE(7) + SION) that S/PDIF has run on at reset-default since item 3's
  measurements. The S/PDIF peripheral, PLL, transmit DMA and the replay
  clock-responsibility handover keep running untouched; the tx24 ring keeps
  draining into a dark pad exactly as the on-stop analysis already
  describes. The gate module is the pad's sole owner after boot (verified:
  no other CORE_PIN14/pinMode/digitalWrite site in the firmware).
- **Actuator later (carrier):** the same predicate drives `TX_ENABLE`
  (pad 39, currently untouched anywhere in firmware) into the 1G126's OE.
  One boolean, two actuators; the bench path is not throwaway.
- **Boot window, honestly stated:** the gate is closed **from `setup()`
  onward** (an early dark-set precedes everything else there, and the
  reboot drain closes it before reset). It cannot be closed earlier: the
  static `AudioOutputSPDIF3` constructor runs `begin()` at ~300 ms after
  every reset with DMA already streaming zeroed buffers, so an unavoidable
  ~0.1–2 ms silence-carrier blip exists at static-construction on every
  boot and reboot, with the pad undriven (reset-default keeper) before
  that. No startup hook can precede the constructor, and patching the
  vendored snapshot is forbidden. The HTTP proof channel polls after the
  blip and structurally cannot observe it — recorded so the carrier-board
  release review reads the truth. The bench guide's boot caveat is
  NARROWED to this blip, never deleted.
- **Degraded modes:** in recovery-safe the scheduler never reaches the
  evaluation checkpoint, so the gate stays frozen dark from setup() —
  intended: recovery-safe also mutes live carrier passthrough. An epoch
  aborting before the evaluation leaves the gate stale for that epoch,
  bounded by the 8 s watchdog.
- **Evaluation site:** checkpoint 7 (`REPLAY`), in `platform_replay()`
  after `replay_clock_service()` — both policy inputs are already read
  there and the reconciler's same-epoch transitions are visible. No new
  budget row (a boolean and at most three register stores). The status
  JSON serves the **latched** per-epoch decision and latched inputs, never
  re-deriving lock at request time, so JSON and pad cannot disagree within
  a poll.
- **Observability + the move-together set:** a `tx_gate` object (latched
  `open`, `carrier_locked`, `replay_running`) joins the HTTP status JSON.
  Moving together in the same commit: the `http_status_json()` literal,
  `swagger.json`'s `Status` schema, `swagger.test.js` assertions, and the
  new native policy suite registered in `firmware/scripts/test.sh` AND its
  self-checking `registry` suite. v1 is deliberately event-free (the
  cutover suite auto-classifies new event types as counter-edges with
  structural requirements this phase does not need). The 99-byte UDP
  payload is NOT touched. Known adjacent drift, surfaced by the gate and
  NOT this phase's to fix: `sd_diag` and `replay_diag` are served by
  `/api/status` but absent from swagger's `Status` schema.
- **Proof:** native truth-table suite for the predicate; `registry`,
  `optical-cutover` and the other affected native suites stay green; flash
  + on-device check that the gate reads closed from setup() onward, a
  replay start opens it (during prebuffer), stop closes it — all over
  HTTP, no jacks needed.
- **Bench-guide consequence:** the §What the bench omits caveat narrows to
  the static-construction blip once this lands.

## PSRAM flight recorder — contracted 2026-07-27

**Shipped 2026-07-27/28** (`66cff2aa1`, `0053de59e`): portable core in
`firmware/lib/aius_recorder/`, the `recorder` native suite, the HTTP surface,
and the committed WAV puller `firmware/tools/aius-recorder-pull.py`. The PSRAM
chips are now fitted and the recorder took its first real capture on
2026-07-28 (defect 27's overrun flood, caught live by the overrun trigger).
Two contract details evolved on hardware after the gate: staging shrank to
16 KiB after defect 26's OCRAM-heap exhaustion (recorded below), then moved
into the fitted PSRAM entirely — the post-link memory guard now pins **zero
OCRAM recorder staging** (`firmware/test/memory/verify_target.py`) — and the
download header stamps the capture-time sample rate instead of pinning 48 kHz
(fixed 2026-07-31). The contract below is retained as the record.

Designed in interview 2026-07-27. **A continuous audio flight recorder in
the 16 MB PSRAM: the ring records whenever the chips are fitted, and
FREEZES itself on detection of the faults it exists to explain** —
preserving pre-trigger history, which is the whole diagnostic value: this
board once shipped a 1.4-semitone pitch error with every counter clean,
and an armed-after-the-fact capture records aftermath, not cause. Wear was
examined and dismissed: PSRAM is DRAM behind QSPI, no endurance limit.

Prebuild-gated 2026-07-27 (GO-WITH-ADJUSTMENTS); the contract below
carries the gate's forced adjustments, the two load-bearing ones being
**gap-honest rather than gapless** (a mount-class epoch stall exceeds any
affordable staging depth) and the **named PSRAM placement mechanism** (an
`EXTMEM` static array is silently wrong on 8 MB boards).

Decisions from the interview, as adjusted:

- **Model: flight recorder.** Continuous circular ring; freeze on trigger
  or operator command, hold a post-roll, then stay frozen until an
  operator thaw/rearm. One-shot windows fall out for free (operator
  freeze).
- **Recording is gap-honest, not gapless.** Staging is sized for the
  measured *normal* worst case; mount-class (~1 s first SD `BEGIN`) and
  watchdog-class (8 s) epoch stalls are NOT ridden out. When staging is
  full the ISR drops the block and counts a ring-position-stamped GAP —
  it never blocks and never overwrites unconsumed staging. Gap count and
  last-gap position are status-surface and frozen-header fields.
- **Triggers (all v1, each independently maskable): block overrun,
  transmit underrun, carrier loss, and limiter gain reduction past a
  configured dB for a configured hold** — plus the operator. Trigger
  metadata (which, when, ring position) is part of the frozen state.
  Precise, testable definitions below.
- **Tap: the output wire words** (the committed planes item 3's tap
  proved ARE the wire at 24-bit) — full ~43 s window. An input-side tap
  is an explicitly contracted FOLLOW-UP slice, not v1.
- **Egress: raw pull.** Token-gated freeze/thaw plus a bounded
  offset-addressed read of the frozen ring over the existing HTTP
  transport, with a JSON header route; a committed host script
  reassembles a WAV. Rationale recorded: you can LISTEN to a capture,
  offline analysis is unbounded, the firmware surface stays minimal, and
  volatile PSRAM means the data must leave the device anyway. On-device
  summaries deliberately deferred.
- **Presence: auto-detected, never assumed.** The Teensy core's boot
  probe exposes the fitted size (`external_psram_size`, valid before
  `setup()`); absent chips → the feature reports `absent` on its status
  surface and every other recorder route refuses cleanly — a bare board
  must never fault (the MPU maps absent PSRAM RW, so a stray access
  returns garbage silently; only the presence latch protects it). One
  presence bool gates the tap and the drain (the `g_meters_active`
  pattern).

### Memory and placement

- **Ring: raw base `0x70000000`, length `external_psram_size` MiB, read
  once at init.** NO `EXTMEM` static array — ERAM is declared 32 MB and
  `.bss.extram` is NOLOAD, so a 16 MB array links without error on an
  8 MB board and corrupts the core's smalloc pool arithmetic. With no
  EXTMEM data the core's extmem heap pool spans the whole chip, so the
  companion rule is part of this contract: **this firmware never calls
  `extmem_malloc`/`extmem_calloc`/`extmem_realloc`**, policed by a text
  check in the recorder's native suite.
- **Format: interleaved stereo `s32le` frames (L,R), 24-bit LSB-aligned**
  — 8 bytes/frame, so `ring_frames = psram_bytes / 8`: 16 MB →
  2,097,152 frames ≈ 43.7 s at 48 kHz; 8 MB → half. The ISR copies
  planes into staging; the DRAIN interleaves (epoch context, where the
  loop is free).
- **PSRAM is never zeroed** (explicit core TODO): the frozen header's
  valid extent (frames recorded since arm) bounds what the host may
  reassemble, so an unwrapped ring never serves power-up garbage.
- **Staging: 16 KiB OCRAM ring (4 × 4 KiB committed blocks ≈ 42.7 ms at
  384 KiB/s)** — amended at build from the gate-reviewed 64 KiB, because
  the real constraint surfaced on hardware (defect 26): OCRAM's heap
  must keep ≥ ~96 KiB free for NativeEthernet's boot-time mallocs, and
  64 KiB of staging squeezed it under that and made the image
  unbootable. 42.7 ms is 4× the measured stacked epoch worst case
  (5.5 ms post-cache SD primitive + the 2 + 2 + 1 ms budget rows);
  mount-class stalls were always gaps under either size. The arrays
  live in their own heap-carved `.bss.aius_staging` section (never
  `DMAMEM`) so their OCRAM cost is visible to the linker's new 128 KiB
  heap-floor ASSERT — an over-grown ring now fails the build, not the
  boot. Cursors and control state in DTCM, never OCRAM (the boot ROM
  scribbles it at reset), init before the transmit interrupt arms — the
  tx24 ring's discipline.
- **EXTMEM is never touched from audio context** (standing hot-path
  tie-breaker): the ISR writes staging only; the epoch drains staging
  into PSRAM from thread context, where a FlexSPI2 stall costs nothing
  audible.

### Drain

- Hosted at the **REPLAY checkpoint (7), after `tx_gate_service()`**,
  as a drain byte quota carried on the REPLAY budget row (the budget
  table is one row per checkpoint). **Cap 8 KiB per epoch**: 8 KiB ×
  ~115 epoch/s ≈ 920 KiB/s ≥ 2 × the 384 KiB/s fill rate, so a
  post-stall backlog clears rather than merely holding level.
- A drain slice is a pure copy/interleave into write-back-cached EXTMEM
  with **no cache maintenance** — nothing DMAs the PSRAM; both sides are
  CPU accesses through the same D-cache (MPU maps the region WBWA).

### Freeze state machine

One writer per transition, the measure machine's discipline; states
`ABSENT` / `RECORDING` / `POSTROLL` / `DRAINING` / `FROZEN`:

- `RECORDING → POSTROLL` — **ISR**, at a block boundary, for the
  block-evaluable triggers (overrun edge, underrun edge, GR hold) and
  for token acknowledgement (below). Trigger metadata — which, when,
  ring position — is stored before the state store (stage/commit).
- **Epoch-detected triggers set a freeze token, never the state**:
  carrier loss (an epoch-sampled level), the operator, and the
  **underrun fallback** — immediately after `tx_gate_service()` the
  epoch edge-detects the tx24 underrun counter under that same epoch's
  latched gate (which also suppresses replay-stop transients: the stop
  closes the gate in the same evaluation) and sets the freeze token,
  because a producer stall is the flagship underrun case and advances
  the counter at up to 48 k/s while `enqueue()` never runs again. The
  ISR underrun edge is first-wins on metadata (it carries the precise
  ring position). The ISR acknowledges a token at its next block; after
  a bounded number of epochs with the block counter unchanged the epoch
  reclaims and performs the transition itself — the measure machine's
  cancel/reclaim shape, needed because the underrun and carrier-loss
  triggers fire precisely when blocks may stop arriving.
- `POSTROLL → DRAINING` — ISR (post-roll block count reached; end
  position latched), or epoch reclaim under the same
  block-counter-unchanged bound.
- `DRAINING → FROZEN` — epoch, once the drain has flushed the staging
  remnant to PSRAM. **The capture is not readable until FROZEN.**
- `FROZEN → RECORDING` — epoch (thaw/rearm), incrementing
  `freeze_generation`.
- A second trigger during `POSTROLL` is first-wins and counted.

### Trigger definitions

- **Carrier loss: the FALLING EDGE of the per-epoch lock sample**,
  qualified by the tx-gate predicate's inputs (armed only while the
  device should be carrying); ring-position precision is one epoch. A
  level trigger would freeze instantly on the bench norm (no source
  connected) and always during replay.
- **Block overrun / TX underrun: edge-detected against baselines that
  RESYNC ON DECREASE** (`POST /api/counters` clears `g_overruns`; a
  clear costs one missed edge, never a spurious freeze). The underrun
  counter advances in the transmit DMA ISR at higher NVIC priority and
  free-runs as silence substitution while the gate is closed, so the
  underrun trigger is **qualified by the tx-gate predicate** too. Both
  are read as counters, never as shared state.
- **Limiter GR: `state.gain < 10^(−dB/20)`**, the linear threshold
  precomputed cold at config apply; hold is an integer count of
  512-frame blocks. (`aius_dsp_limiter_gain_reduction_db()` takes a
  libm log10 and may not be called per block — the meter's own rule.)
- Trigger config is **RAM-only** (defaults at boot: overrun, underrun
  and carrier enabled; GR disabled until configured), echoed on the
  status surface and in the frozen header — no SD schema, no fifth key
  list to drift.

### HTTP surface

- Routes, ALL token-gated (`X-Aius-Sd-Auth`, 401 without — the read
  carries audio and the header reveals trigger state; `aius_auth.h`'s
  weak-boundary disclaimer applies): `GET /api/recorder` (status +
  frozen-header JSON), `POST /api/recorder/freeze`,
  `POST /api/recorder/thaw`, `PUT /api/recorder/config` (PUT, matching
  the DSP-config precedent),
  `GET /api/recorder/data?offset=<bytes>&length=<bytes>&generation=<n>`
  — both in BYTES, `offset` frame-aligned (a multiple of 8, else 400).
- **Data chunks are offset+length addressed and idempotent** (the client
  retries after the 100 ms response-deadline drop a mount-class stall
  causes), length capped at **64 KiB** (~512 connections per full pull,
  clear of the measured FNET TIME_WAIT cliff), served as
  `AIUS_HTTP_APP_IMMEDIATE` bodies streamed straight from the frozen
  PSRAM ring — exact Content-Length, no SD job, and **no `app_sd_busy`
  test** (an sd_busy gate would make the recorder unusable during
  replay, when refill owns ~61 % of epochs). Data reads dispatch only in
  `FROZEN` (409 otherwise).
- **`freeze_generation` increments on every freeze AND thaw**; data
  chunks require it and a mismatch answers 409, so a thaw/rearm between
  chunks can never splice two captures into one WAV.
- Header fields (the metadata route, never prepended to binary): sample
  rate, encoding (`s32le`, 24-bit LSB-aligned, interleaved LR), fitted
  MB + ring capacity in frames, valid extent, oldest-frame position,
  trigger {which, when, ring position}, post-roll frames captured, gap
  count + last-gap position, `freeze_generation`, `boot_generation`,
  and the armed trigger config. The committed host script reassembles a
  WAV (`s24le`) and must complete within one boot session.
- **A reboot destroys a frozen capture** — recorder state is internal
  RAM and the boot probe resets the chips; thaw-by-reboot is
  destructive by design. Volatility is the contract; the data must
  leave the device.
- **Post-roll: `postroll_blocks` config field, default 188 blocks
  (≈ 2 s), bounds 0–938 (≈ 10 s)**, counted in ISR blocks.

### Move-together set + proof

- **The move-together set** for the HTTP surface: routes + swagger +
  contract suite + a native suite registered in
  `firmware/scripts/test.sh` and the registry, exactly as tx-gate did —
  plus the `extmem_malloc` text check above.
- **Proof:** native suite drives the state machine's transition table
  (including token acknowledge vs reclaim interleavings), the trigger
  edge/resync/qualification cases, the ring/chunk arithmetic at 0, 8 and
  16 MB, and gap accounting; all affected native suites stay green;
  `teensy41-optical` builds; flash + on-device ABSENT-path check (the
  chips are not yet soldered): status reports `absent`, every other
  recorder route refuses cleanly, audio path unaffected.

## Control boundary

Ethernet carries only framed control requests, state, counters, and test results.
It must not carry or replay the audio stream. That clause forbids LIVE audio
transport; bounded pulls of recorded data — replay vector upload/download, the
flight recorder's frozen capture — are explicitly permitted, as the shipped
replay file surface already reads it. The first implementation should
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

## DSP control protocol and remote dashboard

**Superseded 2026-07-31** (`c8f0c2571`, B9): the the app dashboard was
built as planned, then retired in favour of the device's own SD-hosted control
surface (`firmware/sd/www/` — status, DSP editor, replay manager, upgrade
page, regression-tested under `test/sd-editor/`). `the companion dashboard app` survives as
exactly the kept contract: `swagger.json`, its vitest suite, and the
`web.json` mount serving the OpenAPI document. The section below is the
record of the superseded design; the OpenAPI paragraph at its end remains
true.

The dashboard is a normal upstream repository app under `the companion dashboard app`: static frontend
assets mounted by `web.json` and shared `mr-*` kit chrome. The frontend is
planned to talk directly to the Teensy's bounded HTTP control surface; no .NET
API module is needed for the prototype. The firmware will serve no dashboard
assets and has no JavaScript runtime. The existing UDP protocol remains
available to the host fixture runner until HTTP parity and concurrent-soak
acceptance are complete.

The DSP control contract is deliberately preset-based:

| operation | request payload | response/telemetry |
| --- | --- | --- |
| `set_mode` | one byte: `flat`, `speech`, or `bass-reduced` | ACK, then status reports the active preset |
| `start_test` | empty | idempotent ACK; status test flag and event transition |
| `stop_test` | empty | idempotent ACK; status test flag cleared |
| `clear_counters` | empty | ACK; counters reset at a new test epoch |
| `get_status` | empty | coherent network, audio, DSP, and identity snapshot |
| `get_events` | cursor `u32` | bounded event records and next cursor |
| `reboot` | empty | ACK before reset; never automatically retried |

Preset changes are applied at a muted/audio-safe boundary and reset EQ state so
old filter history cannot leak between modes. The dashboard displays the board
identity, boot epoch, active preset, test state, carrier/format/rate, counters,
overruns, and event history. It shows stale/offline state after bounded request
failure and never presents a control as successful without a matching ACK.

The initial dashboard uses the host's existing authenticated app boundary. Target
address, expected MAC, and a fixture-network warning are explicit API
configuration. Authentication and multi-user access control use the host's
existing protection conventions rather than a dashboard-specific login.

The app also serves a committed OpenAPI document at
`/apps/aius/swagger.json`, with `/apps/aius/openapi/v1.json` as a conventional
alias. It describes the device-facing HTTP routes, request bodies, response
schemas, bounded error responses, and the reboot semantics. The document uses a
server variable for the configured device host rather than pretending the app
and device share an origin. It is app documentation and tooling input; the
firmware does not embed Swagger or OpenAPI machinery.

## Minimal HTTP API review and implementation plan

The HTTP surface will be a transport adapter over shared command/state handlers,
not a second DSP implementation. **Shipped:** it is in the `teensy41-optical`
image (`firmware/lib/aius_http/` behind the runtime's bounded HTTP checkpoint),
and the surface has since grown well past this original table — DSP config,
replays, SD maintenance, OTA, measure, meters, recorder. The current contract
is `swagger.json`; the table below is the original browser-facing
contract, kept as the record:

| method and path | body | result |
| --- | --- | --- |
| `GET /api/status` | none | bounded JSON snapshot |
| `GET /api/events?cursor=<u32>` | none | bounded JSON event page and cursor |
| `POST /api/mode` | `{"mode":"flat"\|"speech"\|"bass-reduced"}` | ACK plus active mode |
| `POST /api/mix` | `{"mix":"standard"\|"lfe-minus10"\|"clipfree"}` | ACK plus active mix |
| `POST /api/start` | none | idempotent ACK |
| `POST /api/stop` | none | idempotent ACK |
| `POST /api/clear` | none | ACK plus new test epoch |
| `POST /api/reboot` | none | ACK, then controlled reboot |

Adversarial findings and resolutions:

1. A blocking `readStringUntil`, `while (client.connected())`, or unbounded
   JSON parser could starve audio servicing. The server therefore accepts one
   request per connection, has fixed header/body limits, a short idle timeout,
   and a byte/time budget per `loop()` call. Slow or malformed clients are
   discarded without changing audio state.
2. HTTP connection and response buffers must not overlap S/PDIF, DMA, decoder,
   or EQ storage. All HTTP buffers are static; audio buffers remain in internal
   RAM. Status is copied from a coherent snapshot, and commands enter the same
   safe-boundary state transitions as UDP commands.
3. TCP reliability removes UDP response loss but does not make commands safe to
   repeat. Mode, mix, start, stop, and clear remain idempotent. Reboot sends its
   response before disabling TX and restarting, and the browser treats the
   missing post-reboot response as expected.
4. A dashboard polling loop can create connection churn. The dashboard uses a
   bounded status interval, one in-flight request, and event cursor polling; it
   must not issue requests faster than the firmware's service budget.
5. The full FNET HTTP/CGI/filesystem service is not justified for this surface.
   Start with a small `EthernetServer` adapter and measure flash, RAM, maximum
   request latency, and audio processing margin on the Ethernet target.

The initial numeric limits are contract values: one active client, 1024 total
request bytes, 512 header bytes, 256 body bytes, a 150 ms inter-byte timeout,
a 2 ms maximum HTTP service budget per `loop()` call, and 2048 response bytes.
The implementation must expose counters for budget exhaustion and client
rejection so these limits can be evaluated during soak.

The clean-room review was initially RED. Its firmware prerequisites are now
implemented: telemetry counters are snapshotted, event retrieval is chronological
with overflow accounting, mode/mix changes are deferred to the audio update
boundary, UDP work is bounded, and the HTTP adapter has explicit service limits.
The physical gate has since been taken on the bench: the board runs with a real
optical source and sink, and the 2026-08-03 loaded soak passed on the
single-context image (`6950202b7`), with ~8 h of continuous production DTS
decode recorded in the [open-work register](aius-open-work-register.md).

### HTTP implementation phases

#### Phase 4: HTTP parser and response codec

**status:** implemented and pushed; native malformed/truncation/body/HTTP-version tests green

**unit:** `single-file-implementation` for bounded HTTP parsing/formatting  
**seams:** byte stream ↔ validated request; state snapshot ↔ JSON response  
**contract-artifact:** this route table plus parser limits and error matrix  
**fan-out:** one owner; no parallel edits to the HTTP adapter files  
**review-intensity:** malformed request, slow client, oversized body, duplicate command, and response-bound tests  
**commit-checkpoint:** native HTTP parser tests green; Ethernet target still builds

Implement a transport-independent parser/formatter first. It must recognize
only the documented methods and paths, reject HTTP versions/features outside the
contract, bound headers and bodies, and produce deterministic status codes for
400/404/405/413/408/500 cases. No command may execute until the complete request
is validated. The parser tests must cover malformed, truncated, slow, abandoned,
pipelined, duplicate, replayed, unsupported-method, and oversized requests,
proving that none changes audio state. This phase is blocked until the numeric
limits above are represented by named constants and test fixtures.

#### Phase 5: Teensy HTTP adapter

**status:** implemented and pushed; `teensy41-optical` builds with HTTP enabled. Real-board latency and audio-margin measurements remain pending.

**unit:** `component-behind-interface` for the foreground Ethernet adapter  
**seams:** `EthernetServer` client lifecycle ↔ parser; parsed command ↔ shared control handlers  
**contract-artifact:** committed parser tests and fixed buffer/timeout constants  
**fan-out:** one owner for the adapter; test additions use disjoint HTTP test files  
**review-intensity:** audio scheduling, coherent telemetry, connection churn, and reset safety  
**commit-checkpoint:** clean `teensy41-optical` build, native protocol suites, and HTTP loopback tests green

First refactor the shared network service so both UDP and HTTP have strict
bounded parse/send work. Poll HTTP with the 2 ms per-loop budget, serve JSON
only, close each connection after one response, and expose counters for HTTP
requests, rejected requests, active-client drops, and service-budget overruns.
The audio path must not call Ethernet code or allocate memory.

#### Phase 6: concurrent HTTP/optical soak

**status:** host exerciser and JSONL evidence are implemented and pushed; real-board acceptance remains pending.

**unit:** `test-surface` for host HTTP exerciser and soak evidence  
**seams:** HTTP client ↔ device control; repeated commands ↔ audio soak oracle  
**contract-artifact:** JSONL evidence schema with control and audio timelines  
**fan-out:** one owner for the HTTP exerciser and evidence assertions  
**review-intensity:** long-run scheduling and failure-injection review  
**commit-checkpoint:** simulator tests green and real-board soak acceptance recorded

Extend the runner with a concurrent HTTP workload: poll status and events while
repeatedly exercising every non-reboot endpoint, including mode/mix changes at
safe intervals, start/stop idempotence, counter clearing, malformed requests,
and deliberate connection abandonment. Record request latency, HTTP errors,
audio block/overrun counters, carrier state, output transitions, resets, and
event-ring gaps in one JSONL timeline.

Acceptance requires that the concurrent run introduce no unexplained audio
overruns, decoder failures, carrier transitions, output-gate faults, resets, or
counter corruption compared with a control soak of equal duration. HTTP service
budget overruns and rejected clients may occur only within documented limits;
they must not alter audio safety behavior. Reboot is tested separately because
it intentionally interrupts the audio session.

Evidence must compare equal-duration control and concurrent runs and include
HTTP-disabled, idle-polling, nominal-valid-load, maximum-valid-load,
malformed-load, and connection-churn cases. It records maximum HTTP service
latency, interrupt-masking duration, audio deadline margin, network drops,
telemetry sequence gaps, and event-ring overflow in addition to the existing
audio counters. A real-board run exercises every endpoint except reboot in the
concurrent case; reboot is a separate controlled test.

#### Phase 7: dashboard and OpenAPI contract

**status:** Utilities app, direct-device controls, OpenAPI document, Vitest contract test, and Playwright scenario are implemented and pushed.

**unit:** `test-surface` for the standalone Utilities app and its API description  
**seams:** `web.json` mount ↔ app shell; OpenAPI document ↔ device HTTP contract  
**contract-artifact:** committed `swagger.json` validated against the route table  
**fan-out:** one owner for the Aius app; no parallel edits to its manifest or shell  
**review-intensity:** launcher placement, direct-device URL configuration, schema drift, and endpoint discoverability  
**commit-checkpoint:** app frontend tests and OpenAPI validation green; dashboard remains usable without the device

Add the standalone Aius app under the Utilities tier using the existing `mr-*`
shell and menu-strip conventions. Its frontend uses the documented HTTP routes
directly, exposes a configurable device host, and marks the device offline or
stale without fabricating successful control state. Ship and validate
`swagger.json` alongside the app so the dashboard, API explorer, and manual
clients share one contract artifact.

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
- The Teensy PSRAM (16 MB as two chips, since fitted) is deliberately kept out
  of the real-time audio path: EXTMEM is never touched from audio context (the
  standing hot-path tie-breaker). Internal RAM carries audio blocks, decoder
  state, Ethernet buffers, and bounded telemetry. The anticipated diagnostic
  capture feature became the flight recorder above; OTA activation also stages
  its verified image copy in PSRAM.
  **Considered and declined (2026-07-27): PSRAM as the home for www assets
  and DSP presets.** PSRAM is volatile, so the SD card stays the system of
  record for both regardless; the only defensible variant is a boot-built
  read cache for `/sd/www/` assets, which buys page loads immune to
  `503 sd_busy` during replay refill at the cost of a coherence layer
  (upload invalidation) and a second "which tree is truth" question the
  storage design deliberately avoids. Reopen only if `sd_busy` page
  unavailability during replay proves to be a real operator pain on the
  bench. **First live sighting 2026-07-27:** during the editor slice 4
  smoke, one page load racing a replay-refill burst answered `503`; a
  retry loaded. One data point, not yet pain. The strong PSRAM use case remains long diagnostic capture: the
  item 3 window is 16,384 frames because it lives in 128 KiB of `DMAMEM`;
  16 MB holds ~40 s of 24-bit stereo and needs no persistence.

### Deliberately excluded

- RCA/coax S/PDIF input or output.
- Analogue audio circuitry.
- Ethernet audio transport.
- Production enclosure and final connector mechanics.
- Manufacturing release or PCB order authorization.

## Firmware phases

**Status 2026-09-03: stays `in-progress`; design decision D10 applied
(descope what serves no purpose, keep what does;
[`aius-outstanding-work-2026-09.md`](aius-outstanding-work-2026-09.md)
§Decisions).** Descoped 2026-09-03: **NTP synchronization** — the status frame
carries a permanent stub (`payload[22] = 0 /* NTP validity: not implemented */`,
`firmware/src/optical_main.cpp:2362`), its only consumer is
`tools/net/aius_net_runner.py:72`, and the firmware has no wall clock by design
([`aius-tls-termination.md`](aius-tls-termination.md) §Expiry: no RTC, no NTP,
expiry reported not enforced), so nothing depends on it. **Deterministic
static-fallback address** — no reference in `firmware/src`, `firmware/lib` or
`firmware/tools/net`; the board lives on the DHCP LAN and DHCP-first bring-up,
discovery and lease renewal are proven live (register: first observed DHCP
renewal, 2026-08-04). Kept, still owed: **UDP protocol authentication** — the
mutating verbs `SET_MODE` / `SET_MIX` / `START_TEST` / `STOP_TEST` /
`CLEAR_COUNTERS` on 4242 are unauthenticated (`optical_main.cpp:6221-6239`;
`SET_MODE`/`SET_MIX` are refused as retired but the others act), and
[`aius-tls-termination.md`](aius-tls-termination.md) §What it does not buy names
the gap and its open question 2 awaits a ruling — TLS plan Q2 depends on it.
**Raw-bypass test rows** (row 5) — the bypass mux `SN74LVC1G3157` exists only on
the manufactured carrier ([`aius-h1-schematic.md`](aius-h1-schematic.md) sheet
optical-spdif, [`aius-hardware-spec.md`](aius-hardware-spec.md) §Selected
parts), so the rows are re-homed to EVT bring-up H4
([`aius-h2-h3-manufacturing.md`](aius-h2-h3-manufacturing.md) §Not in this
plan; hardware-spec build gate 5). Acceptance gates below are annotated with
what each proved; one line stays open — the Ethernet-control-loss half of the
state-table gate. Everything else in this plan is delivered.

Current implementation status (updated 2026-08-04): all three phases below are
delivered and the runtime is live on bench hardware with real optical input.
The pipeline now decodes DTS Core, AC-3 and E-AC-3, live output follows the
source rate (32/44.1/48 kHz), and the physical acceptance this plan gated on
has been substantially executed — electrical 14→15 loopback proven 2026-07-28,
first full optical loop measured 2026-07-29, E-AC-3 acceptance passed,
and the 2026-08-03 loaded soak passed with ~8 h of continuous production DTS
decode. Still open from this plan's scope: NTP synchronization and the
deterministic static-fallback address are unimplemented (bring-up is
DHCP-only), the UDP protocol remains unauthenticated (SD/recorder/OTA HTTP
writes are token-gated), and the raw-bypass test rows are carrier-board-only.

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
still enumerated as `16c0:0486`. This materially favors a RawHID transport stall
over a decoder failure. The optical image was restored after the reproduction.

The failure mode is actionable: a dropped fragment leaves the device cursor
mid-frame while the host waits for that fragment's ACK. Subsequent frames start
at offset zero and are rejected forever. The P6 receiver now resets its cursor
on an out-of-order fragment, and the host retries the complete access unit once
after a fragment timeout or NACK. Native capability tests and the P6 build pass;
the hardware retry path remains to be re-run when the board is next placed in
bootloader mode. That validation is now complete: the repaired P6 image passed
100 iterations and 4,000/4,000 frame submissions with zero failures, including
the former failure boundary, before the optical image was restored.

### Phase 1: remote-control transport

**status:** done — `lib/aius_net` UDP frame codec + network supervisor behind
`src/aius_network_teensy.cpp`, `net`/`net-runner` native suites green

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

**status:** done — the `teensy41-optical` image is the live runtime (Phase 14
cutover), with real S/PDIF capture/classification/decode and the firmware TX
gate; confirmed on hardware with a real carrier

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

**status:** done except static fallback — `tools/net/aius_net_runner.py` (UDP
discovery/status) and `tools/net/aius_http_runner.py` (HTTP soak) exercise a
running board with JSONL evidence; the deterministic static-fallback address
was never implemented (DHCP-only bring-up)

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

Execution state (2026-08-04): the bench has exercised the bring-up, carrier,
PCM, DTS and carrier-loss rows in some form — records live in the
[open-work register](aius-open-work-register.md) and the delivery plans. Row
5's raw bypass is carrier-board-only, and the static-fallback halves of rows
8–9 are unimplemented.

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
- DHCP-first startup, discovery, lease renewal, ~~NTP correlation when available,
  and deterministic static fallback~~ all pass on the isolated fixture network.
  (2026-09-03: DHCP-first, discovery and renewal proven live on the bench LAN —
  register, first observed DHCP renewal 2026-08-04; the NTP and static-fallback
  halves are descoped under D10, see §Firmware phases.)
- USB-controlled optical smoke test passes before USB removal.
- All remote commands pass with USB physically disconnected.
- DTS input produces processed PCM at the optical output, not a raw DTS bypass.
- Carrier loss, reset, watchdog, and Ethernet-control-loss behavior matches the
  documented state table. (2026-09-03: MET for carrier loss, reset and watchdog
  — [`interfaces-and-states.md`](../reference/interfaces-and-states.md) §Fault
  scenarios rows `optical-carrier-loss`, `reset`, `firmware-crash`, with live
  proof in [`aius-network-hardening.md`](aius-network-hardening.md) defect 17
  "Live proof" and the recorder H2 provocation; **Ethernet-control-loss:
  UNPROVEN** — the state table has no control-loss row and no bench run has
  exercised test-matrix row 10. Not a descope: control loss is a real state.
  This is the one open line of this plan.)
- Eight-hour observation completes with no unexplained reset, malformed packet,
  buffer overrun, or output-enable fault. (2026-09-03: MET by the 10.0 h loaded
  soak of 2026-08-04 and the 8.00 h witness watch on `930f003c6e6e` — register
  §Open defects — runtime, periodic-watchdog entry; run on the bench, not an
  isolated fixture network.)
- The DSP stretch goal is separately accepted only if its host oracle, target
  timing, state continuity, and allocation gates pass; otherwise it remains
  disabled without blocking the optical prototype.

No manufacturing release follows automatically from these gates. The result is a
prototype validation record that informs the later schematic, layout, and DRC
release gates.
