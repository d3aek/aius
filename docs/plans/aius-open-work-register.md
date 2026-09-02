---
title: Aius open-work register
type: plans
scope: component
component: aius
subtype: backlog
status: in-progress
tags: [aius, backlog, debt, defects]
updated: 2026-09-02
toc: true
---

# Aius open-work register

Every open Aius item in one place, assembled 2026-07-27 at the close of the
DSP-editor enhancement (task #7) and the bench-prep session. **This register
points at authoritative homes; it never duplicates their content.** An item
with its own section in another plan carries a one-line gist and a link;
items whose only prior record was a commit message or plan aside are fully
specified here. Strike items here in the commit that closes them.

## Deferred from the 2026-08-15 OTA session

Shipped that session (context, not open items): lowpass EQ type (`6ead65709`
firmware + `9f8234710` editor), steady wall-clock meter-publish cadence
(`461b46552`), the biquad-designer-to-FLASHMEM ITCM fix that funds them
(`db3f554aa`), and three DSP-editor fixes deployed to the card — level-bar
numbers on the ballistic cadence (`6560b0f0f`), dirty-vs-factory reset markers
(`cc8c800b0`), and median-smoothed estimated-latency (`197523cba`). Image B
(current master + those) was OTA-flashed and is decoding cleanly. Two loose
ends left open:

- **Deployed image revision stamp is a commit behind.** The running OTA image
  was built from the working tree just before the ITCM-funding commit
  (`db3f554aa`) landed, so it reports `197523cbab76`, not `db3f554aa8df`. The
  code is correct — it cannot link without the biquad-designer FLASHMEM move,
  and lowpass + meter-cadence are in it — only the provenance stamp is wrong.
  Fix: a clean rebuild from committed `db3f554aa` (or later) and one OTA
  reflash. No functional issue; purely cosmetic provenance.

- ~~**`tx_underrun_frames` counts idle silence and is a `uint32_t`.**~~
  **CLOSED 2026-08-16 via option (b)**: the fill ISR now counts substituted
  frames only while the TX gate is open (`gate_open` flag on the ring, set at
  gate edges inside the existing masked trim section), making the counter a
  true glitch metric that neither wraps in idle nor buries real dropouts —
  the 2026-08-16 dropout evening was uncountable precisely because of this
  defect. Hot-path validation rode the output-jitter plan's adversarial
  review ([`aius-output-jitter.md`](aius-output-jitter.md) Phase D).
  Original entry:
  `aius_spdif_tx24_fill()` counts every substituted-silence frame, including
  during no-carrier / gate-closed idle (~48,000/s), so the counter wraps after
  ~24.9 h of continuous idle. Confirmed **benign**: the only decision-consumer,
  the recorder underrun trigger (`aius_recorder.c`), is wrap-safe
  (resync-on-decrease, lines 162 & 285) AND idle-safe (qualifies only when
  `gate_open`, line 176), so the overflow is cosmetic — the `/api/status`
  lifetime count merely under-reports after a wrap. Surfaced 2026-08-15 when it
  read 1,876,403,584 (≈10.9 h of silence) after a ~11 h no-source window
  post-flash. Options, cheapest first: (a) widen
  `AiusSpdifTx24Ring.underruns` to `uint64_t` (never wraps, keeps current
  meaning); (b) increment only when the gate is open — a true real-glitch
  metric that also never overflows, but it touches the transmit DMA ISR
  (`aius_spdif_tx24_fill`) and wants the same adversarial hot-path validation
  the meter-cadence change got. Pre-existing design, not from this session's
  changes.

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

- **Loose ends from the 2026-09-02 review-fix series (handover retired
  2026-09-02; proposals, not agreed).** Everything decided that day lives in
  its plan: H2 / defect 22 device evidence
  ([`aius-recorder-rolling-buffer.md`](aius-recorder-rolling-buffer.md) Phase
  D, [`aius-network-hardening.md`](aius-network-hardening.md) defect 22),
  gate 7 / gate 2 rulings ([`aius-rate-gate-capture-tool.md`](aius-rate-gate-capture-tool.md)
  §Operator rulings (2026-09-02)), the gate-2 dropout root cause
  ([`aius-sample-rate-following.md`](aius-sample-rate-following.md) §Device
  evidence — 2026-09-02) and its fix phase
  ([`aius-output-jitter.md`](aius-output-jitter.md) Phase F, SHIPPED and
  validated on the board the same evening, image `3570a216216d`). The
  remediation series that followed (2026-09-02, late):
  - ~~`GET /api/recorder` wants `X-Aius-SD-Auth` while `CLAUDE.md` says reads
    need nothing.~~ **The route is right, the rule was wrong**: every recorder
    route is token-gated by the recorder contract, GETs included
    (`dispatch_recorder()` in `aius_optical_app.cpp`, recorded audio + trigger
    state). `CLAUDE.md` corrected.
  - ~~`staged_generation` bumps at PUT accept, before the SD commit, so a PUT
    that fails to commit still advances it while `config_pending` stays
    false.~~ **Moved to commit** (`sd_settle_http_write()`, beside the apply
    arm; [`aius-network-hardening.md`](aius-network-hardening.md) defect 22
    correction (d)). Cross-build proven; **not yet flashed** — the deployed
    `3570a216216d` still bumps at accept.
  - ~~`CONTRIBUTING.md` carries ~12 stale `…` paths.~~
    **Swept** to repository-relative paths; the sd-editor paragraph now names
    the suite runner (`test.sh --suite sd-editor`).
  - ~~`test/sd-editor/fixtures/dsp-config-default.json` lacks
    `staged_generation` / `config_pending`.~~ **By design**: that fixture is
    pinned byte-for-byte to the generated factory model
    (`firmware/config/optical_dsp.json`), which has no runtime keys; the wire
    example that carries them is `dsp-config-sd.json`, refreshed. The swagger
    `DspConfig` schema already declares both.
  - ~~H1 (teardown lease release) is host-proven only.~~ **Accepted on the
    host proof** (recorder plan Phase D).
  - ~~ITCM has 1108 B free and the cold recorder-bank helpers landed there; a
    placement pass is owed before anything else grows.~~ **Done 2026-09-02:
    every definition in `aius_recorder`, `aius_config` and `aius_auth` is
    `FLASHMEM`** (the recorder "producer" is a writer context reached from the
    FLASHMEM `enqueue()`, not an ISR), the three libraries left
    `ITCM_ALLOWED_LDF_LIBS` and are gated to zero `.text`, and the named
    recorder-helper check retired. ITCM 195,500 → 190,764 B used, headroom
    1,108 → 5,844 B; DTCM/OCRAM unchanged. Cross-build + all guards green;
    **not yet flashed**. What is left in ITCM is beyond annotation (nm census
    of the ELF): newlib printf family ≈ 25 KB (`_vfprintf_r`, `_svfprintf_r`,
    `_vfiprintf_r`, `_dtoa_r`), FNET + NativeEthernet ≈ 43 KB, SdFat ≈ 19 KB,
    the AC-3 decoder ≈ 15 KB, the DTS core ≈ 10 KB. Shedding ~32 KB would hand
    a whole FlexRAM block back to the DTCM stack/locals zone. Proposal (not
    agreed): a rule in `firmware/link/aius_teensy41.ld` placing libc's printf
    objects (every caller is a FLASHMEM serializer) and SdFat in `.flashmem`;
    the decoders and FNET only after a measured decode-budget / poll-latency
    check.
  - ~~The `optical-cutover` suite fails at HEAD on a pre-existing `micros()` in
    `update()`.~~ **Fixed**: the suite's `update()` allow-list admits the
    input-fault cadence watermark's `micros()`, which moves no samples.
  - A gate-4 *pass* is unreachable with the aplay stimulus (idle gaps between
    phases): needs a source that cuts the carrier between phases or an
    analyzer decision (tool plan §Follow-on work).
- ~~**Two deploys owed for the 2026-08-16 status-page rework.**~~ **BOTH DONE
  the same evening, owner-directed ("sync and flash"), with audio playing and
  the interruption accepted after it was costed.**
  - **Card synced** (12 files, all 200) and the board serves the reworked page.
  - **`ceffc20dae6c` built for slot A, signed, staged, verified, armed and
    activated over HTTP; the board came back on it and COMMITTED THE TRIAL
    UNAIDED** — `ab` went slot 1 / active 2 / pending 1 / budget 2 on the trial
    boot, then active 1 / pending 0 / sequence 11, `ledger_fell_back` false,
    `rollback_observed` false. This is the second unaided A/B upgrade cycle on
    hardware and the first one nobody was watching for its own sake. The dark
    window stamped `completed`, 2,368 of 2,368 steps, 570,368 B programmed and
    verified, image `9edc29d1c7fd6afe`, 6.36 s dark. `fault_record` and
    `prev_boot` are now live: the first reading is `null` and `final checkpoint
    epoch-complete (10) · first aborting checkpoint none`, i.e. the deliberate
    activation reboot, and checkpoint **10** is a value the page's old
    vocabulary could not name at all.
  - **Two costs recorded, both predicted and both real.** The sync cost ~59 ms
    of `tx_underrun_frames` with a 1.55 s HTTP-checkpoint epoch; staging added
    ~24 ms more across its 140 chunks with the ingress high-water at 5,134
    words (the loop-stall signature). Owner-initiated card traffic is not gated
    behind the transmit gate by design, so this is the standing price of any
    deploy during playback — now stated in
    [`../../CLAUDE.md`](../../CLAUDE.md) §Device access and OTA.
  - **Gotcha worth its own line: sign A/B containers with `--slot`.** The first
    activation attempt was refused at chunk 0 with `400 ota_unsupported`,
    because `aius_ota_sign.py sign` without `--slot` emits a slot-0
    single-image container and the A/B firmware gates the signed slot tag
    against its own inactive slot — the same verdict a wrong target id gets,
    which reads like a broken image rather than a missing flag. Re-signed with
    `--slot a` and it staged first time.
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
  **State 2026-08-04: the jacks ARE soldered and the glass loopback
  locked 2026-07-28** (resolved entry above); what remains of this test
  is the measure flow across glass and the steps needing an EXTERNAL
  source or sink (steps 2–4, 6), as that entry records.
- ~~**The bench SD card needs service (check on a PC, reformat, or swap)
  before the full-size OTA end-to-end (2026-08-01).**~~ **RETRACTED the
  same day — the card was innocent** (owner challenged the verdict;
  the controlled A/B convicted the board's own runtime state, below).
  Keep the card.
- ~~**Darken the CM6206 optical feed for the E-AC-3 replay acceptance
  run (2026-08-02).**~~ **RESOLVED 2026-08-02 evening without the
  software disable: the CM6206 was physically removed from the host
  (port 3-1 disconnect 21:11:09, absent from `lsusb`), the RX went
  dark, and the acceptance run PASSED** — evidence in
  [`aius-eac3-support.md`](aius-eac3-support.md) Phase 4. The residual
  probe below is moot in the observed direction: the start guard
  passed on its first dark attempt, so raw `pllLocked()` did not
  assert without light on this boot. Original item retained for the
  recipe provenance:
  A replay start refuses against a live carrier by
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

- **OTA container verify is a one-epoch ~80 ms stall — an audible dropout
  when staged during playback (2026-08-16).** Measured live while staging
  the `fd9d35fd3f95` slot-B container with AC-3 playing:
  `POST /api/ota/verify` ran as one 79,952 µs HTTP-checkpoint epoch —
  `tx_underrun_frames` +2,560 (53 ms dropout), epoch budget overrun
  counted, ingress high-water jumped 5,376 → 7,680 words (input flowed
  while output starved: the definitive loop-stall signature). Two smaller
  bursts (+512 at a staging burst, +128 mid-stage) rode the same session.
  **The second staging pinned the dominant term: ~80 ms for BOTH the
  1.6 MB and the 570 KB container, so the cost is the Ed25519 signature
  check (TweetNaCl on the M7), not the payload SHA-256** — slicing the
  digest alone will not fix it; the verify epoch needs its own strategy
  (slice the scalar arithmetic, run verify under a gate-aware moment, or
  accept and document the one dropout). Until fixed: stage/verify during
  silence, or accept one ~50 ms dropout per staged release. Related
  device-side improvement candidate from the same evening: the verify
  accepts a payload whose slot IVT is garbage (the operator staged the
  Intel-hex ASCII by mistake; it byte-verified and flashed, and only the
  chooser's IVT-sanity refusal — now field-proven — kept the slot from a
  dead trial boot). Cheap add: validate the staged payload's IVT bounds at
  verify, refusing before the flash is spent. The signer now flattens
  Intel-hex input (incident row in `test_ota_signer.py`).

- **Flight recorder freezes on the boot bring-up underrun, spending its
  one-shot trigger before the session exists (2026-08-16).** Field
  observation on `1dc7cbb80397` slot A: `/api/recorder` reported
  `state: frozen`, trigger `underrun` at `at_ms 3289`, `at_block 1`, 512
  valid frames, `postroll_captured 0` — the recorder froze on the trivial
  first-block underrun ~3.3 s after power-on and was therefore blind to
  every real dropout of the session (an audible pop that advanced
  `tx_underrun_frames` by 384 left no capture). The armed-witness premise
  of the output-jitter acceptance run is void on every boot until the
  operator thaws by hand. Fix shape: suppress the underrun trigger until
  the session settles — e.g. arm it only after the TX gate's first
  closed→open edge (the same settling boundary the gate-open trim owns),
  or after N clean committed blocks. Until fixed: thaw after boot
  (`POST /api/recorder/thaw`, SD-auth) as part of bring-up. Found while
  diagnosing the 2026-08-16 listening-room pop (see
  [`aius-output-jitter.md`](aius-output-jitter.md) Phase E).

- ~~**OTA activation dark window can brick: erase completed, no valid image
  programmed (2026-08-16).**~~ **CLOSED 2026-08-16, the same day, by the
  whole [`aius-ota-resilience.md`](aius-ota-resilience.md) plan shipping
  and bench-proving on the device:** retained dark-window forensics
  (Phase 1, fault-injected stamp read back across a reboot), pad-1-only
  activation staging (Phase 2, full activation with the recorder armed),
  A/B slots with a never-erased first-stage chooser + automatic fallback
  (Phase 3, migrated on the bench and a full unaided A→B→A upgrade cycle,
  wrong-slot + corrupted-container refusals), and the deferred-absorb
  session fix + promoted client (Phase 4, which caught and survived two
  organic mid-stage SD faults during the bench proofs). The queued owner
  decision resolved as: build the second-slot design; flash map 64 KiB
  head + two 3,936 KiB slots, software-defined, no fuse. The interim
  no-remote-activation rule is RETIRED. **Remaining assurance drills
  (operator hands, non-blocking):** (a) a mid-window power cut during an
  activation, expecting the chooser to boot the old slot unaided; (b) a
  real crash-loop trial image, watching `ab.ledger_boots` spend the budget
  and `rollback_observed` persist; (c) Phase 4c's lit-carrier boot row —
  plug an optical source, reboot, confirm the SD dashboard mounts under
  the lit carrier. Original entry:
  First field failure of the staged upgrade: the
  `d5dab0618f4c` container staged clean (387 chunks, one transient 500 whose
  poisoned session was aborted and restaged fresh), device-verified,
  armed, activation acknowledged — then the board never returned. Forensics:
  after a cold power cycle it enumerated as `1fc9:0135` "SE Blank RT
  Family", the i.MX RT ROM serial downloader, i.e. flash held NO valid boot
  image — the dark window erased and then failed to program (or programmed
  garbage; the byte-compare stage evidently never passed/completed). The
  image itself is innocent: the same hex flashed via HalfKay booted first
  try, generation 1, zero early resets, `relocks` 0. Recovery used the
  descoped-by-design boundary (Program button → HalfKay →
  `teensy_loader_cli`), which required physical access — the recorded
  descope ("a dual-image rollback scheme is not required", network-hardening
  §Deferred, quoted in `aius-ota-upgrade.md`) held technically but its cost
  landed on a deployed listening-room device. OPEN QUESTIONS for the chase:
  what the dark window's program/compare path did (the health-mark cell may
  have recorded a phase), whether PSRAM staging content was intact at
  program time, and whether the in-window failure is reproducible on a
  bench flash. OWNER DECISION QUEUED: whether the remote-upgrade
  requirement now justifies the second-slot/boot-health-revert design the
  descope rejected, or an operational rule (activate only with USB in
  reach) suffices.
  **2026-08-16, later: the chase is now the resilience plan**
  ([`aius-ota-resilience.md`](aius-ota-resilience.md)). The health-mark
  question is RESOLVED from source — the mark was a plain `.bss` buffer and
  could not have survived (the plan's forensic-thread section has the full
  verdict); Phase 1 (retained dark-window stamp: phase, step address,
  compare offset, fault code, served on `AIUS ID` as `od=` and
  `/api/status` as `ota_dark`) is implemented with host proof green, so the
  NEXT in-window failure names itself. The PSRAM-staging and
  reproducibility questions become Phase 2's bench soak; the queued owner
  decision is the plan's Phase 3 flash-map decision.
- ~~**Phase C probe guard starves SD bring-up when a carrier is lit at boot
  (2026-08-16, found in post-incident re-review; fix owed).**~~ **FIXED
  2026-08-16 (aius-ota-resilience.md Phase 4c, not yet flashed):** the
  `g_sd_bringup_done` once-per-boot latch exempts the FIRST mount from the
  carrier deferral; steady-state deferral (hot-swap detection, usage
  refresh) is unchanged. On-device confirmation (bring-up completes under a
  lit carrier) rides the next flash. Original entry: the
  output-jitter Phase C guard defers the SD probe on
  `g_tx_gate_open || pllLocked()`; on a boot with the source already
  emitting a carrier, `pllLocked()` is true before the FIRST probe ever
  runs, so the SD lifecycle never mounts: no card, no config load, no
  SD-hosted dashboard until the source goes quiet. Steady-state deferral
  is correct; first bring-up must be exempt (e.g. defer only once
  `g_sd_ready` has been achieved this boot). Not boot-lethal — network and
  audio are unaffected — but the dashboard 404s exactly when the owner is
  using the system.

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

- **Periodic watchdog reset every ~31 min 45 s on the E-AC-3 phase-4
  image (2026-08-02; ~~FIXED 2026-08-03~~ REOPENED the same morning —
  the single-context fix eliminated the ~1,905 s T1-anchored mode but
  a SECOND death mode at ~6,176 s survives it; chain, premature
  closure, and the reopening evidence at the end of this entry.
  2026-08-03 evening: the surviving stall/death family CONVICTED as
  FlexSPI2 unfitted-tail bus holds, `stall_mpu_tighten()` standing
  tripwire fix. 2026-08-04: 10.0 h loaded soak clean, four-hour gates
  UNBLOCKED by owner — see the standing-state note at the end of this
  entry).** Since ~11:23 — right after the phase-4
  image (`5151bb21f8be+teensy41-optical`, built 10:51) went on the
  board — the idle bench board has reset on a 1,904±5 s cadence: 17
  resets in the host kernel log through 21:23:29 (each one
  re-enumerates the Teensy CDC, which is how the host sees it), with
  `reset_source` 16 (WDOG) confirmed on the 21:23:30 boot. Two
  irregular intervals (15:05:41→16:49:02 ≈103 min, 19:59:36→20:20:01
  ≈20.4 min) say it is not a literal timer. Every observed boot is
  otherwise healthy: epoch ~7,710/s, `relocks` 0, `nrst` 0,
  recovery-safe never latched. Suggestive lead: ALL ELEVEN
  `checkpoint_walltime_us` maxima on the current boot read
  1,789,6xx µs — a recurring global stall of one audio-beat length
  (1,791.7 ms nominal, the boot-lottery family's signature period)
  landing in every loop segment, i.e. the interrupts-masked-stall
  shape rather than one slow service — and
  `runtime_epoch_budget_overruns` reached 33 in ~35 min, about one
  stall per minute on a dark-carrier idle board. How a ~1.79 s stall
  family escalates past the 8 s WDOG deadline once per ~1,904 s is
  the open mechanism question. Load-sensitivity note: through a
  ~12-minute live watch plus the E-AC-3 acceptance activity
  (2-second status polling from 21:52), the next expected reset
  (~21:55) had NOT fired by 22:04 — 40+ min into the cycle. ~~So an
  actively-polled board stretches or suppresses the cadence~~ —
  REFUTED overnight: four consecutive deaths under continuous 10 s
  polling landed within seconds of their boot-anchored predictions,
  so the two ~103-minute outlier intervals have some other cause
  (unpinned; both tolerated). Bench impact: any single unattended run
  longer than ~30 min on this image is invalid (four-hour gates
  included); short acceptance runs fit inside a cycle. Distinct from
  the FIXED DPLL boot-lottery defect (its gate holds — `relocks` 0 on
  every boot).

  **2026-08-03 overnight — mechanism convicted at source, fix
  authored (device confirmation pending):**

  - The death is anchored to the first DHCP T1 renewal. Every boot
    dies at boot+~1,895 s against a lease granted at boot+10.3 s;
    four consecutive deaths landed within seconds of prediction
    (23:38:2x, 00:10:1x, ~00:42:2x, ~01:14); `dhcp_updates` never
    reaches 2 (the renewal never completes); and the retained ledger
    stamps checkpoint 5 (HTTP) on both stamped deaths so far.
  - Source conviction: the vendored NativeEthernet registers NO-OP
    FNET mutexes while FNET compiles with `FNET_CFG_MULTITHREADING`,
    and the whole service stack (DHCP client included) runs inside a
    1 kHz PIT ISR (`fnet_poll` IntervalTimer, NVIC 224) while
    NativeEthernetServer's main-loop receive path ALSO spin-calls
    `fnet_poll()` for up to 10 ms per empty-socket check — a
    non-reentrant poll entered from two unsynchronized contexts,
    colliding constantly. The T1 renewal is the ISR side's only
    socket-layer transmit+ACK cascade holding multi-tick DHCP client
    state through those collisions; the wedge follows within seconds
    (both retained stamps sit in the HTTP job, where the main-loop
    spin lives) and the 8 s WDOG reaps it.
  - THE FIX (authored, both images built, host suites green; the
    committed flag default preserves the historic model until
    on-device confirmation): single-context FNET.
    `AIUS_NET_POLL_SINGLE_CONTEXT=1` arms no IntervalTimer and the
    network job drives the poll once per millisecond from the epoch
    loop — the deployment model FNET's no-op mutexes are actually
    sound under. Every fnet_poll caller now routes through
    `EthernetClass::pollProbed()`, which maintains a RETAINED probe
    (`AiusPollProbe`, 64 B in the ldscript's ROM-safe
    `.aius_retained` window; OCRAM baseline consciously moved
    131,328→131,264, still 192 B above the defect-26 floor) served
    as `poll_probe` on `/api/status`: live reentry counters
    (minutes-fast confirmation on the old model) plus the dying
    boot's in-poll context and DHCP state after a wedge. Vendor
    provenance manifests, NOTICE, the network-transport suite (with
    a new stack_poll primitive test) and the swagger drift gate are
    all updated and green.
  - ~~Morning protocol: flash the instrumented flag-0 image → confirm
    live `reent` climbing and one T1 death with `wedges` ≥ 1 (ctx +
    dhcp naming the in-poll wedge) → flip the flag → campaign across
    at least three T1 windows (~100 min) for deaths-gone proof →
    close this entry.~~ **2026-08-03 morning: the in-poll confirmation
    was NOT obtainable — the instrumentation wall again.** The flag-0
    image (identical split-polling model plus the probe) outlived its
    first T1 window with `reent` 0, `wedges` 0 and no completed
    renewal visible, where the uninstrumented image died 25+/25; the
    probe's per-poll cache-flush serialization sits exactly at the
    suspected race site, so the probe itself is the prime suspect for
    masking the wedge (one survived window also has ~10 % precedent
    as a 103-min outlier — not decisive alone, not worth hours of
    discrimination). Acceptance standard switched to the campaign
    delta: uninstrumented `5151bb21f8be` dies every T1 (25+
    consecutive), the flag-1 fix image must survive ≥3 T1 windows
    under the same conditions. The probe stays aboard as a field
    sentinel: any future in-poll wedge stamps `wedges`/ctx/dhcp on
    the next boot.

  **CLOSED 2026-08-03 ~09:38 — campaign PASSED.** The fix image
  (`45153bb5ad43`, single-context polling) survived three full T1
  windows in one unbroken boot generation (97 min, zero generation
  steps, zero wedges, zero in-poll depth, zero HTTP gaps), under
  HEAVIER load than any death boot carried (the full latency row
  session ran inside the window). Bonus, twice confirmed: the ~1.79 s
  beat-stall family is GONE on this image — both pin-advance brackets
  put a zero delta on `runtime_epoch_budget_overruns`, and the whole
  97-minute campaign accumulated 3 ticks (the AC-3 row transitions)
  against the ~1.2/min every predecessor image bled — the beats were
  the ISR polling model too, and committed output recovered the
  ~4.5 % they stole. Honest residuals, neither blocking: (a) the
  in-poll wedge was never named on hardware (instrumentation wall;
  the retained probe remains the field sentinel); (b) renewal
  completion is not yet OBSERVED — `dhcp_updates` stayed 1 through
  97 min while the lease demonstrably persisted, so either the update
  callback skips some ACK path or the reconstructed T1/lease values
  are off; one small follow-up (serve FNET's lease/T1/T2 options) or
  a server-side capture settles it. (Residual (b) CLOSED 2026-08-04:
  the first observed renewal completed — `dhcp_updates` 1→2 — see the
  2026-08-04 bullet below. No boot had ever lived long enough to see
  it.) Unattended runs longer than
  30 minutes are valid again on this image; the four-hour gates are
  unblocked from this defect's side.

  **REOPENED 2026-08-03 ~11:25 — the closure above was premature.**
  Boot 329 watchdog-died at 09:40:40, two minutes after the campaign
  summary printed, at 6,176 s of age — the "103-minute outlier"
  interval of the historical record (2× ~6,201 s), now revealed as a
  SECOND death mode rather than an outlier of the first. The
  campaign criterion (three 1,905 s windows ≈ 95 min) was sized
  against the T1-anchored mode and missed this one by three minutes.
  What stands from the fix: the ~1,905 s mode IS gone (three T1
  windows survived where the predecessor died 25+/25), the
  beat-stall family IS gone, and every latency/bracket result on the
  image remains valid. What does not stand: "deaths eliminated". New
  evidence from the retained probe's first field catch: the 6,176 s
  death stamped checkpoint 5 (HTTP) with `wedges` 0 and `depth` 0 —
  the fatal wedge is NOT inside `fnet_poll` on the single-context
  image; it died in HTTP-job code outside the poll. The DHCP
  schedule question above is now load-bearing for naming what
  happens at ~6,176 s: serving FNET's actual lease/T1/T2 options is
  the next instrumentation, riding the next flash alongside the ring
  normalization. Boot 330 outlived a per-boot 6,176 s anchor
  (watched past it), so the second mode's schedule has jitter or a
  different anchor; a longer observation names it. ~~Unattended-run
  validity is re-bounded at ~100 minutes, not 30; the four-hour
  gates stay BLOCKED.~~ (Superseded 2026-08-04: the 10.0 h loaded
  soak passed and the owner unblocked the gates — see the 2026-08-04
  bullet below.)

  **Two further corrections, 2026-08-03 afternoon (Phase 3 R4 data
  plus the bracket record):**

  - **DHCP is ACQUITTED entirely.** The served schedule (net_diag,
    Phase 3 R4) reads lease 78,239 s, T1 39,119 s, T2 68,460 s —
    HOURS, not minutes. The "T1-anchored" reconstruction
    (1,895 s ≈ lease/2) was a coincidence fit; no renewal has ever
    been due on any observed boot, which is also why `dhcp_updates`
    never ticked and `dhcp_state` never left BOUND. Both death
    anchors (~1,905 s and ~6,176 s) belong to an UNNAMED periodic
    process; candidate hypothesis space for the next session: FNET
    periodic services (ARP aging, TCP timers, IGMP), HTTP-layer
    session state (both retained stamps sit in the HTTP job with
    poll depth 0), or a firmware-side timer wrap. The probe and the
    retained stamps accumulate evidence per death from here.
  - **"The beat-stall family is gone" was overclaimed.** It held on
    the `45153bb5ad43` boot lineage (zero-delta brackets, 3 ticks in
    a 97-minute campaign) but the Phase 3 image's boots re-rolled
    the odds: five brackets across two boots read
    clean / 1-stall / 3-stall / 1-stall / clean, every event ~1.79 s
    with matching committed-block deficits. The single-context
    change REDUCED the family's rate roughly 40× at best, and the
    rate is boot/layout-sensitive exactly as this entry originally
    documented. The mechanism hunt (an audio-beat-length global
    stall) remains open and is plausibly the same process behind the
    surviving death modes.

  **2026-08-03 evening — the stall family characterized end-to-end on
  a live reproducer; count-threshold death hypothesis refuted; witness
  instrument shipped (this commit, flash pending the probe verdict):**

  - **Count anchor refuted in its simple form.** The fast-rate
    discriminator (2 s status polling on a fresh boot,
    `count_anchor_probe.py`) carried the board past 2,500 requests /
    5,200 s alive — 5× the ~500-connection netheap-exhaustion
    prediction. Whatever kills at ~6,176 s, it is not a fixed
    request-count threshold. The heap-telemetry image (`d1aefa470`)
    still flashes next regardless: the netheap decline curve convicts
    or acquits a slower per-connection leak on sight.
  - **Fixed-time anchor refuted the same evening.** The same boot
    crossed 6,481 s alive at 3,164 requests — past the ~6,176 s mark
    under a 5× request rate — joining boot 330's 8,345 s survival.
    Neither count- nor time-anchored: the death is a stochastic
    per-exposure hazard, which is the working model's prediction
    (every TX frame / churn burst a dice roll), and the historic
    ~6,176 / 2× ~6,201 s cluster was sampling, not a schedule. The
    tight clustering of those three samples remains an open oddity
    worth one glance once the witness names the mechanism. Final
    datum: the boot ran to the discriminator's full 3 h cap —
    10,800 s alive, 5,273 requests, 212 stalls at a stationary
    1.18/min, zero drops/rejections.
  - **Flash boundary 2026-08-03 ~15:36: the board runs
    `3cbe28ef3fa1` (heap telemetry + calibrated stall witness).**
    First flash of the witness mis-clocked GPT1 6.25× slow (CLKSRC(1)
    is the 24 MHz-OSC perclk, not ipg) — and its two boot-stall
    records still validated the instrument: at the true rate the tick
    arithmetic closed to ZERO deficit with a clean IABR, correctly
    classifying the boot-adjacent stalls as legitimate long boot work
    rather than theft. Calibration fix committed (`3cbe28ef3`) and
    flashed. Live watch running (`witness_watch.py`, 2 s cadence,
    8 h cap): FNET heap settled at 60,984 B free and reads FLAT
    against request count in early samples — the netheap-leak variant
    is trending toward acquittal on sight; the next 1.79 s-family
    stall on a stall-prone boot gets a calibrated verdict.

  **2026-08-03 ~16:10 — THE WITNESS CONVICTED ON ITS FIRST RUNTIME
  CATCH. The 1.79 s stall spins inside the S/PDIF audio-DMA ISR
  walking 4,096 timed-out PSRAM-space accesses:**

  - The calibrated witness caught a textbook family member at
    1,861 s: `last_us` 1,789,722 with **tick deficit 0** — the
    priority-0 witness ran at full 1 kHz through the whole stall, so
    bus-fabric hold and interrupts-masked theories are REFUTED — and
    `isr_samples` advanced with IABR words `00000001 / — / — /
    00000010`: **IRQ 0 (eDMA channel 0) active** beneath the witness
    for the stall's duration. The S/PDIF driver attaches its hook
    via DMAChannel there (`aius_spdif_sources.cpp`
    `dma.attachInterrupt`); the thief is the audio-update DMA ISR.
  - `flexspi2_intr` latched **0x440 — AHBBUSTIMEOUT (bit 10)** on
    the PSRAM controller, absent from every pre-stall read (0x40
    baseline). The arithmetic then closes exactly: the FlexSPI AHB
    timeout is 0xFFFF bus cycles @150 MHz = 437 µs = 262,144 CPU
    cycles, and 2^30 / 262,144 = **4,096 accesses** — the stall is a
    4,096-iteration walk through FlexSPI2 address space that times
    out per access (fitted PSRAM answers fast; the 16–32 MB tail of
    the MPU window, or a wedged chip, does not). Each access stalls
    the core under one witness tick, so ticks all land (deficit 0)
    while the ISR crawls — and 4,096 dwords is one 8,192-halfword
    ingress-ring's worth, a suspicious shape. A wild/corrupt walk
    pointer landing in 0x71xxxxxx per-boot explains the lottery:
    garbage varies per boot, and most boots' garbage lands in benign
    space.
  - **Heap ACQUITTED formally:** `fnet_heap_free_b` flat at
    ~61,000 B across 1,462 requests on the calibrated boot. No
    netheap leak exists; the count-anchored leak death variant is
    closed.
  - **Next: name the line.** Two convergent paths, either suffices:
    (1) witness increment — capture the preempted context's stacked
    PC (exception-frame walk from the GPT1 ISR) on episode-first
    sample: the next stall hands over the exact spinning
    instruction; (2) code-read `AiusSpdifTx24Hook::isr` /
    `audio_ingress_publish_live` and every walk the ch-0 ISR chain
    performs for a pointer that can go wild into FlexSPI2 space.
    Then fix the pointer bug, and as defence-in-depth tighten the
    MPU FlexSPI2 region to the fitted 16 MB (extmem heap is banned
    and test-enforced, so NOACCESS beyond the probes is
    safe-by-construction and turns any recurrence into a named
    MemManage fault instead of a silent 1.79 s crawl). The death
    connection (a walk long enough to out-spin the 8 s WDOG, or the
    same wild pointer corrupting further) is now the working death
    mechanism pending the same fix.
  - **~17:25 addendum — the recorder ring STRADDLES the suspect
    pad-2 chip, and the PC capture shipped.** `begin_pipeline()`
    places the ring at `0x70000000 + 32 KiB` staging, and
    `aius_recorder_ring_bytes_after_staging` sizes it to 8 MiB on a
    16 MiB board — so the ring's FINAL 32 KiB lands on the second
    chip (`0x70800000..0x70808000`), the pad the owner's board-level
    investigation already flagged as marginal. Two concrete hazard
    surfaces now exist: (a) the MPU FlexSPI2 window is 32 MiB over
    16 MiB fitted — speculative/wild accesses into the unfitted
    upper half time out (fits the idle Poisson stalls); (b) the ring
    tail touches marginal pad-2 under recording load (fits the
    activity correlation). The witness now captures the preempted
    context's stacked PC/LR (naked GPT1 entry hands the exception
    frame to the body; served as `stall_diag.pc`/`lr`, addr2line
    host-side) — the next stall discriminates (a) from (b) and
    names the walking code outright. Fix shape once named: shrink
    the MPU window to fitted size (kills (a) structurally), and
    keep the ring off pad-2 (end the extent at 0x70800000 or halve
    the ring) or retire pad-2 entirely if the chip is confirmed
    marginal.
  - **~17:35 — the PC capture fired twice on a prone boot and turned
    the "walking code" theory inside out; MPU bisect + STS1 shipped.**
    Boot 338 rolled stall-prone (3 family events inside 150 s) and
    both runtime PCs landed at 0x4764/0x4768 — adjacent instructions
    in `AiusSpdifTx24Hook::isr`'s inlined fill loop, IRQ1 (the other
    S/PDIF DMA channel) active, AHBBUSTIMEOUT latched each time. The
    disassembly acquits the loop itself: its load is masked to an
    11-bit ring cursor and its store bounded to the DMA half-buffer —
    structurally OCRAM-only, incapable of addressing FlexSPI2. (The
    other captured PC, `SdioCard::readData`, was the boot-adjacent SD
    class.) So the fill loop is a VICTIM: its accesses queue at the
    AXI/cache interface behind FlexSPI2 transactions that some OTHER
    agent issues — cache eviction/linefill traffic or another AHB
    master — each eating the 437 µs controller timeout. Two closers
    shipped (this commit): `stall_diag.flexspi2_sts1` (AHBCMDERRID/
    CODE — the register that names WHO issued the erroring PSRAM
    command) and `stall_mpu_tighten()` — the FlexSPI2 MPU window
    shrunk at setup-end from 32 MiB to the probed fitted size, found
    by base address not table index. Read the next prone boot as a
    bisect: stalls gone → the unfitted 16–32 MiB tail was the
    surface (and any future toucher faults attributably); stalls
    persist → the timing-out accesses are within fitted space and
    STS1 names the master, pointing at pad-2 marginality or a
    fitted-space wild agent.
  - **2026-08-04 morning — LOADED SOAK PASS, FIRST OBSERVED DHCP
    RENEWAL, GATES UNBLOCKED.** The bisect boot ran a 10.0 h watch to
    cap on one generation: zero stalls, zero deaths, FNET heap flat
    across 17,936 requests — with ~8 h of continuous production DTS
    decode (2,842 + 460+ aplay passes, zero failures, ~96 bursts/s,
    zero rejects) driving the flight-recorder ring through ~1,300
    clean pad-2-tail cycles. The same boot then crossed T1 alive and
    `dhcp_updates` ticked 1→2 with a fresh 24 h lease (86,400 /
    43,200 / 75,600), no restart, IP intact: the renewal path works;
    the single-context closure's residual (b) is CLOSED — no boot
    had ever lived long enough to see it. **Owner decision
    2026-08-04: the four-hour gates are UNBLOCKED** — the validity
    blocker (unattended-run deaths) is gone and this single run
    exceeds gate length 2.5× under heavier-than-gate load; the
    formal AC-3 four-hour acceptance run remains to be scheduled as
    its own protocol, and the by-ear gate waits on the owner.
    Decision-queue evidence: the pad-2 ring clamp looks UNNECESSARY
    (~1,300 clean tail cycles under load); the OTA `test_key`
    production cutover and a formal `dts` bench mode remain queued.
  - **~20:35 — BISECT VERDICT: the unfitted tail convicts,
    strong-but-provisional.** Three consecutive ~58-minute windows
    on the tightened-window image (boots re-rolled deliberately
    between them) ran ZERO runtime stalls — against the wide-window
    image's recent 2-in-4 prone rate, including a boot with 3 family
    events inside 150 s. Honest statistics: p(three clean windows by
    lottery alone) ≈ (1/2)^3 ≈ 12 %, so this is a strong-but-
    provisional conviction pending a long soak, not proof. Reading:
    the 1.79 s stalls were FlexSPI2 transactions into the UNFITTED
    16–32 MiB tail of the core's 32 MiB MPU window — issued not by
    the victim code the PCs sampled but by cache/AXI traffic
    carrying wild addresses that per-boot garbage steers into or
    away from that tail (the lottery) — and by the established
    mechanism chain (per-exposure hazard, checkpoint-5 stamps under
    load, no clock, no counter) the stochastic WDOG deaths are the
    same walks outlasting the 8 s deadline. `stall_mpu_tighten()`
    graduates from experiment to STANDING FIX with tripwire
    semantics: the tail is background-NOACCESS, so any recurrence is
    a named MemManage fault, and the witness + STS1 stay aboard to
    catch a within-fitted residue if one exists. Residuals: (1) the
    wild-address SOURCE is unnamed — the tripwire converts its next
    firing into an attributable fault, so it names itself; (2) the
    recorder-ring straddle onto pad-2 (0x70800000..0x70808000)
    stands as a separate recorded risk for recording workloads —
    clamp candidate ready if recording-load stalls ever surface;
    (3) the four-hour gates stay blocked until one long soak on this
    image passes (the current watch runs as that soak), then
    unblocking is proposed to the owner. (Satisfied 2026-08-04: that
    soak ran 10.0 h clean and the owner unblocked the gates — the
    2026-08-04 bullet above.)
  - **The stall is CPU theft at random epoch phase, duration
    2^30 cycles.** This boot is a strong reproducer (~1.3 stalls/min,
    77+ events) and the poll stream itself resolved per-stall timing:
    arrivals are Poisson (inter-stall CV 0.93, no periodicity, no
    phase-locking), and NINE of eleven `checkpoint_walltime_us`
    maxima read 1,789,602–645 µs — every checkpoint except
    reset-capture and usb-tx has carried the same interval. A stall
    that lands in whatever service happens to be running is not a
    slow service; it is the CPU being taken. The duration is
    crystal-tight (±22 µs = 12 ppm) at ≈2^30 CPU cycles @600 MHz
    (1,789,570 µs) — a COUNTED power-of-two of a PLL-derived clock,
    which kills every analog candidate (flash-erase, PHY autoneg,
    pause frames). The earlier "one audio-beat length" reading
    (1,791.7 ms = 168 blocks) misses by 0.13 % and is retired; the
    beat coincidence was numerology.
  - **Acquitted by counters on the same boot:** PHY link
    (`link_callbacks` 1 across 77 stalls — no drops, no renegotiation),
    HTTP service (`http_service_overruns` 2 vs 77), UDP (0), audio
    transmit (0), network restarts (0).
  - **Community precedent:** PJRC thread 66525 "Teensy 4.1 Freeze
    (~1786ms)" — same fixed-duration freeze family, root-caused in
    2021 to overlapping MPU regions on FlexSPI2 (PSRAM) stalling the
    bus fabric, fixed in Teensyduino 1.54-beta9. Our core carries
    that fix, yet the signature is alive here — on an image that
    reshaped the memory map (retained window, flash-pinned rodata)
    and on a board with a known-odd PSRAM pad-2 fitment. The thread's
    "GCC version" and "USB type" sensitivities read as the same
    layout lottery this register already documents.
  - **Working model, unifying stall and death:** a bus-fabric-level
    CPU hold (FlexSPI2/speculative-access class) whose bounded form
    is the 1.79 s stall and whose unbounded form wedges until the
    8 s WDOG — plus the FEC driver's own unbounded TX spins
    (`fnet_fec_output`: descriptor wait and TDAR errata loop, both
    "blocked here forever" per upstream's comment) as a second
    death-shaped candidate sitting exactly where all three retained
    death stamps point (checkpoint 5, poll depth 0: the HTTP job's
    socket sends). Under load most epoch wall-time IS the HTTP
    interval, so uniform-phase theft also lands ckpt-5 stamps.
    Activity correlation falls out naturally: every TX frame and
    every memory-churn burst is a hazard exposure — no count
    threshold, no clock.
  - **Shipped (this commit): the `stall_diag` witness.** GPT1 at NVIC
    priority 0 (own IRQ — deliberately not IntervalTimer: all four
    PIT channels share IRQ_PIT, which stack_start() demotes to 224
    every generation) ticks 1 kHz; across any ≥100 ms epoch the loop
    latches the witness's tick deficit, and the witness itself
    snapshots NVIC_IABR the first tick it survives inside a stale
    heartbeat window. Deficit ≈ duration → interrupts-masked section
    or bus-level hold; deficit ≈ 0 → a named ISR spin (IABR carries
    the culprit). FLEXSPI/FLEXSPI2 INTR words (AHBBUSTIMEOUT bit 10,
    SEQTIMEOUT bit 11) are read after every stalled epoch. Served on
    `GET /api/status`, swagger + drift gate green, ITCM hot-set pin
    justified (a FLASHMEM witness would stall on the very FlexSPI
    holds it exists to witness).
  - **Next, in order:** (1) probe verdict at/past the 6,176 s mark →
    flash the heap+witness image; (2) first witnessed stall names the
    thief class; (3) if bus-hold: tighten the MPU at runtime —
    FlexSPI2 NOACCESS is safe-by-construction here (extmem heap is
    banned and test-enforced; only boot probes touch PSRAM) and doubles
    as a tripwire (any genuine toucher faults with an attributable
    PC); SEMC region likewise. If ISR-spin: the IABR word names the
    handler and the fix goes where it points. (4) The FEC TX spins
    want bounding regardless (drop + counter beats a WDOG death);
    that patch needs FNET vendored or overridden — decision deferred
    until the witness verdict says whether it is the death mechanism.
    (This list is a 2026-08-03 13:59 record, overtaken same day:
    steps 1–3 executed — flash boundary ~15:36, witness verdict
    ~16:10, MPU tighten ~17:35 and its ~20:35 graduation to standing
    fix, all timestamped bullets above. Step 4's verdict input
    arrived — the bisect convicts the FlexSPI2 bus-hold class — but
    the FEC-bounding decision itself remains unrecorded.)
  - **Standing state as of 2026-08-04 (audit note).** The ~1,905 s
    T1-anchored mode is gone (single-context FNET polling,
    `45153bb5a`); the 1.79 s stall family is convicted as FlexSPI2
    unfitted-tail bus holds with `stall_mpu_tighten()` the standing
    tripwire fix (`58596243d`); the 10.0 h loaded soak passed with
    zero stalls and zero deaths and the first DHCP renewal was
    observed (`6950202b7`); **the owner unblocked the four-hour
    gates 2026-08-04.** Open residuals: the wild-address source is
    unnamed (the tripwire converts a recurrence into a named
    MemManage fault), the recorder-ring pad-2 straddle stands
    recorded (clamp looks unnecessary on the soak evidence), the FEC
    TX-spin bounding decision is unrecorded, and the formal AC-3
    four-hour acceptance run is still to be scheduled.
  - **8.00 h witness + heap watch on `930f003c6e6e+teensy41-optical`,
    2026-08-04 14:13–22:13 (boot generation 346 held throughout).**
    Zero deaths, zero resets: the generation never stepped, so this is
    the longest clean unattended run recorded on this bench and it
    clears the four-hour bar twice over. Three findings.
    - **The FNET heap-decline hypothesis this watch existed to test did
      not reproduce.** Free heap is flat across 13,866 served requests:
      first-half mean 61,075 B against second-half mean 61,099 B, a
      +24 B drift over eight hours. The 63,744 B opening sample is a
      pre-settle boot transient, not the top of a slope. Steady state
      sits at ~61,000 B with a 59,008 B transient minimum and the
      largest free block tracking free space (60,648 B at exit), so
      there is no fragmentation trend either.
    - **Zero SPONTANEOUS stalls in eight hours.** The watch logged one
      stall event, at age 188.7 s: 121.4 ms, `last_deficit_ms` and
      `max_deficit_ms` both 0. That event was operator-induced, not a
      residual firing on its own — the watch's age 0 is 14:13:16.9, so
      age 188.7 s is 14:16:25.6, inside the defect-35 mid-burst kill
      campaign that was running concurrently (`d35_reproduce.py`
      14:15:32 → `d35-repro-1.log` final tally 14:16:56, whose round 5
      recorded `service_max_step_us` 118,768 µs and whose closing tally
      recorded 121,193 µs against the witness's 121,363 µs). It is
      therefore the accepted defect-35 parse pathology being provoked
      on purpose (`e33022292`), and the remaining 7.9 h ran with no
      stall of any class. No 1.79 s-class event occurred at any point;
      `flexspi2_sts1` read clean (`00000000`) on the one sample, which
      is the expected signature for the parse pathology rather than
      the convicted FlexSPI2 bus-hold family. Epoch-budget overruns
      moved 2 → 3 over the same window, i.e. the single induced event
      is the only budget overrun in eight hours.
    - **New observation, not previously recorded: 336 of 13,851 status
      polls (2.4 %) drew no HTTP response inside the 4 s client
      timeout while the board stayed alive** (generation unchanged
      across every one). First at age 4,289 s, then spread to
      28,755 s. Whether this is poll-probe cache-flush serialization,
      an FNET service-window transient, or a client-side artifact is
      unnamed; it costs nothing at audio and did not precede the stall.
      Raw log retained at `.scratch/<session>/witness-watch-d35.jsonl`.

- ~~**Replay schemas absent from swagger.**~~ **CLOSED 2026-08-03:** all
  five `/api/replays*` routes documented (listing with its truncation
  contract, start/stop/delete with their strict bodies and per-token
  refusals, status with full vocabulary). The status document joined
  the drift gate's emitter table; the listing's row shape — a loop
  with a raw-character tail the extractor cannot assemble — is pinned
  against the row format literal directly, so a renamed row key still
  fails the suite. (Originally: `/api/replays*` request/response
  bodies served but undocumented; the contract suite asserted nothing
  about them.)
- ~~**`AIUS ID` worst-case length "proof" is stale (2026-08-03).**~~
  **CLOSED same day**: the `AIUS_ID_MAX_PAYLOAD` sum now carries every
  field including `cw=` and the 2-digit checkpoint domain, funded by
  display-saturating `ngen`/`bgen`/`rsrc`/`nrst` at 99,999 and `cw`'s
  microseconds at 9,999,999 (this line only; `GET /api/status` stays
  full-width — nothing diagnostic lives above the caps, and a stall at
  or past the 8 s WDOG deadline is a named death, not a measurement).
  Worst case now sums to 251 of 256. The doc's checkpoint table,
  `pv`/`fa` domains and the previously undocumented `cw` field were
  refreshed alongside
  ([`usb-control-protocol.md`](../reference/usb-control-protocol.md)).
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
  entry above remains open. (That sibling closed 2026-08-03,
  `623ecc942`: all five `/api/replays*` routes are now documented and
  gated — see its entry above.)
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
- ~~**The DSP editor shipped a spectrogram no device can feed (2026-08-16).**~~
  **CLOSED the same day.** Output-jitter Phase A dropped `analysis_window` from
  `/api/meters` in `b4a3914ce` and its own recorded outcome said "the dsp-editor
  renders without the spectrum underlay and spectrogram" — but only the firmware
  half shipped. The page kept the canvas, the client-side FFT, the band mapping
  and a status line reading *"history is paused"*, which describes a transient
  gap rather than a feed that no longer exists; the owner found it on the
  deployed board. Owner decision: **delete, not hide.** Gone: the spectrogram
  canvas and its status line, the spectrum underlay, `fftInPlace` /
  `spectrumBins` / `spectrumBandsOf` / `bandEdgeHz` /
  `appendSpectrogramColumn` / `spectrogramLevel`, the ten `SPECTRUM_*` tunables,
  three pieces of page state, the per-frame plot redraw they justified, their
  CSS rows and ~170 lines of analyser coverage. 230 editor rows green, verified
  in a browser against the live board (three canvases left, no spectrogram
  nodes, meters live, no console errors). Owed: the three DSP editor captures in
  [`../reference/user-interface.md`](../reference/user-interface.md) still show
  the removed feature, and the hero's clip recipe needs a new lower anchor now
  that `#spectrogram` is gone (`#gr-history` keeps its "opens and closes on a
  live graphic" property). Deliberately NOT re-rendered from the deployed board:
  every capture run costs audio dropouts, and the recipe wants a stub anyway.
- **Cost-model rows carry the old Cliff quotes** for the optical jacks
  (`aius-cost-model.md`); the purchased parts are RS PRO 805-1671/805-1680.
  Update when the cost model is next touched.
- ~~**Status page misreported the checkpoint vocabulary and read the boot
  config as a standing fault (2026-08-16).**~~ **CLOSED the same day**, owner
  report ("the status html always shows configuration valid no / configuration
  error not loaded (code 10) / errors since boot 1 — I don't think this
  reflects the state"), three defects found from one live `/api/status`
  capture:
  - **Checkpoint off-by-one.** `status.js`'s `CHECKPOINTS` list carried the
    pre-audio ten-name vocabulary, so every value from 9 up rendered one name
    early: a board that starved in the AUDIO checkpoint reported
    `epoch-complete (9)`, the one word meaning the epoch finished. `swagger.json`
    carried the same stale list and bounded `last_checkpoint` at 9, where
    `aius_net.c` has always bounded it against
    `AIUS_BOOT_CHECKPOINT_EPOCH_COMPLETE` = 10. Both corrected;
    [`usb-control-protocol.md`](../reference/usb-control-protocol.md) already
    carried the right table and its own note that an earlier revision predated
    the audio checkpoint — so the fix was available in-repo the whole time and
    two consumers had drifted from it independently.
  - **The configuration card conflated two unrelated files.** It put "DSP model
    source" beside "Configuration valid: no / not loaded (code 10) / 1 error" in
    warning colour, on every device: nothing writes `/aius/config.json`, and its
    three feature flags (`sd_web`, `remote_control`, `telemetry`) have **no
    firmware consumer** — grep finds only `aius_config.c`'s own defaults. Read
    together the card looked like a verdict on the DSP editor's *Apply*, which
    writes `/aius/dsp.json` and cannot create the other file. The card now names
    both files, renders an absent boot config as the normal state with no warn
    tone (only a parser-refused file is marked), and adds a `model_origin` row
    that distinguishes an editor apply from a boot load.
  - **Neither the firmware slot nor the new forensics were on the page at all.**
    The board card now carries the running slot, the ping-pong boot record, the
    chooser trial ledger, `prev_boot`, `fault_record`, `ota_dark` and
    `stall_diag`. Absence is rendered in its two distinct forms — *not reported
    by this image* for a key the build does not serve, versus the record's own
    empty spelling — because merging them would invent a clean bill of health.
  Proof: 33 rows in `test/sd-editor/status.test.js` (up from 20) plus a browser
  probe against the live device document and both doc captures re-rendered.
- ~~**Unverified: does a DSP editor apply land on the deployed board?
  (2026-08-16.)**~~ **CLOSED the same evening: THE APPLY PATH IS SOUND.** The
  discriminating test below was run against the deployed board (slot A,
  `ceffc20dae6c`, AC-3 playing): `PUT /api/dsp/config` with the device's own
  model and its `base_generation` answered `200 {"ok":true}`,
  `config_generation` went 1 → 2, and `model_origin` flipped `sd` → `put`.
  Audio never broke — `overruns` 0 with blocks climbing across the apply. So
  the editor's write, the SD commit and the block-boundary swap all work on the
  deployed device, and the report that prompted this was the status card
  showing an unrelated file's "not loaded" verdict in warning colour beside the
  DSP model. Note for the next reader: **`model_origin` is the field that
  answers this question and it resets to `sd` on every reboot**, because the
  boot load re-applies the same file through the SD stager — so "no apply has
  landed" and "the board has rebooted since the apply" are indistinguishable
  from a cold reading, which is exactly why the live counters could not settle
  it before the test. Original entry: raised by the owner alongside the card
  confusion above
  ("I think there is an issue with the dsp editor apply to device if there is no
  config, this has been clicked more than once"). What the live board says, and
  it does not settle it either way: `source` `sd` with `model_origin` `sd` means
  `/aius/dsp.json` EXISTS and was loaded at boot, so an apply has committed at
  some point; but `config_generation` is 1 and the board was ~14 minutes into
  `boot_generation` 3 at the reading, so no PUT has landed *this boot* and the
  counters carry no evidence about the clicks. The card defect above is
  sufficient on its own to explain the report. Discriminating test, cheap and
  owed: with audio idle, `GET /api/dsp/config`, PUT the same model back with its
  `base_generation`, and watch `config_generation` increment and `model_origin`
  flip to `put`. Needs the SD auth token (physical USB `AIUS AUTH`) and a quiet
  moment — the apply rebuilds the biquads at the next audio block boundary.

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
- ~~**OTA investigation verdict, then implementation**~~ — **COMPLETE,
  owner-confirmed 2026-08-03.** The investigation gate closed and the
  implementation shipped ([`aius-ota-upgrade.md`](aius-ota-upgrade.md)
  `status: shipped`; the signed end-to-end upgrade completed on
  hardware, `67c44a381`, after the staging path, operator-latched
  activation, PSRAM dark-window activation and both rehearsal arms
  landed). Verified live on the current bench image (`249894edb20c`):
  `ota_diag` serves idle with key fingerprint `78cafee9c5728f06`,
  PSRAM staging available. Standing note, not a blocker: the image
  runs with `test_key: true` — the production-key cutover remains the
  documented security-posture follow-up in the plan. (Superseded by
  the 2026-08-04 home-key rotation below: `test_key` is now false;
  the off-repo production-key cycle stays the queued later step.)
  Historic
  sequencing riders below are preserved for the record.
  **2026-08-04 — HOME KEY ROTATION, owner-directed.** The standing
  device key is now the repo-committed home pair
  (`firmware/config/ota-home-signing-key.{pub,sec}`, host-entropy;
  the repository is the deployment's declared trust boundary), the
  compiled key is GENERATED from that file resource at build time
  (`build_config.py` → `aius_ota_public_key.h`; the source carries no
  key bytes and `test_key` is DERIVED by fixture comparison — a
  hardcoded serializer literal that missed the rotation was caught on
  the device and fixed, `17bd5d077`). Acceptance on hardware:
  sign-with-committed-secret → host verify (ARMED) → device stage →
  `verified` → abort; fingerprint `df29355e5a8d7b8c`, `test_key`
  false. Production cycle stays the documented later step (off-repo
  keygen, swap the file, rebuild, one signed upgrade). Defect-2 datum
  from the acceptance: three intermittent 507 `ota_storage` chunk
  faults across ~1,150 staged chunks at random offsets, each
  transparent to a resuming client (the route's own next_offset
  protocol); NOTE the OTA route's chunk fault does NOT latch into
  `sd_diag.last_failure` (a boot-time root job held the latch) — a
  diagnostic gap for defect 2's conviction read.
  (Original entry: owner decision
  2026-07-31: OTA is the next key feature, ahead of E-AC-3, which is
  considered only after OTA lands. Complete the finite investigation gate in
  [`aius-ota-upgrade.md`](aius-ota-upgrade.md) first (no OTP fuse burn
  implied). By owner decision 2026-07-29, the AC-3 four-hour and by-ear
  gates run only after this verdict as well as sample-rate following.)
  Sequencing rider, same 2026-07-31 decision: close the easy-to-prove open
  defects first (30, 18, 10, 19, 20, 35, 2 and 34 dispatched or queued
  2026-07-31, plus B9); anything hard to prove or diagnose waits for the ESI
  U24 XL bench session — the open-gate TX underrun, the ±29.6k impulses,
  both CM6206 discriminators, gate 1, the ITCM mechanism question, and
  hardening defects 25 (conditional rider) and 27 (needs the loopback rig).
  Defects 22 and 23 stay parked as recorded design calls.
- ~~**DSP roadmap item 4 — surround decorrelation.**~~ **DELIVERED
  2026-08-04** as `matrix.surround_delay_ms` (default 0 = the bit-exact
  pre-feature fold; details in
  [`aius-dsp-config-api.md` §Accepted order](aius-dsp-config-api.md#accepted-order-for-the-next-level)).
  The audible on/off choice and delay-time voicing belong to the owner's
  ears round; ~~the loop measurement (impulse-vector arrival shift) rides
  the next characterization pass~~ — loop measurement DONE later the
  same day (`5efdce47d`): differential impulse run on the flashed
  `fd65a530347b` shifted exactly the two SL-in-L/SR-in-R arrivals by
  exactly +384 samples = the commanded 8 ms, every other channel
  unmoved. Remaining: the ears round (on/off + voicing, plus the
  lfe_gain A/B below). Related finding recorded the same day:
  the LFE −8.9 dB observation was the TEST VECTOR (ffmpeg's DTS encoder
  stores LFE ~9.8 dB down; our decode chain is exact), so an lfe_gain
  A/B (0.316 vs 1.0) joins the ears-round queue for convention-encoded
  film content.
- **B9 — the app dashboard retirement**, keeping `swagger.json` and
  its contract suite. Independent of everything above
  ([`aius-network-hardening.md`](aius-network-hardening.md), B-series
  table).
- **Open defects** — authoritative list with full context:
  [`aius-network-hardening.md` §Open defects](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking).
  Open as of 2026-07-27, gists only:
  - ~~**2**: `500 sd_failure` on large uploads ~1 in 15; diagnostics landed
    in-image; flash-reproduce-read `sd_diag.last_failure` remains, and the
    recorded one-line fix is deliberately unapplied until convicted.~~
    **CONVICTED AND FIXED 2026-08-04**: a busy-window mislabel — mid-stream
    upload chunks colliding with root SD jobs holding the volume
    (~1/400 chunks; × 8–32 chunks/upload = the historic "~1 in 15"),
    answered as a hard storage error instead of the retryable busy. The
    OTA chunk route's `sd_busy()` case now answers `503 sd_busy` like
    every other SD route; the Phase-14f yield fix stays unapplied (the
    conviction points away from it). Full record:
    [`aius-network-hardening.md` defect 2](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking).
  - **4**: `record_event` has two call sites; everything else is a
    counter — prerequisite to any push observability.
  - **36 — CONVICTED ON HARDWARE 2026-08-05, cause is defect 38, fix
    committed but NOT FLASHED.** The board came back on Ethernet running
    the same image and the discriminating watch ran: 2.50 h, 4,481 polls,
    3 failures, `http_client_drops` 0 → 3 incrementing exactly once per
    failure. That counter's arm is reachable on a plain GET only via
    `AIUS_HTTP_FAULT_RESPONSE_TIMEOUT`, so the firmware aborted its own
    response each time; two of the three failures were `IncompleteRead`,
    the truncation signature itself. Both hypotheses hold and compose: a
    lost segment stalls the write for FNET's ~1.0–1.5 s first RTO (the
    failures took 1.298/4.022/1.178 s, one slow success 1.389 s) and the
    100 ms deadline turns that recoverable stall into a truncated reply.
    Defect 38's fix should close it; re-running the watch on the fixed
    image is the proof. Rate was 0.067 % today against 2.4 % on 08-04
    with the board idle rather than under audio load, so the rate is
    load-dependent and the "71.5 min onset" is not a fixed threshold.
    Original static entry:
    2.4 % of `/api/status` polls return no complete body on the current
    image. Durations are strictly bimodal (nothing between 0.30 s and
    1.00 s), and a three-build bisect is clean: the previous build ran
    10.0 h with zero polls even slower than 0.30 s, while this one shows
    550 polls over 1.0 s — one firmware commit apart (`930f003c6`, which
    only adds two `/api/status` fields). Leading mechanism is a response
    segment recovered by FNET's ~1.0–1.5 s first RTO. DHCP T1,
    `micros()` rollover, TIME_WAIT pressure and CPU starvation are all
    refuted with evidence; the 71.5 min onset is unexplained. Next step
    is one poll-side observation of `http_client_drops`, no firmware
    change needed.
  - ~~**37**: every accepted socket inherits a 2,048 B send buffer from
    the listener, so the `fnet_user_config.h` 32 KB is dead and a ~7 KB
    response stalls at least three times.~~ **FIXED 2026-08-05, NOT YET
    FLASHED**: `accept_once` raises SO_SNDBUF to 8,192 B on the accepted
    socket. Deliberately not `setSocketSize()`, which is global and eager
    and would have added ~98 KB of boot-time OCRAM allocation (defect
    26's bank); measured OCRAM heap is unchanged at 131,232 B. Hosting
    the call needed ITCM the image did not have — 20 bytes of headroom —
    so `accept_once` moved to `FLASHMEM`, leaving 148 bytes free.
  - **39** (opened 2026-08-05, static): the upgrade page reads six
    `/api/ota` fields the firmware does not emit — `latch_armed`,
    `image_length`, `staged_length`, `fail_reason`, `key_fingerprint`
    and a top-level `target_id` — against the firmware's `armed`,
    `total_bytes`, `staged_bytes`, `fault`, nested `key.*` and nested
    `image.target_id`. Swagger's `OtaStatus` matches the firmware, so
    the page is the stale side. Four cells read wrong on a real device;
    the page still works. `upgrade.test.js` stubs the page's own names,
    so the suite passes and the fixture encodes the bug — any fix must
    re-pin it to `OtaStatus`.
  - ~~**38**: `RESPONSE_TIMEOUT_MS` (100 ms) is not re-armed on
    `WOULD_BLOCK`, so a slow peer makes the firmware truncate a response
    whose `Content-Length` it already promised, then blame the client.~~
    **FIXED 2026-08-05, NOT YET FLASHED**: raised to 5,000 ms, which
    spans two consecutive FNET RTO recoveries. The design is unchanged —
    the deadline still bounds lack of forward progress — but is now
    reachable only by a peer that has genuinely stopped. Regression
    `send.survives-would-block-past-retransmit` fails on the old
    constant.
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
  - ~~**35**: terminating a live DTS source mid-burst causes one 120 ms
    AUDIO service, 14 ingress overflow chunks / 3,584 dropped words,
    a recorder underrun and an HTTP disconnect before PCM recovery.~~
    **CONVICTED AND RECHARACTERIZED 2026-08-04; harmful expression
    already resolved, bounded residual accepted with permanent
    instrumentation.** The prescribed substep timing landed
    (`audio_diag.service_max_step_us` / `service_max_ingest_us`,
    `930f003c6`) and an 8-round instrumented kill campaign convicted
    the mechanism on the first try: every mid-burst cut hands the
    decoder ONE length-complete AU whose tail is stuffing garbage
    (the cut is invisible to length-based burst assembly), producing
    exactly one `decode_rejects` per cut — usually a fast reject, and
    ~1-in-8 a pathological slow parse (measured 118.8 ms in one
    decode step; ingest never exceeded 48 µs). The parse cost is
    bounded by construction (the core parses at most the frame's
    declared size), and the 2026-07-29 collateral is GONE on the
    current architecture: across all 8 cuts, ZERO dropped
    chunks/words, zero recorder impact, zero stop/stall churn, zero
    output drops — defect 31's ISR-to-checkpoint move absorbed what
    used to starve ingress. The prescription's "drop partial
    compressed work" is recorded as not implementable at the parser
    (a payload-length-complete burst with a garbage tail is
    indistinguishable from a valid one), and decoder-internal early
    abort is vendor surgery not worth one bounded silent budget
    overrun per channel change. Residual accepted: one >8 ms AUDIO
    epoch per unlucky discontinuity, inaudible (TX ring rides
    through). **Family churn observation:** not reproducible today
    under either targeted trigger (8 clean cuts; one garbled-stream
    round — a garble from the start never classifies DTS, so nothing
    churns); stays a watch-for with its documented `POST /api/clear`
    remedy, and the substep maxima now attribute any recurrence on
    sight.
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

- ~~**CM6206 playback corruption** — characterized parallel-datapath
  high-byte corruption in the adapter playback path, leaning systemic.
  Deferred discriminators: the direct non-Icron host test and the
  second-unit bracket.~~ **CLOSED AS OVERTAKEN 2026-08-03 (owner
  decision).** The discriminators existed to decide whether the CM6206
  could be trusted as a measurement source; the CM was physically
  retired from the bench on 2026-08-02 and the qualified clean source
  is now the U24 XL (bit-exact self-loop at 44.1/48 kHz, single-value
  carrier over ten minutes), so they no longer gate anything. The
  requirement itself — waveform-critical gates use a source the
  discriminator has shown clean — is satisfied by the U24 XL
  qualification. The
  [CM6206 characterization](../reference/cm6206-playback-corruption.md)
  stays current as the fingerprint record for any future CM-sourced
  capture.
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
- ~~**Three ±29.6k capture impulses match no fingerprint.** The 44.1 kHz
  PCM external observation contained six impulses; three fit the
  characterized CM6206 high-byte fingerprint, three near ±29.6k do not
  and cannot be waived.~~ **CLOSED AS OVERTAKEN 2026-08-03 (owner
  decision).** The observation window belongs to a CM6206-sourced
  44.1 kHz capture; the CM is retired and every waveform gate reruns on
  the qualified U24 XL, so no gate will ever consume that window. If a
  ±29.6k-class impulse appears in a U24 XL-sourced capture it is a NEW
  observation against the new source, not a revival of this one.
- **48 kHz gate 1 is unpassable under active CM playback; source
  QUALIFIED 2026-08-02 (U24 XL, resolution at the end of this entry).**
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
  the 125 s bracket was cut at 50.4 s when the device dropped off the
  bus at 21:26:04.

  **Same night, the drop was root-caused as port-side VBUS overcurrent
  (the first "operator unplug" reading was wrong — owner confirmed
  nobody touched the bench).** With the device un-enumerated, the
  kernel's `usb3-port1/over_current_count` was found ringing at 7.8
  pulses/s — a power switch auto-retrying against a persistent
  overcurrent — and back-extrapolation puts onset ≈21:25:30, inside the
  49 s window between the capture's only glitch episode and the
  disconnect. Everything else is acquitted: PCIe AER counters all zero,
  no xhci errors, ports 3-2/3-3/3-4 untouched (port 2's OC count is 0),
  not the CM-darkening deauthorize recipe (that leaves the sysfs node
  present; 3-1's is gone). Transient OC pulses never reach the kernel
  log (`dev_dbg` only), which is why the event train was invisible
  until the counter was read. Reread in that light, the salvaged
  capture is bit-exact everywhere except one bounded 103-frame episode
  at 2.27 s (wall ≈21:25:15: one boundary frame, ~100 frames held at a
  constant value, net 145-frame slip) — a brownout prodrome, not a
  data-path fault, and matching no CM6206 class in any reading (not a
  high-byte impulse, not a word-phase slip, L/R equal throughout).
  **A short is NOT required (owner challenge, same night): the
  leading no-short mechanism is an inrush-retry lock.** One transient
  trip of port 1's power switch (candidates: a shared-5V sag from
  coincident peaks — the Teensy's 21:23:29 WDOG-reboot inrush, CI
  load, the U24 XL's duplex draw — on a no-name uPD720201 card with a
  blank subsystem ID and unknown supplemental-power provisioning),
  after which every ~7.8 Hz automatic VBUS retry meets the U24 XL's
  input-capacitance inrush plus boot current and re-trips before
  enumeration. Self-sustaining, device innocent. Steady-state
  aggregate overload is ruled out: the same four devices with heavier
  streaming ran 13.8 min pulse-free (and the CM6206 for weeks), the
  survivors declare only 120 mA combined (FTDIs 10 mA each, Teensy
  100 mA), and through 25k+ pulses on port 1 the other three devices
  never glitched (port 2 OC count 0). Onset back-extrapolation
  (7.0–7.8 pulses/s across samples) brackets 21:21–21:25, spanning
  both the Teensy reboot and the capture episode.
  **CONVICTED the same night by the executed bench bracket: the
  original USB cable's power path.** (1) Unplug → pulsing stopped dead
  (counter froze at 28,027 after ~50 min at 7.8/s). (2) Cold replug of
  the same unit + same cable, quiet bench → instant re-trip at 7.8/s
  with no enumeration, killing the inrush-retry-lock hypothesis. (3)
  Second cable → zero OC pulses but repeated `-71` protocol failures
  through a kernel-commanded port power cycle (that cable's data
  lines are bad; noise, not signal). (4) Third cable, same port, same
  unit → clean full-speed enumeration (full descriptors, 500 mA
  declared, one initial `-71` retry), zero OC, and a clean unity
  smoke bracket through the still-wired optical loop. Reconstruction:
  cable 1 streamed bit-exact for 13.8 min, developed a resistive VBUS
  short at 21:25:15 (the capture's brownout episode is the short
  forming), hard-shorted at 21:26:04, and fed the switch's 7.8 Hz
  auto-retry loop (~28k trips) until unplugged. Device, port, card
  and aggregate draw all acquitted; the port performs normal power
  cycles on command. RETIRE CABLE 1 (mark it faulty — its data lines
  test fine, which makes it a trap).

  **Owed brackets rerun the same night on cable 3: 48 kHz PASSED
  pristine** — zero impulses, zero unequal stereo frames, zero
  off-cadence intervals, bit-exact against the source over 6.0 M
  frames, with the port-1 OC counter frozen through the full duplex
  load (the port is definitively healthy under streaming). **32 kHz
  FAILED with a new, non-CM signature:** the LEFT subframe arrives
  corrupted in 8.4 % of frames (336,498 of 3,996,437) in bursts of
  bit-inversion-family damage — residuals cluster at odd ±819
  (one's-complement of the tone value), bits 9–15 flip together in
  96 % of events, and rapid sign alternation puts ~218k spurious
  crossings on channel 0 — while the RIGHT subframe is clean (5
  boundary frames in 4.0 M). Not the CM6206 class (no isolated
  high-byte impulses; single-rate, single-channel, dense). A
  self-loop cannot localize TX vs RX for this; the Teensy-side
  32 kHz observation will discriminate. **Practical standing: the
  U24 XL is a qualified clean discriminator source at 44.1 and
  48 kHz; 32 kHz use is blocked pending the Teensy-side check
  (32 kHz was already long-tail by the 2026-07-31 owner decision).**
  **Ten-minute carrier observation PASSED the same night (fibre moved
  to U24 XL TX → Teensy RX): 311 samples over 630 s under active
  48 kHz USB playback read ONE carrier value — 48,020,342 mHz
  (+424 ppm, the unit's crystal signature), span 0 mHz,
  `trim_updates` 0, `clock_faults` 0, tracking unbroken.** Against
  the CM6206's disqualifying wander (ten values spanning 6.3 Hz, 53
  post-settle trims in 600 s under the same active-playback
  condition), the designated path resolves: the U24 XL IS the gate-1
  source, and the qualified clean source for gate 7 and encoded AC-3
  at 44.1/48 kHz
  ([`aius-sample-rate-following.md`](aius-sample-rate-following.md)
  §Acceptance). Raw runs:
  `.scratch/f100000d-6907-42c1-8ece-ea0c38d7a786/u24xl-loop-*`
  (7-day GC; the numbers above are the complete record).
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
