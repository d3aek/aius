// @vitest-environment happy-dom

/* Regression for the SD-hosted DSP control surface (firmware/sd/www/index.html
 * + dsp-editor.js). The page is a classic script served off the Teensy's SD
 * card, so the harness loads the real index.html body, evaluates the real
 * dsp-editor.js against it, and stubs `fetch` with the device contract -- no
 * build step and no served URL, which is the cheapest harness that still
 * exercises the shipped files byte-for-byte.
 *
 * Fixture provenance: `dsp-config-*.json` are the exact wire shape
 * `http_dsp_config_json()` emits (firmware/src/optical_main.cpp) and the
 * swagger `DspConfig` example; the SD one additionally carries every EQ type
 * and the clip-free matrix policy. `meters-*.json` are the settled
 * `GET /api/meters` shape that `http_meters_json()` emits as of Phase 14b --
 * six positional dBFS input entries floored at -120, `[L, R]` output arrays,
 * and a per-mille integer waveform -- authored to the contract rather than
 * captured from a device, and covering the live, all-at-the-floor, and
 * never-published (`sequence == 0`) cases.
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const www = resolve(here, '../../firmware/sd/www');
const fixture = (name) => JSON.parse(readFileSync(resolve(here, 'fixtures', name), 'utf8'));
const indexHtml = readFileSync(resolve(www, 'index.html'), 'utf8');
const editorSource = readFileSync(resolve(www, 'dsp-editor.js'), 'utf8');
const bodyMarkup = indexHtml
  .slice(indexHtml.indexOf('<body>') + '<body>'.length, indexHtml.indexOf('</body>'))
  .replace(/<script[\s\S]*?<\/script>/g, '');

const CHANNELS = ['fl', 'fr', 'fc', 'lfe', 'sl', 'sr'];

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
    config: 'config' in options ? options.config : fixture('dsp-config-default.json'),
    meters: 'meters' in options ? options.meters : fixture('meters-snapshot.json'),
    transportFailure: false,
    put: options.put || (() => jsonResponse(200, { ok: true })),
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
    return jsonResponse(404, { ok: false, error: 'not_found' });
  });
  return stub;
}

async function boot(options) {
  device = createDevice(options);
  document.body.innerHTML = bodyMarkup;
  installCanvas();
  new Function(editorSource)();
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
    expect(valueOf('matrix.center_gain_linear')).toBe('0.7071067811865475');
    expect(valueOf('matrix.surround_gain_linear')).toBe('0.7071067811865475');
    expect(valueOf('matrix.lfe_gain_linear')).toBe('0');
    expect(valueOf('matrix.headroom_linear')).toBe('1');
    expect(fieldOf('matrix.clipfree').checked).toBe(false);
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(false);
    expect(fieldOf('limiter.enabled').checked).toBe(false);
    expect(valueOf('limiter.threshold_dbfs')).toBe('0');
    expect(valueOf('limiter.attack_ms')).toBe('1');
    expect(valueOf('limiter.release_ms')).toBe('100');
    expect(valueOf('limiter.lookahead_ms')).toBe('0');
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

    expect(fieldOf('matrix.clipfree').checked).toBe(true);
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(true);
    // 1 / (1 + 0.85 + 0.6 + |0.25|), resolved client-side exactly as the DSP does.
    expect(Number(valueOf('matrix.headroom_linear'))).toBeCloseTo(0.37037037037037035, 12);
    expect(fieldOf('limiter.enabled').checked).toBe(true);
    expect(valueOf('limiter.threshold_dbfs')).toBe('-1.5');
    expect(valueOf('limiter.lookahead_ms')).toBe('2');
  });

  it('reports the serialized model size against the device cap', async () => {
    await boot({ config: fixture('dsp-config-sd.json') });
    const [bytes, cap] = el('model-bytes').textContent.split(' / ').map(Number);
    expect(cap).toBe(12288);
    expect(bytes).toBeGreaterThan(0);
    expect(bytes).toBeLessThan(cap);
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
});

describe('Aius SD DSP editor — bounds validation', () => {
  const cases = [
    ['input_channels.fl.gain_db', '13', 'Front left trim must be between -60 and 12 dB.'],
    ['input_channels.lfe.gain_db', '-61', 'LFE trim must be between -60 and 12 dB.'],
    ['matrix.center_gain_linear', '4.5', 'Centre gain must be between 0 and 4 linear.'],
    ['matrix.surround_gain_linear', '-0.1', 'Surround gain must be between 0 and 4 linear.'],
    ['matrix.lfe_gain_linear', '9', 'LFE gain must be between 0 and 4 linear.'],
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

  it('surfaces a rejected write without touching the loaded model', async () => {
    await boot({ put: () => jsonResponse(400, { ok: false, error: 'dsp_config_invalid' }) });
    el('token').value = '0123456789abcdef';

    await click('apply');

    expect(el('apply-state').textContent).toBe('error');
    expect(el('errors').textContent).toContain('dsp_config_invalid');
    expect(el('generation').textContent).toBe('0');
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
    expect(el('meter-latency').textContent).toBe('10667 us');
    expect(el('meter-cycles').textContent).toBe('41230 / 58800');
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
    const afterFirst = device.calls.filter((call) => call.url === '/api/meters').length;
    await vi.advanceTimersByTimeAsync(1000);
    expect(device.calls.filter((call) => call.url === '/api/meters').length).toBe(afterFirst + 10);

    // Once the device starts publishing, the page goes live on the next poll.
    device.meters = fixture('meters-snapshot.json');
    await vi.advanceTimersByTimeAsync(100);
    expect(el('meter-state').textContent).toBe('live');
    expect(el('meter-sequence').textContent).toBe('1287');
  });

  it('draws the waveform envelope mirrored about the centre line', async () => {
    await boot();
    await vi.advanceTimersByTimeAsync(100);

    const paths = canvasOps.reduce((groups, op) => {
      if (op[0] === 'beginPath') groups.push([]);
      else if (groups.length > 0) groups[groups.length - 1].push(op);
      return groups;
    }, []);
    expect(paths).toHaveLength(2);

    const envelope = paths[1].filter((op) => op[0] === 'moveTo' || op[0] === 'lineTo');
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

    const points = canvasOps.filter((op) => op[0] === 'lineTo' || op[0] === 'moveTo');
    // Centre line (moveTo + lineTo) plus the mirrored envelope of 128 points.
    expect(points).toHaveLength(2 + 128 * 2);
  });

  it('degrades gracefully and backs off when /api/meters is absent', async () => {
    await boot({ meters: null });
    await vi.advanceTimersByTimeAsync(100);

    expect(el('meter-state').textContent).toBe('unavailable');
    expect(el('meter-summary').textContent).toContain('HTTP 404');
    expect(el('meter-in-fl-peak').textContent).toBe('--');
    // The rest of the page is unaffected.
    expect(el('apply-state').textContent).toBe('loaded');
    expect(valueOf('matrix.headroom_linear')).toBe('1');

    const afterFirst = device.calls.filter((call) => call.url === '/api/meters').length;
    await vi.advanceTimersByTimeAsync(1000);
    expect(device.calls.filter((call) => call.url === '/api/meters').length).toBe(afterFirst);

    await vi.advanceTimersByTimeAsync(4100);
    expect(device.calls.filter((call) => call.url === '/api/meters').length).toBe(afterFirst + 1);
  });
});

describe('Aius SD DSP editor — transport failures', () => {
  it('shows a visible error when the config read fails outright', async () => {
    device = createDevice();
    device.transportFailure = true;
    document.body.innerHTML = bodyMarkup;
    installCanvas();
    new Function(editorSource)();
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
