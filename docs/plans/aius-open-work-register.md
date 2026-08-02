---
title: Aius open-work register
type: plans
scope: component
component: aius
subtype: backlog
status: in-progress
tags: [aius, backlog, debt, defects]
updated: 2026-08-02
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
- ~~**The bench SD card needs service (check on a PC, reformat, or swap)
  before the full-size OTA end-to-end (2026-08-01).**~~ **RETRACTED the
  same day — the card was innocent** (owner challenged the verdict;
  the controlled A/B convicted the board's own runtime state, below).
  Keep the card.
- **Darken the CM6206 optical feed for the E-AC-3 replay acceptance
  run (2026-08-02).** A replay start refuses against a live carrier by
  design (`replay_carrier_present`, guard `79a881f59` 2026-07-25), and
  the bench RX carrier is permanent. Confirmed replay-global on the
  E-AC-3 image: the AC-3 control (`channel_id_51_ac3`) refuses
  identically, and the refusal is unwinnable by timing — 303 start
  attempts raced across a reboot all 409'd, SRPC LOCK pinned from
  +7.8 s (before HTTP rises), SRPC/SRFM byte-stable across polls.
  **The carrier source is the CM6206 dongle's optical out, not the TX
  loop:** the measured carrier is 48,016.85 Hz = +351 ppm off nominal,
  an external crystal (an own-TX loop would read ~0 ppm against the
  Teensy's own clock). It is `usb 3-1` on the dev host (0d8c:0102,
  ALSA card `ICUSBAUDIO7D`). **Software disable — needs root; the
  agent's guards forbid sudo, so the operator runs it** (the ALSA
  mixer exposes no S/PDIF-out switch and the USB node is root-only,
  so no unprivileged path exists; owner authorized the disable
  2026-08-02):
  `echo 0 | sudo tee /sys/bus/usb/devices/3-1/authorized` then
  `echo auto | sudo tee /sys/bus/usb/devices/3-1/power/control` —
  suspend caps the device at 2.5 mA, which kills the optical LED;
  deauthorize alone may leave it lit. Restore afterwards with `on` /
  `1` in reverse order (or replug). **Then run**
  [`eac3_acceptance.py`](../../firmware/tools/replay_samples/eac3_acceptance.py)
  `--token <sd-auth>`: it polls until the carrier drops (the start
  attempt is the exact guard probe), starts `eac3_id51_3s` looped,
  samples `codec_pc` 21 / decode counters / `eac3_decode_cycles`
  against the 8 ms budget, re-measures the AC-3 and DTS controls
  (tables now flash-cached), stops everything and writes JSONL
  evidence. **Residual worth one probe:** if start still refuses with
  the RX provably dark, raw `pllLocked()` is asserting without light
  (the boot-lottery fossil family) and the start guard needs the
  runtime's debounced carrier signal instead of the raw SRPC bit —
  register that as a runtime defect.

## Open defects — runtime

