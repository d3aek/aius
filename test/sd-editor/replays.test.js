// @vitest-environment happy-dom

/* Regression for the SD-hosted replay manager (firmware/sd/www/replays.html +
 * replays.js). The page is a classic script served off the Teensy's SD card,
 * so the harness loads the real HTML body, evaluates the real sd-shared.js and
 * then the real replays.js against it, and stubs `fetch` with the device
 * contract -- no build step and no served URL, which is the cheapest harness
 * that still exercises the shipped files byte-for-byte. Loading sd-shared.js
 * first is the page's own order: replays.js consumes `window.aiusSdShared` and
 * would be a ReferenceError without it.
 *
 * Wire-shape provenance, all authored to the firmware rather than captured:
 *   /api/replays          optical_main.cpp's replay listing -- note a `.part`
 *                         row serves entry.size() as BOTH `bytes` and
 *                         `upload_next_offset`, which is why an incomplete
 *                         file cannot state the total it was aiming at.
 *   /api/replays/status   the same file's replay status serializer (state,
 *                         stop_reason, queue/watermark words, input_class,
 *                         decoder_errors, refill_faults).
 *   /sd/replays/<id>      aius_optical_app.cpp's chunked upload: every chunk
 *                         but the last answers {"ok":true,"next_offset":N},
 *                         the final one answers {"ok":true}.
 *   /api/meters           the settled Phase 14b shape, from the fixtures.
 *
 * The 2026-07-27 UX findings this pins (report sections C and D): P1 both row
 * actions reachable, P2 selection survives the refresh and an empty Start
 * says so, P3 confirm-gated delete, P4 per-card errors and an `unreachable`
 * pill over a dimmed grid, P5 upload progress/cancel/completion, P6 Play
 * refused on an incomplete capture, P7 one token, sent only on mutations, and
 * the cheap R-N items (KiB sizes, playing marker, highlighted underruns,
 * percentage counters, latency in ms).
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const www = resolve(here, '../../firmware/sd/www');
const fixture = (name) => JSON.parse(readFileSync(resolve(here, 'fixtures', name), 'utf8'));
const replaysHtml = readFileSync(resolve(www, 'replays.html'), 'utf8');
const sharedSource = readFileSync(resolve(www, 'sd-shared.js'), 'utf8');
const replaysSource = readFileSync(resolve(www, 'replays.js'), 'utf8');
const bodyMarkup = replaysHtml
  .slice(replaysHtml.indexOf('<body>') + '<body>'.length, replaysHtml.indexOf('</body>'))
  .replace(/<script[\s\S]*?<\/script>/g, '');

const TOKEN_KEY = 'aius-sd-auth';

const el = (id) => document.getElementById(id);
const settle = async (rounds = 60) => {
  for (let index = 0; index < rounds; index++) await Promise.resolve();
};

const jsonResponse = (status, body) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => body,
  text: async () => JSON.stringify(body),
});

/* A complete capture: `.iec` on the card, playable, duration known. */
const completeFile = (id, bytes, extra = {}) =>
  Object.assign(
    {
      id,
      bytes,
      duration_seconds: bytes / 192000,
      upload_next_offset: bytes,
      complete: true,
      active: false,
    },
    extra,
  );

/* A `.part` on the card: offset == size and complete false, the exact shape
 * that used to render as "upload N/N" beside a live Play button. */
const partialFile = (id, bytes) => ({
  id,
  bytes,
  duration_seconds: null,
  upload_next_offset: bytes,
  complete: false,
  active: false,
});

const DEFAULT_STATUS = {
  ok: true,
  state: 'stopped',
  stop_reason: 'none',
  id: '',
  file_bytes: 0,
  offset_bytes: 0,
  duration_seconds: 0,
  loop: false,
  loops_completed: 0,
  queue_words: 0,
  watermark_words: 2048,
  underruns: 0,
  input_class: 'unknown',
  decoder_errors: 0,
  refill_faults: 0,
};

