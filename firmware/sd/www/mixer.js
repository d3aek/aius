/* Aius SD mixer skin (mixer.html).
 *
 * OWNS: the views mixer.html draws where index.html draws something else --
 * the matrix faders, the equation readout, the centre-weight dial and the
 * worst-case verdict, the preset chips, the layout-aware chip, the live
 * signal-flow figure, the EQ channel tabs (the editor's channel table,
 * regrouped one tab per curve with bonded stereo pairs), and the dialog the
 * token field lives in. Every one is a VIEW over state dsp-editor.js already
 * owns: a fader writes the editor's own form field and dispatches 'input',
 * exactly as the routing canvas does, so validation, the dirty pill, presets
 * and Apply never learn that a fader moved.
 *
 * DOES NOT OWN: the model, the network or the meter ballistics. It makes no
 * request of its own -- the device serves one HTTP client at a time and the
 * editor's pollers already carry everything shown here -- so it reads the
 * editor's DOM instead: the form fields (f:<path>), the bar needles
 * (bar-in-*, bar-out-*), the flow ends and the preset picker.
 *
 * It also decorates every number input with a touch stepper (minus and plus
 * buttons that hold-repeat) and builds the matrix strip of crossover,
 * headroom and surround delay; both write only through the editor's fields.
 *
 * Plain classic script, no imports, for the same reason as dsp-editor.js: the
 * card cannot reach a host application's module mounts.
 */
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const field = (path) => $(`f:${path}`);

  /* The editor builds the form synchronously; without it there is nothing to
   * be a view of. */
  if (!field('matrix.center_gain_linear')) return;

  const SVG_NS = 'http://www.w3.org/2000/svg';
  const FADER_STEPS = 1000;
  /* Gain faders are dB-tapered over the linear field: the bottom stop is OFF
   * (0, which the field allows), then -40 dB up to the field's 4.0 maximum. */
  const GAIN_MIN_DB = -40;
  const GAIN_MAX_DB = 20 * Math.log10(4);
  const RATIO_SPAN_DB = 12; /* the dial's half-width */
  const CLIP_EPSILON = 0.0005; /* a sum that rounds to unity is unity */
  const FORM_CHECK_MS = 200; /* how soon a device read shows on the faders */
  /* Every field the views here read: their values, enablement and validity
   * are the whole input to syncAll(). */
  const WATCHED_PATHS = [
    'matrix.center_gain_linear',
    'matrix.surround_gain_linear',
    'matrix.lfe_gain_linear',
    'matrix.lfe_crossover_hz',
    'matrix.headroom_linear',
    'matrix.surround_delay_ms',
    'matrix.clipfree',
    'matrix.layout_aware',
    'features.input_eq',
    'features.matrix',
    'features.output_eq',
    'features.limiter',
    'limiter.enabled',
  ];

  const clamp = (value, low, high) => Math.min(high, Math.max(low, value));
  const round2 = (value) => Math.round(value * 100) / 100;
  const signedDb = (db) => `${db >= 0 ? '+' : ''}${db.toFixed(1)} dB`;

  /* ---- fader scales ------------------------------------------------------
   * Each maps a 0..FADER_STEPS slider position to the field's value and back.
   * toValue() only ever returns a value the field accepts, so a fader cannot
   * produce an out-of-range message on its own. */

  const GAIN_SCALE = {
    toValue(position) {
      if (position <= 0) return 0;
      const db = GAIN_MIN_DB + (position / FADER_STEPS) * (GAIN_MAX_DB - GAIN_MIN_DB);
      return clamp(round2(10 ** (db / 20)), 0.01, 4);
    },
    toPosition(value) {
      if (!(value > 0)) return 0;
      const db = 20 * Math.log10(value);
      return clamp(Math.round(((db - GAIN_MIN_DB) / (GAIN_MAX_DB - GAIN_MIN_DB)) * FADER_STEPS), 1, FADER_STEPS);
    },
    format: (value) => ({ value: value.toFixed(2), sub: value > 0 ? signedDb(20 * Math.log10(value)) : 'off' }),
  };

  /* The four channel weights of the sum, in the order the equation reads. F has
   * no field -- the firmware adds FL and FR at unity -- so its fader is a fixed,
   * disabled reference. Crossover, surround delay and headroom are set-and-
   * forget values, so they live in the exact values, not on a fader. */
  const FADERS = [
    { key: 'c', name: 'C', hint: 'dialogue', label: 'Centre gain', path: 'matrix.center_gain_linear', scale: GAIN_SCALE },
    { key: 'f', name: 'F', hint: 'fronts, fixed', label: 'Front gain (fixed at 1.0)', fixed: 1, scale: GAIN_SCALE },
    { key: 's', name: 'S', hint: 'surrounds', label: 'Surround gain', path: 'matrix.surround_gain_linear', scale: GAIN_SCALE },
    { key: 'lfe', name: 'LFE', hint: 'sub', label: 'LFE gain', path: 'matrix.lfe_gain_linear', scale: GAIN_SCALE },
  ];

  /* What a field holds right now, or NaN while it is blank or mid-typing. */
  function numberOf(path) {
    const input = field(path);
    const raw = input ? input.value.trim() : '';
    return raw === '' ? NaN : Number(raw);
  }

  const checked = (path) => Boolean(field(path) && field(path).checked);

  /* ---- faders ------------------------------------------------------------ */

  const faders = new Map();

  function buildFader(descriptor) {
    const holder = document.createElement('label');
    holder.className = `fader fader--${descriptor.key}`;
    const name = document.createElement('span');
    name.className = 'fader__name';
    name.textContent = descriptor.name;
    const range = document.createElement('input');
    range.type = 'range';
    range.min = '0';
    range.max = String(FADER_STEPS);
    range.step = '1';
    range.id = `fader:${descriptor.key}`;
    range.setAttribute('aria-label', descriptor.label);
    const value = document.createElement('output');
    value.className = 'fader__val';
    value.htmlFor = range.id;
    const sub = document.createElement('span');
    sub.className = 'fader__sub';
    const hint = document.createElement('span');
    hint.className = 'fader__hint';
    hint.textContent = descriptor.hint;
    holder.append(name, range, value, sub, hint);

    if (descriptor.fixed !== undefined) {
      range.disabled = true;
      range.value = String(descriptor.scale.toPosition(descriptor.fixed));
      const shown = descriptor.scale.format(descriptor.fixed);
      value.textContent = shown.value;
      sub.textContent = shown.sub;
      holder.dataset.fixed = 'true';
    } else {
      range.addEventListener('input', () => writeFromFader(descriptor, range));
    }
    $('mixer-faders').append(holder);
    faders.set(descriptor.key, { descriptor, holder, range, value, sub });
  }

  /* The single write path, the same shape as the editor's writeField(): set the
   * field, drop its exact-value memory, dispatch 'input'. A disabled field (the
   * headroom under the clip-free policy) is left alone, as the editor does. */
  function writeFromFader(descriptor, range) {
    const input = field(descriptor.path);
    if (!input || input.disabled) return;
    const next = String(descriptor.scale.toValue(Number(range.value)));
    range.dataset.written = next;
    input.value = next;
    input.dataset.exact = '';
    input.dispatchEvent(new Event('input'));
  }

  function syncFader(entry) {
    const { descriptor, holder, range, value, sub } = entry;
    if (descriptor.fixed !== undefined) return;
    const input = field(descriptor.path);
    const raw = input.value.trim();
    const number = raw === '' ? NaN : Number(raw);
    const valid = Number.isFinite(number);
    range.disabled = input.disabled;
    holder.dataset.disabled = String(input.disabled);
    holder.dataset.invalid = String(!valid || Boolean(input.closest('.field--invalid')));
    /* A fader that wrote this very value already sits where it was dragged; a
     * re-quantised position would make the thumb twitch under the pointer. */
    if (valid && range.dataset.written !== raw) {
      range.value = String(descriptor.scale.toPosition(number));
      range.dataset.written = '';
    }
    const shown = valid ? descriptor.scale.format(number) : { value: '--', sub: '' };
    value.textContent = shown.value;
    sub.textContent = shown.sub;
    range.setAttribute('aria-valuetext', valid ? `${shown.value} ${shown.sub}` : 'no value');
  }

  /* ---- equation, dial and verdict ----------------------------------------- */

  function term(weight, channel, voice) {
    const fragment = document.createDocumentFragment();
    const bold = document.createElement('b');
    if (voice) bold.className = 'eqn__v';
    bold.textContent = Number.isFinite(weight) ? weight.toFixed(2) : '--';
    fragment.append(bold, `·${channel}`);
    return fragment;
  }

  /* The firmware's downmix sum (aius_dsp.c), one line per output, then the
   * headroom scale: the same terms the routing canvas draws as paths. */
  function renderEquation(values) {
    const eqn = $('mixer-eqn');
    eqn.textContent = '';
    for (const [side, front, surround] of [
      ['L', 'FL', 'SL'],
      ['R', 'FR', 'SR'],
    ]) {
      const lhs = document.createElement('span');
      lhs.className = 'eqn__lhs';
      lhs.textContent = side;
      eqn.append(lhs, ' = ( ', term(1, front), ' + ', term(values.c, 'C', true), ' + ', term(values.s, surround));
      eqn.append(' + ', term(values.lfe, 'LFE'), ' ) × ', term(values.hr, 'HR'), '\n');
    }
    const comment = document.createElement('span');
    comment.className = 'eqn__c';
    const notes = [`# LFE low-passed at ${Number.isFinite(values.xo) ? values.xo : '--'} Hz`];
    if (values.dly > 0) notes.push(`surrounds delayed ${values.dly.toFixed(1)} ms`);
    if (values.clipfree) notes.push('HR resolved clip-free');
    if (!checked('features.matrix')) notes.push('matrix stage bypassed');
    comment.textContent = notes.join(' · ');
    eqn.append(comment);
  }

  /* Centre weight against the power sum of every other weight feeding the same
   * side: FL at unity, the surround and the LFE. Weights only -- what the
   * programme puts on each channel is the meters' business, not this dial's. */
  function renderDial(values) {
    const rest = Math.sqrt(1 + values.s ** 2 + values.lfe ** 2);
    const usable = Number.isFinite(values.c) && Number.isFinite(rest);
    const db = usable && values.c > 0 ? 20 * Math.log10(values.c / rest) : -Infinity;
    $('mixer-ratio').textContent = !usable ? '--' : Number.isFinite(db) ? signedDb(db) : 'centre off';
    const fraction = usable ? (clamp(db, -RATIO_SPAN_DB, RATIO_SPAN_DB) + RATIO_SPAN_DB) / (2 * RATIO_SPAN_DB) : 0.5;
    $('mixer-ratio-fill').style.width = `${(fraction * 100).toFixed(1)}%`;
  }

  /* The worst case the matrix can produce: every input in phase at full scale,
   * summed and scaled by headroom. A fact about the weights, stated as one. */
  function renderVerdict(values) {
    const verdict = $('mixer-verdict');
    const sum = values.hr * (1 + values.c + values.s + Math.abs(values.lfe));
    if (!Number.isFinite(sum)) {
      verdict.textContent = 'A matrix value is blank or out of range.';
      verdict.dataset.tone = 'bad';
      return;
    }
    if (sum <= 1 + CLIP_EPSILON) {
      verdict.textContent = `Clip-safe: a worst-case in-phase full-scale sum peaks at ${(20 * Math.log10(sum)).toFixed(1)} dBFS.`;
      verdict.dataset.tone = 'ok';
      return;
    }
    const over = signedDb(20 * Math.log10(sum));
    const limiting = checked('features.limiter') && checked('limiter.enabled');
    verdict.textContent = limiting
      ? `A worst-case in-phase sum overshoots by ${over}; the limiter catches it.`
      : `A worst-case in-phase sum overshoots by ${over} and the limiter is off: hot programme can clip.`;
    verdict.dataset.tone = limiting ? 'warn' : 'bad';
  }

  function readValues() {
    return {
      c: numberOf('matrix.center_gain_linear'),
      s: numberOf('matrix.surround_gain_linear'),
      lfe: numberOf('matrix.lfe_gain_linear'),
      xo: numberOf('matrix.lfe_crossover_hz'),
      hr: numberOf('matrix.headroom_linear'),
      dly: numberOf('matrix.surround_delay_ms'),
      clipfree: checked('matrix.clipfree'),
    };
  }

  /* ---- chips -------------------------------------------------------------- */

  function syncLayoutAwareChip() {
    const chip = $('mixer-layout-aware');
    const on = checked('matrix.layout_aware');
    chip.setAttribute('aria-pressed', String(on));
    chip.classList.toggle('chip--on', on);
  }

  $('mixer-layout-aware').addEventListener('click', () => {
    const input = field('matrix.layout_aware');
    if (input.disabled) return;
    input.checked = !input.checked;
    input.dispatchEvent(new Event('input'));
  });

  /* One chip per saved preset, read off the editor's picker. A chip loads
   * through the editor's own Load (its discard confirm included), and reads as
   * pressed while the form still equals that preset unmodified -- the editor's
   * origin line is where that fact already lives. */
  function renderPresetChips() {
    const mount = $('mixer-presets');
    mount.textContent = '';
    const names = Array.from($('preset-select').options, (option) => option.value).filter((name) => name !== '');
    if (names.length === 0) {
      const none = document.createElement('span');
      none.className = 'mixer__none';
      none.textContent = 'no presets on the card';
      mount.append(none);
      return;
    }
    const origin = $('preset-origin').textContent;
    for (const name of names) {
      const chip = document.createElement('button');
      chip.type = 'button';
      chip.className = 'chip';
      chip.dataset.preset = name;
      chip.textContent = name;
      chip.setAttribute('aria-pressed', String(origin === `preset: ${name}`));
      chip.disabled = $('preset-load').disabled;
      chip.addEventListener('click', () => {
        $('preset-select').value = name;
        $('preset-load').click();
      });
      mount.append(chip);
    }
  }

  function syncPresetChipState() {
    const origin = $('preset-origin').textContent;
    for (const chip of $('mixer-presets').querySelectorAll('[data-preset]')) {
      chip.setAttribute('aria-pressed', String(origin === `preset: ${chip.dataset.preset}`));
      chip.disabled = $('preset-load').disabled;
    }
  }

  /* ---- steppers ---------------------------------------------------------------
   * Every number input on the page gets a minus and a plus button beside it.
   * The input itself stays where the editor put it (its validator reads
   * input.parentElement for the error line), so the buttons are siblings after
   * it and the holder becomes a grid. A press steps by the field's own `step`
   * and clamps to its `min`/`max`; a hold repeats. Each step writes the value,
   * drops the exact-value memory and dispatches 'input', as a fader does; the
   * release of a press that stepped dispatches one 'change'. */

  const HOLD_DELAY_MS = 400;
  const HOLD_REPEAT_MS = 80;
  const CLICK_ECHO_MS = 400; /* a pointer press is followed by a click it must not repeat */
  const CELL_LABELS = { type: 'Type', frequency_hz: 'Freq Hz', q: 'Q', gain_db: 'Gain dB' };

  const decimalsOf = (number) => (String(number).split('.')[1] || '').length;

  /* Steps `input` one `step` in `direction`; true when the value moved. */
  function stepValue(input, direction) {
    if (input.disabled || input.readOnly) return false;
    const step = Number(input.step) > 0 ? Number(input.step) : 1;
    const low = input.min === '' ? -Infinity : Number(input.min);
    const high = input.max === '' ? Infinity : Number(input.max);
    const raw = input.value.trim();
    const current = raw === '' ? NaN : Number(raw);
    const base = Number.isFinite(current) ? current : clamp(0, low, high);
    const places = Math.max(decimalsOf(step), decimalsOf(base));
    const next = clamp(Number((base + direction * step).toFixed(places)), low, high);
    if (next === current) return false;
    input.value = String(next);
    if (input.dataset.field) input.dataset.exact = '';
    input.dispatchEvent(new Event('input'));
    return true;
  }

  function stepButton(input, direction) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = `stepper__btn stepper__btn--${direction < 0 ? 'minus' : 'plus'}`;
    button.tabIndex = -1;
    button.textContent = direction < 0 ? '−' : '+';
    const subject = input.dataset.label || input.getAttribute('aria-label') || 'value';
    button.setAttribute('aria-label', `${direction < 0 ? 'Decrease' : 'Increase'} ${subject}`);

    let delayTimer = 0;
    let repeatTimer = 0;
    let stepped = false;
    let pointerSeq = false;
    const step = () => {
      if (stepValue(input, direction)) stepped = true;
    };
    const stop = () => {
      clearTimeout(delayTimer);
      clearInterval(repeatTimer);
      delayTimer = 0;
      repeatTimer = 0;
      if (stepped) input.dispatchEvent(new Event('change'));
      stepped = false;
      setTimeout(() => {
        pointerSeq = false;
      }, CLICK_ECHO_MS);
    };
    button.addEventListener('pointerdown', (event) => {
      if (event.button > 0 || button.disabled || delayTimer || repeatTimer) return;
      event.preventDefault();
      pointerSeq = true;
      if (typeof button.setPointerCapture === 'function' && event.pointerId !== undefined) {
        try {
          button.setPointerCapture(event.pointerId);
        } catch {
          /* a synthetic pointer has nothing to capture */
        }
      }
      step();
      delayTimer = setTimeout(() => {
        repeatTimer = setInterval(step, HOLD_REPEAT_MS);
      }, HOLD_DELAY_MS);
    });
    for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) button.addEventListener(type, stop);
    /* A click with no pointer press before it is the keyboard or assistive tech. */
    button.addEventListener('click', () => {
      if (pointerSeq) return;
      step();
      stop();
    });
    button.addEventListener('contextmenu', (event) => event.preventDefault());
    return button;
  }

  const steppers = [];

  /* The one place a number input becomes a stepper. */
  function stepper(input) {
    const holder = input.parentElement;
    if (!holder || input.dataset.stepper) return;
    input.dataset.stepper = 'true';
    input.inputMode = 'decimal';
    input.classList.add('stepper__input');
    holder.classList.add('stepper');
    input.after(stepButton(input, -1), stepButton(input, 1));
    steppers.push(input);
  }

  /* A table cell has no caption of its own (the column head carries it), so
   * where the table reflows into labelled bands the cell names itself. */
  function labelEqCells() {
    for (const input of document.querySelectorAll('.eq-table .field--cell [data-field]')) {
      input.parentElement.dataset.col = CELL_LABELS[input.dataset.field.split('.').pop()] || '';
    }
  }

  /* ---- matrix strip ------------------------------------------------------------
   * Crossover, headroom and surround delay as steppers under the faders. Each
   * is a view of the editor's exact-values field: its own input carries the
   * same bounds, forwards every 'input'/'change' to that field, and follows the
   * field back (value, disabled under clip-free, out-of-range). */

  const STRIP = [
    { key: 'xo', path: 'matrix.lfe_crossover_hz', label: 'LFE crossover', unit: 'Hz' },
    { key: 'hr', path: 'matrix.headroom_linear', label: 'Headroom', unit: '×' },
    { key: 'dly', path: 'matrix.surround_delay_ms', label: 'Surround delay', unit: 'ms' },
  ];
  const stripItems = [];

  function buildStrip() {
    const mount = $('mixer-strip');
    for (const def of STRIP) {
      const source = field(def.path);
      if (!source) continue;
      const item = document.createElement('div');
      item.className = 'strip__item';
      const caption = document.createElement('label');
      caption.className = 'strip__label';
      caption.textContent = `${def.label} (${def.unit})`;
      const holder = document.createElement('div');
      holder.className = 'strip__field';
      const input = document.createElement('input');
      input.type = 'number';
      input.id = `strip:${def.key}`;
      input.min = source.min;
      input.max = source.max;
      input.step = source.step;
      input.setAttribute('aria-label', `${def.label} (${def.unit})`);
      input.dataset.label = def.label;
      caption.htmlFor = input.id;
      const note = document.createElement('span');
      note.className = 'strip__note';
      holder.append(input);
      item.append(caption, holder, note);
      mount.append(item);
      const forward = (type) => {
        if (source.disabled) return;
        source.value = input.value;
        source.dataset.exact = '';
        source.dispatchEvent(new Event(type));
      };
      input.addEventListener('input', () => forward('input'));
      input.addEventListener('change', () => forward('change'));
      stripItems.push({ def, source, input, holder, note });
    }
  }

  function syncStrip() {
    for (const { def, source, input, holder, note } of stripItems) {
      if (document.activeElement !== input && input.value !== source.value) input.value = source.value;
      input.disabled = source.disabled;
      holder.dataset.invalid = String(Boolean(source.closest('.field--invalid')));
      note.textContent = source.disabled && def.key === 'hr' ? 'resolved by clip-free' : '';
    }
  }

  /* A stepper button is dead exactly when its input is, which changes with no
   * event (the clip-free policy, a stage switch). */
  function syncSteppers() {
    for (const input of steppers) {
      for (const button of input.parentElement.querySelectorAll('.stepper__btn')) button.disabled = input.disabled;
    }
  }

  /* ---- signal-flow figure -------------------------------------------------- */

  /* Box rows on the left, the core in the middle, L and R on the right; the
   * wire's `weight` names the matrix term that scales it. */
  const FLOW_INPUTS = [
    { label: 'FL', bar: 'bar-in-fl', y: 30, entry: 140, weight: 'f' },
    { label: 'C', bar: 'bar-in-fc', y: 86, entry: 162, weight: 'c', voice: true },
    { label: 'FR', bar: 'bar-in-fr', y: 142, entry: 184, weight: 'f' },
    { label: 'SL', bar: 'bar-in-sl', y: 214, entry: 216, weight: 's' },
    { label: 'SR', bar: 'bar-in-sr', y: 270, entry: 238, weight: 's' },
    { label: 'LFE', bar: 'bar-in-lfe', y: 326, entry: 260, weight: 'lfe' },
  ];
  const FLOW_OUTPUTS = [
    { label: 'L', bar: 'bar-out-l', y: 120, exit: 160 },
    { label: 'R', bar: 'bar-out-r', y: 240, exit: 240 },
  ];
  const FLOW_STAGES = [
    { label: 'IN EQ', on: () => checked('features.input_eq'), x: 0, y: 0 },
    { label: 'MATRIX', on: () => checked('features.matrix'), x: 70, y: 0 },
    { label: 'OUT EQ', on: () => checked('features.output_eq'), x: 0, y: 32 },
    { label: 'LIMIT', on: () => checked('features.limiter') && checked('limiter.enabled'), x: 70, y: 32 },
  ];
  const INPUT_BOX = { x: 10, w: 64, h: 34, meterInset: 6 };
  const OUTPUT_BOX = { x: 446, w: 64, h: 40, meterInset: 6 };
  const CORE_LEFT = 200;
  const CORE_RIGHT = 370;
  const WIRE_MIN_PX = 0.8;
  const WIRE_MAX_PX = 5;

  const svg = (tag, attributes) => {
    const node = document.createElementNS(SVG_NS, tag);
    for (const [name, value] of Object.entries(attributes || {})) node.setAttribute(name, String(value));
    return node;
  };

  const flowNodes = { wiresIn: [], wiresOut: [], meters: new Map(), stages: [] };

  function buildFlowBox(mount, box, row) {
    const group = svg('g', { transform: `translate(${box.x},${row.y})` });
    if (row.voice) group.classList.add('is-c');
    group.append(svg('rect', { class: 'flow__box', width: box.w, height: box.h, rx: 6 }));
    const text = svg('text', { x: box.w / 2, y: box.h / 2 + 1 });
    text.textContent = row.label;
    group.append(text);
    const meterWidth = box.w - 2 * box.meterInset;
    group.append(svg('rect', { class: 'flow__meter-track', x: box.meterInset, y: box.h - 8, width: meterWidth, height: 3, rx: 1.5 }));
    const meter = svg('rect', { class: 'flow__meter', x: box.meterInset, y: box.h - 8, width: 0, height: 3, rx: 1.5 });
    group.append(meter);
    flowNodes.meters.set(row.bar, { meter, width: meterWidth });
    mount.append(group);
  }

  function buildFlow() {
    for (const row of FLOW_INPUTS) {
      const startY = row.y + INPUT_BOX.h / 2;
      const startX = INPUT_BOX.x + INPUT_BOX.w;
      const path = svg('path', {
        d: `M${startX} ${startY} C ${startX + 66} ${startY}, ${CORE_LEFT - 60} ${row.entry}, ${CORE_LEFT} ${row.entry}`,
      });
      if (row.voice) path.classList.add('is-c');
      $('flow-wires-in').append(path);
      flowNodes.wiresIn.push({ path, row });
      buildFlowBox($('flow-inputs'), INPUT_BOX, row);
    }
    for (const row of FLOW_OUTPUTS) {
      const endY = row.y + OUTPUT_BOX.h / 2;
      const path = svg('path', {
        d: `M${CORE_RIGHT} ${row.exit} C ${CORE_RIGHT + 44} ${row.exit}, ${OUTPUT_BOX.x - 30} ${endY}, ${OUTPUT_BOX.x} ${endY}`,
      });
      $('flow-wires-out').append(path);
      flowNodes.wiresOut.push(path);
      buildFlowBox($('flow-outputs'), OUTPUT_BOX, row);
    }
    for (const stage of FLOW_STAGES) {
      const group = svg('g', { class: 'flow__stage', transform: `translate(${stage.x},${stage.y})` });
      group.append(svg('rect', { width: 64, height: 24, rx: 12 }));
      group.append(svg('circle', { class: 'flow__led', cx: 11, cy: 12, r: 3 }));
      const text = svg('text', { x: 38, y: 13 });
      text.textContent = stage.label;
      group.append(text);
      $('flow-stages').append(group);
      flowNodes.stages.push({ group, stage });
    }
  }

  /* A muted term is still a path, drawn thin and dashed, the way the routing
   * canvas draws one; width otherwise follows the weight, capped so a 4.0 gain
   * does not swallow the picture. */
  function styleWire(path, weight) {
    const usable = Number.isFinite(weight);
    const muted = !usable || weight <= 0;
    path.setAttribute('stroke-width', String(muted ? WIRE_MIN_PX : clamp(WIRE_MIN_PX + 2.2 * weight, WIRE_MIN_PX, WIRE_MAX_PX)));
    path.dataset.muted = String(muted);
  }

  function syncFlowWeights(values) {
    const weights = { f: 1, c: values.c, s: values.s, lfe: values.lfe };
    for (const { path, row } of flowNodes.wiresIn) styleWire(path, weights[row.weight]);
    for (const path of flowNodes.wiresOut) styleWire(path, 2 * values.hr);
    for (const { group, stage } of flowNodes.stages) group.dataset.on = String(stage.on());
  }

  /* The needles are the editor's, painted at 25 fps by its ballistics ticker;
   * this mirrors the painted width and zone, so the figure and the bars can
   * never disagree. */
  function syncFlowMeters() {
    for (const [barId, { meter, width }] of flowNodes.meters) {
      const fill = $(`${barId}-fill`);
      if (!fill) continue;
      const fraction = clamp((parseFloat(fill.style.width) || 0) / 100, 0, 1);
      meter.setAttribute('width', (fraction * width).toFixed(1));
      meter.dataset.zone = fill.dataset.zone || '';
    }
  }

  function syncFlowEnds() {
    const gate = $('tx-gate').dataset.state;
    const input = $('input-source').dataset.state;
    $('dashboard-input').querySelector('.flow').dataset.live = String(gate === 'open');
    $('hero-led').dataset.state = input || 'unknown';
  }

  /* ---- EQ channel tabs -------------------------------------------------------
   * The editor builds ONE grid for all six channels: eight header cells, then
   * per channel its name, its trim cell and two section rows of six cells
   * (section number, type, frequency, Q, gain, reset). The cells move into one
   * panel per channel intact -- ids, listeners and the cell an input reports
   * errors through all travel with them -- and the panels are grouped one tab
   * per curve on the plot, so FL+FR and SL+SR share a tab. The output EQ card
   * is the last tab as it stands. */

  const EQ_TABS = [
    { key: 'front', label: 'FL+FR', title: 'Front (fl+fr)', members: ['fl', 'fr'] },
    { key: 'centre', label: 'C', title: 'Front centre (fc)', members: ['fc'] },
    { key: 'lfe', label: 'LFE', title: 'LFE', members: ['lfe'] },
    { key: 'surround', label: 'SL+SR', title: 'Surround (sl+sr)', members: ['sl', 'sr'] },
    { key: 'output', label: 'OUT', title: 'Output', members: [] },
  ];
  const CHANNEL_ORDER = ['fl', 'fr', 'fc', 'lfe', 'sl', 'sr']; /* the editor's row order */
  const EQ_HEADER_CELLS = 8;
  const EQ_SECTION_HEADS = ['Sec', 'Type', 'Freq (Hz)', 'Q', 'Gain (dB)', ''];

  const tabs = new Map();
  let activeTab = 'front';

  function head(text) {
    const cell = document.createElement('span');
    cell.className = 'eq-table__h';
    cell.textContent = text;
    return cell;
  }

  function buildChannelPanel(channel, cells) {
    const [, trim, ...sections] = cells;
    const panel = document.createElement('div');
    panel.className = 'tabs__channel';
    panel.id = `eq-panel:${channel}`;
    const member = document.createElement('span');
    member.className = 'tabs__member';
    member.textContent = channel.toUpperCase();
    const trimRow = document.createElement('div');
    trimRow.className = 'tabs__trim';
    trimRow.append(head('Trim (dB)'), trim);
    const grid = document.createElement('div');
    grid.className = 'eq-table eq-table--output';
    grid.append(...EQ_SECTION_HEADS.map(head), ...sections);
    panel.append(member, trimRow, grid);
    return panel;
  }

  /* ---- bonded pairs ---------------------------------------------------------
   * A stereo pair is one thing on the plot (the editor writes both members of
   * every curve edit), and its tab follows suit: LINKED, it shows the left
   * member's controls and copies every edit to the right; UNLINKED, it shows
   * both. A pair whose members differ -- a device read, a preset, a hand-edited
   * config -- is shown unlinked rather than silently overwritten; linking it
   * again copies left into right, as the bond chip says. */

  const pairOf = (tab) => (tab.members.length === 2 ? tab.members : null);

  /* Each left-member input with its right-member partner, types first: a type
   * change re-seeds and re-enables its row's value fields, so the values are
   * copied after it lands. */
  function partnerFields(tab) {
    const [left, right] = pairOf(tab);
    const inputs = Array.from($(`eq-panel:${left}`).querySelectorAll('[data-field]'));
    inputs.sort((a, b) => Number(b.tagName === 'SELECT') - Number(a.tagName === 'SELECT'));
    return inputs.map((input) => [
      input,
      field(input.dataset.field.replace(`input_channels.${left}.`, `input_channels.${right}.`)),
    ]);
  }

  const pairDiffers = (tab) => partnerFields(tab).some(([from, to]) => to && from.value !== to.value);

  function copyLeftToRight(tab) {
    for (const [from, to] of partnerFields(tab)) {
      if (!to || from.value === to.value) continue;
      to.value = from.value;
      to.dataset.exact = from.dataset.exact || '';
      to.dispatchEvent(new Event(to.tagName === 'SELECT' ? 'change' : 'input'));
    }
  }

  function scheduleMirror(entry) {
    if (!entry.linked || entry.mirrorPending) return;
    entry.mirrorPending = true;
    setTimeout(() => {
      entry.mirrorPending = false;
      if (entry.linked) copyLeftToRight(entry.tab);
    }, 0);
  }

  function renderBond(entry) {
    if (!entry.bond) return;
    const [left, right] = pairOf(entry.tab);
    entry.panel.dataset.linked = String(entry.linked);
    $(`eq-panel:${right}`).hidden = entry.linked;
    entry.bond.setAttribute('aria-pressed', String(entry.linked));
    entry.bond.classList.toggle('chip--on', entry.linked);
    entry.note.textContent = entry.linked
      ? `one set of controls writes ${left.toUpperCase()} and ${right.toUpperCase()}`
      : pairDiffers(entry.tab)
        ? `${left.toUpperCase()} and ${right.toUpperCase()} differ; linking copies ${left.toUpperCase()} to ${right.toUpperCase()}`
        : 'edited separately';
  }

  function toggleBond(entry) {
    if (entry.linked) {
      entry.linked = false;
    } else {
      copyLeftToRight(entry.tab);
      entry.linked = true;
    }
    renderBond(entry);
  }

  /* A linked pair whose members came apart without an edit here was rewritten
   * from outside; show the truth instead of hiding the right member. */
  function syncBonds() {
    for (const entry of tabs.values()) {
      if (!entry.bond) continue;
      if (entry.linked && !entry.mirrorPending && pairDiffers(entry.tab)) entry.linked = false;
      renderBond(entry);
    }
  }

  function buildBond(entry) {
    const [left, right] = pairOf(entry.tab);
    const row = document.createElement('div');
    row.className = 'tabs__bond';
    const chip = document.createElement('button');
    chip.type = 'button';
    chip.className = 'chip chip--bond';
    chip.textContent = `link ${left.toUpperCase()}+${right.toUpperCase()}`;
    chip.addEventListener('click', () => toggleBond(entry));
    const note = document.createElement('span');
    note.className = 'tabs__bond-note';
    row.append(chip, note);
    entry.panel.prepend(row);
    entry.bond = chip;
    entry.note = note;
    /* Any edit inside the left member -- typed, chosen, or a reset button --
     * is mirrored once the editor's own handler has run. */
    const leftPanel = $(`eq-panel:${left}`);
    const onEdit = () => scheduleMirror(entry);
    leftPanel.addEventListener('input', onEdit, true);
    leftPanel.addEventListener('change', onEdit, true);
    leftPanel.addEventListener('click', onEdit);
  }

  function buildEqTabs() {
    const grid = $('input-channels').querySelector('.eq-table--channels');
    if (!grid) return;
    const groups = [];
    for (const cell of Array.from(grid.children).slice(EQ_HEADER_CELLS)) {
      if (cell.classList.contains('eq-table__channel')) groups.push([]);
      if (groups.length > 0) groups[groups.length - 1].push(cell);
    }
    const panels = new Map(CHANNEL_ORDER.slice(0, groups.length).map((channel, index) => [channel, buildChannelPanel(channel, groups[index])]));
    const wrap = grid.parentElement;
    wrap.textContent = '';

    for (const tab of EQ_TABS) {
      let panel = $('card-output-eq');
      if (tab.members.length > 0) {
        panel = document.createElement('div');
        panel.className = 'tabs__group';
        panel.id = `eq-group:${tab.key}`;
        for (const channel of tab.members) panel.append(panels.get(channel));
        wrap.append(panel);
      }
      panel.setAttribute('role', 'tabpanel');

      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'tabs__tab';
      button.id = `eq-tab:${tab.key}`;
      button.title = tab.title;
      button.setAttribute('role', 'tab');
      button.setAttribute('aria-controls', panel.id);
      const label = document.createElement('span');
      label.textContent = tab.label;
      const mark = document.createElement('i');
      mark.className = 'tabs__mark';
      mark.setAttribute('aria-hidden', 'true');
      button.append(label, mark);
      button.addEventListener('click', () => activateTab(tab.key, true));
      button.addEventListener('keydown', onTabKey);
      $('eq-tablist').append(button);

      const entry = { tab, button, panel, linked: true, mirrorPending: false, bond: null, note: null };
      tabs.set(tab.key, entry);
      if (pairOf(tab)) buildBond(entry);
    }
  }

  /* A tab IS a curve on the plot (same keys as the legend), so choosing either
   * selects the other. */
  function activateTab(key, selectCurve) {
    if (!tabs.has(key)) return;
    activeTab = key;
    for (const [tabKey, { button, panel }] of tabs) {
      const on = tabKey === key;
      button.setAttribute('aria-selected', String(on));
      button.tabIndex = on ? 0 : -1;
      if (tabKey !== 'output') panel.hidden = !on;
    }
    $('card-input-channels').hidden = key === 'output';
    $('card-output-eq').hidden = key !== 'output';
    if (key !== 'output') $('eq-channel-title').textContent = `Input EQ · ${tabs.get(key).tab.title}`;
    if (!selectCurve) return;
    const chip = $(`chip:${key}`);
    if (chip && chip.getAttribute('aria-pressed') !== 'true') chip.click();
  }

  function onTabKey(event) {
    const keys = EQ_TABS.map((tab) => tab.key);
    const index = keys.indexOf(activeTab);
    const next = {
      ArrowRight: keys[(index + 1) % keys.length],
      ArrowLeft: keys[(index - 1 + keys.length) % keys.length],
      Home: keys[0],
      End: keys[keys.length - 1],
    }[event.key];
    if (!next) return;
    event.preventDefault();
    activateTab(next, true);
    tabs.get(next).button.focus();
  }

  function followCurveSelection() {
    const pressed = $('curve-legend').querySelector('[aria-pressed="true"]');
    if (pressed && pressed.dataset.entity !== activeTab) activateTab(pressed.dataset.entity, false);
  }

  /* A tab says what is behind it: a dot when any section is present, red when
   * a field in it is out of range, so a closed tab never hides a problem. */
  function syncTabMarks() {
    for (const { button, panel } of tabs.values()) {
      const types = Array.from(panel.querySelectorAll('select'), (select) => select.value);
      button.dataset.eq = String(types.some((type) => type !== 'none' && type !== 'bypass'));
      button.dataset.invalid = String(panel.querySelector('.field--invalid') !== null);
    }
  }

  /* The error summary's links focus their field; one in a closed tab, behind a
   * linked pair, or in the folded exact values is shown first, ahead of the
   * editor's own handler. */
  function revealContainerOf(input) {
    for (const [key, entry] of tabs) {
      if (!entry.panel.contains(input)) continue;
      activateTab(key, true);
      const pair = pairOf(entry.tab);
      if (pair && entry.linked && $(`eq-panel:${pair[1]}`).contains(input)) toggleBond(entry);
    }
    if ($('mixer-exact').contains(input)) $('mixer-exact').open = true;
  }

  /* ---- token dialog -----------------------------------------------------------
   * The token field and its Save/Change toggle are the editor's, unchanged;
   * they just live in a dialog the lock chip opens. A save that locks the field
   * closes it, since that is the whole errand. */
  const tokenDialog = $('token-dialog');
  $('token-open').addEventListener('click', () => {
    if (typeof tokenDialog.showModal === 'function') tokenDialog.showModal();
    else tokenDialog.setAttribute('open', '');
    $('token').focus();
  });
  $('token-toggle').addEventListener('click', () => {
    if ($('token').readOnly && typeof tokenDialog.close === 'function') tokenDialog.close();
  });

  $('errors').addEventListener(
    'click',
    (event) => {
      const link = event.target.closest('a[href^="#f:"]');
      const input = link && field(decodeURIComponent(link.getAttribute('href').slice('#f:'.length)));
      if (input) revealContainerOf(input);
    },
    true,
  );

  /* ---- wiring --------------------------------------------------------------- */

  function syncAll() {
    for (const entry of faders.values()) syncFader(entry);
    syncStrip();
    syncSteppers();
    const values = readValues();
    renderEquation(values);
    renderDial(values);
    renderVerdict(values);
    syncLayoutAwareChip();
    syncFlowWeights(values);
    syncBonds();
    syncTabMarks();
    /* The exact values open themselves the moment one goes out of range, so
     * the error summary's link always lands on a visible field. */
    if ($('card-matrix').classList.contains('card--invalid')) $('mixer-exact').open = true;
  }

  function formSignature() {
    const parts = WATCHED_PATHS.map((path) => {
      const input = field(path);
      return input ? `${input.type === 'checkbox' ? input.checked : input.value}|${input.disabled}` : '';
    });
    parts.push($('card-matrix').className);
    for (const input of document.querySelectorAll('#input-channels [data-field], #output-eq [data-field]')) {
      parts.push(input.value);
    }
    for (const input of steppers) parts.push(`${input.disabled}`);
    parts.push(String(document.querySelectorAll('.field--invalid').length));
    return parts.join(';');
  }

  let lastSignature = '';
  let syncQueued = false;
  function syncIfChanged() {
    syncQueued = false;
    const signature = formSignature();
    if (signature === lastSignature) return;
    lastSignature = signature;
    syncAll();
  }

  for (const descriptor of FADERS) buildFader(descriptor);
  buildFlow();
  buildEqTabs();
  activateTab('front', false);
  followCurveSelection();
  buildStrip();
  labelEqCells();
  for (const input of document.querySelectorAll('input[type="number"]')) {
    if (!input.closest('.bench-legacy')) stepper(input);
  }

  /* An edit -- typed, a fader, the curve, a preset -- dispatches 'input' or
   * 'change' on a field. Those do not bubble, but a capture listener on the
   * document still sees them. The sync waits for a zero-delay timer, not a
   * microtask: for a real keystroke the browser drains microtasks between
   * listeners, so a microtask queued here would run BEFORE the field's own
   * handler (the clip-free headroom, the disabled flags, validation). A device
   * read rewrites the fields with no event at all, so the same check also runs
   * on a timer. A text observer on the editor's byte count would miss an edit
   * that leaves the count unchanged: Chromium reports no mutation for a text
   * write equal to the old text. */
  const queueSync = () => {
    if (syncQueued) return;
    syncQueued = true;
    setTimeout(syncIfChanged, 0);
  };
  document.addEventListener('input', queueSync, true);
  document.addEventListener('change', queueSync, true);
  setInterval(syncIfChanged, FORM_CHECK_MS);

  const observe = (node, options, callback) => {
    if (node) new MutationObserver(callback).observe(node, options);
  };
  /* Hints, not the signal: a device read, a preset load and an apply each
   * rewrite one of these lines, so the faders follow at once rather than on
   * the next tick. The signature check makes a redundant hint free. */
  const lineChanges = { childList: true, characterData: true, subtree: true, attributes: true };
  for (const id of ['status', 'apply-state', 'preset-status', 'preset-origin']) observe($(id), lineChanges, queueSync);
  observe($('curve-legend'), { attributes: true, attributeFilter: ['aria-pressed'], subtree: true }, followCurveSelection);
  observe($('preset-select'), { childList: true }, renderPresetChips);
  observe($('preset-origin'), { childList: true, characterData: true, subtree: true }, syncPresetChipState);
  observe($('preset-load'), { attributes: true, attributeFilter: ['disabled'] }, syncPresetChipState);
  const needleOptions = { attributes: true, attributeFilter: ['style', 'data-zone'], subtree: true };
  observe($('bars-input'), needleOptions, syncFlowMeters);
  observe($('bars-output'), needleOptions, syncFlowMeters);
  observe($('tx-gate'), { attributes: true, attributeFilter: ['data-state'] }, syncFlowEnds);
  observe($('input-source'), { attributes: true, attributeFilter: ['data-state'] }, syncFlowEnds);

  syncIfChanged();
  renderPresetChips();
  syncFlowMeters();
  syncFlowEnds();
})();