- **Boot-lottery epoch crawl (2026-08-01; FIXED 2026-08-02 — mechanism
  convicted, boot gate shipped, 28/28 flash-boots healthy on the
  reproducer lineage that failed 3/3 without it; conviction chain and
  fix at the end of this entry).** Some boots enter a degraded mode in which the
  epoch loop runs orders of magnitude slow (measured 14–130 epochs/s
  against the healthy ~7,800/s), which cascades into everything the
  epoch paces: SD write "primitives" stretching to seconds (the
  misdiagnosed "card failure": 5.9 s longest 512-byte write, driver
  faults, remounts), HTTP crawling, audio overrun events every ~1.8 s,
  and occasional WDOG resets (`reset_source` 16) when a stretched
  primitive outlives the 8 s watchdog — each reset wiping the RAM OTA
  session and activation latch. Other boots of the byte-identical image
  are fully healthy (50 ms SD writes over the same card, same minute).
  Evidence trail: the pre-change image A/B first misread as a firmware
  regression, a 7-step content bisect whose every increment was healthy,
  and finally the same image flipping healthy→degraded across a single
  warm reboot. What is ruled out: the card (A/B clean), capacity (4 GB /
  19 MB used), CPU temp (54 °C), any specific code change (bisect), and
  pure flash-layout shifts (probe builds). Detector shipped with the
  activation-fix commit: `checkpoint_walltime_us` on `/api/status` (11
  per-checkpoint wall-time maxima) and `cw=<id>:<µs>` on `AIUS ID`, so
  a crawling board names its stall over CDC even with HTTP collapsed.
  **Bench protocol until pinned: after any boot, read the epoch rate
  (two `AIUS ID` samples) before timing-sensitive work; a degraded boot
  is cured by rebooting until healthy.** Suspects for the real
  investigation: a boot-time race in network/audio bring-up leaving a
  service in a degraded steady state; instrument via the new vector on
  a degraded boot.

  **Afternoon findings, same day — the odds are a property of the
  image layout, and the degraded state is characterized live:**

  - Reproduction: 50 consecutive boots of the current image (30 clean
    reboots + 20 reset-mid-SD-write) were ALL healthy, while the
    e6417b428-content image degraded on its first boot — as it did on
    ~every boot this morning. An out-of-tree build of e6417b428's
    firmware is a durable ON-DEMAND REPRODUCER (two independent builds
    of that content behaved identically); each source perturbation
    re-rolls the odds, which is what made the morning's bisect chase
    its own tail. `checkpoint_cumulative_ms` (shipped) rides only
    post-detector images, so a bad-odds instrumented layout must be
    found by a padding sweep before the vector can attribute a
    degraded boot directly.
  - Live degraded capture (bad-odds image, first boot): audio ingress
    delivered 29 chunks total against ~15,000 expected (99.8 %
    absent) while the audio service itself stayed fast
    (`service_max_us` 41); pipeline starved (`blocks` 0, meters all
    silence, `stream_rate_hz` 0) with the clock happily `tracking`;
    epochs BIMODAL — most normal, rare multi-second stalls
    (`runtime_epoch_max_us` 5,520,770 against only 13 budget
    overruns), HTTP dead windows ≥2 s; network started once cleanly
    (no restart loop); SD clean on this boot. The morning's "5.9 s SD
    primitive" was main-line wall time inflated by the same stalls.
  - The defect has a CLOCK: audio-overrun event edges tick at
    1,791.7±3 ms — exactly 168 blocks at 93.75 blocks/s — through the
    whole degraded lifetime.
  - Working hypothesis: interrupt-context CPU theft in bursts (an ISR
    storm) in the SPDIF-RX/audio-ingress chain, armed by a boot-time
    race whose odds image layout shifts through boot-path timing;
    the 168-block period points at a buffer/window wrap in that chain
    re-arming the burst.
  - ~~Next steps: (1) audio-update ISR invocation + cycle counters
    served beside the checkpoint vectors; (2) padding-sweep builds of
    the instrumented tree, boot-looped until one lands on bad odds;
    (3) one degraded boot of that image then convicts or acquits the
    ISR by calls/s and lets `checkpoint_cumulative_ms` attribute the
    remainder.~~ **Steps 1–2 executed same day; the sweep hit a
    wall worth recording.** The ISR counters shipped
    (`audio_update_calls` / `audio_update_max_us`, healthy baseline
    ~410 calls/s and 6 µs at 48 kHz) with an `AIUS_LAYOUT_NUDGE`
    compile knob padding flash CODE per variant. Three sweep
    campaigns — 8 rodata-nudged variants, 8 code-nudged variants of
    the current tree, and 6 code-nudged variants of the KNOWN-BAD
    tree carrying only an eight-line counter — totalled 44
    instrumented variant-boots with ZERO reproductions, while the
    pristine bad layout reproduces on its FIRST boot every time. The
    manifestation window is razor thin: any in-image instrumentation
    observed so far destroys it, so the defect currently cannot be
    convicted from inside the image. The code-nudge does modulate the
    healthy epoch rate continuously (7,300–8,000/s across variants,
    one partial-degradation outlier at 2,077/s), confirming a real
    layout-timing interaction. Remaining viable paths, in order:
    (1) a hardware-debugger session (attach on a degraded boot of the
    pristine reproducer and sample the PC / NVIC state directly —
    a candidate rider for the ESI U24 XL bench session);
    (2) offline differential analysis of the pristine-bad vs healthy
    ELF pair focused on the SPDIF-RX/ingress/clock ISR chain, guided
    by the 168-block (1,791.7 ms) period; (3) an external logic/scope
    probe on the S/PDIF and ENET activity during a degraded boot.
    The on-demand reproducer stays: build `e6417b428`'s firmware
    out-of-tree, flash, first boot degrades.

  ~~**GATE (owner decision 2026-08-01): this defect now blocks every
  E-AC-3 flash stage** ([`aius-eac3-support.md`](aius-eac3-support.md)
  Phase 4 and beyond). Every flash re-rolls the boot lottery, so the
  deployment cadence the E-AC-3 target work needs is unsafe until boots
  are deterministically healthy. Host-side E-AC-3 work (Phase 3 native
  proof, plan work) stays unblocked.~~ **Gate LIFTED by owner
  2026-08-02 after the fix below; see the gate note at the end of this
  entry.**

  - Hypothesis candidate for path (2), recorded 2026-08-01 from the
    signature arithmetic: a one-block slip every 168 blocks is a
    ~0.595 % (1/168) rate mismatch — a BEAT between two clocks that
    should be locked, pointing at a boot-latched bad state in the
    SPDIF-RX clock recovery / audio PLL chain (per-boot: a lock race;
    layout-sensitive: boot-path timing shifts the race; warm-reboot
    flip: a re-lock). Discriminating experiment: a clock-domain
    register dump (SPDIF SRPC/clock-recovery state, CCM audio PLL,
    SAI/SPDIF dividers) served over HTTP, captured on a DEGRADED boot
    and again after the warm-reboot flip of the SAME image — the diff
    convicts or acquits the clock chain. Adding the dump to the
    reproducer is one more roll against the in-image-instrumentation
    wall; if manifestation survives, this is the cheapest remaining
    conviction path, ahead of the hardware-debugger rider.

  **2026-08-02 — a LIVE partial-degraded specimen quantified the beat,
  and the discriminator shipped:**

  - The bench board (image ceb1f3dbffe9, ~17 h up) was found sitting in
    a partially degraded boot: epochs a healthy-ish ~6,000/s, but the
    audio side showed the full syndrome live — `audio_update_calls`
    advancing at 288.0/s against the 375/s block rate (76.8 % of
    nominal), every ingress event a `missing_live` gap flush at exactly
    144.0/s (half the update rate, a stable alternation held for
    hours), `audio_update_max_us` 1,789,558 ≈ the 1,791.7 ms beat, and
    `runtime_epoch_max_us` 5.37 s — while `clock` reported carrier
    locked, tracking, 48,016.850 Hz. The carrier measure is
    CROSS-DOMAIN (SRFM scaled by the 150 MHz IPG clock), so the carrier
    reading is real: the S/PDIF ENGINE paces at 76.8 % of the rate the
    receiver's own DPLL measures. Every in-domain surface
    self-consistently reports 48 kHz; only cross-domain observers (CPU
    cycle counter, wall clock) see the crawl.
  - Mechanism candidate sharpened by source reading:
    `set_audioClock()` (both the retained PJRC helper in
    `aius_audio_clock.cpp` and the framework original) SILENTLY KEEPS a
    pre-enabled PLL4 — `if (!force && ENABLE) return` — so whatever
    frequency PLL4 carries when `config_spdif3()` runs is latched for
    the boot. A boot-order/timing race that leaves PLL4 enabled-wrong
    fits every observed property: per-boot odds, layout sensitivity
    (boot-path timing), the warm-reboot flip, and in-domain
    self-consistency.
  - Shipped (this commit): `clk_regs` on GET /api/status — raw hex
    SPDIF SCR/SRPC/SRCD/STC/SRFM/SIS + CCM PLL4 (CTRL/NUM/DENOM) +
    MISC2 + CDCDR + CCGR5, plus `spdif_in_master`/`spdif_out_master`
    (the two per-class update-responsibility statics) and `tx_fills`
    (transmit-DMA fill passes — the TX bit clock measured from OUTSIDE
    the S/PDIF domain). Raw words, host-decoded: the report cannot be
    fooled by the clock it reports on. Plus
    `tools/boot_lottery/boot_campaign.py`: button-free reboot loop
    that samples rates + registers per boot, classifies
    healthy/degraded, and emits JSONL — the "boots deterministically
    healthy" gate made measurable. Next: flash the instrumented image,
    campaign until a degraded boot lands, diff its clk_regs against
    the post-flip capture of the same image.

  **2026-08-02, later — CONVICTED AND FIXED. The conviction chain, from
  three instrumented degraded specimens (the in-image-instrumentation
  wall broke: `e6417b428`-content builds carrying the full dump still
  degraded on their first flash-boot, three layouts out of three):**

  - The campaign infrastructure first ACQUITTED, register by register,
    everything the earlier hypotheses named: 60 warm reboots of the
    instrumented mainline all healthy; then on the reproducer's
    degraded boots the clk_regs dump read BYTE-IDENTICAL to a healthy
    boot (PLL4/CDCDR/STC/SRPC nominal, DPLL LOCK set), `millis`
    tracked wall clock exactly (21,535 ms over a 21 s gap — the CPU
    and ARM/SYS PLL chain true), eDMA showed no errors and no
    unserviced completions, AHB/IPG dividers nominal, and no
    interrupt storm was active or pending across burst-sampled NVIC
    IABR/ISPR.
  - What remained: BOTH S/PDIF DMA channels (identified by DMAMUX
    source) advancing IN LOCKSTEP at a junk rate that VARIES PER BOOT
    — 1/44 of nominal on one capture, 1/158 on another, and the
    morning live specimen's 0.768x is the same signature at a milder
    landing point. A non-integer, boot-varying, engines-in-lockstep
    slowdown is no divider mislatch; and SPDIF_SRFM read bit-identical
    (0x00053EA6) across healthy and degraded boots — the "48.017 kHz
    carrier" the clock diag reports is a FOSSIL, not a measurement,
    which is how every in-domain surface stayed self-consistently
    green through every earlier investigation.
  - CONVICTION: the S/PDIF receiver DPLL comes out of some boots
    asserting LOCK on a junk clock; SRPC_CLKSRC_SEL(1) honours LOCK
    and hands the S/PDIF engines that junk clock; the RX DMA paces
    the audio graph (update responsibility), so the audio path and
    the epoch loop crawl behind it. Boot-path timing shifts the
    acquisition race — hence layout-sensitive odds; a warm reboot
    re-runs acquisition — hence the flip-cure; the odds concentrate
    on the POST-FLASH boot (every graft reproduction was a
    flash-boot; warm reboots ran 68/68 healthy across the
    instrumented-mainline and fix-prototype layouts).
  - THE FIX (shipped with this entry): a boot gate in
    `begin_pipeline()` cross-checks the engine rate against the DWT
    cycle clock — `tx_fills` over a 100 ms window, a probe no S/PDIF
    clock state can fool — and on mismatch forces DPLL re-acquisition
    via `spdif_engine_reacquire()` (config-preserving S/PDIF soft
    reset, DMA requests gated across it), up to five attempts. The
    served `clk_regs.relocks` counter is the sentinel: 0 = verified
    first try, 1–4 = junk-locked boot cured, pinned 5 =
    alive-but-dark and named. Evidence: the fix-bearing reproducer
    lineage ran 28/28 flash-boots + 8 warm boots healthy (including
    four deliberately layout-nudged variants) against 3/3 degraded
    first flash-boots for the same content without the fix; the
    mainline fix image ran its own flash-boot campaign healthy
    before this landed (JSONL records under the session scratch,
    `boot-campaign-*` / `nudge-hunt-*` / `mainline-fix-*`).
  - Honest residual: no cured-boot (`relocks >= 1`) event has been
    captured — with the gate aboard, no boot has junk-locked at all
    in any roll. The gate's 100 ms settle window may be prophylactic
    (letting acquisition finish before the heavy bring-up races it)
    rather than curative; either way boots come out healthy, and the
    first organic junk-lock anywhere will stamp itself on `relocks`
    and settle the question from the field.
  - **Gate note:** the owner's 2026-08-01 decision blocked E-AC-3
    flash stages "until boots are deterministically healthy". The fix
    + campaign evidence met that condition on the bench, and the
    owner LIFTED the gate 2026-08-02 — E-AC-3 Phase 4 (target
    integration) is cleared to flash
    ([`aius-eac3-support.md`](aius-eac3-support.md) Phase 4).
