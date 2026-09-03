---
title: Aius gate-4 replay leg and S/PDIF receive fix (2026-09-03) — handover
type: plans
scope: component
component: aius
status: in-progress
subtype: how-to
tags: [aius, firmware, handover, rate-gate, spdif-rx, replay]
updated: 2026-09-03
toc: true
---

# Aius gate-4 replay leg and S/PDIF receive fix (2026-09-03) — handover

**Transient baton, not a record.** Delete this document in the commit that lands the
last item under §Next action and §Pending decisions, after promoting anything durable
per the handover skill's retirement rule. Resumers: check §What landed against
`git -C git_modules/aius log` first; anything after it is invisible here.

Two sessions on 2026-09-03. The first ran the live gate-4 `--with-replay` leg five
times, fixed the RX pool corruption and the redundant acceptance reset, and reached the
first green gate 4. The second fixed the one defect that made gate 4 flake (the
replay-start open-gate underrun) and proved it live three times. The durable homes are
[`aius-open-work-register.md`](aius-open-work-register.md) (§Open defects — runtime),
[`aius-rate-gate-capture-tool.md`](aius-rate-gate-capture-tool.md) (§Live-run results —
2026-09-03, both sections) and
[`aius-output-jitter.md`](aius-output-jitter.md) (Phase F amendment).

## State — resume here

**Status.** DONE: replay gate-open floor (`tx_gate_execute_open(bool replay)` pads a
replay's open to 768 frames, sized to the SD refill's `OPEN_READ`), its cutover pin,
swagger field, three live runs (zero underruns across three replay starts, runs 2–3
`verdict pass`), docs, clean-stamp flash. IN FLIGHT: nothing. OPEN: one queued
`CLAUDE.md` promote; the `au_abandons` runtime defect (recorded, not started); one tool
marginality finding (§Findings).

### What landed (all pushed)

| Commit | Contents | Proof |
|---|---|---|
| `git_modules/aius` `baef793` … `b977488` (session 1) | RX guarded stager, `input_diag` pool instruments, acceptance reset gated on ingest, tool poller stop, register + tool plan | in the register / tool plan; gate 4 green run 5 (`0cf5eeda…`) |
| `git_modules/aius` `4c82434` | `CLAUDE.md` deployed-image line `b977488b3080` (landed promote) | docs only |
| `git_modules/aius` `4c9eb3d` | `AIUS_TX_GATE_OPEN_REPLAY_TARGET_FRAMES` (768, `platformio.ini`; `#error` below the live target); `tx_gate_execute_open(bool replay)` selects the floor from `enqueue()`'s `replay` fact; `clock.gate_open_replay_target_frames`; cutover pin `assert_replay_gate_open_pads_to_its_own_floor` + three mutations; register entry FIXED; tool plan §Live-run results — 2026-09-03, replay floor; output-jitter Phase F amendment | optical-cutover 34/1/1/2/6; `teensy41-optical` SUCCESS, ITCM 190,956 / 5,652 free (unchanged); live runs on `4c82434311f0`: `tx_underrun_frames` 0 lifetime across three replay starts, pad +768 each, ring 1024–1280 through replay; run 1 FAIL on the 44.1 kHz acceptance latency only, runs 2–3 PASS `[1,1,1]` |
| upstream repository `a9b574b7c`, `75f44a40a` | pointer bumps; `75f44a40a` also `swagger.json` `Clock.gate_open_replay_target_frames` | `swagger.test.js` 11 green; prettier clean |

**Deployed image: `4c9eb3d497ac+teensy41-optical`** (flashed 2026-09-03 10:37 UTC from
the committed tree; healthy on boot: PCM tracking at 48 kHz, gate open, `overruns 0`,
`tx_underrun_frames 0`, `gate_open_replay_target_frames 768`, pool 2 / 4).

### Uncommitted tree state

`git_modules/aius`: clean. A `the release-promotion step` of `CLAUDE.md` is queued (deployed-image
line → `4c9eb3d497ac`, plus one sentence after the Phase-F relock note saying a replay
start pads +768): `git -C git_modules/aius status --short CLAUDE.md` clean means still
queued, dirty means it landed and wants a one-path `docs(aius)` commit plus a upstream repository
pointer bump. Do not restate the approval; check.

upstream repository root: every dirty path (`.claude/**`, `data/*.sqlite`, `docs/plans/almanac.md`,
`docs/plans/deploy-backlog-and-red-suite.md`, `scripts/CLAUDE.md`, `src/apps/tiro/CLAUDE.md`,
other submodule pointers, untracked ``, `.codex/`, …) is foreign.
`the repo tooling test` cannot compile the repo graph while the foreign untracked almanac
tests exist; run vitest directly (`pnpm exec vitest run test/swagger.test.js`).

Evidence for the three replay-floor runs lives in session scratch
(`.scratch/c520374c-a5ad-4092-b445-d613091e8422/gate4-replay-floor-run{1,2,3}/evidence.jsonl`,
digests in the tool plan) and is subject to the 7-day scratch GC; the tone fixtures are
beside them.

### Next action

1. `git -C git_modules/aius status --short CLAUDE.md`; if dirty, commit it alone
   (`git -C git_modules/aius commit -F <msg> -- CLAUDE.md`), push, bump the pointer.
2. Then whichever the owner picks (§Pending decisions). For `au_abandons`, start at
   `drop_staged_compressed_input()` (`firmware/lib/aius_pipeline/src/aius_pipeline.c:701`)
   and the carrier-return path in `replay_source_service()` (`firmware/src/optical_main.cpp`,
   `REPLAY_STOP_CARRIER`): the counter reads +1 per carrier return during a PCM replay
   (`pipeline_last_failure 3` MALFORMED), now at 3 on the board after the three runs.
   Reproduction: any `--with-replay` gate-4 run, then read `audio_diag.au_abandons` from
   `/api/status` after the tool restores the U24 (it is outside the evidence by design).

