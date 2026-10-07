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

- **Status**: Phase 1 DONE. Phases 1a, 2 (fonts), 3 (logos), 4 (limiter
  tool) and 5 (board limiter) not started. Phase 6 (deploy) BLOCKED on the
  user's word. Nothing from this plan is on the board.
- **What landed** (all pushed):

  | Commit | Contents | Proof |
  | --- | --- | --- |
  | aius `4b68103` | `firmware/sd/www/mixer.{html,css,js}`, `test/sd-editor/mixer.test.js`, this plan, plans README entry | `pnpm exec vitest run --root git_modules/aius` from the repository root: 6 files, 387 tests green (21 in `mixer.test.js`); Chromium probes via `the repo tooling tool playwright-inspect` at 1920, 1440 and 390 px (see Phase 1) |
  | upstream repository `8da5b5556` | `git_modules/aius` pointer to `4b68103`; also carries `a974465`, another session's pushed aius docs commit | none beyond the aius row |
  | aius, this edit | owner answers recorded, answered questions removed, state rewritten | `the repo tooling doc-index --validate-only` |

- **Uncommitted tree state**: nothing in this submodule. Two transient
  folders sit on the the host application preview mount, gitignored by design
  (`src/apps/preview/README.md`). The next agent can build on both:
  - `src/apps/preview/content/aius-mixer/`: the preview bundle, served at
    `https://apps.example.net/apps/preview/aius-mixer/mixer.html`. Its
    `_tools/` folder holds `build_preview.py`, `preview-shim.js`, the probe
    scenarios, and `fixtures/` captured read-only from the board.
  - `src/apps/preview/content/aius-logos/`: eight raster explorations, the
    gallery `index.html` and `_tools/generate_logos.py`.
- **Next action**: Phase 3, refinement round 1. Hand-draw SVG variants of the
  six-to-two mark into `src/apps/preview/content/aius-logos/svg/`, add them to
  a review page, and hand the URL to the user.

## Constraints

- **`dsp-editor.js` runs unchanged.** `mixer.html` carries every id
  `index.html` carries (asserted in `test/sd-editor/mixer.test.js`). Markup the
  skin does not show sits in `.bench-legacy[hidden]` blocks: the routing
  canvas and its captions, the view toggle, `#bars-input`.
- **`mixer.js` makes no request of its own.** The device serves one HTTP client
  at a time, and the editor's pollers carry everything shown. The adapter reads
  the editor's DOM and writes only through the editor's own `f:<path>` fields
  with an `input` or `change` event, the same path the routing canvas uses.
- **The page fetches nothing off the card.** Fonts and logo ship as card files
  (Phases 2 and 3), never from a CDN.
- **Device mutations wait for the owner's word.** Owner, 2026-10-07: deploy
  "when I tell you to". This covers the card sync (Phase 6) and the board
  config change (Phase 5).

## Phases

### Phase 1: Mixer dashboard page (DONE)

- outcome: `mixer.html` + `mixer.css` + `mixer.js`, a full-width 12-column
  dashboard of panels headed by kickers only (`00 / Signal` to `05 / Presets`),
  with no hero, section headings or footer.
- seams: `firmware/sd/www/mixer.{html,css,js}`. `replays.css` and
  `dsp-editor.css` load first and are re-tokened, not edited.
- targeted-proof: `test/sd-editor/mixer.test.js` (21 cases). Chromium probes
  checked:
  - no sideways scroll at 1920, 1440 and 390 px
  - a trusted click on an EQ tab
  - trusted typing into a linked FL trim, mirrored to FR
  - the token dialog opening on a trusted click and closing on Escape
  - `position: static` for the action bar at phone width

Settled, user-directed unless marked:

- **Matrix faders are C, F, S and LFE only.** F is a fixed, disabled 1.0
  reference because the firmware has no field for it. LFE crossover, surround
  delay and headroom live only in the folded "Exact values" form. The
  equation readout still shows them, and the clip-free chip still resolves
  headroom.
- **Gain faders are dB-tapered over the linear field** (own call). The bottom
  stop is 0 (off), then the scale runs from -40 dB to the field's 4.0
  maximum, rounded to the field's 0.01 step.