- **Boot-adjacent interrupts-masked stall, ~1.84 s (observation,
  2026-08-02).** One boot of the E-AC-3-image flash campaign (boot 6
  of 10) recorded `runtime_epoch_max_us` 1,840,681 — a single stall
  with interrupts masked, arithmetic-consistent with the epoch deficit
  it left (375 epochs/s × 1.84 s ≈ 341 lost ≈ measured 341.7); every
  other counter healthy before and after, and the other nine boots
  clean. Shape says flash-wait with interrupts off (EEPROM-emulation
  compaction is the standing suspect), NOT the fixed DPLL boot-lottery
  defect (`relocks` 0, engines verified, rates nominal outside the one
  window). Impact: a campaign classifier window that straddles the
  stall reads a healthy boot as degraded — `boot_campaign.py`
  classifications near a boot's first seconds should re-sample before
  convicting. No fix owed until it recurs; a recurrence wants a DWT
  timestamp + caller tag on the longest masked section.

- **Replay schemas absent from swagger.** `/api/replays*` request/response
  bodies are served but undocumented; the contract suite asserts nothing
  about them. (Recorded since the item 3 era.)
- ~~**Swagger `Status` schema incompleteness.**~~ **CLOSED 2026-07-31**:
  every served field of the status, recorder and DSP-config documents is
  now documented (~145 fields across six new sub-schemas incl. `sd_diag`,
  `replay_diag`, `audio_diag`, `clock`, `pipeline_diag`, `boot_fuse`), and
  a drift gate in the swagger contract suite extracts the served key tree
  from the firmware's own serializer format strings and fails on
  divergence in either direction, with type checks and negative-control
  proof. The gate found and closed further drift beyond this entry
  (`NetDiagGeneration` keys, recorder `rate_changes`). Scope note: the
  `/api/replays*` documents are still outside the gate — the sibling debt
  entry above remains open.
