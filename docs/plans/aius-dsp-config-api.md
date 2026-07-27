---
title: Aius DSP pipeline, SD editor, and configuration API
type: plans
scope: component
component: aius
status: in-progress
tags: [firmware, dsp, audio, http-api, sd]
updated: 2026-07-27
toc: true
---

# Aius DSP pipeline, SD editor, and configuration API

## Shared delivery sequence — 2026-07-24

**This section is identical in [`aius-network-hardening.md`](aius-network-hardening.md)
and [`aius-dsp-config-api.md`](aius-dsp-config-api.md). Change both together.** It
supersedes the separate execution handoffs both documents previously carried. Both
plans are active, they share one critical path, and neither finishes without the
other.

### Deciding principle

**Audio hot-path reliability decides every trade-off. Everything else is lower
priority.** Recorded 2026-07-25 as the standing tie-breaker for the rest of this
work, and it is not a tie-breaker of last resort — it is the first question to ask
of any choice.

Concretely, when a decision is contested:

- A feature that cannot be made safe for the audio path is dropped or deferred,
  not accommodated. Refusing a route is an acceptable outcome; risking a block
  callback is not. The `POST /api/replays/start` refusal from S2a is this rule
  applied.
- ITCM headroom belongs to code that genuinely runs per audio block. Anything
  else is `FLASHMEM` on each definition, and a slice that cannot hold to that is
  re-scoped rather than granted an exception.
- A bounded primitive beats a smaller diff. Where those conflict — as they did
  when growing the SD transfer slice would have made every primitive a 12 KB
  blocking call — the bound wins.
- Memory that is genuinely free is not an argument. OCRAM is plentiful and DTCM's
  free-for-locals zone is not; a change that spends the scarce bank to save the
  plentiful one is backwards.
- When a diagnostic or convenience field cannot be produced without reaching into
  a path that touches the audio timing budget, serve it as absent and say so.

### Resume here

**Phase 14 is delivered and the board is running it.** All fifteen slices landed
(S0, P1a, P1b, R1, R2, P2, S1, C1, S2a, S2b, S3a, S3b, S4, S5, S6, S7), plus 14b
(meters) and 14c (replay refill onto a bounded job). Four of those slices were not
in the original breakdown and were forced by what the work found: **C1** when S2a
linked the portable engines and RAM1 went 8,612 bytes over, **S3a/S3b** when S3
proved too large for one reviewable unit, and **S6/S7** when S5 found an
acceptance criterion unmet and then that nothing proved the device met it.

#### The board, and how to reach it

Serving at **192.0.2.66**. USB needs a group wrapper because the login is stale:
`sg dialout -c "<command>"` for the CDC port, `sg plugdev -c "<command>"` for the
Teensy loader — they are different groups and both are needed.

Flashing is now reliable: `AIUS BOOTLOADER <id>` over CDC enters HalfKay on the
first attempt (five consecutive trials with a reflash between each), against 48
failed attempts before that fix. The SD maintenance token is read with
`AIUS AUTH <id>`; it is stable for the life of the board and
[`docs/reference/usb-control-protocol.md`](../../docs/reference/usb-control-protocol.md)
explains why and where to keep it. The card's web tree is provisioned and correct.

**Confirmed on hardware:** boot, one epoch per `loop()`, the watchdog not
resetting in normal operation, SD probe and both config loads, the status surface
on all three channels, `POST /api/reboot`, software bootloader entry, the SD
directory index, and DHCP reaching ONLINE on generation 1 with no fault after the
readiness gate landed.

**Not yet confirmed on hardware:** anything in the audio path. There is no S/PDIF
carrier — the optical input is unsoldered — so `blocks` is 0, the DSP has never
run, and the meters endpoint has never published. The per-block cost in 14b is
still a static estimate.

**The next flash is the interesting one**, and carries three unvalidated things
at once: the SD failure diagnostics (`0bcc32eec`), and the clock handover
(`24ff70f7b`) which should make replay actually produce audio. What to look for,
with the input still unsoldered: start a replay and **`blocks` on
`GET /api/status` should climb continuously** while `overruns` stays at or near 0
— both were frozen at 0, which is how the defect was found. Stop it and `blocks`
should freeze with the output going digitally silent and `stop_reason:"request"`.

That run also finally yields the **measured per-block DSP cost** from the DWT
counters on `GET /api/meters`, which is the last estimate standing in this plan
and a prerequisite for Phase 14e.

Once the input is soldered: `blocks` must climb from passthrough alone with no
replay, and **starting a replay against a live carrier must be refused with
`409 replay_carrier_present`** — the refusal landed, so the start no longer
reaches `stopped`. `stop_reason:"carrier"` and its type-3 event are now reachable
only when the carrier arrives *during* an already-running replay, which is the
one case the refusal cannot cover.

#### What is in flight

Two slices were running when this session ended. **Check their state before
starting anything**: `git status`, and re-run the standing sweep.

Both landed before the session closed. The memory gate now agrees with the linker
symbols and reports `omits: NonCacheable 9756B` explicitly, and a `memory` suite is
registered and green.

#### The 409 refusal — shipped

`AIUS_OPTICAL_APP_REPLAY_CARRIER_PRESENT` runs from the enum through the
dispatcher arm, the replay-start error table and the `app_replay_start()` guard,
so a start against a live carrier now returns `409 replay_carrier_present`
instead of being admitted and reversed at the next checkpoint. It is a distinct
token from the generic `409 replay_start` on purpose: a client must be able to
tell "that id is unusable", which it retries with a different id, from "the input
is live", which clears only when the carrier goes away. A half-refusal reusing
the existing token was deliberately not shipped.

The guard sits ahead of the `g_sd_ready` and id-validity check, so a live carrier
refuses regardless of SD state and the client gets the accurate reason rather
than `replay_start` from an unrelated precondition.

**The guard itself has no native coverage and cannot have any.** `optical_main.cpp`
is target-only and `optical-cutover` is a structural suite; the dispatcher half is
covered by `optical-app`, but `AudioInputSPDIF3::pllLocked()` is proven only by
the target build and the on-device pass with the input soldered.

**`stop_reason` on `GET /api/replays/status`.** A string, always present, served
alongside `state` rather than folded into it (`optical_main.cpp:1585`): `state`
keeps the replay service's own vocabulary and `stop_reason` says *who* ended the
selection. It is cleared to `none` by every accepted start. Three values:

- **`none`** — nothing has stopped a replay since the last start, or a selection
  is live. `eof`, `underrun`, and `error` states also report `none`: those are
  the replay ending on its own terms, not something displacing it.
- **`request`** — an operator stopped it, through `POST /api/replays/stop`.
- **`carrier`** — the optical input came up and took the audio path, so the
  replay was displaced (`replay_clock_service`); the same transition emits a
  type-3 event. Reachable only for a replay already running when the carrier
  arrives, since a start against a live carrier is refused with
  `409 replay_carrier_present`.

#### Open defects, in the order they are worth taking

1. ~~**`mem_report.py` under-reports DTCM by ~10.8 KB.**~~ **Fixed.** The gate now
   reads the linked ELF's section table and linker symbols rather than regexing
   `teensy_size:`, so an orphan-placed section cannot be invisible to it. Measured
   DTCM free-for-locals on `teensy41-optical` is **9,280 bytes**, not 19,036; the
   difference is `NonCacheable`, now named explicitly. A `memory` suite is
   registered and green. Full account in
   [`aius-network-hardening.md`](aius-network-hardening.md#the-memory-gate-under-reports-dtcm-by-108-kb).
2. **`500 sd_failure` on large SD uploads**, roughly 1 in 15, succeeds on retry.
   Diagnostics landed (`0bcc32eec`) and are **not yet flashed**. A structural hole
   was found and is real — the root driver's yield cannot see an HTTP transaction
   admitted but whose `SD_START` has not yet been emitted, a one-epoch window — but
   the arithmetic gives ~1e-4 against an observed ~1-in-15, three orders of
   magnitude short. The one-line fix is recorded in §Phase 14f and deliberately
   **not applied**, because it would look like a resolution while possibly leaving
   the real defect. Flash, reproduce, then read `sd_diag.last_failure`; the
   convicting values for each hypothesis are tabulated there.
3. ~~**`"source":"default"` is a hardcoded literal** in the status JSON format
   string.~~ **Fixed.** It now serves `g_dsp_source_sd ? "sd" : "default"` — the
   same fact, from the same variable, as `GET /api/dsp/config`, so the two can no
   longer disagree. Note the field is the **effective DSP model source**, not the
   audio source: `optical_main.cpp:246`, the DSP GET serializer and the `Status`
   schema in `swagger.json` all pin it to `default | sd`, and the
   audio source is `state`. This entry previously read as though it should track
   the audio source; that reading contradicts the wire contract and was not
   implemented. `g_dsp_source_sd` is a latch — set on a successful SD override
   load and never cleared — so status inherits exactly the DSP GET's behaviour,
   latch included.
4. **`record_event` has only two call sites** — the S/PDIF lock transition and the
   test-tone toggle. Everything else observable is a counter, not an event. Closing
   this is the prerequisite to any push observability being worth having; see the
   syslog assessment.
5. ~~**The suite registry is hand-maintained with no guard.**~~ **Fixed.** The
   `registry` suite parses `scripts/test.sh` and fails on a suite directory with
   no arm, an arm naming a missing path, an entry point a registered arm never
   runs, or either catalogue disagreeing with the arms. No remaining orphans; the
   true count is **36 arms over 30 suite directories**. Detail in
   [`aius-network-hardening.md`](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking).
6. **"At most one SD primitive per epoch" is false at checkpoint 5.**
   `http_replay_list_json` walks `/aius/replays` with `for (;;) openNextFile()`,
   uncapped and ungated, and `app_replay_delete` spends `SD.exists()` +
   `SD.remove()` ungated. The most plausible unexplained 8 s reset in the image,
   and a live candidate for defect 2. Full detail in
   [`aius-network-hardening.md`](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking).
7. ~~**The FNET poll timer shares the audio ISR's interrupt priority** (both
   128).~~ **FIXED 2026-07-26, and "shares" was the wrong word — it outranked
   it.** The DSP runs in the audio *software* interrupt at 208, not the DMA ISR
   at 128, so `fnet_poll` **preempted the block callback** a thousand times a
   second rather than merely failing to yield to it. `stack_start()` now demotes
   `IRQ_PIT` to 224 on every network generation (once at boot is undone by the
   first restart). Detail, including why 209 would have been a no-op, in
   [`aius-network-hardening.md`](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking).
8. **Three JSON cursors, one seam, not hoisted.** `aius_config.c` and
   `aius_dsp_config.c` duplicate the same recursive-descent primitives, and the
   replay body reader now carries a third. Three consumers clears the seam-arity
   rule, so this should become `lib/aius_json`; it was deliberately deferred
   because it rewrites two libraries that do not have the defect. Detail in
   [`aius-network-hardening.md`](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking).
9. **Five source comments still quote the pre-fix DTCM free figure** (~22–24 kB
   against a measured 9,280). The README was corrected; the source was not.

#### Decisions taken, so they are not re-opened

- **A network syslog sender was assessed and declined.** Not on cost — the event
  ring has two call sites, so it would carry a lock change and nothing else. An
  absent collector is undetectable in both directions, and the board cannot claim
  a timestamp. Polling with the existing runner closes most of the gap, and the
  retained boot ledger already covers the pre-reset case better than log lines
  would. If ever built, it belongs after B6. Full reasoning in §Network syslog.
- **The link gate is kept though it is inert here**, because it is correct for a
  genuinely dead link and bounds that at 3 s.
- **The replay refill port stayed out of Phase 14** and landed as 14c.

#### Next

**14f, the first flash and assumption validation**, remains the gate before 14d
and 14e — both are required to carry measured figures, and the memory gate above
must be fixed first or they will document the wrong numbers. Then B6, the live
recovery smoke, which has never run and is what proves the watchdog and recovery
contract this whole plan exists to establish.

### Why the two strands are coupled

A firmware flash attempt during the DSP `PUT /api/dsp/config` work found the
deployed board crash-looping through DHCP on the legacy pre-cutover
`optical_main.cpp` path — the exact unbounded-recovery failure the network plan
exists to close. Only Phase 14 fixes it.

