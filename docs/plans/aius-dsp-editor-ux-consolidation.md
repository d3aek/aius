---
title: Aius DSP editor UX consolidation
type: plans
scope: component
component: aius
status: complete
updated: 2026-09-04
toc: true
---

# Aius DSP editor UX consolidation

## Objective

Resolve the confusion in the SD-hosted DSP editor's apply and preset surfaces
(`firmware/sd/www/index.html` + `dsp-editor.js`) without changing the firmware
contract. The page already reads the device config on load and validates edits
against the parser's bounds; the problems are all in the control surface:

- Two apply paths (a top-level "Apply to device" and a per-panel "Apply" in
  every stage head) leave it unclear which is authoritative or whether the top
  one is mandatory.
- The signal-flow strip is preview-only, so dragging an EQ handle or matrix node
  changes nothing audible until an Apply. Nothing on the page makes "staged, not
  live" unmistakable.
- Mutating controls are enabled without a token and only fail on use.
- The presets card consumes a full section of vertical space for a rarely-used
  surface.

## Non-goals / fixed constraints

- **No live per-keystroke apply.** Each apply is a `PUT /api/dsp/config` -> SD
  write -> generation bump -> "applies at the next audio block boundary" with a
  pending poll up to ~20 s. Auto-applying on every drag would wear the SD card,
  churn biquad re-designs, and stack pending windows. The fix is to make the
  staged model unmistakable, not to apply live. (An optional debounced "Live"
  mode was offered and declined.)
- **No firmware/route changes.** Presets keep riding the existing SD file
  surface (`/sd/www/presets/`); apply keeps the optimistic-concurrency
  `config_generation` discipline.
- **No shared UI kit.** These files are served off the Teensy SD card: classic
  scripts, no imports, no build step. Keep every SD page looking like one
  product via `replays.css` + `dsp-editor.css` and the `--aius-*` tokens.
- **Read-only viewing needs no token.** The dashboard, meters, preset list,
  preset Load (fills the form only), and preset Download (a browser save) all
  stay open with no token entered.

## Decisions (2026-09-04 interview)

1. **Apply model: one sticky action bar, single Apply.** A persistent bar,
   visible at every scroll position, carries the apply-state pill, the aggregate
   "N unapplied changes" summary, the token lock/unlock control, **Revert**, and
   **Apply**. The per-panel Apply buttons and `applySection`/`sectionModel` are
   removed; `applyConfig` (the whole-form write) becomes the only writer. Each
   panel keeps its enable checkbox, per-parameter reset, off-note, and its
   informational "edited" pill (a locator for which stage diverged); it loses its
   Apply button.
2. **Token gating: disable + inline unlock.** Every mutating control (Apply,
   preset Save, preset Upload, per-row Delete) is disabled with a stated reason
   until a token is present. The token entry lives in the action bar as a
   lock/unlock affordance on the same `sd-shared` localStorage slot. No blocking
   modal.
3. **Presets: compact inline row.** The full presets card collapses to a single
   row: a preset `<select>` + **Load** + **Save as...**, with Import file /
   Download selected / Delete selected behind an overflow, and inline name
   validation in place of the standalone hint line.

## Surface changes

### Action bar (new)

A sticky element inside `main`, above or below the fold-independent content. Left
to right: apply-state pill (reusing the shared vocabulary
loading/loaded/edited/sending/pending/applied/stale/invalid/error), the
"N unapplied changes" summary (hidden when clean), the model byte fact, the token
lock control, **Revert** (re-read device, discard local edits, keeping the
existing `confirmDiscard` guard), and **Apply** (primary). The `beforeunload`
guard and the 409 stale notice stay.

### Effective configuration card (slimmed)

Reduced to a provenance strip: `Config source . generation . sample rate` plus
the 409 stale notice. Token, Apply, Reload, dirty count, apply-state and byte
count move to the action bar. The bypass-flags explanatory paragraph shrinks to a
one-line note (the flags already live per-panel). The page keeps its Dashboard
default view (it already reads the device config on load); a failed initial read
stays loud through the existing error path.

### Signal-flow strip (labeling only)

A persistent "Preview only. Edits apply on Apply." label tied to the sticky bar,
so drag-with-no-sound is explained in place. No behavioural change.

### Presets (compact row)

`<select>` populated from `state.presets`; **Load** fills the form from the
selection; **Save as...** uses an inline name field with `presetNameProblem`
validation shown only on save; overflow carries Import file, Download selected,
and Delete selected. The long hint paragraph, the separate name-hint line, and
the always-on file-input row are removed from the default view.

