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

- **Posture**: checkpoint (owner, 2026-10-07): surface each decision and
  human-review gate as it arises.
- **Status**: Phases 1, 1a, 1b, 1c, 2 and 4 shipped; 1b and 1c await the
  owner's phone check of the preview. Phase 3 round 1 is delivered and waits on the
  owner's picks. Phases 5 (board limiter) and 6 (deploy) are
  BLOCKED on the user's word. Nothing from this plan is on the board.
- **What landed** (all pushed):

  | Commit | Contents | Proof |
  | --- | --- | --- |
  | aius `4b68103` | `firmware/sd/www/mixer.{html,css,js}`, `test/sd-editor/mixer.test.js`, this plan, plans README entry | `pnpm exec vitest run --root git_modules/aius` from the repository root: 6 files, 387 tests green (21 in `mixer.test.js`); Chromium probes via `the repo tooling tool playwright-inspect` at 1920, 1440 and 390 px (see Phase 1) |
  | upstream repository `8da5b5556` | `git_modules/aius` pointer to `4b68103`; also carries `a974465`, another session's pushed aius docs commit | none beyond the aius row |
  | aius `63b7ff8` | owner answers recorded, answered questions removed, state rewritten | `the repo tooling doc-index --validate-only` |
  | aius `6186506` | `firmware/sd/www/fonts/` (Fontsource 5.3.0 woff2, Latin, 135 KB, plus OFL texts), `@font-face` in `mixer.css`, `THIRD-PARTY.md` entries | vitest 6 files, 387 tests; Chromium probe of the preview: both `document.fonts.check` true, 0 off-origin requests |
  | aius, Phases 1a and 4 | `firmware/tools/dsp_config/aius_dsp_set.py` with its test, fixture and `test.sh` wiring; SD page docs (`CONTRIBUTING.md`, `dsp-editor.js` banner, `docs/reference/user-interface.md`, `CLAUDE.md` through `the release-promotion step`, `THIRD-PARTY.md`); register entry for the float-drift defect | `test.sh --environment native --suite dsp-config` green (26 setter tests); live `--dry-run` printed only `features.limiter: false -> true`; vitest 6 files, 387 tests; doc-index clean |

- **Uncommitted tree state**: nothing of this plan's in this submodule. Two transient
  folders sit on the the host application preview mount, gitignored by design
  (`src/apps/preview/README.md`). The next agent can build on both:
  - `src/apps/preview/content/aius-mixer/`: the preview bundle, served at
    `https://apps.example.net/apps/preview/aius-mixer/mixer.html`. Its
    `_tools/` folder holds `build_preview.py`, `preview-shim.js`, the probe
    scenarios, and `fixtures/` captured read-only from the board.
  - `src/apps/preview/content/aius-logos/`: eight raster explorations, the
    gallery `index.html` and `_tools/generate_logos.py`; round 1's SVGs in
    `svg/` and its review page `round-1.html`.
- **Next action**: Phase 3 round 2, once the owner gives picks and answers
  the two round-2 questions in the Phase 3 block. Phase 5 is ready and runs
  on the owner's word, ideally with a carrier present.

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
- **The board is shared with the sample-rate-following work**
  ([`aius-sample-rate-following.md`](aius-sample-rate-following.md)), whose
  device sessions (OTAs, source-off checks) also wait for the owner. Before
  Phase 5 or 6 touches the board, read `/api/status` `build_revision` and
  that plan's State block, and confirm with the owner that no run of theirs
  is in progress: a config apply or card sync during one perturbs its
  measurements.

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

### Phase 1a: Doc touch-ups for a second SD page ✓ SHIPPED

- outcome: every doc that lists the SD pages names `mixer.html`.
- seams:
  - `firmware/sd/www/dsp-editor.js` banner says "OWNS: the whole
    /sd/www/index.html page"; it now also runs under `mixer.html`.
  - `CONTRIBUTING.md`, the SD pages list near line 70.
  - `CLAUDE.md` §Device access and OTA says "`aius-sd-sync.sh ...` (all 12
    files, unconditionally)". The card tree grew by the mixer's 3 files,
    Phase 2's 6 (`fonts/`: 4 woff2 plus 2 OFL texts) and Phase 3's favicon;
    `aius-sd-sync.sh` uploads every file under `www/` (`find -type f`), so
    drop the count rather than restate it. CLAUDE.md is protected and changes
    only through `the release-promotion step`.
  - `THIRD-PARTY.md`: the closing sentence "Each vendored tree keeps its
    upstream licence file..." now follows Phase 2's fonts table; move it back
    under the components table it describes.
