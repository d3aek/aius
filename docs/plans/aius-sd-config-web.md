---
title: Aius SD-backed configuration and web assets
type: plans
scope: component
component: aius
status: shipped
updated: 2026-08-05
toc: true
---

# Aius SD-backed configuration and web assets

## Objective

Move persisted DSP configuration and optional web assets off program flash while
keeping the audio path deterministic and retaining a minimal embedded recovery
page when the card is absent or invalid.

## Decisions

- The SD card is optional. Boot must remain successful without it.
- Active DSP configuration is always held in internal RAM and applied only at an
  audio block boundary.
- Configuration writes are explicit, bounded, and journaled. UI changes do not
  write the card automatically.
- Web assets are served in bounded chunks; no request may block the audio path.
- The embedded page remains the fallback and reports SD availability/config state.
- Asset paths are allowlisted; traversal, symlinks, and arbitrary filesystem
  enumeration are not exposed.

## Current implementation slice

The firmware now initializes the optional built-in SD card and exposes bounded
raw file operations for `index.html`, `app.js`, `style.css`, and `config.json`:

- `GET /api/sd` reports card availability.
- `GET /sd/{resource}` streams the file in bounded SD/RAM chunks.
- `PUT /sd/{resource}` streams through `/aius/.upload.tmp`, then replaces the
  target. `Content-Length` is required; the body is no longer limited by the
  firmware request buffer.
- `DELETE /sd/{resource}` removes an allowlisted file.

The embedded page remains the root page in this slice. Uploaded `index.html`
is available explicitly at `/sd/index.html`; root-page handoff is part of the
static asset server and control-surface migration phases. `config.json` is
stored but is not yet an active DSP configuration source; the build-time JSON
continues to define the current runtime profiles until the journaled parser and
activation phases land.

**Superseded (noted 2026-08-04) — the slice above is a historical snapshot.**
The delivered form: the SD surface is the path-confined `/aius/www/` tree
(streamed `PUT`/`GET`/`DELETE /sd/{resource}` with the atomic
temp/backup-replace protocol); `/aius/config.json` is the feature-flag document
only, updated via `PUT /api/config/public`
(`lib/aius_http/src/aius_http.c:133`), and was never made a DSP source — DSP
configuration became `/aius/dsp.json` with its own validated
`PUT /api/dsp/config`, owned by
[`aius-dsp-config-api.md`](aius-dsp-config-api.md). Root-page handoff was
resolved the other way: the flash page stays the root recovery/status surface
(with the SD upload form and a status link), and the operator loop lives
entirely in the SD pages, which link among themselves
(`index.html` / `replays.html` / `status.html` / `upgrade.html`).

## Phases

### Phase 1: bounded runtime configuration

**Done** — delivered as the bounded `lib/aius_config` parser (feature flags,
safe defaults on any error). The CRC/versioned journal contract was
superseded: persistence is plain JSON behind the atomic replace protocol, and
the DSP model moved to [`aius-dsp-config-api.md`](aius-dsp-config-api.md).

**unit:** portable config parser, validator, and journal record contract  
**seams:** JSON bytes ↔ validated runtime config; config ↔ audio-boundary apply  
**contract-artifact:** `config/optical_dsp.json`, parser API, CRC/versioned journal layout  
**fan-out:** parser tests own `lib/aius_config`; firmware adapter owns SD I/O  
**review-intensity:** exhaustive malformed, range, truncation, duplicate, and CRC cases  
**commit-checkpoint:** native config tests and cross-build green

Implement read-only load first, with factory defaults on any error. Limit the
profile count, EQ sections, names, and numeric ranges at parse time.

### Phase 2: SD persistence adapter

**Done, in revised form** — SD init plus `/aius/config.json` loaded through
the bounded SD job engine at boot and on hot-reinsert
(`firmware/src/optical_main.cpp:5816-5836`); status carries config
validity/error state. The `/aius/config.0`/`.1` two-slot journal was dropped
with the journal contract (Phase 1 note).