let device;
let canvasOpsById;
const rejections = [];
const trackRejection = (reason) => rejections.push(reason);

const opsOf = (id) => canvasOpsById.get(id) || [];

function installCanvas() {
  canvasOpsById = new Map();
  window.HTMLCanvasElement.prototype.getContext = function getContext() {
    const id = this.id || 'anonymous';
    if (!canvasOpsById.has(id)) canvasOpsById.set(id, []);
    const ops = canvasOpsById.get(id);
    const record =
      (name) =>
      (...args) =>
        ops.push([name, ...args]);
    return {
      clearRect: record('clearRect'),
      beginPath: record('beginPath'),
      closePath: record('closePath'),
      moveTo: record('moveTo'),
      lineTo: record('lineTo'),
      stroke: record('stroke'),
      fill: record('fill'),
      strokeStyle: '',
      fillStyle: '',
    };
  };
}

/* The last frame the waveform canvas drew, grouped into the paths the shared
 * renderer strokes: [0] is the always-drawn centre line, [1] the envelope. */
const wavePaths = () => {
  const ops = opsOf('waveform');
  const start = ops.map((op) => op[0]).lastIndexOf('clearRect');
  return (start === -1 ? ops : ops.slice(start)).reduce((groups, op) => {
    if (op[0] === 'beginPath') groups.push([]);
    else if (groups.length > 0) groups[groups.length - 1].push(op);
    return groups;
  }, []);
};
const envelope = () => {
  const paths = wavePaths();
  return paths.length < 2 ? [] : paths[1].filter((op) => op[0] === 'moveTo' || op[0] === 'lineTo');
};

function createDevice(options = {}) {
  const stub = {
    meters: 'meters' in options ? options.meters : fixture('meters-snapshot.json'),
    status: 'status' in options ? options.status : DEFAULT_STATUS,
    files: options.files || [],
    truncated: options.truncated === true,
    startResult: options.startResult || jsonResponse(200, { ok: true, state: 'prebuffering' }),
    deleteResult: options.deleteResult || jsonResponse(200, { ok: true }),
    /* Per-chunk hook + optional refusal, so the upload tests can cancel mid
     * loop or fault a specific chunk. */
    onChunk: options.onChunk || null,
    chunks: [],
    calls: [],
  };
  globalThis.fetch = vi.fn(async (url, init = {}) => {
    const method = init.method || 'GET';
    stub.calls.push({ url, method, headers: init.headers || {}, body: init.body });
    if (url === '/api/replays' && method === 'DELETE') return stub.deleteResult;
    if (url === '/api/replays') {
      return stub.files ? jsonResponse(200, { ok: true, files: stub.files, truncated: stub.truncated }) : jsonResponse(503, { ok: false, error: 'sd_busy' });
    }
    if (url === '/api/replays/status') {
      return stub.status ? jsonResponse(200, stub.status) : jsonResponse(503, { ok: false, error: 'unavailable' });
    }
    if (url === '/api/replays/start') return stub.startResult;
    if (url === '/api/replays/stop') return jsonResponse(200, { ok: true, state: 'stopped' });
    if (url === '/api/meters') {
      return stub.meters ? jsonResponse(200, stub.meters) : jsonResponse(500, { ok: false, error: 'unavailable' });
    }
    if (url.startsWith('/sd/replays/')) {
      const range = String((init.headers || {})['Content-Range'] || '');
      const match = range.match(/bytes (\d+)-(\d+)\/(\d+)/);
      const start = match ? Number(match[1]) : 0;
      const end = match ? Number(match[2]) + 1 : 0;
      const total = match ? Number(match[3]) : 0;
      stub.chunks.push({ start, end, total });
      const refusal = stub.onChunk ? stub.onChunk(stub.chunks.length - 1, stub) : null;
      if (refusal) return refusal;
      return end >= total ? jsonResponse(200, { ok: true }) : jsonResponse(200, { ok: true, next_offset: end });
    }
    return jsonResponse(404, { ok: false, error: 'not_found' });
  });
  return stub;
}