- targeted-proof: `the repo tooling doc-index --validate-only` clean.
- Shipped: all three seams, plus `docs/reference/user-interface.md`'s URL
  list. `CLAUDE.md` landed through `the release-promotion step` with the owner's
  approval. Left as written on purpose: `docs/plans/aius-sd-config-web.md:62`
  (a historical page list) and `CLAUDE.md`'s "12-file sync measured ~59 ms"
  (a dated measurement).

### Phase 1b: Matrix strip and touch-first steppers ✓ SHIPPED

Owner, 2026-10-07: LFE crossover, headroom and surround delay must be
visible, as number inputs like the EQ ones, not sliders. Number inputs must
be touch friendly, with larger increment and decrement controls packed for a
touch-first interface. Placement and stepper layout were chosen by the
owner; the rest are own calls.

- outcome:
  - **Matrix strip** (owner): a row of three steppers inside the matrix
    panel, under the C/F/S/LFE faders: crossover Hz, headroom, surround
    delay ms. Each writes through the editor's own `f:matrix.*` field with
    an `input` or `change` event (Constraints). The folded "Exact values"
    form stays, for the gains. While clip-free is on, the headroom stepper is
    disabled and shows the resolved value, matching the clip-free chip (own
    call).
  - **Stepper** (owner: value, then a minus and a plus button on the right)
    for every number input on the mixer page: the EQ tabs, the matrix strip
    and the Exact values form. Own calls:
    - each button is at least 44 by 44 px, and the value is typeable with
      `inputmode="decimal"`;
    - the browser's own spinners are hidden;
    - each press steps by the field's `step` and clamps to its `min` and
      `max`;
    - holding a button repeats, after 400 ms and then every 80 ms; it fires
      `input` per step and one `change` on release;
    - buttons are `type="button"`, `aria-label`led, and excluded from
      the tab order (the input keeps it).
  - **Packing**: the EQ controls reflow so the steppers fit without sideways
    scroll. At phone width each band becomes one row of labelled steppers
    rather than a dense table row.
  - `index.html` (the classic editor) is untouched; the steppers live only
    in `mixer.js` and `mixer.css`.
- seams: `firmware/sd/www/mixer.{html,css,js}`, `test/sd-editor/mixer.test.js`,
  and the preview bundle (`src/apps/preview/content/aius-mixer/`).
  `dsp-editor.js` runs unchanged.
- targeted-proof:
  - `mixer.test.js` cases:
    - the strip writes each `f:matrix.*` field;
    - a step clamps at `max`;
    - a hold repeats and fires one `change`;
    - a linked EQ pair mirrors a stepper press;
    - headroom is disabled under clip-free.
  - Chromium probes of the preview at 1920, 1440 and 390 px, with touch
    emulation at 390:
    - no sideways scroll;
    - every stepper button measures at least 44 by 44 px;
    - a trusted tap on plus changes the field;
    - a press-and-hold on minus steps more than once.
- Built (own calls, recorded at delivery):
  - One `stepper(input)` helper in `mixer.js` decorates every
    `input[type=number]` outside `.bench-legacy`, including the limiter form
    and `#curve-q`. It adds the buttons as siblings rather than wrapping the
    input, because `dsp-editor.js` `validateField` finds `.field__error`
    through `input.parentElement`.
  - The strip's mirror inputs forward `input`/`change` to the `f:matrix.*`
    fields, and follow an eventless rewrite of them.
  - The strip is a full-width row below the faders and readout, because the
    fader column cannot hold three steppers.
  - EQ bands are an auto-fill grid (`minmax(150px, 1fr)`, gap `12px 16px`),
    2 steppers per row at phone width, because three 44 px steppers do not
    fit in 350 px. Column headers become per-cell labels, and "Band N"
    replaces the bare number.
  - Under clip-free the headroom stepper reads "resolved by clip-free".
