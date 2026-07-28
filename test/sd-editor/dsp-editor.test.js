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
 *
 * The analyser's `analysis_window` is the one stimulus NOT carried as a JSON
 * fixture: it is synthesised per test as two bin-centred tones at stated
 * amplitudes, because 1024 checked-in integers would be an unverifiable wall of
 * numbers while `sin(2*pi*32*n/1024) / 2` says exactly what the expected bins
 * are and why.
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
    /* Dashing is drawing STATE, like the stroke style: the routing diagram says
     * "muted" with it, so a stroke records the dash pattern it was issued
     * under. */
    let lineDash = [];
    const record =
      (name) =>
      (...args) =>
        ops.push([name, ...args]);
    context.clearRect = record('clearRect');
    context.beginPath = record('beginPath');
    context.closePath = record('closePath');
    context.moveTo = record('moveTo');
    context.lineTo = record('lineTo');
    /* A rect carries its fill state the way a stroke does: the analyser underlay
     * is identified by being FAINT, which is drawing state and not geometry. */
    context.fillRect = (...args) => ops.push(['fillRect', ...args, context.fillStyle, context.globalAlpha]);
    context.setLineDash = (segments) => {
      lineDash = segments;
      ops.push(['setLineDash', segments]);
    };
    context.arc = (...args) => ops.push(['arc', ...args, context.fillStyle, context.lineWidth]);
    context.fillText = (text, x, y) => ops.push(['fillText', text, x, y, context.textAlign]);
    context.stroke = () => ops.push(['stroke', context.strokeStyle, context.lineWidth, context.globalAlpha, lineDash]);
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

/* Saving a preset to a FILE is a browser action, so the only observable is the
 * anchor the page clicks: the object URL and the `download` filename. The URL
 * factory is RECORDED rather than mocked away, so a test can read the exact
 * bytes handed over (and check the page revoked the URL afterwards). The
 * anchor's click is intercepted only when it carries a download attribute --
 * the error summary's own links stay real, because they rely on happy-dom's
 * navigation-on-click and their handler's preventDefault to suppress it. */
const objectUrls = [];
const downloads = [];
URL.createObjectURL = (blob) => {
  const url = `blob:aius/${objectUrls.length}`;
  objectUrls.push({ url, blob, revoked: false });
  return url;
};
URL.revokeObjectURL = (url) => {
  const entry = objectUrls.find((candidate) => candidate.url === url);
  if (entry) entry.revoked = true;
};
const nativeAnchorClick = window.HTMLAnchorElement.prototype.click;
window.HTMLAnchorElement.prototype.click = function click() {
  if (!this.hasAttribute('download')) return nativeAnchorClick.call(this);
  downloads.push({ name: this.getAttribute('download'), url: this.getAttribute('href') });
};
const downloadBody = async (index = 0) =>
  JSON.parse(await objectUrls.find((entry) => entry.url === downloads[index].url).blob.text());

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