- ~~**DSP editor does not surface the live source type / channel count /
  rate.**~~ **CLOSED 2026-07-28** (operator-directed UI batch): the
  editor's input line now names the encoding and rate from `status.state`
  / `status.sample_rate` ("input: optical carrier · PCM · 48000 Hz"), and
  the header's overloaded "Source" label (DSP-model provenance, not audio)
  was renamed "Config". Residual: no channel count — `/api/status` serves
  none today, so that half needs firmware if ever wanted.
- ~~**hiDPI waveform stroke in `sd-shared.js`.**~~ **CLOSED 2026-07-29
  (SD dashboard Phase 9):** the shared replay renderer now fits its backing
  store from both CSS dimensions and DPR, scales its strokes by DPR, and
  retains the centre-line/128-point contract under replay-page coverage.
- **UX NICE-tier residue** from the 2026-07-27 review (full list: session
  scratch `ux/ux-review-report.md`, sections B/D; the MUST-FIX tiers all
  shipped). ~~No marker of which preset the form came from (N2)~~ — N2
  CLOSED 2026-07-28: the preset line names the origin and tracks its
  "(modified)" state live. Still outstanding: the replay upload form sits
  below the full library (R-N5). Sweep the B/D lists against the shipped
  pages before assuming any other item's state.
