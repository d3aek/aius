---
title: Aius open-work register
type: plans
scope: aius
subtype: backlog
status: in-progress
tags: [aius, backlog, debt, defects]
updated: 2026-07-27
toc: true
---

# Aius open-work register

Every open Aius item in one place, assembled 2026-07-27 at the close of the
DSP-editor enhancement (task #7) and the bench-prep session. **This register
points at authoritative homes; it never duplicates their content.** An item
with its own section in another plan carries a one-line gist and a link;
items whose only prior record was a commit message or plan aside are fully
specified here. Strike items here in the commit that closes them.

## In flight at handoff

- ~~**Downmix-matrix visual surface**~~ **LANDED before the handoff closed**
  (`77f64a8be`): the routing diagram is in the strip, 168 dsp-editor tests
  (221 across the aius scope), deployed to the card, and drag-verified
  on-device with trusted input (centre gain moved by a native drag at the
  measured handle, then restored). Probe lesson recorded in the resume:
  `matrixHandlePoints(values, rect)` takes HANDLE-keyed values
  (`center`/`surround`/`lfe`/`headroom`), not field names.

## Contracted, not started

- ~~**PSRAM flight recorder**~~ **SHIPPED 2026-07-27** (gate protocol
  complete: adversarial prebuild + clean-room GO, portable core
  suite-first, Teensy wiring + HTTP surface + swagger + host WAV script,
  absent-path verified on the device — the chips are still not fitted,
  and absence is first-class as contracted). Delivery surfaced defects
  25 (open) and 26 (closed, link-gated); see the defect gists below.
- **Input-side capture tap** — explicit follow-up slice inside the
  recorder contract; not v1.

## Waiting on the operator

- **Bench first-signal test** — everything is staged:
  [`../guides/proto-optical-wiring.md`](../guides/proto-optical-wiring.md)
  (22 pF fitted value recorded), RS PRO jacks + datasheets + AP214 STEP
  models in evidence, and the firmware TX gate live on the board
  (`tx_gate` on `/api/status`). Runs when the jacks are soldered. Its step
  5 (glass loopback re-running item 3 across real fibre) also owns the
  clock/content-ratio residue recorded in `signal-path.md` §Open.

## Recorded debt — small, none blocking

- **Replay schemas absent from swagger.** `/api/replays*` request/response
  bodies are served but undocumented; the contract suite asserts nothing
  about them. (Recorded since the item 3 era.)
- **Swagger `Status` schema incompleteness.** Served but unlisted:
  `sd_diag`, `replay_diag`, `dhcp_pending`, `cpu_temp_c` /
  `cpu_temp_valid` / `cpu_temp_max_c` / `cpu_temp_age_ms`, the event and
  HTTP counters. The TYPE lies were fixed and pinned 2026-07-27
  (`4eb3744ce`); the absences remain. No `additionalProperties:false`, so
  nothing breaks — but a schema-coded client cannot discover these fields.
- **hiDPI waveform stroke in `sd-shared.js`.** `drawWaveform` strokes the
  backing store directly: one device pixel on a 2× display while the EQ
  plot's strokes are DPR-scaled. Cosmetic; fix is a DPR-aware backing in
  the shared renderer.
- **UX NICE-tier residue** from the 2026-07-27 review (full list: session
  scratch `ux/ux-review-report.md`, sections B/D; the MUST-FIX tiers all
  shipped). Known outstanding: no marker of which preset the form came
  from (N2); the replay upload form sits below the full library (R-N5).
  Sweep the B/D lists against the shipped pages before assuming any other
  item's state.
- **Cost-model rows carry the old Cliff quotes** for the optical jacks
  (`aius-cost-model.md`); the purchased parts are RS PRO 805-1671/805-1680.
  Update when the cost model is next touched.

## Larger backlog

- **DSP roadmap item 4 — surround decorrelation.** The last item of the
  accepted audio-quality order
  ([`aius-dsp-config-api.md` §Accepted order](aius-dsp-config-api.md#accepted-order-for-the-next-level)):
  a short allpass or few-ms delay on Ls/Rs. Now measurable via item 3's
  loopback, and better still once the glass loop runs.
- **B9 — the app dashboard retirement**, keeping `swagger.json` and
  its contract suite. Independent of everything above
  ([`aius-network-hardening.md`](aius-network-hardening.md), B-series
  table).
- **Open defects** — authoritative list with full context:
  [`aius-network-hardening.md` §Open defects](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking).
  Open as of 2026-07-27, gists only:
  - **2**: `500 sd_failure` on large uploads ~1 in 15; diagnostics landed
    in-image; flash-reproduce-read `sd_diag.last_failure` remains, and the
    recorded one-line fix is deliberately unapplied until convicted.
  - **4**: `record_event` has two call sites; everything else is a
    counter — prerequisite to any push observability.
  - **14**: the 128 KB DTCM DTS arena is reserved and never used
    (`--wrap` unflagged in the optical env) — resolving it either way
    changes the memory story every later item reads.
  - **18**: five source comments still quote the pre-fix DTCM free
    figure.
  - **19**: `read_once()` carries the latch-substitution hole the send
    path had.
  - **20**: the network fake gets `_fnet_socket_addr_check_len(addr, 0)`
    backwards and the suite passes anyway.
  - **22**: `GET /api/dsp/config` cannot report a committed config on a
    card-absent board (design call).
  - **23**: `AIUS_NET_DHCP_DEADLINE_MS` at 9,000 ms bought one mode, not
    headroom (design call).
  - **25**: a flash-driven warm reboot crashes boot twice and latches
    recovery-safe; cold power cycle boots clean. Reproduced with
    byte-identical sources to a working image, so environmental to the
    reboot path, not code. UNATTENDED REFLASH IS BROKEN until chased;
    `ps=`/`pv=` USB ID diagnostics landed for it. (2026-07-27.)
  - **26** (CLOSED same day, kept for the residue): OCRAM heap
    exhaustion under NativeEthernet's ~96 KiB boot mallocs bricked every
    boot when recorder staging took 64 KiB; root-caused by bisection,
    gated by a 128 KiB link-time heap-floor ASSERT, staging resized to
    16 KiB. Open residue: boot converts malloc failure into an unfed-
    watchdog crash loop instead of a diagnosable degraded state.
- **H2 footprint gate** — validate the Gerber against the RS PRO PCB
  layout drawings before any board order; evidence filed (datasheets +
  AP214 STEP models), gate recorded in
  [`aius-h1-schematic.md`](aius-h1-schematic.md). Activates with the H2
  layout phase.