In the same window the DSP strand built its config PUT, its `/aius/dsp.json`
override load, and its `dsp_upload` job bookkeeping directly on top of the legacy
ad-hoc SD job machine and the legacy blocking HTTP path — all of which Phase 14
deletes. So the DSP strand cannot ship without the cutover, and the cutover now
carries DSP behaviour it did not originally own. Everything else in the DSP strand
(the SD-hosted control surface) is genuinely independent and runs in parallel.

### Corrections to earlier claims in both plans

- **`runtime-target` was wrongly listed as a delivered regression suite.** It did
  not exist when this section was written; Phase 12 has since created it, along
  with `optical-app` from Phase 11. The standing guard is now `net`, `boot`,
  `network-transport`, `sd`, `sd-target`, `http`, `usb-control`, `runtime`,
  `runtime-target`, `optical-app`, `boot-target`, and `fault-target`.
  `optical-cutover`, `flash`, and `recovery-runner` still do not exist and are
  created by Phases 14, 15, and 16.
- **`PUT /api/dsp/config` was not described in OpenAPI.** The write phase shipped
  the firmware without reaching `swagger.json`, so the DSP plan's
  OpenAPI gate was quietly unmet. Closed at step B5. `GET /api/meters` is still
  absent and lands with 14b.
- **Phase 10a's stated justification was wrong in one detail.** The DSP config
  `PUT` does not need the SD publish mechanism: it parses and validates the model
  from the HTTP body itself and writes the accepted bytes verbatim. The real
  second publish caller is the **boot / hot-reinsert load** of `/aius/dsp.json`,
  once that moves onto a portable `CONFIG_LOAD` job in Phase 14. Phase 10a is
  still required — for that path. It is delivered (`df9ea4483`).

### Structural obstacles the cutover must clear

Recorded here because they are cheap to miss and expensive to hit mid-phase:

- **The legacy SD symbols collide with the portable header in two places.**
  `optical_main.cpp:118` declares its own `struct AiusSdJob`, unrelated to the
  portable one in `aius_sd.h`, and `optical_main.cpp:115` declares an
  `AIUS_SD_JOB_DELETE` enumerator that redefines `aius_sd.h:20`. It compiles
  today only because the file never includes that header — but
  `aius_optical_app.h:21` includes it unconditionally, so merely naming the
  callback table pulls it in. Both legacy names must be renamed before any
  cutover work, which is what slice S0 is for.
- **The composed storage service cannot start a job on its own.** It accepts
  `AIUS_HTTP_ACTION_SD_START` and nothing else (`aius_runtime_storage.cpp:250`),
  and it acknowledges `AIUS_SD_ACTION_PUBLISH_CONFIG` into a saturating
  `publish_dropped` counter rather than delivering the payload
  (`aius_runtime_storage.cpp:134`). Every boot-time SD load in Phase 14 needs
  both gaps closed first.
- **`AiusRecoveryStatus` carries no IP, link state or DHCP counters**
  (`aius_net.h:146`), while `optical_main.cpp` reaches `Ethernet.localIP()` /
  `linkStatus()` in fourteen places including the status JSON, the UDP status
  payload and USB `ID`. Dropping the vendor include without first adding an
  accessor to `aius_network_teensy` leaves the board undiscoverable headless.
- **Any SD job remounts the card.** `aius_sd_job_start` enters
  `AIUS_SD_PHASE_BEGIN` for every job kind (`aius_sd.c:407`) and the adapter's
  `AIUS_SD_ACTION_BEGIN` calls `SD.begin(BUILTIN_SDCARD)`
  (`aius_sd_teensy.cpp:137`), guarded only by its own `file_open` flag. It cannot
  see a foreign handle such as `g_replay_file` (`optical_main.cpp:68`), so a
  config `PUT` or a probe re-mounts the volume beneath an open replay file.
- **`stage_dsp_sd_override()` is a blocking synchronous read.**
  `optical_main.cpp:279` reads up to 12,288 bytes one byte at a time from
  `setup()` and from `service_sd()`, and puts a 12,288-byte array on the stack
  while the RAM1 free-for-locals zone is 22,684 B. It violates the normative
  one-SD-primitive-per-epoch rule head-on. Porting it is part of Phase 14, not
  optional cleanup.
- **The legacy watchdog interval is 16 s, not the pinned 8 s.**
  `watchdog_init()` writes `WDOG_WCR_WT(31)`, and the feed has three call sites —
  `setup:2593`, `loop:2607` and `loop:2618`. The normative contract is one 8 s
  interval with exactly one feed after a complete epoch.
- **ITCM headroom is 2,120 bytes** (98.92 % used) as of slice S1. Every new
  per-epoch service, dispatcher, and adapter must be `FLASHMEM` unless it
  genuinely runs per audio block. Apply the attribute on each definition — GCC
  does not inherit a `section` attribute from a forward declaration, so a
  prototype-only `FLASHMEM` silently leaves the code in ITCM. Phases 11 and 12
  both held to zero ITCM this way.

  The figure was 1,416 bytes until S1, which **gained** 704 bytes of headroom
  rather than spending any. Referencing the previously single-use static
  serializers from the new callbacks changed GCC's inlining decisions, pushing
  code that had been inlined into ITCM callers out into the FLASHMEM adapters.
  Behaviour is unchanged; codegen is not. Expect the same effect to run in
  reverse at S2b, when the legacy callers those serializers shared are deleted.
- **The portable engines land in ITCM, not FLASH, and this broke the build at
  S2a.** On Teensy 4.1 a function with no `FLASHMEM` attribute goes to ITCM, and
  ITCM shares FlexRAM with DTCM — so engine code competes directly with the audio
  path's own budget. Phases 11 and 12 held their *services* to zero ITCM, but
  nobody had annotated the engines underneath them, which were unreferenced and
  therefore invisible until S2a linked them. Measured at that point:
  `aius_http_transaction.c` 8,688 B, `aius_sd.c` 8,292 B, `aius_sd_teensy.cpp`
  7,156 B, `aius_http.c` 3,464 B, `aius_network_teensy.cpp` 3,284 B,
  `aius_boot_teensy.cpp` + `aius_boot.c` 1,936 B, `aius_net.c` +
  `aius_runtime.c` 1,272 B — against 2,120 B of headroom. RAM1 overflowed by
  8,612 bytes and the build went red.

  **The plan predicted this cost would land in FLASH. It does not.** Slice C1
  moves the wholly-cold units to flash and adds a post-link guard so a new
  unannotated engine fails the build instead of silently eating the audio path's
  headroom. `aius_dsp`, `aius_pipeline`, `aius_iec61937` and `aius_audio` stay in
  ITCM because they genuinely run per audio block.
- **The bank report does not yet include Phases 11 or 12.** Nothing references
  the dispatcher or the composed runtime, so `--gc-sections` drops every section
  and `nm` on `firmware.elf` finds none of their symbols. Phase 14 is where
  roughly 9.4 KB of FLASH and ~907 bytes of DTCM string data become real, and
  where the ~35 KB runtime instance must be placed in `DMAMEM` rather than the
  22,684-byte DTCM free-for-locals zone.

### Critical path

Two tracks. Track A has no firmware-source overlap and runs concurrently
throughout. Track B is serialized because every phase in it touches
`optical_main.cpp` or the files that replace it.

**Track A — SD control surface** (independent, no dependencies)

| Step | Work | Gate |
|---|---|---|
| A1 | **Done** — `firmware/sd/www/index.html` + assets: every config control mapped to the documented JSON model, meters and canvas waveform from `GET /api/meters`, matching the existing `replays.*` precedent | `test/sd-editor/`, 36 tests green |
| A2 | Target asset smoke — the page loads and drives the real device | folded into the single on-device session (B6) |

**Track B — bounded runtime cutover** (strictly ordered)

| Step | Work | Gate |
|---|---|---|
| B0 | **Done** (`df9ea4483`) — Phase 10a, SD job publish-capacity generalization | `sd` + `http` suites, target build |
| B1 | **Done** — Phase 11, optical application dispatch **including the DSP routes** | `optical-app` suite green |
| B2 | **Done** — Phase 12, Teensy runtime service composition | `runtime-target` suite green |
| B3 | Phase 14 — optical runtime cutover **plus the DSP SD-override load port**, eleven slices after the 2026-07-25 re-scope | `optical-cutover` + all component suites |
| B4 | Phase 14b — meters endpoint and accumulate wiring | `optical-app` + ITCM budget |
| B5 | OpenAPI catch-up — `PUT /api/dsp/config` **done**, `GET /api/meters` with 14b | Vitest OpenAPI contract suite |
| B5a | Phase 14c — replay refill onto a bounded SD job, deferred out of Phase 14 | `replay` + `sd` + `runtime-target` |
| B5b | **Phase 14f — first flash and assumption validation.** Nothing in Phase 14 has ever executed; this is where the plan's assertions become measurements | the recorded validation log, one verdict per assumption |
| B5c | Phase 14d — the firmware I/O reference: every path, its memory, its rationale, and why it cannot disturb the audio hot path | doc-index check + a real build's bank report |
| B5d | Phase 14e — the signal path and DSP reference: block diagram, per-stage implementation, memory, and the measured per-block budget | `dsp` suite + figures taken from the device |
| B6 | Phase 16a — live recovery smoke on the deployed board, entered by **one accepted manual Program-button flash** | the smoke's own pass/fail per gate |
| B7 | Phase 15 — button-free flash tooling | `flash` + `usb-control` suites |
| B8 | Phase 16b — the zero-button-press flash gate, re-run once B7 exists | one clean button-free flash and restore |
| B9 | Retire the the app dashboard, keeping `swagger.json` and its contract suite | `ui-contract` and Vitest suites green after removal |

Phase 13's code baseline is delivered and unchanged. Phases 1–10 and 10a are
delivered and are not re-reviewed. Each remaining phase's gate is its green
native suite plus an owned-diff review — nothing heavier. The per-phase
source-blind oracle chains and clean-room delivery audits described in the
delivered-phase blocks are historical and are not re-run.

### Scope moved by this reconciliation

- **Phase 11 owns the DSP routes.** Its route inventory is every current HTTP
  route *plus* `GET /api/dsp/config`, `PUT /api/dsp/config`, and `GET /api/meters`.
  Writing the dispatcher without them means writing it twice.
- **Phase 14 absorbs the DSP SD-override load.** `stage_dsp_sd_override()`'s
  blocking read is replaced by a portable `CONFIG_LOAD` job with
  `publish_capacity = AIUS_DSP_MODEL_MAX_JSON_BYTES`, and the staged model still
  applies at the audio block boundary. This is what Phase 10a exists for. The
  `dsp_upload` write path moves onto the portable `UPLOAD/REPLACE` job with
  `publish = false` — the model is already parsed and validated in RAM, so
  re-publishing the written bytes would buy nothing.
- **The meters phase becomes Phase 14b**, owned by the network plan's phase
  sequence rather than standing alone in the DSP plan, because it wires into the
  post-cutover runtime and audio path. Its contract stays the DSP plan's.
- **The two live-hardware gates merge into one session.** The DSP plan's target
  validation (default audio unchanged, config round-trip, meters render, editor
  loads, short soak) and the network plan's recovery smoke run against one flash,
  as B6.
- **Phase 15 moves after the first live validation.** One manual button press is
  accepted to get the post-cutover image onto a board that currently cannot be
  reached any other way; the button-free tooling then removes the need for the
  next one.
- **The the app dashboard is out of scope until B9.** It is superseded by the
  device's own SD-hosted control surface and is queued for deletion as the last
  step, not repaired along the way. Two consequences to expect rather than
  investigate: the `control-shell.json` browser scenario **fails today** and stays
  failing until B9, because it asserts `/api/mode` and `/api/mix` are present in
  `swagger.json` while the OpenAPI suite asserts they are absent; and the
  dashboard's mode/mix buttons already 404 and will simply keep doing so. Do not
  spend time on either. B9 removes the dashboard, that scenario, and the
  `ui-contract-allowlist.json` entry for its stylesheet together, and coordinates
  with the ui-kit accessibility rollout that currently enrols the app. Nothing
  earlier in the sequence depends on it, including Phase 14's removal of the
  firmware-side `MODE`/`MIX` route enumerators.

## Audio-quality roadmap — accepted 2026-07-26

