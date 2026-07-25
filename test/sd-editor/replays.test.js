// @vitest-environment happy-dom

/* Regression for the SD-hosted replay manager's meter rendering
 * (firmware/sd/www/replays.html + replays.js). Same harness shape as
 * dsp-editor.test.js: the page is a classic script served off the Teensy's SD
 * card, so this loads the real HTML body, evaluates the real script against it,
 * and stubs `fetch` with the device contract -- no build step, no served URL.
 *
 * Scope is deliberately the meter path. Phase 14b landed `GET /api/meters` in
 * firmware and this page was wrong about it on two counts that predate the
 * chosen wire shape:
 *
 *   1. it read `output.peak_dbfs` as a scalar and ran Number(...).toFixed(1)
 *      over it, which yields NaN for the [L, R] array the pipeline has always
 *      produced;
 *   2. its waveform maths was `y = h/2 - point*h/2`, i.e. it assumed points in
 *      roughly [-1, 1], while the snapshot carries magnitudes -- so every point
 *      drew thousands of pixels off-canvas.
 *
 * Both are asserted below against the settled shape (per-mille integers
 * 0..1000, [L, R] dBFS arrays floored at -120, sequence 0 = never published).
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const www = resolve(here, '../../firmware/sd/www');
const fixture = (name) => JSON.parse(readFileSync(resolve(here, 'fixtures', name), 'utf8'));
const replaysHtml = readFileSync(resolve(www, 'replays.html'), 'utf8');
const replaysSource = readFileSync(resolve(www, 'replays.js'), 'utf8');
const bodyMarkup = replaysHtml
  .slice(replaysHtml.indexOf('<body>') + '<body>'.length, replaysHtml.indexOf('</body>'))
  .replace(/<script[\s\S]*?<\/script>/g, '');

const el = (id) => document.getElementById(id);
const settle = async (rounds = 30) => {
  for (let index = 0; index < rounds; index++) await Promise.resolve();
};

const jsonResponse = (status, body) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => body,
});

let device;
let canvasOps;
const rejections = [];
const trackRejection = (reason) => rejections.push(reason);

function installCanvas() {
  canvasOps = [];
  const context = {
    clearRect: (...args) => canvasOps.push(['clearRect', ...args]),
    beginPath: () => canvasOps.push(['beginPath']),
    closePath: () => canvasOps.push(['closePath']),
    moveTo: (x, y) => canvasOps.push(['moveTo', x, y]),
    lineTo: (x, y) => canvasOps.push(['lineTo', x, y]),
    stroke: () => canvasOps.push(['stroke']),
    fill: () => canvasOps.push(['fill']),
    strokeStyle: '',
    fillStyle: '',
  };
  window.HTMLCanvasElement.prototype.getContext = () => context;
}

function createDevice(options = {}) {
  const stub = {
    meters: 'meters' in options ? options.meters : fixture('meters-snapshot.json'),
    status: { state: 'playing', id: 'film-51', queue_words: 1024, watermark_words: 2048 },
    files: [],
    calls: [],
  };
  globalThis.fetch = vi.fn(async (url, init = {}) => {
    stub.calls.push({ url, method: init.method || 'GET' });
    if (url === '/api/replays') return jsonResponse(200, { files: stub.files });
    if (url === '/api/replays/status') return jsonResponse(200, stub.status);
    if (url === '/api/meters') {
      return stub.meters ? jsonResponse(200, stub.meters) : jsonResponse(500, { ok: false, error: 'unavailable' });
    }
    return jsonResponse(404, { ok: false, error: 'not_found' });
  });
  return stub;
}

async function boot(options) {
  device = createDevice(options);
  document.body.innerHTML = bodyMarkup;
  installCanvas();
  new Function(replaysSource)();
  await settle();
  return device;
}

/* The envelope path, as flat [op, x, y] triples in draw order. */
const envelope = () => canvasOps.filter((op) => op[0] === 'moveTo' || op[0] === 'lineTo');

beforeEach(() => {
  rejections.length = 0;
  process.on('unhandledRejection', trackRejection);
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
  process.off('unhandledRejection', trackRejection);
  expect(rejections).toEqual([]);
});

describe('Aius SD replay manager — meters', () => {
  it('reports both output channels from the [L, R] arrays', async () => {
    await boot();

    const summary = el('meter-summary').textContent;
    expect(summary).not.toContain('NaN');
    expect(summary).toContain('Output peak -3.1 / -3.4 dBFS');
    expect(summary).toContain('RMS -14.6 / -14.9 dBFS');
    expect(summary).toContain('latency 10667');
  });

  it('draws the per-mille waveform mirrored about the centre line', async () => {
    await boot();

    // [0, 500, 1000, 500, 0] per-mille across a 960x180 canvas:
    // x = index * 240, y = 90 -/+ (permille / 1000) * 90.
    const points = envelope();
    expect(points).toHaveLength(10);
    expect(points[0]).toEqual(['moveTo', 0, 90]);
    expect(points[1]).toEqual(['lineTo', 240, 45]);
    expect(points[2]).toEqual(['lineTo', 480, 0]);
    expect(points[4]).toEqual(['lineTo', 960, 90]);
    // Every drawn y stays inside the canvas -- the bug this replaces put a raw
    // 8388607 sample unit at y = 90 - 8388607 * 90.
    for (const [, , y] of points) {
      expect(y).toBeGreaterThanOrEqual(0);
      expect(y).toBeLessThanOrEqual(180);
    }
    // The return leg mirrors below the centre line and closes at the origin.
    expect(points[7]).toEqual(['lineTo', 480, 180]);
    expect(points[9]).toEqual(['lineTo', 0, 90]);
  });

  it('caps rendering at the contract 128 points', async () => {
    await boot({ meters: { ...fixture('meters-snapshot.json'), waveform: Array.from({ length: 400 }, () => 1000) } });

    // One moveTo plus 127 lineTo forward, then 128 mirrored back.
    expect(envelope()).toHaveLength(256);
  });

  it('renders a never-published snapshot as no data rather than silence', async () => {
    await boot({ meters: fixture('meters-idle.json') });

    expect(el('meter-summary').textContent).toBe('No meter snapshot published yet.');
    expect(envelope()).toHaveLength(0);
  });

  it('renders levels at the -120 dBFS floor as silence', async () => {
    await boot({ meters: fixture('meters-silent.json') });

    const summary = el('meter-summary').textContent;
    expect(summary).toContain('Output peak -0.1 / -inf dBFS');
    expect(summary).toContain('RMS -3.2 / -inf dBFS');
  });

  it('surfaces a meters failure without disturbing the replay status it already drew', async () => {
    await boot({ meters: null });

    expect(el('state').textContent).toBe('playing');
    expect(el('active').textContent).toBe('film-51');
    expect(el('decoder').textContent).toBe('unavailable');
  });

  it('keeps polling meters on the 1 s status tick', async () => {
    await boot();
    const afterBoot = device.calls.filter((call) => call.url === '/api/meters').length;

    await vi.advanceTimersByTimeAsync(3000);

    expect(device.calls.filter((call) => call.url === '/api/meters').length).toBe(afterBoot + 3);
  });
});
