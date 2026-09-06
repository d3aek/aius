---
title: Aius output-jitter Phase G handover — same-carrier latency reset
type: plans
scope: component
component: aius
status: in-progress
subtype: how-to
tags: [aius, firmware, audio, latency, output-jitter, handover]
updated: 2026-09-06
---

# Aius output-jitter Phase G handover — same-carrier latency reset

**This is a transient handover baton, not a permanent record.** Retire it per the
handover skill step 8 once its Next action and Pending decisions are resolved:
promote the durable capture analysis below into
[`aius-output-jitter.md`](aius-output-jitter.md) Phase G (or
[`../reference/signal-path.md`](../reference/signal-path.md)) and delete this file
in the commit that lands the decision.

The task: the DSP editor page reported an average ~25-30 ms "estimated latency" over
a live AC-3 5.1 session that was itself glitch-free, ~2x the expected ~13 ms. The
question was whether that is a real firmware ratchet or inherent AC-3 behaviour, and
whether a same-carrier automatic reset can fix it.

## State — resume here

**Status:** BLOCKED on two owner decisions (see Pending decisions). Two firmware
designs were built this session and BOTH killed by adversarial review; the root
question was then settled by DEVICE MEASUREMENT (2026-09-06). The fix, if wanted, is
the EXISTING governor, not a new mechanism. No code from this session is worth keeping.

**What landed (pushed commits):** NONE except this handover doc. Nothing was committed
or flashed for the task itself; every firmware change is a discard candidate.

**Uncommitted tree state** — all owned by this session, all part of the two dead
designs, all DISCARD CANDIDATES (do not build on):

- Submodule `git_modules/aius`:
  - `firmware/lib/aius_audio/src/aius_tx_refloor.h` — NEW; the final design-B
    transition-detector rule. Dead.
  - `firmware/src/optical_main.cpp` — the refloor `#include`, constants, `g_tx_refloor*`
    globals, the `enqueue()` refloor tap, and the `refloor_trims`/`refloor_trimmed_frames`
    clock-JSON fields. ALSO carries a CORRECT, independent fix: three stale `~93.75/s`
    → `~375/s` audio-update-rate comment corrections (near `:1294`, `:1574`, `:5539`) —
    keep these even if the refloor is reverted.
  - `firmware/lib/aius_audio/src/aius_spdif_tx24.h` — `trim_to` third-caller comment
    (refloor). Revert with the refloor.
  - `firmware/test/tx-refloor/test_tx_refloor.c` — NEW; design-B tests. Dead.
  - `firmware/scripts/test.sh` — `tx-refloor` suite arm + `# Suites:` header entry.
  - `firmware/README.md` — `tx-refloor` suites-table row.
  - `firmware/test/optical-cutover/test_optical_cutover.py` — added `refloor_trims`/
    `refloor_trimmed_frames` to `CLOCK_JSON_KEYS` (order-sensitive assertion).
  - `docs/plans/aius-output-jitter.md` — Phase G section describing design B. WRONG per
    the analysis below; rewrite (promote the analysis) or revert.
- upstream repository `swagger.json` — `Clock` schema `refloor_trims`/
  `refloor_trimmed_frames` (required list + properties). Revert with the refloor.

The registry / optical-cutover / README / test.sh / swagger edits are all COUPLED to
the refloor; they were green as a set but the whole set is being discarded, so revert
them together.

**Next action:** get the two owner decisions below. Then EITHER — governor path —
`git checkout` the design-B paths, cherry-keep the three `375/s` comment fixes, set
`-DAIUS_TX_GOVERNOR_ENABLED=1` in `firmware/platformio.ini` for `[env:teensy41-optical]`,
build + `firmware/scripts/test.sh --environment native --suite tx-governor`, then OTA;
OR — status-quo path — `git checkout` every design-B path (keeping only the `375/s`
comment fixes) and close Phase G as "no automatic reset; ~16 ms AC-3 is inherent,
rare stall-ratchets cleared by a manual carrier-cycle/reboot."