- **EQ tabs follow the plot's curves one to one**: FL+FR, C, LFE, SL+SR, OUT.
  A pair is bonded by default: one set of controls, and every edit (resets
  included) is copied left to right after the editor's own handler runs. A
  pair whose members differ after a device read or preset load opens unlinked
  rather than being overwritten. Linking copies left into right, and the bond
  note says so first.
- **Token entry lives in a `<dialog>`** opened from the lock chip. A save that
  locks the field closes it.
- **The action bar is sticky only above 720 px.**
- **The `00 / Signal` diagram is the only input-level display.** Its boxes
  mirror the editor's needles, wire widths follow the matrix weights, and the
  wires animate only while the transmit gate is open.
- **The verdict line states the worst-case in-phase overshoot** (own call):
  `headroom x (1 + centre + surround + |lfe|)`, and whether the limiter is on
  to catch it.
- **The preview runs on the host application with a fetch shim and captured fixtures**
  (own call). The gateway origin cannot reach `192.0.2.66`, and the shim keeps
  every write in the browser tab.

### Phase 1a: Doc touch-ups for a second SD page

- outcome: every doc that lists the SD pages names `mixer.html`.
- seams:
  - `firmware/sd/www/dsp-editor.js` banner says "OWNS: the whole
    /sd/www/index.html page"; it now also runs under `mixer.html`.
  - `CONTRIBUTING.md`, the SD pages list near line 70.
  - `CLAUDE.md` §Device access and OTA says "`aius-sd-sync.sh ...` (all 12
    files, unconditionally)". The card tree is now 15 files, 17 after Phase 2.
    CLAUDE.md is protected and changes only through `the release-promotion step`.
- targeted-proof: `the repo tooling doc-index --validate-only` clean.

### Phase 2: Vendor the fonts

Settled by the owner, 2026-10-07: yes, vendor them.

- outcome: local `@font-face` rules in `mixer.css` for Archivo (variable,
  `wdth` axis) and IBM Plex Mono, with woff2 files under
  `firmware/sd/www/fonts/`, about 150 KB.
- seams: `mixer.css`, `firmware/sd/www/fonts/`, and `THIRD-PARTY.md`. Both
  families are SIL OFL 1.1, so add their entries and ship the licence text
  beside the files.
- targeted-proof: a Chromium probe of the preview where
  `document.fonts.check('12px Archivo')` and
  `document.fonts.check('12px "IBM Plex Mono"')` are true, and the transcript
  shows no request leaving the page's origin. `build_preview.py` copies a
  fixed file list, so add `fonts/` to it.

### Phase 3: Six-to-two logo, refinement rounds

Settled by the owner, 2026-10-07: direction 1 (six-to-two) only, refined over
several rounds with the user's review between each.

- outcome: a hand-authored SVG on the bench tokens: six dots in a 3 by 2 grid
  converging to two, centre dot amber. It is reviewed at favicon (16 and
  32 px), nav (20 px tall) and large size, then replaces `.logo__dots` +
  `.logo__word` in `mixer.html` and becomes the page's favicon.
- seams: `src/apps/preview/content/aius-logos/svg/` and a review page there
  for each round; then `mixer.html`, `mixer.css` (`.logo`), and an SVG
  favicon file under `firmware/sd/www/`.
- targeted-proof: each round, a Chromium screenshot of the review page at
  1x and 2x DPR, read before handing the URL over. On adoption, the mixer
  suite still green.
- Round 1 offers 4 to 6 variants: dot size and spacing, curve tension, with
  and without the wordmark, and stroke versus filled outputs. Round 2 and later
  narrow on the user's picks; the user says when to stop.
- Source: `src/apps/preview/content/aius-logos/six-to-two.png` (gpt-image-2
  raster). Redraw it; do not trace it.

### Phase 4: A committed DSP-config setter with `--auth-file`

The owner chose to enable the board's limiter (Phase 5). No committed tool can
flip one config field with the token read from a file, and `CLAUDE.md` forbids
a literal token on a command line or a scratch-script workaround.

- outcome: a committed tool, either new as
  `firmware/tools/dsp_config/aius_dsp_set.py` or added to
  `firmware/tools/dsp_characterize/dsp_characterize.py`. It runs GET
  `/api/dsp/config`, sets the named dotted paths, then PUTs the result with
  the current `config_generation` and `X-Aius-SD-Auth` read from
  `--auth-file`. It refuses on 409 or 400.