async function boot(options) {
  device = createDevice(options);
  document.body.innerHTML = bodyMarkup;
  installCanvas();
  window.confirm = vi.fn(() => true);
  new Function(sharedSource)();
  new Function(replaysSource)();
  await settle();
  return device;
}

/* The page reads name/size/slice off the picked file and nothing else, so a
 * literal stand-in keeps the harness free of happy-dom's File plumbing. */
function pickFile(name, size) {
  const stub = {
    name,
    size,
    slice: (start, end) => ({ arrayBuffer: async () => new ArrayBuffer(end - start) }),
  };
  Object.defineProperty(el('file'), 'files', { value: [stub], configurable: true });
  return stub;
}

const rowsOf = () => Array.from(el('files').querySelectorAll('.file'));
const rowFor = (id) => rowsOf().find((row) => row.dataset.id === id);
const actionIn = (row, action) => row.querySelector(`button[data-action="${action}"]`);
const optionsOf = () => Array.from(el('selected').options).map((option) => option.value);

async function click(node) {
  node.click();
  await settle();
}

async function setToken(value) {
  el('token').value = value;
  el('token').dispatchEvent(new Event('input'));
  await settle();
}

async function selectReplay(id) {
  el('selected').value = id;
  el('selected').dispatchEvent(new Event('change'));
  await settle();
}

async function submitUpload() {
  el('upload').dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
  await settle(400);
}

const putCalls = () => device.calls.filter((call) => call.method === 'PUT');
const postCalls = () => device.calls.filter((call) => call.method === 'POST');
const deleteCalls = () => device.calls.filter((call) => call.method === 'DELETE');
const errorsShown = (card) => (el(`${card}-errors`).hidden ? '' : el(`${card}-errors`).textContent);

beforeEach(() => {
  rejections.length = 0;
  process.on('unhandledRejection', trackRejection);
  localStorage.clear();
  vi.useFakeTimers();
});

afterEach(() => {
  vi.useRealTimers();
  process.off('unhandledRejection', trackRejection);
  expect(rejections).toEqual([]);
});

describe('Aius SD replay manager — library rows', () => {
  it('gives every row both actions, in their own cells, with Delete marked destructive', async () => {
    /* UX finding P1, the worst defect in the review: at <=650px both buttons
     * shared one grid cell and Delete painted over Play, so the only visible
     * row button destroyed a capture. The markup half of the fix is that each
     * action is its own element in the row; the CSS half is the media rule. */
    await boot({ files: [completeFile('alpha', 2097152), completeFile('beta', 4194304)] });

    const rows = rowsOf();
    expect(rows).toHaveLength(2);
    for (const row of rows) {
      expect(actionIn(row, 'play')).not.toBeNull();
      expect(actionIn(row, 'delete')).not.toBeNull();
      expect(actionIn(row, 'delete').className).toContain('danger');
      expect(row.children).toHaveLength(4);
    }
  });

  it('refuses Play on an incomplete capture and says what the card actually holds', async () => {
    /* UX finding P6: offset == size on a .part rendered as "upload N/N" beside
     * a live Play button. */
    await boot({ files: [partialFile('half-done', 4096)] });

    const row = rowFor('half-done');
    expect(actionIn(row, 'play').disabled).toBe(true);
    expect(row.querySelector('small').textContent).toBe(
      'incomplete upload, 4.0 KiB on the card (resume with the same ID)',
    );
    expect(optionsOf()).toEqual(['']);
  });

  it('shows small captures in KiB and marks the row the device is playing', async () => {
    /* R-N2 (a 4 KB file read "0.00 MiB") and R-N6 (no playing marker). */
    await boot({
      files: [completeFile('tiny', 8192), completeFile('film-51', 4194304)],
      status: Object.assign({}, DEFAULT_STATUS, { state: 'playing', id: 'film-51' }),
    });

    expect(rowFor('tiny').querySelector('small').textContent).toContain('8.0 KiB');
    expect(rowFor('film-51').querySelector('.file__playing')).not.toBeNull();
    expect(rowFor('tiny').querySelector('.file__playing')).toBeNull();
  });

  it('separates "no card" from "no replays" -- the device answers both with a 200', async () => {
    device = await boot({ files: [] });
    globalThis.fetch = vi.fn(async (url, init = {}) => {
      device.calls.push({ url, method: init.method || 'GET', headers: init.headers || {} });
      if (url === '/api/replays') return jsonResponse(200, { ok: false, error: 'sd_unavailable' });
      if (url === '/api/replays/status') return jsonResponse(200, DEFAULT_STATUS);
      return jsonResponse(200, { sequence: 0 });
    });

    await click(el('refresh'));

    expect(el('library-state').textContent).toBe('error');
    expect(el('library-status').textContent).toContain('no SD card is mounted');
    expect(el('files').dataset.stale).toBe('true');
  });

  it('says so when the listing came back incomplete', async () => {
    await boot({ files: [completeFile('alpha', 2097152)], truncated: true });

    expect(el('library-state').textContent).toBe('truncated');
    expect(errorsShown('library')).toContain('INCOMPLETE');
  });
});

