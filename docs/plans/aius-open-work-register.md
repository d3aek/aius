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

- ~~**First PSRAM chip: on the FLASH pad, and VERIFIED WORKING there
  (2026-07-27, late).**~~ **RESOLVED 2026-07-28: chip 2 soldered on the
  PSRAM pad, `ps=16`, recorder armed on the full 16 MB ring; capture,
  freeze and WAV pull all proven live** (the recorder's first real
  capture — see defect 27's gist).
- ~~**Optical jacks: restore the ORIGINAL pin arrangement.**~~
  **RESOLVED 2026-07-28 ~04:30: GLASS LOOPBACK LOCKED.** After both
  guide errors were corrected (series bypass caps; pinout numbering is
  on the FRONT view — the operator's original placement had been right
  all along), the rewired jacks locked the carrier through real fibre
  in seconds: 48 kHz, blocks climbing, both modules healthy (the TX
  survived its accumulated anomalous-bias exposure). Defect 27's
  overrun flood reproduces across glass too — real-jitter immunity is
  ruled out as a factor. Remaining optical work: the measure flow
  across glass (guide step 5, `measure_vectors/measure_run.py`) and
  the remaining §First real signal test steps that need an EXTERNAL
  source or sink (steps 2–4, 6) — plus DTS-over-carrier
  classification, untested because replay self-terminates on lock
  (defect 27's policy note) and the brief IEC window stayed PCM(3).
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
- **DSP editor does not surface the live source type / channel count /
  rate.** Found the moment a real source was first connected
  (2026-07-28): no at-a-glance "PCM 2.0 @ 48 kHz" vs "DTS 5.1" vs "no
  carrier" indicator, though `/api/status` already serves `state`
  (2/3/4 = none/PCM/DTS) and `sample_rate`. Pure card-www change: a
  source badge beside the meter cards reading existing fields; no
  firmware needed.
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
  - **10**: the network-transport fake still diverges from real FNET in
    five characterized places (void `close`, `MSG_PEEK` ignored, `int8`
    error type, no `AGAIN` on empty accept, `SO_SNDNUM` hard-wired 0);
    the two UDP mappings are fixed, the rest stands.
  - ~~**14**: the 128 KB DTCM DTS arena is reserved and never used.~~
    RESOLVED 2026-07-28 with defect 29: the optical env's
    `build_unflags` stripped the wrap set; restored, the arena now
    serves every decode (62.5 KB peak, steady-state no-heap proven
    on hardware).
  - **18**: five source comments still quote the pre-fix DTCM free
    figure.
  - **19**: `read_once()` carries the latch-substitution hole the send
    path had.
  - **20**: the network fake gets `_fnet_socket_addr_check_len(addr, 0)`
    backwards and the suite passes anyway.
  - **21**: the boot-to-online timing gap is real and board-side: USB
    ready ~4 s against a 2 s deadline, DHCP/UDP/HTTP ~9–10.6 s against
    7–8 s, bimodal at exactly 8.90/10.56 s (one discrete retry's step);
    root-caused as NOT the ledger, NOT DHCP negotiation.
  - **22**: `GET /api/dsp/config` cannot report a committed config on a
    card-absent board (design call).
  - **23**: `AIUS_NET_DHCP_DEADLINE_MS` at 9,000 ms bought one mode, not
    headroom (design call).
  - **25**: a flash-driven warm reboot crashes boot twice and latches
    recovery-safe; cold power cycle boots clean. Reproduced with
    byte-identical sources to a working image, so environmental to the
    reboot path, not code. UNATTENDED REFLASH IS BROKEN until chased;
    `ps=`/`pv=` USB ID diagnostics landed for it. (2026-07-27.) Late
    same day: `POST /api/reboot` boots CLEAN (warm resets as a class
    exonerated; loader CDC session stays the prime suspect), the `fa=`
    first-abort retained latch is live and board-validated, and two
    instrumented flashes booted CLEAN — 0/2 against the prior 4/4, so
    the defect is CONDITIONAL and the instrument simply rides every
    future flash until it fires.
  - ~~**29**: live DTS decode silent (state 4, zero output, zero
    faults).~~ ROOT-CAUSED AND FIXED 2026-07-28 (with 14): the
    optical env unflagged `--wrap=malloc`, so the armed arena was
    bypassed into the NativeEthernet-drained heap and every access
    unit died AIUS_NO_SPACE, silently. First live 5.1 decode ever on
    this hardware followed within minutes of the fix. Full chase +
    the permanent diagnostics it left: hardening doc entry 29.
  - **30**: decoded 5.1 plane order vs downmix slot order disagree — a
    pure spatial permutation, found by tone-identity DFT the same
    hour (left carries FR's 600 Hz strong; right carries C's 900).
    Chase is a table diff plus a native per-channel tone test; no
    hardware needed.
  - **27**: the silence self-loop (locked carrier, validity-muted PCM
    in, silent out, TX gate held open) floods `overruns` at ~1 kHz
    sustained; found on the first-ever external-carrier input
    (electrical 14→15 loopback, 2026-07-28); the flight recorder's
    overrun trigger caught it live (its first real capture).
    Reproducible on demand; reboot exits it. Same-day verdict: a real
    independent-clock source runs CLEAN (0 overruns / 20 s), so the
    unity-clock-ratio path is convicted and the flood is
    loopback-rig-specific in practice.
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