Everything above concerns the DSP's *plumbing*: the config surface, persistence,
meters and wire shapes. This section is about whether it **sounds** good, which
is a separate axis and was never assessed until an audit on 2026-07-26 read the
chain end to end.

**[`docs/reference/signal-path.md`](../reference/signal-path.md) (Phase 14e) is
the living record of the signal chain, and every item below updates it in the
commit that ships the change.** A quality roadmap whose document lags the code
is worse than none, because the next reader cannot tell which stage they are
listening to. Treat that document the way §Commit cadence treats a plan: it is
part of the work, not a follow-up.

### Three defects found and accepted for fix

Being implemented now; each is a defect rather than a preference.

1. **The 16-bit output truncated with no dither.** `optical_main.cpp` reduced the
   24-bit result with an arithmetic `>> 8`, which rounds toward −∞ (a −0.5 LSB DC
   offset and asymmetric distortion) and adds no dither, so quantisation error was
   *correlated with the signal* rather than benign noise. Audible as grunge on
   fades — precisely where a decoder is judged. Note the irony this closes: the
   DSP is double-precision, no-FMA, bit-exact host-to-target, and then discarded
   that care in one shift.
2. **The default downmix clipped.** Center and surround both at 1/√2 with
   `AIUS_DSP_HEADROOM_DEFAULT = 1.0` sums to +7.6 dB worst case, and
   `aius_downmix_clipfree_headroom()` — which returns the correct 0.414 — existed
   and was not the default. Not hypothetical: `dsp-vectors` reported
   `surround51_impulse … out_peak=8388607/8388607`, exactly `AIUS_DSP_SAMPLE_MAX`,
   so the **committed corpus was clipping**. The fix is not simply the clip-free
   value: −7.6 dB assumes six correlated full-scale channels, a case real
   programme material never presents, and paying it makes the device audibly quiet.
   The choice is being made against the measured corpus peak distribution.
3. **The "true-peak safety limiter" was neither.** It read sample magnitudes with
   no oversampling, so inter-sample peaks passed through to clip in the DAC's
   reconstruction filter downstream of anything observable; and with
   `lookahead_ms` defaulting to 0 against a 1 ms attack, gain reduction *lagged
   the peak it existed to catch*.

**Measured on the board after the three fixes landed** (`f789aa449c6e`, 24 s of
replay, 93.3–94.2 blocks/s, **zero overruns and zero underruns**):

| | before | after |
|---|---|---|
| `dsp_cycles_last` | 398,591 | **607,086** |
| `dsp_cycles_max` | 431,995 | **638,204** |
| share of the 6,400,000-cycle block | 6.2–6.8 % | **9.5–10.0 %** |
| DTCM free for locals | 9,248 B | **17,152 B** |

So the three fixes cost about **3.3 points of block budget** and *returned* 7,904
bytes of the scarcest bank — the playback ring became `int16_t`, which it always
should have been, freeing 8,192 against 256 for the true-peak FIR history. **The
constraint that shaped most placement decisions in this plan has nearly doubled**,
which is worth knowing before the next item spends it.

The measurement landed above the predicted quiet-path figure (6.6–7.1 %) and
below the predicted worst case (10.0–13.0 %), which is consistent with cinematic
programme material opening the true-peak gate often rather than rarely. Note the
prediction was made before `-ffp-contract=off` landed, and removing fused
multiply-adds costs cycles as well as fixing determinism.

**Item 1 delivered and measured on the board** (`f76baa87c67d`, 20 s of replay,
93.3–94.5 blocks/s, zero overruns and zero underruns):

| | 16-bit path | 24-bit path | predicted |
|---|---|---|---|
| `dsp_cycles_last` | 607,086 | **534,411** | ~560,000 |
| `dsp_cycles_max` | 638,204 | **565,024** | ~590,000 |
| share of block | 9.5–10.0 % | **8.4–8.8 %** | 8.5–9.5 % |
| DTCM free for locals | 17,152 | **25,376** | — |

**Better than predicted on both axes.** Removing 1,024 `aius_out16_sample()` calls
per block — each ~45–60 cycles of xoshiro, two `vcmpe`/`vmrs` pairs and a
`vrintm` — cost more than the estimate credited, and the ring's move to `DMAMEM`
*returned* 8,224 bytes of DTCM rather than spending it. The audio path is now
lossless from `aius_dsp_process` to `SPDIF_STL`.

**One layout consequence worth knowing before the next `DMAMEM` change.** Adding
the 16,396-byte ring at `0x20200000` pushed `g_aius_boot_slots` from `0x20201120`
to `0x20205140`, so the retained ledger at the old address no longer validated and
`boot_generation` restarted at 1. The post-link guard still passes and the ledger
degrades safely by design — an invalid CRC creates a fresh normal ledger — and the
changed `build_id` would have cleared the same state anyway. But it means **the
retained ledger's address is layout-dependent**, which matters for
[`aius-ota-upgrade.md`](aius-ota-upgrade.md): two A/B images with different
`DMAMEM` layouts cannot share a ledger across a switch, so an A/B design must
either pin the slots' address or carry image health somewhere layout-independent.

### Accepted order for the next level

Accepted 2026-07-26 in this order, with the reasoning that chose it.

1. **A 24-bit output path.** `audio_block_t` is `int16_t data[]`
   (`cores/teensy4/AudioStream.h:76`), so the engine is hard 16-bit and everything
   downstream of the DSP is capped at a ~−93 dBFS noise floor. S/PDIF carries
   24-bit and the IMXRT transmit registers are 24-bit, so **the hardware is not
   the limit — the block format is.** This firmware is unusually well placed to
   fix it: it already `lib_ignore`s the framework `Audio` library and builds its
   own S/PDIF subset, so the change is bypassing `audio_block_t` for the final
   hop rather than fighting a dependency. **First because it is the ceiling every
   other improvement runs into**, and it demotes dither from a workaround to a
   refinement.
2. **LFE fold-in and bass management.** `AIUS_DSP_LFE_GAIN_DEFAULT` is `0.0`.
   Discarding LFE is defensible per BS.775 when a subwoofer follows, but for
   stereo speakers it throws away the bottom octave of film content entirely.
   Fold LFE into L/R through a low-pass around 120 Hz with level compensation.
   Related and wholly absent: there is no crossover or bass management — the
   generic biquad EQ is not a Linkwitz-Riley crossover.
3. **A measurement loopback. — DELIVERED 2026-07-27**, all six slices; measured
   figures in [`signal-path.md` §Measured output quality](../reference/signal-path.md).
   The original rationale, kept: there was **no way to measure output
   quality on this device**: everything was proven by host known-answer vectors,
   and the meters report peak and RMS, not distortion. So every item here would
   be believed rather than verified. The replay path is already a signal
   generator; feeding the S/PDIF output back into the input would let the board
   measure its own THD+N, SNR and frequency response, with the DWT counters
   already giving per-block cost. Third rather than first only because items 1
   and 2 are the two most audible; it is what makes the rest checkable, and the
   1.4-semitone pitch error that survived on a board reporting every counter
   clean is the argument for it.
4. **Surround decorrelation in the downmix.** The coefficients are
   standard-correct, which is arithmetic rather than psychoacoustics. Summing
   FL+FC and FR+FC with correlated content comb-filters, and surrounds summed
   straight into L/R collapse envelopment. A short allpass or few-millisecond
   delay on Ls/Rs is what separates a downmix that sounds like a mix from one
   that sounds like an average — the largest perceptual gap between "correct"
   and "good" in this chain.

**Noise shaping is the hedge**, not a step: if the 24-bit path proves too
invasive, psychoacoustically weighted shaping on the 16-bit reduction buys
10–20 dB of *perceived* SNR for very little CPU and composes with the dither
already landing.

**Budget for all of it**, current as of item 1 landing and **not** the figures
this paragraph carried before — those were superseded by the two measurement
tables above and had gone stale inside their own document, which is exactly the
drift the §Audio-quality roadmap preamble warns about. Measured **534,411–565,024
cycles of 6,400,000** per 512-frame block at 48 kHz — **8.4–8.8 % used** — with
**~293 kB of OCRAM** free and DTCM free-for-locals at **25,376 bytes**. Anything
carrying real state still goes in `DMAMEM`; DTCM remains the scarcest bank even
after item 1 returned 8 kB of it.

