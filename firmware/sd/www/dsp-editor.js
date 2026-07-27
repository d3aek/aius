/* Aius SD-hosted DSP control surface.
 *
 * OWNS: the whole /sd/www/index.html page -- building the config form from the
 * field table below, rendering GET /api/dsp/config into it, validating edits
 * against the firmware parser's bounds, PUTting the model back with an
 * optimistic-concurrency token, and polling GET /api/meters at 10 Hz.
 *
 * DOES NOT OWN: the replay manager page (replays.js) and anything the firmware
 * decides -- the device is the authority on what a valid config is. The bounds
 * here mirror lib/aius_dsp_config (and scripts/build_config.py) so a mistake is
 * caught before the round trip, never to replace the device-side validation.
 *
 * This file is served off the Teensy's SD card. It is deliberately a plain
 * classic script with no imports, no build step, and no shared UI kit -- the
 * device cannot reach a host application's module mounts. Same constraint as
 * replays.js; keep the two pages looking like one product.
 */
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);

  const METER_POLL_MS = 100; /* 10 Hz, per the API contract */
  const METER_RETRY_MS = 5000; /* back-off while /api/meters is absent */
  const PENDING_POLL_MS = 500; /* the apply lands at an audio block boundary */
  const PENDING_POLL_MAX = 40; /* ~20 s before the page stops waiting */
  const WAVEFORM_MAX_POINTS = 128; /* AIUS_METER_WAVEFORM_MAX */
  const WAVEFORM_FULL_SCALE = 1000; /* waveform points are per-mille of full scale */
  const METER_FLOOR_DBFS = -120; /* every emitted level is finite, floored here */
  const MODEL_MAX_BYTES = 12288; /* AIUS_DSP_MODEL_MAX_JSON_BYTES */
  const EQ_SLOTS = 2; /* AIUS_DSP_MODEL_EQ_MAX */

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
  /* Accepted trade (task #7 interview): a stored token is readable by any
   * script served from the device origin for as long as it is stored. */
  const TOKEN_STORAGE_KEY = 'aius-sd-auth';

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
    /* Names as stored on the card (with case), .json stripped. */
    presets: [],
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
   * shown without being sent (or vice versa). */

  const eqFields = (base, label) => [
    { path: `${base}.type`, kind: 'eq-type', label: `${label} type`, caption: 'Type' },
    {
      path: `${base}.frequency_hz`,
      kind: 'number',
      label: `${label} frequency`,
      caption: 'Frequency (Hz)',
      min: 10,
      max: 24000,
      unit: ' Hz',
    },
    { path: `${base}.q`, kind: 'number', label: `${label} Q`, caption: 'Q', min: 0.1, max: 20 },
    {
      path: `${base}.gain_db`,
      kind: 'number',
      label: `${label} gain`,
      caption: 'Gain (dB)',
      min: -24,
      max: 24,
      unit: ' dB',
    },
  ];

  const MATRIX_FIELDS = [
    { path: 'matrix.center_gain_linear', kind: 'number', label: 'Centre gain', min: 0, max: 4, unit: ' linear' },
    { path: 'matrix.surround_gain_linear', kind: 'number', label: 'Surround gain', min: 0, max: 4, unit: ' linear' },
    { path: 'matrix.lfe_gain_linear', kind: 'number', label: 'LFE gain', min: 0, max: 4, unit: ' linear' },
    /* 20..500 mirrors MATRIX_LFE_CROSSOVER_{MIN,MAX}_HZ in the PUT validator. 0 is
     * deliberately OUTSIDE that range: "no LFE" is lfe_gain_linear = 0, so a corner
     * of zero would be a second encoding of one state. */
    { path: 'matrix.lfe_crossover_hz', kind: 'number', label: 'LFE crossover', min: 20, max: 500, unit: ' Hz' },
    { path: 'matrix.headroom_linear', kind: 'number', label: 'Headroom', min: 0, max: 1, exclusiveMin: true, unit: ' linear' },
    { path: 'matrix.clipfree', kind: 'bool', label: 'Clip-free headroom policy' },
  ];

  const LIMITER_FIELDS = [
    { path: 'limiter.enabled', kind: 'bool', label: 'Limiter enabled' },
    { path: 'limiter.threshold_dbfs', kind: 'number', label: 'Threshold', min: -24, max: 0, unit: ' dBFS' },
    { path: 'limiter.attack_ms', kind: 'number', label: 'Attack', min: 0.1, max: 100, unit: ' ms' },
    { path: 'limiter.release_ms', kind: 'number', label: 'Release', min: 1, max: 1000, unit: ' ms' },
    { path: 'limiter.lookahead_ms', kind: 'number', label: 'Lookahead', min: 0, max: 5, unit: ' ms' },
  ];

  const field = (path) => $(`f:${path}`);

  /* ---- DOM construction -------------------------------------------------- */

  function buildField(descriptor) {
    const holder = document.createElement('label');
    holder.className = 'field';
    /* The caption is the compact form; `label` is the full subject a validation
     * message names ("Front left section 1 frequency must be ..."). */
    const caption = document.createElement('span');
    caption.className = 'field__label';
    caption.textContent = descriptor.caption || descriptor.label;
    holder.append(caption);

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
      holder.classList.add('field--check');
    } else {
      input = document.createElement('input');
      input.type = 'number';
      input.step = 'any';
      input.dataset.min = String(descriptor.min);
      input.dataset.max = String(descriptor.max);
      if (descriptor.exclusiveMin) input.dataset.exclusiveMin = 'true';
      if (descriptor.unit) input.dataset.unit = descriptor.unit;
    }
    input.id = `f:${descriptor.path}`;
    input.dataset.field = descriptor.path;
    input.dataset.label = descriptor.label;
    input.dataset.kind = descriptor.kind;
    holder.append(input);

    const error = document.createElement('span');
    error.className = 'field__error';
    error.id = `e:${descriptor.path}`;
    holder.append(error);

    /* Per-parameter reset to the factory value. preventDefault matters: the
     * holder is a <label>, and without it a click on the button would ALSO
     * activate the labelled control -- toggling a checkbox right back. */
    const reset = document.createElement('button');
    reset.type = 'button';
    reset.className = 'field__reset';
    reset.id = `r:${descriptor.path}`;
    reset.textContent = 'reset';
    reset.setAttribute('aria-label', `Reset ${descriptor.label} to factory default`);
    reset.addEventListener('click', (event) => {
      event.preventDefault();
      resetField(descriptor.path);
    });
    holder.append(reset);
    return holder;
  }

  /* An absent value (an EQ path the factory model has no section for) resets a
   * number to empty and a type select to the page-level "none", exactly what
   * render() shows for the same absent value. */
  function resetField(path) {
    const input = field(path);
    if (input.disabled) return;
    const value = readPath(FACTORY_DEFAULTS, path);
    if (input.dataset.kind === 'bool') input.checked = Boolean(value);
    else if (input.dataset.kind === 'eq-type') input.value = typeof value === 'string' ? value : 'none';
    else input.value = showNumber(value);
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

  function buildEqSlots(base, label) {
    const wrap = document.createElement('div');
    wrap.className = 'eq-slots';
    for (let slot = 0; slot < EQ_SLOTS; slot++) {
      const row = document.createElement('div');
      row.className = 'eq-row';
      const title = document.createElement('span');
      title.className = 'eq-row__title';
      title.textContent = `Section ${slot + 1}`;
      row.append(title, buildFieldGrid(eqFields(`${base}.eq.${slot}`, `${label} section ${slot + 1}`)));
      wrap.append(row);
    }
    return wrap;
  }

  function buildForm() {
    const features = $('features');
    features.append(
      buildFieldGrid(FEATURES.map(([key, label]) => ({ path: `features.${key}`, kind: 'bool', label }))),
    );

    const channels = $('input-channels');
    for (const [key, label] of CHANNELS) {
      const block = document.createElement('section');
      block.className = 'channel';
      const heading = document.createElement('h3');
      heading.className = 'channel__title';
      heading.textContent = `${label} (${key})`;
      block.append(
        heading,
        buildFieldGrid([
          {
            path: `input_channels.${key}.gain_db`,
            kind: 'number',
            label: `${label} trim`,
            caption: 'Trim (dB)',
            min: -60,
            max: 12,
            unit: ' dB',
          },
        ]),
        buildEqSlots(`input_channels.${key}`, label),
      );
      channels.append(block);
    }

    $('output-eq').append(buildEqSlots('output', 'Output'));
    $('matrix').append(buildFieldGrid(MATRIX_FIELDS));
    $('limiter').append(buildFieldGrid(LIMITER_FIELDS));

    for (const [key, label] of CHANNELS) meterRow(`meter-in-${key}`, `${label} (${key})`);
    meterRow('meter-out-l', 'Output left');
    meterRow('meter-out-r', 'Output right');

    for (const input of document.querySelectorAll('[data-field]')) {
      input.addEventListener('input', () => onFieldInput(input));
      input.addEventListener('change', () => onFieldInput(input));
    }
  }

  function meterRow(id, label) {
    const row = document.createElement('tr');
    const name = document.createElement('th');
    name.scope = 'row';
    name.textContent = label;
    const peak = document.createElement('td');
    peak.id = `${id}-peak`;
    peak.textContent = '--';
    const rms = document.createElement('td');
    rms.id = `${id}-rms`;
    rms.textContent = '--';
    row.append(name, peak, rms);
    $('meter-rows').append(row);
  }

  /* ---- model <-> DOM ----------------------------------------------------- */

  const readPath = (root, path) =>
    path.split('.').reduce((node, key) => (node === null || node === undefined ? undefined : node[key]), root);

  const showNumber = (value) => (Number.isFinite(value) ? String(value) : '');

  function render(model) {
    for (const [key] of FEATURES) field(`features.${key}`).checked = Boolean(readPath(model, `features.${key}`));

    for (const [key] of CHANNELS) {
      field(`input_channels.${key}.gain_db`).value = showNumber(readPath(model, `input_channels.${key}.gain_db`));
      renderEqSlots(`input_channels.${key}`, readPath(model, `input_channels.${key}.eq`));
    }
    renderEqSlots('output', model.output_eq);

    for (const descriptor of [...MATRIX_FIELDS, ...LIMITER_FIELDS]) {
      const input = field(descriptor.path);
      const value = readPath(model, descriptor.path);
      if (descriptor.kind === 'bool') input.checked = Boolean(value);
      else input.value = showNumber(value);
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
      field(`${path}.frequency_hz`).value = showNumber(section ? section.frequency_hz : undefined);
      field(`${path}.q`).value = showNumber(section ? section.q : undefined);
      field(`${path}.gain_db`).value = showNumber(section ? section.gain_db : undefined);
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
      field(`${path}.frequency_hz`).value = String(EQ_SEED.frequency_hz);
    }
    if (field(`${path}.q`).value.trim() === '') field(`${path}.q`).value = String(EQ_SEED.q);
    if (type === 'highpass') field(`${path}.gain_db`).value = String(EQ_SEED.gain_db);
    else if (field(`${path}.gain_db`).value.trim() === '') field(`${path}.gain_db`).value = String(EQ_SEED.gain_db);
  }

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
      headroom.value = String(denominator <= 1 ? 1 : 1 / denominator);
    }
    const bytes = JSON.stringify(collectModel()).length;
    $('model-bytes').textContent = `${bytes} / ${MODEL_MAX_BYTES}`;
    syncResetButtons();
  }

  function numberOf(path, fallback) {
    const raw = field(path).value.trim();
    const value = Number(raw);
    return raw === '' || !Number.isFinite(value) ? fallback : value;
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
    return message;
  }

  function validateAll() {
    const problems = [];
    for (const input of document.querySelectorAll('[data-field]')) {
      const message = validateField(input);
      if (message !== '') problems.push(message);
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
    if (state.applyState === 'applied' || state.applyState === 'error') setApplyState('edited');
  }

  /* ---- device I/O --------------------------------------------------------
   * request() never throws. Every caller below is fire-and-forget (a click
   * handler or the meter ticker) with nowhere to route a rejection, so a
   * failure comes back as data and the caller's own cleanup/render always
   * runs. guard() is the second belt: a defect anywhere in a handler surfaces
   * in the UI instead of becoming a silent unhandled rejection. */

  async function request(url, options) {
    try {
      const response = await fetch(url, options);
      let body = null;
      try {
        body = await response.json();
      } catch (parseError) {
        body = null;
      }
      return { ok: response.ok, status: response.status, body };
    } catch (error) {
      return { ok: false, status: 0, body: null, transport: error && error.message ? error.message : String(error) };
    }
  }

  async function guard(run) {
    try {
      await run();
    } catch (error) {
      showError(`Editor error: ${error && error.message ? error.message : String(error)}`);
    }
  }

  const describe = (result) =>
    result.transport
      ? `network error: ${result.transport}`
      : `HTTP ${result.status}${result.body && result.body.error ? ` (${result.body.error})` : ''}`;

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

  function adoptModel(model) {
    state.model = model;
    state.baseGeneration = Number.isFinite(model.config_generation) ? model.config_generation : 0;
    $('source').textContent = model.source || 'unknown';
    $('generation').textContent = String(state.baseGeneration);
    $('sample-rate').textContent = Number.isFinite(model.sample_rate_hz) ? `${model.sample_rate_hz} Hz` : '--';
    render(model);
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
      showError(`${problems.length} field${problems.length === 1 ? '' : 's'} out of range: ${problems[0]}`);
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
      adoptModel(result.body);
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

  /* Like request(), but body-agnostic: the directory index is HTML while every
   * refusal is the JSON error shape, so callers get the raw text plus the
   * error token when one parses out. Never throws. */
  async function requestRaw(url, options) {
    try {
      const response = await fetch(url, options);
      const text = await response.text();
      let token = '';
      try {
        const parsed = JSON.parse(text);
        if (parsed && typeof parsed.error === 'string') token = parsed.error;
      } catch (parseError) {
        /* HTML or other non-JSON body: no token. */
      }
      return { ok: response.ok, status: response.status, text, token };
    } catch (error) {
      return {
        ok: false,
        status: 0,
        text: '',
        token: '',
        transport: error && error.message ? error.message : String(error),
      };
    }
  }

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
      load.addEventListener('click', () => void guard(() => loadPreset(name)));
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.className = 'secondary';
      remove.textContent = 'Delete';
      remove.addEventListener('click', () => void guard(() => deletePreset(name)));
      row.append(title, load, remove);
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
      renderPresetList();
      setPresetState('idle');
      setPresetStatus('No presets on the card yet.');
      return;
    }
    if (!result.ok) {
      setPresetState('error');
      showPresetError(`Could not list ${PRESET_DIR_URL} -- ${describeRaw(result)}.`);
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
    renderPresetList();
    setPresetState('idle');
    const footer = result.text.match(/truncated: (yes|no)\./);
    if (footer && footer[1] === 'yes') {
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
    return describeRaw(result);
  }

  const describeRaw = (result) =>
    result.transport
      ? `network error: ${result.transport}`
      : `HTTP ${result.status}${result.token ? ` (${result.token})` : ''}`;

  async function savePreset(name, groups) {
    const token = $('token').value.trim();
    if (token === '') {
      showPresetError('SD auth token required to save a preset.');
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
    const result = await requestRaw(presetUrl(name), { headers: { Accept: 'application/json' } });
    if (!result.ok) {
      setPresetState('error');
      showPresetError(`Could not read ${name}.json -- ${describeRaw(result)}.`);
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
      showPresetError(`Could not delete ${name}.json -- ${describeRaw(result)}.`);
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

  function setPresetState(next) {
    $('preset-state').textContent = next;
    $('preset-state').dataset.state = next === 'truncated' ? 'error' : next;
  }

  const setPresetStatus = (text) => {
    $('preset-status').textContent = text;
  };

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
    drawWaveform([]);
  }

  function renderMetersAbsent(result) {
    $('meter-state').textContent = 'unavailable';
    blankMeterRows();
    $('meter-summary').textContent = `Meters unavailable -- ${describe(result)}. Retrying every ${METER_RETRY_MS / 1000} s.`;
  }

  /* sequence 0 is the snapshot's own documented "never published" sentinel, so
   * it renders as no data rather than as a genuine zero-level signal. */
  function renderMetersIdle() {
    $('meter-state').textContent = 'no data';
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

  const formatDb = (value) => {
    if (value === null || value === undefined || Number.isNaN(value)) return '--';
    if (!Number.isFinite(value) || value <= METER_FLOOR_DBFS) return '-inf';
    return value.toFixed(1);
  };

  function setMeterRow(id, peak, rms) {
    $(`${id}-peak`).textContent = formatDb(peak);
    $(`${id}-rms`).textContent = formatDb(rms);
  }

  function renderMeters(meters) {
    $('meter-state').textContent = 'live';
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
      ? `${meters.estimated_latency_us} us`
      : '--';
    $('meter-cycles').textContent =
      Number.isFinite(meters.dsp_cycles_last) || Number.isFinite(meters.dsp_cycles_max)
        ? `${meters.dsp_cycles_last ?? '--'} / ${meters.dsp_cycles_max ?? '--'}`
        : '--';
    $('meter-summary').textContent = `Limiter gain reduction ${formatDb(meters.limiter_gain_reduction_db)} dB at ${
      Number.isFinite(meters.sample_rate_hz) ? meters.sample_rate_hz : '--'
    } Hz.`;
    drawWaveform(meters.waveform);
  }

  /* The waveform is a bounded per-bucket peak-magnitude envelope carried as
   * per-mille integers 0..1000 of full scale, so it renders mirrored about the
   * centre line. The array's own length is the count; never more than the
   * contract's 128 points. */
  function normaliseWaveform(points) {
    if (!Array.isArray(points)) return [];
    return points
      .slice(0, WAVEFORM_MAX_POINTS)
      .map(Number)
      .filter(Number.isFinite)
      .map((value) => Math.max(0, Math.min(1, value / WAVEFORM_FULL_SCALE)));
  }

  function drawWaveform(points) {
    const canvas = $('waveform');
    const context = canvas.getContext('2d');
    if (!context) return;
    const { width, height } = canvas;
    const middle = height / 2;
    context.clearRect(0, 0, width, height);
    context.strokeStyle = '#2a3542';
    context.beginPath();
    context.moveTo(0, middle);
    context.lineTo(width, middle);
    context.stroke();

    const values = normaliseWaveform(points);
    if (values.length === 0) return;
    const xOf = (index) => (values.length < 2 ? 0 : (index * width) / (values.length - 1));
    context.strokeStyle = '#74d7ff';
    context.fillStyle = 'rgba(116,215,255,0.22)';
    context.beginPath();
    values.forEach((value, index) => {
      const y = middle - value * middle;
      if (index === 0) context.moveTo(xOf(index), y);
      else context.lineTo(xOf(index), y);
    });
    for (let index = values.length - 1; index >= 0; index--) {
      context.lineTo(xOf(index), middle + values[index] * middle);
    }
    context.closePath();
    context.fill();
    context.stroke();
  }

  /* ---- page state -------------------------------------------------------- */

  function setApplyState(next) {
    state.applyState = next;
    $('apply-state').textContent = next;
    $('apply-state').dataset.state = next;
  }

  const setStatus = (text) => {
    $('status').textContent = text;
  };

  function showError(text) {
    $('errors').textContent = text;
    $('errors').hidden = false;
  }

  function clearError() {
    $('errors').textContent = '';
    $('errors').hidden = true;
  }

  function setStale(stale) {
    $('stale').hidden = !stale;
  }

  /* ---- boot -------------------------------------------------------------- */

  /* localStorage can throw (storage disabled, private mode); the token box
   * degrades to per-session entry rather than taking the page down. */
  function readStoredToken() {
    try {
      return localStorage.getItem(TOKEN_STORAGE_KEY) || '';
    } catch (storageError) {
      return '';
    }
  }

  function storeToken(value) {
    try {
      if (value === '') localStorage.removeItem(TOKEN_STORAGE_KEY);
      else localStorage.setItem(TOKEN_STORAGE_KEY, value);
    } catch (storageError) {
      /* nothing to do: the field still holds the value for this session */
    }
  }

  buildForm();
  $('apply').onclick = () => void guard(applyConfig);
  $('reload').onclick = () => void guard(loadConfig);
  $('stale-reload').onclick = () => void guard(loadConfig);
  $('token').value = readStoredToken();
  $('token').addEventListener('input', () => storeToken($('token').value.trim()));
  $('preset-save').onclick = () =>
    void guard(async () => {
      clearPresetError();
      /* A preset is a model snapshot; never snapshot a form that Apply itself
       * would refuse. */
      const problems = validateAll();
      if (problems.length > 0) {
        showPresetError(`Fix the form first -- ${problems.length} field${problems.length === 1 ? '' : 's'} out of range: ${problems[0]}`);
        return;
      }
      await savePreset($('preset-name').value.trim(), collectPresetGroups());
    });
  $('preset-upload').onclick = () => void guard(uploadPreset);
  $('preset-refresh').onclick = () => void guard(refreshPresets);
  setApplyState('loading');
  void guard(loadConfig);
  void guard(refreshPresets);
  setInterval(() => void guard(pollMeters), METER_POLL_MS);
})();
