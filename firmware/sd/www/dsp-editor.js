/* Aius SD-hosted DSP control surface.
 *
 * OWNS: the whole /sd/www/index.html page -- building the config form from the
 * field table below, rendering GET /api/dsp/config into it, validating edits
 * against the firmware parser's bounds, PUTting the model back with an
 * optimistic-concurrency token, polling GET /api/meters at 10 Hz and
 * GET /api/status at 1 Hz, and drawing the signal-flow strip: ONE stacked EQ
 * plot carrying every cascade, the downmix routing diagram, the level bars, the
 * DSP load and the limiter's gain reduction, all from that same model and those
 * same snapshots.
 *
 * DOES NOT OWN: the replay manager (replays.js), the device status page
 * (status.js), the shared chrome behaviours (sd-shared.js -- pills, fetch
 * discipline, formatters, token storage, waveform renderer; consumed here,
 * never mutated), and anything the firmware decides -- the device is the
 * authority on what a valid config is. The bounds here mirror
 * lib/aius_dsp_config (and scripts/build_config.py) so a mistake is caught
 * before the round trip, never to replace the device-side validation.
 *
 * This file is served off the Teensy's SD card. It is deliberately a plain
 * classic script with no imports, no build step, and no shared UI kit -- the
 * device cannot reach a host application's module mounts. Same constraint as
 * replays.js; keep the SD pages looking like one product.
 */
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);

  /* index.html loads sd-shared.js first; this is the whole of the page's
   * dependency on it. */
  const shared = window.aiusSdShared;

  const METER_POLL_MS = 100; /* 10 Hz, per the API contract */
  const METER_RETRY_MS = 5000; /* back-off while /api/meters is absent */
  const STATUS_POLL_MS = 1000; /* the signal-flow ends only need 1 Hz */
  const PENDING_POLL_MS = 500; /* the apply lands at an audio block boundary */
  const PENDING_POLL_MAX = 40; /* ~20 s before the page stops waiting */
  const MODEL_MAX_BYTES = 12288; /* AIUS_DSP_MODEL_MAX_JSON_BYTES */
  const EQ_SLOTS = 2; /* AIUS_DSP_MODEL_EQ_MAX */
  const DISPLAY_SIG_FIGS = 5; /* what a rendered number shows, never what it sends */
  const ERROR_LINK_MAX = 4; /* linked problems in the summary before "+N more" */

  /* Signal-flow strip. The curve axes are the page's own display choice; the
   * gain axis deliberately equals the gain_db field's INPUT bound, so a drag
   * handle can reach either rail and no reachable pixel is an invalid value.
   * That bound is NARROWER than the firmware's (+-12 against +-24, operator
   * request 2026-07-28): a config carrying a wider gain draws off the top or
   * bottom of the plot rather than being clamped behind the operator's back --
   * the field still holds the value and validation still passes it. */
  const CURVE_MIN_HZ = 20;
  const CURVE_MAX_HZ = 20000;
  const CURVE_SPAN_DB = 12; /* the +-12 dB gain_db input bound in eqFields() */
  const CURVE_FLOOR_DB = -120; /* a highpass is -inf at DC; floor it so the polyline stays finite */
  const CURVE_STEPS = 192; /* polyline segments across the log-frequency axis */
  const CURVE_GRID_HZ = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  const CURVE_GRID_DB = [-12, -6, 0, 6, 12];
  const CURVE_DEFAULT_RATE_HZ = 48000; /* until GET /api/dsp/config reports the device rate */
  const CURVE_DIM_ALPHA = 0.3; /* an unselected cascade is context, not a target */
  const HZ_LABEL_INSET_PX = 34; /* the 20 Hz label clears the bottom dB rail label */
  const HANDLE_RADIUS_PX = 6;
  const HANDLE_GRAB_PX = 14; /* pointer slop around a handle, in client pixels */
  const Q_WHEEL_STEP = 1.12; /* one wheel notch, multiplicative */
  const Q_MIN = 0.1; /* both mirror the Q field bound in eqFields() */
  const Q_MAX = 20;
  const NUDGE_DB = 0.5; /* one arrow press, matching the dB field's step */
  const NUDGE_HZ_RATIO = 1.05; /* proportional, because the axis is logarithmic */
  const DOUBLE_TAP_MS = 400; /* touch has no dblclick of its own to rely on */
  const DOUBLE_TAP_PX = 30;
  /* Downmix routing diagram. The picture IS the firmware's downmix sum
   * (aius_dsp.c: FL into L and FR into R at unity, FC into both at `center`, the
   * LR4-low-passed LFE into both at `lfe`, SL into L and SR into R at
   * `surround`, the pair then scaled by `headroom`), so every path on it is one
   * term of that sum and there are no others. Columns and the value track are
   * FRACTIONS of the canvas rect, so one set of maths serves the DPR-scaled
   * backing store and the CSS-sized client rect alike. */
  const MATRIX_SOURCE_X = 0.09; /* the six decoded channels, stacked */
  const MATRIX_OUTPUT_X = 0.79; /* L and R, after the mix */
  const MATRIX_TRACK_TOP = 0.11; /* a handle's own rail: its field maximum ... */
  const MATRIX_TRACK_BOTTOM = 0.89; /* ... and its field minimum */
  const MATRIX_OUTPUT_Y = [0.34, 0.66]; /* L and R, as fractions of that track */
  const MATRIX_NODE_PX = 9;
  const MATRIX_HANDLE_RADIUS_PX = 7;
  const MATRIX_GRAB_PX = 16; /* pointer slop around a handle, in client pixels */
  const MATRIX_PATH_MIN_PX = 0.8; /* a silent path is still a path, just barely */
  const MATRIX_PATH_MAX_PX = 5;
  const MATRIX_PATH_MIN_ALPHA = 0.35;
  const MATRIX_MUTED_ALPHA = 0.28;
  const MATRIX_WHEEL_STEP = 1.12; /* one wheel notch, multiplicative, like Q */
  const MATRIX_WHEEL_SEED = 0.05; /* a muted path has no ratio to scale */
  const MATRIX_NUDGE = 0.01; /* one arrow press, matching the fields' own step */
  /* The crossover label's hit box is AUTHORED, not measured: measureText is a
   * rendering-context fact, and this geometry has to be provable without one. */
  const MATRIX_LABEL_W_PX = 104;
  const MATRIX_LABEL_H_PX = 18;
  const BAR_FLOOR_DBFS = -60; /* the bar strip's own floor: at -120 every bar is a sliver */
  const BAR_WARN_DBFS = -6; /* above this a level is close enough to clipping to say so */
  const BAR_DANGER_DBFS = -1;
  const LOAD_WARN_PERCENT = 70;
  const LOAD_DANGER_PERCENT = 90;
  const LIMITER_SPAN_DB = 24; /* gain-reduction bar + history full scale */
  const LIMITER_HISTORY_MAX = 128; /* ~12.8 s of history at the 10 Hz poll */

  /* Firmware facts mirrored here the way MODEL_MAX_BYTES mirrors
   * AIUS_DSP_MODEL_MAX_JSON_BYTES: the snapshot carries raw DWT cycle counts
   * and nothing on the wire says what one block may spend.
   *   600 MHz -- board_build.f_cpu in firmware/platformio.ini
   *   512     -- AIUS_PIPELINE_PCM_BLOCK_FRAMES, the DECODED block the DWT
   *              bracket in enqueue() measures (optical_main.cpp: "512 frames /
   *              48 kHz = 10,667 us (6,400,000 cycles at 600 MHz)")
   * so the per-block budget is cpu_hz * block_frames / sample_rate_hz. */
  const DSP_CPU_HZ = 600e6;
  const DSP_BLOCK_FRAMES = 512;

  const CHANNELS = [
    ['fl', 'Front left'],
    ['fr', 'Front right'],
    ['fc', 'Front centre'],
    ['lfe', 'LFE'],
    ['sl', 'Surround left'],
    ['sr', 'Surround right'],
  ];
  const FEATURES = [
    ['input_eq', 'Input EQ'],
    ['matrix', 'Downmix matrix'],
    ['output_eq', 'Output EQ'],
    ['limiter', 'Limiter'],
    ['meters', 'Meters'],
  ];
  /* "none" is a page-level state, not a wire value: the section is absent from
   * the eq[] array. "bypass" is a real firmware EQ type -- a section that is
   * present but does nothing -- so both round-trip faithfully. */
  const EQ_TYPES = [
    ['none', 'none (no section)'],
    ['bypass', 'bypass'],
    ['highpass', 'highpass'],
    ['peaking', 'peaking'],
  ];
  const EQ_SEED = { frequency_hz: 1000, q: 0.707, gain_db: 0 };

  /* The five entities of the stacked plot. A STEREO PAIR IS ONE ENTITY here
   * (task #7 slice 5, user requirement): fl/fr and sl/sr are never separately
   * controlled on this surface, so every edit through it writes both members'
   * fields and the per-channel form below stays the escape hatch. The first
   * base is the LEFT member and is the display authority: a pair whose members
   * differ in the model (a hand-edited config) renders the left member's
   * cascade, shows an "unlinked" chip marker, and the first surface edit adopts
   * the left cascade into both -- announced before it applies. */
  const CURVE_ENTITIES = [
    { key: 'front', label: 'Front (fl+fr)', bases: ['input_channels.fl', 'input_channels.fr'], colour: '#74d7ff' },
    { key: 'centre', label: 'Centre (fc)', bases: ['input_channels.fc'], colour: '#73e6a0' },
    { key: 'lfe', label: 'LFE', bases: ['input_channels.lfe'], colour: '#e8c46a' },
    { key: 'surround', label: 'Surround (sl+sr)', bases: ['input_channels.sl', 'input_channels.sr'], colour: '#c79bf0' },
    { key: 'output', label: 'Output', bases: ['output'], colour: '#ff9f6b' },
  ];

  /* The strip has ONE colour key: the downmix diagram names its paths with the
   * same colour the legend gives that channel group, so a reader learns it
   * once. */
  const entityColour = (key) => CURVE_ENTITIES.find((entity) => entity.key === key).colour;

  /* The factory model's five editable groups, mirroring
   * firmware/config/optical_dsp.json (compiled into AIUS_OPTICAL_CONFIG by
   * scripts/build_config.py). Per-parameter reset reads this table. The chain
   * that keeps it honest: the reset-all test asserts the collected model equals
   * the dsp-config-default.json fixture, and the fixture is asserted equal to
   * optical_dsp.json itself -- so a factory change this table misses fails the
   * suite instead of shipping wrong "defaults". */
  const FACTORY_DEFAULTS = {
    features: { input_eq: true, matrix: true, output_eq: true, limiter: true, meters: true },
    input_channels: {
      fl: { gain_db: 0, eq: [] },
      fr: { gain_db: 0, eq: [] },
      fc: { gain_db: 0, eq: [] },
      lfe: { gain_db: 0, eq: [] },
      sl: { gain_db: 0, eq: [] },
      sr: { gain_db: 0, eq: [] },
    },
    output_eq: [],
    matrix: {
      center_gain_linear: 0.7071067811865475,
      surround_gain_linear: 0.7071067811865475,
      lfe_gain_linear: 0.31622776601683794,
      lfe_crossover_hz: 120,
      headroom_linear: 0.5,
      clipfree: false,
    },
    limiter: { enabled: true, threshold_dbfs: -1, attack_ms: 0.5, release_ms: 100, lookahead_ms: 2 },
  };

  const state = {
    model: null,
    baseGeneration: 0,
    applyState: 'loading',
    pendingTimer: null,
    pendingAttempts: 0,
    meterInFlight: false,
    meterRetryAt: 0,
    statusInFlight: false,
    /* Serialised editable groups as last read from the device; null until the
     * first successful read. isDirty() is this string against a fresh collect. */
    clean: null,
    /* One request at a time from the page's own buttons (UX finding E4). */
    busy: false,
    /* Names as stored on the card (with case), .json stripped. */
    presets: [],
    /* True when the last listing FAILED: state.presets is then unknown rather
     * than empty, and a case-insensitive duplicate check cannot be trusted. */
    presetsFaulted: false,
    /* Which preset the form was last filled from -- {name, snapshot} -- or
     * null when the form came from the device instead. The snapshot is the
     * serialised groups as they arrived, so "modified" is a comparison rather
     * than a flag something has to remember to set. */
    presetOrigin: null,
    /* Which cascade the stacked plot is editing, and which of its sections the
     * keyboard and the wheel act on. */
    curveEntity: CURVE_ENTITIES[0].key,
    activeSlot: 0,
    /* Which downmix handle the keyboard and the wheel act on. */
    matrixHandle: 'center',
    /* Field paths that currently fail validation, for the card markers. */
    problems: new Map(),
    /* Page-side ring of polled limiter_gain_reduction_db values; the device
     * publishes an instant, never a history. */
    limiterHistory: [],
    /* Last rendered waveform, so a resize can redraw without a fresh poll. */
    waveform: [],
    /* The handle currently under a captured pointer, or null -- one per canvas,
     * because the plot and the diagram are two independent instruments. */
    drag: null,
    matrixDrag: null,
    lastTap: null,
  };

  /* Preset name grammar, page-enforced BEFORE any PUT. The 27-char cap is the
   * SD path grammar's 32-byte segment minus ".json". The `config` suffix
   * exclusion exists because the firmware's resource_is_config() is a bare
   * suffix match on "config.json": a preset named myconfig would 403 on PUT
   * and DELETE while GET still serves it -- a file you can see but never
   * change. Case-insensitive, because FAT lookup is case-insensitive too. */
  const PRESET_NAME_MAX = 27;
  const PRESET_DIR_URL = '/sd/www/presets/';
  const PRESET_GROUPS = ['features', 'input_channels', 'output_eq', 'matrix', 'limiter'];

  /* ---- field table -------------------------------------------------------
   * Every editable value is one descriptor whose `path` is its JSON path in the
   * model. Render and collect both walk this table, so a field can never be
   * shown without being sent (or vice versa). `step` is the NATIVE stepping
   * granularity (UX finding E8: step="any" made ArrowUp take Q from 0.707 to
   * 1.707); min/max are emitted natively too, and JS validation stays the
   * authority -- the device's bounds are what a message must describe.
   *
   * `min`/`max` are what the EDITOR offers: native stepping, the visual rails
   * and every gesture stop there. `deviceMin`/`deviceMax` are the FIRMWARE's
   * bounds and default to them; a descriptor states them separately only where
   * the editor deliberately offers a narrower working range than the device
   * accepts, so a config already carrying a wider value stays valid. */

  const eqFields = (base, label) => [
    { path: `${base}.type`, kind: 'eq-type', label: `${label} type`, caption: 'Type' },
    {
      path: `${base}.frequency_hz`,
      kind: 'number',
      label: `${label} frequency`,
      caption: 'Frequency (Hz)',
      min: 10,
      max: 24000,
      step: 1,
      unit: ' Hz',
    },
    { path: `${base}.q`, kind: 'number', label: `${label} Q`, caption: 'Q', min: 0.1, max: 20, step: 0.05 },
    {
      path: `${base}.gain_db`,
      kind: 'number',
      label: `${label} gain`,
      caption: 'Gain (dB)',
      /* +-12 dB is the working range this surface offers (operator request
       * 2026-07-28); +-24 is EQ_GAIN_{MIN,MAX}_DB in lib/aius_dsp_config, so a
       * model written before this range narrowed -- or by hand -- renders,
       * validates and applies untouched. */
      min: -12,
      max: 12,
      step: 0.5,
      deviceMin: -24,
      deviceMax: 24,
      unit: ' dB',
    },
  ];

  const MATRIX_FIELDS = [
    {
      path: 'matrix.center_gain_linear',
      kind: 'number',
      label: 'Centre gain',
      caption: 'Centre gain (linear)',
      min: 0,
      max: 4,
      step: 0.01,
      unit: ' linear',
      note: 'db',
    },
    {
      path: 'matrix.surround_gain_linear',
      kind: 'number',
      label: 'Surround gain',
      caption: 'Surround gain (linear)',
      min: 0,
      max: 4,
      step: 0.01,
      unit: ' linear',
      note: 'db',
    },
    {
      path: 'matrix.lfe_gain_linear',
      kind: 'number',
      label: 'LFE gain',
      caption: 'LFE gain (linear)',
      min: 0,
      max: 4,
      step: 0.01,
      unit: ' linear',
      note: 'db',
    },
    /* 20..500 mirrors MATRIX_LFE_CROSSOVER_{MIN,MAX}_HZ in the PUT validator. 0 is
     * deliberately OUTSIDE that range: "no LFE" is lfe_gain_linear = 0, so a corner
     * of zero would be a second encoding of one state. */
    {
      path: 'matrix.lfe_crossover_hz',
      kind: 'number',
      label: 'LFE crossover',
      caption: 'LFE crossover (Hz)',
      min: 20,
      max: 500,
      step: 1,
      unit: ' Hz',
    },
    {
      path: 'matrix.headroom_linear',
      kind: 'number',
      label: 'Headroom',
      caption: 'Headroom (linear)',
      min: 0,
      max: 1,
      step: 0.01,
      exclusiveMin: true,
      unit: ' linear',
    },
    { path: 'matrix.clipfree', kind: 'bool', label: 'Clip-free headroom policy' },
  ];

  /* The diagram's bounds are the FORM's bounds, read back out of the descriptor
   * table rather than restated: no reachable pixel may be a value the field
   * would then mark out of range. An exclusive minimum has no legal value AT
   * its rail, so the field's own step is the lowest thing a gesture may write --
   * the smallest value its native stepping could have produced. */
  function matrixBound(path) {
    const descriptor = MATRIX_FIELDS.find((entry) => entry.path === path);
    return { min: descriptor.exclusiveMin ? descriptor.step : descriptor.min, max: descriptor.max };
  }

  /* One draggable per editable term of the downmix. A term used on BOTH sides
   * (centre, the LFE fold-in) or by both surround channels is ONE handle,
   * because the model has one field for it -- the same "a pair is one thing on
   * this surface" contract the stacked plot follows. Array order is the
   * keyboard's cycle order; `column` is where the handle rides, so two equal
   * gains never land on the same point. */
  const MATRIX_HANDLES = [
    { key: 'center', path: 'matrix.center_gain_linear', label: 'Centre', column: 0.42, colour: entityColour('centre') },
    {
      key: 'surround',
      path: 'matrix.surround_gain_linear',
      label: 'Surround',
      column: 0.62,
      colour: entityColour('surround'),
    },
    { key: 'lfe', path: 'matrix.lfe_gain_linear', label: 'LFE', column: 0.52, colour: entityColour('lfe') },
    { key: 'headroom', path: 'matrix.headroom_linear', label: 'Headroom', column: 0.93, colour: entityColour('output') },
  ].map((handle) => ({ ...handle, bound: matrixBound(handle.path) }));

  const matrixHandleOf = (key) => MATRIX_HANDLES.find((handle) => handle.key === key);

  const MATRIX_GAIN_BOUND = matrixBound('matrix.center_gain_linear');
  const MATRIX_OUTPUTS = [
    ['l', 'L'],
    ['r', 'R'],
  ];

  /* Every term of the downmix sum, in the order the firmware adds them. A route
   * with no handle is a FIXED unity reference: the matrix has no field for it,
   * so the diagram draws it and refuses to edit it. SL and SR share one gain
   * field (one handle, both paths through one point) but carry their own path
   * colours (2026-07-27 feedback, item 8): the split says "two channels" while
   * the shared point keeps saying "one bonded value". */
  const SR_PATH_COLOUR = '#e88fd2';
  const MATRIX_ROUTES = [
    { source: 'fl', output: 'l', handle: null },
    { source: 'fr', output: 'r', handle: null },
    { source: 'fc', output: 'l', handle: 'center' },
    { source: 'fc', output: 'r', handle: 'center' },
    { source: 'lfe', output: 'l', handle: 'lfe' },
    { source: 'lfe', output: 'r', handle: 'lfe' },
    { source: 'sl', output: 'l', handle: 'surround' },
    { source: 'sr', output: 'r', handle: 'surround', colour: SR_PATH_COLOUR },
  ];

  const LIMITER_FIELDS = [
    { path: 'limiter.enabled', kind: 'bool', label: 'Limiter enabled', caption: 'Limiter enabled' },
    {
      path: 'limiter.threshold_dbfs',
      kind: 'number',
      label: 'Threshold',
      caption: 'Threshold (dBFS)',
      min: -24,
      max: 0,
      step: 0.5,
      unit: ' dBFS',
    },
    { path: 'limiter.attack_ms', kind: 'number', label: 'Attack', caption: 'Attack (ms)', min: 0.1, max: 100, step: 0.1, unit: ' ms' },
    { path: 'limiter.release_ms', kind: 'number', label: 'Release', caption: 'Release (ms)', min: 1, max: 1000, step: 1, unit: ' ms' },
    {
      path: 'limiter.lookahead_ms',
      kind: 'number',
      label: 'Lookahead',
      caption: 'Lookahead (ms)',
      min: 0,
      max: 5,
      step: 0.1,
      unit: ' ms',
    },
  ];

  /* Which card owns a field, so a validation message can put a marker on the
   * title the reader has to scroll to (UX finding E6). */
  const CARD_OF_PREFIX = [
    ['features.', 'card-features'],
    ['input_channels.', 'card-input-channels'],
    ['output.', 'card-output-eq'],
    ['matrix.', 'card-matrix'],
    ['limiter.', 'card-limiter'],
  ];
  const CARD_IDS = CARD_OF_PREFIX.map(([, id]) => id);
  const cardIdOf = (path) => (CARD_OF_PREFIX.find(([prefix]) => path.startsWith(prefix)) || [null, null])[1];

  const field = (path) => $(`f:${path}`);

  /* ---- DOM construction -------------------------------------------------- */

  function buildInput(descriptor) {
    let input;
    if (descriptor.kind === 'eq-type') {
      input = document.createElement('select');
      for (const [value, text] of EQ_TYPES) {
        const option = document.createElement('option');
        option.value = value;
        option.textContent = text;
        input.append(option);
      }
    } else if (descriptor.kind === 'bool') {
      input = document.createElement('input');
      input.type = 'checkbox';
    } else {
      input = document.createElement('input');
      input.type = 'number';
      /* Native bounds and stepping, from the same descriptor the JS validator
       * reads -- so ArrowUp lands on a legal value and the browser's own UI
       * agrees with the message the page would show. */
      input.min = String(descriptor.min);
      input.max = String(descriptor.max);
      input.step = String(descriptor.step);
      /* What fieldProblem() answers to: the DEVICE's bound, which is the same
       * number for every field but the ones that narrow their input range. */
      input.dataset.min = String(descriptor.deviceMin ?? descriptor.min);
      input.dataset.max = String(descriptor.deviceMax ?? descriptor.max);
      if (descriptor.exclusiveMin) input.dataset.exclusiveMin = 'true';
      if (descriptor.unit) input.dataset.unit = descriptor.unit;
    }
    input.id = `f:${descriptor.path}`;
    input.dataset.field = descriptor.path;
    input.dataset.label = descriptor.label;
    input.dataset.kind = descriptor.kind;
    input.setAttribute('aria-describedby', `e:${descriptor.path}`);
    return input;
  }

  function buildError(path) {
    const error = document.createElement('span');
    error.className = 'field__error';
    error.id = `e:${path}`;
    return error;
  }

  /* Per-parameter reset to the factory value. tabindex -1 keeps the crowd of
   * them out of the tab order; the aria-label still names the target for AT
   * that reaches it another way. */
  function buildReset(descriptor) {
    const reset = document.createElement('button');
    reset.type = 'button';
    reset.className = 'field__reset';
    reset.id = `r:${descriptor.path}`;
    reset.tabIndex = -1;
    reset.textContent = 'reset';
    reset.setAttribute('aria-label', `Reset ${descriptor.label} to factory default`);
    reset.addEventListener('click', () => resetField(descriptor.path));
    return reset;
  }

  function buildField(descriptor) {
    /* A DIV, not a LABEL: a wrapping label folds caption, error text and the
     * reset button into one accessible name (UX finding E11). The caption is a
     * real <label for>, and the error is wired with aria-describedby. */
    const holder = document.createElement('div');
    holder.className = 'field';
    /* The caption is the compact form; `label` is the full subject a validation
     * message names ("Front left section 1 frequency must be ..."). */
    const caption = document.createElement('label');
    caption.className = 'field__label';
    caption.htmlFor = `f:${descriptor.path}`;
    caption.textContent = descriptor.caption || descriptor.label;
    holder.append(caption);

    if (descriptor.kind === 'bool') holder.classList.add('field--check');
    holder.append(buildInput(descriptor), buildError(descriptor.path));

    /* A linear gain says nothing about level to a human, so the dB it means is
     * printed under it (UX finding E7). */
    if (descriptor.note === 'db') {
      const note = document.createElement('span');
      note.className = 'field__note';
      note.id = `n:${descriptor.path}`;
      holder.append(note);
    }

    holder.append(buildReset(descriptor));
    return holder;
  }

  /* An absent value (an EQ path the factory model has no section for) resets a
   * number to empty and a type select to the page-level "none", exactly what
   * render() shows for the same absent value. */
  function resetField(path) {
    const input = field(path);
    if (input.disabled) return;
    const value = factoryValueOf(FACTORY_DEFAULTS, path);
    if (input.dataset.kind === 'bool') input.checked = Boolean(value);
    else if (input.dataset.kind === 'eq-type') input.value = typeof value === 'string' ? value : 'none';
    else setNumberValue(input, Number.isFinite(value) ? value : NaN);
    input.dispatchEvent(new Event('input'));
  }

  function syncResetButtons() {
    for (const input of document.querySelectorAll('[data-field]')) {
      const reset = $(`r:${input.dataset.field}`);
      if (reset) reset.disabled = input.disabled;
    }
  }

  function buildFieldGrid(descriptors) {
    const grid = document.createElement('div');
    grid.className = 'field-grid';
    for (const descriptor of descriptors) grid.append(buildField(descriptor));
    return grid;
  }

  /* ---- the dense EQ table (2026-07-27 feedback, item 1) -------------------
   * One grid for all six input channels (rows = channel x section) and a
   * two-row sibling for the output EQ. The cells carry the SAME inputs -- ids,
   * data-field wiring, validation -- the stacked field cards used to; only the
   * chrome is denser. Captions live once in the header row, so each input
   * carries the full aria-label instead of a <label for>, and the error line
   * surfaces on demand instead of reserving a row of its own. */

  /* Short forms: the full words overlap their neighbours at tablet width, and
   * the aria-labels carry the full subject anyway. */
  const EQ_COLUMN_HEADS = ['Type', 'Freq (Hz)', 'Q', 'Gain (dB)'];

  function tableHead(text) {
    const head = document.createElement('span');
    head.className = 'eq-table__h';
    head.textContent = text;
    return head;
  }

  function buildTableCell(descriptor) {
    const holder = document.createElement('div');
    holder.className = 'field field--cell';
    const input = buildInput(descriptor);
    input.setAttribute('aria-label', descriptor.label);
    holder.append(input, buildError(descriptor.path));
    return holder;
  }

  /* Factory reset for one whole section row (feedback item 2): the type
   * resets first, so the factory type decides which value fields are enabled
   * before they reset in turn -- the same order a hand reset would use. */
  function resetEqSection(base, slot) {
    for (const key of ['type', 'frequency_hz', 'q', 'gain_db']) resetField(`${base}.eq.${slot}.${key}`);
  }

  function buildSectionReset(base, slot, label) {
    const reset = document.createElement('button');
    reset.type = 'button';
    reset.className = 'field__reset eq-table__reset';
    reset.id = `sr:${base}.eq.${slot}`;
    reset.tabIndex = -1;
    reset.textContent = 'reset';
    reset.setAttribute('aria-label', `Reset ${label} section ${slot + 1} to factory default`);
    reset.addEventListener('click', () => resetEqSection(base, slot));
    return reset;
  }

  function appendSectionRow(grid, base, label, slot) {
    const section = document.createElement('span');
    section.className = 'eq-table__sec';
    section.textContent = String(slot + 1);
    grid.append(section);
    for (const descriptor of eqFields(`${base}.eq.${slot}`, `${label} section ${slot + 1}`)) {
      grid.append(buildTableCell(descriptor));
    }
    grid.append(buildSectionReset(base, slot, label));
  }

  function buildInputChannelTable() {
    const wrap = document.createElement('div');
    wrap.className = 'eq-table-wrap';
    const grid = document.createElement('div');
    grid.className = 'eq-table eq-table--channels';
    for (const text of ['Channel', 'Trim (dB)', 'Sec', ...EQ_COLUMN_HEADS, '']) grid.append(tableHead(text));
    for (const [key, label] of CHANNELS) {
      const name = document.createElement('span');
      name.className = 'eq-table__channel';
      name.textContent = `${label} (${key})`;
      grid.append(name);
      const trim = buildTableCell({
        path: `input_channels.${key}.gain_db`,
        kind: 'number',
        label: `${label} trim`,
        min: -60,
        max: 12,
        step: 0.5,
        unit: ' dB',
      });
      trim.classList.add('field--trim');
      trim.append(buildReset({ path: `input_channels.${key}.gain_db`, label: `${label} trim` }));
      grid.append(trim);
      for (let slot = 0; slot < EQ_SLOTS; slot++) appendSectionRow(grid, `input_channels.${key}`, label, slot);
    }
    wrap.append(grid);
    return wrap;
  }

  function buildOutputEqTable() {
    const wrap = document.createElement('div');
    wrap.className = 'eq-table-wrap';
    const grid = document.createElement('div');
    grid.className = 'eq-table eq-table--output';
    for (const text of ['Sec', ...EQ_COLUMN_HEADS, '']) grid.append(tableHead(text));
    for (let slot = 0; slot < EQ_SLOTS; slot++) appendSectionRow(grid, 'output', 'Output', slot);
    wrap.append(grid);
    return wrap;
  }

  function buildForm() {
    const features = $('features');
    features.append(
      buildFieldGrid(
        FEATURES.map(([key, label]) => ({ path: `features.${key}`, kind: 'bool', label, caption: `${label} enabled` })),
      ),
    );

    $('input-channels').append(buildInputChannelTable());
    $('output-eq').append(buildOutputEqTable());
    $('matrix').append(buildFieldGrid(MATRIX_FIELDS));
    $('limiter').append(buildFieldGrid(LIMITER_FIELDS));

    for (const id of CARD_IDS) {
      const marker = document.createElement('span');
      marker.className = 'card-marker';
      marker.id = `m:${id}`;
      marker.hidden = true;
      $(id).querySelector('.section-title').append(marker);
    }

    buildMeterGroups();

    for (const input of document.querySelectorAll('[data-field]')) {
      input.addEventListener('input', () => onFieldInput(input));
      input.addEventListener('change', () => onFieldInput(input));
    }

    buildVisualStrip();
  }

  /* Live peak/RMS as grouped cards instead of an 8-row table (feedback item
   * 9): FRONT carries the four front-of-house decodes (LFE rides with the
   * fronts -- the three named groups leave it nowhere else), SURROUND the
   * pair, OUTPUT the post-mix stereo. Same cell ids the table used. */
  const METER_GROUPS = [
    [
      'Front',
      [
        ['meter-in-fl', 'FL', 'Front left'],
        ['meter-in-fr', 'FR', 'Front right'],
        ['meter-in-fc', 'FC', 'Front centre'],
        ['meter-in-lfe', 'LFE', 'LFE'],
      ],
    ],
    [
      'Surround',
      [
        ['meter-in-sl', 'SL', 'Surround left'],
        ['meter-in-sr', 'SR', 'Surround right'],
      ],
    ],
    [
      'Output',
      [
        ['meter-out-l', 'L', 'Output left'],
        ['meter-out-r', 'R', 'Output right'],
      ],
    ],
  ];

  function buildMeterCell(id, label, title) {
    const cell = document.createElement('div');
    cell.className = 'meter-cell';
    cell.title = title;
    const name = document.createElement('span');
    name.className = 'meter-cell__name';
    name.textContent = label;
    const peak = document.createElement('strong');
    peak.className = 'meter-cell__peak';
    peak.id = `${id}-peak`;
    peak.textContent = '--';
    const rms = document.createElement('span');
    rms.className = 'meter-cell__rms';
    rms.id = `${id}-rms`;
    rms.textContent = '--';
    cell.append(name, peak, rms);
    return cell;
  }

  function buildMeterGroups() {
    const mount = $('meter-groups');
    for (const [title, cells] of METER_GROUPS) {
      const group = document.createElement('section');
      group.className = 'meter-group';
      const head = document.createElement('div');
      head.className = 'meter-group__head';
      const name = document.createElement('h4');
      name.className = 'meter-group__title';
      name.textContent = title;
      const note = document.createElement('span');
      note.className = 'meter-group__note';
      note.textContent = 'peak / RMS dBFS';
      head.append(name, note);
      const row = document.createElement('div');
      row.className = 'meter-group__cells';
      for (const [id, label, full] of cells) row.append(buildMeterCell(id, label, full));
      group.append(head, row);
      mount.append(group);
    }
  }

  /* ---- model <-> DOM ----------------------------------------------------- */

  const readPath = (root, path) =>
    path.split('.').reduce((node, key) => (node === null || node === undefined ? undefined : node[key]), root);

  /* The form's output EQ paths are `output.eq.N.<key>` (they mirror the input
   * channels' shape), while the model -- and FACTORY_DEFAULTS with it -- keys
   * the same sections `output_eq[N]`. Without this alias a reset of an output
   * section silently read undefined; correct only while the factory ships no
   * output sections, which is exactly the kind of latent break the delivery
   * audit flagged. Pure, and exported, so a synthetic factory table with a real
   * output section can prove the path. */
  function factoryValueOf(defaults, path) {
    const match = /^output\.eq\.(\d+)\.([a-z_]+)$/.exec(path);
    if (!match) return readPath(defaults, path);
    const sections = defaults && Array.isArray(defaults.output_eq) ? defaults.output_eq : [];
    const section = sections[Number(match[1])];
    return section ? section[match[2]] : undefined;
  }

  /* What a number LOOKS like in a field: 5 significant figures, so a downmix
   * gain reads 0.70711 instead of 0.7071067811865475 (UX finding E7). The
   * device still receives full precision -- setNumberValue parks the exact
   * value on the input and numberOf() takes it back while the visible text is
   * still that value's own rendering, so only a human's edit loses digits. */
  const displayNumber = (value) => {
    if (!Number.isFinite(value)) return '';
    return String(Number(value.toPrecision(DISPLAY_SIG_FIGS)));
  };

  function setNumberValue(input, value) {
    input.value = displayNumber(value);
    input.dataset.exact = Number.isFinite(value) ? String(value) : '';
  }

  const setNumberField = (path, value) => setNumberValue(field(path), value);

  function render(model) {
    for (const [key] of FEATURES) field(`features.${key}`).checked = Boolean(readPath(model, `features.${key}`));

    for (const [key] of CHANNELS) {
      setNumberField(`input_channels.${key}.gain_db`, readPath(model, `input_channels.${key}.gain_db`));
      renderEqSlots(`input_channels.${key}`, readPath(model, `input_channels.${key}.eq`));
    }
    renderEqSlots('output', model.output_eq);

    for (const descriptor of [...MATRIX_FIELDS, ...LIMITER_FIELDS]) {
      const input = field(descriptor.path);
      const value = readPath(model, descriptor.path);
      if (descriptor.kind === 'bool') input.checked = Boolean(value);
      else setNumberValue(input, value);
    }
    refreshDerived();
    validateAll();
  }

  function renderEqSlots(base, sections) {
    const list = Array.isArray(sections) ? sections : [];
    for (let slot = 0; slot < EQ_SLOTS; slot++) {
      const section = list[slot];
      const path = `${base}.eq.${slot}`;
      field(`${path}.type`).value = section && typeof section.type === 'string' ? section.type : 'none';
      setNumberField(`${path}.frequency_hz`, section ? section.frequency_hz : NaN);
      setNumberField(`${path}.q`, section ? section.q : NaN);
      setNumberField(`${path}.gain_db`, section ? section.gain_db : NaN);
    }
  }

  function eqSlotBases() {
    const bases = CHANNELS.map(([key]) => `input_channels.${key}`);
    bases.push('output');
    return bases;
  }

  /* An EQ section only carries the values its type uses; the firmware parser
   * bounds-checks frequency/Q/gain for non-bypass sections only, so the page
   * disables (and stops validating) exactly the inputs the device ignores. */
  function refreshEqEnablement() {
    for (const base of eqSlotBases()) {
      for (let slot = 0; slot < EQ_SLOTS; slot++) {
        const path = `${base}.eq.${slot}`;
        const type = field(`${path}.type`).value;
        const active = type === 'highpass' || type === 'peaking';
        field(`${path}.frequency_hz`).disabled = !active;
        field(`${path}.q`).disabled = !active;
        field(`${path}.gain_db`).disabled = type !== 'peaking';
      }
    }
  }

  function seedEqSection(base, slot) {
    const path = `${base}.eq.${slot}`;
    const type = field(`${path}.type`).value;
    if (type === 'none' || type === 'bypass') return;
    if (field(`${path}.frequency_hz`).value.trim() === '') {
      setNumberField(`${path}.frequency_hz`, EQ_SEED.frequency_hz);
    }
    if (field(`${path}.q`).value.trim() === '') setNumberField(`${path}.q`, EQ_SEED.q);
    if (type === 'highpass') setNumberField(`${path}.gain_db`, EQ_SEED.gain_db);
    else if (field(`${path}.gain_db`).value.trim() === '') setNumberField(`${path}.gain_db`, EQ_SEED.gain_db);
  }

  const linearToDb = (value) => (Number.isFinite(value) && value > 0 ? 20 * Math.log10(value) : null);

  /* Clip-free headroom is resolved by the DSP, not typed: with the policy on,
   * the field shows 1 / (1 + centre + surround + |lfe|) and goes read-only. */
  function refreshDerived() {
    refreshEqEnablement();
    const clipfree = field('matrix.clipfree').checked;
    const headroom = field('matrix.headroom_linear');
    headroom.disabled = clipfree;
    if (clipfree) {
      const denominator =
        1 +
        numberOf('matrix.center_gain_linear', 0) +
        numberOf('matrix.surround_gain_linear', 0) +
        Math.abs(numberOf('matrix.lfe_gain_linear', 0));
      setNumberValue(headroom, denominator <= 1 ? 1 : 1 / denominator);
    }
    for (const descriptor of MATRIX_FIELDS) {
      if (descriptor.note !== 'db') continue;
      const db = linearToDb(numberOf(descriptor.path, NaN));
      $(`n:${descriptor.path}`).textContent = db === null ? '= -inf dB' : `= ${db.toFixed(1)} dB`;
    }
    const bytes = JSON.stringify(collectModel()).length;
    $('model-bytes').textContent = `request body ${bytes} of ${MODEL_MAX_BYTES} bytes`;
    /* Both are facts about the form as it stands, so they move on the same
     * beat as the byte count: every edit -- typed, reset, preset-loaded or
     * dragged -- arrives here. */
    refreshDirtyPill();
    refreshPresetOrigin();
    syncResetButtons();
    /* The curves and the routing diagram are views of the fields, so every
     * edit -- typed, reset, preset-loaded or dragged -- redraws them from here,
     * the resolved clip-free headroom above included. collectModel() is
     * untouched: both read the inputs directly. */
    drawCurveSurface();
    drawMatrixSurface();
  }

  /* The exact value the field was RENDERED from wins while the visible text is
   * still that value's own 5-significant-figure rendering; the moment a human
   * types something else, what they typed is what the device gets. */
  function numberOf(path, fallback) {
    const input = field(path);
    const raw = input.value.trim();
    if (raw === '') return fallback;
    const exact = input.dataset.exact;
    if (exact !== undefined && exact !== '' && displayNumber(Number(exact)) === raw) return Number(exact);
    const value = Number(raw);
    return Number.isFinite(value) ? value : fallback;
  }

  function collectEqSection(base, slot) {
    const path = `${base}.eq.${slot}`;
    const type = field(`${path}.type`).value;
    if (type === 'none') return null;
    return {
      type,
      frequency_hz: numberOf(`${path}.frequency_hz`, 0),
      q: numberOf(`${path}.q`, 0),
      /* Only a peaking section carries gain; the firmware expects zero on the
       * others (a highpass gain outside +-24 dB is rejected outright). */
      gain_db: type === 'peaking' ? numberOf(`${path}.gain_db`, 0) : 0,
    };
  }

  const collectEqArray = (base) =>
    [...Array(EQ_SLOTS).keys()].map((slot) => collectEqSection(base, slot)).filter((section) => section !== null);

  /* The PUT wire shape: the eight keys GET returns plus base_generation. All
   * nine are required and "source" must be the literal "sd". */
  function collectModel() {
    const inputChannels = {};
    for (const [key] of CHANNELS) {
      inputChannels[key] = {
        gain_db: numberOf(`input_channels.${key}.gain_db`, 0),
        eq: collectEqArray(`input_channels.${key}`),
      };
    }
    const features = {};
    for (const [key] of FEATURES) features[key] = field(`features.${key}`).checked;
    return {
      source: 'sd',
      config_generation: state.baseGeneration,
      sample_rate_hz: state.model ? state.model.sample_rate_hz : 0,
      features,
      input_channels: inputChannels,
      output_eq: collectEqArray('output'),
      matrix: {
        center_gain_linear: numberOf('matrix.center_gain_linear', 0),
        surround_gain_linear: numberOf('matrix.surround_gain_linear', 0),
        lfe_gain_linear: numberOf('matrix.lfe_gain_linear', 0),
        /* Required, like every other matrix key -- the PUT validator refuses a
         * partial matrix. This body is built explicitly rather than round-tripped
         * from the GET, so a key missing HERE is a 400 on every save, which is
         * exactly what happened between the LFE fold-in landing and this line. */
        lfe_crossover_hz: numberOf('matrix.lfe_crossover_hz', 120),
        headroom_linear: numberOf('matrix.headroom_linear', 1),
        clipfree: field('matrix.clipfree').checked,
      },
      limiter: {
        enabled: field('limiter.enabled').checked,
        threshold_dbfs: numberOf('limiter.threshold_dbfs', 0),
        attack_ms: numberOf('limiter.attack_ms', 0),
        release_ms: numberOf('limiter.release_ms', 0),
        lookahead_ms: numberOf('limiter.lookahead_ms', 0),
      },
      base_generation: state.baseGeneration,
    };
  }

  /* ---- dirty tracking ----------------------------------------------------
   * "Dirty" is the five editable groups differing from what the device last
   * told us -- one JSON.stringify against a snapshot taken when the model was
   * adopted, cheap enough to run on demand and never per keystroke. Every
   * work-destroying path consults it (UX finding E2). */

  const markClean = () => {
    state.clean = JSON.stringify(collectPresetGroups());
    refreshDirtyPill();
  };

  const isDirty = () => state.clean !== null && state.clean !== JSON.stringify(collectPresetGroups());

  /* The same fact the confirm() guards and beforeunload have always consulted,
   * finally SAID on the page (operator request 2026-07-28). It rides the Apply
   * row because Apply is what resolves it, and it is absent rather than
   * negative when the form matches the device -- "no unsaved changes" is not
   * news. */
  function refreshDirtyPill() {
    $('dirty-state').hidden = !isDirty();
  }

  const DISCARD_QUESTION = 'This page has edits that have not been applied to the device. Discard them?';
  const confirmDiscard = () => !isDirty() || window.confirm(DISCARD_QUESTION);

  /* ---- validation -------------------------------------------------------- */

  function fieldProblem(input) {
    if (input.dataset.kind !== 'number' || input.disabled) return '';
    const label = input.dataset.label;
    const raw = input.value.trim();
    if (raw === '') return `${label} is required.`;
    const value = Number(raw);
    if (!Number.isFinite(value)) return `${label} must be a finite number.`;
    const min = Number(input.dataset.min);
    const max = Number(input.dataset.max);
    const unit = input.dataset.unit || '';
    if (input.dataset.exclusiveMin === 'true') {
      if (value <= min || value > max) return `${label} must be greater than ${min} and at most ${max}${unit}.`;
      return '';
    }
    if (value < min || value > max) return `${label} must be between ${min} and ${max}${unit}.`;
    return '';
  }

  function validateField(input) {
    const message = fieldProblem(input);
    const holder = input.parentElement;
    const error = holder.querySelector('.field__error');
    if (error) error.textContent = message;
    holder.classList.toggle('field--invalid', message !== '');
    input.setAttribute('aria-invalid', message === '' ? 'false' : 'true');
    if (message === '') state.problems.delete(input.dataset.field);
    else state.problems.set(input.dataset.field, message);
    markCards();
    return message;
  }

  /* A message at the top of a 4000 px page has to say WHERE, so each card
   * carries the count of its own out-of-range fields (UX finding E6). */
  function markCards() {
    const counts = new Map();
    for (const path of state.problems.keys()) {
      const card = cardIdOf(path);
      if (card) counts.set(card, (counts.get(card) || 0) + 1);
    }
    for (const id of CARD_IDS) {
      const count = counts.get(id) || 0;
      const marker = $(`m:${id}`);
      marker.textContent = count === 0 ? '' : `${count} out of range`;
      marker.hidden = count === 0;
      $(id).classList.toggle('card--invalid', count > 0);
    }
  }

  /* In DOM order, so "the first problem" is the first one down the page. */
  function validateAll() {
    const problems = [];
    for (const input of document.querySelectorAll('[data-field]')) {
      const message = validateField(input);
      if (message !== '') problems.push({ path: input.dataset.field, message });
    }
    return problems;
  }

  function onFieldInput(input) {
    const isEqType = input.dataset.kind === 'eq-type';
    if (isEqType) {
      const path = input.dataset.field.replace(/\.type$/, '');
      const slot = Number(path.slice(path.lastIndexOf('.') + 1));
      refreshEqEnablement();
      seedEqSection(path.slice(0, path.lastIndexOf('.eq.')), slot);
    }
    refreshDerived();
    /* The section type drives which of its values the device bounds-checks, so
     * a message left on a field the new type ignores would be a lie. */
    if (isEqType) validateAll();
    else validateField(input);
    /* `loaded` is promotable too: without it the first edit after a fresh read
     * left the pill claiming the form matched the device (delivery audit,
     * slice 3 finding). */
    if (state.applyState === 'applied' || state.applyState === 'error' || state.applyState === 'loaded') {
      setApplyState('edited');
    }
  }

  /* ---- device I/O --------------------------------------------------------
   * shared.request() never throws. Every caller below is fire-and-forget (a
   * click handler or a ticker) with nowhere to route a rejection, so a failure
   * comes back as data and the caller's own cleanup/render always runs.
   * guard() is the second belt: a defect anywhere in a handler surfaces in the
   * UI instead of becoming a silent unhandled rejection. */

  const request = shared.request;
  const requestRaw = shared.requestRaw;
  const describe = shared.describe;
  const guard = (run) => shared.guard(run, showError);

  /* One in-flight request at a time from the page's own buttons: a second
   * click on Apply during a >7 s first save used to land a stale PUT and paint
   * a false 409 (UX finding E4). */
  const BUSY_BUTTONS = [
    'apply',
    'reload',
    'stale-reload',
    'preset-save',
    'preset-download',
    'preset-upload',
    'preset-refresh',
  ];

  function setBusy(busy) {
    state.busy = busy;
    for (const id of BUSY_BUTTONS) $(id).disabled = busy;
    for (const button of document.querySelectorAll('#preset-list button')) button.disabled = busy;
  }

  async function act(run) {
    if (state.busy) return;
    setBusy(true);
    try {
      await guard(run);
    } finally {
      setBusy(false);
    }
  }

  async function loadConfig() {
    setStatus('Reading device configuration...');
    const result = await request('/api/dsp/config', { headers: { Accept: 'application/json' } });
    if (!result.ok || !result.body) {
      setApplyState('error');
      showError(`Could not read /api/dsp/config -- ${describe(result)}.`);
      setStatus('No configuration loaded. The controls below are not connected to the device.');
      return;
    }
    adoptModel(result.body);
    clearError();
    setStale(false);
    setApplyState('loaded');
    setStatus(`Loaded generation ${state.baseGeneration} from ${result.body.source}.`);
  }

  /* keepForm leaves the form exactly as the operator left it and adopts only
   * the device facts: the pending-window poll must not overwrite what was
   * typed while the previous apply was landing (delivery audit finding). */
  function adoptModel(model, keepForm) {
    state.model = model;
    state.baseGeneration = Number.isFinite(model.config_generation) ? model.config_generation : 0;
    $('source').textContent = model.source || 'unknown';
    $('generation').textContent = String(state.baseGeneration);
    $('sample-rate').textContent = Number.isFinite(model.sample_rate_hz) ? `${model.sample_rate_hz} Hz` : '--';
    if (keepForm) {
      refreshDerived();
      return;
    }
    render(model);
    markClean();
    /* The form is the device's now, not a preset's. The keepForm path above
     * returns before this on purpose: it keeps the operator's edits, so it
     * keeps where those edits came from too. */
    setPresetOrigin(null);
  }

  async function applyConfig() {
    clearError();
    const token = $('token').value.trim();
    if (!state.model) {
      showError('No configuration loaded yet -- re-read the device first.');
      return;
    }
    if (token === '') {
      showError('SD auth token required. Read it over USB with the AIUS AUTH command.');
      return;
    }
    const problems = validateAll();
    if (problems.length > 0) {
      setApplyState('invalid');
      showFieldProblems(problems);
      return;
    }
    const payload = JSON.stringify(collectModel());
    if (payload.length > MODEL_MAX_BYTES) {
      setApplyState('invalid');
      showError(`Model is ${payload.length} bytes; the device accepts at most ${MODEL_MAX_BYTES}.`);
      return;
    }

    setApplyState('sending');
    setStatus('Writing /aius/dsp.json...');
    const result = await request('/api/dsp/config', {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json', 'X-Aius-SD-Auth': token },
      body: payload,
    });
    if (result.status === 409) {
      setStale(true);
      setApplyState('stale');
      setStatus('Nothing was written: the device is at a newer generation than this page.');
      return;
    }
    if (!result.ok) {
      setApplyState('error');
      showError(`PUT /api/dsp/config rejected -- ${describe(result)}. SD and the active DSP are unchanged.`);
      setStatus('Configuration unchanged.');
      return;
    }
    setApplyState('pending');
    setStatus('Written to SD. Waiting for the apply at the next audio block boundary...');
    startPendingPoll();
  }

  function startPendingPoll() {
    stopPendingPoll();
    state.pendingAttempts = 0;
    state.pendingTimer = setInterval(() => void guard(pollPending), PENDING_POLL_MS);
  }

  function stopPendingPoll() {
    if (state.pendingTimer !== null) {
      clearInterval(state.pendingTimer);
      state.pendingTimer = null;
    }
  }

  async function pollPending() {
    state.pendingAttempts++;
    const result = await request('/api/dsp/config', { headers: { Accept: 'application/json' } });
    if (result.ok && result.body && result.body.config_generation !== state.baseGeneration) {
      stopPendingPoll();
      const dirty = isDirty();
      adoptModel(result.body, dirty);
      if (dirty) {
        setApplyState('edited');
        setStatus(
          `Applied at generation ${state.baseGeneration} (source ${result.body.source}). The device moved while this form was being edited, so the edits were kept -- re-read to see the device's values.`,
        );
        return;
      }
      setApplyState('applied');
      setStatus(`Applied at generation ${state.baseGeneration} (source ${result.body.source}).`);
      return;
    }
    if (state.pendingAttempts >= PENDING_POLL_MAX) {
      stopPendingPoll();
      setApplyState('error');
      showError('The write was accepted but no generation change appeared. Re-read the device to check.');
    }
  }

  /* ---- presets -----------------------------------------------------------
   * Presets ride the existing SD file surface -- zero firmware routes. The
   * list is the firmware's HTML directory index parsed with DOMParser; save
   * and delete are the auth-gated file PUT/DELETE; load fills the form and the
   * normal Apply flow writes to the device. A preset carries ONLY the five
   * editable groups: sample_rate_hz is a live biquad design rate nothing
   * cross-checks, so a preset must never smuggle one in. */

  function presetNameProblem(name) {
    if (name === '') return 'Preset name is required.';
    if (name.length > PRESET_NAME_MAX) return `Preset name is at most ${PRESET_NAME_MAX} characters.`;
    if (!/^[A-Za-z0-9._-]+$/.test(name)) {
      return 'Preset name may use letters, digits, dot, underscore and hyphen only.';
    }
    if (/config$/i.test(name)) {
      return 'Preset name may not end in "config": the device protects *config.json paths from writes.';
    }
    const collision = state.presets.find(
      (existing) => existing.toLowerCase() === name.toLowerCase() && existing !== name,
    );
    if (collision) {
      return `The card already has "${collision}", and SD names are case-insensitive -- saving "${name}" would overwrite it. Use that exact name to overwrite deliberately.`;
    }
    return '';
  }

  /* Sections the wire model bounds at EQ_SLOTS; anything deeper cannot round
   * back out of the form and would silently truncate on Apply. */
  function presetShapeProblem(parsed) {
    if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
      return 'Not a preset: the file is not a JSON object.';
    }
    for (const group of PRESET_GROUPS) {
      if (!(group in parsed)) return `Not a preset: missing "${group}".`;
    }
    if (!Array.isArray(parsed.output_eq) || parsed.output_eq.length > EQ_SLOTS) {
      return `Not a preset: output_eq must be an array of at most ${EQ_SLOTS} sections.`;
    }
    for (const [key] of CHANNELS) {
      const channel = parsed.input_channels ? parsed.input_channels[key] : undefined;
      if (!channel || !Array.isArray(channel.eq) || channel.eq.length > EQ_SLOTS) {
        return `Not a preset: input_channels.${key}.eq must be an array of at most ${EQ_SLOTS} sections.`;
      }
    }
    return '';
  }

  const collectPresetGroups = () => {
    const model = collectModel();
    return {
      features: model.features,
      input_channels: model.input_channels,
      output_eq: model.output_eq,
      matrix: model.matrix,
      limiter: model.limiter,
    };
  };

  const presetUrl = (name) => `${PRESET_DIR_URL}${encodeURIComponent(name)}.json`;

  function renderPresetList() {
    const list = $('preset-list');
    list.textContent = '';
    for (const name of state.presets) {
      const row = document.createElement('li');
      const title = document.createElement('span');
      title.className = 'preset-list__name';
      title.textContent = name;
      const load = document.createElement('button');
      load.type = 'button';
      load.textContent = 'Load';
      load.addEventListener('click', () => void act(() => loadPreset(name)));
      /* A browser save of the card's own bytes: a public GET and a blob, so no
       * token, no write, and nothing on the device moves. */
      const download = document.createElement('button');
      download.type = 'button';
      download.className = 'secondary';
      download.textContent = 'Download';
      download.addEventListener('click', () => void act(() => downloadPreset(name)));
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.className = 'danger';
      remove.textContent = 'Delete';
      remove.addEventListener('click', () => void act(() => deletePreset(name)));
      row.append(title, load, download, remove);
      list.append(row);
    }
  }

  async function refreshPresets() {
    clearPresetError();
    const result = await requestRaw(PRESET_DIR_URL, { headers: { Accept: 'text/html' } });
    /* Keyed on the TOKEN, not the status: 404 sd_not_found is simply "no
     * presets saved yet", while 404 sd_not_directory means a FILE named
     * presets is squatting on the directory name -- a real fault. */
    if (result.status === 404 && result.token === 'sd_not_found') {
      state.presets = [];
      state.presetsFaulted = false;
      renderPresetList();
      setPresetState('idle');
      setPresetStatus('No presets on the card yet.');
      return;
    }
    if (!result.ok) {
      state.presetsFaulted = true;
      setPresetState('error');
      showPresetError(`Could not list ${PRESET_DIR_URL} -- ${describe(result)}.`);
      return;
    }
    const parsed = new DOMParser().parseFromString(result.text, 'text/html');
    const names = [];
    for (const anchor of parsed.querySelectorAll('li > a')) {
      const href = anchor.getAttribute('href') || '';
      /* Directories (including the ../ row every listing below www/ carries)
       * end in a slash; presets are the .json files only. */
      if (href.endsWith('/') || !href.endsWith('.json')) continue;
      names.push(href.slice(0, -'.json'.length));
    }
    names.sort((a, b) => a.localeCompare(b));
    state.presets = names;
    state.presetsFaulted = false;
    renderPresetList();
    setPresetState('idle');
    const footer = result.text.match(/truncated: (yes|no)\./);
    if (footer && footer[1] === 'yes') {
      /* A partial list cannot rule out a case-variant collision either. */
      state.presetsFaulted = true;
      setPresetState('truncated');
      showPresetError(
        'The card holds more entries than one 64-entry listing; this list is INCOMPLETE. Prune presets to see them all.',
      );
    }
    setPresetStatus(`${names.length} preset${names.length === 1 ? '' : 's'} on the card.`);
  }

  /* The 507 sd_upload token covers BOTH an invalid path and a busy volume --
   * never present it as "card full", which is sd_capacity's meaning. */
  function describePresetWriteFailure(result) {
    if (result.token === 'sd_upload') {
      return 'the SD volume is busy (replay or upload in flight) or refused the path; retry when the card is idle';
    }
    if (result.token === 'sd_capacity') return 'the card is full';
    if (result.token === 'sd_auth') return 'the SD auth token was rejected';
    if (result.token === 'sd_unavailable') return 'no SD card is mounted';
    return describe(result);
  }

  async function savePreset(name, groups) {
    const token = $('token').value.trim();
    if (token === '') {
      showPresetError('SD auth token required to save a preset.');
      return;
    }
    /* An empty-because-it-faulted listing cannot answer "does Bass.json already
     * exist?", and FAT would silently case-overwrite it (delivery audit). */
    if (state.presetsFaulted) {
      setPresetState('error');
      showPresetError(
        'The preset list could not be read, so a case-insensitive overwrite cannot be ruled out. Refresh the list, then save.',
      );
      return;
    }
    const problem = presetNameProblem(name);
    if (problem !== '') {
      showPresetError(problem);
      return;
    }
    setPresetState('saving');
    setPresetStatus(`Writing ${name}.json to the card...`);
    const result = await requestRaw(presetUrl(name), {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json', 'X-Aius-SD-Auth': token },
      body: JSON.stringify(groups),
    });
    if (!result.ok) {
      setPresetState('error');
      showPresetError(`Preset not saved: ${describePresetWriteFailure(result)}.`);
      return;
    }
    /* Refresh first so the confirmation is the message left standing. */
    await refreshPresets();
    setPresetStatus(`Saved ${name}.json.`);
  }

  async function loadPreset(name) {
    clearPresetError();
    if (!confirmDiscard()) {
      setPresetStatus(`Kept the edits on this page; ${name} was not loaded.`);
      return;
    }
    const result = await requestRaw(presetUrl(name), { headers: { Accept: 'application/json' } });
    if (!result.ok) {
      setPresetState('error');
      showPresetError(`Could not read ${name}.json -- ${describe(result)}.`);
      return;
    }
    let parsed;
    try {
      parsed = JSON.parse(result.text);
    } catch (parseError) {
      setPresetState('error');
      showPresetError(`${name}.json is not valid JSON.`);
      return;
    }
    const problem = presetShapeProblem(parsed);
    if (problem !== '') {
      setPresetState('error');
      showPresetError(`${name}.json: ${problem}`);
      return;
    }
    /* Merge ONLY the five groups into the form. Never adoptModel(): that would
     * clobber baseGeneration and the displayed device facts, and a preset's
     * sample_rate_hz (if a hand-edited file carries one) must never reach the
     * device -- collectModel() takes the rate from state.model. */
    render({
      features: parsed.features,
      input_channels: parsed.input_channels,
      output_eq: parsed.output_eq,
      matrix: parsed.matrix,
      limiter: parsed.limiter,
    });
    if (state.applyState === 'applied' || state.applyState === 'loaded' || state.applyState === 'error') {
      setApplyState('edited');
    }
    setPresetOrigin(name);
    setPresetState('idle');
    setPresetStatus(`Loaded ${name} into the form. Apply to send it to the device.`);
  }

  async function deletePreset(name) {
    const token = $('token').value.trim();
    if (token === '') {
      showPresetError('SD auth token required to delete a preset.');
      return;
    }
    clearPresetError();
    if (!window.confirm(`Delete ${name}.json from the card? This cannot be undone.`)) {
      setPresetStatus(`${name}.json was kept.`);
      return;
    }
    const result = await requestRaw(presetUrl(name), {
      method: 'DELETE',
      headers: { 'X-Aius-SD-Auth': token },
    });
    if (result.status === 404 && result.token === 'sd_not_found') {
      await refreshPresets();
      setPresetStatus(`${name}.json was already gone.`);
      return;
    }
    if (!result.ok) {
      setPresetState('error');
      showPresetError(`Could not delete ${name}.json -- ${describe(result)}.`);
      return;
    }
    await refreshPresets();
    setPresetStatus(`Deleted ${name}.json.`);
  }

  async function uploadPreset() {
    clearPresetError();
    const file = $('preset-file').files && $('preset-file').files[0];
    if (!file) {
      showPresetError('Choose a .json preset file first.');
      return;
    }
    let parsed;
    try {
      parsed = JSON.parse(await file.text());
    } catch (readError) {
      setPresetState('error');
      showPresetError(`${file.name} is not valid JSON.`);
      return;
    }
    const problem = presetShapeProblem(parsed);
    if (problem !== '') {
      setPresetState('error');
      showPresetError(`${file.name}: ${problem}`);
      return;
    }
    /* Precedence, said out loud in the hint under the field: a filled Preset
     * name renames the upload, an empty one keeps the file's own name. */
    const name = $('preset-name').value.trim() || file.name.replace(/\.json$/i, '');
    /* Re-serialise the five groups only, so a hand-edited file's extra keys
     * (source, generations, sample_rate_hz) never reach the card. */
    await savePreset(name, {
      features: parsed.features,
      input_channels: parsed.input_channels,
      output_eq: parsed.output_eq,
      matrix: parsed.matrix,
      limiter: parsed.limiter,
    });
  }

  /* ---- saving a preset to a FILE -----------------------------------------
   * The mirror of the upload, and the only path here that talks to neither the
   * card nor the device: the JSON is handed to the browser's own download,
   * which needs no auth token because nothing is written anywhere. */

  function downloadJson(name, text) {
    const url = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = `${name}.json`;
    anchor.click();
    URL.revokeObjectURL(url);
  }

  /* The card's bytes, verbatim: a download is how an operator RESCUES a preset
   * the page would refuse to load, so this deliberately skips the shape check
   * loadPreset() applies. */
  async function downloadPreset(name) {
    clearPresetError();
    const result = await requestRaw(presetUrl(name), { headers: { Accept: 'application/json' } });
    if (!result.ok) {
      setPresetState('error');
      showPresetError(`Could not read ${name}.json -- ${describe(result)}.`);
      return;
    }
    downloadJson(name, result.text);
    setPresetState('idle');
    setPresetStatus(`Downloaded ${name}.json.`);
  }

  /* The form itself as a file. Validated like the card save for the same
   * reason -- an out-of-range field collects as its fallback, so an ungated
   * export would quietly write a 0 nobody typed -- but NOT name-checked: the
   * grammar exists for the SD path, and this file never reaches one. */
  function downloadCurrent() {
    clearPresetError();
    const problems = validateAll();
    if (problems.length > 0) {
      showPresetError(
        `Fix the form first -- ${problems.length} field${problems.length === 1 ? '' : 's'} out of range: ${problems[0].message}`,
      );
      return;
    }
    const name = $('preset-name').value.trim() || 'current';
    downloadJson(name, JSON.stringify(collectPresetGroups()));
    setPresetState('idle');
    setPresetStatus(`Downloaded ${name}.json.`);
  }

  /* Grammar as it is typed, not only when the save is refused (UX finding N8:
   * maxlength silently swallowed the 28th keystroke). */
  function refreshPresetNameHint() {
    const name = $('preset-name').value.trim();
    const problem = name === '' ? '' : presetNameProblem(name);
    const hint = $('preset-name-hint');
    hint.textContent =
      `${name.length} / ${PRESET_NAME_MAX} characters. ` +
      (problem === ''
        ? "An upload uses this name when it is filled, and the file's own name when it is empty."
        : problem);
    hint.classList.toggle('hint--warn', problem !== '');
  }

  function setPresetState(next) {
    shared.setPill($('preset-state'), next);
  }

  const setPresetStatus = (text) => {
    $('preset-status').textContent = text;
  };

  /* Where the form came from, kept beside the status sentence rather than
   * inside it: the sentence describes the last ACTION, this describes the
   * form, and the two change on different beats. */
  function setPresetOrigin(name) {
    state.presetOrigin = name === null ? null : { name, snapshot: JSON.stringify(collectPresetGroups()) };
    refreshPresetOrigin();
  }

  /* "modified" is the loaded snapshot against a fresh collect -- the same
   * question isDirty() asks of the DEVICE's model, asked of the preset. The
   * two answers genuinely differ: a preset can be loaded and applied (clean,
   * unmodified), or edited and never applied (dirty AND modified). */
  function refreshPresetOrigin() {
    const origin = state.presetOrigin;
    if (origin === null) {
      $('preset-origin').textContent = '';
      return;
    }
    const modified = origin.snapshot !== JSON.stringify(collectPresetGroups());
    $('preset-origin').textContent = `preset: ${origin.name}${modified ? ' (modified)' : ''}`;
  }

  function showPresetError(text) {
    $('preset-errors').textContent = text;
    $('preset-errors').hidden = false;
  }

  function clearPresetError() {
    $('preset-errors').textContent = '';
    $('preset-errors').hidden = true;
  }

  /* ---- meters ------------------------------------------------------------ */

  /* Three outcomes, not two. A non-2xx or a dead transport means the endpoint
   * is unavailable and is worth backing off from; a 200 whose sequence is 0
   * means the device is answering perfectly well and has simply never
   * published (metering bypassed, or no block processed yet), which is a
   * render state rather than a fault -- so it keeps the full 10 Hz poll. */
  async function pollMeters() {
    if (state.meterInFlight || Date.now() < state.meterRetryAt) return;
    state.meterInFlight = true;
    const result = await request('/api/meters', { headers: { Accept: 'application/json' } });
    state.meterInFlight = false;
    if (!result.ok || !result.body) {
      state.meterRetryAt = Date.now() + METER_RETRY_MS;
      renderMetersAbsent(result);
      return;
    }
    if (!(Number(result.body.sequence) > 0)) {
      renderMetersIdle();
      return;
    }
    renderMeters(result.body);
  }

  function blankMeterRows() {
    for (const [key] of CHANNELS) setMeterRow(`meter-in-${key}`, null, null);
    setMeterRow('meter-out-l', null, null);
    setMeterRow('meter-out-r', null, null);
    state.waveform = [];
    shared.drawWaveform($('waveform'), []);
    blankStrip();
  }

  /* Scoped to the blocks that actually read a snapshot: the EQ plot is a view
   * of the FORM and stays live when the meters go away, so it never wears a
   * "no data" pill (UX finding E5). */
  function setMeterState(text) {
    for (const id of ['meter-state', 'strip-input-state', 'strip-limiter-state', 'strip-output-state']) {
      shared.setPill($(id), text);
    }
  }

  function renderMetersAbsent(result) {
    setMeterState('unreachable');
    blankMeterRows();
    $('meter-summary').textContent = `Meters unavailable -- ${describe(result)}. Retrying every ${METER_RETRY_MS / 1000} s.`;
  }

  /* sequence 0 is the snapshot's own documented "never published" sentinel, so
   * it renders as no data rather than as a genuine zero-level signal. */
  function renderMetersIdle() {
    setMeterState('no data');
    blankMeterRows();
    $('meter-sequence').textContent = '0';
    $('meter-queue').textContent = '--';
    $('meter-latency').textContent = '--';
    $('meter-cycles').textContent = '--';
    $('meter-summary').textContent =
      'No meter snapshot published yet -- metering is bypassed, or no audio block has been processed since the last config apply.';
  }

  /* Levels arrive as dBFS, floored at -120 so every number on the wire is
   * finite; the page does no conversion of its own. A value at or below the
   * floor is silence and reads as -inf. */
  const dbfsOf = (entry, key) => (entry && Number.isFinite(entry[key]) ? entry[key] : null);
  const channelDbfs = (values, index) =>
    Array.isArray(values) && Number.isFinite(values[index]) ? values[index] : null;

  const formatDb = shared.formatDb;

  /* Raw DWT counts are unreadable at six digits and say nothing without their
   * budget, so both are scaled the same way (UX finding E12). */
  function formatCycles(cycles) {
    if (!Number.isFinite(cycles)) return '--';
    if (cycles < 10000) return String(Math.round(cycles));
    if (cycles < 1e6) return `${Math.round(cycles / 1000)}k`;
    return `${(cycles / 1e6).toFixed(2)}M`;
  }

  function setMeterRow(id, peak, rms) {
    $(`${id}-peak`).textContent = formatDb(peak);
    $(`${id}-rms`).textContent = formatDb(rms);
  }

  function renderMeters(meters) {
    setMeterState('live');
    /* Six positional entries in AIUS_CH_* order; position is the identity. */
    const inputs = Array.isArray(meters.input) ? meters.input : [];
    CHANNELS.forEach(([key], index) => {
      const entry = inputs[index];
      setMeterRow(`meter-in-${key}`, dbfsOf(entry, 'peak_dbfs'), dbfsOf(entry, 'rms_dbfs'));
    });

    /* The stereo output keeps both channels as [L, R] arrays. */
    const output = meters.output && typeof meters.output === 'object' ? meters.output : {};
    ['meter-out-l', 'meter-out-r'].forEach((id, index) => {
      setMeterRow(id, channelDbfs(output.peak_dbfs, index), channelDbfs(output.rms_dbfs, index));
    });

    $('meter-sequence').textContent = Number.isFinite(meters.sequence) ? String(meters.sequence) : '--';
    $('meter-queue').textContent = Number.isFinite(meters.queue_frames) ? `${meters.queue_frames} frames` : '--';
    $('meter-latency').textContent = Number.isFinite(meters.estimated_latency_us)
      ? shared.formatUs(meters.estimated_latency_us)
      : '--';
    const budget = dspBudgetCycles(Number.isFinite(meters.sample_rate_hz) ? meters.sample_rate_hz : 0);
    $('meter-cycles').textContent =
      Number.isFinite(meters.dsp_cycles_last) || Number.isFinite(meters.dsp_cycles_max)
        ? `${formatCycles(meters.dsp_cycles_last)} / ${formatCycles(meters.dsp_cycles_max)}${
            budget > 0 ? ` of ${formatCycles(budget)} budget` : ''
          }`
        : '--';
    $('meter-summary').textContent = `Limiter gain reduction ${formatDb(meters.limiter_gain_reduction_db)} dB at ${
      Number.isFinite(meters.sample_rate_hz) ? meters.sample_rate_hz : '--'
    } Hz.`;
    state.waveform = Array.isArray(meters.waveform) ? meters.waveform : [];
    shared.drawWaveform($('waveform'), state.waveform);
    renderDspLoad(meters);
    renderStripLevels(meters);
    pushLimiter(meters.limiter_gain_reduction_db);
  }

  /* ---- signal-flow ends: what the device is hearing, and transmitting -----
   * GET /api/status at 1 Hz. `carrier`, the decode `state`, the negotiated
   * `sample_rate` and the latched tx_gate terms are the facts the page needs,
   * and they are the two ends of the strip. */

  /* The firmware's own numeric vocabulary for `state`, mirrored the way
   * MODEL_MAX_BYTES mirrors AIUS_DSP_MODEL_MAX_JSON_BYTES: 2 no carrier,
   * 3 PCM, 4 DTS (optical_main.cpp current_state()). status.js names the same
   * numbers on the status page -- one vocabulary, two readers. */
  const CARRIER_ENCODINGS = { 3: 'PCM', 4: 'DTS' };

  async function pollStatus() {
    if (state.statusInFlight) return;
    state.statusInFlight = true;
    const result = await request('/api/status', { headers: { Accept: 'application/json' } });
    state.statusInFlight = false;
    renderFlowEnds(result.ok && result.body ? result.body : null);
  }

  function setFlowEnd(id, dataState, text) {
    const element = $(id);
    element.dataset.state = dataState;
    element.textContent = text;
  }

  /* "optical carrier" alone never said WHAT was arriving on it, which is the
   * question a DSP editor is opened to answer. A term the snapshot cannot
   * support -- state 2 or a state this page has no name for, a rate not yet
   * negotiated -- is LEFT OFF rather than printed as "unknown". */
  function carrierText(status) {
    const parts = ['optical carrier'];
    const encoding = CARRIER_ENCODINGS[Number(status.state)];
    if (encoding) parts.push(encoding);
    const rate = Number(status.sample_rate);
    if (Number.isFinite(rate) && rate > 0) parts.push(`${rate} Hz`);
    return `input: ${parts.join(' · ')}`;
  }

  function renderFlowEnds(status) {
    if (!status) {
      setFlowEnd('input-source', 'unreachable', 'input: device unreachable');
      setFlowEnd('tx-gate', 'unreachable', 'output: device unreachable');
      return;
    }
    const gate = status.tx_gate && typeof status.tx_gate === 'object' ? status.tx_gate : {};
    if (status.carrier) setFlowEnd('input-source', 'carrier', carrierText(status));
    else if (gate.replay_running) setFlowEnd('input-source', 'replay', 'input: replay');
    else setFlowEnd('input-source', 'none', 'input: none');
    if (gate.open) setFlowEnd('tx-gate', 'open', 'output: gate open');
    else setFlowEnd('tx-gate', 'dark', 'output: gate dark');
  }

  /* ---- visual surface: pure maths ----------------------------------------
   * Dependency-free on purpose: happy-dom has no layout engine (its
   * getBoundingClientRect returns an all-zero rect), so the vitest suite proves
   * this arithmetic by calling it with literal rects rather than by
   * synthesising a gesture. A "rect" is any {left, top, width, height} -- the
   * canvas's CLIENT rect for pointer work, {0, 0, canvas.width, canvas.height}
   * for drawing, which differ because CSS scales the canvas to the card width
   * and the backing store is sized in device pixels.
   *
   * The curves are DISPLAY ONLY: the RBJ audio-EQ cookbook mirrored in JS so
   * the page can show what a section is about to do. The device designs the
   * real biquads from the applied config and stays the authority, exactly as
   * the bounds table mirrors rather than replaces its validation. */

  const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

  /* Only a highpass or a peaking section bends the response, and only with a
   * usable frequency and Q -- "none" and "bypass" contribute nothing, and so
   * does a half-typed section. */
  const isActiveSection = (section) =>
    Boolean(section) &&
    (section.type === 'highpass' || section.type === 'peaking') &&
    Number.isFinite(section.frequency_hz) &&
    section.frequency_hz > 0 &&
    Number.isFinite(section.q) &&
    section.q > 0;

  /* RBJ audio-EQ cookbook, in the same two flavours the firmware designs. */
  function sectionCoefficients(section, sampleRateHz) {
    const w0 = (2 * Math.PI * section.frequency_hz) / sampleRateHz;
    const cos0 = Math.cos(w0);
    const alpha = Math.sin(w0) / (2 * section.q);
    if (section.type === 'peaking') {
      const a = Math.pow(10, (Number.isFinite(section.gain_db) ? section.gain_db : 0) / 40);
      return {
        b0: 1 + alpha * a,
        b1: -2 * cos0,
        b2: 1 - alpha * a,
        a0: 1 + alpha / a,
        a1: -2 * cos0,
        a2: 1 - alpha / a,
      };
    }
    return {
      b0: (1 + cos0) / 2,
      b1: -(1 + cos0),
      b2: (1 + cos0) / 2,
      a0: 1 + alpha,
      a1: -2 * cos0,
      a2: 1 - alpha,
    };
  }

  /* |H(f)| in dB, evaluating H(z) on the unit circle at z = e^{jw}: the ratio of
   * |b0 + b1 z^-1 + b2 z^-2| to |a0 + a1 z^-1 + a2 z^-2| (the imaginary parts
   * carry a leading minus that a magnitude discards). The angle is capped at pi
   * -- Nyquist -- because above it the response mirrors, which would draw a
   * phantom rise at the right of the plot on a sub-40 kHz rate. */
  function sectionMagnitudeDb(section, frequencyHz, sampleRateHz) {
    if (!isActiveSection(section) || !(sampleRateHz > 0) || !(frequencyHz > 0)) return 0;
    const coefficients = sectionCoefficients(section, sampleRateHz);
    const w = Math.min(Math.PI, (2 * Math.PI * frequencyHz) / sampleRateHz);
    const cos1 = Math.cos(w);
    const sin1 = Math.sin(w);
    const cos2 = Math.cos(2 * w);
    const sin2 = Math.sin(2 * w);
    const numerator = Math.hypot(
      coefficients.b0 + coefficients.b1 * cos1 + coefficients.b2 * cos2,
      coefficients.b1 * sin1 + coefficients.b2 * sin2,
    );
    const denominator = Math.hypot(
      coefficients.a0 + coefficients.a1 * cos1 + coefficients.a2 * cos2,
      coefficients.a1 * sin1 + coefficients.a2 * sin2,
    );
    if (!(denominator > 0) || !(numerator > 0)) return CURVE_FLOOR_DB;
    return Math.max(CURVE_FLOOR_DB, 20 * Math.log10(numerator / denominator));
  }

  /* Sections in a cascade multiply, so their dB add. */
  const cascadeMagnitudeDb = (sections, frequencyHz, sampleRateHz) =>
    (Array.isArray(sections) ? sections : []).reduce(
      (total, section) => total + sectionMagnitudeDb(section, frequencyHz, sampleRateHz),
      0,
    );

  /* Identity of one cascade for comparison purposes, using exactly the rules
   * collectEqSection() applies on the wire: an absent section is "none"
   * whatever numbers are stranded in its fields, a bypass carries none of
   * them, and a highpass has no gain. Two members of a stereo pair are the
   * same cascade iff their signatures match. */
  const cascadeSignature = (sections) =>
    (Array.isArray(sections) ? sections : [])
      .map((section) => {
        if (!section || section.type === 'none' || section.type === undefined) return 'none';
        if (section.type === 'bypass') return 'bypass';
        const gain = section.type === 'peaking' && Number.isFinite(section.gain_db) ? section.gain_db : 0;
        return `${section.type}|${section.frequency_hz}|${section.q}|${gain}`;
      })
      .join(',');

  /* A stereo pair is ONE entity on the plot: the FIRST (left) member is the
   * display and edit authority, and `unlinked` is true when any other member
   * disagrees with it -- which is only reachable through a hand-edited config
   * or the per-channel form below. */
  function resolveCascade(cascades) {
    const list = Array.isArray(cascades) ? cascades : [];
    const sections = list.length > 0 ? list[0] : [];
    const signature = cascadeSignature(sections);
    return {
      sections,
      unlinked: list.slice(1).some((other) => cascadeSignature(other) !== signature),
    };
  }

  const hzToX = (hz, rect) =>
    rect.left +
    (rect.width * Math.log(clamp(hz, CURVE_MIN_HZ, CURVE_MAX_HZ) / CURVE_MIN_HZ)) /
      Math.log(CURVE_MAX_HZ / CURVE_MIN_HZ);

  const xToHz = (x, rect) =>
    CURVE_MIN_HZ * Math.pow(CURVE_MAX_HZ / CURVE_MIN_HZ, clamp((x - rect.left) / rect.width, 0, 1));

  const dbToY = (db, rect) => rect.top + rect.height / 2 - (db / CURVE_SPAN_DB) * (rect.height / 2);

  const yToDb = (y, rect) =>
    clamp(((rect.top + rect.height / 2 - y) / (rect.height / 2)) * CURVE_SPAN_DB, -CURVE_SPAN_DB, CURVE_SPAN_DB);

  /* Field-sized precision, so a dragged value is one a human could have typed,
   * lands on the field's own native step, and keeps the byte count sane. */
  const roundHz = (hz) => Math.round(hz);
  const roundDb = (db) => Math.round(db * 10) / 10;
  const roundQ = (q) => Math.round(q * 1000) / 1000;
  const roundLinear = (value) => Math.round(value * 1000) / 1000;

  /* (rect, client coords) -> the two model values a drag can carry. */
  const pointToEq = (rect, clientX, clientY) => ({
    frequency_hz: roundHz(xToHz(clientX, rect)),
    gain_db: roundDb(yToDb(clientY, rect)),
  });

  /* One arrow press. Proportional, because a fixed 10 Hz is a semitone at the
   * bottom of the axis and nothing at the top; clamped to the plot's own span
   * so the handle can never leave the surface it is being driven on. */
  const nudgeHz = (hz, direction) => {
    const base = Number.isFinite(hz) && hz > 0 ? hz : EQ_SEED.frequency_hz;
    return roundHz(clamp(direction > 0 ? base * NUDGE_HZ_RATIO : base / NUDGE_HZ_RATIO, CURVE_MIN_HZ, CURVE_MAX_HZ));
  };

  /* (sections, rect, rate) -> the polyline the canvas strokes for the whole
   * cascade. An empty or all-bypassed cascade is a flat line at 0 dB. */
  function curvePoints(sections, rect, sampleRateHz) {
    const points = [];
    for (let step = 0; step <= CURVE_STEPS; step++) {
      const x = rect.left + (rect.width * step) / CURVE_STEPS;
      points.push({ x, y: dbToY(cascadeMagnitudeDb(sections, xToHz(x, rect), sampleRateHz), rect) });
    }
    return points;
  }

  /* One draggable point per active section, carrying the slot so a gesture
   * knows which fields it is editing. A highpass has no gain, so its handle
   * rides the 0 dB line. */
  const handlePoints = (sections, rect) =>
    (Array.isArray(sections) ? sections : []).filter(isActiveSection).map((section) => ({
      slot: section.slot,
      type: section.type,
      x: hzToX(section.frequency_hz, rect),
      y: dbToY(section.type === 'peaking' && Number.isFinite(section.gain_db) ? section.gain_db : 0, rect),
    }));

  /* Nearest handle within the grab radius, or null. */
  function pickHandle(points, clientX, clientY, grabPx) {
    let best = null;
    let bestDistance = grabPx;
    for (const point of points) {
      const distance = Math.hypot(point.x - clientX, point.y - clientY);
      if (distance <= bestDistance) {
        best = point;
        bestDistance = distance;
      }
    }
    return best;
  }

  /* One wheel notch, multiplicative, clamped to the Q field's own bounds. An
   * unset Q starts from the seed rather than from nothing. */
  const stepQ = (q, deltaY) => {
    const base = Number.isFinite(q) && q > 0 ? q : EQ_SEED.q;
    return roundQ(clamp(deltaY < 0 ? base * Q_WHEEL_STEP : base / Q_WHEEL_STEP, Q_MIN, Q_MAX));
  };

  /* ---- the downmix matrix's geometry -------------------------------------
   * Same seam as the curve maths above and for the same reason: no layout
   * engine under happy-dom, so every mapping takes an explicit rect. `values`
   * is one plain object -- {center, surround, lfe, headroom, clipfree,
   * crossoverHz} -- read from the form by the DOM half and passed in literally
   * by the tests. */

  const matrixTrack = (rect) => ({
    top: rect.top + rect.height * MATRIX_TRACK_TOP,
    bottom: rect.top + rect.height * MATRIX_TRACK_BOTTOM,
  });

  /* value <-> y over ONE handle's own bound: the top of the track is that
   * field's maximum, the bottom its minimum. */
  function matrixValueToY(value, rect, bound) {
    const { top, bottom } = matrixTrack(rect);
    const span = bound.max - bound.min;
    const current = Number.isFinite(value) ? value : bound.min;
    const level = span > 0 ? (clamp(current, bound.min, bound.max) - bound.min) / span : 0;
    return bottom - level * (bottom - top);
  }

  function matrixYToValue(y, rect, bound) {
    const { top, bottom } = matrixTrack(rect);
    const level = bottom > top ? clamp((bottom - y) / (bottom - top), 0, 1) : 0;
    return roundLinear(clamp(bound.min + level * (bound.max - bound.min), bound.min, bound.max));
  }

  /* Six decoded sources stacked left, the stereo pair right, and the columns the
   * gain handles ride between them. */
  function matrixLayout(rect) {
    const { top, bottom } = matrixTrack(rect);
    const columnX = (fraction) => rect.left + rect.width * fraction;
    return {
      top,
      bottom,
      sources: CHANNELS.map(([key], index) => ({
        key,
        label: key.toUpperCase(),
        x: columnX(MATRIX_SOURCE_X),
        y: top + ((bottom - top) * index) / (CHANNELS.length - 1),
      })),
      outputs: MATRIX_OUTPUTS.map(([key, label], index) => ({
        key,
        label,
        x: columnX(MATRIX_OUTPUT_X),
        y: top + (bottom - top) * MATRIX_OUTPUT_Y[index],
      })),
    };
  }

  /* One draggable per editable term, at its own column and at the y its value
   * maps to. The headroom handle is LOCKED while the clip-free policy owns the
   * value -- the diagram's copy of the form field going read-only. */
  const matrixHandlePoints = (values, rect) => {
    const columnX = (fraction) => rect.left + rect.width * fraction;
    const read = (key) => (values && Number.isFinite(values[key]) ? values[key] : NaN);
    return MATRIX_HANDLES.map((handle) => ({
      key: handle.key,
      label: handle.label,
      path: handle.path,
      colour: handle.colour,
      bound: handle.bound,
      value: read(handle.key),
      locked: handle.key === 'headroom' && Boolean(values && values.clipfree),
      x: columnX(handle.column),
      y: matrixValueToY(read(handle.key), rect, handle.bound),
    }));
  };

  /* Width and opacity carry the gain, so the picture reads at a glance; a zero
   * gain is DASHED, because "a hairline" and "silent" are not the same
   * statement. Scaled against the shared 0..4 gain bound, so unity sits a
   * quarter of the way up and the eye can find it. */
  const matrixPathStyle = (gain) => {
    const level = Number.isFinite(gain) ? clamp(gain / MATRIX_GAIN_BOUND.max, 0, 1) : 0;
    return {
      width: MATRIX_PATH_MIN_PX + level * (MATRIX_PATH_MAX_PX - MATRIX_PATH_MIN_PX),
      alpha: gain > 0 ? MATRIX_PATH_MIN_ALPHA + level * (1 - MATRIX_PATH_MIN_ALPHA) : MATRIX_MUTED_ALPHA,
      muted: !(gain > 0),
    };
  };

  /* The eight terms of the downmix sum as polylines: a gained path BENDS through
   * its handle, a unity reference goes straight across. Sharing is drawn rather
   * than asserted -- FC into L and FC into R are two edges through the same
   * point, because they are two uses of one field. */
  function matrixEdges(values, rect) {
    const layout = matrixLayout(rect);
    const handles = new Map(matrixHandlePoints(values, rect).map((handle) => [handle.key, handle]));
    return MATRIX_ROUTES.map((route) => {
      const source = layout.sources.find((node) => node.key === route.source);
      const output = layout.outputs.find((node) => node.key === route.output);
      const handle = route.handle === null ? null : handles.get(route.handle);
      const gain = handle === null ? 1 : handle.value;
      return {
        source: route.source,
        output: route.output,
        handle: route.handle,
        fixed: route.handle === null,
        gain,
        colour: handle === null ? entityColour('front') : route.colour || handle.colour,
        points:
          handle === null
            ? [
                { x: source.x, y: source.y },
                { x: output.x, y: output.y },
              ]
            : [
                { x: source.x, y: source.y },
                { x: handle.x, y: handle.y },
                { x: output.x, y: output.y },
              ],
        style: matrixPathStyle(gain),
      };
    });
  }

  /* What a linear multiplier means, in both languages the form prints (UX
   * finding E7's rule, on the diagram): the number the field holds and the
   * decibels it is. A zero gain is MUTED -- "-inf dB" reads like a level. */
  function matrixGainLabel(value) {
    if (!Number.isFinite(value)) return '--';
    const db = linearToDb(value);
    return db === null ? `${displayNumber(value)} linear, muted` : `${displayNumber(value)} linear, ${formatDb(db)} dB`;
  }

  const matrixCrossoverLabel = (hz) => (Number.isFinite(hz) ? `LR4 @ ${roundHz(hz)} Hz` : 'LR4 @ -- Hz');

  /* The crossover label sits on the LFE path's first leg -- at the source node's
   * own height, so it holds still while the gain handle moves. `scale` is the
   * device-pixel ratio when drawing and 1 when hit-testing a client rect, which
   * is the only difference between the two uses. */
  function matrixCrossoverBox(rect, scale) {
    const factor = Number.isFinite(scale) && scale > 0 ? scale : 1;
    const layout = matrixLayout(rect);
    const node = layout.sources.find((source) => source.key === 'lfe');
    const width = MATRIX_LABEL_W_PX * factor;
    const height = MATRIX_LABEL_H_PX * factor;
    const middle = (node.x + rect.left + rect.width * matrixHandleOf('lfe').column) / 2;
    return { left: middle - width / 2, top: node.y - height - 4 * factor, width, height };
  }

  const matrixHitsBox = (box, clientX, clientY) =>
    clientX >= box.left && clientX <= box.left + box.width && clientY >= box.top && clientY <= box.top + box.height;

  /* One wheel notch over a handle: multiplicative, like the plot's Q, because a
   * gain is a ratio. A muted path has no ratio to scale, so a notch UP starts
   * from a seed and a notch DOWN leaves it exactly where it is. */
  const stepMatrixValue = (value, deltaY, bound) => {
    const current = Number.isFinite(value) ? clamp(value, bound.min, bound.max) : bound.min;
    if (current <= bound.min && deltaY > 0) return bound.min;
    const base = current > 0 ? current : MATRIX_WHEEL_SEED;
    return roundLinear(clamp(deltaY < 0 ? base * MATRIX_WHEEL_STEP : base / MATRIX_WHEEL_STEP, bound.min, bound.max));
  };

  /* One arrow press, matching the matrix fields' own native step. */
  const nudgeMatrixValue = (value, direction, bound) =>
    roundLinear(clamp((Number.isFinite(value) ? value : bound.min) + direction * MATRIX_NUDGE, bound.min, bound.max));

  /* Cycles one decoded block may spend; 0 when the snapshot has no usable rate
   * (an unpublished snapshot reports 0 Hz). */
  const dspBudgetCycles = (sampleRateHz) =>
    Number.isFinite(sampleRateHz) && sampleRateHz > 0 ? (DSP_CPU_HZ * DSP_BLOCK_FRAMES) / sampleRateHz : 0;

  /* null, not 0, when the snapshot cannot answer -- the page renders that as
   * '--' rather than as a genuine "no load" reading. */
  function dspLoadPercent(cycles, sampleRateHz) {
    const budget = dspBudgetCycles(sampleRateHz);
    if (!(budget > 0) || !Number.isFinite(cycles) || cycles < 0) return null;
    return (cycles / budget) * 100;
  }

  /* Bar fill 0..1 over BAR_FLOOR_DBFS..0 dBFS. */
  const levelFraction = (dbfs) =>
    Number.isFinite(dbfs) ? clamp((dbfs - BAR_FLOOR_DBFS) / -BAR_FLOOR_DBFS, 0, 1) : 0;

  /* 0 dBFS is not a healthy green (UX finding N6). */
  const levelZone = (dbfs) => {
    if (!Number.isFinite(dbfs)) return 'idle';
    if (dbfs >= BAR_DANGER_DBFS) return 'danger';
    if (dbfs > BAR_WARN_DBFS) return 'warn';
    return 'ok';
  };

  const loadZone = (percent) => {
    if (percent === null) return 'idle';
    if (percent > LOAD_DANGER_PERCENT) return 'danger';
    if (percent > LOAD_WARN_PERCENT) return 'warn';
    return 'ok';
  };

  /* ---- visual surface: DOM ----------------------------------------------- */

  const selectedEntity = () =>
    CURVE_ENTITIES.find((entity) => entity.key === state.curveEntity) || CURVE_ENTITIES[0];

  /* Slot-preserving view of one cascade, read straight from the inputs: the
   * index IS the slot, so a dragged handle knows which fields to write. An
   * empty or unparsable number reads as absent, which makes the section
   * inactive rather than a curve at 0 Hz. */
  function curveSections(base) {
    return [...Array(EQ_SLOTS).keys()].map((slot) => {
      const path = `${base}.eq.${slot}`;
      return {
        slot,
        type: field(`${path}.type`).value,
        frequency_hz: numberOf(`${path}.frequency_hz`, NaN),
        q: numberOf(`${path}.q`, NaN),
        gain_db: numberOf(`${path}.gain_db`, 0),
      };
    });
  }

  const entityCascade = (entity) => resolveCascade(entity.bases.map((base) => curveSections(base)));

  const curveSampleRate = () =>
    state.model && Number.isFinite(state.model.sample_rate_hz) && state.model.sample_rate_hz > 0
      ? state.model.sample_rate_hz
      : CURVE_DEFAULT_RATE_HZ;

  const labelHz = (hz) => (hz >= 1000 ? `${hz / 1000}k` : String(hz));

  const shortName = (base) => (base.startsWith('input_channels.') ? base.slice('input_channels.'.length) : base);

  function buildVisualStrip() {
    const legend = $('curve-legend');
    for (const entity of CURVE_ENTITIES) {
      const chip = document.createElement('button');
      chip.type = 'button';
      chip.className = 'chip';
      chip.id = `chip:${entity.key}`;
      chip.dataset.entity = entity.key;
      chip.style.setProperty('--chip-colour', entity.colour);
      const key = document.createElement('span');
      key.className = 'chip__key';
      const label = document.createElement('span');
      label.textContent = entity.label;
      const unlinked = document.createElement('span');
      unlinked.className = 'chip__unlinked';
      unlinked.id = `unlinked:${entity.key}`;
      unlinked.textContent = 'unlinked';
      unlinked.hidden = true;
      chip.append(key, label, unlinked);
      chip.addEventListener('click', () => selectEntity(entity.key));
      legend.append(chip);
    }

    for (const [key, label] of CHANNELS) buildBar('bars-input', `bar-in-${key}`, `${label} (${key})`);
    buildBar('bars-output', 'bar-out-l', 'Output left');
    buildBar('bars-output', 'bar-out-r', 'Output right');

    attachCurveSurface();
    $('curve-add').addEventListener('click', () => addSection(selectedEntity(), EQ_SEED.frequency_hz));
    $('curve-remove').addEventListener('click', () => removeSection(selectedEntity(), state.activeSlot));
    $('curve-q').addEventListener('input', onCurveQInput);

    attachMatrixSurface();
    $('matrix-clipfree').addEventListener('click', toggleClipfree);

    /* A pointer released off the canvas still ends the drag; without capture
     * (or with it, since the event bubbles) window is the one listener that
     * always sees it. */
    window.addEventListener('pointerup', endDrag);
    window.addEventListener('pointercancel', endDrag);
    window.addEventListener('pointerup', endMatrixDrag);
    window.addEventListener('pointercancel', endMatrixDrag);
    window.addEventListener('resize', () => {
      fitCanvases();
      drawCurveSurface();
      drawMatrixSurface();
      drawLimiterHistory();
      shared.drawWaveform($('waveform'), state.waveform);
    });

    fitCanvases();
    drawCurveSurface();
    drawMatrixSurface();
    drawLimiterHistory();
  }

  function buildBar(mountId, id, label) {
    const row = document.createElement('div');
    row.className = 'bar';
    const name = document.createElement('span');
    name.className = 'bar__label';
    name.textContent = label;
    const track = document.createElement('div');
    track.className = 'bar__track';
    const fill = document.createElement('div');
    fill.className = 'bar__fill';
    fill.id = `${id}-fill`;
    track.append(fill);
    const value = document.createElement('span');
    value.className = 'bar__value';
    value.id = `${id}-value`;
    value.textContent = '--';
    row.append(name, track, value);
    $(mountId).append(row);
  }

  function setBar(id, dbfs) {
    const fill = $(`${id}-fill`);
    fill.style.width = `${(levelFraction(dbfs) * 100).toFixed(1)}%`;
    fill.dataset.zone = levelZone(dbfs);
    $(`${id}-value`).textContent = formatDb(dbfs);
  }

  /* ---- visual surface: canvas backing store ------------------------------
   * A fixed 960-px backing store was blurry on every hiDPI screen and drew
   * 3.6-px axis text at phone width (UX finding E9). The backing store follows
   * the laid-out width times the device pixel ratio, and everything drawn is
   * scaled by that ratio -- so no context transform is needed and the pointer
   * maths keeps working against the CLIENT rect. A zero clientWidth (no layout
   * engine, or a hidden card) keeps the authored attributes. */

  const CANVAS_IDS = ['curve', 'matrix-diagram', 'gr-history', 'waveform'];
  const dprOf = (canvas) => Number(canvas.dataset.dpr) || 1;

  function fitCanvas(canvas) {
    if (!canvas.dataset.aspect) canvas.dataset.aspect = String(canvas.height / canvas.width);
    const dpr = clamp(window.devicePixelRatio || 1, 1, 3);
    const cssWidth = canvas.clientWidth;
    if (!(cssWidth > 0)) return false;
    const width = Math.round(cssWidth * dpr);
    const height = Math.round(cssWidth * Number(canvas.dataset.aspect) * dpr);
    if (canvas.width === width && canvas.height === height) return false;
    canvas.width = width;
    canvas.height = height;
    canvas.dataset.dpr = String(dpr);
    return true;
  }

  const fitCanvases = () => CANVAS_IDS.forEach((id) => fitCanvas($(id)));

  /* ---- visual surface: the stacked plot ---------------------------------- */

  function drawCurveSurface() {
    const canvas = $('curve');
    const context = canvas.getContext('2d');
    if (!context) return;
    const scale = dprOf(canvas);
    const rect = { left: 0, top: 0, width: canvas.width, height: canvas.height };
    context.clearRect(0, 0, rect.width, rect.height);
    drawCurveGrid(context, rect, scale);

    const rate = curveSampleRate();
    const selected = selectedEntity();
    const resolved = new Map(CURVE_ENTITIES.map((entity) => [entity.key, entityCascade(entity)]));

    /* Unselected first: the editable curve is drawn last so it is never buried
     * under a neighbour, and only it carries handles. */
    const order = [...CURVE_ENTITIES.filter((entity) => entity !== selected), selected];
    for (const entity of order) {
      const chosen = entity === selected;
      context.globalAlpha = chosen ? 1 : CURVE_DIM_ALPHA;
      context.strokeStyle = entity.colour;
      context.lineWidth = (chosen ? 2 : 1) * scale;
      context.beginPath();
      curvePoints(resolved.get(entity.key).sections, rect, rate).forEach((point, index) => {
        if (index === 0) context.moveTo(point.x, point.y);
        else context.lineTo(point.x, point.y);
      });
      context.stroke();
      context.globalAlpha = 1;
    }

    const handles = handlePoints(resolved.get(selected.key).sections, rect);
    for (const handle of handles) {
      context.fillStyle = selected.colour;
      context.strokeStyle = handle.slot === state.activeSlot ? '#e8edf2' : '#101318';
      context.lineWidth = (handle.slot === state.activeSlot ? 2 : 1) * scale;
      context.beginPath();
      context.arc(handle.x, handle.y, HANDLE_RADIUS_PX * scale, 0, Math.PI * 2);
      context.fill();
      context.stroke();
    }
    context.lineWidth = scale;

    for (const entity of CURVE_ENTITIES) {
      const chip = $(`chip:${entity.key}`);
      const chosen = entity === selected;
      chip.setAttribute('aria-pressed', chosen ? 'true' : 'false');
      chip.classList.toggle('chip--on', chosen);
      $(`unlinked:${entity.key}`).hidden = !resolved.get(entity.key).unlinked;
    }
    $('curve-caption').textContent =
      handles.length === 0
        ? `${selected.label}: no sections -- double-click the plot (or press Add section) to add one.`
        : `${selected.label}: section ${state.activeSlot + 1} of ${EQ_SLOTS} selected, ${handles.length} in use.`;
    syncCurveQ();
  }

  /* The visible Q affordance (2026-07-27 feedback, item 2): a spinner beside
   * Add/Remove bound to the SELECTED section, because the wheel was the only
   * way to Q from the plot and a wheel is invisible (and absent on touch).
   * Sync follows every redraw; the one exception is while the spinner itself
   * has focus, so a keystroke is not rewritten under the typing hand. */
  function syncCurveQ() {
    const input = $('curve-q');
    const section = entityCascade(selectedEntity()).sections[state.activeSlot];
    const active = isActiveSection(section);
    input.disabled = !active;
    if (document.activeElement === input) return;
    input.value = active ? displayNumber(section.q) : '';
  }

  function onCurveQInput() {
    const entity = selectedEntity();
    const slot = state.activeSlot;
    if (!isActiveSection(entityCascade(entity).sections[slot])) return;
    const value = Number($('curve-q').value);
    if (!Number.isFinite(value)) return;
    beginSurfaceEdit(entity);
    surfaceWrite(entity, slot, 'q', value);
  }

  function drawCurveGrid(context, rect, scale) {
    context.strokeStyle = '#232b35';
    context.lineWidth = scale;
    context.beginPath();
    for (const hz of CURVE_GRID_HZ) {
      context.moveTo(hzToX(hz, rect), 0);
      context.lineTo(hzToX(hz, rect), rect.height);
    }
    for (const db of CURVE_GRID_DB) {
      context.moveTo(0, dbToY(db, rect));
      context.lineTo(rect.width, dbToY(db, rect));
    }
    context.stroke();

    /* 0 dB is the reference the whole plot is read against, so it gets its own
     * brighter line on top of the grid. */
    context.strokeStyle = '#2a3542';
    context.beginPath();
    context.moveTo(0, dbToY(0, rect));
    context.lineTo(rect.width, dbToY(0, rect));
    context.stroke();

    context.fillStyle = '#5d6b78';
    context.font = `${Math.round(11 * scale)}px ui-monospace,monospace`;
    CURVE_GRID_HZ.forEach((hz, index) => {
      /* The last label is right-aligned INSIDE the plot ("20k" used to be
       * clipped off the right edge) and the first is inset clear of the bottom
       * dB rail label it used to collide with. */
      const last = index === CURVE_GRID_HZ.length - 1;
      context.textAlign = last ? 'right' : 'left';
      const x = last
        ? rect.width - 3 * scale
        : index === 0
          ? HZ_LABEL_INSET_PX * scale
          : hzToX(hz, rect) + 3 * scale;
      context.fillText(labelHz(hz), x, rect.height - 5 * scale);
    });
    context.textAlign = 'left';
    for (const db of CURVE_GRID_DB) {
      if (db === 0) continue;
      /* The rails are inset INTO the plot: the top label sat above the top
       * edge and the bottom one below it, so neither was readable. */
      const inset = db === CURVE_SPAN_DB ? 12 : db === -CURVE_SPAN_DB ? -6 : -4;
      context.fillText(`${db > 0 ? '+' : ''}${db}`, 3 * scale, dbToY(db, rect) + inset * scale);
    }
  }

  /* ---- visual surface: gestures ------------------------------------------
   * The canvas edits the model through the form, never around it: writeField()
   * is the single write path, and dispatching 'input' is what makes a gesture
   * indistinguishable from typing downstream. A disabled input (a highpass's
   * gain, a bypassed section) is left alone, exactly as the device ignores it. */

  function writeField(path, value) {
    const input = field(path);
    if (input.disabled) return;
    input.value = String(value);
    input.dataset.exact = '';
    input.dispatchEvent(new Event('input'));
  }

  function writeChoice(path, value) {
    const input = field(path);
    input.value = value;
    input.dispatchEvent(new Event('input'));
  }

  function writeCheck(path, checked) {
    const input = field(path);
    if (input.disabled) return;
    input.checked = checked;
    input.dispatchEvent(new Event('input'));
  }

  /* Every write through the surface fans out to BOTH members of a stereo pair:
   * the pair is one entity here by contract. */
  function surfaceWrite(entity, slot, key, value) {
    for (const base of entity.bases) writeField(`${base}.eq.${slot}.${key}`, value);
  }

  /* The pair-divergence policy, announced before it applies: a pair whose
   * members differ can only be edited as one thing once they ARE one thing, so
   * the left member's cascade is adopted into the others. */
  function beginSurfaceEdit(entity) {
    if (entity.bases.length < 2 || !entityCascade(entity).unlinked) return false;
    const bases = entity.bases;
    setCurveStatus(
      `${entity.label} members differ; adopting ${shortName(bases[0])}'s cascade into ${bases
        .slice(1)
        .map(shortName)
        .join(' and ')} so the pair can be edited as one.`,
    );
    const left = curveSections(bases[0]);
    for (const base of bases.slice(1)) {
      left.forEach((section, slot) => {
        const path = `${base}.eq.${slot}`;
        writeChoice(`${path}.type`, section.type);
        if (Number.isFinite(section.frequency_hz)) writeField(`${path}.frequency_hz`, section.frequency_hz);
        if (Number.isFinite(section.q)) writeField(`${path}.q`, section.q);
        writeField(`${path}.gain_db`, Number.isFinite(section.gain_db) ? section.gain_db : 0);
      });
    }
    return true;
  }

  /* Adds a section at the frequency the gesture names, seeded exactly as the
   * form seeds a slot switched to peaking. The 2-per-channel cap is the wire
   * model's (AIUS_DSP_MODEL_EQ_MAX), so at the cap it says so rather than
   * silently doing nothing. */
  function addSection(entity, frequencyHz) {
    beginSurfaceEdit(entity);
    const sections = entityCascade(entity).sections;
    const slot = sections.findIndex((section) => section.type === 'none');
    if (slot === -1) {
      setCurveStatus(
        `${entity.label} already uses both of its ${EQ_SLOTS} sections -- change or remove one before adding another.`,
      );
      return false;
    }
    for (const base of entity.bases) {
      const path = `${base}.eq.${slot}`;
      writeChoice(`${path}.type`, 'peaking');
      writeField(`${path}.frequency_hz`, roundHz(frequencyHz));
      writeField(`${path}.q`, EQ_SEED.q);
      writeField(`${path}.gain_db`, EQ_SEED.gain_db);
    }
    state.activeSlot = slot;
    setCurveStatus(`Added a peaking section at ${roundHz(frequencyHz)} Hz on ${entity.label}.`);
    drawCurveSurface();
    return true;
  }

  /* Removes the ACTIVE section from every member of the entity. The form's
   * page-level "none" IS removal on the wire -- the slot drops out of eq[] at
   * collect -- so the surface speaks exactly the form's vocabulary and a
   * removal round-trips identically whichever surface performed it. */
  function removeSection(entity, slot) {
    const section = entityCascade(entity).sections[slot];
    if (!section || section.type === 'none') {
      setCurveStatus(`${entity.label} has no section ${slot + 1} to remove.`);
      return false;
    }
    beginSurfaceEdit(entity);
    /* A bypass section has no meaningful frequency; name only what is real. */
    const summary = isActiveSection(section)
      ? `${section.type} at ${roundHz(section.frequency_hz)} Hz`
      : section.type;
    for (const base of entity.bases) {
      writeChoice(`${base}.eq.${slot}.type`, 'none');
    }
    const remaining = entityCascade(entity).sections.filter(isActiveSection);
    state.activeSlot = remaining.length > 0 ? remaining[0].slot : 0;
    hideReadout();
    setCurveStatus(`Removed ${entity.label}'s section ${slot + 1} (${summary}); the slot is free again.`);
    drawCurveSurface();
    return true;
  }

  function selectEntity(key) {
    state.curveEntity = key;
    const sections = entityCascade(selectedEntity()).sections.filter(isActiveSection);
    state.activeSlot = sections.length > 0 ? sections[0].slot : 0;
    hideReadout();
    drawCurveSurface();
  }

  /* Pointer work needs the CLIENT rect, not the backing store: CSS scales the
   * canvas to the card width, so a client coordinate only means something
   * against the rect the browser actually laid out. A zero-sized rect (no
   * layout yet, or a headless DOM) simply yields no handle. */
  function clientRect() {
    const rect = $('curve').getBoundingClientRect();
    return rect.width > 0 && rect.height > 0 ? rect : null;
  }

  function handleUnder(event) {
    const rect = clientRect();
    if (!rect) return null;
    const points = handlePoints(entityCascade(selectedEntity()).sections, rect);
    return pickHandle(points, event.clientX, event.clientY, HANDLE_GRAB_PX);
  }

  function attachCurveSurface() {
    const canvas = $('curve');
    canvas.addEventListener('pointerdown', (event) => {
      const handle = handleUnder(event);
      if (!handle) return;
      /* No write on the press itself: a click that does not move must not nudge
       * the section it grabbed. Clicking a handle DOES select it. */
      event.preventDefault();
      state.activeSlot = handle.slot;
      beginSurfaceEdit(selectedEntity());
      state.drag = { entity: selectedEntity(), slot: handle.slot, type: handle.type, pointerId: event.pointerId };
      /* Suppress panning for the duration of the drag ONLY: an unconditional
       * touch-action:none made the whole plot a scroll trap (UX finding E1). */
      canvas.style.touchAction = 'none';
      showReadout(handle.slot);
      drawCurveSurface();
      if (typeof canvas.setPointerCapture === 'function') canvas.setPointerCapture(event.pointerId);
    });
    canvas.addEventListener('pointermove', (event) => {
      if (state.drag) {
        dragTo(event);
        return;
      }
      /* Disclose the target: a crosshair everywhere said nothing about where
       * the draggable points are (UX finding E10). */
      canvas.style.cursor = handleUnder(event) ? 'pointer' : '';
    });
    canvas.addEventListener('pointerup', (event) => {
      if (event.pointerType !== 'touch') return;
      const tap = { time: Date.now(), x: event.clientX, y: event.clientY };
      const previous = state.lastTap;
      state.lastTap = tap;
      if (
        previous &&
        tap.time - previous.time <= DOUBLE_TAP_MS &&
        Math.hypot(tap.x - previous.x, tap.y - previous.y) <= DOUBLE_TAP_PX
      ) {
        state.lastTap = null;
        addAt(event);
      }
    });
    canvas.addEventListener('dblclick', (event) => addAt(event));
    /* passive: false, because the page must keep the wheel from scrolling while
     * it IS the Q control -- which it only is while a handle is held or the
     * plot has focus. Anywhere else the wheel belongs to the page (E3). */
    canvas.addEventListener(
      'wheel',
      (event) => {
        const grabbed = state.drag !== null;
        if (!grabbed && document.activeElement !== canvas) return;
        const entity = grabbed ? state.drag.entity : selectedEntity();
        const slot = grabbed ? state.drag.slot : state.activeSlot;
        const section = entityCascade(entity).sections[slot];
        if (!isActiveSection(section)) return;
        event.preventDefault();
        beginSurfaceEdit(entity);
        surfaceWrite(entity, slot, 'q', stepQ(numberOf(`${entity.bases[0]}.eq.${slot}.q`, NaN), event.deltaY));
        showReadout(slot);
      },
      { passive: false },
    );
    canvas.addEventListener('keydown', onCurveKey);
    canvas.addEventListener('blur', hideReadout);
  }

  function addAt(event) {
    const rect = clientRect();
    if (!rect) return;
    /* A double-click that lands on a handle is handle fiddling, not an add. */
    if (handleUnder(event)) return;
    addSection(selectedEntity(), xToHz(event.clientX, rect));
  }

  /* The keyboard path to the same three parameters, so the plot is not a
   * mouse-only instrument (UX finding E11). */
  function onCurveKey(event) {
    const entity = selectedEntity();
    const sections = entityCascade(entity).sections;
    if (event.key === 'PageUp' || event.key === 'PageDown') {
      event.preventDefault();
      const step = event.key === 'PageDown' ? 1 : EQ_SLOTS - 1;
      state.activeSlot = (state.activeSlot + step) % EQ_SLOTS;
      showReadout(state.activeSlot);
      drawCurveSurface();
      return;
    }
    if (event.key === 'Delete' || event.key === 'Backspace') {
      event.preventDefault();
      removeSection(entity, state.activeSlot);
      return;
    }
    const slot = state.activeSlot;
    const section = sections[slot];
    if (!isActiveSection(section)) return;
    const path = `${entity.bases[0]}.eq.${slot}`;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      beginSurfaceEdit(entity);
      surfaceWrite(entity, slot, 'frequency_hz', nudgeHz(numberOf(`${path}.frequency_hz`, NaN), event.key === 'ArrowRight' ? 1 : -1));
      showReadout(slot);
      return;
    }
    if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
      if (section.type !== 'peaking') return; /* a highpass has no gain to nudge */
      event.preventDefault();
      beginSurfaceEdit(entity);
      const gain = numberOf(`${path}.gain_db`, 0);
      const next = roundDb(clamp(gain + (event.key === 'ArrowUp' ? NUDGE_DB : -NUDGE_DB), -CURVE_SPAN_DB, CURVE_SPAN_DB));
      surfaceWrite(entity, slot, 'gain_db', next);
      showReadout(slot);
    }
  }

  function dragTo(event) {
    const drag = state.drag;
    const rect = clientRect();
    if (!rect) return;
    const point = pointToEq(rect, event.clientX, event.clientY);
    surfaceWrite(drag.entity, drag.slot, 'frequency_hz', point.frequency_hz);
    /* Only a peaking section carries gain -- the rule collectEqSection()
     * applies -- so a highpass handle moves horizontally only. */
    if (drag.type === 'peaking') surfaceWrite(drag.entity, drag.slot, 'gain_db', point.gain_db);
    showReadout(drag.slot);
  }

  function endDrag() {
    if (state.drag === null) return;
    const { pointerId } = state.drag;
    state.drag = null;
    const canvas = $('curve');
    canvas.style.touchAction = '';
    hideReadout();
    if (typeof canvas.releasePointerCapture !== 'function' || !Number.isFinite(pointerId)) return;
    try {
      canvas.releasePointerCapture(pointerId);
    } catch (captureError) {
      /* the pointer was already released (or never captured) */
    }
  }

  /* The numbers a gesture is writing, beside the handle writing them: a drag
   * used to change three fields 4000 px down the page with nothing on screen
   * to read (UX finding E10). Positioned against a unit rect, so it needs no
   * layout of its own. */
  function showReadout(slot) {
    const entity = selectedEntity();
    const path = `${entity.bases[0]}.eq.${slot}`;
    const type = field(`${path}.type`).value;
    const hz = numberOf(`${path}.frequency_hz`, NaN);
    const q = numberOf(`${path}.q`, NaN);
    const gain = numberOf(`${path}.gain_db`, 0);
    const parts = [Number.isFinite(hz) ? `${roundHz(hz)} Hz` : '-- Hz'];
    if (type === 'peaking') parts.push(`${gain >= 0 ? '+' : ''}${gain.toFixed(1)} dB`);
    parts.push(`Q ${Number.isFinite(q) ? q.toFixed(2) : '--'}`);
    const readout = $('curve-readout');
    readout.textContent = parts.join(' ');
    readout.hidden = false;
    const unit = { left: 0, top: 0, width: 1, height: 1 };
    readout.style.left = `${(clamp(hzToX(Number.isFinite(hz) ? hz : CURVE_MIN_HZ, unit), 0, 1) * 100).toFixed(1)}%`;
    readout.style.top = `${(clamp(dbToY(type === 'peaking' ? gain : 0, unit), 0, 1) * 100).toFixed(1)}%`;
  }

  function hideReadout() {
    const readout = $('curve-readout');
    readout.hidden = true;
    readout.textContent = '';
  }

  const setCurveStatus = (text) => {
    $('curve-status').textContent = text;
  };

  /* ---- visual surface: the downmix matrix --------------------------------
   * The routing diagram is a view of the SAME six matrix fields the form below
   * edits: it reads them through numberOf() and writes them through
   * writeField()/writeCheck(), so a dragged gain is indistinguishable from a
   * typed one downstream -- validation, the dirty pill, the resolved headroom
   * and the request-body count all ride along. There is no second model here,
   * and nothing on the diagram is editable that the model has no field for. */

  const matrixCanvas = () => $('matrix-diagram');

  const CLIPFREE_POLICY_MESSAGE =
    'Headroom is resolved by the clip-free policy (1 / (1 + centre + surround + |LFE|)), so it cannot be dragged. Turn the clip-free chip off to set it by hand.';

  function readMatrixValues() {
    const values = {
      clipfree: field('matrix.clipfree').checked,
      crossoverHz: numberOf('matrix.lfe_crossover_hz', NaN),
    };
    for (const handle of MATRIX_HANDLES) values[handle.key] = numberOf(handle.path, NaN);
    return values;
  }

  const activeMatrixHandle = () => matrixHandleOf(state.matrixHandle) || MATRIX_HANDLES[0];

  const matrixLocked = (handle) => handle.key === 'headroom' && field('matrix.clipfree').checked;

  function drawMatrixSurface() {
    const canvas = matrixCanvas();
    const context = canvas.getContext('2d');
    if (!context) return;
    const scale = dprOf(canvas);
    const rect = { left: 0, top: 0, width: canvas.width, height: canvas.height };
    const values = readMatrixValues();
    const layout = matrixLayout(rect);
    context.clearRect(0, 0, rect.width, rect.height);
    drawMatrixTracks(context, layout, rect, scale);
    drawMatrixEdges(context, values, rect, scale);
    drawMatrixNodes(context, layout, scale);
    drawMatrixHandles(context, values, rect, scale);
    drawMatrixLabels(context, values, rect, scale);

    const chip = $('matrix-clipfree');
    chip.setAttribute('aria-pressed', values.clipfree ? 'true' : 'false');
    chip.classList.toggle('chip--on', values.clipfree);

    const active = activeMatrixHandle();
    $('matrix-caption').textContent = `${active.label} selected: ${matrixGainLabel(values[active.key])}${
      matrixLocked(active) ? ', resolved by the clip-free policy' : ''
    }.`;
  }

  /* The axis each handle rides, drawn so the drag surface discloses itself
   * rather than hiding until a pointer happens to find it (UX finding E10). */
  function drawMatrixTracks(context, layout, rect, scale) {
    context.strokeStyle = '#232b35';
    context.lineWidth = scale;
    context.beginPath();
    for (const handle of MATRIX_HANDLES) {
      const x = rect.left + rect.width * handle.column;
      context.moveTo(x, layout.top);
      context.lineTo(x, layout.bottom);
    }
    context.stroke();
  }

  function drawMatrixEdges(context, values, rect, scale) {
    for (const edge of matrixEdges(values, rect)) {
      context.strokeStyle = edge.colour;
      context.lineWidth = edge.style.width * scale;
      context.globalAlpha = edge.style.alpha;
      context.setLineDash(edge.style.muted ? [4 * scale, 4 * scale] : []);
      context.beginPath();
      edge.points.forEach((point, index) => {
        if (index === 0) context.moveTo(point.x, point.y);
        else context.lineTo(point.x, point.y);
      });
      context.stroke();
    }
    context.setLineDash([]);
    context.globalAlpha = 1;
    context.lineWidth = scale;
  }

  /* Nodes are squares and handles are circles, so the picture says which of the
   * two a shape is before anything is read. */
  function drawMatrixNodes(context, layout, scale) {
    const size = MATRIX_NODE_PX * scale;
    context.font = `${Math.round(11 * scale)}px ui-monospace,monospace`;
    for (const node of [...layout.sources, ...layout.outputs]) {
      context.fillStyle = '#5d6b78';
      context.fillRect(node.x - size / 2, node.y - size / 2, size, size);
      context.fillStyle = '#9aa8b5';
      context.textAlign = layout.outputs.includes(node) ? 'left' : 'right';
      const offset = layout.outputs.includes(node) ? size : -size;
      context.fillText(node.label, node.x + offset, node.y + 4 * scale);
    }
    context.textAlign = 'left';
  }

  function drawMatrixHandles(context, values, rect, scale) {
    for (const handle of matrixHandlePoints(values, rect)) {
      const chosen = handle.key === state.matrixHandle;
      /* A locked handle is drawn as the dim thing it is, exactly as the form
       * greys the field the clip-free policy owns. */
      context.fillStyle = handle.locked ? '#5d6b78' : handle.colour;
      context.strokeStyle = chosen ? '#e8edf2' : '#101318';
      context.lineWidth = (chosen ? 2 : 1) * scale;
      context.beginPath();
      context.arc(handle.x, handle.y, MATRIX_HANDLE_RADIUS_PX * scale, 0, Math.PI * 2);
      context.fill();
      context.stroke();
    }
    context.lineWidth = scale;
  }

  function drawMatrixLabels(context, values, rect, scale) {
    context.font = `${Math.round(11 * scale)}px ui-monospace,monospace`;
    context.textAlign = 'center';

    /* The unity references say what they are: the matrix scales everything else
     * and leaves these two alone. */
    const edges = matrixEdges(values, rect);
    for (const edge of edges.filter((entry) => entry.fixed)) {
      const from = edge.points[0];
      const to = edge.points[edge.points.length - 1];
      context.fillStyle = '#5d6b78';
      context.fillText('1.0', (from.x + to.x) / 2, (from.y + to.y) / 2 - 5 * scale);
    }

    /* The crossover, on the LFE path and click-to-edit. */
    const box = matrixCrossoverBox(rect, scale);
    context.fillStyle = '#74d7ff';
    context.fillText(matrixCrossoverLabel(values.crossoverHz), box.left + box.width / 2, box.top + box.height * 0.75);

    /* Each handle's own number under it: the dB reading is the readout chip's
     * and the caption's job, so the diagram stays legible at phone width. */
    context.fillStyle = '#aab4c0';
    for (const handle of matrixHandlePoints(values, rect)) {
      context.fillText(displayNumber(handle.value) || '--', handle.x, handle.y + 19 * scale);
    }
    context.textAlign = 'left';
  }

  /* ---- visual surface: the downmix matrix's gestures ---------------------- */

  function matrixClientRect() {
    const rect = matrixCanvas().getBoundingClientRect();
    return rect.width > 0 && rect.height > 0 ? rect : null;
  }

  function matrixHandleUnder(event) {
    const rect = matrixClientRect();
    if (!rect) return null;
    return pickHandle(matrixHandlePoints(readMatrixValues(), rect), event.clientX, event.clientY, MATRIX_GRAB_PX);
  }

  function matrixLabelUnder(event) {
    const rect = matrixClientRect();
    return rect !== null && matrixHitsBox(matrixCrossoverBox(rect), event.clientX, event.clientY);
  }

  /* The crossover is a number with units and a 20..500 bound; an in-canvas
   * editor for it would be a second, worse copy of a field that already exists,
   * so the label takes the reader to the real one (the E6 reveal pattern). */
  function revealCrossover() {
    revealField('matrix.lfe_crossover_hz');
    setMatrixStatus('LFE crossover: the field in the Downmix matrix card below is focused -- type the corner there.');
  }

  function refuseLockedHeadroom() {
    setMatrixStatus(CLIPFREE_POLICY_MESSAGE);
  }

  function attachMatrixSurface() {
    const canvas = matrixCanvas();
    canvas.addEventListener('pointerdown', (event) => {
      const handle = matrixHandleUnder(event);
      if (!handle) {
        if (matrixLabelUnder(event)) revealCrossover();
        return;
      }
      /* No write on the press itself, exactly as on the plot: a click that does
       * not move must not nudge the gain it grabbed. Pressing it DOES select
       * it, so the keyboard and the wheel follow the pointer. */
      event.preventDefault();
      state.matrixHandle = handle.key;
      if (handle.locked) {
        refuseLockedHeadroom();
        drawMatrixSurface();
        return;
      }
      state.matrixDrag = { key: handle.key, pointerId: event.pointerId };
      canvas.style.touchAction = 'none';
      showMatrixReadout(handle.key);
      drawMatrixSurface();
      if (typeof canvas.setPointerCapture === 'function') canvas.setPointerCapture(event.pointerId);
    });
    canvas.addEventListener('pointermove', (event) => {
      if (state.matrixDrag) {
        matrixDragTo(event);
        return;
      }
      canvas.style.cursor = matrixHandleUnder(event) || matrixLabelUnder(event) ? 'pointer' : '';
    });
    /* Double-click on a handle resets that channel to the factory default
     * (2026-07-27 feedback, item 3) -- through resetField(), so it is the same
     * write the form's own reset buttons make. Mouse-only, like dblclick
     * itself; the per-field reset in the form below stays the touch path. */
    canvas.addEventListener('dblclick', (event) => {
      const handle = matrixHandleUnder(event);
      if (!handle) return;
      state.matrixHandle = handle.key;
      if (handle.locked) {
        refuseLockedHeadroom();
        drawMatrixSurface();
        return;
      }
      resetField(handle.path);
      setMatrixStatus(
        `${handle.label} reset to the factory default: ${matrixGainLabel(numberOf(handle.path, NaN))}.`,
      );
    });
    /* passive: false for the same reason the plot needs it -- the wheel is the
     * fine adjustment only while a handle is held or the diagram has focus. */
    canvas.addEventListener(
      'wheel',
      (event) => {
        const grabbed = state.matrixDrag !== null;
        if (!grabbed && document.activeElement !== canvas) return;
        /* The handle under the pointer when there is one, so a notch lands
         * where the reader is looking; the selected one otherwise. */
        const under = grabbed ? state.matrixDrag : matrixHandleUnder(event);
        const handle = under ? matrixHandleOf(under.key) : activeMatrixHandle();
        event.preventDefault();
        state.matrixHandle = handle.key;
        if (matrixLocked(handle)) {
          refuseLockedHeadroom();
          drawMatrixSurface();
          return;
        }
        writeField(handle.path, stepMatrixValue(numberOf(handle.path, NaN), event.deltaY, handle.bound));
        showMatrixReadout(handle.key);
      },
      { passive: false },
    );
    canvas.addEventListener('keydown', onMatrixKey);
    canvas.addEventListener('blur', hideMatrixReadout);
  }

  /* The keyboard path to the same four values, so the diagram is not a
   * mouse-only instrument (UX finding E11). */
  function onMatrixKey(event) {
    if (event.key === 'PageUp' || event.key === 'PageDown') {
      event.preventDefault();
      const index = MATRIX_HANDLES.indexOf(activeMatrixHandle());
      const step = event.key === 'PageDown' ? 1 : MATRIX_HANDLES.length - 1;
      state.matrixHandle = MATRIX_HANDLES[(index + step) % MATRIX_HANDLES.length].key;
      showMatrixReadout(state.matrixHandle);
      drawMatrixSurface();
      return;
    }
    if (event.key === 'Enter') {
      event.preventDefault();
      revealCrossover();
      return;
    }
    const direction =
      event.key === 'ArrowUp' || event.key === 'ArrowRight'
        ? 1
        : event.key === 'ArrowDown' || event.key === 'ArrowLeft'
          ? -1
          : 0;
    if (direction === 0) return;
    event.preventDefault();
    const handle = activeMatrixHandle();
    if (matrixLocked(handle)) {
      refuseLockedHeadroom();
      return;
    }
    writeField(handle.path, nudgeMatrixValue(numberOf(handle.path, NaN), direction, handle.bound));
    showMatrixReadout(handle.key);
  }

  /* Vertical only: a gain has one axis, and the column a handle rides is part of
   * the routing picture rather than a value. */
  function matrixDragTo(event) {
    const rect = matrixClientRect();
    if (!rect) return;
    const handle = matrixHandleOf(state.matrixDrag.key);
    writeField(handle.path, matrixYToValue(event.clientY, rect, handle.bound));
    showMatrixReadout(handle.key);
  }

  function endMatrixDrag() {
    if (state.matrixDrag === null) return;
    const { pointerId } = state.matrixDrag;
    state.matrixDrag = null;
    const canvas = matrixCanvas();
    canvas.style.touchAction = '';
    hideMatrixReadout();
    if (typeof canvas.releasePointerCapture !== 'function' || !Number.isFinite(pointerId)) return;
    try {
      canvas.releasePointerCapture(pointerId);
    } catch (captureError) {
      /* the pointer was already released (or never captured) */
    }
  }

  /* The policy chip IS the form checkbox: one write path, so the resolved
   * headroom, the read-only field and the locked handle all follow from it. */
  function toggleClipfree() {
    const next = !field('matrix.clipfree').checked;
    writeCheck('matrix.clipfree', next);
    setMatrixStatus(
      next
        ? 'Clip-free headroom on: the device resolves headroom as 1 / (1 + centre + surround + |LFE|), so its handle is locked.'
        : 'Clip-free headroom off: drag the headroom handle to set it by hand.',
    );
  }

  /* The number a gesture is writing, beside the handle writing it -- positioned
   * against a unit rect, so it needs no layout of its own. */
  function showMatrixReadout(key) {
    const values = readMatrixValues();
    const point = matrixHandlePoints(values, { left: 0, top: 0, width: 1, height: 1 }).find(
      (handle) => handle.key === key,
    );
    const readout = $('matrix-readout');
    readout.textContent = `${point.label} ${matrixGainLabel(values[key])}`;
    readout.hidden = false;
    readout.style.left = `${(clamp(point.x, 0, 1) * 100).toFixed(1)}%`;
    readout.style.top = `${(clamp(point.y, 0, 1) * 100).toFixed(1)}%`;
  }

  function hideMatrixReadout() {
    const readout = $('matrix-readout');
    readout.hidden = true;
    readout.textContent = '';
  }

  const setMatrixStatus = (text) => {
    $('matrix-status').textContent = text;
  };

  /* ---- visual surface: meters -------------------------------------------- */

  function renderStripLevels(meters) {
    const inputs = Array.isArray(meters.input) ? meters.input : [];
    CHANNELS.forEach(([key], index) => setBar(`bar-in-${key}`, dbfsOf(inputs[index], 'peak_dbfs')));
    const output = meters.output && typeof meters.output === 'object' ? meters.output : {};
    setBar('bar-out-l', channelDbfs(output.peak_dbfs, 0));
    setBar('bar-out-r', channelDbfs(output.peak_dbfs, 1));
  }

  /* Load is against the per-block cycle budget, not wall clock: the snapshot's
   * counters are DWT cycles for one decoded block. One phrasing for both
   * cells, because two ways of saying "percent of the budget" read as two
   * different quantities (UX finding E12). */
  function renderDspLoad(meters) {
    const rate = Number.isFinite(meters.sample_rate_hz) ? meters.sample_rate_hz : 0;
    const load = dspLoadPercent(meters.dsp_cycles_last, rate);
    const peak = dspLoadPercent(meters.dsp_cycles_max, rate);
    $('meter-load').textContent =
      load === null ? '--' : `${load.toFixed(1)} % of budget, ${(100 - load).toFixed(1)} % headroom`;
    $('meter-load').dataset.zone = loadZone(load);
    $('meter-load-peak').textContent = peak === null ? '--' : `${peak.toFixed(1)} % of budget`;
    $('meter-load-peak').dataset.zone = loadZone(peak);
  }

  function setLimiterBar(db) {
    const reduction = Number.isFinite(db) ? clamp(-db, 0, LIMITER_SPAN_DB) : 0;
    $('gr-fill').style.width = `${((reduction / LIMITER_SPAN_DB) * 100).toFixed(1)}%`;
    $('gr-value').textContent = Number.isFinite(db) ? `${formatDb(db)} dB` : '--';
  }

  /* Each snapshot's gain reduction is the value at the publish instant, not a
   * windowed average, so the strip is a record of instants at the poll rate --
   * said as much in the note under the canvas. */
  function pushLimiter(db) {
    state.limiterHistory.push(Number.isFinite(db) ? clamp(db, -LIMITER_SPAN_DB, 0) : 0);
    if (state.limiterHistory.length > LIMITER_HISTORY_MAX) state.limiterHistory.shift();
    setLimiterBar(db);
    drawLimiterHistory();
  }

  function blankStrip() {
    for (const [key] of CHANNELS) setBar(`bar-in-${key}`, null);
    setBar('bar-out-l', null);
    setBar('bar-out-r', null);
    $('meter-load').textContent = '--';
    $('meter-load').dataset.zone = 'idle';
    $('meter-load-peak').textContent = '--';
    $('meter-load-peak').dataset.zone = 'idle';
    setLimiterBar(null);
    state.limiterHistory = [];
    drawLimiterHistory();
  }

  function drawLimiterHistory() {
    const canvas = $('gr-history');
    const context = canvas.getContext('2d');
    if (!context) return;
    const scale = dprOf(canvas);
    const { width, height } = canvas;
    context.clearRect(0, 0, width, height);
    /* 0 dB is the top rail: gain reduction only ever goes down from it. */
    context.strokeStyle = '#2a3542';
    context.lineWidth = scale;
    context.beginPath();
    context.moveTo(0, 0);
    context.lineTo(width, 0);
    context.stroke();

    const values = state.limiterHistory;
    if (values.length === 0) return;
    /* Newest at the right at a FIXED step, so a filling buffer grows into the
     * strip instead of stretching to fit. */
    const step = width / (LIMITER_HISTORY_MAX - 1);
    context.strokeStyle = '#e8c46a';
    context.beginPath();
    values.forEach((value, index) => {
      const x = width - (values.length - 1 - index) * step;
      const y = (Math.min(0, value) / -LIMITER_SPAN_DB) * height;
      if (index === 0) context.moveTo(x, y);
      else context.lineTo(x, y);
    });
    context.stroke();
  }

  /* ---- page state -------------------------------------------------------- */

  function setApplyState(next) {
    state.applyState = next;
    shared.setPill($('apply-state'), next);
  }

  const setStatus = (text) => {
    $('status').textContent = text;
  };

  function showError(text) {
    $('errors').textContent = text;
    $('errors').hidden = false;
  }

  /* A count with the first message and the field 4000 px away was unusable:
   * each problem is a link that scrolls to and focuses its own input, and the
   * owning card wears a marker (UX finding E6). */
  function showFieldProblems(problems) {
    const summary = $('errors');
    summary.textContent = '';
    summary.append(
      document.createTextNode(`${problems.length} field${problems.length === 1 ? '' : 's'} out of range: `),
    );
    problems.slice(0, ERROR_LINK_MAX).forEach((problem, index) => {
      if (index > 0) summary.append(document.createTextNode(' '));
      const link = document.createElement('a');
      link.href = `#f:${problem.path}`;
      link.textContent = problem.message;
      link.addEventListener('click', (event) => {
        event.preventDefault();
        revealField(problem.path);
      });
      summary.append(link);
    });
    if (problems.length > ERROR_LINK_MAX) {
      summary.append(document.createTextNode(` (+${problems.length - ERROR_LINK_MAX} more)`));
    }
    summary.hidden = false;
  }

  function revealField(path) {
    const input = field(path);
    if (!input) return;
    if (typeof input.scrollIntoView === 'function') input.scrollIntoView({ block: 'center' });
    if (typeof input.focus === 'function') input.focus();
  }

  function clearError() {
    $('errors').textContent = '';
    $('errors').hidden = true;
  }

  function setStale(stale) {
    $('stale').hidden = !stale;
  }

  /* ---- boot -------------------------------------------------------------- */

  /* The pure half of the visual surface is also its unit-test surface: with no
   * layout engine under happy-dom, the only way to prove the drag arithmetic
   * and the curve maths is to call them with literal rects. Nothing on the page
   * reads this back -- it is an export, not a channel. */
  window.aiusDspEditorMath = Object.freeze({
    cascadeMagnitudeDb,
    cascadeSignature,
    curvePoints,
    dbToY,
    dspBudgetCycles,
    dspLoadPercent,
    factoryValueOf,
    handlePoints,
    hzToX,
    levelFraction,
    levelZone,
    loadZone,
    matrixCrossoverBox,
    matrixCrossoverLabel,
    matrixEdges,
    matrixGainLabel,
    matrixHandlePoints,
    matrixHitsBox,
    matrixLayout,
    matrixPathStyle,
    matrixValueToY,
    matrixYToValue,
    nudgeHz,
    nudgeMatrixValue,
    pickHandle,
    pointToEq,
    resolveCascade,
    sectionMagnitudeDb,
    stepMatrixValue,
    stepQ,
    xToHz,
    yToDb,
  });

  buildForm();
  $('apply').onclick = () => void act(applyConfig);
  $('reload').onclick = () =>
    void act(async () => {
      if (!confirmDiscard()) {
        setStatus('Kept the edits on this page; the device was not re-read.');
        return;
      }
      await loadConfig();
    });
  $('stale-reload').onclick = () =>
    void act(async () => {
      if (!confirmDiscard()) {
        setStatus('Kept the edits on this page; the device was not re-read.');
        return;
      }
      await loadConfig();
    });
  /* ONE toggle over the old silent per-keystroke store (2026-07-27 feedback
   * item 7; the save/edit pair merged 2026-07-28), on the SAME sd-shared
   * localStorage slot. Unlocked it saves and locks, so a stray keystroke
   * cannot diverge the field from the store; locked it unlocks for a change.
   * An empty save clears the store and leaves the field editable -- there is
   * nothing to lock. The LABEL is the state: one control never has to explain
   * which of two buttons is the live one. */
  function setTokenLocked(locked) {
    $('token').readOnly = locked;
    $('token-toggle').textContent = locked ? 'Change token' : 'Save token';
  }
  $('token-toggle').addEventListener('click', () => {
    if ($('token').readOnly) {
      setTokenLocked(false);
      $('token').focus();
      return;
    }
    const value = $('token').value.trim();
    shared.storeToken(value);
    setTokenLocked(value !== '');
    setStatus(value === '' ? 'SD auth token cleared from this browser.' : 'SD auth token saved in this browser.');
  });
  $('token').value = shared.readStoredToken();
  setTokenLocked($('token').value !== '');
  $('preset-name').addEventListener('input', refreshPresetNameHint);
  $('preset-save').onclick = () =>
    void act(async () => {
      clearPresetError();
      /* A preset is a model snapshot; never snapshot a form that Apply itself
       * would refuse. */
      const problems = validateAll();
      if (problems.length > 0) {
        showPresetError(
          `Fix the form first -- ${problems.length} field${problems.length === 1 ? '' : 's'} out of range: ${problems[0].message}`,
        );
        return;
      }
      await savePreset($('preset-name').value.trim(), collectPresetGroups());
    });
  $('preset-download').onclick = () => void act(downloadCurrent);
  $('preset-upload').onclick = () => void act(uploadPreset);
  $('preset-refresh').onclick = () => void act(refreshPresets);
  /* The last guard on the way out: an unapplied edit is work, and the browser
   * is the only thing that can ask before the tab goes (UX finding E2). */
  window.addEventListener('beforeunload', (event) => {
    if (!isDirty()) return;
    event.preventDefault();
    event.returnValue = '';
  });
  setApplyState('loading');
  /* Authored text, JS-stamped data-state: the shared tone map in replays.css
   * keys off the state, and dsp-editor.css adds this one word to it. */
  shared.setPill($('dirty-state'), 'unsaved changes');
  refreshPresetNameHint();
  void guard(loadConfig);
  void guard(refreshPresets);
  void guard(pollStatus);
  setInterval(() => void guard(pollMeters), METER_POLL_MS);
  setInterval(() => void guard(pollStatus), STATUS_POLL_MS);
})();
