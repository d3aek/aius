// @vitest-environment happy-dom

/* Regression for the SD-hosted DSP control surface (firmware/sd/www/index.html
 * + dsp-editor.js, on top of the shared sd-shared.js). The page is a classic
 * script served off the Teensy's SD card, so the harness loads the real
 * index.html body, evaluates the real sd-shared.js and then the real
 * dsp-editor.js against it, and stubs `fetch` with the device contract -- no
 * build step and no served URL, which is the cheapest harness that still
 * exercises the shipped files byte-for-byte.
 *
 * Fixture provenance: `dsp-config-*.json` are the exact wire shape
 * `http_dsp_config_json()` emits (firmware/src/optical_main.cpp) and the
 * swagger `DspConfig` example; the SD one additionally carries every EQ type,
 * the clip-free matrix policy, and (usefully for the stacked plot) two stereo
 * pairs whose members DIFFER. `meters-*.json` are the settled
 * `GET /api/meters` shape that `http_meters_json()` emits as of Phase 14b --
 * six positional dBFS input entries floored at -120, `[L, R]` output arrays,
 * and a per-mille integer waveform -- authored to the contract rather than
 * captured from a device, and covering the live, all-at-the-floor, and
 * never-published (`sequence == 0`) cases. The `/api/status` stub carries the
 * `carrier` flag and the latched `tx_gate` object the swagger `Status` schema
 * documents.
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const www = resolve(here, '../../firmware/sd/www');
const fixture = (name) => JSON.parse(readFileSync(resolve(here, 'fixtures', name), 'utf8'));
const indexHtml = readFileSync(resolve(www, 'index.html'), 'utf8');
const sharedSource = readFileSync(resolve(www, 'sd-shared.js'), 'utf8');
const editorSource = readFileSync(resolve(www, 'dsp-editor.js'), 'utf8');
const bodyMarkup = indexHtml
  .slice(indexHtml.indexOf('<body>') + '<body>'.length, indexHtml.indexOf('</body>'))
  .replace(/<script[\s\S]*?<\/script>/g, '');

const CHANNELS = ['fl', 'fr', 'fc', 'lfe', 'sl', 'sr'];
/* The five entities of the stacked plot, in legend order. A stereo pair is ONE
 * entity here; the per-channel form below is the escape hatch. */
const ENTITIES = ['front', 'centre', 'lfe', 'surround', 'output'];

const el = (id) => document.getElementById(id);
const fieldOf = (path) => el(`f:${path}`);
const valueOf = (path) => fieldOf(path).value;
const errorOf = (path) => el(`e:${path}`).textContent;
const settle = async (rounds = 30) => {
  for (let index = 0; index < rounds; index++) await Promise.resolve();
};

const jsonResponse = (status, body) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => body,
  text: async () => JSON.stringify(body),
});

const htmlResponse = (status, html) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => {
    throw new SyntaxError('not JSON');
  },
  text: async () => html,
});

/* The firmware's exact index framing (index_write), including the ../ row every
 * listing below the www/ root carries. Deliberately unsorted (FAT order) with a
 * directory and a non-json file mixed in. */
const presetsIndexHtml = (rows, footer) =>
  '<!DOCTYPE html><html><head><meta charset="utf-8"><title>/sd/www/presets/</title></head><body>' +
  `<h1>/sd/www/presets/</h1><ul>${rows}</ul><p>${footer} Sizes in bytes; at most 64 entries per listing.</p></body></html>`;
const PRESET_INDEX_DEFAULT = presetsIndexHtml(
  '<li><a href="../">../</a> dir</li><li><a href="movie.json">movie.json</a> 512</li>' +
    '<li><a href="Bass.json">Bass.json</a> 300</li><li><a href="notes.txt">notes.txt</a> 10</li>' +
    '<li><a href="sub/">sub/</a> dir</li>',
  '4 shown, 0 hidden, truncated: no.',
);

/* Nothing playing, gate dark: the quietest possible device, so a test that
 * cares about the signal-flow ends says so itself. */
const STATUS_IDLE = { carrier: 0, tx_gate: { open: 0, carrier_locked: 0, replay_running: 0 } };

let device;
let canvasOpsById;
const rejections = [];
const trackRejection = (reason) => rejections.push(reason);

/* Ops recorded per canvas id, with the drawing state each op was issued under:
 * the stacked plot draws five cascades through one context, so a stroke is only
 * identifiable by its colour, width and alpha. */
const opsOf = (id) => canvasOpsById.get(id) || [];

function installCanvas() {
  canvasOpsById = new Map();
  window.HTMLCanvasElement.prototype.getContext = function getContext() {
    const id = this.id || 'anonymous';
    if (!canvasOpsById.has(id)) canvasOpsById.set(id, []);
    const ops = canvasOpsById.get(id);
    const context = {
      strokeStyle: '',
      fillStyle: '',
      lineWidth: 1,
      globalAlpha: 1,
      font: '',
      textAlign: 'left',
    };
    const record =
      (name) =>
      (...args) =>
        ops.push([name, ...args]);
    context.clearRect = record('clearRect');
    context.beginPath = record('beginPath');
    context.closePath = record('closePath');
    context.moveTo = record('moveTo');
    context.lineTo = record('lineTo');
    context.arc = (...args) => ops.push(['arc', ...args, context.fillStyle, context.lineWidth]);
    context.fillText = (text, x, y) => ops.push(['fillText', text, x, y, context.textAlign]);
    context.stroke = () => ops.push(['stroke', context.strokeStyle, context.lineWidth, context.globalAlpha]);
    context.fill = () => ops.push(['fill', context.fillStyle]);
    return context;
  };
}

function createDevice(options = {}) {
  const stub = {
    config: 'config' in options ? options.config : fixture('dsp-config-default.json'),
    meters: 'meters' in options ? options.meters : fixture('meters-snapshot.json'),
    status: 'status' in options ? options.status : STATUS_IDLE,
    transportFailure: false,
    put: options.put || (() => jsonResponse(200, { ok: true })),
    /* null = no presets directory on the card yet (404 sd_not_found). */
    presetsIndex: 'presetsIndex' in options ? options.presetsIndex : null,
    presetFiles: options.presetFiles || {},
    presetPut:
      options.presetPut ||
      ((name, body, device) => {
        device.presetFiles[name] = JSON.parse(body);
        return jsonResponse(200, { ok: true });
      }),
    calls: [],
  };
  globalThis.fetch = vi.fn(async (url, init = {}) => {
    const method = init.method || 'GET';
    stub.calls.push({ url, method, headers: init.headers || {}, body: init.body });
    if (stub.transportFailure) throw new TypeError('Failed to fetch');
    if (url === '/api/dsp/config' && method === 'PUT') return stub.put(JSON.parse(init.body), stub);
    if (url === '/api/dsp/config') {
      return stub.config ? jsonResponse(200, stub.config) : jsonResponse(500, { ok: false, error: 'unavailable' });
    }
    if (url === '/api/meters') {
      return stub.meters ? jsonResponse(200, stub.meters) : jsonResponse(404, { ok: false, error: 'not_found' });
    }
    if (url === '/api/status') {
      return stub.status ? jsonResponse(200, stub.status) : jsonResponse(503, { ok: false, error: 'unavailable' });
    }
    if (url === '/sd/www/presets/') {
      return stub.presetsIndex
        ? htmlResponse(200, stub.presetsIndex)
        : jsonResponse(404, { ok: false, error: 'sd_not_found' });
    }
    if (url.startsWith('/sd/www/presets/')) {
      const name = decodeURIComponent(url.slice('/sd/www/presets/'.length)).replace(/\.json$/, '');
      if (method === 'PUT') return stub.presetPut(name, init.body, stub);
      if (method === 'DELETE') {
        if (!(name in stub.presetFiles)) return jsonResponse(404, { ok: false, error: 'sd_not_found' });
        delete stub.presetFiles[name];
        return jsonResponse(200, { ok: true });
      }
      return name in stub.presetFiles
        ? jsonResponse(200, stub.presetFiles[name])
        : jsonResponse(404, { ok: false, error: 'sd_not_found' });
    }
    return jsonResponse(404, { ok: false, error: 'not_found' });
  });
  return stub;
}

/* The document is rebuilt per test but `window` is not, so a previous boot's
 * window-level listeners (pointerup, resize, beforeunload) would answer for a
 * page instance that no longer exists -- with the state of the test that
 * created it. Each boot drops the last one's. */
const windowListeners = [];
const addWindowListener = window.addEventListener.bind(window);

/* sd-shared.js FIRST, exactly as index.html orders the two script tags: the
 * editor consumes window.aiusSdShared as it boots. */
function evaluatePage() {
  for (const [type, handler] of windowListeners.splice(0)) window.removeEventListener(type, handler);
  window.addEventListener = (type, handler, options) => {
    windowListeners.push([type, handler]);
    return addWindowListener(type, handler, options);
  };
  try {
    new Function(sharedSource)();
    new Function(editorSource)();
  } finally {
    window.addEventListener = addWindowListener;
  }
}

async function boot(options) {
  device = createDevice(options);
  document.body.innerHTML = bodyMarkup;
  installCanvas();
  evaluatePage();
  await settle();
  return device;
}

async function setNumber(path, raw) {
  const input = fieldOf(path);
  input.value = raw;
  input.dispatchEvent(new Event('input'));
  await settle();
}

async function setChoice(path, choice) {
  const input = fieldOf(path);
  input.value = choice;
  input.dispatchEvent(new Event('change'));
  await settle();
}

async function click(id) {
  el(id).click();
  await settle();
}

const putCalls = () => device.calls.filter((call) => call.method === 'PUT');
const callsTo = (url) => device.calls.filter((call) => call.url === url);

/* Ops accumulate across redraws, so an assertion about "what is on screen" reads
 * the last frame -- everything the canvas recorded after its final clearRect --
 * and groups it into paths the way the page strokes them. */
const lastFrame = (id) => {
  const ops = opsOf(id);
  const start = ops.map((op) => op[0]).lastIndexOf('clearRect');
  return start === -1 ? ops : ops.slice(start);
};
const pathsOf = (id) =>
  lastFrame(id).reduce((groups, op) => {
    if (op[0] === 'beginPath') groups.push([]);
    else if (groups.length > 0) groups[groups.length - 1].push(op);
    return groups;
  }, []);
const handlesOf = (id) => lastFrame(id).filter((op) => op[0] === 'arc');
const vertices = (ops) => ops.filter((op) => op[0] === 'moveTo' || op[0] === 'lineTo');
const labelsOf = (id) => lastFrame(id).filter((op) => op[0] === 'fillText');

/* The five cascade polylines: each is CURVE_STEPS + 1 vertices, an order of
 * magnitude more than the grid (30) or the 0 dB reference (2). */
const cascadePaths = () => pathsOf('curve').filter((path) => vertices(path).length > 64);
const strokeOf = (path) => path.find((op) => op[0] === 'stroke');
/* The emphasised curve is the selected one: full alpha, double width. */
const selectedCascade = () => cascadePaths().find((path) => strokeOf(path)[2] === 2 && strokeOf(path)[3] === 1);
const dimmedCascades = () => cascadePaths().filter((path) => strokeOf(path)[3] < 1);

/* happy-dom has no layout engine -- every getBoundingClientRect is all zeroes --
 * so a canvas that has to answer pointer questions is given one explicitly. It
 * deliberately matches the canvas's own 960x240 backing store, which makes the
 * client coordinates in these tests and the drawn coordinates the same numbers. */
const CURVE_RECT = { left: 0, top: 0, width: 960, height: 240, right: 960, bottom: 240, x: 0, y: 0 };
const stubRect = (id = 'curve') => {
  el(id).getBoundingClientRect = () => CURVE_RECT;
};
const maths = () => window.aiusDspEditorMath;
const atHz = (hz) => maths().hzToX(hz, CURVE_RECT);
const atDb = (db) => maths().dbToY(db, CURVE_RECT);
const pointer = (type, clientX, clientY, extra = {}) =>
  el('curve').dispatchEvent(
    new window.PointerEvent(type, { clientX, clientY, pointerId: 1, bubbles: true, cancelable: true, ...extra }),
  );
