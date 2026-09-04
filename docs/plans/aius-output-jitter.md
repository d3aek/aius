---
title: Aius output jitter elimination
type: plans
scope: component
component: aius
status: in-progress
tags: [aius, firmware, audio, latency, jitter, monitoring]
updated: 2026-09-04
---

# Aius output jitter elimination

Eliminate the audible output dropouts observed on 2026-08-16 during live 5.1
decode, without adding a frame of output latency. The owner's constraint is
explicit: the current 13 ms 5.1 residence is below the perception floor and
must not grow; the fix is to remove the loop-jitter sources, not to widen the
transmit cushion.

## State — resume here

**Status:** Phases A, B, C, D SHIPPED and RUNNING ON THE DEVICE
(`d5dab0618f4c+teensy41-optical`, flashed 2026-08-16 via HalfKay after the
OTA dark-window brick — see
[`aius-ota-resilience.md`](aius-ota-resilience.md) for that incident and its
plan). The device has since migrated to the A/B layout and runs slot A of
`1dc7cbb80397` (resilience plan, all shipped). Phase E is decomposed and its
governor is BUILT (2026-08-16, host-proven); live verify on the deployed
image is the one owed step. The acceptance run (one 5.1 evening, dashboard
open, gate-open underrun delta 0) has NOT happened yet. The Phase C
lit-carrier SD-bring-up defect is FIXED (resilience Phase 4c) and
field-confirmed.

| Commit | Contents | Proof |
|---|---|---|
| `b4a3914ce` | A (meters window drop) + C (probe gating incl. `pllLocked()` edge fix) + D (gate-open underrun counter) + this plan | native dsp/recorder/tx-gate/audio-service/pipeline; vitest dsp-editor 238 + swagger 11; optical build; adversarial delivery audit to exhaustion |
| `f7e770cf0` | B (second AUDIO pass per epoch) | native runtime oracle re-pinned (24-event traces); same audit |
| `d5dab0618` | E finding: 512 PCM quantum is retype containment; frame-budget test row | pipeline suite green at 512 |
| `2c67244b0` | register: OTA brick incident + Phase C bring-up gap | docs only |
| `6d6962c1b` | `aius-ota-resilience.md` scoped (A/B slots, forensics, staging off pad-2) | docs only |

**On-device state (post-flash checks, all passed):** generation-1 boot,
zero early resets, `relocks` 0, SD mounted, `/api/meters` 470 B with no
`analysis_window`, `tx_underrun_frames` 0 (fresh gate-open-only baseline),
flight recorder armed. The pulled dropout capture from the earlier
programme (21.8 s, loud program to the cut) lives in session scratch only
(7-day GC) — its numbers are in this plan's Evidence section.

**Uncommitted tree state:** nothing owned by this thread. Dirty
`electronics/*` and `compose.yaml` belong to the H3a
electronics session — foreign, untouched.