- Proof gap: `playwright-inspect` cannot emulate touch (no `hasTouch` or
  `isMobile` on its `BrowserNewContextOptions`,
  `src/tools/playwright-inspect/src/playwright-inspect/Program.cs:161`), so
  the 390 px tap and hold used trusted mouse input. The touch-only path
  (`pointerdown` with capture, `contextmenu` suppressed,
  `touch-action: manipulation`) is proven in vitest only; the owner's
  phone check of the preview is the real-touch proof, and its feedback
  becomes new work here.

### Phase 1c: Control consistency pass ✓ SHIPPED

Owner review of the Phase 1b preview, 2026-10-07:

- The matrix strip's three steppers stack inside one strip item, grouped
  the way the EQ bands group. The strip notes are dropped as redundant,
  and the wasted whitespace goes with them.
- Every reset button reads `RESET`, matches the height of its neighbours,
  and sits in the same place throughout. An EQ band's reset sits top right,
  in the row it shares with the band label. The limiter resets follow the
  same rule.
- The EQ type select matches the height of the inputs beside it.
- Every pill and button is squarer.
- Buttons and status pills must not be told apart only by their (uneven)
  height.

Own calls:

- Buttons and status pills share a 4 px radius.
- A button gets a solid border, an uppercase label and a pressed state, at
  the control height (44 px).
- A status pill gets no border, a tinted fill, a leading status dot, a
  fixed smaller height and no pointer cursor.
- `RESET` is `text-transform` in `mixer.css`; the label text in
  `dsp-editor.js` stays as is.

```yaml
outcome: matrix strip is one grouped item with no notes; every RESET is uppercase, control-height and consistently placed (EQ band and limiter resets top right in the label row); EQ type select matches input height; 4 px radius on buttons and pills; buttons and status pills distinguished by border, fill, dot and cursor, not height
seams: [firmware/sd/www/mixer.css, firmware/sd/www/mixer.js, firmware/sd/www/mixer.html, src/apps/preview/content/aius-mixer/]
targeted-proof: vitest (root git_modules/aius) green with test/sd-editor/mixer.test.js unedited (another session owns it from 2026-10-07); Chromium probe of the preview at 1440 and 390 px measuring every RESET button's height against its row neighbours, the EQ type select's height against the inputs, every button and pill border-radius at most 4 px, the band reset's top-right position, and no sideways scroll; viewport screenshots read at both widths
```

- `dsp-editor.js`, `index.html` and `test/sd-editor/mixer.test.js` stay
  untouched. If a mixer test breaks on a DOM change, change the approach
  rather than the test, or stop and report.
- Built (own calls, recorded at delivery):
  - New tokens in `mixer.css`: `--bench-radius-ctl` (4 px),
    `--bench-control-h` (44 px), `--bench-pill-h` (24 px) and `--bench-dot`.
    No `999px` radius remains, and inputs and selects take the 4 px radius
    too.
  - One button rule covers the action bar, the dialog, the curve, preset
    and stale-reload buttons, the steppers, the chips (toggle buttons), the
    disclosure `summary` elements, and the tabs (flat, 44 px).
  - Pills (apply, dirty and strip state, the lock chip, `.flow-end`) take
    their tone from `currentColor`, so the `data-state` colours carry over.
    The lock chip keeps the pointer cursor, because it opens the token
    dialog.
  - Reset placement:
    - `mixer.js` `labelEqCells()` moves each EQ band's reset to follow its
      `.eq-table__sec` label, so the two share a row; ids are kept.
    - A trim reset closes the value, minus and plus row.
    - Stacked-field and checkbox resets sit top right in the label row.
    - Stage-head resets have no row neighbour.
  - The matrix strip is one `.strip__item` of three `.strip__cell` steppers,
    one per row at 390 px. The clip-free explanation moved to a `title`.
  - The probe scenario
    `src/apps/preview/content/aius-mixer/_tools/consistency-probe.json`
    reads the `.field__reset` ids, so it needs updating if `dsp-editor.js`
    renames them.

### Phase 1d: Trim label and bypass-note reflow ✓ SHIPPED

The owner reported on 2026-10-07 that each EQ tab carries an unlabelled
"gain dB" input, and that the "EQ bypassed" note reflows the panel.

- The input is the per-channel trim, `input_channels.<ch>.gain_db`, from
  -60 to +12 dB. It is a flat gain applied before that channel's EQ biquads
  and the downmix (`firmware/lib/aius_dsp/src/aius_dsp.c:574-585`). Its
  "Trim (dB)" head was hidden by Phase 1c's `.bench .eq-table__h` rule.