- **Cost-model rows carry the old Cliff quotes** for the optical jacks
  (`aius-cost-model.md`); the purchased parts are RS PRO 805-1671/805-1680.
  Update when the cost model is next touched.

## Larger backlog

- **Sample-rate following** — active production work in
  [`aius-sample-rate-following.md`](aius-sample-rate-following.md). It is the
  technical clock prerequisite for the AC-3 four-hour gate. Three device
  gates remain in scope (the 48→44.1→48 plus loss→replay transitions, the
  trim-boundary waveform, clean-source encoded AC-3) plus the gate-1 rerun;
  32 kHz was deferred to the long tail by owner decision 2026-07-31 (kept
  host-proven, no real source in the estate). Final acceptance re-runs the
  in-scope gates on one exact image;
  [`aius-e2e-latency.md`](aius-e2e-latency.md) was deliberately interleaved
  ahead of them by owner priority.
- **OTA investigation verdict, then implementation** — **owner decision
  2026-07-31: OTA is the next key feature**, ahead of E-AC-3, which is
  considered only after OTA lands. Complete the finite investigation gate in
  [`aius-ota-upgrade.md`](aius-ota-upgrade.md) first (no OTP fuse burn
  implied). By owner decision 2026-07-29, the AC-3 four-hour and by-ear
  gates run only after this verdict as well as sample-rate following.
  Sequencing rider, same 2026-07-31 decision: close the easy-to-prove open
  defects first (30, 18, 10, 19, 20, 35, 2 and 34 dispatched or queued
  2026-07-31, plus B9); anything hard to prove or diagnose waits for the ESI
  U24 XL bench session — the open-gate TX underrun, the ±29.6k impulses,
  both CM6206 discriminators, gate 1, the ITCM mechanism question, and
  hardening defects 25 (conditional rider) and 27 (needs the loopback rig).
  Defects 22 and 23 stay parked as recorded design calls.
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
  - ~~**10**: the network-transport fake still diverges from real FNET in
    five characterized places.~~ CLOSED: fixes landed 2026-07-26, ledger
    verified stale 2026-07-31; all five have discriminating tests and
    `fake-fidelity` now covers `fake/fnet.h` positionally with mutation
    self-checks.
  - ~~**14**: the 128 KB DTCM DTS arena is reserved and never used.~~
    RESOLVED 2026-07-28 with defect 29: the optical env's
    `build_unflags` stripped the wrap set; restored, the arena now
    serves every decode (62.5 KB peak, steady-state no-heap proven
    on hardware).
  - ~~**18**: five source comments still quote the pre-fix DTCM free
    figure.~~ CLOSED 2026-07-31: six comments corrected to the measured
    25,376 bytes (the ledger's five had drifted; its `:292` citation
    matched nothing and two `~48 kB` comments stood there instead), plus
    one decision-era figure clarified rather than rewritten.
  - ~~**19**: `read_once()` carries the latch-substitution hole the send
    path had.~~ CLOSED: fixed 2026-07-26 via the shared
    `send_error_is_closure()` predicate, inverted rows pinned; ledger
    verified stale 2026-07-31.
  - ~~**20**: the network fake gets `_fnet_socket_addr_check_len(addr, 0)`
    backwards and the suite passes anyway.~~ CLOSED for the fake and test
    row (fixed 2026-07-26, queue-unchanged discriminator); the vendor's
    uninitialised `fromlen` accept half remains recorded in the hardening
    ledger, deliberately unpatched.
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
  - ~~**30**: decoded 5.1 plane order vs downmix slot order disagree — a
    pure spatial permutation.~~ CLOSED 2026-07-28 in the hardening ledger
    as NOT A FIRMWARE DEFECT (the tone programme was rotated by `ffmpeg
    join` without `map=` before it was ever encoded); this gist was a
    dated pre-chase snapshot. 2026-07-31: both channel maps independently
    re-verified against the shared mask rule, and the composed
    decode→downmix `downmix-identity` suite now pins every route for both
    codecs with a rotated-front negative control.
  - ~~**31**: sustained live DTS decode watchdog-crashes within ~10 min
    under capture/HTTP load.~~ FIXED 2026-07-29: decode/DSP/TX moved
    out of the ISR; a 1,857-second live DTS soak under recorder and
    measurement/HTTP load held AUDIO to 2.401 ms and the epoch to
    3.036 ms with zero drops, gaps, generation changes or resets.
  - **35**: terminating a live DTS source mid-burst causes one 120 ms
    AUDIO service, 14 ingress overflow chunks / 3,584 dropped words,
    a recorder underrun and an HTTP disconnect before PCM recovery.
    Natural EOF of the same complete 2,813-burst source is clean.
    Capture the exact transition AU, add target decode/sink substep
    timing, then drop partial compressed work on source discontinuity.
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