**Next action:** the instrumented image (governor compiled but HELD —
`AIUS_TX_GOVERNOR_ENABLED=0`) is LIVE on slot B as `fd9d35fd3f95`
(healthy-committed seq 9, 2026-08-16 evening — after a first activation
attempt that signed the Intel-hex TEXT as the payload; the chooser's
IVT-sanity refusal saved the slot and the signer now flattens hex input).
Wait for the next pop and read `input_diag` to attribute it: one
`production_gap_over_count` increment pairs with either a same-window SIS
error (input-side) or an ISR-cadence spike (firmware-side). That verdict
decides the governor's target policy; only then is the governor enabled
and Phase E's live verify run. The next image also carries `prev_boot` +
`fault_record` (the core's retained crash record over HTTP), already
built and committed.

**Update 2026-08-19 — the pop verdict is still owed, and two things above
are stale.** An owner-reported dropout was investigated live and turned out
NOT to be the awaited pop: the 36 min 18 s dark window (device uptime
174,682,408 → 176,859,984 ms) was a deliberate operator pause. Over the
whole boot the box showed 283 ms of lifetime `tx_underrun_frames` and 1.35 s
of lifetime `squelched_frames`, so no aius mechanism had accumulated enough
to produce a multi-second drop; the decoded 5.1 read −105 dBFS on all six
channels at the resume, meaning the AC-3 payload itself carried silence.
`production_gap_over_count` did not move and could not have: the watermark
resets its sentinel whenever the transmit gate is closed
(`firmware/src/optical_main.cpp:1751`), so **the attribution recipe above
only works for a pop that happens with the gate OPEN.** A locked-but-dark
interval is invisible to it, and that failure class now has its own plan,
[`aius-input-recovery.md`](aius-input-recovery.md).
**Closed 2026-09-04: the listening soak is considered complete against the last
pre-bench build. The pop verdict is satisfied by organic in-situ use — no buzz
or stall reported — with any future glitch reported as experienced rather than
run as a documented soak (owner).**

Also stale: the running image is `ceffc20dae6c+teensy41-optical-slot-a`
(built 2026-08-16T19:31:13Z, `ab.sequence` 11, boot generation 4), not the
`fd9d35fd3f95` this section names. And the flight recorder was found FROZEN
from boot bring-up (trigger `underrun` at `at_ms` 2942), so it had captured
nothing for the whole boot; it has been thawed and is recording. Its
`carrier_loss` trigger is still armed, which means the next deliberate pause
will burn the one-shot freeze — an open question in the input-recovery plan's
§Pending decisions.

**Update 2026-09-02 — Phase F opened.** The rate-gate capture tool's gate-2
run root-caused a 128-frame dropout to the post-relock transmit-ring floor
being phase luck (the gate-open target only trims down; nothing pads up).
Phase F below makes the floor deterministic at the gate-open edge; built
and flashed the same day (`3570a216216d+teensy41-optical`, plain
single-image posture, no A/B — the "slot B" statements above are
historical), with gate 2 passing for the first time on it.

**Decisions taken this session (owner, in chat):**
- Jitter is fixed by removing sources, never by widening the transmit
  cushion: added latency is unacceptable; 13 ms is below the perception
  floor and stays.
- The analysis window is dropped entirely (not decimated, not opt-in);
  meter bars and GR stay; EQ curve is client-side and free.
- PCM 512→128 was approved "with live check first"; the attempt exposed the
  retype-containment coupling and was reverted (my call, recorded in the
  Phase E section) — the live decomposition now gates any quantum change.
- "Flash it": the OTA activation bricked the board (dark-window failure,
  NOT the image); recovered via Program button + HalfKay. Remote activation
  is now interim-ruled in the resilience plan.

**Traps:**
- The deployed image's `gate_open_target_frames` is 128 from a
  `platformio.ini` override; the source default reads 256. Do not "fix"
  either to match the other.
- `tx_underrun_frames` is now gate-open-only: any tool comparing lifetime
  counts across images sees a semantic step; the rate-bench
  `allow_dark_tx_underrun` waiver is vestigial (harmless).
- A FROZEN recorder counts nothing, `suppressed_triggers` included — do not
  read zeros on a frozen boot as "no dropouts".
- The dsp suite still carries "meter analysis window JSON fits the body"
  rows: they pin lib-level arithmetic for a surface the route no longer
  serves (deliberate: capture core retained).
- The device SD auth token is deliberately NOT in the repo; ask the owner
  (derivation: `firmware/tools/aius_token.py` from fused MAC words).

### Findings and loose ends

- `firmware/tools/net/aius_rate_bench.py`
  `allow_dark_tx_underrun` waiver is dead under gate-open-only counting —
  proposed: remove on next touch (not agreed).
- The meters route stays outside the swagger format-string extractor gate
  (its two array loops need extractor shapes it lacks) — recorded in Phase
  A's targeted-proof; proposed: extend the extractor someday (not agreed).
- `test_dsp.c` analysis-window rows pin an unserved surface — proposed:
  keep (they guard the retained capture core) (not agreed).
- The OTA health record's pre-window mark may have survived the incident
  and could name the failed phase — one bench read before resilience
  Phase 1 locks (recorded there too).

### Pending decisions

- **A/B flash map** (resilience Phase 3): "Approve the two-slot flash
  layout direction — never-erased first-stage chooser at the boot address,
  dual-linked images per release, trial-boot commit — before 3a lands?"
  Options: approve (Phase 3 can start after 1–2) / modify / reject (stays
  on the interim physical-access rule). Recommendation: approve — it is
  the only shape that removes the physical-access failure mode this cycle
  demonstrated. Blocked until answered: Phase 3 only.
- **Acceptance evening scheduling**: "Run the jitter acceptance (one 5.1
  evening, dashboard open, counter delta 0) on the current image before
  the Phase C bring-up fix flashes, or fold both into one image first?"
  Recommendation: run acceptance now — the bring-up bug only bites boots
  under a lit carrier and does not touch the counters being proven.
  **Closed 2026-09-04: the listening soak is considered complete against the
  last pre-bench build; the acceptance evening is satisfied by organic in-situ
  use, glitches reported as experienced rather than run as a documented soak
  (owner).**