const dblclick = (clientX, clientY) =>
  el('curve').dispatchEvent(new window.MouseEvent('dblclick', { clientX, clientY, bubbles: true, cancelable: true }));
const key = (name) =>
  el('curve').dispatchEvent(new window.KeyboardEvent('keydown', { key: name, bubbles: true, cancelable: true }));
/* happy-dom's WheelEvent extends UIEvent rather than MouseEvent, so it carries
 * no client coordinates at all -- the one place this harness diverges from the
 * browser. The page reads event.clientX/clientY as the spec defines them, so the
 * test supplies them here rather than bending the page around the stub. */
const wheel = (clientX, clientY, deltaY) => {
  const event = new window.WheelEvent('wheel', { deltaY, bubbles: true, cancelable: true });
  return el('curve').dispatchEvent(Object.assign(event, { clientX, clientY }));
};
const chip = (entity) => el(`chip:${entity}`);

beforeEach(() => {
  rejections.length = 0;
  process.on('unhandledRejection', trackRejection);
  vi.useFakeTimers();
  /* happy-dom's localStorage persists across tests in a suite; without this a
   * token stored by one test would leak into every later boot. */
  localStorage.clear();
  /* Every destructive path is confirm()-gated now, and happy-dom has no dialog:
   * the default answer is "yes, go ahead", and a test that cares stubs its own. */
  window.confirm = vi.fn(() => true);
});

afterEach(() => {
  vi.useRealTimers();
  process.off('unhandledRejection', trackRejection);
  expect(rejections).toEqual([]);
});

describe('Aius SD DSP editor — model to DOM', () => {
  it('renders the factory default config into every control', async () => {
    await boot();

    expect(el('source').textContent).toBe('default');
    expect(el('generation').textContent).toBe('0');
    expect(el('sample-rate').textContent).toBe('48000 Hz');
    expect(el('apply-state').textContent).toBe('loaded');

    for (const feature of ['input_eq', 'matrix', 'output_eq', 'limiter', 'meters']) {
      expect(fieldOf(`features.${feature}`).checked).toBe(true);
    }
    for (const channel of CHANNELS) {
      expect(valueOf(`input_channels.${channel}.gain_db`)).toBe('0');
      expect(valueOf(`input_channels.${channel}.eq.0.type`)).toBe('none');
      expect(valueOf(`input_channels.${channel}.eq.1.type`)).toBe('none');
    }
    expect(valueOf('output.eq.0.type')).toBe('none');
    // Five significant figures on screen; the exact value still goes on the wire.
    expect(valueOf('matrix.center_gain_linear')).toBe('0.70711');
    expect(valueOf('matrix.surround_gain_linear')).toBe('0.70711');
    expect(valueOf('matrix.lfe_gain_linear')).toBe('0.31623');
    expect(valueOf('matrix.lfe_crossover_hz')).toBe('120');
    expect(valueOf('matrix.headroom_linear')).toBe('0.5');
    expect(fieldOf('matrix.clipfree').checked).toBe(false);
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(false);
    expect(fieldOf('limiter.enabled').checked).toBe(true);
    expect(valueOf('limiter.threshold_dbfs')).toBe('-1');
    expect(valueOf('limiter.attack_ms')).toBe('0.5');
    expect(valueOf('limiter.release_ms')).toBe('100');
    expect(valueOf('limiter.lookahead_ms')).toBe('2');
  });

  /* The factory-default fixture IS the checked-in factory model: build_config.py
   * generates AIUS_OPTICAL_CONFIG from firmware/config/optical_dsp.json, and a
   * factory-fresh GET /api/dsp/config serves exactly that model (source
   * "default", generation 0). This pin is what keeps the fixture honest -- it
   * shipped for weeks with pre-limiter-fix values (limiter off, LFE silenced,
   * doubled headroom) that no test could see, because nothing anchored it to
   * the firmware's input file. The page's reset-to-default table chains onto
   * the same anchor: page table -> this fixture -> optical_dsp.json. */
  it('keeps the default fixture identical to the generated factory model', () => {
    const factory = JSON.parse(readFileSync(resolve(here, '../../firmware/config/optical_dsp.json'), 'utf8'));
    expect(fixture('dsp-config-default.json')).toEqual(factory);
  });

  it('renders an SD-sourced config including every EQ type and the clip-free policy', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });

    expect(el('source').textContent).toBe('sd');
    expect(el('generation').textContent).toBe('4');
    expect(fieldOf('features.output_eq').checked).toBe(false);

    expect(valueOf('input_channels.fl.gain_db')).toBe('-3.5');
    expect(valueOf('input_channels.fl.eq.0.type')).toBe('peaking');
    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('120');
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('1.2');
    expect(valueOf('input_channels.fl.eq.0.gain_db')).toBe('-4.5');
    expect(valueOf('input_channels.fl.eq.1.type')).toBe('none');

    expect(valueOf('input_channels.fr.eq.1.type')).toBe('highpass');
    expect(valueOf('input_channels.fr.eq.1.frequency_hz')).toBe('45');
    // A highpass carries no gain, so the device ignores the field and so does the page.
    expect(fieldOf('input_channels.fr.eq.1.gain_db').disabled).toBe(true);
    // A bypass section is present in the config but has no bounded values.
    expect(valueOf('input_channels.sl.eq.0.type')).toBe('bypass');
    expect(fieldOf('input_channels.sl.eq.0.frequency_hz').disabled).toBe(true);
    expect(valueOf('input_channels.lfe.gain_db')).toBe('-60');

    expect(valueOf('output.eq.0.type')).toBe('peaking');
    expect(valueOf('output.eq.0.frequency_hz')).toBe('4000');
    expect(valueOf('output.eq.0.gain_db')).toBe('2');

    expect(valueOf('matrix.lfe_crossover_hz')).toBe('80');
    expect(fieldOf('matrix.clipfree').checked).toBe(true);
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(true);
    // 1 / (1 + 0.85 + 0.6 + |0.25|), resolved client-side exactly as the DSP does.
    expect(valueOf('matrix.headroom_linear')).toBe('0.37037');
    expect(fieldOf('limiter.enabled').checked).toBe(true);
    expect(valueOf('limiter.threshold_dbfs')).toBe('-1.5');
    expect(valueOf('limiter.lookahead_ms')).toBe('2');
  });

  it('reports the request body size beside Apply, against the device cap', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    const text = el('model-bytes').textContent;
    expect(text).toMatch(/^request body \d+ of 12288 bytes$/);
    const bytes = Number(text.match(/(\d+) of/)[1]);
    expect(bytes).toBeGreaterThan(0);
    expect(bytes).toBeLessThan(12288);

    // It is a fact about the FORM, so it moves with a keystroke.
    await setNumber('input_channels.fl.gain_db', '-3.55555');
    expect(Number(el('model-bytes').textContent.match(/(\d+) of/)[1])).not.toBe(bytes);
  });

  it('emits native min, max and step from the descriptor table', async () => {
    await boot();

    const frequency = fieldOf('input_channels.fl.eq.0.frequency_hz');
    expect([frequency.min, frequency.max, frequency.step]).toEqual(['10', '24000', '1']);
    const q = fieldOf('input_channels.fl.eq.0.q');
    expect([q.min, q.max, q.step]).toEqual(['0.1', '20', '0.05']);
    const gain = fieldOf('input_channels.fl.eq.0.gain_db');
    expect([gain.min, gain.max, gain.step]).toEqual(['-24', '24', '0.5']);
    const threshold = fieldOf('limiter.threshold_dbfs');
    expect([threshold.min, threshold.max, threshold.step]).toEqual(['-24', '0', '0.5']);
    const headroom = fieldOf('matrix.headroom_linear');
    expect([headroom.min, headroom.max, headroom.step]).toEqual(['0', '1', '0.01']);

    // step="any" is what took ArrowUp from Q 0.707 to 1.707.
    const numbers = [...document.querySelectorAll('input[type="number"]')];
    expect(numbers.length).toBeGreaterThan(50);
    expect(numbers.every((input) => input.step !== 'any' && input.min !== '' && input.max !== '')).toBe(true);
  });

  it('associates every caption and error with its own control, and keeps 78 resets out of the tab order', async () => {
    await boot();

    const caption = [...document.querySelectorAll('.field__label')].find(
      (label) => label.htmlFor === 'f:limiter.attack_ms',
    );
    expect(caption.textContent).toBe('Attack (ms)');
    expect(fieldOf('limiter.attack_ms').getAttribute('aria-describedby')).toBe('e:limiter.attack_ms');
    // A wrapping <label> folded caption + error + button into one accessible name.
    expect(fieldOf('limiter.attack_ms').closest('label')).toBe(null);

    const resets = [...document.querySelectorAll('.field__reset')];
    expect(resets.length).toBeGreaterThan(50);
    expect(resets.every((button) => button.tabIndex === -1)).toBe(true);
  });

  it('captions the matrix gains as linear and prints the decibels they mean', async () => {
    await boot();

    const caption = [...document.querySelectorAll('.field__label')].find(
      (label) => label.htmlFor === 'f:matrix.center_gain_linear',
    );
    expect(caption.textContent).toBe('Centre gain (linear)');
    expect(el('n:matrix.center_gain_linear').textContent).toBe('= -3.0 dB');
    expect(el('n:matrix.lfe_gain_linear').textContent).toBe('= -10.0 dB');

    await setNumber('matrix.surround_gain_linear', '0.5');
    expect(el('n:matrix.surround_gain_linear').textContent).toBe('= -6.0 dB');
    await setNumber('matrix.surround_gain_linear', '0');
    expect(el('n:matrix.surround_gain_linear').textContent).toBe('= -inf dB');
  });
});

describe('Aius SD DSP editor — writing the model back', () => {
  it('PUTs the loaded model verbatim with the auth header and concurrency token', async () => {
    const loaded = fixture('dsp-config-sd.json');
    await boot({ config: loaded });
    el('token').value = '0123456789abcdef';

    await click('apply');

    expect(putCalls()).toHaveLength(1);
    const [put] = putCalls();
    expect(put.url).toBe('/api/dsp/config');
    expect(put.headers['X-Aius-SD-Auth']).toBe('0123456789abcdef');
    expect(put.headers['Content-Type']).toBe('application/json');
    expect(JSON.parse(put.body)).toEqual({ ...loaded, base_generation: 4 });
  });

  /* Display precision is not wire precision: the field shows 0.70711 so a human
   * can read it, and the exact value the device sent comes back untouched until
   * a human actually types a different one. */
  it('sends the exact rendered value, and the typed one once it is typed', async () => {
    await boot();
    el('token').value = '0123456789abcdef';

    await click('apply');
    expect(JSON.parse(putCalls()[0].body).matrix.center_gain_linear).toBe(0.7071067811865475);

    await setNumber('matrix.center_gain_linear', '0.5');
    await click('apply');
    expect(JSON.parse(putCalls()[1].body).matrix.center_gain_linear).toBe(0.5);
  });

  it('sends an edited trim and a newly added peaking section', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('input_channels.fc.gain_db', '3.5');
    await setChoice('input_channels.fc.eq.0.type', 'peaking');
    await setNumber('input_channels.fc.eq.0.frequency_hz', '2500');
    await setNumber('input_channels.fc.eq.0.q', '1.4');
    await setNumber('input_channels.fc.eq.0.gain_db', '-6');

    await click('apply');

    const body = JSON.parse(putCalls()[0].body);
    expect(body.source).toBe('sd');
    expect(body.base_generation).toBe(0);
    expect(body.input_channels.fc).toEqual({
      gain_db: 3.5,
      eq: [{ type: 'peaking', frequency_hz: 2500, q: 1.4, gain_db: -6 }],
    });
    expect(body.input_channels.fl.eq).toEqual([]);
  });

  it('drops a section switched back to none', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    el('token').value = '0123456789abcdef';
    await setChoice('output.eq.0.type', 'none');

    await click('apply');

    expect(JSON.parse(putCalls()[0].body).output_eq).toEqual([]);
  });

  it('refuses to write without an SD auth token', async () => {
    await boot();
    await click('apply');

    expect(putCalls()).toHaveLength(0);
    expect(el('errors').hidden).toBe(false);
    expect(el('errors').textContent).toContain('SD auth token required');
  });

  /* A double-click on Apply used to land a second PUT built from the same
   * generation, which the device answered with a false 409 (UX finding E4). */
  it('disables the initiating buttons until the write resolves', async () => {
    let release;
    const gate = new Promise((resolve) => {
      release = resolve;
    });
    await boot({
      put: async () => {
        await gate;
        return jsonResponse(200, { ok: true });
      },
    });
    el('token').value = '0123456789abcdef';

    el('apply').click();
    await settle();
    expect(el('apply').disabled).toBe(true);
    expect(el('reload').disabled).toBe(true);
    expect(el('preset-save').disabled).toBe(true);

    // A second press during the flight is not a second write.
    el('apply').click();
    await settle();
    expect(putCalls()).toHaveLength(1);

    release();
    await settle();
    expect(el('apply').disabled).toBe(false);
    expect(el('reload').disabled).toBe(false);
    expect(el('apply-state').textContent).toBe('pending');
  });
});

