---
title: Aius SD mixer dashboard
type: plans
scope: component
component: aius
status: in-progress
updated: 2026-10-07
toc: true
---

# Aius SD mixer dashboard

A second skin for the SD-hosted DSP editor: `firmware/sd/www/mixer.html`, a
full-viewport dashboard in the product site's bench palette (black, phosphor
amber for the voice, signal teal for the path). It runs the unchanged
`dsp-editor.js` and replaces the routing canvas with a fader mixer, the
signal-flow strip with a live routing diagram, and the dense channel EQ table
with bonded tabs. `index.html` stays as the classic editor.

## State: resume here

- **Status**: Phase 1 DONE (committed, not deployed). Phase 1a, the
  interrogate steps and their dependent phases are untouched. Nothing is on
  the board yet.
- **What landed**: the aius commit that adds this plan also adds
  `firmware/sd/www/mixer.html`, `mixer.css`, `mixer.js` and
  `test/sd-editor/mixer.test.js`; the upstream repository commit after it bumps the
  `git_modules/aius` pointer. Proof that ran before it: `pnpm exec vitest run
  --root git_modules/aius` from the repository root, 6 files and 387 tests green
  (21 in `mixer.test.js`); Chromium probes through `the repo tooling tool
  playwright-inspect` at 1920, 1440 and 390 px, covering no sideways scroll,
  a trusted click on an EQ tab, trusted typing into a linked FL trim
  mirroring to FR, the token dialog opening on a trusted click and closing on
  Escape, and the action bar going `position: static` at phone width.
- **Uncommitted tree state**: none in this submodule. Two transient preview
  folders live on the the host application preview mount, which is gitignored by design
  (`src/apps/preview/README.md`):
  - `src/apps/preview/content/aius-mixer/`: the preview bundle (served at
    `https://apps.example.net/apps/preview/aius-mixer/mixer.html`) and its
    `_tools/` folder: `build_preview.py`, `preview-shim.js`, the probe
    scenarios and `fixtures/` captured read-only from the board.
  - `src/apps/preview/content/aius-logos/`: eight raster logo explorations,
    the gallery `index.html` and `_tools/generate_logos.py`.
- **Next action**: run the Phase 2 interrogate step: ask the user whether and
  when to deploy (it opens with the exact question).

## Constraints

- **`dsp-editor.js` runs unchanged.** `mixer.html` carries every id
  `index.html` carries (asserted in `test/sd-editor/mixer.test.js`); markup the
  skin does not show sits in `.bench-legacy[hidden]` blocks (the routing
  canvas and its captions, the view toggle, `#bars-input`).
- **`mixer.js` makes no request of its own.** The device serves one HTTP client
  at a time and the editor's pollers carry everything shown; the adapter reads
  the editor's DOM and writes only through the editor's own `f:<path>` fields
  with an `input` or `change` event, the path the routing canvas uses.
- **No web fonts are fetched.** The card's page must not phone home; Archivo
  and IBM Plex Mono are used only when installed locally (Phase 6 decides
  whether to vendor them).

## Phases

### Phase 1: Mixer dashboard page (DONE)

- outcome: `mixer.html` + `mixer.css` + `mixer.js`, a full-width 12-column
  dashboard of panels headed by kickers only (`00 / Signal` to `05 / Presets`),
  no hero, section headings or footer.
- seams: `firmware/sd/www/mixer.{html,css,js}`; `replays.css` and
  `dsp-editor.css` are loaded first and re-tokened, not edited.
- targeted-proof: `test/sd-editor/mixer.test.js` (21 cases) plus the Chromium
  probes listed under State.

Decisions settled in this phase (user-directed unless marked):

- **Matrix faders are C, F, S and LFE only.** F is a fixed, disabled 1.0
  reference because the firmware has no field for it. LFE crossover, surround
  delay and headroom live only in the folded "Exact values" form (user: they
  "do not deserve sliders"); the equation readout still shows them, and the
  clip-free chip still resolves headroom.
- **Gain faders are dB-tapered over the linear field** (own call): bottom stop
  is 0 (off), then -40 dB up to the field's 4.0 maximum; values round to the
  field's 0.01 step.
- **EQ tabs follow the plot's curves one to one**: FL+FR, C, LFE, SL+SR, OUT.
  A pair is bonded by default: one set of controls, every edit (resets
  included) copied left to right after the editor's own handler runs. A pair
  whose members differ after a device read or preset load opens unlinked
  rather than being overwritten; linking copies left into right and the bond
  note says so first.