describe('Aius SD replay manager — selection', () => {
  it('keeps the operator selection across the 5 s library refresh', async () => {
    /* UX finding P2: the refresh rebuilt the select with innerHTML and wiped
     * the choice out from under the pointer. */
    await boot({ files: [completeFile('alpha', 2097152), completeFile('beta', 4194304)] });
    await selectReplay('beta');

    await vi.advanceTimersByTimeAsync(5000);
    await settle();

    expect(el('selected').value).toBe('beta');
    expect(optionsOf()).toEqual(['', 'alpha', 'beta']);
  });

  it('seeds an untouched selection from the replay the device reports active', async () => {
    await boot({
      files: [completeFile('alpha', 2097152), completeFile('beta', 4194304, { active: true })],
      status: Object.assign({}, DEFAULT_STATUS, { state: 'playing', id: 'beta' }),
    });

    expect(el('selected').value).toBe('beta');
  });

  it('clears a selection whose file left the card, and says so out loud', async () => {
    device = await boot({ files: [completeFile('alpha', 2097152), completeFile('beta', 4194304)] });
    await selectReplay('beta');

    device.files = [completeFile('alpha', 2097152)];
    await vi.advanceTimersByTimeAsync(5000);
    await settle();

    expect(el('selected').value).toBe('');
    expect(errorsShown('playback')).toContain('"beta" is no longer on the card');
  });

  it('refuses Start with nothing selected instead of silently doing nothing', async () => {
    /* The second half of UX finding P2. */
    await boot({ files: [completeFile('alpha', 2097152)] });
    await setToken('secret');

    await click(el('start'));

    expect(postCalls()).toHaveLength(0);
    expect(errorsShown('playback')).toContain('Select a replay first');
  });
});