### Decisions taken this session

- **The gate-open floor is per source, and a replay's is sized to the SD primitive, not
  to latency.** Why: the live 128-frame floor covers epoch jitter of a producer that never
  blocks; a replay's producer is the refill job whose `OPEN_READ` / `READ` primitives
  (8.3 / 6.3 ms measured) block checkpoint 7 two checkpoints before AUDIO, and pad +
  first block = 5.3 ms lost that race 1 run in 3. Replay has no lip-sync, so 768 frames
  (16 ms, ~2x the longest primitive) costs nothing audible. The live target and the held
  governor's policy are untouched; the `#error` guard keeps the replay floor ≥ the live one.
- **The floor is chosen at the same pre-commit instant, from `enqueue()`'s own `replay`
  fact**, not from checkpoint 7's view — a replay→live relock still trims back to 128.
- **Serve the constant (`gate_open_replay_target_frames`)** so a +768 in
  `gate_open_padded_frames` at replay start reads as designed, not as a fault.
- **Run 1's failure is the tool's, not the board's.** The board accepted 44.1 kHz 1.8 s
  after the switch (run 5: 1.05 s), leaving one sample where the analyzer wants two; the
  replay window of that run was clean. Recorded in the tool plan as a marginality finding,
  no threshold weakened, no re-run counted as a pass.
- Session-1 decisions (owner: bench needs no audio-interruption ask; ruling (b) on the
  acceptance reset; RX fix at the receive ISR; evidence ends with the declared stimulus;
  instruments over inference; clean-stamp reflash after each firmware commit) are already
  in `CLAUDE.md` and the register.

### Traps

- The optical-cutover suite reads firmware sources as TEXT; re-pin to the current fact,
  never loosen. `function_body(text, marker)` requires a UNIQUE marker: a forward-declared
  function (`tx_gate_execute_open`, `tx_gate_actuate`) appears twice, so the new pin
  slices on `"…(bool replay)\n"` — the trailing newline excludes the `;`-terminated
  declaration. The self-test fixtures must reproduce that exact newline.
- The 12-char `build_revision` is the HEAD commit with no dirty marker: a dirty-tree build
  stamps the previous commit (the live runs ran on `4c82434311f0`, the fix's parent), so
  `--expect-build` proves the flash, not the content. Clean-stamp reflash after committing.
- The tool's 2 s phase is marginal against the board's 44.1 kHz acceptance latency
  (1.05–1.8 s observed): a run reading `observed ['48000', 'replay']` with
  `carrier_rate_millihz` at ~44118 kHz through the phase is this, not a board fault. 2 s is
  the longest phase the 262144-frame slot admits without `--allow-rolloff`.
- `firmware/scripts/build.sh teensy41-optical` is ~25 s and exits 75 when another session
  holds the flock; never clear it. Its memory report is the ITCM proof (headroom 5,652 B).
- `CLAUDE.md` is path-guarded: write to scratch, `the release-promotion step`; the
  promote CONSUMES the scratch file, so re-copy before a second edit.
- The tool's U24 re-auth resets the mixer to 70,70: `amixer -c 0 cset numid=5 110,110`
  after every run. The tool also PUTs the recorder config back (an SD write).
- A status read issued in the same turn as a background capture can land after the
  capture; read the evidence samples, not a concurrent `/api/status`, for run facts.
- No host suite compiles `optical_main.cpp`; the cross-build is its automated proof.
- `rx_dropped_halves` reads 1 after every boot and climbs by thousands during any replay
  with the carrier down; that is the receiver storm being dropped, not a fault.
- `au_abandons` and `gate_open_padded_frames` are lifetime counters: after N replay runs
  without a reboot they read N and 128 + 768·N (+128 per live relock).

### Findings and loose ends

All recorded in the register or the tool plan; listed here so nothing is inherited as a
commitment:

- **`au_abandons` +1 at every carrier return during replay** (`aius_pipeline.c:701`,
  `drop_staged_compressed_input()`): a PCM replay should have nothing staged. Proposed
  (not agreed): one look, likely a false MALFORMED latch on the replay→live discontinuity.
- **Tool: the 2 s phase is marginal against 44.1 kHz acceptance latency** (tool plan
  §Live-run results — 2026-09-03, replay floor). Proposed (not agreed): the ring-capacity
  check counts `phase × 2 + 1 s` but only the FIRST phase's last block is needed, so an
  asymmetric stimulus (short first 48 kHz phase, 3 s 44.1 kHz phase) fits the slot without
  `--allow-rolloff`; alternatively accept the flake as a retry. No threshold change.
- **Unlocked-receiver DMA storm** (~2,460 ISR entries/s while dark). Harmless now.
  Proposed: gate the receive DMA request while the output drives the graph.
- ITCM census / ldscript proposal, `final_replay_output_blocks_delta` naming, register
  length: unchanged proposals from 2026-09-02, in the register.

### Pending decisions

- **Which next?** Exact words: *"Gate 4 is now repeatable on the board side (three replay
  starts, zero underruns). Look at the `au_abandons` latch at carrier return next, fix the
  tool's marginal 2 s phase, or park both?"* Options: `au_abandons` (small pipeline
  investigation, cross-build + one live run as proof) / tool phase asymmetry (Python-only,
  offline re-analysis + one live run) / park. Recommendation: `au_abandons` — it is a
  counter that reads as a board fault to anyone not carrying this context, and the tool
  flake is a retry. Blocks: nothing but the next phase; this document retires once both
  are dispositioned or the owner parks them.
