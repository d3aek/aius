// @vitest-environment happy-dom

/* Regression for the SD-hosted device status page (firmware/sd/www/status.html
 * + status.js). Same harness as dsp-editor.test.js and replays.test.js: the
 * page is a classic script served off the Teensy's SD card, so this loads the
 * real HTML body, evaluates the real sd-shared.js and then the real status.js
 * against it, and stubs `fetch` with the device contract.
 *
 * Fixture provenance: SNAPSHOT below is the exact wire shape
 * `http_status_json()` emits (firmware/src/optical_main.cpp) -- authored to
 * that serializer rather than captured from a device, and deliberately in its
 * integer spelling: `state`, `carrier`, `format`, `link`, `sd_present`,
 * `config_valid`, `recovery_safe` and every `tx_gate` member are 0/1 or small
 * enumerations on the wire even where swagger types them as booleans or
 * strings. The `tx_gate` object is swagger's TxGate schema: open,
 * carrier_locked, replay_running, each 0 or 1, latched once per epoch.
 *
 * What this pins beyond rendering: the input-source derivation, which is the
 * question the page exists to answer and the one thing the device never
 * answers itself (carrier -> optical carrier, else the gate's replay term ->
 * replay, else none), and the unreachable degradation -- a poll failure must
 * dim every grid and say so, never leave a confident-looking snapshot on
 * screen (2026-07-27 UX report finding P4, inherited from the replay page).
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const www = resolve(here, '../../firmware/sd/www');
const statusHtml = readFileSync(resolve(www, 'status.html'), 'utf8');
const sharedSource = readFileSync(resolve(www, 'sd-shared.js'), 'utf8');
const statusSource = readFileSync(resolve(www, 'status.js'), 'utf8');
const bodyMarkup = statusHtml
  .slice(statusHtml.indexOf('<body>') + '<body>'.length, statusHtml.indexOf('</body>'))
  .replace(/<script[\s\S]*?<\/script>/g, '');

const el = (id) => document.getElementById(id);
const text = (id) => el(id).textContent;
const settle = async (rounds = 30) => {
  for (let index = 0; index < rounds; index++) await Promise.resolve();
};

const jsonResponse = (status, body) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => body,
  text: async () => JSON.stringify(body),
});

/* A healthy board: optical carrier locked, PCM classified, gate open, DHCP
 * online, card mounted, no faults. */
const SNAPSHOT = {
  version: 1,
  state: 3,
  carrier: 1,
  format: 1,
  sample_rate: 48000,
  blocks: 20492,
  overruns: 0,
  source: 'sd',
  sd_present: 1,
  sd_filesystem: 'fat32',
  sd_total_bytes: 31914983424,
  sd_used_bytes: 1073741824,
  sd_free_bytes: 30841241600,
  config_valid: 1,
  config_error_code: 0,
  config_error: '',
  config_error_count: 0,
  network_mode: 'dhcp',
  ip: '192.0.2.66',
  link: 1,
  network_restarts: 0,
  dhcp_pending: 0,
  dhcp_discovers: 0,
  dhcp_updates: 1,
  reset_source: 64,
  http_server_rebinds: 0,
  cpu_temp_c: 47.25,
  cpu_temp_valid: 1,
  cpu_temp_max_c: 52.5,
  cpu_temp_age_ms: 812,
  build_revision: 'b5f9a1351',
  build_utc: '2026-07-27T09:12:44Z',
  event_sequence: 12,
  event_overflows: 0,
  network_drops: 0,
  network_budget_overruns: 0,
  http_requests: 431,
  http_rejections: 0,
  http_client_drops: 0,
  http_service_overruns: 0,
  network_state: 2,
  recovery_safe: 0,
  network_last_fault: 0,
  last_checkpoint: 9,
  network_generation: 1,
  boot_generation: 250,
  watchdog_early_resets: 0,
  loop_epoch: 48211,
  tx_gate: { open: 1, carrier_locked: 1, replay_running: 0 },
};

const snapshot = (overrides = {}) => Object.assign({}, SNAPSHOT, overrides);

const PILLS = ['audio-pill', 'config-pill', 'network-pill', 'storage-pill', 'board-pill'];
const GRIDS = ['audio-grid', 'config-grid', 'network-grid', 'storage-grid', 'board-grid'];

let device;
const rejections = [];
const trackRejection = (reason) => rejections.push(reason);

function createDevice(options = {}) {
  const stub = { status: 'status' in options ? options.status : SNAPSHOT, calls: [] };
  globalThis.fetch = vi.fn(async (url, init = {}) => {
    stub.calls.push({ url, method: init.method || 'GET', headers: init.headers || {} });
    if (url === '/api/status') {
      return stub.status ? jsonResponse(200, stub.status) : jsonResponse(503, { ok: false, error: 'unavailable' });
    }
    return jsonResponse(404, { ok: false, error: 'not_found' });
  });
  return stub;
}