describe('Aius SD DSP editor — bounds validation', () => {
  const cases = [
    ['input_channels.fl.gain_db', '13', 'Front left trim must be between -60 and 12 dB.'],
    ['input_channels.lfe.gain_db', '-61', 'LFE trim must be between -60 and 12 dB.'],
    ['matrix.center_gain_linear', '4.5', 'Centre gain must be between 0 and 4 linear.'],
    ['matrix.surround_gain_linear', '-0.1', 'Surround gain must be between 0 and 4 linear.'],
    ['matrix.lfe_gain_linear', '9', 'LFE gain must be between 0 and 4 linear.'],
    ['matrix.lfe_crossover_hz', '10', 'LFE crossover must be between 20 and 500 Hz.'],
    ['matrix.headroom_linear', '0', 'Headroom must be greater than 0 and at most 1 linear.'],
    ['matrix.headroom_linear', '1.2', 'Headroom must be greater than 0 and at most 1 linear.'],
    ['limiter.threshold_dbfs', '0.5', 'Threshold must be between -24 and 0 dBFS.'],
    ['limiter.attack_ms', '0.05', 'Attack must be between 0.1 and 100 ms.'],
    ['limiter.release_ms', '1200', 'Release must be between 1 and 1000 ms.'],
    ['limiter.lookahead_ms', '6', 'Lookahead must be between 0 and 5 ms.'],
    ['limiter.lookahead_ms', '', 'Lookahead is required.'],
  ];

  for (const [path, raw, message] of cases) {
    it(`rejects ${path} = ${JSON.stringify(raw)}`, async () => {
      await boot();
      el('token').value = '0123456789abcdef';
      await setNumber(path, raw);

      expect(errorOf(path)).toBe(message);
      expect(fieldOf(path).getAttribute('aria-invalid')).toBe('true');

      await click('apply');
      expect(putCalls()).toHaveLength(0);
      expect(el('apply-state').textContent).toBe('invalid');
      expect(el('errors').textContent).toContain(message);
    });
  }

  const eqCases = [
    ['frequency_hz', '9', 'Front left section 1 frequency must be between 10 and 24000 Hz.'],
    ['frequency_hz', '24001', 'Front left section 1 frequency must be between 10 and 24000 Hz.'],
    ['q', '0.05', 'Front left section 1 Q must be between 0.1 and 20.'],
    ['q', '21', 'Front left section 1 Q must be between 0.1 and 20.'],
    ['gain_db', '25', 'Front left section 1 gain must be between -24 and 24 dB.'],
    ['gain_db', '-25', 'Front left section 1 gain must be between -24 and 24 dB.'],
  ];

  for (const [name, raw, message] of eqCases) {
    it(`rejects an EQ section ${name} of ${raw}`, async () => {
      await boot();
      el('token').value = '0123456789abcdef';
      await setChoice('input_channels.fl.eq.0.type', 'peaking');
      await setNumber(`input_channels.fl.eq.0.${name}`, raw);

      expect(errorOf(`input_channels.fl.eq.0.${name}`)).toBe(message);
      await click('apply');
      expect(putCalls()).toHaveLength(0);
    });
  }

  it('stops validating a section switched to bypass, and keeps validating a peaking one', async () => {
    await boot();
    await setChoice('input_channels.fl.eq.0.type', 'peaking');
    await setNumber('input_channels.fl.eq.0.frequency_hz', '5');
    expect(errorOf('input_channels.fl.eq.0.frequency_hz')).not.toBe('');

    await setChoice('input_channels.fl.eq.0.type', 'bypass');
    expect(errorOf('input_channels.fl.eq.0.frequency_hz')).toBe('');

    el('token').value = '0123456789abcdef';
    await click('apply');
    expect(JSON.parse(putCalls()[0].body).input_channels.fl.eq).toEqual([
      { type: 'bypass', frequency_hz: 5, q: 0.707, gain_db: 0 },
    ]);
  });

  /* "3 fields out of range" with the fields 4000 px away was unusable: each
   * message is now a link to its own input, and the owning card says how many
   * of its own fields are wrong (UX finding E6). */
  it('anchors each error to its field and marks the owning card', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('matrix.lfe_crossover_hz', '10');
    await setNumber('limiter.release_ms', '1200');

    await click('apply');

    const links = [...el('errors').querySelectorAll('a')];
    expect(links.map((link) => link.getAttribute('href'))).toEqual([
      '#f:matrix.lfe_crossover_hz',
      '#f:limiter.release_ms',
    ]);
    expect(el('errors').textContent).toContain('2 fields out of range');

    expect(el('m:card-matrix').hidden).toBe(false);
    expect(el('m:card-matrix').textContent).toBe('1 out of range');
    expect(el('card-matrix').classList.contains('card--invalid')).toBe(true);
    expect(el('m:card-features').hidden).toBe(true);

    const scrolled = [];
    fieldOf('limiter.release_ms').scrollIntoView = (options) => scrolled.push(options);
    links[1].click();
    expect(scrolled).toHaveLength(1);
    expect(document.activeElement).toBe(fieldOf('limiter.release_ms'));

    // Fixing the field clears its card marker again.
    await setNumber('limiter.release_ms', '100');
    expect(el('m:card-limiter').hidden).toBe(true);
    expect(el('card-limiter').classList.contains('card--invalid')).toBe(false);
  });
});

describe('Aius SD DSP editor — optimistic concurrency', () => {
  it('surfaces a stale generation on 409 and re-reads on demand', async () => {
    const advanced = { ...fixture('dsp-config-default.json'), source: 'sd', config_generation: 7 };
    await boot({ put: () => jsonResponse(409, { ok: false, error: 'stale_generation' }) });
    el('token').value = '0123456789abcdef';

    await click('apply');

    expect(el('apply-state').textContent).toBe('stale');
    expect(el('stale').hidden).toBe(false);
    expect(el('status').textContent).toContain('Nothing was written');

    device.config = advanced;
    await click('stale-reload');

    expect(el('stale').hidden).toBe(true);
    expect(el('generation').textContent).toBe('7');
    expect(el('source').textContent).toBe('sd');
    expect(el('apply-state').textContent).toBe('loaded');

    // The re-read model becomes the new concurrency token.
    await click('apply');
    expect(JSON.parse(putCalls()[1].body).base_generation).toBe(7);
  });

  it('holds a pending state until the block-boundary apply bumps the generation', async () => {
    await boot();
    el('token').value = '0123456789abcdef';

    await click('apply');
    expect(el('apply-state').textContent).toBe('pending');
    expect(el('status').textContent).toContain('next audio block boundary');

    await vi.advanceTimersByTimeAsync(500);
    expect(el('apply-state').textContent).toBe('pending');

    device.config = { ...fixture('dsp-config-default.json'), source: 'sd', config_generation: 1 };
    await vi.advanceTimersByTimeAsync(500);
    await settle();

    expect(el('apply-state').textContent).toBe('applied');
    expect(el('source').textContent).toBe('sd');
    expect(el('generation').textContent).toBe('1');
  });

  /* The pending window is up to 20 s long and the form stays live throughout,
   * so adopting the device model wholesale used to throw away everything typed
   * during it (delivery audit finding). */
  it('keeps edits made during the pending window, and says the device moved', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await click('apply');

    await setNumber('limiter.release_ms', '250');
    device.config = { ...fixture('dsp-config-default.json'), source: 'sd', config_generation: 1 };
    await vi.advanceTimersByTimeAsync(500);
    await settle();

    expect(valueOf('limiter.release_ms')).toBe('250');
    expect(el('generation').textContent).toBe('1');
    expect(el('apply-state').textContent).toBe('edited');
    expect(el('status').textContent).toContain('re-read');
  });

  it('surfaces a rejected write without touching the loaded model', async () => {
    await boot({ put: () => jsonResponse(400, { ok: false, error: 'dsp_config_invalid' }) });
    el('token').value = '0123456789abcdef';

    await click('apply');

    expect(el('apply-state').textContent).toBe('error');
    expect(el('errors').textContent).toContain('dsp_config_invalid');
    expect(el('generation').textContent).toBe('0');
  });
});

describe('Aius SD DSP editor — protecting unapplied work', () => {
  it('promotes the pill to edited from a freshly loaded read', async () => {
    await boot();
    expect(el('apply-state').textContent).toBe('loaded');

    await setNumber('limiter.release_ms', '250');
    expect(el('apply-state').textContent).toBe('edited');
  });

  it('asks before a re-read discards edits, and keeps them on a refusal', async () => {
    await boot();
    await setNumber('limiter.release_ms', '250');
    window.confirm = vi.fn(() => false);

    await click('reload');

    expect(window.confirm).toHaveBeenCalledTimes(1);
    expect(valueOf('limiter.release_ms')).toBe('250');
    expect(el('status').textContent).toContain('Kept the edits');

    window.confirm = vi.fn(() => true);
    await click('reload');
    expect(valueOf('limiter.release_ms')).toBe('100');
  });

  it('does not ask when the form still matches the device', async () => {
    await boot();
    await click('reload');
    expect(window.confirm).not.toHaveBeenCalled();
  });

  it('guards the tab against leaving with unapplied edits', async () => {
    await boot();
    const leaving = () => {
      const event = new window.Event('beforeunload', { cancelable: true, bubbles: false });
      window.dispatchEvent(event);
      return event.defaultPrevented;
    };
    expect(leaving()).toBe(false);

    await setNumber('limiter.release_ms', '250');
    expect(leaving()).toBe(true);
  });
});