- **PCM decomposition** needs ~2 min of genuine PCM playing (owner
  action); the poll itself is scripted in this plan's Phase E.

## Evidence (2026-08-16 session)

Deployed image `197523cbab76+teensy41-optical` (built 2026-08-15 00:26), boot
generation 4, ~16.4 h up. Owner observed repeated momentary output dropouts
with input continuing, during ~2 h of live AC-3 5.1 decode and during earlier
viewing the same evening.

- **One dropout is device-convicted.** The flight recorder froze on a
  gate-open TX underrun at uptime 47,007,931 ms. The pulled capture
  (21.8 s, full ring, zero gaps) contains **zero silent frames** and loud
  program (median −8.6 dBFS) right up to the freeze: committed-block flow
  stopped abruptly while the gate was open. Capture + header saved to session
  scratch.
- **The absence was in the 2.7–100 ms band.** Both stall-witness events this
  boot are accounted for elsewhere (boot-time network stack start, 1.558 s,
  matching `stack_start_blocking_ms`; and a dark-carrier SD stall below), so
  the dropout's cause was too short for the witness but longer than the
  transmit cushion.
- **The transmit cushion is 2.67 ms.** `AIUS_TX_GATE_OPEN_TARGET_FRAMES=128`
  (`firmware/platformio.ini`, tuned 2026-08-03 against sub-millisecond
  measured jitter). This boot's own maxima break that assumption: audio
  service 6,464 µs, HTTP 5,603 µs, `http_service_overruns` 6.
- **The meters analysis window is the dominant recurring HTTP cost.** 1024
  int32 samples serialized to ~7–10 KB of JSON inside one HTTP epoch, per
  poll, at the dashboard's 10 Hz — running exactly and only when the owner is
  watching, which is when the dropouts were observed. The idle (empty-window)
  response measured 3.9 KB.
- **An SD primitive is unbounded.** The witness caught a 582,524 µs epoch at
  checkpoint 8 with the preempted PC in `SdioCard::readData`
  (`sd_diag.longest_primitive_us` 582,363 concurs). Card-internal
  stall (wear-leveling class); it landed while the carrier was dark (zero
  ingress drops proves no live-carrier stall that long this boot), but the
  exposure is a once-per-second hot-plug probe
  (`AIUS_SD_PROBE_INTERVAL_MS 1000`; `root_jobs` ≈ uptime seconds).
- **Dropouts tonight were uncountable.** `tx_underrun_frames` counts idle
  silence at ~48 k/s and wraps (register entry of 2026-08-15); the recorder
  was frozen from the first dropout onward, and a frozen recorder counts
  nothing (`suppressed_triggers` only ticks during post-roll).

Related but distinct: the register entry "Open-gate 128-frame TX
underrun, unexplained (2026-07-31)" is this same class, observed once before
the tuning to 128 even landed (struck 2026-09-03: explained and fixed by
Phase F).

## Locked decisions (owner, 2026-08-16)

- No added output latency; the 128-frame gate-open target stays.
- The spectrum/spectrogram feed (`analysis_window`) is dropped from
  `/api/meters` entirely. Meter bars and limiter GR stay. The EQ curve is
  client-side and unaffected.
- A second audio-service pass is interleaved into the epoch.
- PCM pass-through block drops 512 → 128 frames (~8 ms mean PCM latency cut),
  with a live before/after decomposition.