**unit:** Teensy SD initialization, two-slot journal read/write, and status fields  
**seams:** Arduino SD filesystem ↔ portable record validator  
**contract-artifact:** `/aius/config.0`, `/aius/config.1`, generation/CRC rules  
**fan-out:** SD adapter owns only `optical_main.cpp` and its SD helper; no DSP edits  
**review-intensity:** missing card, power loss between slots, full card, corrupt CRC, downgrade  
**commit-checkpoint:** build green and no-card boot path validated

### Phase 3: bounded static asset server

**Done** — the path-confined `/aius/www/` tree served at `/sd/{resource}` in
bounded 512-byte slices under the one-SD-primitive-per-epoch rule
(`lib/aius_sd/src/aius_sd.c:23`), with the directory index and the flash page
retained as fallback.

**unit:** allowlisted SD asset lookup and chunked HTTP response service  
**seams:** HTTP path ↔ asset manifest; SD read slice ↔ Ethernet write slice  
**contract-artifact:** `/aius/index.html`, `/aius/app.js`, `/aius/style.css`, MIME map  
**fan-out:** asset server owns HTTP adapter changes; page remains fallback-owned  
**review-intensity:** traversal, missing files, oversized files, disconnects, and timing budget  
**commit-checkpoint:** native route tests, target build, and fallback page smoke test

### Phase 4: control-surface migration

