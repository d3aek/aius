// @vitest-environment happy-dom

/* Regression for the SD-hosted firmware upgrade page (firmware/sd/www/
 * upgrade.html + upgrade.js, on top of the shared sd-shared.js). The page is a
 * classic script served off the Teensy's SD card, so the harness loads the real
 * HTML body, evaluates the real sd-shared.js and then the real upgrade.js
 * against it, and stubs `fetch` with the device contract -- no build step and
 * no served URL, which is the cheapest harness that still exercises the shipped
 * files byte-for-byte. Loading sd-shared.js first is the page's own order:
 * upgrade.js consumes `window.aiusSdShared` and would be a ReferenceError
 * without it.
 *
 * THE HEADER BUILDER BELOW IS THE ONE DELIBERATE SECOND COPY of the container
 * layout. `lib/aius_ota`'s C suites build valid headers through
 * aius_ota_header_serialize() and mutate single wire bytes to prove each
 * rejection; a browser cannot call that serializer, so this file authors the
 * same wire bytes from aius_ota.h's PUBLISHED offsets and mutates them the same
 * way. The parser under test is upgrade.js's -- if the two disagree about an
 * offset, every accept test fails loudly rather than one rejection silently
 * passing for the wrong reason.
 *
 * Wire-shape provenance, all authored to docs/plans/aius-ota-upgrade.md's
 * slice-3 contract (the routes ship from a parallel slice):
 *   GET  /api/ota          the AiusOtaProgress fields plus the ota_diag ones
 *                          (test_key, key_fingerprint, activation_available);
 *                          404 {"error":"ota_absent"} when OTA is compiled out.
 *   PUT  /api/ota/image    container-absolute Content-Range chunks answering
 *                          {"ok":true,"next_offset":N}, the last one
 *                          {"ok":true} -- the same shape /sd/replays/<id> uses.
 *   POST /api/ota/verify   digest then signature, refusals latched in
 *                          fail_reason / verifier_fault rather than only in the
 *                          response, so the page must READ THE POLL.
 *   POST /api/ota/arm | abort | activate.
 *   GET  /api/status       `target_id`, the board identity the container's
 *                          target_id is checked against before an upload.
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const www = resolve(here, '../../firmware/sd/www');
const upgradeHtml = readFileSync(resolve(www, 'upgrade.html'), 'utf8');
const sharedSource = readFileSync(resolve(www, 'sd-shared.js'), 'utf8');
const upgradeSource = readFileSync(resolve(www, 'upgrade.js'), 'utf8');
const bodyOpen = upgradeHtml.match(/<body[^>]*>/);
const bodyMarkup = upgradeHtml
  .slice(upgradeHtml.indexOf(bodyOpen[0]) + bodyOpen[0].length, upgradeHtml.indexOf('</body>'))
  .replace(/<script[\s\S]*?<\/script>/g, '');

const TOKEN_KEY = 'aius-sd-auth';
const DEVICE_TARGET = 'teensy41-optical';

/* aius_ota.h's published container constants. */
const HEADER_BYTES = 256;
const MAGIC = 0x4f554941;
const OFFSET = {
  MAGIC: 0,
  FORMAT_VERSION: 4,
  HEADER_LENGTH: 6,
  IMAGE_LENGTH: 8,
  SIGNATURE_SCHEME: 12,
  RESERVED0: 14,
  BUILD_UTC: 16,
  TARGET_ID: 24,
  BUILD_REVISION: 56,
  PAYLOAD_DIGEST: 88,
  SIGNATURE: 120,
  RESERVED1: 184,
  CRC32: 252,
};
const PAYLOAD_BYTES = 10000; /* 256 + 10000 = four chunks: 256, 4096, 4096, 1808 */
const BUILD_UTC = 1753900000; /* 2025-07-30, inside the plausibility window */
const BUILD_REVISION = '7a66153703d1';

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

/* ---- the container, authored to the published offsets --------------------- */