- The note is an always-present empty span that `refreshStageInertness`
  fills on bypass, with no reserved space.

```yaml
outcome: the trim reads "Trim dB" on every EQ tab; a stage bypass changes neither the input card's box nor the action bar's height
seams: [firmware/sd/www/mixer.css, firmware/sd/www/mixer.js, src/apps/preview/content/aius-mixer/]
targeted-proof: vitest (root git_modules/aius) green; Chromium probes trim-note-probe.json (1440) and trim-note-phone-probe.json (390) show the trim label box non-zero on four tabs and the card box, first-stepper offset and action-bar height unchanged across a trusted bypass click
```

- Built:
  - `.bench .tabs__trim > .eq-table__h` shows the head.
  - `.bench .stage-head__note` holds a fixed two-line band (`flex: 0 0 100%`,
    `height: 2.6em`).
  - `.bench .action-bar__state > .pill[hidden]` keeps its box with
    `visibility: hidden`. The 22 px growth at 390 px came from
    `#dirty-state` appearing, not from the note.
  - At 390 px the action bar stays at 150 px and the card top stays at
    2541 across the bypass.

### Phase 1e: Bypass note truth and trim on the EQ curve

Owner, 2026-10-07:

- **Bypass skips trim.** With `features.input_eq` off, the firmware skips
  the whole input stage, trim included
  (`firmware/lib/aius_dsp_runtime_config/src/aius_dsp_runtime_config.c:119-124`).
  The firmware is right; the note is wrong. Change the `input_eq`
  `offNote` (`firmware/sd/www/dsp-editor.js:188-190`) to "EQ and trim
  bypassed". Also fix the stale "+-24" gain-bound comment at
  `dsp-editor.js:61`; the `gain_db` bound is +-12.
- **The curve shows trim.** Each channel's EQ curve is offset by its trim,
  so the plot shows the total level. It reads `input_channels.<ch>.gain_db`
  beside the sections in `curveSections` / `entityCascade`
  (`dsp-editor.js:2784-2797`). A bonded pair draws each member's own
  offset. This changes the classic `index.html` curve too.

```yaml
outcome: the input_eq bypass note says "EQ and trim bypassed"; each channel's EQ curve sits at its trim offset on both SD pages
seams: [firmware/sd/www/dsp-editor.js, test/sd-editor/dsp-editor.test.js]
targeted-proof: dsp-editor.test.js cases for the new note text and for a curve point shifting by exactly the trim dB; Chromium probe of the mixer preview showing the curve move on a trim step; vitest green
```

- **Blocked:** `dsp-editor.js` and its test belong to the
  sample-rate-following session (path-ownership write gate). Start once
  that session commits and releases them; check with `hook-admin
  ownership list`.

### Phase 1f: BYPASSED pill ✓ SHIPPED

Owner, 2026-10-09: the bypass note should read just BYPASSED, in a pill
like the EDITED one (`#dirty-state`).

```yaml
outcome: on mixer.html every bypassed stage head shows one BYPASSED status pill styled like EDITED; the editor's off-note text is not shown; the pill's box is reserved, so toggling a bypass moves nothing
seams: [firmware/sd/www/mixer.js, firmware/sd/www/mixer.css, src/apps/preview/content/aius-mixer/]
targeted-proof: vitest (root git_modules/aius) green with mixer.test.js unedited; Chromium probe at 1440 and 390 px where a trusted bypass click shows the pill (text BYPASSED, the same computed height, radius and border as #dirty-state), hides the long note, and leaves the card box and action-bar height unchanged within 1 px
```

- Own call: `mixer.js` drives the pill from the card's `data-stage="off"`,
  which `refreshStageInertness` already sets (`dsp-editor.js:1340-1341`),
  and the off-note span is visually hidden on `mixer.html`. So this needs
  no `dsp-editor.js` change, and Phase 1e's note-text fix now matters only
  for the classic page.
- Built:
  - Each `.stage-head` gets a `span.pill.stage-head__bypass`, placed after
    EDITED. A `MutationObserver` on the nearest `[id^="card-"]` follows
    `data-stage` and sets `data-on`; `visibility: hidden` holds the box
    while off.
  - `.stage-head__note` is visually hidden but kept in the accessibility
    tree.
  - Probed at both widths: the pill's height, radius and border match
    `#dirty-state` (24 px, 4 px, none), and the card and action bar are
    unchanged across the bypass.