**Decisions taken this session (with why):**
- Design A (underrun-gated per-block trim to 128) and design B (one-shot trim at a
  class/codec transition) are BOTH DEAD. A trims healthy AC-3 bursts and loops into
  sustained dropouts; B is a no-op at the empty transition ring AND fires a spurious
  trim on every AC-3 start, because AC-3 sets `codec_pc` one whole AU late
  (`firmware/lib/aius_pipeline/src/aius_pipeline.c:1042`, vs DTS/E-AC-3 at arm
  `:947`/`:950` — verified in source). Both confirmed by adversarial review.
- The correct mechanism is a WINDOW-MINIMUM floor estimator — which is the existing
  governor (`firmware/lib/aius_audio/src/aius_tx_governor.h`, host-tested, compiled out
  by `AIUS_TX_GOVERNOR_ENABLED=0`). Both custom designs were worse reinventions of it.

**Traps:**
- The instantaneous ring level is USELESS as a park detector on AC-3: healthy AC-3
  legitimately sweeps 128 → 1536 every 32 ms (measured). Only the 4 s window-MINIMUM
  distinguishes healthy (min ≤ 384) from a park (min sustained > 384). A and B both
  fell into this.
- Host tests CANNOT validate any of this — they feed hand-picked level sequences and
  never model the closed ring loop. Both dead designs passed their host suites.
- The `~13 ms healthy 5.1 residence` in `aius-output-jitter.md:15` is the RESIDENCE
  (median), NOT the floor. The floor (window-min) is 128. The adversarial review and
  this session both mis-read residence as floor and mis-targeted twice as a result.

## The capture analysis (the measured answer — save this)

Measured on the deployed board (`http://192.0.2.66`, `GET /api/status`, reads need no
auth) 2026-09-06 by polling `clock.tx_level_frames` at 4 Hz through a full
power-on → OSD navigation → AC-3 content sequence (including an operator
preview→OSD→content bounce). Raw trace archived in session scratch (7-day GC):
`.scratch/<session>/ac3-seq.tsv`, poll script `ac3-ring-poll.sh` alongside.

| Stream | tx_level floor (window-min) | tx_level peak / high-water | median (~DSP page) | underruns |
|---|---|---|---|---|
| Idle (no carrier) | 0 | 0 | 0 | 0 |
| PCM menu (state 3) | 128 | 640 | ~8 ms (~384 fr) | 0 |
| AC-3 content (state 4) | 128 | 1536 (one AU) | ~16 ms (~768 fr) | 0 |

Key facts, all measured over a clean 6-minute run:

1. **The AC-3 ring floor is 128 frames (2.67 ms), the SAME as PCM.** The ring drains
   fully to 128 between access units (128 was hit every few seconds throughout). Not
   640, not 1200. This is the number both failed designs guessed wrong.
2. **AC-3's ring sweeps 128 → 1536 (one full 32 ms access unit) and back, roughly
   uniformly.** High-water NEVER exceeded 1536 (exactly one AU). So the DSP-page median
   of that sawtooth is ~768 frames ≈ **16 ms** — AC-3's INHERENT latency from its 32 ms
   AU granularity, unfixable without breaking the codec. AC-3 will always read ~2x PCM
   on the DSP page for this reason alone.
3. **The clean session NEVER ratcheted and logged ZERO underruns**, including the
   codec transitions of the preview→OSD→content bounce. So codec transitions themselves
   do NOT cause the park — which is exactly why design B (transition-triggered) was
   aimed at the wrong event.
4. **Therefore 25-30 ms = inherent ~16 ms + a stall-driven ratchet.** The glitch session
   had 768 underrun frames + unsupported episodes; those stalls shift the FLOOR up
   (128 → ~500-700), lifting the whole sawtooth to a 25-30 ms median. No stalls → no
   ratchet (this session).