## Open defects — sample-rate and latency delivery

Recorded 2026-07-31 while taking over the sample-rate-following and
e2e-latency delivery. Authoritative homes are the two plans and the CM6206
characterization; gists plus links where a home exists, full specification
where the only prior record was a plan aside.

- **CM6206 playback corruption** — characterized parallel-datapath high-byte
  corruption in the adapter playback path, leaning systemic. Deferred
  discriminators: the direct non-Icron host test and the second-unit bracket.
  Waveform-critical gates (sample-rate gate 7, encoded AC-3) require a source
  the discriminator has shown clean. Authority:
  [CM6206 characterization](../reference/cm6206-playback-corruption.md).
- ~~**Flight-recorder download header pins 48 kHz.**~~ **FIXED 2026-07-31**
  (`fecc4a0a2473`): the producer tap stamps each committed block's rate into
  the recorder, the header serves the stamped rate plus a `rate_changes`
  counter, and a frozen capture keeps its own capture-time stamp.
  Device-verified: 48,000 on the frozen pre-change capture, 44,100 while
  recording a live 44.1 kHz stream after a thaw.
- ~~**AC-3→PCM transition latches `AIUS_MALFORMED`.**~~ **FIXED 2026-07-31**
  (`fecc4a0a2473`): the operator clear operation now owns observation
  latches; automatic transitions deliberately still do not clear them.
  Device-verified: a truncated burst followed by PCM words latched 3/3, one
  authenticated `POST /api/clear` took both fields to zero with no reboot.
  Post-stop silence never latches (legal pause stuffing), which is why only
  the immediate-PCM adjacency ever showed it.