describe('Aius SD replay manager — destructive and failing actions', () => {
  it('confirms a delete by name and sends nothing when the operator declines', async () => {
    /* UX finding P3: one unguarded click, forty pixels from Play. */
    await boot({ files: [completeFile('alpha', 2097152)] });
    await setToken('secret');
    window.confirm = vi.fn(() => false);

    await click(actionIn(rowFor('alpha'), 'delete'));

    expect(window.confirm).toHaveBeenCalledTimes(1);
    expect(window.confirm.mock.calls[0][0]).toContain('"alpha"');
    expect(deleteCalls()).toHaveLength(0);
    expect(el('library-status').textContent).toContain('cancelled');
  });

  it('deletes after confirmation and reports it after the list refresh', async () => {
    device = await boot({ files: [completeFile('alpha', 2097152)] });
    await setToken('secret');

    await click(actionIn(rowFor('alpha'), 'delete'));

    const sent = deleteCalls();
    expect(sent).toHaveLength(1);
    expect(JSON.parse(sent[0].body)).toEqual({ id: 'alpha' });
    expect(sent[0].headers['X-Aius-SD-Auth']).toBe('secret');
    expect(el('library-status').textContent).toBe('Deleted "alpha".');
  });

  it('lands a row action failure in the library card, never in another card', async () => {
    /* UX finding P4: every Start/Stop/Delete error used to surface in
     * #upload-status at the bottom of a different card. */
    await boot({
      files: [completeFile('alpha', 2097152)],
      startResult: jsonResponse(409, { ok: false, error: 'replay_carrier_present' }),
    });
    await setToken('secret');

    await click(actionIn(rowFor('alpha'), 'play'));

    expect(errorsShown('library')).toContain('the optical input is live');
    expect(el('playback-errors').hidden).toBe(true);
    expect(el('upload-errors').hidden).toBe(true);
  });

  it('lands the Start button failure in the playback card', async () => {
    await boot({
      files: [completeFile('alpha', 2097152)],
      startResult: jsonResponse(404, { ok: false, error: 'replay_not_found' }),
    });
    await setToken('secret');
    await selectReplay('alpha');

    await click(el('start'));

    expect(errorsShown('playback')).toContain('no such replay on the card');
    expect(el('library-errors').hidden).toBe(true);
  });

  it('refuses a mutation with no token rather than spending a 401', async () => {
    await boot({ files: [completeFile('alpha', 2097152)] });
    await selectReplay('alpha');

    await click(el('start'));

    expect(postCalls()).toHaveLength(0);
    expect(errorsShown('playback')).toContain('SD auth token required');
  });
});

describe('Aius SD replay manager — polling', () => {
  it('shows unreachable over a dimmed grid when the status poll fails', async () => {
    /* UX finding P4: the pill and grid froze, so a dead device still read
     * "playing" with a full buffer bar. */
    device = await boot({
      files: [completeFile('film-51', 4194304)],
      status: Object.assign({}, DEFAULT_STATUS, { state: 'playing', id: 'film-51' }),
    });
    expect(el('state').textContent).toBe('playing');

    device.status = null;
    await vi.advanceTimersByTimeAsync(1000);
    await settle();

    expect(el('state').textContent).toBe('unreachable');
    expect(el('state').dataset.state).toBe('unreachable');
    expect(el('playback-grid').dataset.stale).toBe('true');
    expect(el('playback-status').textContent).toContain('stale');
  });

  it('highlights non-zero underruns and states the queue and position as percentages', async () => {
    /* R-N8 (underruns never stood out) and R-N3 (raw word counters only). */
    await boot({
      status: Object.assign({}, DEFAULT_STATUS, {
        state: 'playing',
        id: 'film-51',
        file_bytes: 4194304,
        offset_bytes: 1048576,
        queue_words: 1024,
        watermark_words: 2048,
        underruns: 3,
        input_class: 'pcm',
        stop_reason: 'none',
      }),
    });

    expect(el('underruns').textContent).toBe('3');
    expect(el('underruns').dataset.tone).toBe('warn');
    expect(el('queue').textContent).toBe('1024 / 2048 words (50 %)');
    expect(el('position').textContent).toBe('1.00 MiB / 4.00 MiB (25 %)');
    expect(el('buffer').value).toBe(50);
  });
});