- **Token entry lives in a `<dialog>`** opened from the lock chip; a save that
  locks the field closes it.
- **The action bar is sticky only above 720 px.**
- **The `00 / Signal` diagram is the only input-level display**; its boxes
  mirror the editor's needles, wire widths follow the matrix weights, and the
  wires animate only while the transmit gate is open.
- **The verdict line states the worst-case in-phase overshoot** (own call):
  `headroom x (1 + centre + surround + |lfe|)`, and whether the limiter is on
  to catch it.
- **Preview runs on the host application with a fetch shim and captured fixtures** (own
  call): the gateway origin cannot reach `192.0.2.66`, and the shim keeps every
  write in the browser tab.

### Phase 1a: Doc touch-ups for a second SD page

- outcome: the docs that enumerate the SD pages name `mixer.html`.
- seams:
  - `firmware/sd/www/dsp-editor.js` banner: "OWNS: the whole
    /sd/www/index.html page" now also runs under `mixer.html`.
  - `CONTRIBUTING.md` (the SD pages list near line 70).
  - `CLAUDE.md` §Device access and OTA: "`aius-sd-sync.sh ...` (all 12
    files, unconditionally)" is now 15 files. CLAUDE.md is protected and
    changes only through `the release-promotion step`.
- targeted-proof: `the repo tooling doc-index --validate-only` clean.

### Phase 2: Interrogate the user: deploy and its timing

- outcome: the user's answer recorded in Phase 3; this step deleted.
- seams: none (a question).
- targeted-proof: Phase 3 carries no open question.

Ask: "The mixer dashboard is committed but not on the board. Upload
`mixer.html`, `mixer.css` and `mixer.js` now? The board had carrier and an open
gate when last read, so a card sync costs a short dropout (a 12-file sync once
measured about 59 ms of `tx_underrun_frames`)."

- **Now**: deploy immediately and accept the dropout.
- **At a named quiet time**: deploy when the user says audio is off.
- **Not yet**: more design changes first; Phase 3 waits.

Recommendation: at a quiet time. The page is additive and nothing links to it
from the other pages yet, so there is no rush worth a dropout. Blocks Phase 3.

### Phase 3: Deploy to the board

- outcome: the three files served from the card.
- seams: the card's `/sd/www/` tree; additive only.
- targeted-proof: the sync prints `PUT /sd/www/<file>` for each file with no
  retry failure, and a `GET` of `http://192.0.2.66/sd/www/mixer.html` returns
  the committed bytes.

```bash
firmware/tools/aius-sd-sync.sh --device http://192.0.2.66 \
  --www firmware/sd/www --token-file .secrets/aius-sd-auth \
  --file mixer.html --file mixer.css --file mixer.js
```

### Phase 4: Interrogate the user: default page

- outcome: the answer recorded in Phase 5; this step deleted.
- seams: none (a question).
- targeted-proof: Phase 5 carries no open question.

Ask: "Should the mixer dashboard become the default DSP editor, with the other
SD pages' menus pointing to it?"

- **Yes**: `index.html`, `replays.html`, `status.html` and `upgrade.html` link
  "DSP editor" to `mixer.html`; the classic editor stays reachable from the
  mixer's nav.
- **Yes, and swap the files**: `mixer.html` becomes `index.html` and the
  classic page is renamed, so the device's root lands on the dashboard.
- **No**: the mixer stays an opt-in page linked only from its own nav.

Recommendation: yes, menus only. It needs no rename and keeps the classic page
one click away. Blocks Phase 5.

### Phase 5: Make the mixer the default editor

- outcome: per the Phase 4 answer.
- seams: the four SD pages' `<nav>` blocks; `test/sd-editor/*.test.js` and
  `firmware/test/optical-app/test_bitstream_ui.py` where they read
  `index.html`.
- targeted-proof: `pnpm exec vitest run --root git_modules/aius` green, then a
  Phase 3-style sync of the touched pages.

### Phase 6: Interrogate the user: vendor the fonts

- outcome: the answer recorded in Phase 7; this step deleted.
- seams: none (a question).
- targeted-proof: Phase 7 carries no open question.

Ask: "Copy Archivo and IBM Plex Mono onto the card (about 150 KB of woff2) so
the dashboard looks like the product site? Without them it uses system fonts."

