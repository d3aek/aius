---
title: Aius loose-end remediation (2026-09-02, late) — handover
type: plans
scope: component
component: aius
status: in-progress
subtype: how-to
tags: [aius, firmware, handover, itcm, dsp-config, recorder]
updated: 2026-09-02
toc: true
---

# Aius loose-end remediation (2026-09-02, late) — handover

**Transient baton, not a record.** Delete this document in the commit that lands the
last item under §Next action and §Pending decisions, after promoting anything durable
per the handover skill's retirement rule. Resumers: check §What landed against
`git -C git_modules/aius log` first; anything after it is invisible here.

The 2026-09-02 review-fix handover was retired the same day and its loose ends moved to
[`aius-open-work-register.md`](aius-open-work-register.md) §Waiting on the operator.
This session worked that list: every item is now closed or reduced to one owner decision,
except the ITCM placement pass, whose state is below.

## State — resume here

**Status.** DONE: recorder-auth rule corrected, CONTRIBUTING path sweep, SD fixture,
H1 accepted on host proof, optical-cutover and registry suites green, `staged_generation`
advancing at SD commit, ITCM placement pass (headroom 1,108 → 5,844 B). BLOCKED on the
operator: the flash, the gate-4 decision. Nothing is in flight.

### What landed (all pushed)

| Commit | Contents | Proof |
|---|---|---|
| `git_modules/aius` `22fe1ae` | CONTRIBUTING.md paths swept to repository-relative; sd-editor paragraph names `test.sh --suite sd-editor`; recorder plan: auth note + H1 accepted, Phase D still IN-PROGRESS on three items; `dsp-config-sd.json` carries `staged_generation`/`config_pending` and the PUT-verbatim case strips them; optical-cutover re-pinned to the current firmware (micros watermark, recorder-bank rename, six clock keys, staged-aware origin, per-slot recorder array, OTA pad-1 alias model); registry guard models repo-root suites via `$aius_root`, sd-editor catalogued | sd-editor 359 green; optical-cutover 33/1/1/1/6 green; registry green (60 arms); shellcheck clean |
| `git_modules/aius` `c34242e` | `g_staged_generation++` moved from `app_store_dsp_config()` (accept) to `sd_settle_http_write()` (commit, before arming `g_dsp_apply_pending`); serializer comment; network-hardening defect 22 correction (d); register strikes | `teensy41-optical` cross-build SUCCESS, ITCM 195500 / 1108 free unchanged |
| `git_modules/aius` `f0a91e0` | `CLAUDE.md`: reads need nothing except the five `/api/recorder*` routes, token-gated on read by contract (promoted through `the maintainer`, operator-approved) | docs only |
| `git_modules/aius` `bcf2233` | every definition in `aius_recorder.c`, `aius_recorder_bank.c`, `aius_config.c`, `aius_auth.c` is `FLASHMEM` (51 functions); the three libraries left `ITCM_ALLOWED_LDF_LIBS` and are gated to zero `.text`; the named recorder-helper guard retired (five guards); README/CONTRIBUTING/test.sh wording; register: item struck, nm census + ldscript proposal recorded | `teensy41-optical` cross-build SUCCESS, ITCM 195,500 → 190,764 B; itcm-placement (29 unit + target), memory, recorder, auth, config, optical-cutover, registry green |
| upstream repository `b91a507fc` | pointer bump; `swagger.json` `staged_generation` / `dsp_staged_generation` descriptions say "committed" | `swagger.test.js` 11 green; prettier clean |

### Uncommitted tree state

`git_modules/aius`: clean at `bcf2233` plus this document. The current
`firmware/.pio/build/teensy41-optical/firmware.{elf,hex}` is the `bcf2233` build (ITCM
190,764 B) and is the image to flash.

upstream repository root: the `git_modules/aius` pointer is bumped in the commit that carries this
document. Every other dirty path (`.claude/**`, `data/*.sqlite`, `docs/plans/almanac.md`,
other submodule pointers, untracked ``, …) is foreign. `the repo tooling test`
cannot compile the repo graph while the foreign untracked almanac tests exist, so vitest
was run directly (`pnpm exec vitest run <file>`).

### Next action

Ask the owner the two questions under §Pending decisions. If the flash is approved, at a
silent window: `firmware/scripts/flash_optical.sh` (default hex =
the `bcf2233` build; rebuild first with `firmware/scripts/build.sh teensy41-optical` if
`.pio/build` has moved), then read `/api/status` (`build_revision` should start
`bcf2233`, `tx_underrun_frames` 0 after the relock pad), re-run the carrier-less
two-PUT sequence from network-hardening defect 22 and confirm `staged_generation`
advances only on `200`, and run the 48 kHz smoke. Record the results in the register's
2026-09-02 remediation bullets and retire this document.

### Decisions taken this session

- **`GET /api/recorder` auth: the route is right, the rule was wrong.** `dispatch_recorder()`
  token-gates every recorder route including GETs, per the recorder contract (recorded
  audio + trigger state). `CLAUDE.md` says so (`f0a91e0`).