- SD hot-plug probing pauses while the TX gate is open.

## Phases

Heading markers for Phases A–D added 2026-09-03 from their own records: A, C and
D landed together in aius `d8e2a2d` (2026-08-16; A's editor half completed the
same day in `725e280`), B in aius `9748752` (2026-08-16); the register's
2026-08-15 `tx_underrun_frames` entry records D closed 2026-08-16.

### Phase A — drop the meters analysis window ✓ SHIPPED 2026-08-16

- outcome: `/api/meters` no longer serves `analysis_window` /
  `analysis_count` / `analysis_full_scale`; response shrinks to bars, GR and
  telemetry (~500 B). The dsp-editor renders without the spectrum underlay and
  spectrogram; no console errors, no dead polling of absent fields.
- seams: meters serializer in `firmware/src/optical_main.cpp`; swagger
  `Meters` schema + drift gate emitter table; `firmware/sd/www/dsp-editor.js`
  spectrum/spectrogram paths; dsp-editor vitest suite.
- targeted-proof: the swagger `Meters` schema (additionalProperties: false,
  no analysis fields) never documented the window, so the removal FIXES a
  served-but-undocumented drift; the schema-pinning row in
  `test/swagger.test.js` and the dsp-editor suite (which
  feature-detects the window and degrades) stay green unchanged. The meters
  route remains outside the format-string extractor gate (pre-existing scope;
  its two array loops need extractor shapes it does not have — recorded, not
  expanded here). On-device: post-flash `/api/meters` body ≤ ~1.5 KB with no
  `analysis_window` key.
- unit: parent-only
- **2026-08-16, later: the editor half of this outcome was NOT delivered with
  the firmware half, and is now done.** The firmware stopped serving the window
  in `b4a3914ce`; the page kept its canvas, its FFT, its band mapping and a
  status line reading *"history is paused"*, which describes a transient gap
  rather than a feed no device has. The owner found it on the deployed board.
  The spectrum underlay, the spectrogram, the client-side FFT and the ~20 rows
  of analyser coverage are now deleted outright (owner decision: delete, not
  hide), leaving three canvases on the page and 230 green editor rows. The
  three DSP editor captures in
  [`../reference/user-interface.md`](../reference/user-interface.md) still show
  the removed feature and are owed a re-render; its hero recipe needs a new
  lower anchor because `#spectrogram` was its old one.

### Phase B — interleave a second audio-service pass ✓ SHIPPED 2026-08-16

- outcome: pipeline absence per epoch is bounded by one heavy segment, not
  the sum: an extra TX-refill/audio pass runs after the HTTP checkpoint, so
  the worst absence chain no longer stacks HTTP onto the light checkpoints
  plus SD.
- seams: epoch service order (`lib/aius_runtime` + `optical_main.cpp` service
  wiring); checkpoint ledger semantics (`docs/reference/usb-control-protocol.md`
  checkpoint table note); audio service re-entry contract.
- targeted-proof: host runtime suite pins the new service order; checkpoint
  walltime vector semantics unchanged (same 11 domains); adversarial hot-path
  review before commit (same bar as the meter-cadence change); on-device
  soak in the acceptance run below.
- unit: parent-only

### Phase C — pause SD hot-plug probe while the gate is open ✓ SHIPPED 2026-08-16

- outcome: no routine card primitive runs while live audio is being emitted;
  `root_jobs` stops advancing during playback. Hot insert/removal detection
  resumes on gate close; `sd_present`, the usage figures and the card-swap
  config reload may be stale while deferred (documented at the guard).
- delivery-audit fix (2026-08-16): the gate latch alone loses the open edge
  (the guard runs before `tx_gate_service()` in the same checkpoint-7
  callback), which would have run one probe under a freshly opened gate at
  every session start. The guard now also tests `pllLocked()` — the
  current-instant carrier signal that precedes every carrier-driven open by
  at least the classify window. A replay-driven open at the start edge can
  coincide with a just-started probe, harmlessly: the probe's root job
  blocks the first refill (`sd_storage_idle()`), so the replay is
  necessarily still in its PREBUFFERING silence-carrier phase while any
  probe primitive runs — no program material is at risk — and once open,
  the gate latch defers all subsequent probes. Consequence: probes also
  pause while a carrier is merely locked (source powered, gate closed),
  which is acceptable for a probe with no urgency.