describe('Aius SD replay manager — upload', () => {
  it('reports progress, percentage and an explicit completion message', async () => {
    /* UX finding P5: an hour-long operation reported raw bytes only, with no
     * percentage, no bar, no cancel and no completion notice. */
    device = await boot({ files: [] });
    await setToken('secret');
    el('replay-id').value = 'vector-a';
    pickFile('vector-a.iec', 10000);

    await submitUpload();

    expect(device.chunks).toEqual([
      { start: 0, end: 4096, total: 10000 },
      { start: 4096, end: 8192, total: 10000 },
      { start: 8192, end: 10000, total: 10000 },
    ]);
    expect(el('upload-progress').value).toBe(100);
    expect(el('upload-percent').textContent).toBe('100 %');
    expect(el('upload-status').textContent).toBe('Upload complete: "vector-a", 9.8 KiB written to the card.');
    expect(el('upload-state').textContent).toBe('idle');
    expect(el('upload-start').disabled).toBe(false);
    expect(el('upload-cancel').disabled).toBe(true);
    expect(putCalls()[0].headers['X-Aius-SD-Auth']).toBe('secret');
  });

  it('resumes at the offset the card reports for that id', async () => {
    /* The engine verifies the claimed offset against the .part's real size and
     * faults on a mismatch, so restarting a resumed upload at 0 is a 500. */
    device = await boot({ files: [partialFile('vector-a', 4096)] });
    await setToken('secret');
    el('replay-id').value = 'vector-a';
    pickFile('vector-a.iec', 10000);

    await submitUpload();

    expect(device.chunks[0]).toEqual({ start: 4096, end: 8192, total: 10000 });
    expect(device.chunks).toHaveLength(2);
  });

  it('cancels the chunk loop where it stands and says how to resume', async () => {
    device = await boot({
      files: [],
      onChunk: (index) => {
        if (index === 0) el('upload-cancel').click();
        return null;
      },
    });
    await setToken('secret');
    el('replay-id').value = 'vector-a';
    pickFile('vector-a.iec', 20000);

    await submitUpload();

    expect(device.chunks).toHaveLength(1);
    expect(el('upload-state').textContent).toBe('stopped');
    expect(el('upload-status').textContent).toBe(
      'Upload cancelled at 4.0 KiB of 20 KiB. Upload the same ID again to resume.',
    );
    expect(el('upload-cancel').disabled).toBe(true);
    expect(el('upload-start').disabled).toBe(false);
  });

  it('surfaces a chunk refusal in the upload card and names the resume path', async () => {
    device = await boot({
      files: [],
      onChunk: (index) => (index === 1 ? jsonResponse(507, { ok: false, error: 'sd_capacity' }) : null),
    });
    await setToken('secret');
    el('replay-id').value = 'vector-a';
    pickFile('vector-a.iec', 20000);

    await submitUpload();

    expect(errorsShown('upload')).toContain('the card is full');
    expect(errorsShown('upload')).toContain('Upload the same ID again to resume');
    expect(el('upload-state').textContent).toBe('error');
    expect(el('playback-errors').hidden).toBe(true);
    expect(el('library-errors').hidden).toBe(true);
  });

  it('states the total for an incomplete file this session uploaded', async () => {
    /* The device cannot know it, so the page claims "of M" only for its own
     * upload and otherwise says what is on the card (UX finding P6). */
    device = await boot({
      files: [],
      onChunk: (index) => (index === 0 ? jsonResponse(503, { ok: false, error: 'sd_busy' }) : null),
    });
    await setToken('secret');
    el('replay-id').value = 'vector-a';
    pickFile('vector-a.iec', 20000);
    device.files = [partialFile('vector-a', 4096)];

    await submitUpload();

    expect(rowFor('vector-a').querySelector('small').textContent).toBe(
      'incomplete upload, 4.0 KiB of 20 KiB (resume with the same ID)',
    );
  });

  it('refuses an upload whose id fails the device grammar before sending anything', async () => {
    await boot({ files: [] });
    await setToken('secret');
    el('replay-id').value = 'bad id!';
    pickFile('vector-a.iec', 4096);

    await submitUpload();

    expect(putCalls()).toHaveLength(0);
    expect(errorsShown('upload')).toContain('1 to 48 characters');
  });
});