- **`dsp-config-default.json` keeps no runtime keys by design** — it is pinned
  byte-for-byte to `firmware/config/optical_dsp.json` by a test; the wire example that
  carries `staged_generation`/`config_pending` is `dsp-config-sd.json`.
- **`staged_generation` advances at SD commit**, not at PUT accept: a faulted write must
  not report a staging that never existed. Counter before the volatile apply flag, the
  order the SD-load stager already keeps. Semantics of `config_generation` untouched.
- **H1 (teardown lease release) accepted on its host proof**; provoking it needs a
  physical Ethernet unplug mid-pull and is not scheduled.
- **The optical-cutover OTA/recorder pin follows the pad-1 alias model** (`b76f8fc`,
  `optical_main.cpp` "ALIAS, DO NOT PARTITION"): staging over a parked ring, proven by
  ordering + the park assert, not by disjointness. This was the one semantic re-pin; it
  carries more checks than the disjointness pin it replaced.
- **Registry guard: repo-root suites are first-class**, named through `$aius_root/test/…`
  and existence-checked one level above the firmware tree; the sd-editor arm names its path.
- **ITCM pass scope: the three first-party libs only** (`aius_recorder`, `aius_auth`,
  `aius_config`). The bigger levers are ldscript-level and proposals (§Findings).

### Traps

- The optical-cutover suite reads firmware sources as TEXT. A rename or a new JSON key
  in `optical_main.cpp` fails it with a message naming the old token; the fix is to
  re-pin to the current fact, never to loosen (`test_optical_cutover.py` has the
  per-pin commit references). It also has self-test mutations anchored on exact
  indentation (`\n\t\tg_dsp_apply_pending = true;` binds the SD stager's two-tab arm).
- `firmware/scripts/build.sh teensy41-optical` is ~25 s incremental and exits 75 when
  another session holds the flock; never clear it. The memory report it prints is the
  ITCM proof; `python3 firmware/tools/memory/mem_report.py --elf …/firmware.elf`
  re-prints it without building.
- `CLAUDE.md` is path-guarded: write to scratch, `the release-promotion step`.
- The deployed board (`3570a216216d`) still bumps `staged_generation` at accept; the
  register and defect 22 (d) say so. A flash reboots the board and stops audio.
- No host suite compiles `optical_main.cpp`; the cross-build is the only automated
  proof for `sd_settle_http_write()` changes.

### Findings and loose ends

- **ITCM census (nm on the current ELF, 195500 B used):** newlib printf family
  (`_vfprintf_r` 8.6 KB, `_svfprintf_r` 8.3 KB, `_vfiprintf_r` 4.7 KB, `_dtoa_r` 3.6 KB)
  ≈ 25 KB; FNET + NativeEthernet ≈ 43 KB; SdFat/ExFat/FatFile ≈ 19 KB; AC-3 decoder
  (`a52_*`, `parse_frame_data`) ≈ 15 KB; DTS core ≈ 10 KB. Six 32 KiB blocks are
  claimed, so shedding ~32 KB would hand a whole block back to DTCM stack/locals.
  Proposed (not agreed): an ldscript rule in `firmware/link/aius_teensy41.ld` placing
  libc's printf objects (callers are all FLASHMEM serializers) and SdFat in `.flashmem`;
  the decoders and FNET stay pending a measured decode-budget / poll-latency check.
- `aius_codec_api` (`aius_arena.c`) and `aius_dts` stay in `ITCM_ALLOWED_LDF_LIBS`;
  `aius_dts` because its tree carries the vendored dcadec core, not a first-party
  annotation job.
- `aius-open-work-register.md` is 1646 lines; the 2026-08 defect narratives dominate.
  Proposed: leave — it is the authoritative home and the doc-index tolerates it.

### Pending decisions

- **Gate-4 pass (owner).** Exact words: *"A gate-4 pass is unreachable with the aplay
  stimulus because the U24 idles at ~48.02 kHz between rate phases and those idle gaps
  trip `transition-unattributed-unsupported` and the idle underruns. Do you want (a) a
  stimulus source that cuts the carrier cleanly between phases (programmable generator;
  hardware, no tool change), (b) the `transitions` analyzer taught the aplay-blip
  pattern — idle-gap `unsupported` episodes and their underruns declared as between-phase
  windows when they align with the stimulus timeline, thresholds untouched — or (c) leave
  gate 4 at its current honest FAIL-on-stimulus-grounds verdict?"* Recommendation: (b),
  because it is host-provable against the two saved evidence runs and the F8 poll-cadence
  note already treats `event_overflows` as tool-caused; (a) costs hardware, (c) leaves
  a gate that can never go green. Blocks: nothing else; the gate stays red until then.
- **Flash the new image?** `c34242e` (+ the ITCM pass when it lands) is cross-build
  proven only. Flashing is `firmware/scripts/flash_optical.sh` over USB CDC, reboots the
  board and stops all audio; sign without `--slot` (board is `ab: null`). Recommendation:
  flash at the next silent window and re-run the carrier-less two-PUT sequence
  (network-hardening defect 22) plus the 48 kHz smoke. Blocks: on-device closure of the
  `staged_generation` and ITCM items.