- **Open-gate 128-frame TX underrun, unexplained.** In the 48→44.1→48
  transition run on `01477755955b`, one isolated 128-frame underrun occurred
  at 11.719 s between two open-gate 48 kHz status samples after the TX level
  reached zero; the other 127,488 underrun frames were attributed to dark
  transition windows, this one was not, and no other harmful counter moved.
  Not root-caused; the transition analyzer keeps it a hard failure.
- ~~**DSP model `source` label conflates SD and PUT.**~~ **FIXED 2026-07-31**
  (`fecc4a0a2473`): one staged origin enum replaces the bool; status gains
  `model_origin` (`default|sd|put`) while `source` stays byte-compatible.
  Device-verified in all three states. Deliberately absent from
  `GET /api/dsp/config` (the strict nine-key round-trip would 400); the
  two-file remedy is recorded in-source if it is ever wanted there.
- ~~**No authenticated read path for `/aius/dsp.json`.**~~ **FIXED
  2026-07-31** (`fecc4a0a2473`): exact-literal read-only GET carve-out for
  `/sd/aius/dsp.json`; uploads, deletes and the listing stay www-only.
  Device-verified (200 with the 676-byte card model, siblings still 404),
  and consumed the same day as the latency bypass row's restoration
  artifact.
- **Three ±29.6k capture impulses match no fingerprint.** The 44.1 kHz PCM
  external observation contained six impulses; three fit the characterized
  CM6206 high-byte fingerprint, three near ±29.6k do not and cannot be
  waived. Raw-input-plane correlation or recurrence data is needed before
  any gate consumes that window.