## Phases

Each phase commits in the aius submodule, then bumps the upstream pointer.
Proof for every phase is the sd-editor vitest suite from the aius repo root:
`pnpm exec vitest run --root git_modules/aius` (or
`firmware/scripts/test.sh --environment native --suite sd-editor`).

### Phase 1 - Sticky action bar + single Apply

- outcome: one sticky Apply/Revert/state/token bar; per-panel Apply removed;
  `applyConfig` is the sole writer; the aggregate dirty summary reads
  "N unapplied changes".
- seams: `index.html` (bar markup, effective-config card slimmed to the
  provenance strip), `dsp-editor.js` (`buildSectionHead` drops the Apply button;
  remove `applySection`/`sectionModel`/`sectionFields`; wire the bar; dirty
  summary), `dsp-editor.css` (sticky bar rules).
- targeted-proof: rewrite the section-apply cases in
  `test/sd-editor/dsp-editor.test.js` (all `apply:${key}` clicks); add cases for
  the sticky bar (Apply is the sole writer, dirty summary text, Revert
  re-reads/discards), keeping the existing 409 / pending-poll / byte-cap
  coverage green through the single path.

### Phase 2 - Disable-until-token + inline unlock

- outcome: Apply and every preset write disable with a reason until a token is
  present; read-only controls unaffected; token lives in the action bar.
- seams: `index.html` (token control in the bar), `dsp-editor.js`
  (`tokenPresent()` + `refreshTokenGating()` called on input and load; remove the
  soft "SD auth token required" error-on-click as the primary path, keep it as a
  defensive guard), `dsp-editor.css`.
- targeted-proof: cases asserting mutating controls disabled with no token and
  enabled once entered; read-only controls (Load, Download, Refresh) unaffected;
  update the existing "refuses to write without an SD auth token" case to the new
  disabled-button behaviour.

### Phase 3 - Compact presets row

- outcome: the presets card becomes a one-row control (select + Load +
  Save as... + overflow) with inline name validation.
- seams: `index.html` (presets markup), `dsp-editor.js` (`renderPresetList` ->
  select population; Load/Download/Delete act on the selection; wiring), css.
- targeted-proof: rewrite the preset cases (list/select population, Load fills
  the form, Save validates the name, Import/Download/Delete act on the selection,
  case-insensitive collision guard preserved).

### Phase 4 - Evidence + preview labeling

- outcome: effective-config card reduced to the provenance strip
  (`source . generation . sample rate`) + the 409 stale notice; the signal strip
  carries a persistent `#preview-note` ("Editing here is a preview ... Apply");
  the initial device read stays loud (the existing `loadConfig` error path).
- refinement (2026-09-04): the default view stays **Dashboard**. The page already
  reads the device config on load, so the evidence gap was framing, not a missing
  read; flipping the deliberate Dashboard default was unnecessary and out of
  scope. The provenance strip + the sticky bar's apply-state pill make "this is
  the live device config" evident without changing the default view.
- seams: `index.html`, `dsp-editor.js`, css.
- targeted-proof: the existing provenance-line case, the transport-failure case,
  and the retained Dashboard-default case all stay green.

## Outcome

Shipped 2026-09-04. `test/sd-editor` is green (355 tests): the section-apply
cases were replaced by single-Apply and token-gating cases, and the preset cases
by the compact-picker ones. Net structure: one sticky action bar is the sole
writer, mutating controls disable until the token is present (read-only viewing
stays open), and the presets card is one row with a More menu. Two real-browser
traps vitest cannot see were guarded in CSS: the dirty pill keeps
`.pill[hidden]{display:none}`, and the More-menu body's flex layout is scoped to
`[open]` so it never defeats the UA's closed-`<details>` hiding.

## Risks

- **Large coupled test rewrite.** `test/sd-editor/dsp-editor.test.js` (~4057
  lines) references the current IDs heavily; per-phase it must move with the
  surface it proves. Keep the byte-for-byte harness (real `index.html` body +
  real scripts + stubbed `fetch`).
- **ID churn breaks nothing silent.** happy-dom loads no stylesheets, so any
  `display`-toggled visibility (the sticky bar, the presets overflow, the token
  unlock) must restate `[hidden]{display:none}` and be proven by a browser probe,
  not the `.hidden` property alone (per `.claude/rules/javascript.md`).
- **Accessibility parity.** The bar and the presets control keep the existing
  focus-visible, aria-label, and role discipline the current controls carry.