async function setCheck(path, on) {
  const input = fieldOf(path);
  input.checked = on;
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

/* The routing diagram gets the same treatment as the plot: its own explicit
 * client rect (matching its 960x300 backing store, so drawn and client
 * coordinates are the same numbers) and its own event helpers. */
const MATRIX_RECT = { left: 0, top: 0, width: 960, height: 300, right: 960, bottom: 300, x: 0, y: 0 };
const stubMatrixRect = () => {
  el('matrix-diagram').getBoundingClientRect = () => MATRIX_RECT;
};
/* The factory model's matrix, as the page reads it back out of the form. */
const matrixValues = (over = {}) => ({
  center: 0.7071067811865475,
  surround: 0.7071067811865475,
  lfe: 0.31622776601683794,
  headroom: 0.5,
  clipfree: false,
  crossoverHz: 120,
  ...over,
});
const matrixHandle = (key, values = matrixValues()) =>
  maths()
    .matrixHandlePoints(values, MATRIX_RECT)
    .find((handle) => handle.key === key);
const atGain = (key, value) => maths().matrixValueToY(value, MATRIX_RECT, matrixHandle(key).bound);
const matrixPointer = (type, clientX, clientY, extra = {}) =>
  el('matrix-diagram').dispatchEvent(
    new window.PointerEvent(type, { clientX, clientY, pointerId: 2, bubbles: true, cancelable: true, ...extra }),
  );
const matrixWheel = (clientX, clientY, deltaY) => {
  const event = new window.WheelEvent('wheel', { deltaY, bubbles: true, cancelable: true });
  return el('matrix-diagram').dispatchEvent(Object.assign(event, { clientX, clientY }));
};
const matrixKey = (name) =>
  el('matrix-diagram').dispatchEvent(new window.KeyboardEvent('keydown', { key: name, bubbles: true, cancelable: true }));
const matrixDblclick = (clientX, clientY) =>
  el('matrix-diagram').dispatchEvent(
    new window.MouseEvent('dblclick', { clientX, clientY, bubbles: true, cancelable: true }),
  );
/* The eight downmix paths in the last frame: the column tracks are one path of
 * eight vertices and a handle is an arc, so an edge is a group of two (a unity
 * reference) or three (a path bent through its gain handle). */
const matrixEdgePaths = () =>
  pathsOf('matrix-diagram').filter((path) => vertices(path).length === 2 || vertices(path).length === 3);

beforeEach(() => {
  rejections.length = 0;
  objectUrls.length = 0;
  downloads.length = 0;
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
    expect(el('dirty-state').hidden).toBe(true);
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
    // The factory model ships layout-aware headroom ON: the 0.5 pad is the
    // summing-layout pad, so stereo pass-through runs at source level.
    expect(fieldOf('matrix.layout_aware').checked).toBe(true);
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
    // This fixture has the layout-aware rule explicitly OFF -- the diagnostic
    // setting, in which the pad applies to every layout including mono.
    expect(fieldOf('matrix.layout_aware').checked).toBe(false);
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
    // The EDITOR's working range, narrower than the device's own +-24 dB.
    const gain = fieldOf('input_channels.fl.eq.0.gain_db');
    expect([gain.min, gain.max, gain.step]).toEqual(['-12', '12', '0.5']);
    const threshold = fieldOf('limiter.threshold_dbfs');
    expect([threshold.min, threshold.max, threshold.step]).toEqual(['-24', '0', '0.5']);
    const headroom = fieldOf('matrix.headroom_linear');
    expect([headroom.min, headroom.max, headroom.step]).toEqual(['0', '1', '0.01']);

    // step="any" is what took ArrowUp from Q 0.707 to 1.707.
    const numbers = [...document.querySelectorAll('input[type="number"]')];
    expect(numbers.length).toBeGreaterThan(50);
    expect(numbers.every((input) => input.step !== 'any' && input.min !== '' && input.max !== '')).toBe(true);
  });

  it('associates every caption and error with its own control, and keeps every reset out of the tab order', async () => {
    await boot();

    const caption = [...document.querySelectorAll('.field__label')].find(
      (label) => label.htmlFor === 'f:limiter.attack_ms',
    );
    expect(caption.textContent).toBe('Attack (ms)');
    expect(fieldOf('limiter.attack_ms').getAttribute('aria-describedby')).toBe('e:limiter.attack_ms');
    // A wrapping <label> folded caption + error + button into one accessible name.
    expect(fieldOf('limiter.attack_ms').closest('label')).toBe(null);

    // Tabular cells carry no <label for> -- the header row is the visual
    // caption -- so each input names itself in full for AT.
    expect(fieldOf('input_channels.fl.eq.0.frequency_hz').getAttribute('aria-label')).toBe(
      'Front left section 1 frequency',
    );
    expect(fieldOf('input_channels.sr.gain_db').getAttribute('aria-label')).toBe('Surround right trim');
    expect(fieldOf('input_channels.fl.eq.0.frequency_hz').getAttribute('aria-describedby')).toBe(
      'e:input_channels.fl.eq.0.frequency_hz',
    );

    // 23 per-field resets (features, trims, matrix incl. layout_aware, limiter)
    // + 14 per-section ones (6 channels x 2 + output x 2), all reachable by AT
    // but out of the tab order.
    const resets = [...document.querySelectorAll('.field__reset')];
    expect(resets.length).toBe(37);
    expect(resets.every((button) => button.tabIndex === -1)).toBe(true);
    expect(document.querySelectorAll('.eq-table__reset').length).toBe(14);
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
    // Every panel's own Apply is in the same one-in-flight discipline: a
    // section write during another write would carry a stale base_generation.
    expect(el('apply:matrix').disabled).toBe(true);

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

  /* The editor offers +-12 dB (operator request 2026-07-28); the firmware
   * accepts +-24 (EQ_GAIN_{MIN,MAX}_DB). Those are two different questions, so
   * they are two different bounds: the NATIVE one constrains what this surface
   * can produce, the VALIDATED one is the device's and is what a message
   * describes. A config already carrying a wider gain therefore round-trips
   * untouched instead of being refused by the page that just rendered it. */
  it('renders and applies a gain wider than its own input range', async () => {
    const wide = fixture('dsp-config-sd.json');
    wide.input_channels.fl.eq[0].gain_db = -18;
    wide.input_channels.fr.eq[0].gain_db = -18;
    await boot({ config: wide });
    el('token').value = '0123456789abcdef';

    expect(valueOf('input_channels.fl.eq.0.gain_db')).toBe('-18');
    expect(errorOf('input_channels.fl.eq.0.gain_db')).toBe('');
    expect(el('apply-state').textContent).toBe('loaded');

    await click('apply');
    expect(putCalls()).toHaveLength(1);
    expect(JSON.parse(putCalls()[0].body).input_channels.fl.eq[0].gain_db).toBe(-18);
  });

  it('constrains new gain input to +-12 while validating against the device +-24', async () => {
    await boot();
    await setChoice('input_channels.fl.eq.0.type', 'peaking');

    // The rails a spinner, a drag or an arrow press may reach.
    const gain = fieldOf('input_channels.fl.eq.0.gain_db');
    expect([gain.min, gain.max]).toEqual(['-12', '12']);

    // Outside those rails but inside the firmware's: not something this
    // surface would produce, but not an error either.
    await setNumber('input_channels.fl.eq.0.gain_db', '18');
    expect(errorOf('input_channels.fl.eq.0.gain_db')).toBe('');

    await setNumber('input_channels.fl.eq.0.gain_db', '24.5');
    expect(errorOf('input_channels.fl.eq.0.gain_db')).toBe(
      'Front left section 1 gain must be between -24 and 24 dB.',
    );
  });

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
    // A card with nothing wrong in it stays unmarked.
    expect(el('m:card-input-channels').hidden).toBe(true);

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

/* Each processing stage is a PANEL (operator request 2026-07-28): the feature
 * flag that governs it, its own dirty marker and its own Apply all ride the
 * head of the card that holds its controls, so a 4000 px page never sends the
 * operator back to the top to commit one change. */
describe('Aius SD DSP editor — stage panels', () => {
  const SECTION_CARDS = [
    ['input_eq', 'card-input-channels'],
    ['matrix', 'card-matrix'],
    ['output_eq', 'card-output-eq'],
    ['limiter', 'card-limiter'],
    ['meters', 'card-meters'],
  ];
  const factory = () => fixture('dsp-config-default.json');

  it('mounts every stage enable, dirty marker and Apply in the head of the panel it governs', async () => {
    await boot();

    for (const [key, card] of SECTION_CARDS) {
      const head = el(card).querySelector('.section-title');
      expect(head.contains(fieldOf(`features.${key}`))).toBe(true);
      expect(head.contains(el(`dirty:${key}`))).toBe(true);
      expect(head.contains(el(`apply:${key}`))).toBe(true);
    }
    // The cluster they came from is gone, not merely emptied.
    expect(el('card-features')).toBe(null);
    expect(el('features')).toBe(null);
  });

  it('round-trips a stage enable through features.* in the PUT body', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    el('token').value = '0123456789abcdef';
    // The SD fixture ships output_eq off, and the box in that panel says so.
    expect(fieldOf('features.output_eq').checked).toBe(false);

    await setCheck('features.output_eq', true);
    await setCheck('features.limiter', false);
    await click('apply');

    expect(JSON.parse(putCalls()[0].body).features).toEqual({
      input_eq: true,
      matrix: true,
      output_eq: true,
      limiter: false,
      meters: true,
    });
  });

  /* A bypassed stage is still parsed, bounds-checked and STORED by the device,
   * and an operator sets a stage up before switching it on -- so "off" is a
   * dimmed panel and a note, never `disabled` on the inputs. Disabling them
   * would take them out of fieldProblem()'s reach (it skips a disabled input,
   * which is how a bypassed EQ section's unused fields stop being validated)
   * and a bad value would reach the device as a 400 instead of a message. */
  it('dims a bypassed panel and says what stopped, without disabling anything', async () => {
    await boot();
    expect(el('card-limiter').dataset.stage).toBe(undefined);
    expect(el('off:limiter').textContent).toBe('');

    await setCheck('features.limiter', false);
    expect(el('card-limiter').dataset.stage).toBe('off');
    expect(el('off:limiter').textContent).toBe('bypassed');

    expect(fieldOf('limiter.release_ms').disabled).toBe(false);
    await setNumber('limiter.release_ms', '1200');
    expect(errorOf('limiter.release_ms')).toBe('Release must be between 1 and 1000 ms.');

    await setCheck('features.limiter', true);
    expect(el('card-limiter').dataset.stage).toBe(undefined);
    expect(el('off:limiter').textContent).toBe('');
  });

  /* Per-channel trim is not the input EQ: it runs whatever the flag says, so
   * the note refuses to let the dimmed panel imply otherwise. */
  it('says the per-channel trim survives its own panel being bypassed', async () => {
    await boot();
    await setCheck('features.input_eq', false);

    expect(el('card-input-channels').dataset.stage).toBe('off');
    expect(el('off:input_eq').textContent).toBe('EQ bypassed; per-channel trim still runs');
  });

  it('marks each panel dirty on its own', async () => {
    await boot();
    expect(el('dirty:matrix').hidden).toBe(true);
    expect(el('dirty:limiter').hidden).toBe(true);

    await setNumber('matrix.center_gain_linear', '0.5');
    expect(el('dirty:matrix').hidden).toBe(false);
    expect(el('dirty:limiter').hidden).toBe(true);

    await setNumber('limiter.release_ms', '250');
    expect(el('dirty:limiter').hidden).toBe(false);

    // A comparison, not a latch, exactly like the aggregate pill.
    await setNumber('matrix.center_gain_linear', '0.70711');
    expect(el('dirty:matrix').hidden).toBe(true);
    expect(el('dirty:limiter').hidden).toBe(false);
    expect(el('dirty-state').hidden).toBe(false);
  });

  it('marks a panel dirty from its enable box alone', async () => {
    await boot();
    await setCheck('features.meters', false);

    expect(el('dirty:meters').hidden).toBe(false);
    expect(el('dirty:limiter').hidden).toBe(true);
    expect(el('dirty-state').hidden).toBe(false);
  });

  /* THE discriminating case: two panels dirty, one applied. The body is the
   * baseline the device already confirmed with only the applied panel written
   * over it, so the other panel's edit stays local and unsent. */
  it('sends only its own section, leaving another dirty section unapplied', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('matrix.center_gain_linear', '0.5');
    await setNumber('limiter.release_ms', '250');

    await click('apply:matrix');

    expect(putCalls()).toHaveLength(1);
    const put = putCalls()[0];
    expect(put.url).toBe('/api/dsp/config');
    expect(put.headers['X-Aius-SD-Auth']).toBe('0123456789abcdef');
    const body = JSON.parse(put.body);
    expect(body.matrix.center_gain_linear).toBe(0.5);
    expect(body.limiter).toEqual(factory().limiter);
    expect(body.input_channels).toEqual(factory().input_channels);
    expect(body.base_generation).toBe(0);
    // ... and the limiter edit is still on the page, waiting.
    expect(valueOf('limiter.release_ms')).toBe('250');
  });

  /* matrix.layout_aware: a matrix-panel FIELD, not a stage enable. It decides
   * whether `headroom_linear` is the summing-layout pad (mono and stereo
   * pass-through at source level) or the uniform pad on every layout, and it
   * travels in the matrix group like every other value on that panel. */
  it('round-trips the layout-aware headroom checkbox in the matrix body', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    expect(fieldOf('matrix.layout_aware').checked).toBe(true);

    await setCheck('matrix.layout_aware', false);
    expect(el('dirty:matrix').hidden).toBe(false);
    await click('apply:matrix');

    const body = JSON.parse(putCalls()[0].body);
    expect(body.matrix.layout_aware).toBe(false);
    // It is one field of the matrix group, so the rest of that group rides along
    // unchanged and no other group moves.
    expect(body.matrix.headroom_linear).toBe(0.5);
    expect(body.limiter).toEqual(factory().limiter);
    expect(body.features).toEqual(factory().features);

    // Let it land, off, and come back from the device that way: the OFF state
    // has to survive a GET or the next save would quietly re-enable it.
    device.config = {
      ...factory(),
      source: 'sd',
      config_generation: 1,
      matrix: { ...factory().matrix, layout_aware: false },
    };
    await vi.advanceTimersByTimeAsync(500);
    await settle();
    expect(fieldOf('matrix.layout_aware').checked).toBe(false);

    // And back on: the box is the only thing that decides what the device gets.
    await setCheck('matrix.layout_aware', true);
    await click('apply:matrix');
    expect(JSON.parse(putCalls()[1].body).matrix.layout_aware).toBe(true);
  });

  /* THE ABSENT-READS-ON RULE, which is the editor's half of the firmware's own
   * default: lib/aius_dsp_config treats a matrix with no layout_aware as TRUE,
   * because every config written before the field existed omits it. A page that
   * rendered the missing key as Boolean(undefined) would show the box unticked
   * and then SEND that back as a real false on the next save, turning the fix
   * off on exactly the installed base it was written for. */
  it('reads a config with no layout_aware as on, and says so on the wire', async () => {
    const older = JSON.parse(JSON.stringify(fixture('dsp-config-default.json')));
    delete older.matrix.layout_aware;
    await boot({ config: older });
    el('token').value = '0123456789abcdef';

    expect(fieldOf('matrix.layout_aware').checked).toBe(true);
    // Nothing was touched, so the page is clean: an absent key reading ON is not
    // an edit the operator has to notice.
    expect(el('dirty:matrix').hidden).toBe(true);

    await click('apply:matrix');
    expect(JSON.parse(putCalls()[0].body).matrix.layout_aware).toBe(true);
  });

  it('carries its own enable flag and leaves every other panel flag at the baseline', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setCheck('features.matrix', false);
    await setCheck('features.limiter', false);

    await click('apply:matrix');

    const body = JSON.parse(putCalls()[0].body);
    expect(body.features.matrix).toBe(false);
    expect(body.features.limiter).toBe(true);
    // The unchecked limiter box is a local edit until ITS panel is applied.
    expect(fieldOf('features.limiter').checked).toBe(false);
    expect(el('dirty:limiter').hidden).toBe(false);
  });

  it('applies the meters panel, which governs a flag and nothing else', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setCheck('features.meters', false);

    await click('apply:meters');

    const body = JSON.parse(putCalls()[0].body);
    expect(body.features).toEqual({
      input_eq: true,
      matrix: true,
      output_eq: true,
      limiter: true,
      meters: false,
    });
    expect(body.matrix).toEqual(factory().matrix);
    expect(body.limiter).toEqual(factory().limiter);
  });

  /* A panel answers for its OWN fields. What it sends for every other group is
   * the baseline the device already accepted, so a neighbour's broken field is
   * not this panel's problem. */
  it('refuses a section whose own field is out of range, and sends one whose neighbour is', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('limiter.release_ms', '1200');

    await click('apply:limiter');
    expect(putCalls()).toHaveLength(0);
    expect(el('apply-state').textContent).toBe('invalid');
    expect(el('errors').textContent).toContain('Release must be between 1 and 1000 ms.');

    await setNumber('matrix.center_gain_linear', '0.5');
    await click('apply:matrix');

    expect(putCalls()).toHaveLength(1);
    expect(JSON.parse(putCalls()[0].body).limiter).toEqual(factory().limiter);
  });

  /* THE rebase: the baseline moves onto what was WRITTEN, so the applied panel
   * goes clean and every other panel's edits become diffs against the new
   * baseline rather than being silently declared applied. */
  it('rebases only the applied panel when the write lands, and keeps the rest dirty', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('matrix.center_gain_linear', '0.5');
    await setNumber('limiter.release_ms', '250');

    await click('apply:matrix');
    device.config = { ...factory(), source: 'sd', config_generation: 1 };
    await vi.advanceTimersByTimeAsync(500);
    await settle();

    expect(el('generation').textContent).toBe('1');
    expect(el('dirty:matrix').hidden).toBe(true);
    expect(el('dirty:limiter').hidden).toBe(false);
    // The aggregate still says the page as a whole has unsent work.
    expect(el('dirty-state').hidden).toBe(false);
    expect(el('apply-state').textContent).toBe('edited');
    expect(el('status').textContent).toContain('this write did not carry');
    // The form is untouched by the landing: both edits are still on screen.
    expect(valueOf('matrix.center_gain_linear')).toBe('0.5');
    expect(valueOf('limiter.release_ms')).toBe('250');

    // Applying the other panel now diffs against the NEW baseline, so its body
    // carries the matrix value that already landed.
    await click('apply:limiter');
    const second = JSON.parse(putCalls()[1].body);
    expect(second.limiter.release_ms).toBe(250);
    expect(second.matrix.center_gain_linear).toBe(0.5);
    expect(second.base_generation).toBe(1);
  });

  it('puts a section apply through the same token and 409 discipline as the top-level one', async () => {
    await boot({ put: () => jsonResponse(409, { ok: false, error: 'stale_generation' }) });
    await setNumber('matrix.center_gain_linear', '0.5');

    await click('apply:matrix');
    expect(putCalls()).toHaveLength(0);
    expect(el('errors').textContent).toContain('SD auth token required');

    el('token').value = '0123456789abcdef';
    await click('apply:matrix');

    expect(putCalls()).toHaveLength(1);
    expect(el('apply-state').textContent).toBe('stale');
    expect(el('stale').hidden).toBe(false);
    expect(el('status').textContent).toContain('Nothing was written');
  });

  /* setBusy spans the REQUEST; the pending window after it is up to 20 s of
   * re-enabled buttons. A second FULL apply into that window is harmless -- it
   * carries the entire form and supersedes the first -- but a SECTION apply
   * composes from a baseline that has not rebased yet, so it would write the
   * first section's landed change straight back out. */
  it('refuses a second section apply while the first is still waiting to land', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('matrix.center_gain_linear', '0.5');
    await click('apply:matrix');
    expect(el('apply-state').textContent).toBe('pending');

    await setNumber('limiter.release_ms', '250');
    await click('apply:limiter');

    expect(putCalls()).toHaveLength(1);
    expect(el('errors').textContent).toContain('already waiting to land');

    // The whole-form apply is NOT refused: it carries both edits anyway.
    await click('apply');
    expect(putCalls()).toHaveLength(2);
    expect(JSON.parse(putCalls()[1].body).limiter.release_ms).toBe(250);

    // Once it lands, a section apply composes from the new baseline again.
    device.config = { ...factory(), source: 'sd', config_generation: 1 };
    await vi.advanceTimersByTimeAsync(500);
    await settle();
    await click('apply:limiter');

    expect(putCalls()).toHaveLength(3);
    expect(JSON.parse(putCalls()[2].body).matrix.center_gain_linear).toBe(0.5);
  });

  it('clears every section marker on a reload', async () => {
    await boot();
    await setNumber('matrix.center_gain_linear', '0.5');
    await setCheck('features.limiter', false);
    expect(el('dirty:matrix').hidden).toBe(false);
    expect(el('dirty:limiter').hidden).toBe(false);

    await click('reload');

    expect(el('dirty:matrix').hidden).toBe(true);
    expect(el('dirty:limiter').hidden).toBe(true);
    expect(el('dirty-state').hidden).toBe(true);
    expect(el('card-limiter').dataset.stage).toBe(undefined);
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

  /* isDirty() has always guarded the destructive paths; nothing on the page
   * SAID the form had diverged (operator request 2026-07-28). The pill is that
   * predicate made visible, so it is a comparison rather than a latch: typing
   * a value back clears it. */
  it('shows an unsaved-changes pill exactly while the form differs from the device', async () => {
    await boot();
    expect(el('dirty-state').hidden).toBe(true);

    await setNumber('limiter.release_ms', '250');
    expect(el('dirty-state').hidden).toBe(false);
    expect(el('dirty-state').textContent).toBe('unsaved changes');
    expect(el('dirty-state').dataset.state).toBe('unsaved changes');

    await setNumber('limiter.release_ms', '100');
    expect(el('dirty-state').hidden).toBe(true);
  });

  /* Two paths re-anchor "what the device holds": a re-read, and an apply that
   * lands. The apply rebases the baseline onto WHAT WAS WRITTEN, so a full
   * apply of an edited form ends clean -- it used to end `edited` with the pill
   * still lit, because the baseline was compared before the write and then
   * never moved (reported 2026-07-28, fixed with the per-section applies that
   * depend on the same rebase). */
  it('clears the unsaved-changes pill when a full apply lands, and when the device is re-read', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('limiter.release_ms', '250');
    await click('apply');
    expect(el('dirty-state').hidden).toBe(false);

    device.config = { ...fixture('dsp-config-default.json'), source: 'sd', config_generation: 1 };
    await vi.advanceTimersByTimeAsync(500);
    await settle();
    expect(el('apply-state').textContent).toBe('applied');
    expect(el('dirty-state').hidden).toBe(true);

    await setNumber('limiter.release_ms', '400');
    expect(el('dirty-state').hidden).toBe(false);
    await click('reload');
    expect(valueOf('limiter.release_ms')).toBe('100');
    expect(el('dirty-state').hidden).toBe(true);
  });

  /* The keepForm contract is untouched: an edit typed DURING the pending window
   * was not part of the write, so it survives the landing and stays dirty
   * against the newly rebased baseline. */
  it('keeps a mid-flight edit dirty after the apply it was not part of lands', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('limiter.release_ms', '250');
    await click('apply');

    await setNumber('limiter.attack_ms', '3');
    device.config = { ...fixture('dsp-config-default.json'), source: 'sd', config_generation: 1 };
    await vi.advanceTimersByTimeAsync(500);
    await settle();

    expect(valueOf('limiter.release_ms')).toBe('250');
    expect(valueOf('limiter.attack_ms')).toBe('3');
    expect(el('apply-state').textContent).toBe('edited');
    expect(el('dirty-state').hidden).toBe(false);
    expect(el('status').textContent).toContain('this write did not carry');
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

  /* The per-channel table became grouped cards (2026-07-27 feedback, item 9):
   * FRONT (fl, fr, fc and the LFE that rides with them), SURROUND, OUTPUT. */
  it('groups the per-channel meters into Front, Surround and Output cards', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);

    const groups = [...document.querySelectorAll('.meter-group')];
    expect(groups.map((group) => group.querySelector('.meter-group__title').textContent)).toEqual([
      'Front',
      'Surround',
      'Output',
    ]);
    expect(groups[0].contains(el('meter-in-lfe-peak'))).toBe(true);
    expect(groups[1].contains(el('meter-in-sl-rms'))).toBe(true);
    expect(groups[2].contains(el('meter-out-r-peak'))).toBe(true);
    // Same ids, same data: the cells read exactly what the table rows did.
    expect(el('meter-in-fl-peak').textContent).toBe('-6.2');
    expect(el('meter-out-r-rms').textContent).toBe('-14.9');
    // No stale table left behind.
    expect(document.querySelector('.meter-table')).toBe(null);
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

/* The transport facts this page is polled against (measured 2026-07-28): a
 * request on a kept-alive connection costs 2-3 ms, one that has to open a fresh
 * connection ~500 ms, and the firmware's bounded listener DROPS a connection
 * that arrives before the previous one has been released. A stacked timer tick
 * is therefore not a wasted request but a broken one, which is what the old
 * setInterval pollers produced whenever a poll ran long. */
describe('Aius SD DSP editor — serialised polling', () => {
  /* Answers everything the device stub does EXCEPT the one url held open, whose
   * resolver is handed to the test: "the previous request has not come back
   * yet" becomes a state the test drives rather than a race it hopes for. */
  const hold = (url) => {
    const inner = globalThis.fetch;
    const waiting = [];
    globalThis.fetch = vi.fn((target, init = {}) => {
      if (target !== url) return inner(target, init);
      device.calls.push({ url: target, method: init.method || 'GET', headers: init.headers || {}, body: init.body });
      return new Promise((resolve) => waiting.push(resolve));
    });
    return waiting;
  };

  it('holds the meter loop while a request hangs, and never queues the frames it missed', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);
    expect(callsTo('/api/meters')).toHaveLength(1);

    const waiting = hold('/api/meters');
    await vi.advanceTimersByTimeAsync(3000);

    // Thirty 10 Hz ticks' worth of wall clock, ONE request on the wire.
    expect(waiting).toHaveLength(1);
    expect(callsTo('/api/meters')).toHaveLength(2);

    // The ANSWER releases the next request, not the clock -- and the thirty
    // frames the hang covered are skipped, not caught up on.
    waiting[0](jsonResponse(200, fixture('meters-snapshot.json')));
    await settle();
    expect(callsTo('/api/meters')).toHaveLength(2);
    await vi.advanceTimersByTimeAsync(100);
    expect(callsTo('/api/meters')).toHaveLength(3);
    expect(el('meter-state').textContent).toBe('live');
  });

  it('holds the status loop the same way, at its own 1 Hz cadence', async () => {
    await boot();
    expect(callsTo('/api/status')).toHaveLength(1);

    const waiting = hold('/api/status');
    await vi.advanceTimersByTimeAsync(1000);
    expect(callsTo('/api/status')).toHaveLength(2);

    await vi.advanceTimersByTimeAsync(5000);
    expect(waiting).toHaveLength(1);
    expect(callsTo('/api/status')).toHaveLength(2);

    waiting[0](jsonResponse(200, STATUS_IDLE));
    await settle();
    await vi.advanceTimersByTimeAsync(1000);
    expect(callsTo('/api/status')).toHaveLength(3);
  });

  /* The pending window polls the config endpoint every 500 ms for up to 20 s;
   * a slow read there stacks exactly as a slow meter read did. */
  it('holds the pending-window loop while the generation read hangs', async () => {
    await boot();
    el('token').value = '0123456789abcdef';
    await setNumber('matrix.center_gain_linear', '0.5');
    await click('apply');
    expect(el('apply-state').textContent).toBe('pending');

    const before = callsTo('/api/dsp/config').length;
    const waiting = hold('/api/dsp/config');
    await vi.advanceTimersByTimeAsync(5000);

    expect(waiting).toHaveLength(1);
    expect(callsTo('/api/dsp/config')).toHaveLength(before + 1);
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

  /* "optical carrier" alone never said WHAT was on it. `state` is the
   * firmware's own decode vocabulary (2 no carrier, 3 PCM, 4 DTS) and
   * `sample_rate` the negotiated rate; both ride the same 1 Hz snapshot the
   * flow ends already read. */
  const carrierStatus = (over) => ({
    carrier: 1,
    state: 3,
    format: 1,
    sample_rate: 48000,
    tx_gate: { open: 1, carrier_locked: 1, replay_running: 0 },
    ...over,
  });

  it('names the encoding and the rate a locked PCM carrier is delivering', async () => {
    await boot({ status: carrierStatus() });

    expect(el('input-source').textContent).toBe('input: optical carrier · PCM · 48000 Hz');
    expect(el('input-source').dataset.state).toBe('carrier');
  });

  it('names a DTS carrier by its decode state', async () => {
    await boot({ status: carrierStatus({ state: 4, format: 2, sample_rate: 44100 }) });

    expect(el('input-source').textContent).toBe('input: optical carrier · DTS · 44100 Hz');
  });

  it('leaves off a term the snapshot cannot support instead of printing "unknown"', async () => {
    // state 2 is a carrier with nothing decoded off it, and a rate of 0 is one
    // that has not been negotiated: neither is a fact worth a word.
    await boot({ status: carrierStatus({ state: 2, format: 0, sample_rate: 0 }) });
    expect(el('input-source').textContent).toBe('input: optical carrier');

    // A state this page has no name for drops the encoding only.
    device.status = carrierStatus({ state: 9, sample_rate: 96000 });
    await vi.advanceTimersByTimeAsync(1000);
    expect(el('input-source').textContent).toBe('input: optical carrier · 96000 Hz');
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
  it('restores the SD auth token from localStorage on boot, locked against stray edits', async () => {
    localStorage.setItem('aius-sd-auth', '0123456789abcdef');
    await boot();
    expect(el('token').value).toBe('0123456789abcdef');
    expect(el('token').readOnly).toBe(true);
    expect(el('token-toggle').textContent).toBe('Change token');
  });

  /* One toggle (2026-07-28) replaced the save/edit pair, which replaced the
   * silent per-keystroke store: nothing persists until the button is pressed,
   * and the button's own label is the lock state. */
  it('stores the token on Save token -- not per keystroke -- and locks the field', async () => {
    await boot();
    expect(el('token').readOnly).toBe(false);
    expect(el('token-toggle').textContent).toBe('Save token');

    el('token').value = 'fedcba9876543210';
    el('token').dispatchEvent(new Event('input'));
    expect(localStorage.getItem('aius-sd-auth')).toBe(null);

    await click('token-toggle');
    expect(localStorage.getItem('aius-sd-auth')).toBe('fedcba9876543210');
    expect(el('token').readOnly).toBe(true);
    expect(el('token-toggle').textContent).toBe('Change token');
    expect(el('status').textContent).toContain('token saved');
  });

  it('unlocks on Change token, and an empty save clears the stored token', async () => {
    localStorage.setItem('aius-sd-auth', 'fedcba9876543210');
    await boot();
    expect(el('token').readOnly).toBe(true);

    await click('token-toggle');
    expect(el('token').readOnly).toBe(false);
    expect(el('token-toggle').textContent).toBe('Save token');
    expect(document.activeElement).toBe(el('token'));
    // Unlocking is not a write: the stored value is still there.
    expect(localStorage.getItem('aius-sd-auth')).toBe('fedcba9876543210');

    el('token').value = '';
    await click('token-toggle');
    expect(localStorage.getItem('aius-sd-auth')).toBe(null);
    // Nothing stored, so nothing to lock: the field stays editable.
    expect(el('token').readOnly).toBe(false);
    expect(el('token-toggle').textContent).toBe('Save token');
    expect(el('status').textContent).toContain('token cleared');
  });

  it('resets a single field to the factory value', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });

    expect(valueOf('matrix.lfe_gain_linear')).toBe('0.25');
    el('r:matrix.lfe_gain_linear').click();
    await settle();
    expect(valueOf('matrix.lfe_gain_linear')).toBe('0.31623');
    expect(el('n:matrix.lfe_gain_linear').textContent).toBe('= -10.0 dB');
  });

  /* The per-section reset (2026-07-27 feedback, item 2): one button per table
   * row, resetting the whole section through the same per-field path. */
  it('section reset returns an output slot to the factory none and drops it from the PUT', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    el('token').value = '0123456789abcdef';

    el('sr:output.eq.0').click();
    await settle();
    expect(valueOf('output.eq.0.type')).toBe('none');

    await click('apply');
    expect(JSON.parse(putCalls()[0].body).output_eq).toEqual([]);
  });

  it('section reset clears one input-channel row and leaves its neighbours alone', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    el('token').value = '0123456789abcdef';

    // fr carries two sections; reset only the second (the 45 Hz highpass).
    el('sr:input_channels.fr.eq.1').click();
    await settle();

    expect(valueOf('input_channels.fr.eq.1.type')).toBe('none');
    expect(valueOf('input_channels.fr.eq.0.frequency_hz')).toBe('120');
    expect(valueOf('input_channels.fl.eq.0.frequency_hz')).toBe('120');

    await click('apply');
    expect(JSON.parse(putCalls()[0].body).input_channels.fr.eq).toEqual([
      { type: 'peaking', frequency_hz: 120, q: 1.2, gain_db: -4.5 },
    ]);
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

  /* The name is the widest thing this card asks for and both buttons beside it
   * read it, so it owns a row; the file controls are a separate errand and
   * take the row below (operator request 2026-07-28). */
  it('gives the preset name its own row and puts the file operations below it', async () => {
    await boot();
    const rows = [...el('preset-name').closest('.card').querySelectorAll('.controls')];

    expect(rows).toHaveLength(2);
    expect(rows[0].contains(el('preset-name'))).toBe(true);
    expect(rows[0].contains(el('preset-save'))).toBe(true);
    expect(rows[0].contains(el('preset-download'))).toBe(true);
    expect(rows[1].contains(el('preset-file'))).toBe(true);
    expect(rows[1].contains(el('preset-upload'))).toBe(true);
    expect(rows[1].contains(el('preset-refresh'))).toBe(true);
    // The name takes the slack in its row rather than sitting at label width.
    expect(el('preset-name').closest('label').className).toBe('preset-name-field');
  });

  /* Saving to a FILE is the mirror of the upload: a browser save that touches
   * neither the card nor the device, so no token and no write. */
  it('downloads the current form as exactly the five preset groups', async () => {
    await boot({ config: fixture('dsp-config-sd.json'), presetsIndex: PRESET_INDEX_DEFAULT });
    el('preset-name').value = 'evening';

    await click('preset-download');

    expect(downloads).toEqual([{ name: 'evening.json', url: 'blob:aius/0' }]);
    const body = await downloadBody();
    expect(Object.keys(body).sort()).toEqual(['features', 'input_channels', 'limiter', 'matrix', 'output_eq']);
    expect(body).toEqual(fiveGroups('dsp-config-sd.json'));
    expect(putCalls()).toHaveLength(0);
    expect(el('preset-status').textContent).toBe('Downloaded evening.json.');
  });

  it('names an unnamed export "current", and hands the object URL back afterwards', async () => {
    await boot();

    await click('preset-download');

    expect(downloads[0].name).toBe('current.json');
    expect(objectUrls[0].revoked).toBe(true);
  });

  /* An out-of-range field collects as its fallback, so an ungated export would
   * quietly write a zero nobody typed -- the same reason the card save refuses
   * an invalid form. */
  it('refuses to export a form Apply itself would refuse', async () => {
    await boot();
    await setNumber('limiter.release_ms', '1200');

    await click('preset-download');

    expect(downloads).toEqual([]);
    expect(el('preset-errors').textContent).toContain('Fix the form first');
  });

  it('downloads a stored preset as the card holds it, over a public GET', async () => {
    await boot({
      presetsIndex: PRESET_INDEX_DEFAULT,
      presetFiles: { movie: fiveGroups('dsp-config-default.json') },
    });

    presetRowButton('movie', 'Download').click();
    await settle();

    expect(downloads[0].name).toBe('movie.json');
    expect(await downloadBody()).toEqual(fiveGroups('dsp-config-default.json'));
    const read = device.calls.find((call) => call.url === '/sd/www/presets/movie.json');
    expect(read.method).toBe('GET');
    expect(read.headers['X-Aius-SD-Auth']).toBe(undefined);
    // The form is untouched: a download reads the card, it does not load it.
    expect(el('preset-origin').textContent).toBe('');
    expect(el('preset-status').textContent).toBe('Downloaded movie.json.');
  });

  /* Which preset the form came from is a different question from whether it
   * differs from the DEVICE: a loaded preset is dirty against the device and
   * unmodified against itself, and both are worth saying. */
  it('names the preset the form came from and marks it modified once it diverges', async () => {
    await boot({
      config: fixture('dsp-config-sd.json'),
      presetsIndex: PRESET_INDEX_DEFAULT,
      presetFiles: { movie: fiveGroups('dsp-config-default.json') },
    });
    expect(el('preset-origin').textContent).toBe('');

    presetRowButton('movie', 'Load').click();
    await settle();
    expect(el('preset-origin').textContent).toBe('preset: movie');
    expect(el('dirty-state').hidden).toBe(false);

    await setNumber('limiter.release_ms', '250');
    expect(el('preset-origin').textContent).toBe('preset: movie (modified)');

    // A comparison, not a latch: typing the preset's own value back clears it.
    await setNumber('limiter.release_ms', '100');
    expect(el('preset-origin').textContent).toBe('preset: movie');
  });

  it('drops the preset provenance when the device is re-read', async () => {
    await boot({
      presetsIndex: PRESET_INDEX_DEFAULT,
      presetFiles: { movie: fiveGroups('dsp-config-sd.json') },
    });
    presetRowButton('movie', 'Load').click();
    await settle();
    expect(el('preset-origin').textContent).toBe('preset: movie');

    await click('reload');

    expect(el('preset-origin').textContent).toBe('');
    // The status sentence describes the last ACTION and is untouched; the
    // origin describes the FORM, which the re-read replaced. Two beats, two
    // elements.
    expect(el('preset-status').textContent).toContain('Loaded movie');
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
      [20, 12],
      [120, -4.5],
      [1000, 0],
      [12000, 6],
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
    expect(maths().yToDb(100, offset)).toBeCloseTo(12, 9);
    const point = maths().pointToEq(offset, maths().hzToX(1000, offset), maths().dbToY(-6, offset));
    expect(point.frequency_hz).toBeCloseTo(1000, 6);
    expect(point.gain_db).toBeCloseTo(-6, 6);
  });

  /* The rails are the EDITOR's +-12 dB range, not the device's +-24: a gesture
   * may never write a value the surface it was made on cannot show. */
  it('clamps a drag to the axis bounds, so no reachable pixel is out of range', () => {
    expect(maths().pointToEq(rect, -500, -500)).toEqual({ frequency_hz: 20, gain_db: 12 });
    expect(maths().pointToEq(rect, 5000, 5000)).toEqual({ frequency_hz: 20000, gain_db: -12 });
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

  /* The visible Q affordance (2026-07-27 feedback, item 2): the wheel still
   * works, but a wheel is invisible and touch has none. */
  it('edits the selected section Q from the visible box, into both pair members', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    el('token').value = '0123456789abcdef';
    expect(el('curve-q').disabled).toBe(false);
    expect(el('curve-q').value).toBe('1.2');

    el('curve-q').value = '2.5';
    el('curve-q').dispatchEvent(new Event('input'));
    await settle();

    // The unlinked front pair is adopted first, exactly as a wheel edit does.
    expect(valueOf('input_channels.fl.eq.0.q')).toBe('2.5');
    expect(valueOf('input_channels.fr.eq.0.q')).toBe('2.5');
    expect(el('unlinked:front').hidden).toBe(true);

    await click('apply');
    expect(JSON.parse(putCalls()[0].body).input_channels.fl.eq[0].q).toBe(2.5);
  });

  it('follows the selection with the Q box, and disables it with no active section', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    expect(el('curve-q').value).toBe('1.2');

    // The output cascade's section carries Q 0.9.
    chip('output').click();
    await settle();
    expect(el('curve-q').value).toBe('0.9');

    // Surround's slot 0 is a bypass: nothing active to edit.
    chip('surround').click();
    await settle();
    expect(el('curve-q').disabled).toBe(true);
    expect(el('curve-q').value).toBe('');
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

    // The bottom rail and "20" used to collide in the bottom-left corner.
    const floor = hzLabels.find((op) => op[1] === '20');
    const rail = dbLabels.find((op) => op[1] === '-12');
    expect(floor[2]).toBeGreaterThan(rail[2] + 20);
    // The top rail used to be drawn above the top edge; it hangs below it now.
    const ceiling = dbLabels.find((op) => op[1] === '+12');
    expect(ceiling[3]).toBeGreaterThan(0);
    expect(rail[3]).toBeLessThan(240);
  });
});

/* The routing diagram's pure half, driven with literal rects exactly as the
 * curve maths above -- happy-dom has no layout engine to synthesise geometry
 * from. `values` is the plain object the DOM half reads out of the form. */
describe('Aius SD DSP editor — downmix matrix geometry', () => {
  const rect = { left: 0, top: 0, width: 960, height: 300 };
  const gain = { min: 0, max: 4 };
  const headroom = { min: 0.01, max: 1 };

  beforeEach(async () => {
    await boot();
  });

  it('round-trips a gain through its 0..4 track and clamps beyond both rails', () => {
    for (const value of [0, 0.31623, 0.70711, 1, 2.5, 4]) {
      expect(maths().matrixYToValue(maths().matrixValueToY(value, rect, gain), rect, gain)).toBeCloseTo(value, 3);
    }
    // The rails ARE the field bounds, so no reachable pixel is out of range.
    expect(maths().matrixYToValue(-500, rect, gain)).toBe(4);
    expect(maths().matrixYToValue(5000, rect, gain)).toBe(0);
    // Maximum at the top, minimum at the bottom.
    expect(maths().matrixValueToY(4, rect, gain)).toBeLessThan(maths().matrixValueToY(0, rect, gain));
  });

  it('keeps the headroom track off the exclusive zero its field refuses', () => {
    expect(maths().matrixYToValue(5000, rect, headroom)).toBe(0.01);
    expect(maths().matrixYToValue(-5000, rect, headroom)).toBe(1);
    expect(maths().matrixYToValue(maths().matrixValueToY(0.37, rect, headroom), rect, headroom)).toBeCloseTo(0.37, 3);
    // The bounds come from the form's own descriptor table, so the exclusive
    // minimum's floor is that field's native step and not a second opinion.
    expect(matrixHandle('headroom').bound).toEqual({ min: 0.01, max: 1 });
    expect(matrixHandle('center').bound).toEqual({ min: 0, max: 4 });
  });

  it('stacks the six decoded sources left and the stereo pair right', () => {
    const layout = maths().matrixLayout(rect);
    expect(layout.sources.map((node) => node.label)).toEqual(['FL', 'FR', 'FC', 'LFE', 'SL', 'SR']);
    expect(layout.outputs.map((node) => node.label)).toEqual(['L', 'R']);
    // One column each, in signal order: sources, then the gains, then the pair.
    expect(new Set(layout.sources.map((node) => node.x)).size).toBe(1);
    expect(layout.sources[0].x).toBeLessThan(matrixHandle('center').x);
    expect(matrixHandle('surround').x).toBeLessThan(layout.outputs[0].x);
    // Evenly stacked top to bottom, and inside the rect at both ends.
    const ys = layout.sources.map((node) => node.y);
    expect([...ys].sort((a, b) => a - b)).toEqual(ys);
    expect(ys[0]).toBeGreaterThan(rect.top);
    expect(ys[5]).toBeLessThan(rect.top + rect.height);
  });

  /* The picture is the firmware's downmix sum and nothing else: FL and FR cross
   * at unity, FC and the LFE fold-in feed both sides, SL and SR keep their own. */
  it('routes the eight terms of the downmix, one shared handle per gain', () => {
    const edges = maths().matrixEdges(matrixValues(), rect);
    expect(edges.map((edge) => `${edge.source}>${edge.output}`)).toEqual([
      'fl>l',
      'fr>r',
      'fc>l',
      'fc>r',
      'lfe>l',
      'lfe>r',
      'sl>l',
      'sr>r',
    ]);

    // The unity references are fixed: no handle, gain 1, straight across.
    const unity = edges.filter((edge) => edge.fixed);
    expect(unity.map((edge) => edge.gain)).toEqual([1, 1]);
    expect(unity.every((edge) => edge.handle === null && edge.points.length === 2)).toBe(true);

    // Both centre paths bend through the SAME point: one field, two uses.
    const centre = edges.filter((edge) => edge.handle === 'center');
    expect(centre).toHaveLength(2);
    expect(centre[0].points[1]).toEqual(centre[1].points[1]);
    expect(centre[0].points[1].y).toBeCloseTo(matrixHandle('center').y, 9);
    // As do the two LFE paths, and the two surround ones.
    const lfe = edges.filter((edge) => edge.handle === 'lfe');
    expect(lfe[0].points[1]).toEqual(lfe[1].points[1]);
    const surround = edges.filter((edge) => edge.handle === 'surround');
    expect(surround[0].points[1]).toEqual(surround[1].points[1]);
    expect(surround[0].gain).toBeCloseTo(0.7071067811865475, 9);
  });

  it('scales a path with its gain, and draws a silent one muted', () => {
    const loud = maths().matrixPathStyle(4);
    const quiet = maths().matrixPathStyle(0.3);
    const muted = maths().matrixPathStyle(0);
    expect(loud.width).toBeGreaterThan(quiet.width);
    expect(loud.alpha).toBe(1);
    expect(quiet.alpha).toBeLessThan(loud.alpha);
    expect(quiet.muted).toBe(false);
    expect(muted.muted).toBe(true);
    expect(muted.width).toBeLessThan(quiet.width);
    // A gain above the field bound cannot draw wider than the rail.
    expect(maths().matrixPathStyle(40).width).toBe(loud.width);
    // A half-typed gain is not a signal.
    expect(maths().matrixPathStyle(NaN).muted).toBe(true);
  });

  it('picks a handle only within the grab radius, and never two at once', () => {
    const points = maths().matrixHandlePoints(matrixValues(), rect);
    const centre = points.find((point) => point.key === 'center');
    const surround = points.find((point) => point.key === 'surround');
    expect(maths().pickHandle(points, centre.x + 4, centre.y - 4, 16).key).toBe('center');
    expect(maths().pickHandle(points, centre.x + 60, centre.y, 16)).toBe(null);
    // Centre and surround carry the SAME factory gain, so only the separate
    // columns keep both of them reachable.
    expect(surround.y).toBe(centre.y);
    expect(maths().pickHandle(points, surround.x, surround.y, 16).key).toBe('surround');
  });

  it('labels a gain in linear AND decibels, and calls zero muted', () => {
    expect(maths().matrixGainLabel(1)).toBe('1 linear, 0.0 dB');
    expect(maths().matrixGainLabel(0.7071067811865475)).toBe('0.70711 linear, -3.0 dB');
    expect(maths().matrixGainLabel(2)).toBe('2 linear, 6.0 dB');
    // Silence is not a level, so it does not read as one.
    expect(maths().matrixGainLabel(0)).toBe('0 linear, muted');
    expect(maths().matrixGainLabel(NaN)).toBe('--');
    expect(maths().matrixCrossoverLabel(120)).toBe('LR4 @ 120 Hz');
    expect(maths().matrixCrossoverLabel(NaN)).toBe('LR4 @ -- Hz');
  });

  it('puts the crossover label on the LFE path, clear of the gain handle', () => {
    const box = maths().matrixCrossoverBox(rect);
    const node = maths().matrixLayout(rect).sources.find((source) => source.key === 'lfe');
    expect(maths().matrixHitsBox(box, box.left + box.width / 2, box.top + box.height / 2)).toBe(true);
    expect(maths().matrixHitsBox(box, box.left - 5, box.top + 2)).toBe(false);
    expect(maths().matrixHitsBox(box, box.left + 2, box.top + box.height + 5)).toBe(false);
    // It rides the LFE source's own row, so it holds still while the gain moves.
    expect(box.top + box.height).toBeLessThanOrEqual(node.y);
    // The backing store is DPR-scaled; the box scales with it.
    expect(maths().matrixCrossoverBox(rect, 2).width).toBe(box.width * 2);
  });

  it('steps a gain multiplicatively and leaves a muted path muted', () => {
    expect(maths().stepMatrixValue(0.5, -100, gain)).toBeCloseTo(0.56, 6);
    expect(maths().stepMatrixValue(0.5, 100, gain)).toBeCloseTo(0.446, 6);
    let value = 1;
    for (let notch = 0; notch < 20; notch++) value = maths().stepMatrixValue(value, -100, gain);
    expect(value).toBe(4);
    // A ratio cannot lift silence: a notch up seeds it, a notch down leaves it.
    expect(maths().stepMatrixValue(0, 100, gain)).toBe(0);
    expect(maths().stepMatrixValue(0, -100, gain)).toBeCloseTo(0.056, 6);
    // Headroom's exclusive floor holds under the wheel too.
    expect(maths().stepMatrixValue(0.011, 100, headroom)).toBe(0.01);
    expect(maths().stepMatrixValue(1, -100, headroom)).toBe(1);
  });

  /* The SL/SR colour split (2026-07-27 feedback, item 8): two path colours
   * make the two channels legible, while the shared handle point keeps saying
   * the value is one bonded field. */
  it('colours the bonded SL and SR paths separately, through one shared point', () => {
    const edges = maths().matrixEdges(matrixValues(), rect);
    const sl = edges.find((edge) => edge.source === 'sl');
    const sr = edges.find((edge) => edge.source === 'sr');
    expect(sl.colour).not.toBe(sr.colour);
    // Still one gain: both paths bend through the same handle point.
    expect(sl.points[1]).toEqual(sr.points[1]);
    expect(sl.gain).toBe(sr.gain);
    expect(sl.handle).toBe('surround');
    expect(sr.handle).toBe('surround');
  });

  it('nudges by the fields own step and stops at both rails', () => {
    expect(maths().nudgeMatrixValue(0.7071067811865475, 1, gain)).toBe(0.717);
    expect(maths().nudgeMatrixValue(0.7071067811865475, -1, gain)).toBe(0.697);
    expect(maths().nudgeMatrixValue(4, 1, gain)).toBe(4);
    expect(maths().nudgeMatrixValue(0, -1, gain)).toBe(0);
    expect(maths().nudgeMatrixValue(0.01, -1, headroom)).toBe(0.01);
    expect(maths().nudgeMatrixValue(1, 1, headroom)).toBe(1);
    // An empty field starts from the bound rather than from nothing.
    expect(maths().nudgeMatrixValue(NaN, 1, gain)).toBe(0.01);
  });
});

describe('Aius SD DSP editor — the downmix matrix diagram', () => {
  it('sits in the strip between the EQ plot and the limiter', async () => {
    await boot();
    expect([...document.querySelectorAll('.visual .visual-block__title')].map((node) => node.textContent)).toEqual([
      'Input levels',
      'EQ cascades',
      'Downmix matrix',
      'Limiter gain reduction',
      'Output levels',
    ]);
  });

  it('draws every term of the downmix, the unity references included', async () => {
    await boot();
    const edges = matrixEdgePaths();
    expect(edges).toHaveLength(8);
    // Two straight unity references, six paths bent through a gain handle.
    expect(edges.filter((path) => vertices(path).length === 2)).toHaveLength(2);
    expect(edges.filter((path) => vertices(path).length === 3)).toHaveLength(6);
    expect(handlesOf('matrix-diagram')).toHaveLength(4);
    // Both unity labels are drawn, and the crossover with its filter shape.
    const labels = labelsOf('matrix-diagram').map((op) => op[1]);
    expect(labels.filter((text) => text === '1.0')).toHaveLength(2);
    expect(labels).toContain('LR4 @ 120 Hz');
    expect(labels).toContain('LFE');
    expect(el('matrix-caption').textContent).toBe('Centre selected: 0.70711 linear, -3.0 dB.');
  });

  it('thins and dashes a path as its gain goes to silence', async () => {
    await boot();
    const before = strokeOf(matrixEdgePaths().find((path) => vertices(path).length === 3));

    await setNumber('matrix.center_gain_linear', '0');

    const dashed = matrixEdgePaths().filter((path) => strokeOf(path)[4].length > 0);
    // Both centre paths went muted; the LFE, surround and unity ones did not.
    expect(dashed).toHaveLength(2);
    expect(strokeOf(dashed[0])[2]).toBeLessThan(before[2]);
    expect(strokeOf(dashed[0])[3]).toBeLessThan(before[3]);
  });

  it('redraws from the form, so a typed gain moves its handle', async () => {
    await boot();
    const first = handlesOf('matrix-diagram').map((op) => op[2]);

    await setNumber('matrix.surround_gain_linear', '2');

    const moved = handlesOf('matrix-diagram').map((op) => op[2]);
    expect(moved[1]).toBeCloseTo(atGain('surround', 2), 6);
    // A bigger gain sits higher up, and only its own handle moved.
    expect(moved[1]).toBeLessThan(first[1]);
    expect(moved[0]).toBe(first[0]);
  });

  it('writes a dragged centre gain into the same field text mode edits', async () => {
    await boot();
    stubMatrixRect();
    el('token').value = '0123456789abcdef';
    const handle = matrixHandle('center');

    // Grabbed near the handle rather than exactly on it, as a hand would.
    matrixPointer('pointerdown', handle.x + 6, handle.y - 5);
    matrixPointer('pointermove', handle.x + 9, atGain('center', 1.5));
    matrixPointer('pointerup', handle.x + 9, atGain('center', 1.5));
    await settle();

    expect(valueOf('matrix.center_gain_linear')).toBe('1.5');
    // A drag runs the same validation and the same dB note a typed edit does.
    expect(errorOf('matrix.center_gain_linear')).toBe('');
    expect(el('n:matrix.center_gain_linear').textContent).toBe('= 3.5 dB');
    expect(el('apply-state').textContent).toBe('edited');

    await click('apply');
    expect(JSON.parse(putCalls()[0].body).matrix.center_gain_linear).toBe(1.5);
  });

  it('clamps a drag to the field bounds at both rails', async () => {
    await boot();
    stubMatrixRect();
    const handle = matrixHandle('lfe');

    matrixPointer('pointerdown', handle.x - 7, handle.y + 6);
    matrixPointer('pointermove', handle.x, -400);
    await settle();
    expect(valueOf('matrix.lfe_gain_linear')).toBe('4');

    matrixPointer('pointermove', handle.x, 900);
    await settle();
    expect(valueOf('matrix.lfe_gain_linear')).toBe('0');
    expect(errorOf('matrix.lfe_gain_linear')).toBe('');
    expect(el('matrix-readout').textContent).toBe('LFE 0 linear, muted');
  });

  it('drags the headroom handle over its own exclusive-min bound', async () => {
    await boot();
    stubMatrixRect();
    const handle = matrixHandle('headroom');

    matrixPointer('pointerdown', handle.x + 5, handle.y + 5);
    matrixPointer('pointermove', handle.x, atGain('headroom', 0.8));
    matrixPointer('pointermove', handle.x, 5000);
    matrixPointer('pointerup', handle.x, 5000);
    await settle();

    // The bottom rail is the field's step, never the zero it refuses.
    expect(valueOf('matrix.headroom_linear')).toBe('0.01');
    expect(errorOf('matrix.headroom_linear')).toBe('');
  });

  it('takes the wheel over a handle as a fine, multiplicative adjustment', async () => {
    await boot();
    stubMatrixRect();
    const handle = matrixHandle('center');

    // Not grabbed, not focused: the wheel belongs to the page.
    expect(matrixWheel(handle.x, handle.y, -100)).toBe(true);
    await settle();
    expect(valueOf('matrix.center_gain_linear')).toBe('0.70711');

    el('matrix-diagram').focus();
    expect(matrixWheel(handle.x + 6, handle.y + 6, -100)).toBe(false);
    await settle();
    expect(valueOf('matrix.center_gain_linear')).toBe('0.792');

    matrixWheel(handle.x, handle.y, 100);
    await settle();
    expect(valueOf('matrix.center_gain_linear')).toBe('0.707');
    expect(valueOf('matrix.surround_gain_linear')).toBe('0.70711');
  });

  it('leaves the fixed unity references inert: they have no field to write', async () => {
    await boot();
    stubMatrixRect();
    const unity = maths()
      .matrixEdges(matrixValues(), MATRIX_RECT)
      .find((edge) => edge.fixed);
    const middle = {
      x: (unity.points[0].x + unity.points[1].x) / 2,
      y: (unity.points[0].y + unity.points[1].y) / 2,
    };

    matrixPointer('pointerdown', middle.x, middle.y);
    matrixPointer('pointermove', middle.x, 40);
    matrixPointer('pointerup', middle.x, 40);
    await settle();

    expect(valueOf('matrix.center_gain_linear')).toBe('0.70711');
    expect(el('apply-state').textContent).toBe('loaded');
    // There is nothing to write: the model has no FL or FR matrix gain.
    expect(el('f:matrix.fl_gain_linear')).toBe(null);
    expect(unity.gain).toBe(1);
  });

  it('locks the headroom handle while the clip-free policy owns it', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubMatrixRect();
    const handle = matrixHandle('headroom', matrixValues({ headroom: 0.37037, clipfree: true }));
    expect(handle.locked).toBe(true);

    matrixPointer('pointerdown', handle.x, handle.y);
    matrixPointer('pointermove', handle.x, atGain('headroom', 0.9));
    matrixPointer('pointerup', handle.x, atGain('headroom', 0.9));
    await settle();

    // Unmoved, and the refusal names the policy that owns the value.
    expect(valueOf('matrix.headroom_linear')).toBe('0.37037');
    expect(el('matrix-status').textContent).toContain('resolved by the clip-free policy');
    expect(el('matrix-status').textContent).toContain('Turn the clip-free chip off');
    // Drawn as the dim, locked thing it is.
    expect(handlesOf('matrix-diagram')[3][6]).toBe('#5d6b78');
  });

  it('toggles the clip-free policy from the chip on the diagram', async () => {
    await boot();
    expect(el('matrix-clipfree').getAttribute('aria-pressed')).toBe('false');
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(false);

    await click('matrix-clipfree');

    expect(fieldOf('matrix.clipfree').checked).toBe(true);
    expect(el('matrix-clipfree').getAttribute('aria-pressed')).toBe('true');
    // The form's own resolution follows from the same one write.
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(true);
    expect(Number(valueOf('matrix.headroom_linear'))).toBeCloseTo(
      1 / (1 + 0.7071067811865475 + 0.7071067811865475 + 0.31622776601683794),
      5,
    );
    expect(el('matrix-status').textContent).toContain('Clip-free headroom on');

    await click('matrix-clipfree');
    expect(fieldOf('matrix.clipfree').checked).toBe(false);
    expect(el('matrix-clipfree').getAttribute('aria-pressed')).toBe('false');
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(false);
  });

  it('renders the clip-free resolved headroom live as the gains move', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    const first = handlesOf('matrix-diagram')[3][2];

    await setNumber('matrix.center_gain_linear', '3');

    const moved = handlesOf('matrix-diagram')[3][2];
    // More centre in the mix means less headroom, so the handle drops.
    expect(moved).toBeGreaterThan(first);
    expect(moved).toBeCloseTo(atGain('headroom', Number(valueOf('matrix.headroom_linear'))), 1);
  });

  it('sends the crossover label to the form field rather than editing in canvas', async () => {
    await boot();
    stubMatrixRect();
    const box = maths().matrixCrossoverBox(MATRIX_RECT);

    matrixPointer('pointerdown', box.left + box.width / 2, box.top + box.height / 2);
    await settle();

    expect(document.activeElement).toBe(fieldOf('matrix.lfe_crossover_hz'));
    expect(el('matrix-status').textContent).toContain('LFE crossover');
    // A way in, not a control: nothing was written on the way.
    expect(valueOf('matrix.lfe_crossover_hz')).toBe('120');
    expect(el('apply-state').textContent).toBe('loaded');
  });

  it('nudges the selected handle from the keyboard and cycles between them', async () => {
    await boot();
    expect(el('matrix-caption').textContent).toContain('Centre selected');

    matrixKey('ArrowUp');
    await settle();
    expect(valueOf('matrix.center_gain_linear')).toBe('0.717');
    matrixKey('ArrowDown');
    matrixKey('ArrowDown');
    await settle();
    expect(valueOf('matrix.center_gain_linear')).toBe('0.697');

    matrixKey('PageDown');
    await settle();
    expect(el('matrix-caption').textContent).toContain('Surround selected');
    matrixKey('ArrowUp');
    await settle();
    expect(valueOf('matrix.surround_gain_linear')).toBe('0.717');
    // Only the selected handle moves.
    expect(valueOf('matrix.center_gain_linear')).toBe('0.697');

    for (const label of ['LFE selected', 'Headroom selected', 'Centre selected']) {
      matrixKey('PageDown');
      await settle();
      expect(el('matrix-caption').textContent).toContain(label);
    }
    matrixKey('PageUp');
    await settle();
    expect(el('matrix-caption').textContent).toContain('Headroom selected');
  });

  it('refuses a keyboard nudge on a locked headroom, and opens the crossover on Enter', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    matrixKey('PageUp');
    await settle();
    expect(el('matrix-caption').textContent).toContain('Headroom selected');
    expect(el('matrix-caption').textContent).toContain('resolved by the clip-free policy');

    const before = valueOf('matrix.headroom_linear');
    matrixKey('ArrowUp');
    await settle();
    expect(valueOf('matrix.headroom_linear')).toBe(before);
    expect(el('matrix-status').textContent).toContain('cannot be dragged');

    matrixKey('Enter');
    await settle();
    expect(document.activeElement).toBe(fieldOf('matrix.lfe_crossover_hz'));
  });

  it('suppresses panning only while a handle is held, and discloses its targets', async () => {
    await boot();
    stubMatrixRect();
    const handle = matrixHandle('center');
    const box = maths().matrixCrossoverBox(MATRIX_RECT);
    expect(el('matrix-diagram').style.touchAction).toBe('');

    matrixPointer('pointermove', handle.x + 8, handle.y + 8);
    expect(el('matrix-diagram').style.cursor).toBe('pointer');
    matrixPointer('pointermove', handle.x, 20);
    expect(el('matrix-diagram').style.cursor).toBe('');
    // The click-to-edit label is a target too, and says so.
    matrixPointer('pointermove', box.left + 4, box.top + 4);
    expect(el('matrix-diagram').style.cursor).toBe('pointer');

    matrixPointer('pointerdown', handle.x, handle.y);
    expect(el('matrix-diagram').style.touchAction).toBe('none');
    expect(el('matrix-readout').hidden).toBe(false);
    matrixPointer('pointerup', handle.x, handle.y);
    expect(el('matrix-diagram').style.touchAction).toBe('');
    expect(el('matrix-readout').hidden).toBe(true);
  });

  it('does not drag the diagram when the DOM has no layout to map against', async () => {
    await boot();

    matrixPointer('pointerdown', 400, 100);
    matrixPointer('pointermove', 400, 40);
    await settle();

    expect(valueOf('matrix.center_gain_linear')).toBe('0.70711');
  });

  /* Per-channel reset on the diagram itself (2026-07-27 feedback, item 3):
   * double-click a handle, through the same resetField() write the form's own
   * reset buttons make. */
  it('resets a channel to the factory default on a handle double-click', async () => {
    await boot();
    stubMatrixRect();
    await setNumber('matrix.center_gain_linear', '2');
    const handle = matrixHandle('center', matrixValues({ center: 2 }));

    matrixDblclick(handle.x, handle.y);
    await settle();

    expect(valueOf('matrix.center_gain_linear')).toBe('0.70711');
    expect(el('matrix-status').textContent).toContain('Centre reset to the factory default');
    expect(el('matrix-status').textContent).toContain('-3.0 dB');

    // Away from every handle, a double-click is not a reset.
    await setNumber('matrix.surround_gain_linear', '2');
    matrixDblclick(30, 10);
    await settle();
    expect(valueOf('matrix.surround_gain_linear')).toBe('2');
  });

  it('refuses the double-click reset while the clip-free policy owns the headroom', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    stubMatrixRect();
    const handle = matrixHandle('headroom', matrixValues({ headroom: 0.37037, clipfree: true }));

    matrixDblclick(handle.x, handle.y);
    await settle();

    expect(valueOf('matrix.headroom_linear')).toBe('0.37037');
    expect(el('matrix-status').textContent).toContain('clip-free');
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

/* A 10 Hz snapshot drives a bar in 100 ms steps -- a staircase, not an
 * instrument. Between snapshots the page runs a real meter's ballistics:
 * instant rise, hold, then a constant dB/s fall towards (never past) the level
 * the device is actually publishing. */
describe('Aius SD DSP editor — meter ballistics', () => {
  /* The needle is pure: the frame ticker owns the clock, so a decay is provable
   * with literal elapsed times instead of a wall-clock wait. */
  it('rises instantly, holds the peak, then falls at a fixed dB per second', async () => {
    await boot();
    const ballistic = { floor: -60, fall: 20, hold: 500 };
    const needle = maths().newNeedle(ballistic);
    expect(needle.value).toBe(-60);

    // A peak meter that lagged on the way up would UNDER-read, the one error it
    // must never make.
    expect(maths().stepNeedle(needle, -6, 0, ballistic)).toBe(-6);
    // The device goes quiet: the peak sits for its hold ...
    expect(maths().stepNeedle(needle, -40, 500, ballistic)).toBe(-6);
    // ... and then falls 20 dB per second, half a second at a time.
    expect(maths().stepNeedle(needle, -40, 500, ballistic)).toBeCloseTo(-16, 9);
    // Never past what the device is publishing: the snapshot is still the truth.
    expect(maths().stepNeedle(needle, -40, 10000, ballistic)).toBe(-40);
    // A louder snapshot wins at once, whatever the needle was doing.
    expect(maths().stepNeedle(needle, -3, 40, ballistic)).toBe(-3);
    // No data at all is the floor, not the last level held for ever.
    expect(maths().stepNeedle(needle, null, 10000, ballistic)).toBe(-60);
  });

  it('carries a level bar between two snapshots instead of stepping it', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);
    expect(el('bar-in-fl-fill').style.width).toBe('89.7%');

    // fl drops 33.8 dB between one snapshot and the next.
    const quiet = fixture('meters-snapshot.json');
    quiet.input[0] = { peak_dbfs: -40, rms_dbfs: -52 };
    device.meters = quiet;
    await vi.advanceTimersByTimeAsync(100);

    // The printed number is the published instant, immediately ...
    expect(el('meter-in-fl-peak').textContent).toBe('-40.0');
    // ... and the needle beside it is still holding the peak it saw.
    expect(el('bar-in-fl-fill').style.width).toBe('89.7%');
    await vi.advanceTimersByTimeAsync(400);
    expect(el('bar-in-fl-fill').style.width).toBe('89.7%');

    // Then it glides, arriving somewhere no 10 Hz snapshot ever named ...
    await vi.advanceTimersByTimeAsync(700);
    const falling = Number.parseFloat(el('bar-in-fl-fill').style.width);
    expect(falling).toBeLessThan(89.7);
    expect(falling).toBeGreaterThan(33.4);

    // ... and settles ON the published level, not on zero.
    await vi.advanceTimersByTimeAsync(3000);
    expect(el('bar-in-fl-fill').style.width).toBe('33.3%');
  });

  it('holds the gain-reduction bar at the deepest reduction and lets it recover', async () => {
    await boot({ meters: { ...fixture('meters-snapshot.json'), limiter_gain_reduction_db: -12 } });
    await vi.advanceTimersByTimeAsync(100);
    // 12 dB of the bar's 24 dB span.
    expect(el('gr-fill').style.width).toBe('50.0%');

    device.meters = { ...fixture('meters-snapshot.json'), limiter_gain_reduction_db: 0 };
    await vi.advanceTimersByTimeAsync(100);
    expect(el('gr-value').textContent).toBe('0.0 dB');
    expect(el('gr-fill').style.width).toBe('50.0%');

    await vi.advanceTimersByTimeAsync(2000);
    expect(el('gr-fill').style.width).toBe('0.0%');
  });

  /* A snapshot that stops arriving is not a signal that decayed: the needles go
   * back to their floors rather than gliding down from a level nothing is
   * publishing any more. */
  it('drops every needle when the meters go away', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);
    expect(el('bar-in-fl-fill').style.width).toBe('89.7%');

    device.meters = fixture('meters-idle.json');
    await vi.advanceTimersByTimeAsync(100);
    expect(el('bar-in-fl-fill').style.width).toBe('0.0%');
    expect(el('bar-in-fl-fill').dataset.zone).toBe('idle');
    expect(el('gr-fill').style.width).toBe('0.0%');
  });
});