async function boot(options) {
  device = createDevice(options);
  document.body.innerHTML = bodyMarkup;
  new Function(sharedSource)();
  new Function(statusSource)();
  await settle();
  return device;
}

/* The signal-flow line, as the "<stage> <value>" pairs it renders. */
const flowStages = () =>
  Array.from(el('flow').querySelectorAll('span'))
    .filter((span) => span.querySelector('b'))
    .map((span) => ({ text: span.textContent, tone: span.dataset.tone }));

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

describe('Aius SD device status — audio path', () => {
  it('renders the snapshot into the audio card and names the integer vocabularies', async () => {
    await boot();

    expect(text('input-source')).toBe('optical carrier');
    expect(text('audio-state')).toBe('PCM (3)');
    expect(text('carrier')).toBe('locked');
    expect(text('format')).toBe('PCM (IEC 60958)');
    expect(text('sample-rate')).toBe('48000 Hz');
    expect(text('blocks')).toBe('20492');
    expect(text('overruns')).toBe('0');
    expect(text('gate-open')).toBe('open');
    expect(text('gate-carrier')).toBe('locked');
    expect(text('gate-replay')).toBe('stopped');
    for (const pill of PILLS) expect(text(pill)).toBe('live');
  });

  it('draws the transmit gate as input -> DSP -> gate -> output', async () => {
    await boot();

    expect(flowStages()).toEqual([
      { text: 'input optical carrier', tone: 'ok' },
      { text: 'DSP PCM (IEC 60958)', tone: 'ok' },
      { text: 'gate open', tone: 'ok' },
      { text: 'output live', tone: 'ok' },
    ]);
  });

  it('derives a replay as the input source when no carrier is locked', async () => {
    await boot({
      status: snapshot({
        state: 2,
        carrier: 0,
        format: 1,
        sample_rate: 0,
        tx_gate: { open: 1, carrier_locked: 0, replay_running: 1 },
      }),
    });

    expect(text('input-source')).toBe('replay');
    expect(text('carrier')).toBe('absent');
    expect(text('audio-state')).toBe('no carrier (2)');
    expect(text('sample-rate')).toBe('-- (no carrier)');
    expect(text('gate-replay')).toBe('running');
    expect(el('poll-status').textContent).toContain('Input source: replay');
  });

  it('derives no input source with a dark gate, and says the output is silent', async () => {
    await boot({
      status: snapshot({
        state: 2,
        carrier: 0,
        format: 0,
        sample_rate: 0,
        blocks: 0,
        tx_gate: { open: 0, carrier_locked: 0, replay_running: 0 },
      }),
    });

    expect(text('input-source')).toBe('none');
    expect(text('format')).toBe('unknown');
    expect(text('gate-open')).toBe('dark');
    expect(flowStages()).toEqual([
      { text: 'input none', tone: 'off' },
      { text: 'DSP unknown', tone: 'off' },
      { text: 'gate dark', tone: 'off' },
      { text: 'output silent', tone: 'off' },
    ]);
  });

  it('highlights overruns once they are non-zero', async () => {
    await boot({ status: snapshot({ overruns: 4 }) });

    expect(text('overruns')).toBe('4');
    expect(el('overruns').dataset.tone).toBe('warn');
  });
});