### Phase 2: Vendor the fonts ✓ SHIPPED

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
- Round 1 delivered 2026-10-07: five variants at
  `https://apps.example.net/apps/preview/aius-logos/round-1.html`, A
  (source proportions), B (bold, tight), C (airy, ring outputs), D (A stacked
  over the wordmark), E (B beside the wordmark, nav lockup). Favicons are
  redrawn on a 16 px pixel grid, not scaled down. The wordmark is outlined
  shapes, because an SVG used as an image or favicon cannot load a web font.
  Generator: `$CLAUDE_SESSION_SCRATCH/gen_logos_r1.py` (session
  `74c6ef4f`; it is reaped after about a week, so round 2 reauthors from the
  SVGs if it is gone). Awaiting the owner's picks, plus two questions round 2
  needs answered:
  - Grid orientation. The source has 3 rows by 2 columns with the middle-left
    dot amber; the current CSS logo has 3 columns by 2 rows with the
    top-middle dot amber. Round 1 follows the source.
  - Favicon on a light browser chrome. The off-white dots vanish, so either
    keep the black tile or add a `prefers-color-scheme: light` colour swap.
- Owner on round 1, 2026-10-10: likes the text mark and the six-into-two
  concept, but none of the five variants works. Round 2 widens the search
  instead of narrowing it: 8 to 10 structurally different readings of
  "six channels into two" paired with the wordmark, both grid orientations,
  with the favicon and nav lockup shown for each. Page: `round-2.html`.
- Round 2 delivered 2026-10-10 at
  `https://apps.example.net/apps/preview/aius-logos/round-2.html`, nine
  numbered concepts:
  - 01 tittle-rails;
  - 02 downmix-traces, the real 5.1 routing;
  - 03 transpose-meters, 3x2 re-stacked into two meters with a phantom
    centre;
  - 04 knockout-tile;
  - 05 rule-rhythm;
  - 06 stereo-brackets;
  - 07 mux-block;
  - 08 split-centre;
  - 09 braille-i.
  The agent's picks are 03 and 02. Every favicon is a tile, so light
  grounds need no colour swap. Generator:
  `$CLAUDE_SESSION_SCRATCH/gen_logos_r2.py` (session `74c6ef4f`; reaped
  after about a week). Awaiting the owner's picks.

### Phase 4: A committed DSP-config setter with `--auth-file` ✓ SHIPPED

The owner chose to enable the board's limiter (Phase 5). No committed tool can
flip one config field with the token read from a file, and `CLAUDE.md` forbids
a literal token on a command line or a scratch-script workaround.

Settled by the owner, 2026-10-07: a standalone script, so the measurement
tool `dsp_characterize.py` stays read-only.

- outcome: a committed tool,
  `firmware/tools/dsp_config/aius_dsp_set.py`. It runs GET
  `/api/dsp/config`, sets the named dotted paths, then PUTs the result with
  the current `config_generation` and `X-Aius-SD-Auth` read from
  `--auth-file`. It refuses on 409 or 400.
- Wire shape (found 2026-10-07): the PUT body is the whole config plus
  `base_generation` = the GET's `config_generation`
  (`firmware/lib/aius_dsp_config/src/aius_dsp_config.c:1022`). Stale answers
  `409 stale_generation`, invalid `400`, busy or no card `503`
  (`firmware/src/aius_optical_app.cpp:1336-1360`). A 200 means the SD commit
  landed; `config_generation` advances only at the audio block-boundary
  apply, so with no carrier the GET shows `config_pending: true` and the old
  generation.
- Shipped after three adversarial review rounds and a clean-room pass, both
  CLEAR:
  - It refuses when the GET already shows `config_pending: true`.
  - `--wait SECONDS` exits 0 when the board shows `config_pending` false and
    the applied config matches what was sent. Numbers may differ by a
    relative 1e-12 (the decoder drift below);
    `source` is ignored, and so is `matrix.headroom_linear` under clipfree.
    The generation does not count, because the counter restarts at each boot.
  - A transport fault on the PUT re-GETs once rather than reporting a
    refusal, and a token that is not one line of printable ASCII is refused.
    Exception text is never printed.
  - Exit codes are 0, 1 (HTTP or transport failure), 2 (local refusal) and
    3 (wait timeout).
  - Tests: `firmware/test/dsp-config/test_aius_dsp_set.py`, 26 cases on a
    real live-GET fixture, run by the `dsp-config` suite in
    `firmware/scripts/test.sh`.