describe('Aius SD DSP editor — meters', () => {
  it('renders numeric levels and telemetry from a snapshot', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);

    expect(el('meter-state').textContent).toBe('live');
    expect(el('meter-in-fl-peak').textContent).toBe('-6.2');
    expect(el('meter-in-fl-rms').textContent).toBe('-18.4');
    expect(el('meter-in-sr-peak').textContent).toBe('-15.9');
    expect(el('meter-out-l-peak').textContent).toBe('-3.1');
    expect(el('meter-out-r-peak').textContent).toBe('-3.4');
    expect(el('meter-out-r-rms').textContent).toBe('-14.9');
    expect(el('meter-sequence').textContent).toBe('1287');
    expect(el('meter-queue').textContent).toBe('512 frames');
    // Microseconds stop being legible at four digits (UX finding N9).
    expect(el('meter-latency').textContent).toBe('10.7 ms');
    // Raw DWT counts, scaled and against the budget that makes them mean something.
    expect(el('meter-cycles').textContent).toBe('41k / 59k of 6.40M budget');
    expect(el('meter-summary').textContent).toContain('-1.8');
  });

  it('renders levels at the -120 dBFS floor as silence, not as a number', async () => {
    await boot({ meters: fixture('meters-silent.json') });
    await vi.advanceTimersByTimeAsync(100);

    expect(el('meter-state').textContent).toBe('live');
    expect(el('meter-in-fl-peak').textContent).toBe('-inf');
    expect(el('meter-in-fl-rms').textContent).toBe('-inf');
    expect(el('meter-in-fr-peak').textContent).toBe('-0.1');
    expect(el('meter-out-l-peak').textContent).toBe('-0.1');
    expect(el('meter-out-r-peak').textContent).toBe('-inf');
    // The limiter's defensive fully-closed value is below the floor too.
    expect(el('meter-summary').textContent).toContain('-inf');
  });

  it('shows "no data" for a never-published snapshot without backing off', async () => {
    await boot({ meters: fixture('meters-idle.json') });
    await vi.advanceTimersByTimeAsync(100);

    expect(el('meter-state').textContent).toBe('no data');
    expect(el('meter-sequence').textContent).toBe('0');
    // Not rendered as a genuine zero-level signal.
    expect(el('meter-in-fl-peak').textContent).toBe('--');
    expect(el('meter-out-l-rms').textContent).toBe('--');
    expect(el('meter-queue').textContent).toBe('--');
    expect(el('meter-latency').textContent).toBe('--');
    expect(el('meter-cycles').textContent).toBe('--');
    expect(el('meter-summary').textContent).toContain('No meter snapshot published yet');

    // A valid 200 is not a fault, so the 10 Hz poll continues at full rate --
    // unlike the absent-endpoint branch, which backs off to 5 s.
    const afterFirst = callsTo('/api/meters').length;
    await vi.advanceTimersByTimeAsync(1000);
    expect(callsTo('/api/meters').length).toBe(afterFirst + 10);

    // Once the device starts publishing, the page goes live on the next poll.
    device.meters = fixture('meters-snapshot.json');
    await vi.advanceTimersByTimeAsync(100);
    expect(el('meter-state').textContent).toBe('live');
    expect(el('meter-sequence').textContent).toBe('1287');
  });

  it('draws the waveform envelope mirrored about the centre line', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);

    const paths = opsOf('waveform').reduce((groups, op) => {
      if (op[0] === 'beginPath') groups.push([]);
      else if (groups.length > 0) groups[groups.length - 1].push(op);
      return groups;
    }, []);
    expect(paths).toHaveLength(2);

    const envelope = vertices(paths[1]);
    // Five per-mille points [0, 500, 1000, 500, 0] across a 960x180 canvas:
    // x = index * 240, y = 90 -/+ (permille / 1000) * 90.
    expect(envelope[0]).toEqual(['moveTo', 0, 90]);
    expect(envelope[1][1]).toBe(240);
    expect(envelope[1][2]).toBeCloseTo(45, 3);
    expect(envelope[2]).toEqual(['lineTo', 480, 0]);
    expect(envelope[4]).toEqual(['lineTo', 960, 90]);
    // The return leg mirrors below the centre line, ending back at the origin.
    expect(envelope[7][1]).toBe(480);
    expect(envelope[7][2]).toBeCloseTo(180, 3);
    expect(envelope[envelope.length - 1]).toEqual(['lineTo', 0, 90]);
    expect(paths[1].some((op) => op[0] === 'fill')).toBe(true);
  });

  it('caps rendering at the contract 128 points', async () => {
    const oversized = { ...fixture('meters-snapshot.json'), waveform: Array.from({ length: 400 }, () => 500) };
    await boot({ meters: oversized });
    await vi.advanceTimersByTimeAsync(100);

    const points = opsOf('waveform').filter((op) => op[0] === 'lineTo' || op[0] === 'moveTo');
    // Centre line (moveTo + lineTo) plus the mirrored envelope of 128 points.
    expect(points).toHaveLength(2 + 128 * 2);
  });

  it('degrades gracefully and backs off when /api/meters is absent', async () => {
    await boot({ meters: null });
    await vi.advanceTimersByTimeAsync(100);

    expect(el('meter-state').textContent).toBe('unreachable');
    expect(el('meter-summary').textContent).toContain('HTTP 404');
    expect(el('meter-in-fl-peak').textContent).toBe('--');
    // The rest of the page is unaffected.
    expect(el('apply-state').textContent).toBe('loaded');
    expect(valueOf('matrix.headroom_linear')).toBe('0.5');

    const afterFirst = callsTo('/api/meters').length;
    await vi.advanceTimersByTimeAsync(1000);
    expect(callsTo('/api/meters').length).toBe(afterFirst);

    await vi.advanceTimersByTimeAsync(4100);
    expect(callsTo('/api/meters').length).toBe(afterFirst + 1);
  });
});

/* The two ends of the signal-flow strip: the page never said whether the audio
 * was a live optical carrier, a replay, or nothing at all. */
describe('Aius SD DSP editor — input source and transmit gate', () => {
  it('names a live optical carrier and an open gate', async () => {
    await boot({ status: { carrier: 1, tx_gate: { open: 1, carrier_locked: 1, replay_running: 0 } } });

    expect(el('input-source').textContent).toBe('input: optical carrier');
    expect(el('input-source').dataset.state).toBe('carrier');
    expect(el('tx-gate').textContent).toBe('output: gate open');
    expect(el('tx-gate').dataset.state).toBe('open');
  });

  it('names a replay when the latched gate says a replay is running', async () => {
    await boot({ status: { carrier: 0, tx_gate: { open: 1, carrier_locked: 0, replay_running: 1 } } });

    expect(el('input-source').textContent).toBe('input: replay');
    expect(el('input-source').dataset.state).toBe('replay');
    expect(el('tx-gate').dataset.state).toBe('open');
  });

  it('says nothing is playing and the gate is dark', async () => {
    await boot();

    expect(el('input-source').textContent).toBe('input: none');
    expect(el('tx-gate').textContent).toBe('output: gate dark');
    expect(el('tx-gate').dataset.state).toBe('dark');
  });

  it('polls at 1 Hz and reads a failed poll as unreachable, not as silence', async () => {
    await boot();
    expect(callsTo('/api/status')).toHaveLength(1);

    device.status = null;
    await vi.advanceTimersByTimeAsync(1000);

    expect(callsTo('/api/status')).toHaveLength(2);
    expect(el('input-source').dataset.state).toBe('unreachable');
    expect(el('tx-gate').dataset.state).toBe('unreachable');
  });
});

describe('Aius SD DSP editor — transport failures', () => {
  it('shows a visible error when the config read fails outright', async () => {
    device = createDevice();
    device.transportFailure = true;
    document.body.innerHTML = bodyMarkup;
    installCanvas();
    evaluatePage();
    await settle();

    expect(el('apply-state').textContent).toBe('error');
    expect(el('errors').hidden).toBe(false);
    expect(el('errors').textContent).toContain('network error');
    expect(el('status').textContent).toContain('No configuration loaded');
  });

  it('shows a visible error when the write transport fails mid-apply', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    device.transportFailure = true;

    await click('apply');

    expect(el('apply-state').textContent).toBe('error');
    expect(el('errors').textContent).toContain('network error');
    expect(el('status').textContent).toBe('Configuration unchanged.');
  });
});

describe('Aius SD DSP editor — ergonomics', () => {
  it('restores the SD auth token from localStorage on boot', async () => {
    localStorage.setItem('aius-sd-auth', '0123456789abcdef');
    await boot();
    expect(el('token').value).toBe('0123456789abcdef');
  });

  it('stores the token as it is typed, and removes it when cleared', async () => {
    await boot();
    el('token').value = 'fedcba9876543210';
    el('token').dispatchEvent(new Event('input'));
    expect(localStorage.getItem('aius-sd-auth')).toBe('fedcba9876543210');

    el('token').value = '';
    el('token').dispatchEvent(new Event('input'));
    expect(localStorage.getItem('aius-sd-auth')).toBe(null);
  });

  it('resets a single field to the factory value', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });

    expect(valueOf('matrix.lfe_gain_linear')).toBe('0.25');
    el('r:matrix.lfe_gain_linear').click();
    await settle();
    expect(valueOf('matrix.lfe_gain_linear')).toBe('0.31623');
    expect(el('n:matrix.lfe_gain_linear').textContent).toBe('= -10.0 dB');
  });

  it('reset on an EQ type returns the slot to none and drops it from the PUT', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    el('token').value = '0123456789abcdef';

    el('r:output.eq.0.type').click();
    await settle();
    expect(valueOf('output.eq.0.type')).toBe('none');

    await click('apply');
    expect(JSON.parse(putCalls()[0].body).output_eq).toEqual([]);
  });

  /* The form's output paths are `output.eq.N.*` while the model keys the same
   * sections `output_eq[N]`: without the alias a reset of an output section
   * read undefined, correct only while the factory ships no output sections
   * (delivery audit). A synthetic factory table with one proves the path. */
  it('resolves a factory output-EQ value through the output_eq alias', async () => {
    await boot();
    const synthetic = {
      ...{ features: {}, input_channels: {}, matrix: {}, limiter: {} },
      output_eq: [{ type: 'peaking', frequency_hz: 8000, q: 1.1, gain_db: -2.5 }],
    };

    expect(maths().factoryValueOf(synthetic, 'output.eq.0.type')).toBe('peaking');
    expect(maths().factoryValueOf(synthetic, 'output.eq.0.frequency_hz')).toBe(8000);
    expect(maths().factoryValueOf(synthetic, 'output.eq.0.q')).toBe(1.1);
    expect(maths().factoryValueOf(synthetic, 'output.eq.0.gain_db')).toBe(-2.5);
    // Slot 1 has no factory section, which resets to the page-level "none".
    expect(maths().factoryValueOf(synthetic, 'output.eq.1.frequency_hz')).toBe(undefined);
    // Non-output paths still read straight out of the table.
    expect(maths().factoryValueOf({ matrix: { lfe_crossover_hz: 120 } }, 'matrix.lfe_crossover_hz')).toBe(120);
  });

  it('disables reset alongside its field', async () => {
    // SD fixture has clipfree on, so headroom is device-resolved and read-only.
    await boot({ config: fixture('dsp-config-sd.json') });
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(true);
    expect(el('r:matrix.headroom_linear').disabled).toBe(true);
    expect(el('r:matrix.center_gain_linear').disabled).toBe(false);
  });

  /* THE defaults pin. Resetting every control and collecting the model must
   * reproduce the factory fixture's five editable groups exactly -- and the
   * fixture itself is asserted equal to firmware/config/optical_dsp.json
   * above, so the page's FACTORY_DEFAULTS table cannot drift from the real
   * factory model without failing here. Five-significant-figure DISPLAY does
   * not weaken it: the exact value rides the input until a human retypes it. */
  it('resetting every field reproduces the factory model in the PUT body', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    el('token').value = '0123456789abcdef';

    /* Two passes: resetting clipfree re-enables the headroom field, whose own
     * reset was disabled (skipped) when the first pass walked past it. */
    for (let pass = 0; pass < 2; pass++) {
      for (const reset of document.querySelectorAll('.field__reset')) {
        if (!reset.disabled) reset.click();
      }
      await settle();
    }

    await click('apply');
    expect(putCalls()).toHaveLength(1);
    const body = JSON.parse(putCalls()[0].body);
    const factory = fixture('dsp-config-default.json');
    expect(body.features).toEqual(factory.features);
    expect(body.input_channels).toEqual(factory.input_channels);
    expect(body.output_eq).toEqual(factory.output_eq);
    expect(body.matrix).toEqual(factory.matrix);
    expect(body.limiter).toEqual(factory.limiter);
  });
});

