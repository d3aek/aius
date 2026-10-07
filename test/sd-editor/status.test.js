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
  model_origin: 'put',
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
  /* The forensic objects, in the shapes swagger's AbState / PrevBoot /
   * FaultRecord / OtaDark / StallDiag schemas pin. A healthy board: slot B
   * committed with no trial open, the previous boot ended without aborting,
   * no crash record, the last activation window completed, no stall. */
  ab: {
    slot: 2,
    record_valid: true,
    active_slot: 2,
    pending_slot: 0,
    sequence: 9,
    trial_budget: 0,
    ledger_valid: true,
    ledger_boots: 1,
    ledger_fell_back: false,
    rollback_observed: false,
  },
  prev_boot: { final_checkpoint: 5, first_abort: 254 },
  fault_record: null,
  ota_dark: {
    state: 4,
    phase: 4,
    fault: 0,
    rehearsal: false,
    primitive_fault: 0,
    step_address: 0,
    compare_offset: 4294967295,
    programmed_bytes: 570368,
    verified_bytes: 570368,
    steps_taken: 2368,
    steps_total: 2368,
    image_digest_prefix: 'f1a4e7960f17aa77',
    dark_window_cycles: 3540837738,
  },
  stall_diag: {
    events: 0,
    last_us: 0,
    last_deficit_ms: 0,
    max_deficit_ms: 0,
    witness_ticks: 830318,
    isr_samples: 0,
    iabr: ['00000000', '00000000', '00000000', '00000000', '00000000'],
    pc: '00000000',
    lr: '00000000',
    flexspi_intr: '00000040',
    flexspi2_intr: '00000040',
    flexspi2_sts1: '00000000',
  },
};

/* Deleting a key is how an OLDER image is spelled -- `undefined` (the image
 * predates the key) and `null` (the image says there is nothing to report) are
 * different answers and the page must not merge them. */