- seams: `firmware/tools/`, and its usage line in `CONTRIBUTING.md`.
- targeted-proof: a host unit test of the body it builds (only the named
  paths change, the generation is carried), plus `--dry-run` printing the
  diff against a live GET.

### Phase 5: Enable the limiter on the board

Settled by the owner, 2026-10-07: enable the limiter. The live config read at
08:20 UTC had `features.limiter` false, with centre 1.5, surround 0.855,
LFE 0.316 and headroom 0.5. A worst-case in-phase sum overshoots full scale
by 5.3 dB. `limiter.enabled` was already true at -1 dBFS, 0.5 ms attack,
100 ms release and 2 ms lookahead.

- outcome: `features.limiter` true on the board; nothing else changed.
- seams: the board's `/aius/dsp.json`, through Phase 4's tool.
- targeted-proof: a GET `/api/dsp/config` showing `features.limiter: true` at
  a new `config_generation`, with every other field equal to the pre-change
  read.
- Runs only on the owner's word (see Constraints). It is an apply at a block
  boundary, not a card sync, but it still changes live audio.

### Phase 6: Deploy to the board

Settled by the owner, 2026-10-07: deploy only "when I tell you to". The
default-page question is decided at that time.

- **Interrogate the user: default page.** Ask this when the owner says to
  deploy, before uploading:
  - Question: "Should the mixer dashboard become the default DSP editor?"
  - **Yes, menus only**: `index.html`, `replays.html`, `status.html` and
    `upgrade.html` link "DSP editor" to `mixer.html`. The classic editor stays
    reachable from the mixer's nav.
  - **Yes, swap the files**: `mixer.html` becomes `index.html` and the classic
    page is renamed, so the device's root lands on the dashboard.
  - **No**: the mixer stays an opt-in page linked only from its own nav.
  - Recommendation: yes, menus only. It needs no rename and keeps the classic
    page one click away.
  - The answer sets the extra seams below; delete this step once it is
    recorded.
- outcome: the mixer files (plus the fonts, the favicon, and any menu edits
  from the answer) served from the card.
- seams: the card's `/sd/www/` tree, additive only. The answer may add the
  four SD pages' `<nav>` blocks, plus `test/sd-editor/*.test.js` and
  `firmware/test/optical-app/test_bitstream_ui.py` where they read
  `index.html`.
- targeted-proof: the sync prints `PUT /sd/www/<file>` per file with no retry
  failure, and a GET of `http://192.0.2.66/sd/www/mixer.html` returns the
  committed bytes. If menus changed, the vitest suite is green first.
- Card traffic during playback costs a short dropout (a 12-file sync once
  measured about 59 ms of `tx_underrun_frames`). State that cost when the
  owner gives the word.

```bash
firmware/tools/aius-sd-sync.sh --device http://192.0.2.66 \
  --www firmware/sd/www --token-file .secrets/aius-sd-auth \
  --file mixer.html --file mixer.css --file mixer.js
```

## Traps

- **happy-dom and Chromium disagree on text mutations.** happy-dom reports a
  `textContent` write that equals the old text; Chromium does not. An early
  version keyed the faders on `#model-bytes` text: it passed vitest but showed
  stale values in the browser. `mixer.js` now uses capture-phase
  `input`/`change` listeners plus a 200 ms signature check, with status-line
  observers only as hints.
- **Microtasks run between listeners for real user events.** A microtask
  queued in a capture listener runs before the field's own handler on a real
  keystroke, but after it on a scripted `dispatchEvent`. The sync and the pair
  mirror therefore use `setTimeout(0)`, and the suite's `settle()` flushes two
  timer turns. That lets the editor's needle ticker repaint during a test, so
  needle assertions compare against what the editor painted, not a hand-set
  width.
- **The preview is a copy and a snapshot.** `build_preview.py` copies the SD
  files into the preview mount, so rebuild it after any edit:
  `python3 -I src/apps/preview/content/aius-mixer/_tools/build_preview.py
  /other/upstream repository src/apps/preview/content/aius-mixer/_tools/fixtures
  2026-10-07T08:20Z`. Its fixtures are the board's state at 08:20 UTC on
  2026-10-07, and its meters loop 5 s of real readings.
- **`dsp-editor.css` scopes canvas sizing under `.visual`.** This page does
  not use that class, so `mixer.css` sizes `canvas.curve` and
  `canvas.gr__history` itself.