/* The analyser: a radix-2 FFT over a raw sample window the meter snapshot
 * carries, drawn as log-spaced bands under the EQ curves. The firmware change
 * that publishes that window lands separately, so the page has to be correct
 * both before and after it arrives. */
describe('Aius SD DSP editor — the analyser', () => {
  /* A SYNTHETIC window, not a captured one: two bin-centred tones at known
   * amplitudes are the only stimulus whose spectrum can be asserted exactly.
   * 1024 samples at 48 kHz put a bin every 46.875 Hz, so tone bins 32 and 128
   * are 1500 Hz and 6000 Hz -- exactly on a bin, which is what makes the
   * Hann-windowed answer exact instead of approximate. Samples are int16-scale
   * integers, the raw PCM the pipeline carries. */
  const FULL_SCALE = 32768;
  const WINDOW_SIZE = 1024;
  const RATE_HZ = 48000;
  const LOUD_DBFS = 20 * Math.log10(0.5); /* -6.0206 */
  const QUIET_DBFS = 20 * Math.log10(0.125); /* -18.0618 */
  const TONES = [
    { bin: 32, amplitude: 0.5 },
    { bin: 128, amplitude: 0.125 },
  ];
  const twoToneWindow = () =>
    Array.from({ length: WINDOW_SIZE }, (unused, sample) =>
      Math.round(
        TONES.reduce(
          (total, tone) =>
            total + tone.amplitude * FULL_SCALE * Math.sin((2 * Math.PI * tone.bin * sample) / WINDOW_SIZE),
          0,
        ),
      ),
    );
  const analyserMeters = (over = {}) => ({
    ...fixture('meters-snapshot.json'),
    analysis_window: twoToneWindow(),
    ...over,
  });
  const SPECTRUM_BANDS = 48;
  const bandOf = (hz) => {
    let index = 0;
    while (index < SPECTRUM_BANDS - 1 && maths().bandEdgeHz(index + 1) <= hz) index++;
    return index;
  };
  const underlay = () => lastFrame('curve').filter((op) => op[0] === 'fillRect');
  const tallest = () => underlay().reduce((best, op) => (op[4] > best[4] ? op : best), underlay()[0]);
  const redraws = () => opsOf('curve').filter((op) => op[0] === 'clearRect').length;

  it('reads both tones back out of the window at their own amplitudes', async () => {
    await boot();
    const bins = maths().spectrumBins(twoToneWindow(), FULL_SCALE);

    expect(bins).toHaveLength(WINDOW_SIZE / 2);
    expect(bins[32]).toBeCloseTo(LOUD_DBFS, 2);
    expect(bins[128]).toBeCloseTo(QUIET_DBFS, 2);
    // Hann spreads a bin-centred tone over its two immediate neighbours at half
    // the amplitude and nowhere else ...
    expect(bins[31]).toBeCloseTo(LOUD_DBFS - 6.0206, 2);
    expect(bins[129]).toBeCloseTo(QUIET_DBFS - 6.0206, 2);
    // ... so two bins out is the 16-bit quantisation floor, not the tone.
    expect(bins[34]).toBeLessThan(-80);
    expect(bins[300]).toBeLessThan(-80);
  });

  it('refuses a window too short to say anything about 20 Hz to 20 kHz', async () => {
    await boot();
    expect(maths().spectrumBins(Array.from({ length: 64 }, () => 1000), FULL_SCALE)).toEqual([]);
    expect(maths().spectrumBins([], FULL_SCALE)).toEqual([]);
  });

  it('maps the bins onto the log-spaced bands the plot shares its axis with', async () => {
    await boot();
    const bands = maths().spectrumBandsOf(maths().spectrumBins(twoToneWindow(), FULL_SCALE), RATE_HZ);

    expect(bands).toHaveLength(SPECTRUM_BANDS);
    // The loudest bin in a band is the band: a spectrum answers "how much is
    // there at this frequency", not "how much across this width".
    expect(bands[bandOf(1500)]).toBeCloseTo(LOUD_DBFS, 2);
    expect(bands[bandOf(6000)]).toBeCloseTo(QUIET_DBFS, 2);
    // A band the window cannot resolve is EMPTY rather than a level invented
    // for it: the first bin of a 1024-point window at 48 kHz is 46.9 Hz, and a
    // band stays narrower than that spacing until ~300 Hz -- so the bottom of
    // the underlay is deliberately gappy and its top is continuous.
    expect(bands[0]).toBe(null);
    expect(bands[bandOf(25)]).toBe(null);
    expect(bands.slice(bandOf(400)).every((band) => band !== null)).toBe(true);
  });

  it('draws nothing, and costs nothing, until a snapshot carries a window', async () => {
    await boot({ meters: { ...fixture('meters-snapshot.json'), analysis_window: [1, 2, 3] } });
    await vi.advanceTimersByTimeAsync(300);

    expect(underlay()).toHaveLength(0);
    expect(el('meter-state').textContent).toBe('live');

    // And the plot is not redrawing per frame either: a page whose firmware
    // never publishes a window pays nothing for the analyser.
    const before = redraws();
    await vi.advanceTimersByTimeAsync(500);
    expect(redraws()).toBe(before);
  });

  it('lights the underlay up on the first snapshot that carries one', async () => {
    await boot({ meters: analyserMeters() });
    await vi.advanceTimersByTimeAsync(100);

    const frame = lastFrame('curve');
    const bars = underlay();
    expect(bars.length).toBeGreaterThan(0);

    // BENEATH the curves: every bar is drawn before the frame's first path.
    expect(frame.indexOf(bars[bars.length - 1])).toBeLessThan(frame.findIndex((op) => op[0] === 'beginPath'));
    // Faded, one colour, never competing with a cascade.
    expect(bars.every((op) => op[6] < 0.25)).toBe(true);
    // The tallest band sits under 1500 Hz on the plot's OWN frequency axis ...
    const loud = tallest();
    expect(loud[1]).toBeLessThanOrEqual(atHz(1500));
    expect(loud[1] + loud[3]).toBeGreaterThanOrEqual(atHz(1500));
    // ... and its height is its own dBFS scale (-96 dBFS to 0 over the canvas),
    // growing up from the bottom, not the +-12 dB the curves are drawn against.
    expect(loud[4]).toBeCloseTo(((LOUD_DBFS + 96) / 96) * 240, 3);
    expect(loud[2] + loud[4]).toBeCloseTo(240, 6);
    // The plot still works: the curves and their handles are drawn over it.
    expect(cascadePaths()).toHaveLength(5);
  });

  it('smooths the bands between windows and puts the underlay away when it stops', async () => {
    await boot({ meters: analyserMeters() });
    await vi.advanceTimersByTimeAsync(100);
    const loud = tallest()[4];

    // The signal stops: the bands FALL rather than collapsing in one snapshot.
    device.meters = analyserMeters({ analysis_window: Array.from({ length: WINDOW_SIZE }, () => 0) });
    await vi.advanceTimersByTimeAsync(100);
    const falling = tallest()[4];
    expect(falling).toBeLessThan(loud);
    expect(falling).toBeGreaterThan(0);

    // The field itself disappears: the underlay goes with it, and the plot
    // stops redrawing per frame.
    device.meters = fixture('meters-snapshot.json');
    await vi.advanceTimersByTimeAsync(100);
    expect(underlay()).toHaveLength(0);
    const before = redraws();
    await vi.advanceTimersByTimeAsync(500);
    expect(redraws()).toBe(before);
  });
});