describe('Aius SD replay manager — auth token', () => {
  it('restores the stored token and sends it only on mutations', async () => {
    /* UX finding P7: two names for one credential, no persistence here, and
     * the header sprayed over every public GET. */
    localStorage.setItem(TOKEN_KEY, 'stored-token');
    await boot({ files: [completeFile('alpha', 2097152)] });

    expect(el('token').value).toBe('stored-token');
    await selectReplay('alpha');
    await click(el('start'));

    const reads = device.calls.filter((call) => call.method === 'GET');
    expect(reads.length).toBeGreaterThan(0);
    for (const read of reads) expect(read.headers['X-Aius-SD-Auth']).toBeUndefined();
    expect(postCalls()[0].headers['X-Aius-SD-Auth']).toBe('stored-token');
  });

  it('persists a typed token for the next page load', async () => {
    await boot({ files: [] });

    await setToken('typed-token');

    expect(localStorage.getItem(TOKEN_KEY)).toBe('typed-token');
  });
});

describe('Aius SD replay manager — meters', () => {
  it('reports both output channels from the [L, R] arrays, with the latency in ms', async () => {
    await boot();

    const summary = el('meter-summary').textContent;
    expect(summary).not.toContain('NaN');
    expect(summary).toContain('Output peak -3.1 / -3.4 dBFS');
    expect(summary).toContain('RMS -14.6 / -14.9 dBFS');
    /* R-N9's replay-side twin: 10667 us is not a legible number. */
    expect(summary).toContain('latency 10.7 ms');
    expect(el('meter-state').textContent).toBe('live');
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
    for (const [, , y] of points) {
      expect(y).toBeGreaterThanOrEqual(0);
      expect(y).toBeLessThanOrEqual(180);
    }
    expect(points[7]).toEqual(['lineTo', 480, 180]);
    expect(points[9]).toEqual(['lineTo', 0, 90]);
  });

  it('caps rendering at the contract 128 points', async () => {
    await boot({ meters: Object.assign({}, fixture('meters-snapshot.json'), { waveform: Array.from({ length: 400 }, () => 1000) }) });

    expect(envelope()).toHaveLength(256);
  });

  it('renders a never-published snapshot as no data, with the centre line still drawn', async () => {
    await boot({ meters: fixture('meters-idle.json') });

    expect(el('meter-summary').textContent).toBe('No meter snapshot published yet.');
    expect(el('meter-state').textContent).toBe('no data');
    expect(wavePaths()).toHaveLength(1);
    expect(envelope()).toHaveLength(0);
  });

  it('renders levels at the -120 dBFS floor as silence', async () => {
    await boot({ meters: fixture('meters-silent.json') });

    const summary = el('meter-summary').textContent;
    expect(summary).toContain('Output peak -0.1 / -inf dBFS');
    expect(summary).toContain('RMS -3.2 / -inf dBFS');
  });

  it('surfaces a meters failure without disturbing the replay status it already drew', async () => {
    await boot({
      meters: null,
      status: Object.assign({}, DEFAULT_STATUS, { state: 'playing', id: 'film-51' }),
    });

    expect(el('state').textContent).toBe('playing');
    expect(el('active').textContent).toBe('film-51');
    expect(el('meter-state').textContent).toBe('unreachable');
    expect(el('meter-summary').textContent).toContain('HTTP 500');
    expect(el('playback-grid').dataset.stale).toBeUndefined();
  });

  it('polls meters faster than the 1 Hz status tick', async () => {
    await boot();
    const metersAfterBoot = device.calls.filter((call) => call.url === '/api/meters').length;
    const statusAfterBoot = device.calls.filter((call) => call.url === '/api/replays/status').length;

    await vi.advanceTimersByTimeAsync(1000);

    expect(device.calls.filter((call) => call.url === '/api/meters').length).toBe(metersAfterBoot + 4);
    expect(device.calls.filter((call) => call.url === '/api/replays/status').length).toBe(statusAfterBoot + 1);
  });
});