- Float drift, settled by the owner 2026-10-07: accepted. Any save moves an
  untouched float whose text the firmware decoder misrounds by one ulp (live:
  `matrix.lfe_gain_linear`), and a newly typed value by up to 5 ulps (the
  acceptance was asked on "one ulp"; the owner accepted the larger figure
  the same day). The classic editor already does the same. The
  firmware fix is tracked in
  [`aius-open-work-register.md`](aius-open-work-register.md) §Open defects.
- seams: `firmware/tools/`, and its usage line in `CONTRIBUTING.md`.
- targeted-proof: a host unit test of the body it builds (only the named
  paths change, the generation is carried), plus `--dry-run` printing the
  diff against a live GET.

### Phase 5: Enable the limiter on the board

Owner, 2026-10-10: the owner runs this phase by hand; agents do not run it.
The block below stays as the owner's reference.

Settled by the owner, 2026-10-07: enable the limiter. The live config read at
08:20 UTC had `features.limiter` false, with centre 1.5, surround 0.855,
LFE 0.316 and headroom 0.5. A worst-case in-phase sum overshoots full scale
by 5.3 dB. `limiter.enabled` was already true at -1 dBFS, 0.5 ms attack,
100 ms release and 2 ms lookahead.

- Pre-step done 2026-10-07 (review CLEAR): the setter's residual findings
  are closed. After a transport fault, "landed" needs a matching GET (R1).
  The wait timeout names a different running config (R3). Numbers compare
  within a relative 1e-12, and mismatch messages list the differing paths
  (R4). Accepted as is: R2, a no-op `--set` plus a transport fault reports
  "landed", which is harmless. The suite has 30 setter tests.
- **Check before the run:** aius `9c9bd88` (sample-rate-following Phase 4f)
  makes HTTP best-effort during playback, and this phase needs playback for
  the save to apply. Before the run, read 4f's contract in
  [`aius-sample-rate-following.md`](aius-sample-rate-following.md) for what
  a `PUT /api/dsp/config` and the `--wait` GETs get during playback (a
  delayed answer, `503`, or a dropped connection). Confirm the setter's
  timeouts and its transport-fault re-GET cover it. Stop if the build on the
  board predates 4f, and record which one was found.
- outcome: `features.limiter` true on the board; nothing else changed.
- seams: the board's `/aius/dsp.json`, through Phase 4's tool.
- targeted-proof: a GET `/api/dsp/config` showing `features.limiter: true` at
  a new `config_generation` and `config_pending: false`, with every other
  field equal to the pre-change read, numbers within a relative 1e-12
  (owner, 2026-10-07: `matrix.lfe_gain_linear` moves one ulp, known decoder
  drift).
  The save applies only inside `enqueue()` when an audio block arrives
  (`firmware/src/optical_main.cpp:2045-2065`), so the proof needs a carrier
  or a replay running; `--wait` only watches for it. With no carrier (the
  board read `carrier: 0` on 2026-10-07) the run exits 3 with the save
  staged on the card, and the limiter turns on at the next carrier or replay
  start. A re-run while staged is refused (exit 2), so a retry is safe.
- Command, from the repository root: `python3
  firmware/tools/dsp_config/aius_dsp_set.py --device
  http://192.0.2.66 --auth-file .secrets/aius-sd-auth --set
  features.limiter=true --wait 30`. Run `--dry-run` first; it printed
  `features.limiter: false -> true` and nothing else on 2026-10-07.
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
- **First, confirm the card takes a subdirectory.** `aius-sd-sync.sh`
  PUTs `fonts/x.woff2` as `/sd/www/fonts%2Fx.woff2`. Unverified: whether the
  firmware's SD PUT route decodes `%2F` and creates `fonts/`, and whether the
  GET route serves `/sd/www/fonts/x.woff2`. Read the route in
  `firmware/src/aius_optical_app.cpp` before the deploy; if either fails,
  flatten the fonts into `www/` (rename, update `mixer.css` and
  `build_preview.py`) rather than change the firmware.
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