- **48 kHz gate 1 is unpassable under active CM playback; source inbound.**
  The gate's trim-cadence bound (at most two post-settle trims) assumed a
  stable source; with USB playback active this CM wanders across ten values
  spanning 6.3 Hz and produced 53 post-settle trims in 600 s while the audio
  path stayed clean (reconfirmed twice on 2026-07-31: 11 trims/120 s, max
  follow errors 1,107 and 3,911 millihertz). Designated path, decided
  2026-07-31: the incoming ESI U24 XL (32/44.1/48 kHz, optical S/PDIF,
  internal clock master) gets a ten-minute carrier observation on arrival;
  steady carrier makes it the gate-1 source and a candidate clean source
  for gate 7 and encoded AC-3. If it wanders too, the fallback decision
  (ingress-slope correction term or an owner-approved bound revision)
  reopens. The bound is not weakened unilaterally
  ([`aius-sample-rate-following.md`](aius-sample-rate-following.md)
  §Acceptance).

  **2026-08-02 — the U24 XL arrived and its optical self-loop passed the
  CM6206-discriminator bracket at 44.1 kHz: no CM-class transmit fault at
  any tested rate.** The unit (`0a92:00d1`, full-speed USB, host port 3-1;
  the CM6206 is physically off the bus) ran the CM characterization's own
  125 s direct-loop bracket plus stereo-identity/cadence refinement and a
  bit-exact source comparison, via tooling committed with this entry
  ([`source_qualification/`](../../firmware/tools/source_qualification/README.md)).
  At 44.1 kHz — the CM's worst mode (7–9 impulses and 6.31 % unequal
  stereo frames per 125 s run) — the U24 XL returned zero impulses, zero
  unequal stereo frames, zero off-cadence intervals, and was bit-exact
  against the source tone across all 5.5 M frames, a check the CM could
  never pass. At 48 kHz a 15 s smoke was equally clean (bit-exact), but
  the 125 s bracket was cut at 50.4 s when the device was unplugged
  (21:26:04, bench rewiring in progress; Teensy USB appeared on port 3-2
  at 21:23:30). The salvaged 48 kHz capture is bit-exact everywhere
  except one bounded 103-frame episode at 2.27 s (one boundary frame,
  ~100 frames held at a constant value, net 145-frame slip) exactly
  coincident with a host docker-restart burst — an underrun/starvation
  shape with L/R equal throughout, matching no CM6206 class (not a
  high-byte impulse, not a word-phase slip); host-side attribution is
  likely but unproven, so the rerun decides. Still owed before the
  source is fully qualified: clean full 48 kHz and 32 kHz brackets on a
  quiet bench (needs the operator to re-seat the U24 XL USB and optical
  self-loop), then the ten-minute carrier observation — U24 XL TX into
  the Teensy RX, since a self-loop cannot see carrier wander (capture is
  slaved to the TX clock). Raw runs:
  `.scratch/f100000d-6907-42c1-8ece-ea0c38d7a786/u24xl-loop-*` (7-day
  GC; the numbers above are the complete record).
- **ITCM ingress-regression mechanism unestablished.** The `30eeed071` A/B
  convicted the DSP-runtime adapter's default ITCM placement empirically, but
  no mechanism explains how 144 B of resident cold code starves the live
  producer, and both A/B legs also perturbed global code layout. Flash
  placement is the proven-safe configuration, enforced by the itcm-placement
  guard. Residual risk: a layout-sensitive latent defect elsewhere in the
  ingress path.
- **Non-48 kHz latency rows unmeasured.** Every latency acceptance row pins
  `nominal_rate_hz == 48000` while sample-rate following makes 44.1/32 kHz
  first-class live modes with rate-dependent transmit-ring residence and DSP
  coefficients. Deferred until the sample-rate device gates pass; recorded in
  [`aius-e2e-latency.md`](aius-e2e-latency.md).