**Done, in revised form** — the contracted `GET/PUT /api/config` +
`POST /api/config/save` never shipped as such: public flags go through
`PUT /api/config/public` (see §Adversarial review resolutions), and the full
control surface became the SD DSP editor against `GET/PUT /api/dsp/config`
with explicit Apply and block-boundary activation
([`aius-dsp-config-api.md`](aius-dsp-config-api.md) Track A / task #7).

**unit:** SD page controls and explicit save/reload operations  
**seams:** browser API ↔ volatile config; save action ↔ journal writer  
**contract-artifact:** `GET/PUT /api/config`, `POST /api/config/save`, status telemetry  
**fan-out:** page assets own UI; firmware API owns validation and persistence  
**review-intensity:** pending-vs-active state, reboot recovery, and no-write-on-slider-change  
**commit-checkpoint:** HTTP control tests, SD fixture tests, and soak with card absent/present

## Acceptance gates

- No SD card produces the existing embedded page and factory DSP defaults.
- Invalid or interrupted config writes never prevent boot and never partially
  enter the audio path.
- Runtime control changes do not write flash or SD until explicit save.
- SD reads and writes stay outside the audio interrupt and bounded service slice.
- Removing the card during HTTP service does not reset or stall audio.
- The complete SD-backed page is optional; the fallback page always remains usable.

## Dashboard mode — scope, recovered 2026-07-28

**Recovered from the 2026-07-28 session transcript after it was nearly lost.**
It was live in the operator's queue, never deferred by them; a handover note
flattened it to the single word "dashboard (parked)", and the task list
carrying its detail died with a mid-day process restart. Written here so it
is in a checked-in home rather than scratch. Not yet a contract — this is
the agreed scope, and it needs a phase contract and a gate before build.

> **Superseded 2026-08-05 — the resizing is gone.** Everything in this section
> describing a 12-column grid, per-cell spans and heights, bottom-right resize
> handles, arrow-key resizing, persisted cell geometry and the `Reset layout`
> button describes what was built and shipped, and is retained as that record.
> On the owner's call after a design review, the Dashboard is now a single
> column of full-width bands in signal order, each as tall as its content, with
> no resizing of any kind: freely resizable panels bought nothing, and the
> mismatched heights within a row left dead space the drag handles existed to
> paper over. The canvases carry explicit heights because a canvas has no
> content height. Desktop and mobile are now the same single column, which is
> what makes the page flow on a phone. Only the mode (Editor or Dashboard)
> still persists in `aius-dsp-editor-layout-v1`; a stored record carrying the
> retired geometry is read for its mode and the rest ignored. Current
> behaviour: [`../reference/user-interface.md`](../reference/user-interface.md).

**What it is: one resizable, gridded surface on the existing SD-served
editor page** that gathers the DSP control and visualisation elements
together. It is a mode of the existing editor, never a second page — the
one-surface rule from the editor work applies.

**What it carries:**

- **The interactive EQ curve**, with the realtime FFT bar graph underlaid.
  (That underlay shipped separately as the analyzer, `19b7351fe` +
  `5d56f84cf`; the dashboard is where it becomes part of a composed
  surface.)
- **The downmix controls**, alongside the visualisations rather than in a
  separate panel.
- **A spectrogram.** Its origin is the operator's judgement that "the meter
  waveform is not useful visualization" — the spectrogram replaces it as
  the default visual, at comparable cost.
- **Everything free to resize.**

**The consequence that makes resizing more than cosmetic:** free resizing
forces every renderer to become size- and DPR-aware. That is not overhead —
**it retires the existing hiDPI waveform debt as a side effect**, which is
why resizing is in scope rather than deferred.

**Q editing, an open thread to resolve inside this phase.** The EQ canvas
already has a mouse-wheel-over-handle Q binding, so it is either broken or
undiscoverable. The dashboard phase must verify which, make it visible, and
add a modifier-drag alternative so Q is fully mouse-editable either way.

**Deployment**: this is card-hosted UI, so it needs no USB and no flash —
it deploys over HTTP via `aius-sd-sync.sh` and remains fully available after
the board is unplugged from USB.

### Contract (2026-07-29) — grounded in the editor as it actually stands

**Owned paths:** `firmware/sd/www/{dsp-editor.js,dsp-editor.css,sd-shared.js,
index.html}`, `test/sd-editor/`, `docs/plans/aius-open-work-register.md`, this
plan. Relative to ``.

**Three assumptions in the scope above were wrong; the code already answers
them.** Grounding first, because each changes the work:

1. **DPR handling already exists and is correct.** `fitCanvas`
   (`dsp-editor.js:2858`) clamps `devicePixelRatio` to 1–3, sizes the
   backing store from `clientWidth × dpr`, and every renderer already works
   in FRACTIONS of the canvas rect (`:77`). The dashboard does **not** need
   to invent DPR awareness.
2. **What `fitCanvas` DOES lock is ASPECT** — `canvas.dataset.aspect` is
   captured once from the initial `height/width` and height is thereafter
   derived from width. **That single line is the real barrier to free
   resizing**, and the honest statement of the renderer work: height and
   width both become inputs, so any renderer that assumed a fixed aspect
   must take the rect instead. "Size-aware" means aspect-free, not
   DPR-aware.
3. **The hiDPI waveform debt is one precise residue, already recorded**:
   `aius-dsp-config-api.md:1534-1536` — the shared waveform renderer strokes
   one *device* pixel while the plot is DPR-scaled, noted as "a future
   `sd-shared.js` touch". Small and surgical; the scope's claim that
   resizing "retires the hiDPI debt as a side effect" is **too strong** —
   fixing it is a deliberate line item here, not a by-product.

**The Q thread is resolved: the binding is not broken, it is invisible.**
`Q_WHEEL_STEP`, `stepQ()` and the plot's `wheel` handler
(`dsp-editor.js:65, 2371, 3214`, `passive: false` so the page does not
scroll) all work. The code already diagnoses the problem itself at `:2934`:
*"the wheel was the only way to Q from the plot and a wheel is invisible
(and absent on touch)"*. So the phase owes three things, not a bug hunt:
an affordance that makes it discoverable, a modifier-drag alternative for
mouse, and a touch path. **The identical problem exists for the downmix
matrix** (`MATRIX_WHEEL_STEP`, `stepMatrixValue`, `:91, 2522, 3565`) — fix
both, or record why not.

**The spectrogram's data source is already shipped, with one honest
limitation.** `/api/meters` carries `analysis_window` (1024 raw mono
samples) plus `analysis_full_scale`, and the analyzer already computes an
FFT from it for the EQ underlay — **reuse that transform, do not add a
second**. The limitation to state in the UI rather than paper over: at 10 Hz
polling each 1024-sample window is 21.3 ms of signal taken every ~100 ms, so
the time axis is **sampled, not gapless**. It is a strobed spectrogram, and
labelling it honestly is cheaper than pretending otherwise.

**One surface, one control path.** The dashboard is a VIEW MODE over the
same model and the same controls — per-panel enables, section Apply and
per-section dirty markers keep their current semantics exactly. It must not
become a second way to write config, and the editor is already ~3,600 lines,
so the deletion test applies: if the mode cannot be expressed as layout over
existing controls, that is the signal it is being built wrong.

**Layout persistence:** cell sizes persist to `localStorage`, matching where
the editor already keeps the auth token. Not to SD — layout is per-browser
preference, not device configuration, and putting it in the config model
would drag it through the PUT validator and the config generation counter
for no benefit.

**Proof:** the `sd-editor` vitest suite (currently 272/272) covers the
model, dirty tracking and surface writes; extend it for the aspect-free
renderer maths and the new Q affordances. Layout and touch interaction need
the on-device browser pass at desktop and phone widths that
`aius-dsp-config-api.md` already lists as owed for the editor slice — the
dashboard inherits it rather than adding a second one.

**Not in scope:** any firmware change (this is card-only), a second page, and
any new `/api` surface — everything the dashboard renders is already served.

### Phase 9: dashboard view mode

```yaml
phase: 9
title: Resizable DSP dashboard view
unit: parent-only
seams:
  - existing editor DOM and form model ↔ editor/dashboard layout modes
  - dashboard cell rectangle ↔ aspect-free DPR-scaled canvas backing
  - one accepted meter snapshot FFT ↔ EQ underlay and spectrogram history
  - pointer/keyboard resize gesture ↔ validated browser-local layout preference
contract-artifact: spec-only
review-intensity: active
commit-checkpoint:
  green:
    - pnpm exec vitest run test/sd-editor
  buildable:
    - card-served DSP editor at /sd/www/index.html
  parent-commit: "feat(aius): add the resizable DSP dashboard"
```

**Plan coordination.** This is Phase 9 of the existing 2026-07-22 plan, not a
new feature plan. It extends `aius-dsp-config-api.md`'s shipped editor, closes
the hiDPI waveform item in `aius-open-work-register.md`, and inherits the
editor's outstanding desktop/phone device pass. The outstanding-work validator
cannot audit this path: `DocumentDiscovery` deliberately walks only `docs/` and
`src/{apps,libs,services,tools}/**/docs`, so both this plan and
`aius-dsp-config-api.md` return `outstanding.snapshot.plan-absent`. Expanding
the repository document/owner model to `src/hardware` is unrelated tooling work
and is not smuggled into a card-only dashboard phase.

**Placement and mode.** The existing Signal flow `.section-title` row owns an
`Editor / Dashboard` mode switch and `Reset layout`; no new bar or page is
introduced. Dashboard is the first-visit default. The selected mode and cell
sizes persist together in the versioned `aius-dsp-editor-layout-v1`
`localStorage` record. Editor mode preserves the current vertical reading order
and hides the resize handles. Reset restores the default dashboard grid,
remains in Dashboard mode, and replaces the stored record. A malformed,
unknown-version, unavailable, quota-failed, or out-of-range stored value falls
back to the defaults without making the page fail.

The shared repository `mr-*` kit is not loadable on this self-hosted card: these
assets must remain classic, build-free files that work with no repository host.
The phase therefore composes the established SD-page chrome and native controls
already shared through `replays.css`/`sd-shared.js`; it does not introduce a
parallel application shell or a reusable repo widget.

**Surface inventory and default grid.** Effective configuration and Presets
remain above and outside the surface. The existing Signal flow content and Live
processing panel become one dashboard surface with six cells, all using the
same controls and DOM nodes as Editor mode:

1. input levels;
2. stacked EQ/analyser plus its input/output EQ forms;
3. downmix diagram plus its matrix form;
4. limiter reduction/history plus its form;
5. output levels; and
6. live processing telemetry plus the spectrogram.

At desktop width the surface is a 12-column grid: input 4 + EQ 8, matrix 8 +
limiter 4, output 4 + live processing 8. A cell may resize from 3–12 columns
and from its exact minimum through 1,200 CSS px: input levels 180 px, EQ
workspace 480 px, matrix workspace 420 px, limiter 300 px, output levels
180 px, and live processing/spectrogram 360 px. A cell scrolls internally
instead of clipping controls. Resizing snaps width to grid columns and keeps
DOM order; movement/reordering is not in scope. The EQ and matrix drawing areas
never shrink below 280 CSS px wide; below 420 px they cull alternate
non-essential axis/source labels rather than overlap them. The spectrogram
drawing area never shrinks below 240 px wide. At phone width (650 px and below)
cells collapse to one full-width column, retain independently resizable heights,
and preserve page scrolling. Stored desktop spans survive the phone layout and
apply again when the viewport widens.

Each cell has one bottom-right resize handle with pointer capture for mouse and
touch, plus arrow-key resizing for keyboard use. Width/height changes are
clamped before rendering and stored only at resize end (or the keyboard
equivalent), so pointer movement does not churn `localStorage`. A
`ResizeObserver` watches the cells/canvases and refits after direct resizing,
mode changes, and responsive reflow. `fitCanvas` takes both positive
`clientWidth` and `clientHeight`, clamps DPR to 1–3, and leaves the current
backing store untouched when either dimension is zero. Dashboard CSS supplies
the independent canvas height; Editor CSS retains the authored aspect. Renderers
must remain legible at the declared minima and may cull non-essential labels
below their own safe widths rather than drawing them over controls.

**EQ and matrix editing.** The visible `Section Q` field remains the touch path,
and the existing wheel binding remains the fine mouse path. Shift held at EQ
handle pointer-down selects Q-drag for that gesture: vertical movement changes
Q multiplicatively from the captured starting value, 120 CSS px per `e` ratio,
clamped to 0.1–20, while frequency and gain remain frozen. The canvas copy and
accessible label disclose wheel, Shift-drag, and the visible Q field.

The matrix is not given a duplicate gesture. Its existing direct vertical
handle drag already works with mouse and touch, its number fields remain the
explicit touch/typing escape hatch, and the wheel/keyboard paths remain fine
adjustments. Dashboard work preserves and re-proves those paths.

**Spectrogram.** The DSP editor waveform is replaced by a strobed spectrogram.
For each finite, previously unseen meter `sequence`, the page performs exactly
one `spectrumBins()` FFT over the accepted analysis window and derives both the
EQ underlay and one spectrogram column from that result. Repeated sequences,
missing/short windows, and failed polls append nothing and run no transform.
They clear the live EQ underlay but retain the bounded historical image with a
paused/unavailable label until a later accepted window arrives.

The history holds at most 256 snapshot columns of the existing 48 log-frequency
bands, 20 Hz at the bottom and 20 kHz at the top, coloured over -96–0 dBFS,
with newest time at the right. Append happens only at the existing meter poll
cadence; there is no second poll, animation loop, or FFT. Resize redraws the
stored history without recomputation. Visible copy is derived from the current
window and rate, for example `21.3 ms sampled per 100 ms poll — strobed, not
gapless`; it never implies continuous coverage.

**No second write path.** Mode changes, Reset layout, and cell resizing mutate
only DOM layout state and the versioned browser preference. They dispatch no
form `input`, change no dirty marker or generation, modify no preset, perform no
fetch, and issue no PUT. EQ and matrix interaction continue through
`writeField()` and the existing section Apply/full Apply semantics.

**Shared waveform debt.** Although the DSP editor stops showing the waveform,
`sd-shared.js` still renders it for the replay page. `drawWaveform` must size its
backing store from both CSS dimensions and DPR, scale its strokes by DPR, and
retain the centre-line/128-point contract. Replay waveform coverage is part of
the phase gate.

**Targeted proof.** Extend the SD-editor vitest scope for:

- two independent width/height changes observed through `ResizeObserver`,
  zero-size fallback, DPR clamping, and aspect-free coordinate maths;
- default, restored, malformed, wrong-version, out-of-range, failed-storage,
  reset, and phone-collapse layout behavior;
- zero PUTs and unchanged dirty state/generation across mode switch, reset, and
  resize;
- Shift-drag Q with captured modifier, multiplicative sensitivity and bounds,
  plus the visible touch Q field;
- preserved mouse/touch matrix drag;
- one FFT/history append per new sequence, no duplicate append, the 256-column
  bound, missing-window pause, scales/labels, and resize-only redraw; and
- DPR-scaled shared waveform rendering while retaining replay-page tests.

The exact `www` tree is first synced with `aius-sd-sync.sh` using the
already-available 16-hex USB-derived token, then the changed assets and page are
GET-verified from the device. The boundary browser pass then runs against that
deployed page at 1280×900 desktop and 390×844 phone viewports. It verifies mode
placement/default/persistence,
mouse/touch/keyboard resizing and reset, phone scroll without canvas traps,
aspect-free redraw, Shift-drag Q, touch Q field, touch matrix drag, spectrogram
history and strobed label, unchanged dirty state after layout operations, and
one successful existing Apply. No firmware flash occurs, and the final
page-availability check is over Ethernet with USB not required.

**Adversarial prebuild review (2026-07-29): RED, amended.** The review found
the original scope under-specified in placement, cell inventory, resize
mechanics, Q/matrix behavior, spectrogram bounds, persistence failure handling,
proof, deployment, and plan indexing. The operator chose the Signal flow title
row, Dashboard-first default, and persisted mode. The contract above resolves
the remaining findings. Implementation begins only after the same reviewer
confirms them exhausted and a fresh clean-room reviewer returns GREEN.

**Phase 9 completed 2026-07-29.** The retained reviewer and a fresh clean-room
reviewer both returned GREEN before build. The delivered view keeps the six
existing control blocks and write path, adds the versioned Dashboard layout,
aspect-free canvas fitting, Shift-drag Q, the bounded strobed spectrogram, and
the DPR-scaled shared replay waveform. Targeted proof is 283/283 SD-editor
Vitest cases. The exact four changed assets hash-matched after additive card
sync. On-device Chromium passed at 1280×900 and 390×844 with DPR 2, including
mouse/keyboard/touch resizing, reset and persistence, phone scrolling, touch
matrix drag, Shift-drag and visible-field Q, spectrogram disclosure, and clean
reloads after interaction. An unchanged-model Apply advanced device generation
1→2, and the page/status endpoints remained HTTP 200 over Ethernet without a
firmware flash.

## Extension plan: SD scheduling, feature flags, and physical-access authorization

### Phase 5: budgeted SD service scheduling

**Done** — delivered as the portable bounded SD job engine
(`lib/aius_sd`, one job at a time, one 512-byte transfer slice per epoch,
`validate_request()` refusing anything larger), composed into the runtime by
the network-hardening phases (its §Bounded SD job ABI is the authoritative
contract).

**unit:** deferred SD read/write jobs and cached utilization refresh  
**seams:** HTTP transport ↔ SD job queue; SD job completion ↔ HTTP response  
**contract-artifact:** bounded job state, 512-byte transfer slices, status timing counters  
**fan-out:** firmware SD scheduler owns `optical_main.cpp`; runner owns timing assertions  
**review-intensity:** disconnects, partial writes, card removal, queue starvation, audio-overrun correlation  
**commit-checkpoint:** build, native parser/runner tests, target CRUD, and soak with zero audio overruns

SD writes must not run synchronously inside the HTTP service budget. The HTTP
handler will stage bounded data and the main-loop SD service will perform one
small transfer at a time, preserving atomic temporary-file replacement. Usage
scans remain cached and refreshed by the SD maintenance service.

### Phase 6: SD feature-flag configuration

**Done, with one contract narrowing** — the shipped `features` object carries
three booleans, `sd_web`, `remote_control` and `telemetry`
(`lib/aius_config/src/aius_config.c:68-70`); the contracted fourth key
`sd_config` was not implemented. Safe defaults on missing/invalid data and
the auth-not-toggleable rule hold as written.

**unit:** bounded feature-flag loader with defaults and invalid-file fallback  
**seams:** `/aius/config.json` bytes ↔ volatile flags; flags ↔ HTTP/DSP policy  
**contract-artifact:** `features` object with `sd_web`, `sd_config`, `remote_control`, and `telemetry` booleans  
**fan-out:** portable flag parser/tests own the config contract; firmware adapter owns loading; OpenAPI owns exposure  
**review-intensity:** missing card, malformed JSON, duplicate keys, unknown flags, safe defaults, reboot persistence  
**commit-checkpoint:** native parser tests, target load/status test, and config replacement test

Flags are loaded into RAM and never consulted from the audio interrupt. Invalid
or absent configuration leaves safe defaults. The authentication policy is not
feature-toggleable, so a malformed or malicious config cannot disable write
authorization for assets.

### Phase 7: physical-access SD write authorization

**Done** — `AIUS AUTH` over CDC discloses the token
(`lib/aius_auth`; honest-strength caveat recorded in
[`aius-ota-upgrade.md`](aius-ota-upgrade.md) §Security), `X-Aius-SD-Auth`
gates asset writes/deletes, and 2026-07-28 the gate widened to the control
routes ahead of the dispatch switch
([`aius-network-hardening.md`](aius-network-hardening.md) defect 12).

**unit:** per-device firmware-derived token disclosure over USB and HTTP header verification  
**seams:** USB `AIUS AUTH` command ↔ token; `X-Aius-SD-Auth` header ↔ write policy  
**contract-artifact:** token format and authenticated PUT/DELETE rules in OpenAPI and runner  
**fan-out:** firmware auth owns USB/HTTP code; runner owns credential acquisition/use tests; docs own operator procedure  
**review-intensity:** missing/wrong/replayed token, config exception, malformed headers, reboot stability, no HTTP disclosure  
**commit-checkpoint:** native auth tests, USB command smoke test, authenticated asset CRUD, unauthenticated config update

The token is derived from firmware-held device material and a compiled salt,
printed only in response to an exact physical USB command. Asset PUT/DELETE
requires the token in `X-Aius-SD-Auth`; `config.json` PUT remains the explicit
operator exception requested for frequent user changes. DELETE of config and
all non-allowlisted paths remain rejected.

### Revised file and configuration boundary

The fixed four-file allowlist is too narrow for iterative UI development. The
replacement contract is:

- `/aius/www/` is a path-confined UI asset tree. Arbitrary nested files are
  accepted within bounded path and request limits; `..`, duplicate separators,
  absolute paths, hidden upload staging names, and filesystem escape are
  rejected. UI asset PUT/DELETE requires `X-Aius-SD-Auth`.
- `/aius/config.json` is not directly writable over HTTP. The public section is
  updated through a dedicated API operation after parsing and ACL validation.
- The public configuration ACL is firmware-owned and declares which keys may
  be changed without the token. Protected DSP, network, auth, and filesystem
  policy keys cannot be changed through that exception.
- Invalid JSON, schema violations, rejected public keys, and failed SD commits
  are retained in status as a bounded error code/message and never enter the
  active runtime configuration.

JSON remains the external format because the browser dashboard and future
tooling need nested objects, arrays, and a stable self-describing contract. It
will be parsed only on SD load or an explicit configuration update, outside the
audio interrupt, using a bounded tokenizer/validator. A line-oriented key/value
format would be cheaper to parse but would duplicate the web contract and make
ACL/schema evolution harder; it is not warranted for this device.

## Adversarial review resolutions

The clean-room review returned RED. These constraints resolve its findings before
implementation:

- The UI root is `/aius/www/`, with a maximum decoded path length of 96 bytes,
  maximum eight components, maximum 32 bytes per component, and only ASCII
  alphanumerics plus `.`, `_`, `-`, and `/`. Percent encoding, backslashes,
  empty components, `.` and `..` are rejected rather than normalized. The
  firmware never exposes directory enumeration or symlink traversal.
- There is one SD job at a time and one 512-byte SD transfer slice per main
  loop. HTTP body receipt, SD writes, response reads, rename/rollback, and
  utilization scans are separate job states. A disconnect cancels the job and
  removes only its uniquely named temporary file.
- Replacement uses a unique temporary name and rollback protocol: rename the
  existing target to a backup, rename the temporary file into place, restore
  the backup if the second rename fails, and delete the backup only after
  commit. The old target is never removed before a replacement is durable.
- The unauthenticated configuration exception is `PUT /api/config/public`, not
  direct `PUT /sd/config.json`. Its body is a bounded JSON object containing
  only the firmware-owned public ACL keys `features.sd_web`,
  `features.remote_control`, and `features.telemetry`. It performs merge-and-
  replace of those keys, rejects unknown or duplicate keys, and cannot modify
  DSP, network, auth, filesystem, or asset policy.
- `/aius/config.json` is parsed only when loaded or explicitly updated. The
  accepted initial schema is the feature object above, with safe defaults on
  missing or invalid data. Status retains `config_valid`,
  `config_error_code`, and `config_error_count` until the next successful load
  or reboot.
- The USB credential is a fixed-format per-device token derived from
  firmware-held device material and a compiled salt. The exact physical USB
  command is `AIUS AUTH\n`; it prints the token once per command and never over
  Ethernet. Asset writes and deletes require an exact constant-time comparison
  of `X-Aius-SD-Auth`; public-config writes do not. The token is not accepted
  for protected configuration or filesystem paths.

### Phase 8: self-hosted maintenance tooling

**Done** — the flash home page carries the token + directory-picker upload
form with the `config.json` refusal (`firmware/src/http_home_page.cpp:34-43`),
and `firmware/tools/aius-sd-sync.sh` is the standalone uploader (additive
sync; its header carries the don't-clobber-presets note).

**unit:** device-hosted upload form and standalone shell uploader  
**seams:** browser/file picker ↔ authenticated asset PUT; shell file tree ↔ device API  
**contract-artifact:** upload form served by the device; `tools/aius-sd-sync.sh`  
**fan-out:** embedded UI owns the form; shell tool owns transport and token input; OpenAPI owns examples  
**review-intensity:** nested paths, auth failures, config ACL separation, partial sync, missing tools, and no-repo execution  
**commit-checkpoint:** shell syntax check, mocked HTTP integration tests, and target upload smoke test

The device home page will include an upload form and exact curl-compatible
request examples. The standalone script will accept a device URL, a local
`www/` tree, and an auth token obtained over USB; it will upload files using
standard HTTP PUT with `Content-Length`, preserve relative paths, and refuse to
send `config.json` through the asset path. It will run from a copied file on a
maintenance host and will not depend on Python, PlatformIO, or this repository.