- seams: `sd_lifecycle_service()` scheduling in `optical_main.cpp`.
- targeted-proof: the deferral is a three-line guard in target-only lifecycle
  code with no host harness; proof is the optical image build plus on-device
  observation: `root_jobs` flat across a gate-open playback window, advancing
  again after gate close.
- unit: parent-only

### Phase D — gate-open-only underrun counter ✓ SHIPPED 2026-08-16

- outcome: `tx_underrun_frames` counts substituted-silence frames only while
  the TX gate is open — a true glitch metric that neither wraps in idle nor
  hides real dropouts; the recorder underrun trigger keeps firing identically
  (it already qualifies on gate-open).
- seams: `aius_spdif_tx24_fill()` ISR (gate-visible flag), tx24 suite,
  recorder trigger suite; the 2026-08-15 register entry closes.
- targeted-proof: tx24 host suite pins idle-silence non-counting and
  gate-open counting; recorder suite still passes its underrun-trigger rows;
  adversarial hot-path review together with Phase B.
- unit: parent-only

### Phase E — PCM latency decomposition + ratchet governor ✓ SHIPPED 2026-09-03 (decomposition; governor RETIRED)

**Closed 2026-09-03 by design decision D9
([`aius-outstanding-work-2026-09.md`](aius-outstanding-work-2026-09.md)
§Decisions):** the governor is not wanted — "it introduces systematic latency
with repeated triggers; I preferred a silent output but not a buzz/stall in the
output stream." `AIUS_TX_GOVERNOR_ENABLED` stays 0 on every image (it was never
enabled on the board), the pure rule and its `tx-governor` host suite stay as
host-proven code, and the standing behaviour on a stall is Phase F's silence pad
at the gate-open edge. The decomposition below is the phase's delivered value;
the "live verify owed" step is void. The acceptance evening and the pop verdict
are re-read against that preference — a stall must produce silence, never a buzz
— and are owner-scheduled listening (that plan's Phase 9).
**Closed 2026-09-04: the listening soak is considered complete against the last
pre-bench build; both are satisfied by organic in-situ use — silence on a stall,
no buzz reported — with glitches reported as experienced rather than run as a
documented soak (owner).**

**Finding, 2026-08-16 (the attempt's value):** `AIUS_PIPELINE_PCM_BLOCK_FRAMES`
at 512 is **load-bearing for re-type containment**, not just a buffer size. A
PCM→compressed switch validates one burst period (~1024 words) after the
first preamble; a 128-frame block completes inside that window and carries
IEC burst data to the sink as "PCM" — the `retype-switch` row caught 3 leaked
burst blocks, and at 512 the block boundary coincides with the worst
validation point so the staged tail is dropped instead of played. The
constant now documents this; the `eac3-entry` stuffing bound was re-pinned as
a frame budget (≤ 2048 frames) so a future quantum change is measured in
audio time, not block count.

**Also corrected:** staging latency is invisible to the editor's number
(`estimated_latency_us` is transmit-ring occupancy only), so the 512 quantum
cannot explain the observed 2× PCM display. The observed 26 ms is real ring
residence with an unexplained regulator.

**Decomposition, 2026-08-16 (live, 86 × 1 s samples during field PCM
playback; poll script archived in session scratch):** the standing number has
two independent terms and the "unexplained regulator" does not exist — that
was the finding.

1. **Structural ~14–16 ms**: the 512-frame PCM quantum + the 128-frame
   gate-open target + the in-flight block put the publish-instant queue at
   640–768 frames (13.3–16.0 ms) in a clean session. `tx_level_frames`
   sampled at random HTTP instants sawtooths 512–1,792 purely by phase —
   any governor must therefore read a window MINIMUM or a fixed-phase
   sample, never raw level samples.
2. **Episodic ratchet, unbounded and unregulated**: a producer stall (SD
   contention class; 3,968 substituted frames that boot, ingress high-water
   5,376 words ≈ 56 ms) plays silence, then commits its backlog late and
   parks the ring floor up to ~2,048 frames — the operator-observed 42.7 ms
   — with NOTHING to bring it back down: the only trim in the image fires at
   the gate's closed→open edge. The 26/42 ms readings were real standing
   residence, an artefact of stall history, not of the quantum.

Same day, live: a further +384-frame underrun burst (an audible pop) did NOT
ratchet the floor — small stalls pop without parking a backlog, confirming
the two phenomena are separate. The pop's root cause (what starves the
producer) remains open below; the ratchet is what this phase now fixes.

**Design cut (built 2026-08-16, operator-selected "trim to target"):** a
latency governor — pure decision rule in
`firmware/lib/aius_audio/src/aius_tx_governor.h`, wired in `enqueue()` —
keeps the window minimum of the pre-commit ring level over 4 s of committed
frames; when a full window's floor stays above target + 256-frame deadband,
one masked trim (the gate-open edge's own cursor-rebase contract, its
contract comment reopened for this named second caller) drops exactly the
excess: back to the ~16 ms structural baseline in one audible skip, accepted
by the operator over a standing 40 ms lag. An underrun voids the window (the
floor was demonstrably not elevated); a closed gate resets it. The quantum is
UNTOUCHED, so the re-type containment above is not reopened. Served as
`clock.governor_trims` / `clock.governor_trimmed_frames` (swagger'd).

- outcome: ✓ decomposed (above); the governor removes the episodic term; the
  structural ~14–16 ms term is accepted as the cost of the load-bearing
  512-frame quantum. The live verify that was owed here (induce an SD-traffic
  stall, watch one `governor_trims` increment) is void: the governor was
  retired by design decision D9 on 2026-09-03 (heading note).
- seams: `aius_tx_governor.h` (pure decision) / `enqueue()` tap + masked
  trim / clock JSON + swagger.
- targeted-proof: `tx-governor` host suite (8 rows: clean floor, deadband
  tolerance, exact-excess trim, min-not-mean veto, underrun void, gate
  reset, post-trim fresh state, window-close arithmetic) — green;
  `runtime-target`, `memory`, `itcm-placement`, `http`, swagger vitest —
  green 2026-08-16. Quantum untouched: `retype-switch` / `eac3-entry`
  containment gates not reopened.
- unit: parent-only

**Still open in this phase after the governor:** the stall itself (the pop).
The governor bounds the aftermath, not the event; the acceptance run below
and the SD-tail residual risk carry that thread.

**Pop diagnosis, 2026-08-16 evening (operator gate: diagnose before any
deploy).** A live pop advanced `tx_underrun_frames` by exactly
1536 − (measured ring level) twice (+896 at the 640-frame steady phase,
+384 ratcheted) — the arithmetic signature of ONE lost 32 ms AC-3 sync
frame. Firmware exonerated at every counter: no epoch budget overrun, no
checkpoint walltime movement, SD idle (a controlled 189 KB SD-served read
and even an HTTP outage moved nothing), pipeline/clock counters clean, and
the re-armed flight recorder shows continuous program audio to the final
committed frame. `SPDIF_SIS` read `0x0093C404`: BITERR + SYMERR + LOCKLOSS
latched — the receiver HAS taken physical-layer errors this boot, though
the sticky bits cannot be timestamped from HTTP. The operator does not
accept the cable attribution; **the governor is HELD**
(`AIUS_TX_GOVERNOR_ENABLED=0`, compiled but tap off, counters serving
zeros) and the image instead ships three input-fault instruments served as
`input_diag`: a per-epoch-windowed SIS error latch (count + stamp + W1C via
`SPDIF_SIC`, so the next error is attributable to its window), a
production-gap watermark in `enqueue()` (any ≥40 ms hole alerts, whatever
its cause), and an update-ISR cadence watermark (convicts or acquits the
in-firmware RX-overwrite path). The next pop pairs one
`production_gap_over_count` increment with either a same-window SIS error
(input-side) or an ISR-cadence spike (firmware-side) — that verdict decides
the governor's target policy (class-aware cushion vs 16 ms everywhere).
Two incidental finds, both registered: the flight recorder's underrun
trigger spends its one-shot freeze on the boot bring-up underrun (blind
witness every boot until thawed by hand), and rapid recorder-data reads can
wedge HTTP for a few seconds (server recovered unaided, audio unaffected).

### Phase F — deterministic transmit-ring floor at the gate-open edge ✓ SHIPPED 2026-09-02

**Shipped 2026-09-02** (aius `018c000` + `3570a21`, image
`3570a216216d+teensy41-optical` flashed over USB CDC the same evening):
`aius_spdif_tx24_set_level()` (trim down or pad up with silence),
`tx_gate_actuate(true)` arms, `enqueue()` executes the open at its
pre-commit instant, `clock.gate_open_padded_frames` served and swagger'd.
Host: clock-target (new set_level row), tx-governor, optical-app,
runtime-target, http green; teensy41-optical build `[SUCCESS]`, memory and
itcm-placement gates pass (ITCM free unchanged at 1108 B — `enqueue()` is
flash-resident). **Device, fresh boot with the carrier lit:**
`tx_underrun_frames 0`, `gate_open_padded_frames 128`, `tx_high_water 640`
= 128 + 512 — the bring-up underrun that used to spend the flight
recorder's one-shot freeze on every boot is gone (all four slots stayed
recording). Then, without a reboot: gate 2 (44.1 kHz, 129 s, capture tool,
`--expect-build`) **pass** — `continuous_output true`, underrun delta 0,
polled ring level min 128 / max 512 (evidence-sha256
`781250dd5d3e06972905473562b4eef9cf4b2706b035fe4e29c0b431fb011d23`);
e2e latency row 899 frames / 18.73 ms (+76 frames on the `e746cf9` row,
inside this phase's ≤128-frame bound; [`aius-e2e-latency.md`](aius-e2e-latency.md));
48 kHz smoke pass, zero deltas. Across boot, three relocks and the three
runs: `tx_underrun_frames 0`, `gate_open_trims 0`, `tx_high_water 640`
throughout; `gate_open_padded_frames` 128 → 256 (the 48→44.1 relock padded
a full target; the 44.1→48 relock found the ring already at exactly 128 at
the open instant and padded nothing — the pad is "up to" a target, the
floor is the invariant). Not run: the explicit U24 de-auth/re-auth row
(the sysfs write was refused by the agent's permission classifier that
session; the gate-2 stimulus relock is the relock evidence).

**Finding, 2026-09-02 (rate-gate capture tool, gate 2 at 44.1 kHz, image
`e746cf952dd6`; full trace in
[`aius-sample-rate-following.md`](aius-sample-rate-following.md) §Device
evidence — 2026-09-02):** the "128-frame gate-open target" is a ceiling,
not a floor. `clock_apply_desired_plan` empties the ring on every rate
change (`optical_main.cpp:7884`) and `tx_gate_actuate(true)` only ever
trims DOWN to `AIUS_TX_GATE_OPEN_TARGET_FRAMES` (`:2165`); nothing pads it
up, and the open happens at whatever instant `tx_gate_service` runs, not at
a producer-commit boundary. So the pre-commit floor a session settles at is
the phase the producer's first 512-frame burst happened to have against the
transmit ISR: anywhere from 0 to one block. In gate 2 it was 0
(`tx_high_water 512`, the bare quantum) and 18 s of clock following walked
the commit one block past the fill — a 128-frame dropout, after which the
floor sat at 128. The same lottery explains the gate-4 idle-gap underruns
(+896, +768), the +256 at the 2026-09-02 U24 re-auth relock, and why the
48 kHz floor reads 1536 after 23 unsupported episodes: every underrun
ratchets the floor up one block, and the governor (HELD) would only ever
bring it down. Phase E's decomposition assumed the structural floor was
"128-frame target + in-flight block"; it is that only by luck.

**Design cut (owner decision 2026-09-02: record and open the phase; no
flash that session):** execute the gate-open edge at the producer's
pre-commit tap, not in `tx_gate_service`. `tx_gate_actuate(true)` arms a
pending-open flag; in `enqueue()`, immediately before staging a block
(where the governor tap already reads the pre-commit level), a pending open
sets the ring to exactly the target — trim down as today, or pad UP with
digital silence when the level is short (the pad is dark, so padded frames
are unheard, exactly the argument the trim already makes) — then flips
`gate_open` and lights the pad inside the same masked section. The floor
after the block commits is then `target` by construction, every session,
every relock. The ISR-side underrun arm and the governor's window contract
are untouched; `aius_spdif_tx24_trim_to` grows a `set_level` sibling that
zero-fills the deficit (the ring buffer is deliberately never cleared, so
padding must write zeros). A relock with no producer resumption leaves the
gate closed and dark, which is the current behaviour minus the counted
silence. Whether the target should be 128 (one block, this env) or the
source default 256 (two blocks of service-jitter margin) is the same knob
as the held governor's target policy and is decided with it; the change
here makes the knob mean what it says.

- outcome: after any relock or gate reopen the pre-commit ring floor equals
  `AIUS_TX_GATE_OPEN_TARGET_FRAMES` exactly; gate 2 (44.1 kHz, 120 s)
  repeats with `tx_underrun_frames` delta 0 and `max_clock_tx_level_frames`
  = 512 + target; the relock after a U24 re-auth counts no underrun; the
  48 kHz floor no longer ratchets across unsupported episodes.
- seams: `firmware/lib/aius_audio/src/aius_spdif_tx24.h` (`set_level`
  sibling of `trim_to`, zero-fill), `firmware/src/optical_main.cpp`
  (`tx_gate_actuate` → pending flag; `enqueue()` pre-stage tap executes the
  open; `tx_gate_service` bookkeeping), clock JSON `gate_open_trims` /
  `gate_open_trimmed_frames` plus a new `gate_open_padded_frames` counter
  and its swagger row.
- targeted-proof: `tx-gate` host suite rows for pad-up, trim-down and
  exact-target floors at the pre-commit instant; `optical-app` /
  `runtime-target` green; `teensy41-optical` cross-build; then on device:
  gate 2 re-run with the capture tool (`--expect-build`), the U24 de-auth /
  re-auth relock reading `tx_underrun_frames` delta 0, and one e2e latency
  row (`bench.py --expect-build`) proving the standing latency did not move
  beyond `target − 128` frames.
- unit: parent-only

**Amendment 2026-09-03 — the target is per source.** The 128-frame floor
lost a race the live producer never runs: a replay's producer is the SD
refill job at checkpoint 7, whose `OPEN_READ` / `READ` primitives (8.3 /
6.3 ms measured) block the epoch two checkpoints before AUDIO, and pad +
first block = 256 frames = 5.3 ms drained 1 run in 3 at replay start
(register §Open defects — runtime). `tx_gate_execute_open(bool replay)` now
pads a replay's open to `AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES` (768 =
16 ms, `platformio.ini`, `#error` below the live target), chosen from
`enqueue()`'s own source fact; served as
`clock.gate_open_replay_target_frames`. Replay has no lip-sync to protect,
so its floor is sized to the primitive; the live floor and the governor's
target policy are untouched. Live: three `--with-replay` gate-4 runs on
`4c82434311f0`, `tx_underrun_frames` 0 across all three replay starts
([`aius-rate-gate-capture-tool.md`](aius-rate-gate-capture-tool.md)
§Live-run results — 2026-09-03, replay floor).

## Acceptance

One evening of live 5.1 with the dashboard open on the new image:
`tx_underrun_frames` (now gate-open-only) delta 0, no audible dropout, no
recorder freeze. The re-armed flight recorder is the independent witness —
any dropout freezes a capture.

## Residual risks

- The SD primitive tail remains unbounded for owner-initiated SD traffic
  during playback (dashboard file serves, uploads, recorder pulls). Accepted:
  owner-initiated, and Phase B halves its blast radius. A true async sector
  layer is recorded as disproportionate; the vendored FAT layer is
  synchronous to its core.
- Audio-service peaks (6.46 ms max this boot) can still exceed the 2.67 ms
  cushion in the worst decode+DSP epoch even after A–C; Phase B does not
  shorten the audio service itself. If the acceptance run still counts
  underruns, the next lever is bounding the meter-publish and DSP work per
  pass, not the cushion.