describe('Aius SD DSP editor — presets', () => {
  const fiveGroups = (name) => {
    const { features, input_channels, output_eq, matrix, limiter } = fixture(name);
    return { features, input_channels, output_eq, matrix, limiter };
  };
  const presetNames = () => [...document.querySelectorAll('.preset-list__name')].map((node) => node.textContent);
  const presetRowButton = (name, label) => {
    const row = [...document.querySelectorAll('#preset-list li')].find(
      (li) => li.querySelector('.preset-list__name').textContent === name,
    );
    return [...row.querySelectorAll('button')].find((button) => button.textContent === label);
  };

  it('lists .json presets from the directory index: sorted, dirs and ../ and foreign files skipped', async () => {
    await boot({ presetsIndex: PRESET_INDEX_DEFAULT });
    expect(presetNames()).toEqual(['Bass', 'movie']);
    expect(el('preset-status').textContent).toBe('2 presets on the card.');
    expect(el('preset-errors').hidden).toBe(true);
  });

  it('reads a missing presets directory as an empty list, not an error', async () => {
    await boot();
    expect(presetNames()).toEqual([]);
    expect(el('preset-status').textContent).toBe('No presets on the card yet.');
    expect(el('preset-errors').hidden).toBe(true);
  });

  it('treats sd_not_directory as a fault, not emptiness', async () => {
    await boot({ presetsIndex: null });
    device.presetsIndex = undefined;
    globalThis.fetch.mockImplementationOnce(async () => jsonResponse(404, { ok: false, error: 'sd_not_directory' }));
    await click('preset-refresh');
    expect(el('preset-errors').hidden).toBe(false);
    expect(el('preset-errors').textContent).toContain('sd_not_directory');
  });

  it('surfaces a truncated listing instead of passing it off as complete', async () => {
    await boot({
      presetsIndex: presetsIndexHtml(
        '<li><a href="../">../</a> dir</li><li><a href="a.json">a.json</a> 10</li>',
        '64 shown, 3 hidden, truncated: yes.',
      ),
    });
    expect(el('preset-errors').hidden).toBe(false);
    expect(el('preset-errors').textContent).toContain('INCOMPLETE');
  });

  const nameCases = [
    ['', 'Preset name is required.'],
    ['a'.repeat(28), 'at most 27 characters'],
    ['bad name', 'letters, digits, dot, underscore and hyphen'],
    ['myconfig', 'may not end in "config"'],
    ['MyConfig', 'may not end in "config"'],
  ];
  for (const [name, message] of nameCases) {
    it(`refuses the preset name ${JSON.stringify(name)}`, async () => {
      await boot();
      el('token').value = '0123456789abcdef';
      el('preset-name').value = name;
      await click('preset-save');
      expect(putCalls()).toHaveLength(0);
      expect(el('preset-errors').textContent).toContain(message);
    });
  }

  it('counts the name as it is typed and says what is wrong with it', async () => {
    await boot();
    el('preset-name').value = 'movie night';
    el('preset-name').dispatchEvent(new Event('input'));

    expect(el('preset-name-hint').textContent).toContain('11 / 27 characters');
    expect(el('preset-name-hint').textContent).toContain('letters, digits, dot, underscore and hyphen');

    el('preset-name').value = 'movie-night';
    el('preset-name').dispatchEvent(new Event('input'));
    // Precedence between the field and an uploaded file's own name, said once.
    expect(el('preset-name-hint').textContent).toContain("the file's own name when it is empty");
  });

  it('refuses a case-variant of an existing preset name, naming the collision', async () => {
    await boot({ presetsIndex: PRESET_INDEX_DEFAULT });
    el('token').value = '0123456789abcdef';
    el('preset-name').value = 'bass';
    await click('preset-save');
    expect(putCalls()).toHaveLength(0);
    expect(el('preset-errors').textContent).toContain('"Bass"');
    expect(el('preset-errors').textContent).toContain('case-insensitive');
  });

  /* A listing that FAILED leaves state.presets empty, which is indistinguishable
   * from "no presets" -- and FAT would then case-overwrite silently (delivery
   * audit). An unknown list refuses the save instead of guessing. */
  it('refuses to save while the listing is faulted, and saves once it is refreshed', async () => {
    await boot({ presetsIndex: PRESET_INDEX_DEFAULT });
    device.presetsIndex = undefined;
    globalThis.fetch.mockImplementationOnce(async () => jsonResponse(503, { ok: false, error: 'sd_busy' }));
    await click('preset-refresh');
    expect(el('preset-errors').textContent).toContain('sd_busy');

    el('token').value = '0123456789abcdef';
    el('preset-name').value = 'bass';
    await click('preset-save');
    expect(putCalls()).toHaveLength(0);
    expect(el('preset-errors').textContent).toContain('could not be read');

    device.presetsIndex = PRESET_INDEX_DEFAULT;
    await click('preset-refresh');
    await click('preset-save');
    // The refreshed listing knows about "Bass", so the case collision is caught.
    expect(putCalls()).toHaveLength(0);
    expect(el('preset-errors').textContent).toContain('"Bass"');
  });

  it('saves the five editable groups with the auth header, then refreshes the list', async () => {
    await boot({ config: fixture('dsp-config-sd.json'), presetsIndex: PRESET_INDEX_DEFAULT });
    el('token').value = '0123456789abcdef';
    el('preset-name').value = 'evening';
    await click('preset-save');

    const put = device.calls.find((call) => call.method === 'PUT');
    expect(put.url).toBe('/sd/www/presets/evening.json');
    expect(put.headers['X-Aius-SD-Auth']).toBe('0123456789abcdef');
    const body = JSON.parse(put.body);
    expect(Object.keys(body).sort()).toEqual(['features', 'input_channels', 'limiter', 'matrix', 'output_eq']);
    expect(body).toEqual(fiveGroups('dsp-config-sd.json'));
    expect(el('preset-status').textContent).toBe('Saved evening.json.');
  });

  it('refuses to save without a token, and with an invalid form', async () => {
    await boot();
    el('preset-name').value = 'quiet';
    await click('preset-save');
    expect(putCalls()).toHaveLength(0);
    expect(el('preset-errors').textContent).toContain('SD auth token required');

    el('token').value = '0123456789abcdef';
    await setNumber('limiter.release_ms', '1200');
    await click('preset-save');
    expect(putCalls()).toHaveLength(0);
    expect(el('preset-errors').textContent).toContain('Fix the form first');
  });

  it('loads a preset into the form only: device facts and concurrency token untouched', async () => {
    const preset = {
      ...fiveGroups('dsp-config-default.json'),
      /* A hand-edited file smuggling device facts: all three must be ignored. */
      source: 'default',
      config_generation: 99,
      sample_rate_hz: 96000,
    };
    await boot({
      config: fixture('dsp-config-sd.json'),
      presetsIndex: PRESET_INDEX_DEFAULT,
      presetFiles: { movie: preset },
    });
    el('token').value = '0123456789abcdef';
    presetRowButton('movie', 'Load').click();
    await settle();

    expect(valueOf('matrix.headroom_linear')).toBe('0.5');
    expect(fieldOf('limiter.enabled').checked).toBe(true);
    expect(el('generation').textContent).toBe('4');
    expect(el('sample-rate').textContent).toBe('48000 Hz');
    expect(el('apply-state').textContent).toBe('edited');
    expect(el('preset-status').textContent).toContain('Apply to send');

    await click('apply');
    const body = JSON.parse(putCalls()[0].body);
    expect(body.base_generation).toBe(4);
    expect(body.sample_rate_hz).toBe(48000);
    expect(body.matrix).toEqual(fixture('dsp-config-default.json').matrix);
  });

  it('asks before a preset load clobbers unapplied edits', async () => {
    await boot({
      presetsIndex: PRESET_INDEX_DEFAULT,
      presetFiles: { movie: fiveGroups('dsp-config-sd.json') },
    });
    await setNumber('limiter.release_ms', '250');
    window.confirm = vi.fn(() => false);

    presetRowButton('movie', 'Load').click();
    await settle();

    expect(window.confirm).toHaveBeenCalledTimes(1);
    expect(valueOf('limiter.release_ms')).toBe('250');
    expect(el('preset-status').textContent).toContain('was not loaded');
  });

  it('refuses a preset whose EQ exceeds the two-section wire bound', async () => {
    const overfull = fiveGroups('dsp-config-default.json');
    overfull.output_eq = [
      { type: 'bypass', frequency_hz: 0, q: 0, gain_db: 0 },
      { type: 'bypass', frequency_hz: 0, q: 0, gain_db: 0 },
      { type: 'bypass', frequency_hz: 0, q: 0, gain_db: 0 },
    ];
    await boot({ presetsIndex: PRESET_INDEX_DEFAULT, presetFiles: { movie: overfull } });
    presetRowButton('movie', 'Load').click();
    await settle();
    expect(el('preset-errors').textContent).toContain('output_eq must be an array of at most 2');
    expect(el('apply-state').textContent).toBe('loaded');
  });

  it('deletes a preset with the auth header and refreshes, but only after a confirmation', async () => {
    await boot({
      presetsIndex: PRESET_INDEX_DEFAULT,
      presetFiles: { movie: fiveGroups('dsp-config-default.json') },
    });
    el('token').value = '0123456789abcdef';

    window.confirm = vi.fn(() => false);
    presetRowButton('movie', 'Delete').click();
    await settle();
    expect(device.calls.some((call) => call.method === 'DELETE')).toBe(false);
    expect(el('preset-status').textContent).toContain('was kept');

    window.confirm = vi.fn(() => true);
    presetRowButton('movie', 'Delete').click();
    await settle();
    const del = device.calls.find((call) => call.method === 'DELETE');
    expect(window.confirm.mock.calls[0][0]).toContain('movie.json');
    expect(del.url).toBe('/sd/www/presets/movie.json');
    expect(del.headers['X-Aius-SD-Auth']).toBe('0123456789abcdef');
    expect(el('preset-status').textContent).toContain('Deleted');
  });

  it('renders 507 sd_upload as busy-or-refused, never as card full', async () => {
    await boot({
      presetsIndex: PRESET_INDEX_DEFAULT,
      presetPut: () => jsonResponse(507, { ok: false, error: 'sd_upload' }),
    });
    el('token').value = '0123456789abcdef';
    el('preset-name').value = 'evening';
    await click('preset-save');
    expect(el('preset-errors').textContent).toContain('busy');
    expect(el('preset-errors').textContent).not.toContain('full');
  });

  it('uploads a local file as a preset, dropping device facts on the way in', async () => {
    await boot({ presetsIndex: PRESET_INDEX_DEFAULT });
    el('token').value = '0123456789abcdef';
    const smuggling = {
      ...fiveGroups('dsp-config-default.json'),
      sample_rate_hz: 96000,
      config_generation: 12,
    };
    const file = new File([JSON.stringify(smuggling)], 'party.json', { type: 'application/json' });
    Object.defineProperty(el('preset-file'), 'files', { value: [file], configurable: true });
    await click('preset-upload');

    const put = device.calls.find((call) => call.method === 'PUT');
    expect(put.url).toBe('/sd/www/presets/party.json');
    expect(Object.keys(JSON.parse(put.body)).sort()).toEqual([
      'features',
      'input_channels',
      'limiter',
      'matrix',
      'output_eq',
    ]);
  });

  it('lets the preset-name field rename an upload', async () => {
    await boot({ presetsIndex: PRESET_INDEX_DEFAULT });
    el('token').value = '0123456789abcdef';
    el('preset-name').value = 'renamed';
    const file = new File([JSON.stringify(fiveGroups('dsp-config-default.json'))], 'party.json');
    Object.defineProperty(el('preset-file'), 'files', { value: [file], configurable: true });
    await click('preset-upload');

    expect(device.calls.find((call) => call.method === 'PUT').url).toBe('/sd/www/presets/renamed.json');
  });
});

/* The pure half of the visual surface. These call the page's exported maths
 * directly with literal rects: happy-dom lays nothing out, so this -- not a
 * synthesised gesture -- is where the drag arithmetic and the RBJ curve maths
 * are actually proven. The real-pointer proof belongs to the on-device browser
 * pass (task #7 slice 4). */