5. **The governor is the correct and safe regulator.** On healthy AC-3 the window-min
   is ≤ 384 the whole time (values 128-384, governor deadband 256), so a governor
   targeted at 128 provably NEVER fires on healthy AC-3; it fires only when a stall
   holds the floor > 384 for a full 4 s window (a genuine ratchet), trimming it back to
   128 in one skip. It CANNOT reduce AC-3 below ~16 ms.

Conclusion: there is far less of a bug than first thought. Realistic outcomes are
(a) enable the governor to auto-clear stall-ratchets back to ~16 ms (one skip per
correction; a D9 reversal), or (b) accept ~16 ms AC-3 as inherent and clear the rare
ratchet manually (carrier-cycle / reboot).

### Findings and loose ends

- `firmware/src/optical_main.cpp:1353` — `g_tx_governor` is `-Wunused-variable`
  (governor compiled out since 2026-08-16). Pre-existing, benign; the aius cross-build
  tolerates warnings. Proposed: leave, or `[[maybe_unused]]` if the governor stays off.
  NOT AGREED.
- `firmware/sd/www/index.html:69` — the `optical-app` suite's `test_bitstream_ui.py`
  fails wanting the word "bitstream" ("IEC 61937 bitstream framing…"); the html reads
  "IEC 61937 framing…". Pre-existing, foreign to this work. Proposed: whoever owns that
  copy reconciles it. NOT AGREED.
- The three `~93.75/s` → `~375/s` audio-update-rate comment fixes in `optical_main.cpp`
  are correct and independent of the refloor. Proposed: keep them out of the discard.
  NOT AGREED.
- Durable: promote the capture analysis above into `aius-output-jitter.md` Phase G
  and/or `docs/reference/signal-path.md` — the AC-3 ring residence's floor-vs-median
  distinction (floor 128, median ~16 ms from the 1536-frame AU) is currently
  undocumented and is what caused two failed designs. Proposed: promote on decision.
  NOT AGREED.

### Pending decisions

**1. Enable the transmit governor?** (`-DAIUS_TX_GOVERNOR_ENABLED=1` for
`[env:teensy41-optical]` in `firmware/platformio.ini`; rule in
`firmware/lib/aius_audio/src/aius_tx_governor.h`, host suite `tx-governor` green.)
- Reverses design decision D9 (`aius-outstanding-work-2026-09.md`; `aius-output-jitter.md`
  Phase E), which retired the governor for "systematic latency with repeated triggers."
- The MEASURED floor (128) shows D9's objection rested on a floor-vs-residence
  confusion: the window-min is 128 on healthy AC-3, so the governor provably never
  trims healthy audio — only a genuine sustained ratchet, one skip per correction.
- Consequence YES: stall-ratchets (like the 25-30 ms session) auto-clear back to
  ~16 ms; cost is one audible skip per correction, and on a stall-PRONE (bad-reception)
  session that becomes repeated skips — D9's original concern, still real there.
- Consequence NO: 25-30 ms stall-ratchets persist until a manual carrier-cycle/reboot;
  audio stays glitch-free (the deep ring is why).
- RECOMMENDATION: enable it — it is safe per the measurement and clears the exact
  symptom — but as a reversal of the owner's explicit ruling it needs the owner's word.
  It cannot get AC-3 below ~16 ms regardless.
- BLOCKS: any firmware change / OTA for this task.

**2. Discard the dead design-A/B code?** (the uncommitted tree above.)
- Confirmed broken; the governor is the right tool, so none of it ships.
- RECOMMENDATION: `git checkout` all the refloor paths across both repos, cherry-keeping
  only the three `375/s` comment fixes in `optical_main.cpp`. A `git checkout` of owned
  paths needs the owner's OK per repo rules — hence this decision.
- BLOCKS: a clean tree; the stop-guard keeps flagging `swagger.json` until
  this is resolved.