/* Reflected CRC-32, polynomial 0xedb88320 -- aius_ota.c's crc32_over(). */
function crc32(bytes, length) {
  let crc = 0xffffffff;
  for (let index = 0; index < length; index++) {
    crc ^= bytes[index];
    for (let bit = 0; bit < 8; bit++) {
      const mask = -(crc & 1);
      crc = (crc >>> 1) ^ (0xedb88320 & mask);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

const fill = (length, value) => Uint8Array.from({ length }, () => value);

function writeText(bytes, offset, width, text) {
  for (let index = 0; index < width; index++) {
    bytes[offset + index] = index < text.length ? text.charCodeAt(index) : 0;
  }
}

/* A valid v1 container header, plus every knob a rejection test needs. The CRC
 * is computed LAST over the finished bytes, so a field-level test always gets a
 * header whose only defect is the field it set. */
function buildHeader(overrides = {}) {
  const fields = Object.assign(
    {
      magic: MAGIC,
      formatVersion: 1,
      headerLength: HEADER_BYTES,
      imageLength: PAYLOAD_BYTES,
      signatureScheme: 1,
      reserved0: 0,
      buildUtc: BUILD_UTC,
      targetId: DEVICE_TARGET,
      buildRevision: BUILD_REVISION,
      payloadDigest: fill(32, 0x11),
      signature: fill(64, 0x22),
      reservedByte: null, /* [offset, value] to poke a reserved1 byte */
      targetIdBytes: null, /* raw override for the non-ASCII / empty cases */
      corruptAfterCrc: null, /* [offset, value], applied AFTER the CRC is sealed */
    },
    overrides,
  );

  const bytes = new Uint8Array(HEADER_BYTES);
  const view = new DataView(bytes.buffer);
  view.setUint32(OFFSET.MAGIC, fields.magic, true);
  view.setUint16(OFFSET.FORMAT_VERSION, fields.formatVersion, true);
  view.setUint16(OFFSET.HEADER_LENGTH, fields.headerLength, true);
  view.setUint32(OFFSET.IMAGE_LENGTH, fields.imageLength, true);
  view.setUint16(OFFSET.SIGNATURE_SCHEME, fields.signatureScheme, true);
  view.setUint16(OFFSET.RESERVED0, fields.reserved0, true);
  view.setBigUint64(OFFSET.BUILD_UTC, BigInt(fields.buildUtc), true);
  if (fields.targetIdBytes) bytes.set(fields.targetIdBytes, OFFSET.TARGET_ID);
  else writeText(bytes, OFFSET.TARGET_ID, 32, fields.targetId);
  writeText(bytes, OFFSET.BUILD_REVISION, 32, fields.buildRevision);
  bytes.set(fields.payloadDigest, OFFSET.PAYLOAD_DIGEST);
  bytes.set(fields.signature, OFFSET.SIGNATURE);
  if (fields.reservedByte) bytes[OFFSET.RESERVED1 + fields.reservedByte[0]] = fields.reservedByte[1];

  view.setUint32(OFFSET.CRC32, crc32(bytes, OFFSET.CRC32), true);
  if (fields.corruptAfterCrc) bytes[fields.corruptAfterCrc[0]] = fields.corruptAfterCrc[1];
  return bytes;
}

/* The page reads name/size/slice off the picked file and nothing else, so a
 * literal stand-in keeps the harness free of happy-dom's File plumbing. The
 * header slice answers the real bytes; the payload slices answer zeroed buffers
 * of the right length, which is all a chunk loop can observe. */
function pickContainer(headerBytes, options = {}) {
  const declared = new DataView(headerBytes.buffer).getUint32(OFFSET.IMAGE_LENGTH, true);
  const size = options.size != null ? options.size : HEADER_BYTES + declared;
  const file = {
    name: options.name || 'firmware.aiuo',
    size,
    slice: (start, end) => ({
      arrayBuffer: async () => {
        /* `payloadReadFails` lets the header parse succeed and the chunk loop
         * fail, which is the shape a card pulled mid-upload produces. */
        if (options.readFails || (options.payloadReadFails && start > 0)) throw new Error('the card went away');
        if (start === 0) return headerBytes.slice(0, Math.min(end, HEADER_BYTES)).buffer;
        return new ArrayBuffer(end - start);
      },
    }),
  };
  Object.defineProperty(el('image-file'), 'files', { value: [file], configurable: true });
  return file;
}

/* ---- the device ----------------------------------------------------------- */

const idleOta = () => ({
  ok: true,
  state: 'idle',
  latch_armed: true,
  image_length: 0,
  staged_length: 0,
  next_offset: 0,
  fail_reason: 'ok',
  verifier_fault: 0,
  test_key: false,
  key_fingerprint: 'b7c1e4f0',
  activation_available: false,
});

let device;
const rejections = [];
const trackRejection = (reason) => rejections.push(reason);

function createDevice(options = {}) {
  const stub = {
    ota: options.ota === undefined ? idleOta() : options.ota,
    absent: options.absent === true,
    unreachable: options.unreachable === true,
    status: 'status' in options ? options.status : { ok: true, target_id: DEVICE_TARGET },
    /* 'digest' | 'signature' | null -- which half of verify() refuses. */
    verifyFault: options.verifyFault || null,
    armRefusal: options.armRefusal || null,
    onChunk: options.onChunk || null,
    chunks: [],
    calls: [],
  };

  globalThis.fetch = vi.fn(async (url, init = {}) => {
    const method = init.method || 'GET';
    stub.calls.push({ url, method, headers: init.headers || {}, body: init.body });

    if (url === '/api/status') {
      return stub.status ? jsonResponse(200, stub.status) : jsonResponse(503, { ok: false, error: 'unavailable' });
    }
    if (url === '/api/ota' && method === 'GET') {
      if (stub.absent) return jsonResponse(404, { ok: false, error: 'ota_absent' });
      if (stub.unreachable || !stub.ota) return jsonResponse(503, { ok: false, error: 'sd_busy' });
      return jsonResponse(200, Object.assign({}, stub.ota));
    }
    if (url === '/api/ota/image' && method === 'PUT') {
      const range = String((init.headers || {})['Content-Range'] || '');
      const match = range.match(/bytes (\d+)-(\d+)\/(\d+)/);
      const start = match ? Number(match[1]) : 0;
      const end = match ? Number(match[2]) + 1 : 0;
      const total = match ? Number(match[3]) : 0;
      stub.chunks.push({ start, end, total });
      const refusal = stub.onChunk ? stub.onChunk(stub.chunks.length - 1, stub) : null;
      if (refusal) return refusal;
      if (start === 0) {
        Object.assign(stub.ota, { state: 'offered', image_length: total - HEADER_BYTES, staged_length: 0, next_offset: HEADER_BYTES });
      } else {
        Object.assign(stub.ota, {
          state: end >= total ? 'staged' : 'staging',
          staged_length: end - HEADER_BYTES,
          next_offset: end,
        });
      }
      return end >= total ? jsonResponse(200, { ok: true }) : jsonResponse(200, { ok: true, next_offset: end });
    }
    if (url === '/api/ota/verify' && method === 'POST') {
      if (stub.verifyFault === 'digest') {
        Object.assign(stub.ota, { state: 'failed', fail_reason: 'digest' });
        return jsonResponse(409, { ok: false, error: 'ota_digest' });
      }
      if (stub.verifyFault === 'signature') {
        Object.assign(stub.ota, { state: 'failed', fail_reason: 'signature', verifier_fault: 7 });
        return jsonResponse(409, { ok: false, error: 'ota_signature' });
      }
      Object.assign(stub.ota, { state: 'verified', fail_reason: 'ok' });
      return jsonResponse(200, { ok: true });
    }
    if (url === '/api/ota/arm' && method === 'POST') {
      if (stub.armRefusal) return stub.armRefusal;
      Object.assign(stub.ota, { state: 'armed' });
      return jsonResponse(200, { ok: true });
    }
    if (url === '/api/ota/abort' && method === 'POST') {
      Object.assign(stub.ota, idleOta(), { test_key: stub.ota.test_key, activation_available: stub.ota.activation_available });
      return jsonResponse(200, { ok: true });
    }
    if (url === '/api/ota/activate' && method === 'POST') {
      Object.assign(stub.ota, { state: 'activation_requested' });
      return jsonResponse(200, { ok: true });
    }
    return jsonResponse(404, { ok: false, error: 'not_found' });
  });
  return stub;
}

async function boot(options) {
  device = createDevice(options);
  document.body.innerHTML = bodyMarkup;
  window.confirm = vi.fn(() => true);
  new Function(sharedSource)();
  new Function(upgradeSource)();
  await settle();
  return device;
}

/* ---- readers -------------------------------------------------------------- */

const entries = () => Array.from(el('log').querySelectorAll('.ota-log__entry'));
const steps = () => entries().map((entry) => [entry.dataset.step, entry.dataset.tone]);
/* The log is append-only, so a second attempt at a step adds a second entry;
 * the LAST one is the live one, exactly as the page's own step map holds it. */
const entryFor = (key) => entries().filter((entry) => entry.dataset.step === key).at(-1);
const textOf = (key) => (entryFor(key) ? entryFor(key).querySelector('.ota-log__text').textContent : '');
const faultOf = (key) => {
  const detail = entryFor(key) ? entryFor(key).querySelector('.ota-log__detail') : null;
  return detail ? detail.textContent : '';
};
const errorsShown = (card) => (el(`${card}-errors`).hidden ? '' : el(`${card}-errors`).textContent);
const mutations = () => device.calls.filter((call) => call.method !== 'GET');
const reads = () => device.calls.filter((call) => call.method === 'GET');

async function setToken(value) {
  el('token').value = value;
  el('token').dispatchEvent(new Event('input'));
  await settle();
}

async function selectContainer(headerBytes, options = {}) {
  pickContainer(headerBytes, options);
  el('image-file').dispatchEvent(new Event('change'));
  await settle();
}

async function startRun() {
  el('start').click();
  await settle(600);
}

/* Boot, authenticate and pick a valid container: the prelude of every run
 * test. */
async function ready(options = {}) {
  await boot(options);
  await setToken('secret');
  await selectContainer(buildHeader(options.header || {}), options.container || {});
}

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

describe('Aius SD firmware upgrade — container header parse', () => {
  it('renders the image identity from the published offsets before anything is sent', async () => {
    await boot();

    await selectContainer(buildHeader());

    expect(el('hdr-target').textContent).toBe(DEVICE_TARGET);
    expect(el('hdr-revision').textContent).toBe(BUILD_REVISION);
    expect(el('hdr-built').textContent).toBe('2025-07-30 18:26:40 UTC');
    expect(el('hdr-image-length').textContent).toBe('9.8 KiB (10000 bytes)');
    expect(el('hdr-container').textContent).toBe('10 KiB (10256 bytes)');
    expect(el('hdr-scheme').textContent).toBe('ed25519 (1)');
    expect(el('hdr-digest').textContent).toBe('11'.repeat(32));
    expect(el('hdr-signature').textContent).toBe('22'.repeat(64));
    expect(el('hdr-crc').textContent).toMatch(/^0x[0-9a-f]{8}$/);
    expect(el('image-pill').textContent).toBe('loaded');
    /* Parsed, and nothing uploaded: the identity is rendered from the file. */
    expect(mutations()).toHaveLength(0);
    expect(steps()).toEqual([['parsed', 'ok']]);
    expect(textOf('parsed')).toContain(`${BUILD_REVISION} for ${DEVICE_TARGET}`);
  });

  it('timestamps every log entry with a machine-readable time', async () => {
    await boot();

    await selectContainer(buildHeader());

    const time = entryFor('parsed').querySelector('.ota-log__time');
    expect(time.textContent).toMatch(/^\d{2}:\d{2}:\d{2}$/);
    expect(time.dateTime).toMatch(/^\d{4}-\d{2}-\d{2}T/);
  });

  /* One rejection per rule, each header valid but for the field named. The
   * fault word is the one the log prints and the one aius_ota.h uses. */
  const REFUSALS = [
    ['a foreign magic', { magic: 0x41424344 }, {}, 'magic', 'not an Aius OTA container'],
    ['a corrupted header', { corruptAfterCrc: [OFFSET.BUILD_REVISION, 0x5a] }, {}, 'header_crc', 'the bytes compute'],
    ['a future format version', { formatVersion: 2 }, {}, 'format_version', 'this page speaks 1'],
    ['a header length v1 does not pin', { headerLength: 512 }, {}, 'header_length', 'version 1 pins it at 256'],
    ['an empty payload', { imageLength: 0 }, { size: HEADER_BYTES }, 'image_length', 'empty payload'],
    ['a payload larger than the flash', { imageLength: 8126465 }, { size: 8126721 }, 'image_length', 'this board can stage'],
    ['an unsigned container', { signatureScheme: 0 }, {}, 'signature_scheme', 'unsigned'],
    ['an unknown scheme tag', { signatureScheme: 9 }, {}, 'signature_scheme', 'unknown signature scheme 9'],
    ['a non-zero reserved word', { reserved0: 1 }, {}, 'reserved', 'reserved field is non-zero'],
    ['a non-zero reserved tail', { reservedByte: [67, 1] }, {}, 'reserved', 'reserved field is non-zero'],
    ['an empty target', { targetIdBytes: fill(32, 0) }, {}, 'target_id', 'not printable ASCII'],
    ['a non-ASCII target', { targetIdBytes: Uint8Array.from({ length: 32 }, (unused, index) => (index === 0 ? 0x01 : 0)) }, {}, 'target_id', 'not printable ASCII'],
    ['an empty build revision', { buildRevision: '' }, {}, 'build_revision', 'not printable ASCII'],
    ['a clockless build stamp', { buildUtc: 0 }, {}, 'build_utc', '2020-01-01'],
    ['a far-future build stamp', { buildUtc: 4102444800 }, {}, 'build_utc', '2100-01-01'],
    ['an unfilled digest', { payloadDigest: fill(32, 0) }, {}, 'payload_digest', 'nobody filled it in'],
    ['an unsigned image', { signature: fill(64, 0) }, {}, 'signature', 'did not sign this image'],
    ['a truncated container file', {}, { size: HEADER_BYTES + PAYLOAD_BYTES - 1 }, 'container_length', 'the header declares'],
    ['an image for another board', { targetId: 'teensy41-bench' }, {}, 'target_mismatch', `this device is "${DEVICE_TARGET}"`],
  ];

  for (const [name, header, container, fault, message] of REFUSALS) {
    it(`refuses ${name} without sending a byte`, async () => {
      await boot();
      await setToken('secret');

      await selectContainer(buildHeader(header), container);

      expect(steps()).toEqual([['parsed', 'fail']]);
      expect(faultOf('parsed')).toBe(`fault: ${fault}`);
      expect(errorsShown('image')).toContain(message);
      expect(el('image-pill').textContent).toBe('invalid');
      expect(el('start').disabled).toBe(true);
      expect(mutations()).toHaveLength(0);
    });
  }

  it('accepts a header whose target the device has not named yet', async () => {
    /* /api/status carries no identity: the device re-checks target_id at offer
     * time and answers UNSUPPORTED itself, so the page must not block. */
    await boot({ status: { ok: true } });

    await selectContainer(buildHeader({ targetId: 'teensy41-bench' }));

    expect(steps()).toEqual([['parsed', 'ok']]);
    expect(el('ota-target').textContent).toBe('unknown');
    expect(el('start').disabled).toBe(false);
  });

  it('re-applies the target rule when the identity arrives after the pick', async () => {
    await boot({ status: { ok: true } });
    await setToken('secret');
    await selectContainer(buildHeader({ targetId: 'teensy41-bench' }));
    device.status = { ok: true, target_id: DEVICE_TARGET };
    device.ota.target_id = DEVICE_TARGET;
    await vi.advanceTimersByTimeAsync(1000);
    await settle();

    await startRun();

    expect(steps()).toEqual([['parsed', 'fail']]);
    expect(faultOf('parsed')).toBe('fault: target_mismatch');
    expect(device.chunks).toHaveLength(0);
  });
});

describe('Aius SD firmware upgrade — the happy path', () => {
  it('narrates parsed, uploaded, staged, digest, signature and armed in order', async () => {
    await ready();

    await startRun();

    expect(device.chunks).toEqual([
      { start: 0, end: 256, total: 10256 },
      { start: 256, end: 4352, total: 10256 },
      { start: 4352, end: 8448, total: 10256 },
      { start: 8448, end: 10256, total: 10256 },
    ]);
    expect(steps()).toEqual([
      ['parsed', 'ok'],
      ['upload', 'ok'],
      ['staged', 'ok'],
      ['digest', 'ok'],
      ['signature', 'ok'],
      ['armed', 'ok'],
      ['activate', 'pending'],
    ]);
    expect(textOf('upload')).toContain('offset 10256');
    expect(textOf('staged')).toContain('9.8 KiB');
    expect(el('ota-state').textContent).toBe('armed');
    expect(el('log-pill').textContent).toBe('armed');
    expect(errorsShown('log')).toBe('');
  });

  it('sends the header as its own chunk at offset 0, then 4096-byte payload chunks', async () => {
    await ready();

    await startRun();

    const puts = device.calls.filter((call) => call.method === 'PUT');
    expect(puts[0].headers['Content-Range']).toBe('bytes 0-255/10256');
    expect(puts[1].headers['Content-Range']).toBe('bytes 256-4351/10256');
    expect(puts[0].headers['Content-Type']).toBe('application/octet-stream');
  });

  it('narrates live byte offsets while the chunk loop runs', async () => {
    const seen = [];
    await ready({
      onChunk: (index) => {
        seen.push(textOf('upload'));
        return null;
      },
    });

    await startRun();

    expect(seen[1]).toContain('offset 256');
    expect(seen[2]).toContain('offset 4352');
    expect(seen[3]).toContain('offset 8448');
  });

  it('locks the controls while a run owns them and releases them after', async () => {
    let duringRun = null;
    await ready({
      onChunk: (index) => {
        if (index === 1) duringRun = { start: el('start').disabled, file: el('image-file').disabled, abort: el('abort').disabled };
        return null;
      },
    });

    await startRun();

    expect(duringRun).toEqual({ start: true, file: true, abort: true });
    expect(el('start').disabled).toBe(false);
    expect(el('abort').disabled).toBe(false);
  });
});

describe('Aius SD firmware upgrade — interrupted transfers', () => {
  it('stops at the failing chunk, names the fault and says where a re-upload resumes', async () => {
    await ready({ onChunk: (index) => (index === 2 ? jsonResponse(503, { ok: false, error: 'sd_busy' }) : null) });

    await startRun();

    expect(steps()).toEqual([
      ['parsed', 'ok'],
      ['upload', 'fail'],
    ]);
    expect(faultOf('upload')).toBe('fault: sd_busy');
    expect(textOf('upload')).toContain('the SD volume is busy');
    expect(el('log-status').textContent).toContain('4352');
    expect(el('log-pill').textContent).toBe('error');
  });

  it('resumes the next run from the offset the device reports, re-sending nothing', async () => {
    /* The device kept everything up to the dropped chunk, so the second run
     * must start at its next_offset -- restarting at 0 would re-offer a header
     * the machine has already absorbed past. */
    let drop = true;
    await ready({
      onChunk: (index, stub) => {
        if (drop && index === 2) {
          drop = false;
          return jsonResponse(503, { ok: false, error: 'sd_busy' });
        }
        return null;
      },
    });
    await startRun();
    expect(device.ota.next_offset).toBe(4352);
    device.chunks.length = 0;
    /* The narration of the resume, caught while the loop still runs: the
     * settled entry states the whole transfer. */
    let announced = '';
    device.onChunk = () => {
      if (announced === '') announced = textOf('upload');
      return null;
    };

    await startRun();

    expect(announced).toContain('Resuming upload at 4.3 KiB');

    expect(device.chunks[0]).toEqual({ start: 4352, end: 8448, total: 10256 });
    expect(device.chunks).toHaveLength(2);
    expect(textOf('upload')).toContain('offset 10256');
    /* The failed attempt stays in the log: the second upload entry is a second
     * line, not a rewrite of the first. */
    expect(steps()).toEqual([
      ['parsed', 'ok'],
      ['upload', 'fail'],
      ['upload', 'ok'],
      ['staged', 'ok'],
      ['digest', 'ok'],
      ['signature', 'ok'],
      ['armed', 'ok'],
      ['activate', 'pending'],
    ]);
  });

  it('restarts at zero when the device is staging a different image', async () => {
    await ready({ ota: Object.assign(idleOta(), { state: 'staging', image_length: 999, staged_length: 512, next_offset: 768 }) });

    await startRun();

    expect(device.chunks[0]).toEqual({ start: 0, end: 256, total: 10256 });
  });

  it('surfaces a file the browser can no longer read as a stopped run, not a hang', async () => {
    await ready({ container: { payloadReadFails: true } });

    await startRun();

    expect(faultOf('upload')).toBe('fault: file_read');
    expect(el('log-pill').textContent).toBe('error');
  });
});

describe('Aius SD firmware upgrade — verification faults', () => {
  it('fails the digest step and never claims a signature check ran', async () => {
    await ready({ verifyFault: 'digest' });

    await startRun();

    expect(steps()).toEqual([
      ['parsed', 'ok'],
      ['upload', 'ok'],
      ['staged', 'ok'],
      ['digest', 'fail'],
    ]);
    expect(faultOf('digest')).toBe('fault: digest');
    expect(el('log-status').textContent).toContain('re-upload');
    expect(entryFor('signature')).toBeUndefined();
  });

  it('passes the digest, fails the signature and carries the verifier fault', async () => {
    await ready({ verifyFault: 'signature' });

    await startRun();

    expect(steps()).toEqual([
      ['parsed', 'ok'],
      ['upload', 'ok'],
      ['staged', 'ok'],
      ['digest', 'ok'],
      ['signature', 'fail'],
    ]);
    expect(faultOf('signature')).toBe('fault: signature');
    expect(textOf('signature')).toContain('verifier fault 7');
    expect(el('ota-fault').textContent).toBe('signature (verifier 7)');
  });

  it('fails the staged step when the device is not staged after the last chunk', async () => {
    await ready({
      onChunk: (index, stub) => {
        if (index === 3) {
          /* The last chunk is accepted but the machine stays mid-staging: an
           * upload the device did not finish must not read as staged. */
          Object.assign(stub.ota, { state: 'staging', staged_length: 9000, next_offset: 9256 });
          return jsonResponse(200, { ok: true });
        }
        return null;
      },
    });

    await startRun();

    expect(steps()).toEqual([
      ['parsed', 'ok'],
      ['upload', 'ok'],
      ['staged', 'fail'],
    ]);
    expect(textOf('staged')).toContain('staging');
  });

  it('fails the arm step on a refusal and names the device state', async () => {
    await ready({ armRefusal: jsonResponse(409, { ok: false, error: 'ota_state' }) });

    await startRun();

    expect(steps().at(-1)).toEqual(['armed', 'fail']);
    expect(faultOf('armed')).toBe('fault: state');
    expect(textOf('armed')).toContain('not in a state that accepts this step');
  });

  it('renders a disarmed latch as a refusal the device explains', async () => {
    await ready({ ota: Object.assign(idleOta(), { latch_armed: false }), onChunk: () => jsonResponse(409, { ok: false, error: 'ota_disarmed' }) });

    await startRun();

    expect(el('ota-latch').textContent).toBe('disarmed');
    expect(el('ota-status').textContent).toContain('DISARMED');
    expect(faultOf('upload')).toBe('fault: disarmed');
    expect(textOf('upload')).toContain('arm it deliberately');
  });
});

describe('Aius SD firmware upgrade — activation gating', () => {
  it('renders the activate step disabled with the pending caption', async () => {
    await ready();

    await startRun();

    expect(el('activate').disabled).toBe(true);
    expect(el('activate-caption').textContent).toContain('activation slice ships this verb');
    expect(el('ota-activation').textContent).toBe('pending activation slice');
  });

  it('enables the activate step the moment the device reports it available', async () => {
    await ready();
    await startRun();
    expect(el('activate').disabled).toBe(true);

    device.ota.activation_available = true;
    await vi.advanceTimersByTimeAsync(1000);
    await settle();

    expect(el('activate').disabled).toBe(false);
    expect(el('activate-caption').textContent).toContain('reboot into the new image');
  });

  it('renders the activate step for an image a previous session armed', async () => {
    await boot({ ota: Object.assign(idleOta(), { state: 'armed' }) });

    expect(el('activate')).not.toBeNull();
    expect(el('activate').disabled).toBe(true);
    expect(steps()).toEqual([['activate', 'pending']]);
  });

  it('requests activation with the token once the verb is available', async () => {
    await ready({ ota: Object.assign(idleOta(), { activation_available: true }) });
    await startRun();

    el('activate').click();
    await settle();

    const activate = device.calls.filter((call) => call.url === '/api/ota/activate');
    expect(activate).toHaveLength(1);
    expect(activate[0].headers['X-Aius-SD-Auth']).toBe('secret');
    expect(steps().at(-1)).toEqual(['activate', 'ok']);
  });
});

describe('Aius SD firmware upgrade — the TEST KEY banner', () => {
  it('banners the published development key whenever the device reports it', async () => {
    await boot({ ota: Object.assign(idleOta(), { test_key: true, key_fingerprint: 'b7c1e4f0' }) });

    expect(el('test-key').hidden).toBe(false);
    expect(el('test-key').textContent).toContain('TEST KEY');
    expect(el('test-key').textContent).toContain('b7c1e4f0');
    expect(el('ota-key').textContent).toBe('b7c1e4f0 (TEST KEY)');
  });

  it('shows no banner for a device carrying a production key', async () => {
    await boot();

    expect(el('test-key').hidden).toBe(true);
    expect(el('ota-key').textContent).toBe('b7c1e4f0');
  });

  it('drops the banner when the device stops reporting a test key', async () => {
    await boot({ ota: Object.assign(idleOta(), { test_key: true }) });
    expect(el('test-key').hidden).toBe(false);

    device.ota.test_key = false;
    await vi.advanceTimersByTimeAsync(1000);
    await settle();

    expect(el('test-key').hidden).toBe(true);
  });
});

describe('Aius SD firmware upgrade — the absent device', () => {
  it('renders a labelled inactive state with every control disabled', async () => {
    await boot({ absent: true });

    expect(el('absent').hidden).toBe(false);
    expect(el('absent').textContent).toContain('OTA IS NOT AVAILABLE ON THIS DEVICE');
    expect(el('ota-pill').textContent).toBe('unavailable');
    expect(el('ota-state').textContent).toBe('unavailable');
    expect(el('image-file').disabled).toBe(true);
    expect(el('start').disabled).toBe(true);
    expect(el('abort').disabled).toBe(true);
    expect(el('test-key').hidden).toBe(true);
  });

  it('re-enables the page if the device comes back with an OTA surface', async () => {
    await boot({ absent: true });

    device.absent = false;
    await vi.advanceTimersByTimeAsync(1000);
    await settle();

    expect(el('absent').hidden).toBe(true);
    expect(el('image-file').disabled).toBe(false);
    expect(el('abort').disabled).toBe(false);
    /* Still no image picked, so the one control that needs one stays off. */
    expect(el('start').disabled).toBe(true);
  });

  it('separates an unreachable poll from an absent OTA surface', async () => {
    await boot({ unreachable: true });

    expect(el('ota-pill').textContent).toBe('unreachable');
    expect(el('absent').hidden).toBe(true);
    expect(el('ota-grid').dataset.stale).toBe('true');
    expect(el('ota-status').textContent).toContain('stale');
  });
});

describe('Aius SD firmware upgrade — the auth token', () => {
  it('threads the token through every mutating call and no read', async () => {
    localStorage.setItem(TOKEN_KEY, 'stored-token');
    await boot({ ota: Object.assign(idleOta(), { activation_available: true }) });
    expect(el('token').value).toBe('stored-token');
    await selectContainer(buildHeader());

    await startRun();
    el('activate').click();
    await settle();
    el('abort').click();
    await settle();

    const sent = mutations();
    expect(sent.map((call) => call.url)).toEqual([
      '/api/ota/image',
      '/api/ota/image',
      '/api/ota/image',
      '/api/ota/image',
      '/api/ota/verify',
      '/api/ota/arm',
      '/api/ota/activate',
      '/api/ota/abort',
    ]);
    for (const call of sent) expect(call.headers['X-Aius-SD-Auth']).toBe('stored-token');
    expect(reads().length).toBeGreaterThan(0);
    for (const read of reads()) expect(read.headers['X-Aius-SD-Auth']).toBeUndefined();
  });

  it('refuses a run with no token rather than spending a 401', async () => {
    await boot();
    await selectContainer(buildHeader());

    await startRun();

    expect(mutations()).toHaveLength(0);
    expect(errorsShown('log')).toContain('SD auth token required');
  });

  it('confirms an abort by name and sends nothing when the operator declines', async () => {
    await boot();
    await setToken('secret');
    window.confirm = vi.fn(() => false);

    el('abort').click();
    await settle();

    expect(window.confirm).toHaveBeenCalledTimes(1);
    expect(mutations()).toHaveLength(0);
    expect(el('log-status').textContent).toContain('untouched');
  });

  it('persists a typed token for the next page load', async () => {
    await boot();

    await setToken('typed-token');

    expect(localStorage.getItem(TOKEN_KEY)).toBe('typed-token');
  });
});