const without = (...keys) => {
  const body = snapshot();
  for (const key of keys) delete body[key];
  return body;
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
      return stub.status ? jsonResponse(200, stub.status) : jsonResponse(500, { ok: false, error: 'unavailable' });
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
  it('names the DSP model and the stager that applied it, beside a loaded boot config', async () => {
    await boot();

    expect(text('dsp-source')).toBe('loaded from card');
    expect(text('dsp-origin')).toBe('DSP editor apply (PUT)');
    expect(text('config-file')).toBe('loaded');
    expect(text('config-errors')).toBe('0');
    expect(el('config-file').dataset.tone).toBeUndefined();
  });

  /* The defect this card shipped with: a device with no /aius/config.json --
   * every device, since nothing writes one -- read "Configuration valid: no /
   * not loaded (code 10) / 1 error" in warning colour forever, next to the DSP
   * model source. Operators read it as a verdict on their DSP editor apply,
   * which writes a different file entirely and cannot change this one. */
  it('reads an absent boot config as the normal state, not a standing fault', async () => {
    await boot({
      status: snapshot({ config_valid: 0, config_error_code: 10, config_error: 'not loaded', config_error_count: 1 }),
    });

    expect(text('config-file')).toContain('absent');
    expect(text('config-file')).toContain('normal state');
    expect(el('config-file').dataset.tone).toBeUndefined();
    expect(text('config-errors')).toBe('1 (the absent file above)');
    expect(el('config-errors').dataset.tone).toBeUndefined();
  });

  it('surfaces a refused boot config with its code and marks it', async () => {
    await boot({
      status: snapshot({ config_valid: 0, config_error_code: 4, config_error: 'malformed JSON', config_error_count: 2 }),
    });

    expect(text('config-file')).toContain('malformed JSON (code 4)');
    expect(el('config-file').dataset.tone).toBe('warn');
    expect(text('config-errors')).toBe('2');
    expect(el('config-errors').dataset.tone).toBe('warn');
  });

  it('says a boot-loaded model came from the card rather than from an apply', async () => {
    await boot({ status: snapshot({ source: 'sd', model_origin: 'sd' }) });

    expect(text('dsp-source')).toBe('loaded from card');
    expect(text('dsp-origin')).toBe('boot load of /aius/dsp.json');
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
    /* 9 is the AUDIO checkpoint, not epoch-complete: output-jitter Phase B
     * inserted it and only 10 now means the epoch finished. This page's list
     * kept the pre-audio vocabulary and renamed every value from 9 up, so a
     * board that stalled in audio reported the one word meaning it had not. */
    expect(text('last-checkpoint')).toBe('audio (9)');
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

  it('names a completed epoch as the only value that means one', async () => {
    await boot({ status: snapshot({ last_checkpoint: 10 }) });

    expect(text('last-checkpoint')).toBe('epoch-complete (10)');
  });
});

describe('Aius SD device status — firmware slot and forensics', () => {
  it('names the running slot, the committed boot record and the trial ledger', async () => {
    await boot();

    expect(text('fw-slot')).toBe('B (2)');
    expect(el('fw-slot').dataset.tone).toBeUndefined();
    expect(text('fw-record')).toBe('committed slot B (2) · no trial open · sequence 9');
    expect(text('fw-ledger')).toBe('1 trial boots launched for this record');
    expect(el('fw-ledger').dataset.tone).toBeUndefined();
  });

  /* The state the whole A/B design exists for: this boot is the probation
   * boot, and nothing on the page said so before. */
  it('marks a slot running on trial against the committed one', async () => {
    await boot({
      status: snapshot({
        ab: Object.assign({}, SNAPSHOT.ab, { slot: 1, active_slot: 2, pending_slot: 1, trial_budget: 3, sequence: 10 }),
      }),
    });

    expect(text('fw-slot')).toBe('A (1) -- on trial, not yet committed');
    expect(el('fw-slot').dataset.tone).toBe('warn');
    expect(text('fw-record')).toBe('committed slot B (2) · trial slot A (1) · sequence 10 · trial budget 3 boots');
    expect(el('fw-record').dataset.tone).toBe('warn');
  });

  it('marks a spent trial budget and a persisted rollback', async () => {
    await boot({
      status: snapshot({
        ab: Object.assign({}, SNAPSHOT.ab, { ledger_boots: 3, ledger_fell_back: true, rollback_observed: true }),
      }),
    });

    expect(text('fw-ledger')).toContain('the chooser booted the committed slot');
    expect(text('fw-ledger')).toContain('this boot persisted a failed trial rollback');
    expect(el('fw-ledger').dataset.tone).toBe('warn');
  });

  it('renders the previous boot ledger and marks a first aborting checkpoint', async () => {
    await boot();
    expect(text('prev-boot')).toBe('final checkpoint http (5) · first aborting checkpoint none');
    expect(el('prev-boot').dataset.tone).toBeUndefined();

    await boot({ status: snapshot({ prev_boot: { final_checkpoint: 8, first_abort: 8 } }) });
    expect(text('prev-boot')).toBe('final checkpoint sd (8) · first aborting checkpoint sd (8)');
    expect(el('prev-boot').dataset.tone).toBe('warn');
  });

  it('spells an undecodable previous-boot ledger rather than printing 65535', async () => {
    await boot({ status: snapshot({ prev_boot: { final_checkpoint: 65535, first_abort: 255 } }) });

    expect(text('prev-boot')).toContain('retained ledger invalid');
    expect(text('prev-boot')).toContain('latch invalid');
  });

  it('renders a retained crash record with its addr2line input, and dates it honestly', async () => {
    await boot({
      status: snapshot({
        fault_record: {
          ipsr: 0,
          cfsr: '0x00000082',
          hfsr: '0x40000000',
          mmfar: '0x00000004',
          bfar: '0xE000ED38',
          return_address: '0x0000A17C',
          xpsr: '0x61000000',
          temp_c: 48.5,
          uptime_s: 1204,
        },
      }),
    });

    expect(text('fault-record')).toContain('return 0x0000A17C');
    expect(text('fault-record')).toContain('CFSR 0x00000082');
    expect(text('fault-record')).toContain('thread mode');
    expect(text('fault-record')).toContain('48.5 °C at 1,204 s uptime');
    expect(text('fault-record')).toContain('never cleared, so it may predate this image');
    expect(el('fault-record').dataset.tone).toBe('warn');
  });

  it('reads a null crash record as no fault, and an absent key as an older image', async () => {
    await boot();
    expect(text('fault-record')).toContain('none recorded');
    expect(el('fault-record').dataset.tone).toBeUndefined();

    /* The deployed slot-B image (fd9d35fd3f95) predates both forensic keys:
     * "not reported by this image" and "none recorded" are different facts and
     * printing the second here would invent a clean bill of health. */
    await boot({ status: without('fault_record', 'prev_boot') });
    expect(text('fault-record')).toBe('not reported by this image');
    expect(text('prev-boot')).toBe('not reported by this image');
    expect(el('fault-record').dataset.tone).toBeUndefined();
  });

  it('renders a completed dark window with its image identity and duration', async () => {
    await boot();

    expect(text('ota-dark')).toContain('completed (4)');
    expect(text('ota-dark')).toContain('phase ready-to-reset (4)');
    expect(text('ota-dark')).toContain('2,368 of 2,368 steps');
    expect(text('ota-dark')).toContain('570,368 B programmed, 570,368 verified');
    expect(text('ota-dark')).toContain('image f1a4e7960f17aa77');
    expect(text('ota-dark')).toContain('5.90 s dark');
    expect(el('ota-dark').dataset.tone).toBeUndefined();
  });

  it('marks a faulted dark window and names the compare offset that convicted it', async () => {
    await boot({
      status: snapshot({
        ota_dark: Object.assign({}, SNAPSHOT.ota_dark, {
          state: 3,
          phase: 5,
          fault: 7,
          primitive_fault: 2,
          step_address: 1638400,
          compare_offset: 4096,
          verified_bytes: 4096,
        }),
      }),
    });

    expect(text('ota-dark')).toContain('faulted (3)');
    expect(text('ota-dark')).toContain('fault 7 (primitive 2) at 0x190000');
    expect(text('ota-dark')).toContain('first bad compare at image offset 4,096');
    expect(el('ota-dark').dataset.tone).toBe('warn');
  });

  it('says a rehearsal stamp is a rehearsal', async () => {
    await boot({ status: snapshot({ ota_dark: Object.assign({}, SNAPSHOT.ota_dark, { rehearsal: true }) }) });

    expect(text('ota-dark')).toContain('REHEARSAL');
    expect(text('ota-dark')).toContain('the boot region was never touched');
  });

  it('renders the stall witness quietly at zero and marks any real stall', async () => {
    await boot();
    expect(text('stall-witness')).toBe('no epoch over 100 ms · 830,318 witness ticks');
    expect(el('stall-witness').dataset.tone).toBeUndefined();

    await boot({
      status: snapshot({
        stall_diag: Object.assign({}, SNAPSHOT.stall_diag, {
          events: 2,
          last_us: 581987,
          last_deficit_ms: 0,
          max_deficit_ms: 0,
          pc: '00022bac',
          lr: '2004937c',
        }),
      }),
    });
    expect(text('stall-witness')).toContain('2 epochs over 100 ms');
    expect(text('stall-witness')).toContain('last 582.0 ms');
    expect(text('stall-witness')).toContain('tick deficit 0 ms, worst 0 ms');
    expect(text('stall-witness')).toContain('PC 0x00022bac');
    expect(el('stall-witness').dataset.tone).toBe('warn');
  });

  it('reads a single-image build as having no slots at all', async () => {
    await boot({ status: snapshot({ ab: null }) });

    expect(text('fw-slot')).toBe('single-image build -- no slots');
    expect(text('fw-record')).toBe('single-image build -- no slots');
    expect(text('fw-ledger')).toBe('single-image build -- no slots');
    expect(el('fw-slot').dataset.tone).toBeUndefined();
  });

  it('degrades every forensic row on an image that serves none of them', async () => {
    await boot({ status: without('ab', 'prev_boot', 'fault_record', 'ota_dark', 'stall_diag') });

    for (const id of ['fw-slot', 'fw-record', 'fw-ledger', 'prev-boot', 'fault-record', 'ota-dark', 'stall-witness']) {
      expect(text(id)).toBe('not reported by this image');
      expect(el(id).dataset.tone).toBeUndefined();
    }
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
    /* Two failures are silent; the third reads unreachable. */
    await vi.advanceTimersByTimeAsync(2000);
    await settle();
    for (const pill of PILLS) expect(text(pill)).toBe('live');
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
    await vi.advanceTimersByTimeAsync(2000);
    await settle();
    expect(text('audio-pill')).toBe('unreachable');

    device.status = SNAPSHOT;
    await vi.advanceTimersByTimeAsync(1000);
    await settle();

    for (const pill of PILLS) expect(text(pill)).toBe('live');
    for (const grid of GRIDS) expect(el(grid).dataset.stale).toBeUndefined();
    expect(el('flow').dataset.stale).toBeUndefined();
  });

  it('treats a 503 as a silent skip: last values and pills stay, and Retry-After sets the next poll', async () => {
    device = await boot();
    const inner = globalThis.fetch;
    let retryAfter = '30';
    globalThis.fetch = vi.fn(async (url, init) => {
      device.calls.push({ url, method: 'GET', headers: {} });
      return { ...jsonResponse(503, { ok: false, error: 'audio_busy' }), headers: { get: () => retryAfter } };
    });
    void inner;
    const before = device.calls.length;
    await vi.advanceTimersByTimeAsync(1000);
    expect(device.calls.length).toBe(before + 1);
    // 30 s clamps to 5 s: no second poll at 1 s intervals.
    await vi.advanceTimersByTimeAsync(4900);
    expect(device.calls.length).toBe(before + 1);
    await vi.advanceTimersByTimeAsync(200);
    expect(device.calls.length).toBe(before + 2);

    // Many sheds in a row never show unreachable.
    retryAfter = '0';
    await vi.advanceTimersByTimeAsync(5000);
    await settle();
    for (const pill of PILLS) expect(text(pill)).toBe('live');
    for (const grid of GRIDS) expect(el(grid).dataset.stale).toBeUndefined();
    expect(text('input-source')).toBe('optical carrier');
    // 0 clamps up to 250 ms, so a 5 s window holds about twenty polls, never a flood.
    expect(device.calls.length).toBeLessThan(before + 2 + 25);
  });

  it('never overlaps polls: the next request waits for the previous answer', async () => {
    device = await boot();
    const waiting = [];
    globalThis.fetch = vi.fn(() => new Promise((resolve) => waiting.push(resolve)));
    await vi.advanceTimersByTimeAsync(3500); /* inside the 4 s request timeout */
    expect(waiting).toHaveLength(1);
    waiting[0](jsonResponse(200, SNAPSHOT));
    await settle();
    await vi.advanceTimersByTimeAsync(1000);
    expect(waiting).toHaveLength(2);
  });

  it('aborts a hung poll after the request timeout, counts it, and schedules the next', async () => {
    device = await boot();
    const signals = [];
    globalThis.fetch = vi.fn((url, init = {}) => {
      signals.push(init.signal);
      return new Promise(() => {});
    });
    for (let round = 1; round <= 2; round++) {
      await vi.advanceTimersByTimeAsync(1000 + 4000);
      expect(signals).toHaveLength(round);
      expect(signals[round - 1].aborted).toBe(true);
      for (const pill of PILLS) expect(text(pill)).toBe('live');
    }
    await vi.advanceTimersByTimeAsync(1000 + 4000);
    expect(signals).toHaveLength(3);
    for (const pill of PILLS) expect(text(pill)).toBe('unreachable');
  });

  it('carries the four-tab firmware menu, marking the current page', async () => {
    await boot();

    const tabs = Array.from(document.querySelectorAll('nav a'));
    const hrefs = tabs.map((anchor) => anchor.getAttribute('href'));
    /* Every SD page carries the same four tabs in the same order (2026-09-04
       modernization); the strip reads as one menu because it never reorders. */
    expect(hrefs).toEqual(['index.html', 'replays.html', 'status.html', 'upgrade.html']);
    /* This is the status page, so its own tab is the current one. */
    const current = tabs.filter((anchor) => anchor.getAttribute('aria-current') === 'page');
    expect(current.map((anchor) => anchor.getAttribute('href'))).toEqual(['status.html']);
  });
});