/* The 2026-07-27 panel moves: presets directly below the effective
 * configuration (item 10), the processing stages between it and the strip
 * (item 4), and each stage's text entry inside its own visual block
 * (items 12 + 13) -- the limiter's controls sharing the panel with its
 * metering being the named case. */
describe('Aius SD DSP editor — page structure', () => {
  it('orders the cards: config, presets, stages, signal flow, live processing', async () => {
    await boot();
    const titles = [...document.querySelectorAll('main > section')].map(
      (section) => section.querySelector('h2').textContent,
    );
    // "Processing stages" was a cluster of five checkboxes with no controls of
    // its own; each moved into the head of the panel it governs.
    expect(titles).toEqual(['Effective configuration', 'Presets', 'Signal flow', 'Live processing']);
  });

  /* "Source" named the DSP MODEL's provenance beside a strip whose whole job is
   * to say what the device is HEARING -- two different sources under one word.
   * The re-read buttons said "Re-read" without saying of what. */
  it('names the config provenance and the two reload affordances after what they do', async () => {
    await boot();
    const labels = [...document.querySelectorAll('.status-grid span')].map((node) => node.textContent);

    expect(labels).toContain('Config');
    expect(labels).not.toContain('Source');
    expect(el('source').textContent).toBe('default');
    expect(el('reload').textContent).toBe('Reload from device');
    expect(el('stale-reload').textContent).toBe('Reload device config');
  });

  it('embeds each stage form inside its own visual block, after the picture', async () => {
    await boot();
    expect(el('card-input-channels').closest('.visual-block').contains(el('curve'))).toBe(true);
    expect(el('card-output-eq').closest('.visual-block').contains(el('curve'))).toBe(true);
    expect(el('card-matrix').closest('.visual-block').contains(el('matrix-diagram'))).toBe(true);
    // The limiter's controls share the panel with its metering (item 12).
    expect(el('card-limiter').closest('.visual-block').contains(el('gr-fill'))).toBe(true);
    expect(el('card-limiter').contains(fieldOf('limiter.threshold_dbfs'))).toBe(true);

    // The picture precedes its text entry in every block.
    const matrixBlock = [...el('card-matrix').closest('.visual-block').children];
    expect(matrixBlock.indexOf(el('card-matrix'))).toBeGreaterThan(
      matrixBlock.indexOf(el('matrix-diagram').parentElement),
    );
    const limiterBlock = [...el('card-limiter').closest('.visual-block').children];
    expect(limiterBlock.indexOf(el('card-limiter'))).toBeGreaterThan(limiterBlock.indexOf(el('gr-history')));

    // The card contract survives the move: markers still mount on each title.
    expect(el('m:card-input-channels').hidden).toBe(true);
    expect(el('m:card-limiter').hidden).toBe(true);
  });

  it('keeps the input-channel table as the dense per-channel escape hatch', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    const grid = document.querySelector('.eq-table--channels');
    // 8 header cells + 6 channels x (name + trim cell + 2 x (sec + 4 fields + reset)).
    expect(grid.children.length).toBe(8 + 6 * (2 + 2 * 6));
    // fl and fr stay separate rows: the table is where a pair diverges.
    expect(grid.contains(fieldOf('input_channels.fl.eq.0.q'))).toBe(true);
    expect(grid.contains(fieldOf('input_channels.fr.eq.1.type'))).toBe(true);
    // Output EQ gets the same treatment in its own two-row table.
    expect(document.querySelector('.eq-table--output').contains(fieldOf('output.eq.1.gain_db'))).toBe(true);
  });
});
