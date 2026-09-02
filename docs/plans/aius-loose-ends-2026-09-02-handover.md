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
advancing at SD commit, ITCM placement pass (headroom 1,108 → 5,844 B), the flash
(`b38e8448f5c8` on the board, both firmware changes verified live), the gate-4
`transitions` analyzer adaptation (design decision (d); saved run 2 re-analyzes to one
real threshold failure). Nothing is in flight; one live step is pending the owner.

### What landed (all pushed)

| Commit | Contents | Proof |
|---|---|---|
| `git_modules/aius` `22fe1ae` | CONTRIBUTING.md paths swept to repository-relative; sd-editor paragraph names `test.sh --suite sd-editor`; recorder plan: auth note + H1 accepted, Phase D still IN-PROGRESS on three items; `dsp-config-sd.json` carries `staged_generation`/`config_pending` and the PUT-verbatim case strips them; optical-cutover re-pinned to the current firmware (micros watermark, recorder-bank rename, six clock keys, staged-aware origin, per-slot recorder array, OTA pad-1 alias model); registry guard models repo-root suites via `$aius_root`, sd-editor catalogued | sd-editor 359 green; optical-cutover 33/1/1/1/6 green; registry green (60 arms); shellcheck clean |
| `git_modules/aius` `c34242e` | `g_staged_generation++` moved from `app_store_dsp_config()` (accept) to `sd_settle_http_write()` (commit, before arming `g_dsp_apply_pending`); serializer comment; network-hardening defect 22 correction (d); register strikes | `teensy41-optical` cross-build SUCCESS, ITCM 195500 / 1108 free unchanged |
| `git_modules/aius` `f0a91e0` | `CLAUDE.md`: reads need nothing except the five `/api/recorder*` routes, token-gated on read by contract (promoted through `the maintainer`, operator-approved) | docs only |
| `git_modules/aius` `bcf2233` | every definition in `aius_recorder.c`, `aius_recorder_bank.c`, `aius_config.c`, `aius_auth.c` is `FLASHMEM` (51 functions); the three libraries left `ITCM_ALLOWED_LDF_LIBS` and are gated to zero `.text`; the named recorder-helper guard retired (five guards); README/CONTRIBUTING/test.sh wording; register: item struck, nm census + ldscript proposal recorded | `teensy41-optical` cross-build SUCCESS, ITCM 195,500 → 190,764 B; itcm-placement (29 unit + target), memory, recorder, auth, config, optical-cutover, registry green |
| upstream repository `b91a507fc` | pointer bump; `swagger.json` `staged_generation` / `dsp_staged_generation` descriptions say "committed" | `swagger.test.js` 11 green; prettier clean |

### Uncommitted tree state

`git_modules/aius`: clean. `CLAUDE.md`'s deployed-image line (`b38e8448f5c8`) is a
queued `the release-promotion step` if `git -C git_modules/aius status --short CLAUDE.md` is clean;
dirty means it landed and wants a one-path `docs(aius)` commit.

upstream repository root: the `git_modules/aius` pointer is bumped in the commit that carries this
document. Every other dirty path (`.claude/**`, `data/*.sqlite`, `docs/plans/almanac.md`,
other submodule pointers, untracked ``, …) is foreign. `the repo tooling test`
cannot compile the repo graph while the foreign untracked almanac tests exist, so vitest
was run directly (`pnpm exec vitest run <file>`).

### Next action

The one remaining live step: a gate-4 run with `--with-replay` on `b38e8448f5c8`
(`firmware/tools/rate_gate_capture/recorder_transitions.py`, `--phase-seconds 2`; U24
de-auth + replay start, so an audio interruption the owner must accept first), then
`aius_rate_bench.py analyze transitions --evidence <run>/evidence.jsonl` (the driven
sequence is in the metadata). Watch `transition_reset_deltas`: run 2 read `[2, 1]`
offline (two service resets in the first blip), and whether that recurs decides if it
is a stimulus property worth a declared allowance or a board question. Record in
`aius-rate-gate-capture-tool.md` §Live-run results and retire this document.

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
- **Owner, 2026-09-02 ~23:00 UTC: flash now** — done, `b38e8448f5c8` (HEAD rebuild of
  the `bcf2233` code so the revision stamp is clean); both firmware changes verified
  live (register bullets, network-hardening defect 22 (d)).
- **Owner, same ask: gate 4 → adapt the `transitions` analyzer to the aplay-blip
  stimulus, thresholds untouched** (ruling (d) in `aius-rate-gate-capture-tool.md`).
  Shape: live windows derived from the observed labels instead of the positional
  "last window is replay" rule (a real bug, F13 — it produced run 2's false
  `transition-unattributed-unsupported`); the analyzer judges the phase sequence the
  capture tool declares it drove (`expected_phases`, recorded in the evidence
  metadata) instead of asserting a replay leg that was not played; `event_overflows`
  reported rather than gated in `transitions` only (F8: an events-ring overflow
  between two polls is a tool-observability loss; `unsupported-event-count` stays as
  the guard). No threshold changes.

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
- **`transition-reset-count [2, 1]` on the saved gate-4 run 2.** The first aplay blip
  advanced `audio_diag.service_requested_resets` twice inside one transition window
  (stream → `unsupported` at 2.10 s, then `stream_rate_hz 0` at 3.15 s); the second
  window polled no carrier-gone sample and advanced once; the earlier `ce84850` run read
  `[1, 1]`. Proposed (not agreed): leave the one-reset threshold; let the live
  `--with-replay` run say whether two edges per blip is the stimulus's shape, and only
  then decide whether the declared stimulus may carry an allowance.
- `final_replay_output_blocks_delta` (bench metric) keeps its name although the final
  phase is no longer necessarily `replay`; renaming churns a published evidence field.
  Proposed: leave.
- `aius-open-work-register.md` is 1646 lines; the 2026-08 defect narratives dominate.
  Proposed: leave — it is the authoritative home and the doc-index tolerates it.

### Pending decisions

- **Live gate-4 re-run with `--with-replay` on `b38e8448f5c8`.** Not yet asked. Exact
  words: *"The adapted analyzer passes the saved run offline; a live `--with-replay`
  gate-4 run would give the first real green and costs a U24 de-auth plus a replay start
  (audio stops for the run, ~30 s). Run it now, or at a silent window?"* Options: now
  (closes gate 4 tonight) / later (nothing else blocks). Recommendation: at the next
  silent window. Blocks: retiring this document.