- **Yes**: Phase 7 adds the files and `@font-face` rules; the sync grows by
  two files.
- **No**: system fallbacks stay; Phase 7 is dropped.

Recommendation: yes. The condensed Archivo carries much of the look, and
150 KB is small against the card. Blocks Phase 7.

### Phase 7: Vendor the fonts

- outcome: local `@font-face` for Archivo (variable, `wdth`) and IBM Plex Mono
  in `mixer.css`, files under `firmware/sd/www/fonts/`.
- seams: `mixer.css`, `firmware/sd/www/fonts/`, and the sync's file list.
- targeted-proof: a Chromium probe where `document.fonts.check('12px Archivo')`
  is true and no request leaves the page's origin.

### Phase 8: Interrogate the user: logo directions

- outcome: the picks recorded in Phase 9; this step deleted.
- seams: none (a question).
- targeted-proof: Phase 9 names its directions.

Ask: "Which raster explorations should be redrawn as SVG? Gallery:
`https://apps.example.net/apps/preview/aius-logos/index.html`."

- **1 six-to-two**: six dots converging to two, amber centre. Closest to the
  current dot logo and works as a favicon.
- **4 dialogue-lift**: waveform bars with the middle band lifted in amber.
- **6 matrix-grid**: a node grid with an amber centre row.
- **3 routing-a**: an "a" drawn as merging signal lines, as an app icon.

Recommendation: 1, with 4 as the alternate. 2, 5, 7 and 8 picked up
unrequested face and speaker pictograms and fail at small sizes. Blocks
Phase 9.

### Phase 9: SVG logo redraws

- outcome: each picked direction as a hand-authored SVG on the bench tokens,
  shown at favicon, nav and large size on one preview page; the chosen one
  replaces `.logo__dots` in `mixer.html`.
- seams: `src/apps/preview/content/aius-logos/` for review; then `mixer.html`
  and `mixer.css` (`.logo`).
- targeted-proof: a Chromium screenshot of the preview page at 1x and 2x DPR.

### Phase 10: Interrogate the user: board limiter is off

- outcome: the answer recorded here and acted on through the editor; this step
  deleted.
- seams: the board's DSP config (`/aius/dsp.json`), not code.
- targeted-proof: the verdict line on the deployed dashboard reads clip-safe or
  "the limiter catches it".

Ask: "The board's live config has the limiter stage off (`features.limiter`
false) with centre 1.5, surround 0.855, LFE 0.316 and headroom 0.5. A
worst-case in-phase sum overshoots full scale by 5.3 dB. Change it?"

- **Enable the limiter**: tick the limiter's stage enable and Apply; peaks
  are caught at -1 dBFS true-peak.
- **Lower headroom**: about 0.27 makes the worst case clip-safe, at about
  5 dB less level.
- **Leave it**: the overshoot needs every channel in phase at full scale,
  which real programme rarely reaches.

Recommendation: enable the limiter. It costs no level on normal programme.
Blocks nothing above; it can be asked at any point.

## Traps

- **happy-dom and Chromium disagree on text mutations.** happy-dom reports a
  `textContent` write equal to the old text; Chromium does not. An early
  version keyed the faders on `#model-bytes` text and passed vitest but showed
  stale values in the browser. `mixer.js` now uses capture-phase
  `input`/`change` listeners plus a 200 ms signature check, with status-line
  observers only as hints.
- **Microtasks run between listeners for real user events.** A microtask
  queued in a capture listener runs before the field's own handler on a real
  keystroke but after it on a scripted `dispatchEvent`. The sync and the pair
  mirror use `setTimeout(0)`, and the suite's `settle()` flushes two timer
  turns. That lets the editor's needle ticker repaint during a test, so
  needle assertions compare against what the editor painted, not a hand-set
  width.
- **The preview is a copy and a snapshot.** `build_preview.py` copies the
  SD files into the preview mount, so rebuild it after any edit
  (`python3 -I src/apps/preview/content/aius-mixer/_tools/build_preview.py
  /other/upstream repository src/apps/preview/content/aius-mixer/_tools/fixtures
  2026-10-07T08:20Z`). Its fixtures are the board's state at 08:20 UTC on
  2026-10-07, and its meters loop 5 s of real readings.
- **`dsp-editor.css` scopes canvas sizing under `.visual`**, which this page
  does not use; `mixer.css` sizes `canvas.curve` and `canvas.gr__history`
  itself.