describe('Aius SD DSP editor — curve and geometry maths', () => {
  const rect = { left: 0, top: 0, width: 960, height: 240 };

  beforeEach(async () => {
    await boot();
  });

  it('gives a peaking section exactly its gain at the centre frequency', () => {
    for (const gain of [-12, -4.5, 3, 12]) {
      const section = { type: 'peaking', frequency_hz: 1000, q: 1.2, gain_db: gain };
      expect(maths().sectionMagnitudeDb(section, 1000, 48000)).toBeCloseTo(gain, 9);
    }
  });

  it('leaves a peaking section flat far from its centre frequency', () => {
    const section = { type: 'peaking', frequency_hz: 1000, q: 2, gain_db: 12 };
    expect(maths().sectionMagnitudeDb(section, 40, 48000)).toBeCloseTo(0, 1);
    expect(maths().sectionMagnitudeDb(section, 16000, 48000)).toBeCloseTo(0, 1);
  });

  it('rolls a highpass off below its corner and passes it above', () => {
    const section = { type: 'highpass', frequency_hz: 100, q: 0.707, gain_db: 0 };
    // A Butterworth corner is -3 dB by definition, then ~12 dB per octave down.
    expect(maths().sectionMagnitudeDb(section, 100, 48000)).toBeCloseTo(-3.01, 2);
    expect(maths().sectionMagnitudeDb(section, 50, 48000)).toBeLessThan(-12);
    expect(maths().sectionMagnitudeDb(section, 20, 48000)).toBeLessThan(-27);
    expect(maths().sectionMagnitudeDb(section, 1000, 48000)).toBeCloseTo(0, 2);
  });

  it('contributes nothing for bypass, none, or a half-typed section', () => {
    for (const type of ['bypass', 'none']) {
      expect(maths().sectionMagnitudeDb({ type, frequency_hz: 1000, q: 1, gain_db: 6 }, 1000, 48000)).toBe(0);
    }
    const halfTyped = { type: 'peaking', frequency_hz: NaN, q: 1, gain_db: 6 };
    expect(maths().sectionMagnitudeDb(halfTyped, 1000, 48000)).toBe(0);
    expect(maths().sectionMagnitudeDb({ type: 'peaking', frequency_hz: 1000, q: 0, gain_db: 6 }, 1000, 48000)).toBe(0);
  });

  it('sums a cascade in dB', () => {
    const lift = { type: 'peaking', frequency_hz: 1000, q: 1, gain_db: 6 };
    const cut = { type: 'peaking', frequency_hz: 1000, q: 1, gain_db: -2 };
    expect(maths().cascadeMagnitudeDb([lift, cut], 1000, 48000)).toBeCloseTo(4, 9);
    expect(maths().cascadeMagnitudeDb([], 1000, 48000)).toBe(0);
  });

  it('draws an empty cascade as a flat line on the 0 dB axis', () => {
    const points = maths().curvePoints([], rect, 48000);
    expect(points.length).toBeGreaterThan(64);
    expect(points.every((point) => point.y === 120)).toBe(true);
    expect(points[0].x).toBe(0);
    expect(points[points.length - 1].x).toBe(960);
  });

  it('round-trips a drag point through the log-frequency and dB axes', () => {
    for (const [hz, db] of [
      [20, 24],
      [120, -4.5],
      [1000, 0],
      [12000, 12],
    ]) {
      const point = maths().pointToEq(rect, maths().hzToX(hz, rect), maths().dbToY(db, rect));
      expect(point.frequency_hz).toBeCloseTo(hz, 6);
      expect(point.gain_db).toBeCloseTo(db, 6);
    }
  });

  it('maps against the rect it is given, not the viewport origin', () => {
    // A client rect is offset by the page scroll and the card's own position.
    const offset = { left: 40, top: 100, width: 960, height: 240 };
    expect(maths().xToHz(40, offset)).toBeCloseTo(20, 9);
    expect(maths().yToDb(100, offset)).toBeCloseTo(24, 9);
    const point = maths().pointToEq(offset, maths().hzToX(1000, offset), maths().dbToY(-6, offset));
    expect(point.frequency_hz).toBeCloseTo(1000, 6);
    expect(point.gain_db).toBeCloseTo(-6, 6);
  });

  it('clamps a drag to the axis bounds, so no reachable pixel is out of range', () => {
    expect(maths().pointToEq(rect, -500, -500)).toEqual({ frequency_hz: 20, gain_db: 24 });
    expect(maths().pointToEq(rect, 5000, 5000)).toEqual({ frequency_hz: 20000, gain_db: -24 });
  });

  it('steps Q multiplicatively and clamps it to the field bounds', () => {
    expect(maths().stepQ(1.2, -100)).toBeCloseTo(1.344, 6);
    expect(maths().stepQ(1, 100)).toBeCloseTo(0.893, 6);
    let q = 1;
    for (let notch = 0; notch < 40; notch++) q = maths().stepQ(q, 100);
    expect(q).toBe(0.1);
    for (let notch = 0; notch < 60; notch++) q = maths().stepQ(q, -100);
    expect(q).toBe(20);
    // An unset Q starts from the section seed rather than from nothing.
    expect(maths().stepQ(NaN, -100)).toBeCloseTo(0.792, 6);
  });

  it('nudges frequency proportionally and clamps it to the plot span', () => {
    expect(maths().nudgeHz(1000, 1)).toBe(1050);
    expect(maths().nudgeHz(1000, -1)).toBe(952);
    // Proportional, so the same press is a musically similar step at any height.
    expect(maths().nudgeHz(40, 1)).toBe(42);
    expect(maths().nudgeHz(20, -1)).toBe(20);
    expect(maths().nudgeHz(20000, 1)).toBe(20000);
    expect(maths().nudgeHz(NaN, 1)).toBe(1050);
  });

  it('places one handle per active section, with a highpass on the 0 dB line', () => {
    const points = maths().handlePoints(
      [
        { slot: 0, type: 'peaking', frequency_hz: 1000, q: 1, gain_db: 6 },
        { slot: 1, type: 'bypass', frequency_hz: 500, q: 1, gain_db: 0 },
      ],
      rect,
    );
    expect(points).toHaveLength(1);
    expect(points[0].slot).toBe(0);
    expect(points[0].x).toBeCloseTo(maths().hzToX(1000, rect), 9);
    expect(points[0].y).toBeCloseTo(maths().dbToY(6, rect), 9);

    // A highpass has no gain, whatever a stale gain field still holds.
    const stale = { slot: 0, type: 'highpass', frequency_hz: 100, q: 0.707, gain_db: 9 };
    const highpass = maths().handlePoints([stale], rect);
    expect(highpass[0].y).toBeCloseTo(maths().dbToY(0, rect), 9);
  });

  it('picks the nearest handle, and only within the grab radius', () => {
    const points = [
      { slot: 0, x: 100, y: 100 },
      { slot: 1, x: 130, y: 100 },
    ];
    expect(maths().pickHandle(points, 104, 100, 14).slot).toBe(0);
    expect(maths().pickHandle(points, 126, 100, 14).slot).toBe(1);
    expect(maths().pickHandle(points, 115, 100, 14)).toBe(null);
    expect(maths().pickHandle([], 100, 100, 14)).toBe(null);
  });

  it('derives the DSP block budget and load from the firmware constants', () => {
    // 600 MHz * 512 frames / 48 kHz, the same 6,400,000 the firmware comments cite.
    expect(maths().dspBudgetCycles(48000)).toBe(6400000);
    expect(maths().dspBudgetCycles(96000)).toBe(3200000);
    expect(maths().dspLoadPercent(41230, 48000)).toBeCloseTo(0.6442, 4);
    expect(maths().dspLoadPercent(6400000, 48000)).toBe(100);
    // An unpublished snapshot reports 0 Hz: no budget, so no number rather than 0 %.
    expect(maths().dspLoadPercent(0, 0)).toBe(null);
    expect(maths().dspLoadPercent(undefined, 48000)).toBe(null);
  });

  it('maps a level to a bar fraction over the strip floor', () => {
    expect(maths().levelFraction(0)).toBe(1);
    expect(maths().levelFraction(-30)).toBeCloseTo(0.5, 9);
    expect(maths().levelFraction(-120)).toBe(0);
    expect(maths().levelFraction(null)).toBe(0);
  });

  it('names the danger zones a green bar used to hide', () => {
    expect(maths().levelZone(-20)).toBe('ok');
    expect(maths().levelZone(-6)).toBe('ok');
    expect(maths().levelZone(-5.9)).toBe('warn');
    expect(maths().levelZone(-1)).toBe('danger');
    expect(maths().levelZone(0)).toBe('danger');
    expect(maths().levelZone(null)).toBe('idle');
    expect(maths().loadZone(70)).toBe('ok');
    expect(maths().loadZone(70.1)).toBe('warn');
    expect(maths().loadZone(90.1)).toBe('danger');
    expect(maths().loadZone(null)).toBe('idle');
  });

  /* A stereo pair is ONE entity on the plot, so the page needs an identity for
   * "these two channels are the same cascade" that matches the wire exactly. */
  it('reduces a cascade to the identity the wire would carry', () => {
    const signature = maths().cascadeSignature;
    // "none" swallows whatever numbers are stranded in the fields.
    expect(signature([{ type: 'none', frequency_hz: 120, q: 1, gain_db: 3 }])).toBe(signature([{ type: 'none' }]));
    // A bypass carries none of its values either.
    expect(signature([{ type: 'bypass', frequency_hz: 5, q: 0 }])).toBe(signature([{ type: 'bypass' }]));
    // A highpass has no gain, so a stale gain field is not a difference.
    const highpass = { type: 'highpass', frequency_hz: 45, q: 0.707 };
    expect(signature([{ ...highpass, gain_db: 9 }])).toBe(signature([{ ...highpass, gain_db: 0 }]));
    // A peaking section's gain IS part of its identity.
    const peaking = { type: 'peaking', frequency_hz: 120, q: 1.2 };
    expect(signature([{ ...peaking, gain_db: -4.5 }])).not.toBe(signature([{ ...peaking, gain_db: -4 }]));
    // Slot order matters: two sections swapped are a different cascade.
    expect(signature([{ type: 'none' }, { ...peaking, gain_db: 1 }])).not.toBe(
      signature([{ ...peaking, gain_db: 1 }, { type: 'none' }]),
    );
  });

  it('resolves a pair to its left member, and flags a divergent one as unlinked', () => {
    const left = [{ type: 'peaking', frequency_hz: 120, q: 1.2, gain_db: -4.5 }, { type: 'none' }];
    const same = [{ type: 'peaking', frequency_hz: 120, q: 1.2, gain_db: -4.5 }, { type: 'none' }];
    const different = [
      { type: 'peaking', frequency_hz: 120, q: 1.2, gain_db: -4.5 },
      { type: 'highpass', frequency_hz: 45, q: 0.707, gain_db: 0 },
    ];

    expect(maths().resolveCascade([left, same])).toEqual({ sections: left, unlinked: false });
    expect(maths().resolveCascade([left, different]).sections).toBe(left);
    expect(maths().resolveCascade([left, different]).unlinked).toBe(true);
    // A single-channel entity is never unlinked, and an empty one is flat.
    expect(maths().resolveCascade([left]).unlinked).toBe(false);
    expect(maths().resolveCascade([])).toEqual({ sections: [], unlinked: false });
  });
});