describe('Aius SD device status — configuration, network, storage, board', () => {
  it('renders the effective DSP source and a clean configuration', async () => {
    await boot();

    expect(text('dsp-source')).toBe('sd');
    expect(text('config-valid')).toBe('yes');
    expect(text('config-error')).toBe('none');
    expect(text('config-errors')).toBe('0');
    expect(el('config-valid').dataset.tone).toBeUndefined();
  });

  it('surfaces a configuration fault with its code and marks it', async () => {
    await boot({
      status: snapshot({ config_valid: 0, config_error_code: 7, config_error: 'matrix_bounds', config_error_count: 2 }),
    });

    expect(text('config-valid')).toBe('no');
    expect(el('config-valid').dataset.tone).toBe('warn');
    expect(text('config-error')).toBe('matrix_bounds (code 7)');
    expect(text('config-errors')).toBe('2');
  });

  it('names the network supervisor state and fault rather than serving raw integers alone', async () => {
    await boot();

    expect(text('network-mode')).toBe('dhcp');
    expect(text('ip')).toBe('192.0.2.66');
    expect(text('link')).toBe('up');
    expect(text('network-state')).toBe('online (2)');
    expect(text('network-fault')).toBe('none (0)');
    expect(text('network-generation')).toBe('1');
    expect(text('network-restarts')).toBe('0');
  });

  it('marks a down link and a real network fault', async () => {
    await boot({ status: snapshot({ link: 0, network_state: 4, network_last_fault: 1, network_restarts: 3 }) });

    expect(text('link')).toBe('down');
    expect(el('link').dataset.tone).toBe('warn');
    expect(text('network-state')).toBe('backoff (4)');
    expect(text('network-fault')).toBe('dhcp-timeout (1)');
    expect(el('network-fault').dataset.tone).toBe('warn');
  });

  it('renders card capacity through the shared byte formatter', async () => {
    await boot();

    expect(text('sd-present')).toBe('mounted');
    expect(text('sd-filesystem')).toBe('fat32');
    /* The shared formatter's GiB tier (added right after this suite first
     * flagged 30436.5 MiB): whole cards read in GiB, files stay KiB/MiB. */
    expect(text('sd-free')).toBe('28.7 GiB');
    expect(text('sd-used')).toBe('1.0 GiB of 29.7 GiB');
  });

  it('says an absent card is absent instead of showing zero bytes free', async () => {
    await boot({
      status: snapshot({ sd_present: 0, sd_filesystem: 'none', sd_total_bytes: 0, sd_used_bytes: 0, sd_free_bytes: 0 }),
    });

    expect(text('sd-present')).toBe('absent');
    expect(el('sd-present').dataset.tone).toBe('warn');
    expect(text('sd-free')).toBe('0.0 KiB');
  });

  it('renders the retained boot ledger, the checkpoint vocabulary and the running image', async () => {
    await boot();

    expect(text('boot-generation')).toBe('250');
    expect(text('reset-source')).toBe('64 (0x40)');
    expect(text('watchdog-early')).toBe('0');
    expect(text('recovery-safe')).toBe('no');
    expect(text('last-checkpoint')).toBe('epoch-complete (9)');
    expect(text('loop-epoch')).toBe('48211');
    expect(text('cpu-temp')).toBe('47.3 °C');
    expect(text('cpu-temp-max')).toBe('52.5 °C');
    expect(text('build-revision')).toBe('b5f9a1351');
    expect(text('build-utc')).toBe('2026-07-27T09:12:44Z');
  });

  it('marks recovery-safe mode and early watchdog resets, and refuses to print an unsampled temperature', async () => {
    await boot({
      status: snapshot({
        recovery_safe: 1,
        watchdog_early_resets: 2,
        last_checkpoint: 5,
        cpu_temp_valid: 0,
        cpu_temp_c: 0,
        cpu_temp_max_c: 0,
      }),
    });

    expect(text('recovery-safe')).toBe('yes -- bounded USB only');
    expect(el('recovery-safe').dataset.tone).toBe('warn');
    expect(text('watchdog-early')).toBe('2');
    expect(el('watchdog-early').dataset.tone).toBe('warn');
    expect(text('last-checkpoint')).toBe('http (5)');
    expect(text('cpu-temp')).toBe('not sampled');
    expect(text('cpu-temp-max')).toBe('not sampled');
  });
});

describe('Aius SD device status — polling', () => {
  it('polls once a second as a public read, with no token anywhere on the page', async () => {
    await boot();
    const afterBoot = device.calls.length;

    await vi.advanceTimersByTimeAsync(3000);

    expect(device.calls.length).toBe(afterBoot + 3);
    for (const call of device.calls) {
      expect(call.method).toBe('GET');
      expect(call.headers['X-Aius-SD-Auth']).toBeUndefined();
    }
    expect(document.getElementById('token')).toBeNull();
    expect(bodyMarkup).not.toContain('type="password"');
  });

  it('degrades to unreachable with every grid dimmed when the poll fails', async () => {
    device = await boot();
    expect(text('input-source')).toBe('optical carrier');

    device.status = null;
    await vi.advanceTimersByTimeAsync(1000);
    await settle();

    for (const pill of PILLS) {
      expect(text(pill)).toBe('unreachable');
      expect(el(pill).dataset.state).toBe('unreachable');
    }
    for (const grid of GRIDS) expect(el(grid).dataset.stale).toBe('true');
    expect(el('flow').dataset.stale).toBe('true');
    expect(el('poll-status').textContent).toContain('stale');
    /* The last good values stay on screen -- dimmed, not blanked. */
    expect(text('input-source')).toBe('optical carrier');
  });

  it('recovers the live pills and undims the grids on the next good poll', async () => {
    device = await boot({ status: null });
    expect(text('audio-pill')).toBe('unreachable');

    device.status = SNAPSHOT;
    await vi.advanceTimersByTimeAsync(1000);
    await settle();

    for (const pill of PILLS) expect(text(pill)).toBe('live');
    for (const grid of GRIDS) expect(el(grid).dataset.stale).toBeUndefined();
    expect(el('flow').dataset.stale).toBeUndefined();
  });

  it('links only to its sibling pages, by relative href', async () => {
    await boot();

    const hrefs = Array.from(document.querySelectorAll('nav a')).map((anchor) => anchor.getAttribute('href'));
    /* The firmware upgrade page joined the strip with OTA slice 3A. */
    expect(hrefs).toEqual(['index.html', 'replays.html', 'upgrade.html']);
  });
});