**And there may be far more DTCM than that.** Phase 14e found that the 128 KB DTS
arena is reserved and never used, because `[env:teensy41-optical]` unflags the
`--wrap` set the shim depends on — see open defect 14 in
[`aius-network-hardening.md`](aius-network-hardening.md#open-defects-in-the-order-they-are-worth-taking).
Resolving that either way changes the memory story for every item below it, so
read the DTCM figure here as a floor rather than a fact.

**Sequencing note:** the true-peak limiter needs 4× oversampling, which builds
infrastructure a linear-phase or oversampled chain would reuse. And the 24-bit
output path overlaps the output stage with
[`aius-sample-rate-following.md`](aius-sample-rate-following.md), so that design
must account for it before being finalised.

### Item 3 design — the measurement loopback, software tap first

Drafted 2026-07-26 as the phase contract for the item 3 build. **The
adversarial prebuild gate ran the same night and rejected two load-bearing
choices** — the DTS stimulus and the "leakage bin" THD+N method — with the
quantified reasons folded in below. The same reviewer's findings-exhausted
pass confirmed the amendments and surfaced three note-severity residuals
(the `analysing` state missing from the machine, the dual-writer cancel
terminal contradicting the one-writer invariant, and the two generator
obligations), all landed in this revision. **The clean-room confirmation
(fresh reviewer, blind to the iteration) then verified internal coherence
and every load-bearing citation against the code and answered CONFIRMED —
build may start**; its one note, the unlisted ISR-owned `capturing → failed`
latch edge, is folded into the machine below. This is the contract the
slices build against.

**The physical TX→RX loop is not buildable today and is not what the roadmap
needs first.** The optical input is unsoldered, so no cable can close the loop.
What item 3 exists for — making the claims of items 1, 2 and 4 *measured*
rather than believed — is observable one step earlier: the transmit ISR's
`aius_spdif_tx24_fill()` is a verbatim copy of the ring's 24-bit words into
`SPDIF_STL`/`SPDIF_STR`, so the words the producer commits **are** the words
on the wire, and the tap sits after `aius_out_stage_run`, the last mutating
stage. The physical loop stays as the *transport* validation (bit
transparency, clock domains) when the input is soldered, and the tap seam is
written so the physical capture can replace it without touching the analysis.
An SPDIF-peripheral internal loopback (if the RT1062 even has one —
unverified) is rejected for the same reason: it would re-measure the same
words through an unproven peripheral mode. One property comes free and is
worth stating: stimulus and capture share the sample grid, so the analysis is
immune to physical clock error by construction — which also means it cannot
see defect 11's class; the clock/content *ratio* surface remains the physical
loop's job.

**Stimulus — raw PCM replay, not the DTS vectors. The gate measured the DTS
corpus and it cannot support the figures.** From the committed decode
reference (`stereo_tones.ref.s32`, which the differential test proves the
device reproduces): the FFmpeg `dca` encode has a **−78.7 dB THD+N floor of
its own**, decodes at −22.5 dBFS (`volume=0.6` through the decoder's 1/8), and
at 5,120 frames it **loops with a phase seam inside every 8,192-sample
window** (440 Hz = 46.93 cycles per loop; the 0.93-cycle jump costs roughly
−20 dB relative to the tone under Hann). Shipping against it would report
≈−50 dB THD+N on a path whose claims run −93 to −140 dB. Instead: **the
replay ring carries bare 16-bit IEC words, and a file with no IEC 61937
preamble in its first 512 words classifies `AIUS_PIPELINE_PCM` and passes
through bit-exactly at 24-bit scale** (`aius_pipeline.h:26-38`). PCM
measurement vectors are therefore new `.iec` replay-corpus entries (note:
replay plays `.iec` streams, not the `.dts` host-test files — the first draft
conflated the two corpora), generated loop- and window-coherent at any level
up to full scale. Two generator obligations, asserted at generation time
rather than assumed of tone data: the first 512 words must be **verified**
free of the IEC 61937 Pa/Pb preamble pair in either byte order (a tone
sample stream can produce it by coincidence), and coherence is per *vector*,
not per tone — 440 Hz alone is integer-periodic over multiples of 1,200
samples, but an L 440 / R 660 two-tone vector is loop-coherent only at
multiples of 2,400 (the lcm of 1,200 and 800). Resolvable floors, stated per figure: THD+N and SNR bottom out at the
16-bit stimulus floor (≈−93…−96 dB); harmonics-only THD via Goertzel stays
usable to ≈−95 dBc even on the DTS vectors once a seam-free vector exists
(their codec residual is spectrally flat, harmonic content −99…−133 dBc);
**the −140 dB inter-stage floor is not verifiable on-device by any 16-bit
transport and stays host-proven.** The DTS vectors remain as a secondary
whole-chain-as-played measurement, labelled as measuring the codec too.
Limiter distortion needs its own hot vector *plus a config that opens the
limiter* — at −22.5 dBFS it never engages; capture length and vector length
are sized together against LR4 + limiter-release settle (the 16,384-frame
capture leaves 171 ms of settle prefix before an 8,192 window, which is
thin).

**Tap.** Producer-side, in `AiusAudioProcessor::enqueue()` beside the tx24
staging: while a capture is armed, the same stereo 24-bit frames committed to
the ring are also copied into a `DMAMEM` capture buffer: 16,384 frames × 2 ch
× `int32` = **128 KiB of OCRAM** (~293 KiB free), 341 ms at 48 kHz, exactly
32 blocks so a capture always retires on a block boundary. Known consequence,
carried from item 1's delivery note rather than rediscovered at flash time:
a new 128 KiB `DMAMEM` object **moves `g_aius_boot_slots` again**, so the
retained ledger restarts `boot_generation` on the first flash — expected, not
a regression. Control state (armed flag, cursor, latches) lives in **DTCM,
not DMAMEM** — `.bss.dma` is NOLOAD and the boot ROM scribbles OCRAM at
reset, which is precisely the ring's own documented hazard; the 128 KiB
sample buffer itself needs no init for the ring's same reason. The capture
**latches the config generation and the overrun counter at arm and retires to
`failed` if either moved** — `enqueue()` applies a staged DSP model at any
block boundary and an overrun drops 512 frames outright, and either splice
inside a window is exactly the seam artifact the stimulus work just paid to
remove. Estimated ISR cost: 4 KiB of stores through the write-back D-cache
into OCRAM ≈ 4–8 k cycles, **0.06–0.13 %** of the block. Acceptance is a
distribution comparison with meters ON (the DWT bracket only runs when
`g_meters_active`), captures armed vs not, and the bound is that the armed
`dsp_cycles_max` stays within the existing ~30 k block-to-block spread —
a numeric gate, not "does it show".

**Capture state machine — one writer per transition, specified before build.**
The first draft's "epoch arms, ISR retires" hides a race: with no replay
running `enqueue()` never executes, so only the epoch side can time out an
armed capture, and an epoch-side cancel can collide with an ISR mid-block
retire. Contract: `idle → armed` (epoch, with all parameters stored before
the arm store, the ring's stage/commit rule); `armed → capturing → captured`
(ISR only — `captured` is buffer-complete, not figures-published);
`captured → analysing → done` (epoch, the sliced pass; `done` means the
figures are published); `armed/capturing → cancelling` (epoch sets a cancel
token only); `armed/capturing → failed` (ISR, when the arm-time
generation/overrun latch check fails at a block boundary — the retire the
tap paragraph specifies; from `armed` directly when the very first block
fails the check, which is exactly how a cold-boot arm retires when the
boot-time SD model applies at the first block — observed live at delivery);
`cancelling → failed` (ISR acknowledges the token at its next
block, or the epoch reclaims after a bounded number of epochs with `blocks`
unchanged — the one case where the ISR is provably not running); `done/failed
→ idle` (epoch). Analysis reads only `captured` buffers. One writer per
transition **except the dual-terminal cancel edge**, which is benign by
construction — both writers store the same terminal value, and the ISR
cannot act again once the epoch moves `failed → idle` — and the native suite
drives exactly that interleaving (ISR ack preempting between the epoch's
check and its store) along with every other edge. The delivery audit added
one more interleaving to this edge's ledger: the epoch's *cancel* is also a
check-then-store, so an ISR terminal (`captured`/`failed`) retiring between
the check and the store is clobbered by `cancelling` and that capture is
lost to a later reclaim. Bounded and accepted rather than mechanized: the
only cancel caller is the stall watch, which fires after 2 s of `blocks`
unchanged, so the window requires the replay to resume within microseconds
of the timeout and complete the buffer on that exact block; the outcome is a
lost capture the operator re-arms, never corruption, and closing it would
cost the machine its portable no-atomics simplicity.

**Pipeline reset at replay start — a prerequisite fix this phase absorbs.**
The pipeline classifies once per boot and nothing on the target ever calls
`aius_pipeline_reset()`: a DTS-first boot followed by a PCM replay hunts for
an IEC preamble that never comes (zero blocks, a capture armed forever), and
PCM-first followed by DTS plays framed bursts as noise. Replay start gets a
defined reset point, executed in audio context, before any mixed-corpus
measurement runs.

**Analysis.** Runs only over the frozen buffer, sliced in epoch context — a
declared **256 samples per epoch** through the accumulator set (~100–170 µs;
a full 8,192 pass completes in ~32 epochs ≈ 50 ms at the measured epoch
rate), hosted at the METRICS checkpoint, whose budget row gains
`service_budget_ms = 1` (the field's granularity makes 1 ms the smallest
expressible bound) and an overrun counter on the HTTP pattern — "no epoch
computes more than its budget" must bind to a declared number, and today both
candidate rows say 0. Methods, corrected by the gate: **THD+N and SNR come
from an IEEE 1057-shape 3-parameter sine fit** at the known f0 — sin/cos/DC
products accumulated across slices, fitted fundamental subtracted, residual
power = noise+distortion — because "total power minus the fundamental's
leakage bins" is FFT-grid vocabulary that Goertzel does not define, and Hann
leakage outside any usable notch is ≈−40 dB: the first draft would have
printed window leakage as THD+N. **SNR additionally excludes harmonics 2–10;
THD+N does not — the two are different numbers by definition.** Goertzel
keeps the jobs it is right for: harmonic amplitudes (THD) and multitone
frequency-response points, where ≥75-bin separations put Hann leakage below
−120 dB (440 Hz at N = 8,192 is 75.09 bins from DC, and the negative-
frequency image is below −130 dB). Any per-bin noise normalization states
Hann's ENBW (1.5 bins). Double precision throughout, host-to-target
agreement proven the way the DSP already proves it. Native known-answer
tests synthesize signals with known figures (a pure sine, a sine plus a
−60 dB third harmonic, a sine in −80 dB noise) and assert inside tolerance,
with a negative control observed — and the pure-sine case is the canary that
would have caught the leakage error, so its tolerance is never widened to
pass.

**Surface.** `POST /api/measure/start` (auth-gated — it arms a capture and is
mutating; joins `route_is_control()` or carries an in-arm gate like
`dispatch_replays`) and `GET /api/measure` (public read: the state machine
above plus the figures, each with its resolvable floor, and the vector id
they were measured against). Swagger and the contract suite move in the same
commit, per the roadmap preamble. [`signal-path.md`](../reference/signal-path.md)
gains the measured figures in the commit that ships the measurement — that
document is the reason this item exists.

**Settled by the gate:** the analysis lives beside `lib/aius_dsp` (seam
arity: one consumer today; the sample-rate-following monitor is hypothetical
until it exists); METRICS hosts the slicing; tap placement, sample-domain
clock immunity, 440 Hz resolution at N = 8,192, and the surface's auth
pattern all hold as drafted.

**Slice 1 delivered (2026-07-27).** The portable core is
`lib/aius_dsp/src/aius_measure.c` behind `aius_measure.h` in the same library
(the gate's seam-arity call: beside the DSP, not a new lib), proven by the new
`measure` suite: every capture state-machine edge including both dual-terminal
cancel interleavings, the pure-sine canary (−138.6 dB THD+N against the ~−40 dB
a leakage-bin estimator reads), the −60 dB third-harmonic THD+N/SNR split,
−80 dB SNR recovery, multitone probes, the Nyquist harmonic drop, and
bit-identical slicing invariance. Two findings the build added to the contract:

- **The residual identity needs compensated summation.** `rss = Σx² − coef·rhs`
  subtracts ~2^56 sums to expose a residual fourteen orders down; naively
  accumulated rounding lands at the 24-bit floor and the canary read it as
  signal. The data sums carry Neumaier compensation, and the Goertzel
  finalization is the Re/Im form (the phase-free magnitude formula squares
  away an empty bin before it cancels). Both are pinned by the canary's
  never-widened −130 dB bound.
- **SNR's harmonic exclusion is only as deep as the harmonic estimate.** The
  subtraction inherits the Hann-Goertzel estimate's leakage-limited accuracy
  (roughly −90 dB of the subtracted harmonic), so SNR beside a −60 dBc
  harmonic tops out near 100 dB rather than at the stimulus floor. Immaterial
  on device — the 16-bit stimulus floors bind first — but the surface's
  per-figure floor statement (slice on the HTTP surface) should say it.

**Slices 5 and 6 delivered (2026-07-27) — item 3 is complete.** Flash
`b5f9a1351adf` runner-verified, the four-vector corpus uploaded, and the
first-ever on-device quality figures recorded in
[`signal-path.md` §Measured output quality](../reference/signal-path.md):
THD+N at the 16-bit stimulus floor within 0.4 dB at every level tried, FR
flat to 0.01 dB from 40 Hz to 16 kHz, the limiter's steady-state gain
reduction distortion-free at −1.01 dBFS, the ISR-cost acceptance PASSED
(−13.0 k against the ±30 k gate, `dsp_cycles_last` p50 delta 39 cycles), and
zero overruns across the 20,492-block session. The committed driver is
`firmware/tools/measure_vectors/measure_run.py` (upload / measure /
isr-cost). What the leg found beyond the figures:

- **A fresh replay upload had never worked since the storage cutover** —
  every first chunk 500'd because the engine faulted the absent `.part`'s
  resume probe instead of reading it as "nothing to resume". Fixed
  (`b5f9a1351`) with the sd-suite regression, and hardware-verified in the
  same session; the web-tree REPLACE path's health is what localized it.
- **The generation latch fired live on the very first run**: arming before
  the board's first-ever audio block latches generation 0, and the first
  block's boot-time SD model apply bumps it — retire `failed (generation)`,
  exactly as specified. Operator flow: arm after audio has run once, or
  re-arm once after a cold boot.
- **`boot_generation` SURVIVED the 128 KiB `DMAMEM` addition** (247 → 250
  across three flashes and a reboot): the linker's retained-region carve-out
  closed the item-1-era ledger hazard, so the design's predicted restart did
  not happen. Recorded in signal-path.md's maintainer notes.
- The session also yielded the long-owed **first measured per-block DSP cost
  with audio flowing**: `dsp_cycles_last` p50 ≈ 281 k ≈ 4.4 % of the block
  budget on the PCM passthrough path with meters on.

**Slice 4 delivered (2026-07-27).** The surface: `GET /api/measure` (public
read — state machine, figures once published, the stimulus floors beside
them, vector id, and the METRICS overrun counter) and `POST
/api/measure/start` (auth-gated — it joined `route_is_control()` as the
fifth control route, which is exactly that gate's definition: mutates the
running system, touches no card). The body is strict on the replay-body
pattern: `id` required, `f0_l`/`f0_r` optional integers defaulting to 440,
`probes` up to 8 integer Hz; duplicates, unknown keys, fractions and
trailing bytes refuse with `400 measure_body`; a live capture or
analysis-rejected parameters answer `409 measure_start`. Swagger and the
contract suite moved in the same commit (`Measure`/`MeasureFigures`/
`MeasureStartRequest` schemas pin the state vocabulary, both geometries,
the floors and the arm-then-start operator order). Known adjacent debt,
NOT this slice's: the still-undocumented replay schemas in swagger.

**The 15 sd-editor vitest failures are fixed (2026-07-27).** They were
fixture drift from the LFE fold-in: `72bb041a5` added
`matrix.lfe_crossover_hz` to the model and `b5b1c826e` taught the editor
page to require it, but neither reached the vitest fixtures
(`dsp-config-*.json`) or the swagger `DspMatrix` schema — so every loaded
model was invalid, Apply refused to send, and all apply/PUT-flow tests
failed while the page itself was correct. The fix adds the key to both
fixtures (120 default / 80 SD), the field's mapping and bounds coverage to
`dsp-editor.test.js`, and `lfe_crossover_hz` to `DspMatrix` (schema,
required set, both examples) with the contract suite pinning its 20–500
bound. Note the shape of the miss: the firmware-side key-set guard from
`b5b1c826e` covers validator↔editor, but nothing guards
editor↔fixtures↔swagger — a third copy of the key list still drifts
silently.

**Slice 3 delivered (2026-07-27).** The runtime wiring, exactly as contracted
above: the tap sits after the tx24 commit in `enqueue()` (at 24-bit width the
committed words ARE the processed planes, so tapping the planes taps the
wire), with the generation/overrun latches read in the same interrupt that
advances them; the 128 KiB capture planes are `DMAMEM` (boot_generation
restarts at the first flash — expected) and the control state is DTCM
`.bss`; the epoch side lives at the METRICS checkpoint — a wall-clock stall
watch (armed 10 s, capturing 2 s, reclaim 1 s of `blocks` unchanged; the
armed bound covers the natural arm-then-start-replay operator gap), one
256-sample slice per epoch into BOTH channel accumulators (32 epochs per
window), and the budget row's declared 1 ms with
`g_metrics_service_overruns` on the HTTP checkpoint's pattern (the `runtime`
suite's budget pin moved in the same commit). The pipeline-reset prerequisite
is absorbed: `POST /api/replays/start` arms `g_pipeline_reset_pending` and
`update()` consumes it in audio context before any words are pushed.
`measure_start()` is defined and waits for slice 4's dispatcher; nothing can
arm a capture until that route lands.

**Slice 2 delivered (2026-07-27).** The stimulus corpus is
`firmware/tools/measure_vectors/`: four committed loop-coherent PCM vectors
(the −6 dBFS 440 baseline, the full-scale limiter-hot 440, the L 440 / R 660
split, and a seven-tone FR multitone at −18 dBFS per tone), 12,000 frames
each — a common multiple of every tone period used. Both generator
obligations are asserted before any byte is written and revalidated against
the committed bytes by the `measure` suite (obligations + tone content +
residual, deliberately not byte equality: `sin()` is libm's, so the committed
bytes are the provenance record).

### Noted for later, deliberately not scheduled

- **Linear-phase FIR EQ.** RBJ biquads are minimum-phase, so group delay varies
  with frequency — audible on transients once corrections get large. An FIR is
  affordable against 93 % free budget but costs latency, so it is a choice rather
  than an upgrade.
- **DTS dialnorm / DRC.** `core_decoder.h:61` parses `drc_present` and the
  stream's dynamic-range data is read and then discarded, so loudness varies
  between sources. Whether the coefficients are reachable through the decoder's
  interface is unverified — the flag is confirmed, the data is not.
- **Remove inter-stage requantisation.** The signal requantises to 24-bit integer
  between every stage (input EQ, downmix, output EQ, limiter). At 24 bits each
  step is ~−140 dBFS so it is inaudible, but carrying double end to end and
  quantising once is both cleaner and marginally faster.
- **A DC blocker** around 5 Hz.

## DSP work breakdown

The signal path below (steps 1–3) and the write/SD-persistence phase (PUT
`/api/dsp/config`) are done. The remaining DSP-side work is the SD
control-surface editor (Track A above, unblocked now), the meters endpoint
(B4, after the cutover), and the OpenAPI catch-up (B5). The committed sample
vectors are the executable specifications:

- `firmware/tools/dts_vectors/vectors/stereo_impulse.dts`;
- `firmware/tools/dts_vectors/vectors/surround51_impulse.dts`;
- `firmware/tools/dts_vectors/vectors/stereo_tones.dts`;
- `firmware/tools/dts_vectors/vectors/surround51_tones.dts`.

The ordering below is the DSP model's own build order and is complete through
step 4. Where it conflicts with anything else in this document, the shared
delivery sequence above wins:

1. **Done.** The native `dsp-vectors` suite decodes each of the four committed
   vectors through the portable DTS interface, drives the decoded PCM through the
   public DSP interface (`aius_dsp_downmix` / `aius_dsp_process`), and checks
   deterministic decoded-input and stereo-output golden fixtures
   (`tools/dts_vectors/golden/*.golden`). The sample corpus now runs through the
   pipeline.
2. **Mostly done.** The `dsp-vectors` suite proves, through the public interface
   on real decoded vector content: stereo isolation; FL/FR passthrough; symmetric
   FC contribution; SL-to-L and SR-to-R placement; LFE gain (default-excluded and
   −10 dB fold); clip-free headroom; trim/EQ bypass equivalence; finite output;
   and bounded latency (output frame count == input; seamless block-boundary
   continuity). Limiter threshold/lookahead/attack/release and input trim /
   per-input EQ are **not yet provable** — those seams do not exist in the
   portable interface and land with step 3.
3. Implement the bounded signal path and coherent meter snapshot behind the
   portable DSP interface. The sample-vector tests, allocation guard, cycle
   budget, and queue/latency bounds are the completion gate.
   - **Signal path done.** The input stage (per-channel trim + up to two EQ
     sections, `AiusInputStage`) and the true-peak safety limiter tail
     (`AiusLimiter`, feed-forward stereo-linked lookahead) are additive stages in
     `aius_dsp.h`; both bypass under NULL/zero-init so the target and goldens are
     unchanged. Proven through the public interface (`dsp` + `dsp-vectors`):
     trim/EQ behaviour and bypass equivalence, limiter threshold/attack/release/
     lookahead, prevention of the surround51_impulse full-scale saturation,
     finite/bounded output, seamless block boundaries, and allocation-free via
     the `--wrap` shim. `teensy41-optical` builds.
   - **Meter module done (slice 3b).** `AiusMeter` / `AiusMeterSnapshot`: peak/RMS
     per input, stereo out peak/RMS, limiter gain reduction, ≤128 decimated
     waveform points, sequence number, and a seqlock coherent copy boundary
     (host-tested: a reader never observes a torn snapshot); plus queue-depth /
     estimated-latency / DSP-cycle fields the target adapter populates. Proven
     through the public interface and allocation-free via the `--wrap` shim;
     goldens byte-unchanged. Metering is **caller-driven**, deliberately NOT a
     field of `AiusDspConfig` — the caller taps around `aius_dsp_process` — so the
     meter code is not linked into the hot function until the adapter wires it.
   - **Target memory strategy (for Phase 4).** FlexRAM (512 KB) is fully allocated
     as 32 KB banks shared by ITCM code, DTCM data, and the runtime stack, so
     crossing the 192 KB ITCM boundary grabs a whole bank out of the stack budget.
     **Principle: hot-path data earns its DTCM place and is never relocated for
     space.** The single-cycle, deterministic DTCM access is the whole point for
     the decode/DSP arena and the codec lookup tables; FLASH/OCRAM would add
     cache-miss latency and jitter that the audio cycle budget cannot absorb.
     (The codec tables — `adpcm_coeffs`, `high_freq_samples`, `band_fir_*` — are
     already `const`; they live in DTCM only because Teensy copies `.rodata` to
     fast RAM at boot. Leave them.) So the only levers are:
     - **Cold data → OCRAM.** `g_http_request` / `g_http_response` (16 KB) moved
       DTCM→OCRAM (`DMAMEM`) — off the audio path, written-before-read. DTCM
       306 → 290 KB; FlexRAM stack headroom 22 → 38 KB; OCRAM 88 % free.
     - **Cold code → FLASH.** New config/HTTP/serialize/apply/serve code runs in
       `loop()`/HTTP context, not the audio ISR, so mark it `FLASHMEM`: zero ITCM,
       no audio-latency cost (FLASH 93 % free). The single-config default is also
       `const` + `FLASHMEM` (read only on a config change). Only genuinely
       per-audio-block code (decode, DSP, meter accumulate) stays in ITCM.
     - **Escalation** for space, in order: shrink the model (single-config
       re-model does this ~4×), shift a FlexRAM bank DTCM→ITCM (reallocates the
       split without moving hot data), or fit 16 MB PSRAM (EXTMEM) for large cold
       data. Relocating hot data is not on this list.
4. Expose the proven model through `GET/PUT /api/dsp/config` and
   `GET /api/meters`, including transactional SD persistence and
   block-boundary apply.
5. Complete the SD-hosted control surface against captured responses from the
   same vector/config matrix. Test every control-to-JSON mapping, validation
   error, stale generation, pending/apply transition, meter, and waveform
   rendering before target smoke.

Do not resume the deferred network clean-room audits or production receipt
matrix from this plan. This plan's hardware validation — default audio
unchanged, config round-trip, meters rendering, editor loading, and the
latency/audio-continuity gates — no longer runs as a separate session: it is
folded into the single on-device session at B6 above, against one flash.

UI routing is resolved: the canonical editor route is `/sd/www/index.html`,
served from the SD filesystem directory `firmware/sd/www/`. The firmware already
serves `/sd/www/` static assets — `optical_main.cpp` matches the `www/` resource
prefix and `firmware/sd/www/` already holds the replays surface. The separate
`/aius/config.json` feature-flag file is not the editor. There is one UI
surface; do not create a second.

## Objective

Expose an industry-standard, bounded audio pipeline with an editable model
served from the SD card. Keep the flash-resident page as a recovery/status
surface and serve the full editor from `/sd/www/index.html`.

## Current gap

`firmware/config/optical_dsp.json` is converted at build time into bounded
profile and mix tables by `scripts/build_config.py`. The firmware currently
returns only numeric `mode` and `mix` selections from `/api/status`; the
embedded page shows names but not EQ sections or resolved mix coefficients.
The SD `/aius/config.json` file is a separate feature-flag document and is not
the DSP schema. The current compiled model is read-only and has no limiter,
loudness meter, or runtime DSP persistence.

## Contract decisions

**Model — single editable config (revised 2026-07-24).** Full parametric control
(input trim/EQ, downmix matrix, output EQ, limiter) makes the old
select-a-preset model redundant, so there are NO `profiles[]` or `mixes[]` arrays
and no active-selection / `mode` / `mix` machinery. There is exactly one
effective config:

- a hardcoded **factory default** — one `const` (and `FLASHMEM`, since it is read
  cold, only on a config change) config baked from `config/optical_dsp.json`,
  immutable in FLASH, active at boot and on reset;
- a single **SD override** — the full custom config parsed from `/aius/dsp.json`
  into RAM. `source` (`default` | `sd`) reports which is active; a block-boundary
  apply swaps it and increments `config_generation`.

The config shape is:

- `source`, `config_generation`, `sample_rate_hz`
- `features`: `input_eq`, `matrix`, `output_eq`, `limiter`, `meters` (bypass flags)
- `input_channels`: fixed `fl`, `fr`, `fc`, `lfe`, `sl`, `sr`, each `gain_db` + `eq[]`
- `output_eq[]`: up to two bounded EQ sections
- `matrix`: `center_gain_linear`, `surround_gain_linear`, `lfe_gain_linear`,
  `lfe_crossover_hz`, `headroom_linear`, `clipfree`
- `limiter`: `enabled`, `threshold_dbfs`, `attack_ms`, `release_ms`, `lookahead_ms`

Bounds are unchanged (input trim `[-60,12]` dB; EQ freq `[10,24000]`, Q `[0.1,20]`,
gain `[-24,24]`; limiter threshold `[-24,0]`, attack `[0.1,100]`, release
`[1,1000]`, lookahead `[0,5]`; two EQ sections per table; all finite), plus
`matrix.lfe_crossover_hz` `[20,500]` — **0 is deliberately outside the range**, so
"no crossover" is expressed by `lfe_gain_linear = 0` (fold the LFE out) rather than
by a corner of zero, which would otherwise mean two encodings of one state. The whole
config is well under the 12,288-byte cap.

**`matrix` is all-or-nothing on `PUT`: every key above is required.** So a stored
`/aius/dsp.json` written before `lfe_crossover_hz` existed now fails validation on a
missing key, and a `GET`-then-`PUT` round-trip against firmware whose serializer
omitted the key would 400. Both are why the `GET` serializer carries it. `POST /api/mode`, `POST /api/mix`, and
the embedded page's mode/mix selectors are removed (no consumers). Named presets,
if ever wanted, are SD config files loaded via the editor, not firmware arrays.

- Keep `GET /api/dsp/config` as the effective model read endpoint.
- Add authenticated `PUT /api/dsp/config` with a bounded JSON body. It validates
  the complete model, writes `/aius/dsp.json` transactionally, and applies it
  only at an audio block boundary. Invalid input never changes active DSP.
- The SD editor is static HTML/CSS/JavaScript under `firmware/sd/www/` (served at
  `/sd/www/`); it uses only the documented HTTP API and can be replaced without
  firmware flashing.
- The signal path is:
  `decode -> input channel trim/EQ -> downmix matrix -> output EQ -> true-peak
  safety limiter -> output`, with metering taps that do not alter audio.
- Input channel controls cover `FL`, `FR`, `FC`, `LFE`, `SL`, and `SR`. Each has
  a trim and up to two EQ sections. Output EQ is a stereo post-mix section.
  Matrix controls cover centre, surround, LFE, and headroom coefficients.
- Limiter controls are enabled, threshold, attack, release, and fixed bounded
  lookahead. Loudness normalization is not automatic in the critical path;
  BS.1770-style loudness/true-peak telemetry is exposed first, with target gain
  changes applied only as slow, explicit configuration changes.
- Feature flags can independently bypass input EQ, output EQ, matrix shaping,
  limiter, and meters for latency/diagnostic comparisons. DTS framing and
  decoding cannot be bypassed while consuming DTS input.
- Status reports queue depth, estimated latency, DSP cycle counts, limiter gain,
  and peak telemetry alongside the existing counters.
- `GET /api/meters` returns a coherent fixed-size snapshot: peak/RMS per input
  channel, stereo output peak/RMS, limiter gain reduction, and up to 128
  decimated waveform/envelope points. The SD editor polls it at 10 Hz and
  renders meters plus a canvas waveform; it never requests full-rate samples.
- Model shape, bounds, `source`, and `config_generation` are defined in the
  single-config block at the top of this section. All values are finite; the
  whole config is well under the 12,288-byte cap.
- A `PUT` includes `base_generation`; a mismatch returns 409 without touching SD
  or active DSP. `PUT /api/dsp/config` requires `X-Aius-SD-Auth`.
- The body is capped at 12,288 bytes. Because `AIUS_HTTP_MAX_RESPONSE` also sizes
  stack `body[]` arrays, the config response uses a dedicated OCRAM (`DMAMEM`)
  buffer sized to the model, not a 16 KB stack bump, to respect the FlexRAM stack
  budget. At boot the const default is active; a valid `/aius/dsp.json` override is
  parsed outside the audio callback and becomes pending only after transactional
  SD commit; the snapshot swaps at the next audio block boundary and increments
  `config_generation`. Invalid JSON, SD removal, failed rename, or invalid ranges
  leave the last valid snapshot active.
- `GET /api/meters` is a separate schema: `sequence`, `sample_rate_hz`, `input[]`,
  `output`, `limiter_gain_reduction_db`, `queue_frames`, `estimated_latency_us`,
  `dsp_cycles_last`, `dsp_cycles_max`, and `waveform[]` capped at 128 points.
  Meter values are diagnostic snapshots and never become configuration fields.
- The build and runtime validators reject out-of-range values, non-finite
  numbers, and unknown input-channel/limiter keys; the config is capped at 12,288
  JSON bytes.
- `headroom_linear` is the resolved runtime value. `clipfree` indicates the config
  selected the DSP clip-free headroom policy; the response does not expose the
  build-time string sentinel. For the default downmix coefficients clip-free
  resolves to `1 / (1 + 0.7071067811865475 + 0.7071067811865475 +
  abs(lfe_gain_linear))`; unity otherwise.
- `center_gain_linear` is the shared FC-to-L/R coefficient (raises dialogue; zero
  removes centre). `surround_gain_linear` is the shared SL-to-L / SR-to-R
  coefficient (blends the rears while preserving their L/R placement; zero removes
  surround). The coefficients are symmetric by design; independent SL/SR gains are
  excluded because they would shift image balance rather than control blend.
  Matrix gains are finite, `0 <= gain <= 4` linear; configured non-clipfree
  headroom is `0 < h <= 1`.
- OpenAPI marks objects `additionalProperties: false`, arrays with exact
  `maxItems`, and numeric fields with units and finite-number expectations. It is
  the canonical external model description (schemas, field descriptions, enums,
  example, path).
- The embedded flash page gains a compact monospace DSP section: `source`, input
  trims, output-EQ rows, matrix coefficients, limiter, and pending state. It
  fetches `/api/dsp/config` independently of the status poll and stays usable
  without SD. Every `mode`/`mix` selector and every reference to `POST /api/mode`
  or `POST /api/mix` is removed from the page.

## Phases

The delivery ordering is the [shared delivery sequence](#shared-delivery-sequence--2026-07-24)
at the top; this is the single-config work breakdown. **Steps 1–3 (the DSP model
— signal path, true-peak limiter, coherent meter module) are done and
committed**, as are the config model, read endpoint, and the write / SD
persistence / block-boundary apply phase. Remaining:

### Config model + read endpoint

Replace the profiles/mixes preset model with the single default config in
`config/optical_dsp.json` + `build_config.py`; serialize it via
`GET /api/dsp/config` (`FLASHMEM`); apply the single config in the audio path
(output EQ + downmix, default bit-identical to today); remove `POST /api/mode` /
`POST /api/mix` and every mode/mix reference from the embedded page; update swagger
+ the native `config` and Vitest OpenAPI suites. **Gate:** those suites green +
target build + default audio unchanged on device.

**Landed.** Single-config model + `PROGMEM` FLASH-resident default (`FLASHMEM` is
for code, not const data — the const config uses `PROGMEM`); serializer + apply
helper `FLASHMEM`; mode/mix routes, UDP verbs, and all embedded-page references
removed. `config` + swagger suites green; `teensy41-optical` builds with ITCM
headroom *improved* (98.5 %, 2968 B free — the smaller model + FLASH placement net
freed fast RAM). Default audio is bit-identical by construction (matrix = default
coefficients; EQ bypass = no-op); on-device audio confirmation is part of the live
smoke. Input trim/EQ + limiter application and PUT/SD are the next phases.

### Write + SD persistence + block-boundary apply

Authenticated `PUT /api/dsp/config` (`X-Aius-SD-Auth`) validates the bounded body,
writes `/aius/dsp.json` transactionally, and swaps the active snapshot at an audio
block boundary (`base_generation`/409, `source: sd`, `config_generation`
increment). Cold code is `FLASHMEM`; the config response uses a dedicated OCRAM
buffer, not a stack bump. Wire the input-stage trim/EQ and the limiter into the
audio apply here (they exist in `aius_dsp` but are not yet applied). **Gate:**
native config / SD-round-trip tests + target smoke, no flash writes.

**Input trim/EQ + limiter wiring done.** `AiusAudioProcessor::apply_config()`
(`src/optical_main.cpp`) now resolves `AIUS_OPTICAL_CONFIG.input_channels[]` into
an `AiusInputStage` (per-channel trim + up to two EQ sections, backed by static
scratch planes sized to the largest decoded block) and `AIUS_OPTICAL_CONFIG.limiter`
into an `AiusLimiter`; `enqueue()` passes both into `AiusDspConfig` gated by
`features.input_eq` / `features.limiter`. `apply_config()` moved out-of-line (it is
a class member so an in-body definition has vague/comdat linkage, which no longer
compiles once it is too large to always-inline alongside the plain `FLASHMEM` free
functions already sharing the `.flashmem` section — GCC's "section type conflict").
Default config is still bypass-equivalent (unity trim, no EQ, disabled limiter), so
default audio is unchanged by construction. `teensy41-optical` and
`teensy41-optical-fault` both build; RAM1 free-for-locals is 22,684 B (the ~12 KB
input-stage scratch consumes most of the 22→38 KB headroom the earlier OCRAM move
freed, still comfortable margin).

**PUT `/api/dsp/config` + SD persistence + block-boundary apply done.** A new
bounded, allocation-free parser library (`lib/aius_dsp_config/`, mirroring
`lib/aius_config`'s hand-rolled recursive-descent style) validates the PUT wire
shape — the same 8 top-level keys `GET /api/dsp/config` returns plus a 9th,
`base_generation` (the optimistic-concurrency token) — against exactly
`scripts/build_config.py`'s bounds, and is proven by a new native `dsp-config`
suite (40+ assertions: every bound, every unknown/missing key at every nesting
level, wrong types, `>2` EQ sections, `source != "sd"`, malformed JSON, duplicate
keys, oversize). `aius_http.c`'s route gate now allows `PUT` on `DSP_CONFIG`
(POST/DELETE still rejected); `AIUS_HTTP_MAX_REQUEST`/`AIUS_HTTP_MAX_BODY` raised
to fit the 12,288-byte model plus header overhead — `g_http_request`/
`g_http_response` stay `DMAMEM`, unaffected by the earlier ITCM budget.
`http_dsp_config_put_request()` (`FLASHMEM`) checks `X-Aius-SD-Auth`, parses and
bounds-checks the body, 409s on a stale `base_generation` without touching SD or
the active snapshot, then reuses the existing `AiusSdJob` transactional
mkdir→write→backup→install state machine (a new `dsp_upload` job-kind bit,
parallel to the existing `config_upload`) to write `/aius/dsp.json` verbatim. A
new `g_pending_dsp_model`/`g_dsp_upload_pending`/`g_dsp_apply_pending` staging
triple (mirroring `g_pending_config`/`g_config_upload_pending`, all `DMAMEM`
except the `volatile bool` flag) hands the validated model from SD-job success to
`AiusAudioProcessor::enqueue()`, which is the actual block boundary: it applies
the staged model, sets `source: sd`, and increments `config_generation` there,
never in loop/HTTP context. `config_generation` is a per-boot-session counter
(0 at boot for the default default, +1 per applied change) — not itself
persisted; only the client-supplied `base_generation` on a PUT is a trusted
concurrency token, so a re-loaded SD file's stale `config_generation` is safely
ignored. `stage_dsp_sd_override()` (`FLASHMEM`) loads `/aius/dsp.json` through
the *same* parser at boot and on SD hot-reinsert (mirroring `load_sd_config()`'s
two call sites) — the persisted file already carries `source":"sd"` and a
`base_generation`, so it round-trips through the identical validator. `GET
/api/dsp/config` now serializes the live `g_active_model` (RAM mirror, `DMAMEM`,
kept in sync at the same block-boundary apply point) and reports the real
`source`/`config_generation` instead of hardcoded defaults, via a dedicated
`DMAMEM` `g_dsp_config_body[12288]` buffer instead of the shared 8 KB per-route
stack array (that array was previously too small for the model's true 12,288-byte
cap — a latent truncation bug this closes).

Two budget findings worth keeping in mind for future FLASHMEM work in this
codebase: (1) GCC does **not** merge the `section` attribute from a forward
declaration onto a later definition that omits it — `FLASHMEM` must be applied
directly on every definition, not via prototypes, or the code silently stays in
ITCM. (2) `strtod()` was dropped from the new parser in favor of a small
hand-rolled decimal-to-double decoder: pulling in newlib's general `strtod`/
`_dtoa_r` machinery cost ~7.7 KB of ITCM — nothing else in this firmware
previously needed floating-point string parsing, so it wasn't already paid for.
The custom decoder is not bit-exact with `strtod()` for pathological inputs
(very long digit runs, denormals) but is exact within double precision for every
value this schema's bounds allow.

`teensy41-optical` and `teensy41-optical-fault` both build with these changes;
RAM1 free-for-locals is back to 22,684 B, matching the input-stage-wiring
baseline above (net near-zero additional ITCM/DTCM cost once the parser and new
statics were correctly FLASHMEM/DMAMEM-routed). Native `dsp-config`, `http`,
`dsp`, `dsp-vectors`, and `config` suites are all green. Genuine on-device smoke
(a live PUT over the network, an actual SD write/reboot/reload cycle) has not
been run against real hardware in this session — target build success is the
proof available without physical device access; live-hardware smoke is a
follow-up before shipping this to the actual unit.

### Meters endpoint + accumulate wiring

**This work is now Phase 14b of
[`aius-network-hardening.md`](aius-network-hardening.md) (step B4).** It wires
into the audio callback and adds a dispatch route, both of which Phase 14
rewrites; landing it before the cutover means writing it twice against two
runtime shapes. The contract stays this plan's; the phase slot, owned paths, and
gate live with the cutover sequence. Its route is declared by Phase 11's
dispatcher (step B1) so the dispatcher is written once.

`GET /api/meters` from the coherent snapshot; wire the per-block meter accumulate
into the audio path — the one genuinely hot addition, and ITCM free space is
1,352 bytes, so the accumulate tap is the only part that may stay in ITCM.
**Gate:** coherent bounded snapshots, no audio-thread allocation, ITCM within
budget.

#### Settled wire shape (2026-07-24)

Three of the four points this section previously left open were never really
open: `AiusMeterSnapshot` already fixes them. `input_peak[]` / `input_rms[]` are
six entries in `AIUS_CH_*` bit order, `output_peak[2]` / `output_rms[2]` are
already per-channel `[L, R]`, and `waveform` is a non-negative per-bucket
peak-magnitude envelope with an explicit count, capped at 128. The JSON mirrors
the struct rather than reshaping it.

What was genuinely open was **units**, because the struct's header delegates the
`20·log10(v / AIUS_DSP_SAMPLE_MAX)` conversion to the reader. That delegation is
reversed: **the firmware converts.** The deciding argument is that `log10(0)` is
`-Infinity`, which is not valid JSON, and silence is an audio meter's resting
state rather than an edge case. Converting once on the device with a documented
floor beats every client reinventing that clamp.

```json
{
  "sequence": 1234,
  "sample_rate_hz": 48000,
  "input": [
    { "peak_dbfs": -6.0, "rms_dbfs": -12.3 }
  ],
  "output": {
    "peak_dbfs": [-3.1, -3.2],
    "rms_dbfs": [-9.4, -9.5]
  },
  "limiter_gain_reduction_db": -1.5,
  "waveform": [0, 12, 340, 998, 1000, 640],
  "queue_frames": 0,
  "estimated_latency_us": 0,
  "dsp_cycles_last": 0,
  "dsp_cycles_max": 0
}
```

- **Levels are dBFS, floored at `-120.0`.** Every `*_dbfs` field is finite;
  silence reports exactly `-120.0`, never `-Infinity` or `null`. Computed in the
  HTTP path, which is cold `FLASHMEM` code, so the ~16 `log10` calls per snapshot
  cost nothing the audio budget can feel.
  `limiter_gain_reduction_db` is already dB (`<= 0`) and passes through unconverted.
- **`input` is exactly six positional entries** in `AIUS_CH_*` order: `fl`, `fr`,
  `fc`, `lfe`, `sl`, `sr`. No per-entry channel key; position is the identity.
- **`output` keeps both channels** as two-element `[L, R]` arrays. Flattening to a
  scalar would discard information the pipeline already has.
- **`waveform` is per-mille integers, `0..1000`**, of full scale: non-negative
  magnitudes, drawn mirrored about the centre line. Chosen over raw sample units
  and over normalised floats because it is both the smallest encoding (~640 bytes
  against ~900 raw) and the cheapest to format, avoiding 128 float conversions per
  snapshot in the HTTP path. The array's length is the count; there is no separate
  `waveform_count` on the wire.
- **`features.meters` false is not a special case.** The route always returns 200
  with a valid snapshot. Nothing publishes while metering is bypassed, so
  `sequence` stays `0`, which the struct already documents as "never published".
  Clients render `sequence == 0` as "no data" rather than as zero-level signal.
  This reuses an existing sentinel instead of inventing a status-code convention.

**Both consumers need a change, and 14b owns both.** `replays.js` is wrong on two
counts today, independent of which shape was chosen: `line 18` does
`Number(output.peak_dbfs).toFixed(1)`, which yields `NaN` on a two-element array,
and its `y = h/2 - point*h/2` waveform maths assumes points in roughly `[-1, 1]`
while the struct holds `int32_t` sample units, so raw values would draw thousands
of pixels off-canvas. The SD editor accepts the chosen shape already but degrades
on a non-2xx rather than on `sequence == 0`, so it needs the "no data" branch.

### SD control-surface editor

**Track A — delivered, except the on-device asset smoke.**
`firmware/sd/www/index.html` plus `dsp-editor.css` / `dsp-editor.js` — static
HTML/CSS/JS using only the documented API; every control mapped to the config
JSON; meters + canvas waveform from `/api/meters`. It follows the existing
`firmware/sd/www/replays.*` precedent (self-contained classic script, no
shared-module imports, no `mr-*` kit) and links `replays.css` before its own
sheet so shared chrome cannot drift between the two SD pages. The device serving
the page off its own SD card, unable to reach the host application's shared mounts, is the
one standing exception to the repository's UI-kit reuse rule.

The regression is `test/sd-editor/dsp-editor.test.js` — vitest
plus happy-dom, loading the real page body and evaluating the real script against
a stubbed device contract, so it needs neither a served URL nor a `web.json`
mount. 36 tests cover the field-to-DOM mapping for every model field, a
byte-level PUT round trip, every documented bound (12 scalar cases + 6 EQ cases,
each asserting the inline message, `aria-invalid`, and that Apply refuses to
send), the 409 stale-generation path and its re-read recovery, the
pending→applied transition polling for a `config_generation` change, canvas
waveform geometry and the 128-point cap, `/api/meters` 404 degradation with
back-off, and hard transport failure on both read and write surfacing visibly.

Two page-level modelling decisions worth keeping: the EQ type select carries a
page-level `none` (section omitted from `eq[]`) alongside the real firmware
`bypass` type, because the wire model has no "absent" type; and with `clipfree`
checked the headroom input goes read-only and displays the resolved
`1 / (1 + centre + surround + |lfe|)`, which is what gets sent.

**Remaining:** the target asset smoke — the page loading and driving the real
device — folds into B6. The meter path is proven against authored-to-contract
fixtures rather than captured device responses, because `GET /api/meters` does
not exist yet; recheck it against the real endpoint once 14b lands and the wire
shape above is settled.

#### Editor enhancement — visual surface, presets, ergonomics (task #7)

Contracted 2026-07-27 after the design interview; prebuild-gated the same
day. **One requirement from the mid-session capture was wrong and is
dropped: there is NO per-channel L/R output EQ.** `output_eq` stays one
cascade applied to both channels; no config-model, engine, or wire change
anywhere in this phase. The phase is page-side only — zero firmware
*behaviour* changes; one firmware-tree *test-only* update is budgeted:
`test/dsp-config/test_editor_keys.py` regex-greps `collectModel()`'s
object-literal shape, so restructuring it (defaults table, preset merge)
may need the guard's extraction updated in the same commit.

Decisions taken (interview, 2026-07-27):

- **Presets are the full editable model** — the five editable groups
  (`features`, `input_channels`, `output_eq`, `matrix`, `limiter`), exactly
  the PUT body minus `source`, the generations, and `sample_rate_hz` (a
  device fact, displayed read-only).
- **Presets live at `/sd/www/presets/<name>.json`, using the existing SD
  file surface end to end — zero new firmware routes.** GET is the public
  read, PUT and DELETE are the existing auth-gated file verbs (the same
  surface the flash page's uploader uses), and listing parses the documented
  HTML directory index (`GET /sd/www/presets/`, 64-entry cap, exact framing
  pinned in swagger). Chosen over a new `/aius/presets/` route family once
  the reuse option surfaced; consequences accepted: presets are publicly
  readable like every www asset, and a missing directory reads as an empty
  list. Card re-provisioning must stay additive so it never clobbers user
  presets (noted in `firmware/tools/aius-sd-sync.sh`'s header, where the
  next provisioning author will look).

  The prebuild gate (2026-07-27) pinned the transport facts slice 2 must
  build against, all verified in firmware:

  - **One fetch, string body.** The www PUT is a *streamed* request fed to
    the SD job in chunks, so the 12,288-byte non-streamed body cap does not
    apply — but it needs `Content-Length`, so the page sends a string, never
    a `ReadableStream`. `Content-Range` is ignored on the non-replay branch
    (REPLACE, whole body, offset 0): chunked preset upload is impossible and
    unnecessary.
  - **Name grammar, page-enforced before any PUT:** non-empty, ≤27 chars of
    `[A-Za-z0-9._-]` (32-char segment cap minus `.json`), **not ending in
    `config` case-insensitively** — `resource_is_config()` is a bare
    case-sensitive suffix match on `config.json`, so `myconfig.json` would
    403 on PUT/DELETE while GET still serves it — and a case-insensitive
    duplicate check, because FAT lookup is case-insensitive (`Bass.json`
    silently overwrites `bass.json`).
  - **Refusals are enumerated from the firmware, not swagger** (swagger
    under-documents the /sd file verbs; slice 2 also catches swagger's
    `/sd/{resource}` PUT/DELETE response sets up in the same commit).
    Notably `507 sd_upload` covers both invalid-path and busy-on-PUT and
    must never render as "card full" (`sd_capacity` is the disk-full
    token); DELETE of a missing preset is `404 sd_not_found`.
  - **Index parsing:** skip the `../` row (present in any dir below
    `www/`), key "empty list" on the `404 sd_not_found` *token* only
    (`sd_not_directory` means a file named `presets` — an error; every 503
    is an error, never empty), sort page-side (entries arrive in FAT
    order), and surface the footer's `truncated: yes` instead of showing a
    silently partial list.
- **Load fills the editor; the existing `PUT /api/dsp/config` applies.**
  There stays exactly one mutation path into the audio hot path; the
  409/staged-apply flow is unchanged. Upload is the same shape: a local
  file picked in the browser, validated page-side, then the same preset
  PUT. **The merge rule is load-bearing:** loading a preset merges ONLY
  the five editable groups into the form — never `adoptModel()`, which
  would clobber `baseGeneration` and `sample_rate_hz`. `sample_rate_hz`
  is live, not cosmetic: the PUT's value becomes the staged model's
  biquad design rate and nothing compares it to the real audio clock, so
  a preset saved elsewhere carrying a wrong rate would silently redesign
  every filter. Preset save writes the five groups only; preset upload
  validation ignores `source`, the generations, and `sample_rate_hz` if
  present in a user-edited file.
- **The visual mix/EQ surface is interactive**, beside text mode on the one
  editor page: drag handles write frequency/gain back to the same model the
  text fields edit (wheel adjusts Q); bounds validation identical. Layout:
  EQ curves + metering top-adjacent in signal-flow order (input meters →
  input EQ → matrix → output EQ → limiter GR → output meters/waveform).
- **Auth token persists in `localStorage`**; restored on boot.
- **Per-parameter reset-to-default** restores the factory value for that
  field. The defaults table lives in the page and is pinned by a vitest
  chain with a real anchor: page table → the `dsp-config-default.json`
  fixture → `firmware/config/optical_dsp.json`, the checked-in input
  `build_config.py` compiles into `AIUS_OPTICAL_CONFIG`. The gate found
  the fixture itself was pre-drifted — it still carried pre-item-1/2
  values (limiter off, LFE silenced, doubled headroom) and every test was
  green — so the fixture→factory assertion landed with the corrected
  values *before* slice 1, and the swagger `current` example was fixed in
  the same commit.
- **A realtime DSP processing-headroom number** (added 2026-07-27): percent
  of the per-block cycle budget, derived page-side from the snapshot's
  `dsp_cycles_last` / `dsp_cycles_max` against
  `block_frames x cpu_hz / sample_rate_hz` (the same arithmetic the
  measured-figures tables use: 6,400,000 cycles per 512-frame block at
  48 kHz). Shown as load % and headroom % beside the existing raw cycle
  counts; the constants mirror firmware facts with cited sources, like
  `MODEL_MAX_BYTES`. Sample peak vs true peak is irrelevant here — this is
  cycle accounting, not level.
- **A limiter visualization** (added 2026-07-27): the snapshot's
  `limiter_gain_reduction_db` (instantaneous at publish, documented as
  such) rendered as a gain-reduction bar plus a short page-side history
  strip at the 10 Hz poll rate; no firmware change.

Slices, each a commit boundary with its vitest coverage in the same commit:

1. **Ergonomics — DELIVERED 2026-07-27.** Token in `localStorage`
   (harness clears per test; accepted: a stored token is readable by any
   script on the device origin), per-parameter reset on every field
   (button disabled alongside its input; a click inside the checkbox
   `<label>` needs `preventDefault` or it toggles the box back), and the
   defaults pin closed end to end: the page's `FACTORY_DEFAULTS` table →
   the reset-all test reproducing the default fixture's five groups in
   the PUT body → the fixture↔`optical_dsp.json` equality assert.
   `collectModel()` was not restructured, so the firmware-side key guard
   needed no update.
2. **Presets — DELIVERED 2026-07-27.** Preset card (name, save-current,
   load/delete per row, upload, refresh), DOMParser over the index HTML
   (dirs and `../` skipped, `.json` only, page-side sort, `truncated:
   yes` surfaced as an incomplete-list warning), the full name grammar
   with the case-insensitive `config`-suffix and duplicate refusals,
   save gated on a valid form, load merging exactly the five groups
   (device facts in a hand-edited file proven ignored: the following
   Apply still carries the device's generation and rate), 507
   `sd_upload` rendered as busy-or-refused never card-full, and the
   swagger `/sd/{resource}` PUT/DELETE response sets documented from the
   firmware dispatch with contract assertions — including the
   example↔fixture deep-equality pin on the `current` GET example. One
   behaviour worth keeping: mutation confirmations are set AFTER the
   list refresh, or the refresh's count message eats them.
3. **Visual surface — DELIVERED 2026-07-27.** The signal-flow strip under
   the status card: input level bars → interactive input EQ curve (channel
   select) → output EQ curve → limiter GR bar + 128-sample history →
   output level bars, plus DSP load/peak-load/headroom % beside the raw
   cycle counts (budget = 600 MHz × 512 / rate, provenance cited at the
   constants). Pure-function math half (RBJ curves, every rect mapping,
   exported as `window.aiusDspEditorMath` solely as the unit-test seam —
   the classic-script IIFE has no other test surface) + wiring half where
   every gesture lands in `writeField()` → the same form input + `input`
   event, so there is no second model and `collectModel()` is untouched.
   Suite 62 → 92; headless full-page render clean. **Finding to fold into
   slice 4 or a follow-up: `onFieldInput()` promotes to `edited` only from
   `applied`/`error`, so any edit right after a fresh read leaves the pill
   at `loaded` while the form differs from the device (typed edits
   included; `loadPreset()` already treats `loaded` as promotable). One
   word to fix, touches existing pill expectations.**
4. **Deploy + on-device smoke** — upload the changed www assets to the
   card, load the page against the board (real browser), exercise one
   preset save/load, one drag edit → Apply round trip, and the
   load/headroom + limiter strip against live meters. This also closes
   the long-open Track A asset-smoke debt (B6).

### OpenAPI catch-up

**Step B5. The `PUT` half is done; `GET /api/meters` waits for 14b** and the wire
shape settled above. `swagger.json` gains `putDspConfig` with the `SdAuth`
parameter, a `DspConfigUpdate` request body and schema, and an `SdJobError`
schema; the request reuses the existing `DspFeatures` / `DspInputChannels` /
`DspInputChannel` / `DspEqSection` / `DspMatrix` / `DspLimiter` sub-schemas
rather than forking a parallel request-side tree, and the suite asserts that
reuse by deep equality so a future divergence fails loudly.

**Documenting the route against the firmware, rather than against this plan,
found six places the plan was wrong or silent.** They are corrections to the
contract, not to the code:

- **"Validates and commits atomically" hides a split.** The handler never sends
  the 200. On success `http_dsp_config_put_request()` only *starts* an SD job;
  `{"ok":true}` is emitted later from the job's cleanup state
  (`optical_main.cpp:649`), and even then the model is only *staged* — the apply
  and the `config_generation` increment happen at the next audio block boundary.
  A client that reads 200 as "active" is wrong; it means "durably written".
- **The response set is larger than 400/401/409/503.** The transport can emit
  `413 request_too_large` and `413 body_too_large` before the handler runs, and
  the SD job can emit `500 sd_commit` or `507 sd_write` asynchronously with a
  five-field body (`ok`, `error`, `received`, `written`, `expected`), not the
  two-field error shape every other route uses.
- **Every parser failure collapses to one wire string.**
  `AiusDspConfigErrorCode` distinguishes unknown key, duplicate key, out of
  range, too many EQ sections, invalid source, invalid EQ type, and oversize —
  the wire returns `dsp_config_invalid` for all of them. The plan's validation
  section reads as though those distinctions are observable. They are not.
- **The 12,288-byte cap is not the observable boundary.**
  `AIUS_HTTP_MAX_REQUEST` is 13,312 for headers plus body, so a body over
  12,288 still reaches the parser and returns 400; only one that fills the
  transport buffer returns 413.
- **`config_generation` is required on a PUT and then ignored.**
  `parse_top_value()` parses it into a discarded double
  (`aius_dsp_config.c:1131`). Only `base_generation` drives the 409. The plan
  never says the request must carry it at all.
- **Request EQ sections are laxer than response ones.** `parse_eq_section()`
  requires only `type`; `frequency_hz`, `q`, and `gain_db` each default to 0
  when absent. For a non-bypass type the 10 Hz and 0.1 lower bounds make
  frequency and Q effectively required, but `gain_db` may be omitted for any
  type, and `{"type":"bypass"}` alone is accepted. `GET` always emits all four.
  **The schema deliberately stays stricter than the parser here** and records
  the laxity in prose: everything the document permits, the device accepts, and
  forking a laxer `DspEqSectionUpdate` would cascade into duplicate channel and
  channel-map schemas for no client benefit.

**Gate:** the Vitest OpenAPI contract suite (green, 2 tests) plus the Prettier
JSON check. Not proven: no OpenAPI schema validator runs over this document in
the repository — the suite is hand-rolled assertions — and the 500/507 field
list is read from `sd_job_send_error()`'s format string, not from a captured
device response.

### Unfinished mode/mix removal in the the host application app

The single-config work removed `POST /api/mode` and `POST /api/mix` from the
firmware, the UDP verbs, the embedded flash page, and `swagger.json` — but not
from the the host application app at ``. Three loose ends remain, found
while assembling Phase 11's route inventory:

- **Two checked-in tests contradict each other.**
  `frontend/test/scenarios/control-shell.json:13` asserts
  `/api/mode` and `/api/mix` **are** present in `swagger.json` (under
  `AIUS.APP.OPENAPI.1`), while `test/swagger.test.js` asserts they
  are **absent**. `swagger.json` does not contain them, so the browser scenario
  fails whenever it runs.
- **The dashboard still wires dead buttons.** `frontend/src/app.js`
  lines 92–93 POST to both routes. The firmware no longer dispatches either, so
  both controls now produce a 404.
- **The firmware parser still accepts them.** `aius_http.c` matches `/api/mode`
  and `/api/mix` into live route enumerators that nothing dispatches. Phase 11
  deliberately left them parsing and gave each a bounded 404, because removing
  them breaks the `http` suite (`test/http/test_http.c` depends on them in six
  places, four only incidentally as a POST-with-body vehicle) and that suite is
  not Phase 11's to edit.

None of this affects the device. The firmware-side half (retiring the `MODE` and
`MIX` route enumerators and their parser entries) lands with Phase 14, whose
owner holds `test/http/`.

**Decision (2026-07-24): retire the the app dashboard, keep the OpenAPI
document.** The device now serves its own full control surface from SD, so the
101-line dashboard at `frontend/` duplicates it and is the part
that has rotted. `swagger.json`, its Vitest contract suite, and the two
`web.json` mounts stay: the API contract is useful whether or not a device is
reachable, and it is the only machine-readable description of the wire format.
The flash page, not the host application, is the no-SD fallback, so nothing is lost as a
backstop.

**Queued as step B9, the last thing in the sequence.** The dashboard is out of
scope until then: not repaired, not partially fixed, not investigated. Its
`control-shell.json` scenario fails today and stays failing, and its mode/mix
buttons 404 and keep doing so. Both are known and neither blocks anything.

Deferring it to the end is also what makes the deletion cheap, because it is not
a local change: the aius dashboard is an enrolled participant in the ui-kit
accessibility rollout, an active workstream owned elsewhere.

- `src/libs/frontend-shared/test/fixtures/ui-contract-allowlist.json` carries an
  entry for `frontend/src/style.css` under `ownerTask:
  aius-adoption` with a `removeByPhase`. `ui-contract.test.js` matches allowlist
  entries against findings scanned from the tree, so deleting the file fails
  that suite until the entry goes too. The fixture also pins a
  `baselineCommit`, so it is not a file to edit casually.
- `docs/reference/ui-kit-compliance-catalogue.md` carries four aius rows,
  including a sequenced Phase 8 adoption whose manifest row the catalogue states
  "the sequenced Phase 8 manifest coordinator alone owns".
- `docs/plans/ui-kit-accessibility-rollout.md` is currently uncommitted in the
  shared tree, i.e. that rollout is mid-flight right now.

Two things make this cheaper than it looks: the planned contract scenario
`src/libs/frontend-shared/test/scenarios/contracts/aius.json` does not exist
yet, and `contracts/manifest.json` has no aius row. So the retirement cancels
planned work rather than unpicking landed work. It still needs the rollout
owner's agreement before anything is deleted, and by B9 that rollout may have
moved on far enough that the question answers itself.

`AIUS.APP.SHELL.1`, `AIUS.APP.CONTROL.1`, and `AIUS.APP.TELEMETRY.1` retire with
the dashboard; `AIUS.APP.OPENAPI.1` survives and needs rewording, since it says
the document describes "every device route" and the stale route list is exactly
what `control-shell.json` was asserting.

### Target validation

**Merged into step B6**, the single on-device session shared with the network
plan's recovery smoke: default audio unchanged, config round-trip
(GET/PUT/SD/reset), meters render, editor loads, no audio/HTTP overruns under a
short soak — all against one flash rather than a separate DSP-only session.

## Test scope and evidence

There is no `Aius.Regress` project or hardware requirements registry in this
repository. The authoritative regression scope is the existing
`firmware/scripts/test.sh --environment native --suite http`, `--suite
dsp-config` (the PUT /api/dsp/config bounded validator, added with the SD
persistence phase), the generated config validation test added to that runner,
the Vitest OpenAPI contract test, the target PlatformIO build, and live target
smoke/soak evidence. The embedded page contract will also have a Playwright
fixture regression that loads a captured model response and asserts the
field-to-DOM mapping; target-only network checks remain a separate hardware
gate.

## Acceptance gates

- All four committed DTS vectors drive decoder-to-stereo known-answer tests for
  routing, coefficients, EQ, limiter behaviour, finite output, and latency.
  (Done — the `dsp-vectors` suite.)
- `GET /api/dsp/config` returns the complete single effective config (`source`,
  input channels, output EQ, matrix, limiter, features) with resolved units.
  (Done — serializes `g_active_model`, real `source`/`config_generation`.)
- Authenticated `PUT /api/dsp/config` validates and commits atomically; invalid
  or stale (`base_generation`) writes leave SD and the active block-boundary
  snapshot unchanged. (Done — native-proven; live-hardware smoke still open.)
- `GET /api/meters` returns coherent bounded snapshots without audio-thread
  allocation or unbounded sample transfer.
- OpenAPI schemas and example describe every route and returned field; no
  `mode`/`mix` selection endpoints remain. (**Partly met** — `GET` and `PUT
  /api/dsp/config` are both documented as of step B5; `GET /api/meters` lands
  with 14b, once its wire shape is settled.)
- The `/aius/dsp.json` override is loaded through the bounded SD job path, not a
  synchronous read, so the boot and hot-reinsert paths respect the
  one-SD-primitive-per-epoch contract. (Closed by step B3.)
- The SD editor maps every control to the documented JSON model and renders
  meters/waveform data from captured vector-driven responses.
- The embedded page reports the effective config (`source`, trims, EQ, matrix,
  limiter, pending state) without depending on SD, and carries no mode/mix
  selectors.
- The config stays within the request/response capacity and the DSP cycle/latency
  budgets without disrupting audio; hot-path data stays in DTCM, cold code/data in
  FLASH/OCRAM.
- Existing `/api/status`, the fallback page, and SD feature configuration continue
  to work; removing the `mode`/`mix` selection is intentional (no consumers).