describe('Aius SD DSP editor — the stacked EQ plot', () => {
  it('draws every cascade on one plot, with the selected one emphasised', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });

    const cascades = cascadePaths();
    expect(cascades).toHaveLength(ENTITIES.length);
    // One colour per entity: the legend is the key to the plot.
    expect(new Set(cascades.map((path) => strokeOf(path)[1])).size).toBe(ENTITIES.length);
    // Exactly one is emphasised; the other four are dimmed context.
    expect(dimmedCascades()).toHaveLength(ENTITIES.length - 1);
    expect(selectedCascade()).toBeDefined();

    // Front is selected by default and shows fl's single peaking section.
    expect(chip('front').getAttribute('aria-pressed')).toBe('true');
    expect(chip('output').getAttribute('aria-pressed')).toBe('false');
    const handles = handlesOf('curve');
    expect(handles).toHaveLength(1);
    expect(handles[0][1]).toBeCloseTo(atHz(120), 6);
    expect(handles[0][2]).toBeCloseTo(atDb(-4.5), 6);
  });

  it('has one chip per entity, and a stereo pair is one of them', async () => {
    await boot();
    expect([...el('curve-legend').querySelectorAll('button')].map((node) => node.dataset.entity)).toEqual(ENTITIES);
    expect(chip('front').textContent).toContain('Front (fl+fr)');
    expect(chip('surround').textContent).toContain('Surround (sl+sr)');
  });

  it('moves the handles and the emphasis when another chip is picked', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });

    chip('output').click();
    await settle();

    expect(chip('output').getAttribute('aria-pressed')).toBe('true');
    expect(chip('front').getAttribute('aria-pressed')).toBe('false');
    const handles = handlesOf('curve');
    expect(handles).toHaveLength(1);
    expect(handles[0][1]).toBeCloseTo(atHz(4000), 6);

    // The centre channel's highpass rides the 0 dB line.
    chip('centre').click();
    await settle();
    const centre = handlesOf('curve');
    expect(centre).toHaveLength(1);
    expect(centre[0][1]).toBeCloseTo(atHz(80), 6);
    expect(centre[0][2]).toBeCloseTo(atDb(0), 6);
  });

  it('strokes the selected cascade where its section sits', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    chip('output').click();
    await settle();

    const curve = vertices(selectedCascade());
    expect(curve.length).toBeGreaterThan(64);
    const top = curve.reduce((best, op) => (op[2] < best[2] ? op : best));
    expect(Math.abs(top[1] - atHz(4000))).toBeLessThan(6);
    expect(maths().yToDb(top[2], CURVE_RECT)).toBeCloseTo(2, 2);
  });

  it('redraws from the form, so a typed edit and a removed section both show', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });

    await setNumber('input_channels.fl.eq.0.frequency_hz', '4000');
    expect(handlesOf('curve')[0][1]).toBeCloseTo(atHz(4000), 6);

    await setChoice('input_channels.fl.eq.0.type', 'none');
    expect(handlesOf('curve')).toHaveLength(0);
    expect(el('curve-caption').textContent).toContain('no sections');
    expect(el('curve-caption').textContent).toContain('double-click');
  });

  it('says which section the keyboard and the wheel are pointed at', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    expect(el('curve-caption').textContent).toContain('section 1 of 2 selected');
    expect(el('curve-caption').textContent).toContain('Front (fl+fr)');
  });
});

describe('Aius SD DSP editor — editing through the plot', () => {
  it('writes a dragged handle into the same fields text mode edits', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();
    el('token').value = '0123456789abcdef';

    pointer('pointerdown', atHz(120), atDb(-4.5));
    pointer('pointermove', atHz(300), atDb(6));
    pointer('pointerup', atHz(300), atDb(6));
    await settle();

    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('300');
    expect(valueOf('input_channels.fl.eq.0.gain_db')).toBe('6');
    // A drag runs the same validation pass a typed edit does.
    expect(errorOf('input_channels.fl.eq.0.frequency_hz')).toBe('');
    expect(fieldOf('input_channels.fl.eq.0.frequency_hz').getAttribute('aria-invalid')).toBe('false');

    await click('apply');
    expect(JSON.parse(putCalls()[0].body).input_channels.fl.eq[0]).toEqual({
      type: 'peaking',
      frequency_hz: 300,
      q: 1.2,
      gain_db: 6,
    });
  });

  /* A pair is one entity here by contract, so one gesture writes both members. */
  it('writes both members of a stereo pair from one gesture', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();

    pointer('pointerdown', atHz(120), atDb(-4.5));
    pointer('pointermove', atHz(300), atDb(6));
    pointer('pointerup', atHz(300), atDb(6));
    await settle();

    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('300');
    expect(valueOf('input_channels.fr.eq.0.frequency_hz')).toBe('300');
    expect(valueOf('input_channels.fl.eq.0.gain_db')).toBe('6');
    expect(valueOf('input_channels.fr.eq.0.gain_db')).toBe('6');
  });

  /* fl carries one section and fr two: the pair is UNLINKED, and the first
   * surface edit adopts the LEFT member into both -- announced first. */
  it('announces adopting the left member before the first edit of an unlinked pair', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();
    expect(el('unlinked:front').hidden).toBe(false);
    expect(valueOf('input_channels.fr.eq.1.type')).toBe('highpass');

    pointer('pointerdown', atHz(120), atDb(-4.5));
    pointer('pointermove', atHz(300), atDb(6));
    pointer('pointerup', atHz(300), atDb(6));
    await settle();

    expect(el('curve-status').textContent).toContain('Front (fl+fr) members differ');
    expect(el('curve-status').textContent).toContain("adopting fl's cascade into fr");
    // fl's cascade is now both members', so the pair is linked again...
    expect(valueOf('input_channels.fr.eq.1.type')).toBe('none');
    expect(el('unlinked:front').hidden).toBe(true);
    // ...and the edit itself landed on both.
    expect(valueOf('input_channels.fr.eq.0.frequency_hz')).toBe('300');
  });

  it('leaves a divergent pair alone until the surface is used', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    // sl carries a bypass section and sr none: unlinked, and untouched.
    expect(el('unlinked:surround').hidden).toBe(false);
    expect(valueOf('input_channels.sl.eq.0.type')).toBe('bypass');
    expect(valueOf('input_channels.sr.eq.0.type')).toBe('none');
    expect(el('unlinked:centre').hidden).toBe(true);
  });

  it('moves a highpass handle in frequency only -- it has no gain to drag', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();
    chip('centre').click();
    await settle();

    // fc slot 0 is a highpass at 80 Hz, so its handle rides the 0 dB line.
    pointer('pointerdown', atHz(80), atDb(0));
    pointer('pointermove', atHz(200), atDb(12));
    await settle();

    expect(valueOf('input_channels.fc.eq.0.frequency_hz')).toBe('200');
    expect(valueOf('input_channels.fc.eq.0.gain_db')).toBe('0');
    expect(fieldOf('input_channels.fc.eq.0.gain_db').disabled).toBe(true);
  });

  it('ignores a press away from a handle, and a move after the pointer is up', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();

    pointer('pointerdown', atHz(8000), atDb(20));
    pointer('pointermove', atHz(300), atDb(6));
    await settle();
    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('120');
    expect(el('apply-state').textContent).toBe('loaded');

    // A press that never moves must not nudge the section it grabbed either.
    pointer('pointerdown', atHz(120), atDb(-4.5));
    pointer('pointerup', atHz(120), atDb(-4.5));
    pointer('pointermove', atHz(9000), atDb(6));
    await settle();
    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('120');
    expect(valueOf('input_channels.fl.eq.0.gain_db')).toBe('-4.5');
  });

  it('selects the section it grabbed, so the keyboard follows the pointer', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();
    chip('front').click();
    await settle();

    // Add a second section, then grab the first one back.
    dblclick(atHz(6000), atDb(0));
    await settle();
    expect(el('curve-caption').textContent).toContain('section 2 of 2 selected');

    pointer('pointerdown', atHz(120), atDb(-4.5));
    pointer('pointerup', atHz(120), atDb(-4.5));
    await settle();
    expect(el('curve-caption').textContent).toContain('section 1 of 2 selected');
  });

  it('shows the values a drag is writing, beside the handle writing them', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();

    expect(el('curve-readout').hidden).toBe(true);
    pointer('pointerdown', atHz(120), atDb(-4.5));
    pointer('pointermove', atHz(1713), atDb(7.3));
    await settle();

    expect(el('curve-readout').hidden).toBe(false);
    expect(el('curve-readout').textContent).toBe('1713 Hz +7.3 dB Q 1.20');
    expect(el('curve-readout').style.left).not.toBe('');

    pointer('pointerup', atHz(1713), atDb(7.3));
    await settle();
    expect(el('curve-readout').hidden).toBe(true);
  });

  /* An unconditional touch-action:none made the plot a scroll trap on touch
   * (UX finding E1): panning is suppressed for the duration of a grab only. */
  it('suppresses panning only while a handle is actually held', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();
    expect(el('curve').style.touchAction).toBe('');

    pointer('pointerdown', atHz(120), atDb(-4.5));
    expect(el('curve').style.touchAction).toBe('none');

    pointer('pointerup', atHz(120), atDb(-4.5));
    expect(el('curve').style.touchAction).toBe('');
  });

  it('offers a pointer cursor within grab range and a crosshair elsewhere', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();

    pointer('pointermove', atHz(120), atDb(-4.5));
    expect(el('curve').style.cursor).toBe('pointer');

    pointer('pointermove', atHz(9000), atDb(20));
    expect(el('curve').style.cursor).toBe('');
  });

  /* The wheel used to hijack the page's scroll and silently edit Q from 14 px
   * away (UX finding E3). */
  it('takes the wheel for Q only while a handle is held or the plot has focus', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();
    const x = atHz(120);
    const y = atDb(-4.5);

    // Not grabbed, not focused: the wheel belongs to the page.
    expect(wheel(x, y, -100)).toBe(true);
    await settle();
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('1.2');

    // Grabbed.
    pointer('pointerdown', x, y);
    expect(wheel(x, y, -100)).toBe(false);
    await settle();
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('1.344');
    expect(valueOf('input_channels.fr.eq.0.q')).toBe('1.344');
    pointer('pointerup', x, y);

    // Focused, and now anywhere on the plot -- the section is the selected one.
    el('curve').focus();
    wheel(500, 10, 100);
    await settle();
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('1.2');
  });

  it('clamps a wheeled Q to the field bounds', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();
    el('curve').focus();

    for (let notch = 0; notch < 40; notch++) wheel(500, 10, 100);
    await settle();
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('0.1');
    expect(errorOf('input_channels.fl.eq.0.q')).toBe('');

    for (let notch = 0; notch < 60; notch++) wheel(500, 10, -100);
    await settle();
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('20');
  });

  it('adopts an unlinked pair before a wheeled Q lands on it', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();
    el('curve').focus();
    expect(el('unlinked:front').hidden).toBe(false);

    wheel(500, 10, -100);
    await settle();

    expect(el('curve-status').textContent).toContain("adopting fl's cascade into fr");
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('1.344');
    expect(valueOf('input_channels.fr.eq.0.q')).toBe('1.344');
    expect(el('unlinked:front').hidden).toBe(true);
  });

  it('nudges the selected section with the arrow keys', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });

    key('ArrowRight');
    await settle();
    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('126');
    expect(valueOf('input_channels.fr.eq.0.frequency_hz')).toBe('126');

    key('ArrowLeft');
    await settle();
    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('120');

    key('ArrowUp');
    await settle();
    expect(valueOf('input_channels.fl.eq.0.gain_db')).toBe('-4');
    key('ArrowDown');
    key('ArrowDown');
    await settle();
    expect(valueOf('input_channels.fl.eq.0.gain_db')).toBe('-5');
    expect(el('curve-readout').textContent).toContain('-5.0 dB');
  });

  it('leaves a highpass gain alone under the arrow keys', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    chip('centre').click();
    await settle();

    key('ArrowUp');
    await settle();
    expect(valueOf('input_channels.fc.eq.0.gain_db')).toBe('0');
    key('ArrowRight');
    await settle();
    expect(valueOf('input_channels.fc.eq.0.frequency_hz')).toBe('84');
  });

  it('cycles the selected section from the keyboard', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    expect(el('curve-caption').textContent).toContain('section 1 of 2');

    key('PageDown');
    await settle();
    expect(el('curve-caption').textContent).toContain('section 2 of 2');

    key('PageUp');
    await settle();
    expect(el('curve-caption').textContent).toContain('section 1 of 2');
  });

  it('does not drag when the DOM has no layout to map against', async () => {
    // The unstubbed happy-dom rect is all zeroes; a real browser is proven by
    // slice 4's on-device pass, never by synthetic dispatch.
    await boot({ config: fixture('dsp-config-sd.json') });

    pointer('pointerdown', 249, 142);
    pointer('pointermove', 400, 60);
    await settle();

    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('120');
  });
});

describe('Aius SD DSP editor — adding sections from the plot', () => {
  it('adds a seeded peaking section where the plot is double-clicked', async () => {
    await boot();
    stubRect();

    dblclick(atHz(2500), atDb(0));
    await settle();

    expect(valueOf('input_channels.fl.eq.0.type')).toBe('peaking');
    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('2500');
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('0.707');
    expect(valueOf('input_channels.fl.eq.0.gain_db')).toBe('0');
    // Both members of the pair, because the pair is one entity here.
    expect(valueOf('input_channels.fr.eq.0.frequency_hz')).toBe('2500');
    expect(el('curve-status').textContent).toContain('Added a peaking section at 2500 Hz');
    expect(handlesOf('curve')).toHaveLength(1);
  });

  it('adds one for keyboard users too, from the visible button', async () => {
    await boot();
    chip('output').click();
    await settle();

    await click('curve-add');

    expect(valueOf('output.eq.0.type')).toBe('peaking');
    expect(valueOf('output.eq.0.frequency_hz')).toBe('1000');
    expect(el('curve-caption')).toBeDefined();
    expect(el('curve-caption').textContent).toContain('section 1 of 2 selected');
  });

  /* The 2-per-channel cap is the wire model's (AIUS_DSP_MODEL_EQ_MAX). */
  it('says so at the cap instead of silently doing nothing', async () => {
    await boot();
    stubRect();

    dblclick(atHz(500), atDb(0));
    await settle();
    dblclick(atHz(5000), atDb(0));
    await settle();
    expect(handlesOf('curve')).toHaveLength(2);

    dblclick(atHz(9000), atDb(12));
    await settle();

    expect(handlesOf('curve')).toHaveLength(2);
    expect(el('curve-status').textContent).toContain('already uses both of its 2 sections');
    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('500');
    expect(valueOf('input_channels.fl.eq.1.frequency_hz')).toBe('5000');
  });

  it('treats a double-click on a handle as handle fiddling, not an add', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();

    dblclick(atHz(120), atDb(-4.5));
    await settle();

    expect(valueOf('input_channels.fl.eq.1.type')).toBe('none');
  });

  it('adopts an unlinked pair before adding to it', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubRect();

    dblclick(atHz(6000), atDb(0));
    await settle();

    expect(el('curve-status')).toBeDefined();
    // fr's stray highpass went with the adopt, and the new section is on both.
    expect(valueOf('input_channels.fl.eq.1.frequency_hz')).toBe('6000');
    expect(valueOf('input_channels.fr.eq.1.frequency_hz')).toBe('6000');
    expect(el('unlinked:front').hidden).toBe(true);
  });

  it('removes the active section from BOTH pair members via the visible button', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    el('token').value = '0123456789abcdef';
    stubRect();

    // fl.eq.0 is the SD fixture's 120 Hz peaking; it is the active section.
    await click('curve-remove');

    expect(valueOf('input_channels.fl.eq.0.type')).toBe('none');
    expect(valueOf('input_channels.fr.eq.0.type')).toBe('none');
    expect(el('curve-status').textContent).toContain("Removed Front (fl+fr)'s section 1 (peaking at 120 Hz)");
    expect(el('curve-status').textContent).toContain('free again');

    // Removal IS the wire's removal: the slot is gone from the PUT body.
    await click('apply');
    const body = JSON.parse(putCalls()[0].body);
    expect(body.input_channels.fl.eq).toEqual([]);
    expect(body.input_channels.fr.eq).toEqual([]);
  });

  it('removes via the Delete key on the focused plot, freeing the slot for a new add', async () => {
    await boot();
    stubRect();

    dblclick(atHz(500), atDb(0));
    await settle();
    dblclick(atHz(5000), atDb(0));
    await settle();
    expect(handlesOf('curve')).toHaveLength(2);

    key('Delete');
    await settle();

    expect(handlesOf('curve')).toHaveLength(1);
    dblclick(atHz(9000), atDb(0));
    await settle();
    expect(handlesOf('curve')).toHaveLength(2);
    expect(el('curve-status').textContent).toContain('Added a peaking section at 9000 Hz');
  });

  it('says so when there is no section to remove', async () => {
    await boot();
    await click('curve-remove');

    expect(el('curve-status').textContent).toContain('has no section 1 to remove');
    expect(valueOf('input_channels.fl.eq.0.type')).toBe('none');
  });
});

describe('Aius SD DSP editor — canvas honesty', () => {
  it('sizes the backing store to the laid-out width and the device pixel ratio', async () => {
    await boot();
    const canvas = el('curve');
    expect([canvas.width, canvas.height]).toEqual([960, 240]);

    Object.defineProperty(canvas, 'clientWidth', { value: 500, configurable: true });
    Object.defineProperty(window, 'devicePixelRatio', { value: 2, configurable: true });
    try {
      window.dispatchEvent(new window.Event('resize'));
      await settle();
      // 500 CSS px at 2x, keeping the authored 4:1 aspect.
      expect([canvas.width, canvas.height]).toEqual([1000, 250]);
      // And the axis text is scaled with it, rather than shrinking to 3.6 px.
      expect(lastFrame('curve').some((op) => op[0] === 'fillText')).toBe(true);
    } finally {
      Object.defineProperty(window, 'devicePixelRatio', { value: 1, configurable: true });
    }
  });

  it('labels both frequency rails inside the plot, clear of the dB rails', async () => {
    await boot();
    const labels = labelsOf('curve');
    const hzLabels = labels.filter((op) => /^\d+k?$/.test(op[1]));
    const dbLabels = labels.filter((op) => /^[+-]\d+$/.test(op[1]));

    // "20k" used to be drawn off the right edge; it is right-aligned inside now.
    const top = hzLabels.find((op) => op[1] === '20k');
    expect(top[4]).toBe('right');
    expect(top[2]).toBeLessThanOrEqual(960);
    expect(top[2]).toBeGreaterThan(900);

    // "-24" and "20" used to collide into "204" in the bottom-left corner.
    const floor = hzLabels.find((op) => op[1] === '20');
    const rail = dbLabels.find((op) => op[1] === '-24');
    expect(floor[2]).toBeGreaterThan(rail[2] + 20);
    // "+24" used to be drawn above the top edge; it hangs below its rail now.
    const ceiling = dbLabels.find((op) => op[1] === '+24');
    expect(ceiling[3]).toBeGreaterThan(0);
    expect(rail[3]).toBeLessThan(240);
  });
});

describe('Aius SD DSP editor — DSP load, level bars and the limiter strip', () => {
  /* The last frame's first path is the 0 dB rail; the history is the second. */
  const historyPoints = () => {
    const paths = pathsOf('gr-history');
    return paths.length < 2 ? [] : vertices(paths[1]);
  };

  it('renders load and headroom against the per-block cycle budget', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);

    // 41230 / 6,400,000 cycles per 512-frame block at 48 kHz, in ONE phrasing.
    expect(el('meter-load').textContent).toBe('0.6 % of budget, 99.4 % headroom');
    expect(el('meter-load-peak').textContent).toBe('0.9 % of budget');
    expect(el('meter-load').dataset.zone).toBe('ok');
  });

  it('renders a heavy block as a load approaching the budget, and says it is heavy', async () => {
    const busy = { ...fixture('meters-snapshot.json'), dsp_cycles_last: 5600000, dsp_cycles_max: 6400000 };
    await boot({ meters: busy });
    await vi.advanceTimersByTimeAsync(100);

    expect(el('meter-load').textContent).toBe('87.5 % of budget, 12.5 % headroom');
    expect(el('meter-load-peak').textContent).toBe('100.0 % of budget');
    expect(el('meter-load').dataset.zone).toBe('warn');
    expect(el('meter-load-peak').dataset.zone).toBe('danger');
  });

  it('fills the level bars from the same snapshot as the table', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);

    expect(el('bar-in-fl-value').textContent).toBe('-6.2');
    expect(el('bar-in-fl-fill').style.width).toBe('89.7%');
    expect(el('bar-in-lfe-value').textContent).toBe('-30.0');
    expect(el('bar-in-lfe-fill').style.width).toBe('50.0%');
    expect(el('bar-out-l-value').textContent).toBe('-3.1');
    expect(el('bar-out-r-value').textContent).toBe('-3.4');
    expect(el('strip-input-state').textContent).toBe('live');
    expect(el('strip-output-state').textContent).toBe('live');
  });

  it('marks a bar that is close to clipping', async () => {
    await boot({ meters: fixture('meters-silent.json') });
    await vi.advanceTimersByTimeAsync(100);

    // fr sits at -0.1 dBFS: a hair from full scale, not a healthy green.
    expect(el('bar-in-fr-fill').dataset.zone).toBe('danger');
    expect(el('bar-in-fl-fill').dataset.zone).toBe('ok');
  });

  it('accumulates the limiter history across polls and caps the ring', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(300);

    expect(el('gr-value').textContent).toBe('-1.8 dB');
    expect(el('gr-fill').style.width).toBe('7.5%');
    expect(historyPoints()).toHaveLength(3);

    await vi.advanceTimersByTimeAsync(200);
    expect(historyPoints()).toHaveLength(5);

    // 130 polls in, the strip still shows the last 128 published instants.
    await vi.advanceTimersByTimeAsync(12500);
    expect(historyPoints()).toHaveLength(128);
  });

  it('renders a fully closed limiter at the bar rail, not off the scale', async () => {
    await boot({ meters: fixture('meters-silent.json') });
    await vi.advanceTimersByTimeAsync(100);

    // The defensive fully-closed value is below the -120 dBFS floor.
    expect(el('gr-value').textContent).toBe('-inf dB');
    expect(el('gr-fill').style.width).toBe('100.0%');
    expect(historyPoints()).toHaveLength(1);
  });

  it('clears the strip for a never-published snapshot', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(300);
    expect(historyPoints()).toHaveLength(3);

    device.meters = fixture('meters-idle.json');
    await vi.advanceTimersByTimeAsync(100);

    expect(el('strip-limiter-state').textContent).toBe('no data');
    expect(el('meter-load').textContent).toBe('--');
    expect(el('meter-load-peak').textContent).toBe('--');
    expect(el('gr-value').textContent).toBe('--');
    expect(el('gr-fill').style.width).toBe('0.0%');
    expect(el('bar-in-fl-value').textContent).toBe('--');
    expect(el('bar-in-fl-fill').style.width).toBe('0.0%');
    expect(historyPoints()).toHaveLength(0);
  });

  /* The meter pill used to stamp the whole Signal-flow card, curves included
   * (UX finding E5): the plot is a view of the FORM and stays live. */
  it('scopes the meter state to the blocks that read a snapshot', async () => {
    await boot({ config: fixture('dsp-config-sd.json'), meters: null });
    await vi.advanceTimersByTimeAsync(100);

    expect(el('strip-input-state').textContent).toBe('unreachable');
    expect(el('strip-limiter-state').textContent).toBe('unreachable');
    expect(el('meter-state').textContent).toBe('unreachable');
    expect(el('bar-out-l-value').textContent).toBe('--');
    expect(historyPoints()).toHaveLength(0);

    // The curves are unaffected: they never read a snapshot.
    expect(handlesOf('curve')).toHaveLength(1);
    expect(el('apply-state').textContent).toBe('loaded');
  });
});
