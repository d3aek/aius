/* Aius SD-hosted manual firmware upgrade.
 *
 * OWNS: the whole /sd/www/upgrade.html page -- the client-side parse of a
 * signed container's 256-byte header, the resumable chunked PUT of that
 * container, the verify / arm / abort verbs, the step log that narrates them,
 * the 1 Hz /api/ota poll, the TEST KEY banner and the OTA-absent inactive
 * state; and (Phase 4y) the A/B slots card read from /api/status `ab` with
 * its confirmed POST /api/ota/boot-other switch.
 *
 * DOES NOT OWN: the shared chrome behaviours -- pills, the never-throw fetch
 * discipline, the formatters and SD-auth token storage are
 * `window.aiusSdShared` (sd-shared.js, loaded first by the page) and are
 * consumed, never redefined here. Nor any decision the firmware makes: the
 * device is the authority on every state, and this page refuses locally only
 * what the PUBLISHED container layout lets it prove before spending an upload.
 * Nothing here programs flash; activation is the device-gated slice that ships
 * the verb the last log entry is waiting for.
 *
 * This file is served off the Teensy's SD card. It is deliberately a plain
 * classic script with no imports, no build step and no shared UI kit -- the
 * device cannot reach a host application's module mounts. Same constraint as
 * dsp-editor.js; keep the pages looking like one product.
 *
 * THE CONTAINER, from lib/aius_ota/include/aius_ota.h. The offsets below are
 * that header's published constants, transcribed field for field; the container
 * FILE is AIUS_OTA_HEADER_BYTES of header followed by exactly `image_length`
 * payload bytes (tools/ota/aius_ota_cli.c's stated convention). Every
 * multi-byte field is little-endian; the digests are byte strings. The
 * validation order below is aius_ota_header_parse()'s rule order, so a local
 * refusal names the same reason the device would have named -- a corrupted
 * header must report corruption rather than whichever field the corruption
 * happened to land in.
 *
 * THE ROUTES, coded to docs/plans/aius-ota-upgrade.md's slice-3 contract and
 * built in a parallel slice:
 *
 *   GET  /api/ota          public poll. 200 with {armed, state, staged_bytes,
 *                          total_bytes, next_offset, image:{target_id,
 *                          build_revision, build_utc}, key:{fingerprint,
 *                          test_key}, activation_available, fault,
 *                          verifier_fault}; 404 {error:"ota_absent"} when OTA is
 *                          compiled out. This is ota_status_document()'s wire
 *                          shape (aius_optical_app.cpp) -- the OtaStatus schema,
 *                          not a flat record. `state` is the AiusOtaState name;
 *                          `fault` is a kOtaVerdicts token, "none" when there is
 *                          no fault and an `ota_`-prefixed word otherwise.
 *                          ITS THREE BYTE COUNTS ARE CONTAINER-ABSOLUTE, the
 *                          same coordinate PUT /api/ota/image uses:
 *                          app_ota_status() publishes total_bytes =
 *                          image_length + 256, staged_bytes = staged_length +
 *                          256 and next_offset = the session's next payload
 *                          offset + 256 once a container is offered
 *                          (optical_main.cpp:4706-4709), and all three read 0
 *                          before an offer. So `total_bytes` is the CONTAINER
 *                          length, never the payload length, and next_offset is
 *                          a file offset a resuming client can seek to
 *                          unchanged. Anything this page shows as a payload
 *                          size subtracts the header exactly once.
 *   PUT  /api/ota/image    one Content-Range chunk per request, answering
 *                          {"ok":true,"next_offset":N} exactly as any
 *                          Content-Range upload does. OFFSETS ARE CONTAINER-ABSOLUTE: the
 *                          header is the chunk at offset 0 and the payload
 *                          follows at 256, so one number resumes an interrupted
 *                          transfer whichever half it stopped in.
 *   POST /api/ota/verify   digest then signature, in that order.
 *   POST /api/ota/arm      verified -> armed.
 *   POST /api/ota/abort    anything except activation-requested -> idle.
 *   POST /api/ota/activate the activation slice's verb. The final log entry is
 *                          rendered disabled until /api/ota reports
 *                          activation_available, which is that slice's signal
 *                          that the verb exists.
 *
 * Every mutating call carries X-Aius-SD-Auth; every read is public and carries
 * nothing (the credential is a physical-access bootstrap, not an authorization
 * boundary -- the image signature is what makes remote replacement defensible).
 */
(() => {
  'use strict';

  const CONTRACT_MODE = new URLSearchParams(window.location.search).has('contract');

  const $ = (id) => document.getElementById(id);
  const shared = window.aiusSdShared;
  const { setPill, request, guard, describe, formatBytes, readStoredToken, storeToken } = shared;

  const OTA_POLL_MS = 1000; /* the OTA record is an epoch-latched snapshot */
  const CHUNK_BYTES = 4096; /* AIUS_OTA_MAX_CHUNK_BYTES, one flash sector */

  /* 503 sd_busy is the device's RETRYABLE refusal, not a stopped run: a
   * mid-stream chunk legitimately collides with a root SD job holding the
   * volume, and the one-epoch ota_pending window after every accepted chunk
   * answers the same way (aius_optical_app.cpp:1712-1730 -- "a plain retry
   * resolves"). Bounded, because a card that is busy for over a second is a
   * different problem and the operator should see it. */
  const SD_BUSY_RETRIES = 5;
  const SD_BUSY_RETRY_MS = 250;

  const sleep = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

  /* ---- the published container layout (aius_ota.h) ------------------------ */

  const HEADER_BYTES = 256; /* AIUS_OTA_HEADER_BYTES */
  const MAGIC = 0x4f554941; /* AIUS_OTA_MAGIC, "AIUO" little-endian */
  const FORMAT_VERSION = 1; /* AIUS_OTA_FORMAT_VERSION */
  const MAX_IMAGE_BYTES = 8126464; /* AIUS_OTA_MAX_IMAGE_BYTES */
  const MIN_BUILD_UTC = 1577836800; /* AIUS_OTA_MIN_BUILD_UTC, 2020-01-01 */
  const MAX_BUILD_UTC = 4102444800; /* AIUS_OTA_MAX_BUILD_UTC, 2100-01-01 */
  const TARGET_ID_BYTES = 32;
  const BUILD_REVISION_BYTES = 32;
  const DIGEST_BYTES = 32;
  const SIGNATURE_BYTES = 64;
  const RESERVED1_BYTES = 68;

  const OFFSET = Object.freeze({
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
  });

  /* The tag registry, and only the registry: a tag names the whole recipe, so
   * an unknown one is refused rather than read as a scheme from the future. */
  const SCHEME_NAMES = Object.freeze({ 0: 'none', 1: 'ed25519' });

  /* reserved0's LOW byte is the image's linked-for slot (AIUS_OTA_SLOT_*,
   * aius_ota.c:223-231): 0 = untagged, the single-image and slot-agnostic
   * posture; 1 = slot A; 2 = slot B. It is inside the authenticated prefix, so
   * a signed tag cannot be rewritten. The HIGH byte stays reserved-must-be-zero.
   * The device compares this against its own expected slot at offer time
   * (aius_ota.c:433) and answers UNSUPPORTED on a mismatch, so the page reports
   * the tag rather than judging it -- only the device knows which it wants. */
  const SLOT_B = 2; /* AIUS_OTA_SLOT_B, the top of the tag domain */
  const SLOT_NAMES = Object.freeze({ 0: 'untagged (slot-agnostic)', 1: 'slot A', 2: 'slot B' });

  /* AiusOtaState, in declaration order, so a numeric wire encoding decodes to
   * the same word the string encoding uses. There is deliberately no companion
   * fault table: `fault` is a kOtaVerdicts TOKEN on every route
   * (aius_optical_app.cpp:1457-1469 pairs each AiusOpticalAppOtaResult with its
   * HTTP status and its wire word, and ota_status_document() prints the word),
   * so a numeric fault never reaches this page. The table that used to sit here
   * decoded AiusOtaStatus -- the core library's enum, not the wire's -- and so
   * would have named the wrong fault for every index had anything ever hit it. */
  const STATE_NAMES = Object.freeze([
    'idle',
    'offered',
    'staging',
    'staged',
    'verified',
    'armed',
    'activation_requested',
    'failed',
  ]);

  /* One wire byte count, converted once: every /api/ota byte field is
   * container-absolute (header + payload) once a container is offered, and 0
   * before one is, so the payload figure is the count minus the header and
   * never negative. Nothing else on this page subtracts HEADER_BYTES. */
  const payloadBytes = (containerAbsolute) => Math.max(0, (Number(containerAbsolute) || 0) - HEADER_BYTES);

  /* ---- the step log's vocabulary ------------------------------------------
   * The whole narration, in the order the device performs it. A step is
   * appended when it STARTS and settled green or red from the route's answer
   * plus a read of /api/ota, so the log never shows a step the run did not
   * reach. */
  const STEP_TEXT = Object.freeze({
    parsed: 'Container selected and parsed',
    upload: 'Uploading to the device',
    staged: 'Staged on the device',
    digest: 'Payload digest verified',
    signature: 'Signature verified',
    ivt: 'Image vector table in bounds',
    armed: 'Image armed for activation',
    activate: 'Activate the armed image',
  });

  const state = {
    file: null, /* the picked container, or null */
    header: null, /* the parsed header once it passes every rule */
    ota: null, /* the last /api/ota body, or null when unreachable */
    absent: false, /* /api/ota answered 404 ota_absent */
    deviceTargetId: '', /* the board identity, from /api/status */
    running: false, /* an upgrade run owns the controls */
    steps: new Map(), /* step key -> the <li> that narrates it */
    slots: null, /* slotFacts() of the last /api/status, or null */
  };

  /* ---- per-card status and error lines ------------------------------------
   * Every acting card owns a status line (role=status) and an error line
   * (role=alert), and a failure lands in the card whose control was pressed --
   * the same discipline dsp-editor.js carries. The POLL reports through the pill
   * and the status line only: a once-a-second failure must not paint over what
   * the operator's last button press had to say. */

  const setCardStatus = (card, text) => {
    $(`${card}-status`).textContent = text;
  };

  function showCardError(card, text) {
    const line = $(`${card}-errors`);
    line.textContent = text;
    line.hidden = false;
  }

  function clearCardError(card) {
    const line = $(`${card}-errors`);
    line.textContent = '';
    line.hidden = true;
  }

  const cardSink = (card) => (text) => showCardError(card, text);

  /* ---- device access ------------------------------------------------------ */

  const authToken = () => $('token').value.trim();

  /* MUTATIONS ONLY: /api/ota and /api/status are public reads, so a token on
   * either would be a credential sprayed for nothing. */
  function mutationHeaders(extra) {
    const token = authToken();
    return token === '' ? Object.assign({}, extra) : Object.assign({}, extra, { 'X-Aius-SD-Auth': token });
  }

  function tokenMissing(card, action) {
    if (authToken() !== '') return false;
    showCardError(card, `SD auth token required to ${action}. Read it over USB with the AIUS AUTH command.`);
    return true;
  }

  /* A wire fault name: the `ota_`-prefixed kOtaVerdicts token, or one of the
   * SD vocabulary's own tokens, stripped to the bare word the log prints. The
   * OTA poll's `fault` field spells no-fault as the literal "none"
   * (kOtaVerdicts row 0), which reads the same as an empty field -- no fault to
   * name. Strings only: see STATE_NAMES on why no numeric fault exists. */
  function faultName(value) {
    const text = String(value || '').trim();
    if (text === '' || text === 'none') return '';
    return text.startsWith('ota_') ? text.slice(4) : text;
  }

  /* The fault word a step should print: the route's own error token first, the
   * poll's latched `fault` second, the bare HTTP status last. `ok`/`none` is not
   * a fault -- a refusal whose poll has nothing latched must not read "fault: ok". */
  function faultFor(result, ota) {
    const fromBody = faultName(result.body && result.body.error ? result.body.error : '');
    if (fromBody !== '' && fromBody !== 'ok') return fromBody;
    const fromPoll = ota ? faultName(ota.fault) : '';
    if (fromPoll !== '' && fromPoll !== 'ok') return fromPoll;
    return `http_${result.status}`;
  }

  function stateName(value) {
    if (typeof value === 'number' && Number.isInteger(value)) {
      return STATE_NAMES[value] || `state ${value}`;
    }
    return String(value || 'unknown');
  }

  /* The refusals the OTA routes and the SD staging path can emit, said in
   * operator terms. One row per kOtaVerdicts token
   * (aius_optical_app.cpp:1457-1469) plus the dispatcher's own request-shape
   * and SD tokens; anything else falls back to the shared HTTP description
   * rather than being guessed at. */
  function describeOtaFailure(result) {
    const token = faultName(result.body && result.body.error ? result.body.error : '');
    if (token === 'absent') return 'this firmware has no OTA support compiled in';
    if (token === 'disarmed') return 'the device OTA latch is disarmed; arm it deliberately before staging';
    if (token === 'state') return 'the device is not in a state that accepts this step';
    if (token === 'range') return 'the device expected a different byte offset; re-upload to resume from the one it reports';
    if (token === 'overflow') return 'that chunk would run past the declared image length';
    if (token === 'malformed') return 'the device refused the container header as malformed';
    if (token === 'unsupported') return 'the device refused the container as not for this build';
    if (token === 'digest') return 'the staged bytes do not match the header digest';
    if (token === 'signature') return 'the signature over this image was refused';
    if (token === 'ivt') return 'the image is authentic but its vector table is not linked for this device flash window (wrong build, or not a firmware image at all); nothing was armed';
    if (token === 'corrupt') return 'the persisted staging record failed its own CRC';
    if (token === 'no_space') return 'the device has no room for the staging record';
    if (token === 'invalid_arg') return 'the device rejected the request arguments';
    /* 507 STORAGE: the genuine staging-medium refusal, as opposed to the
     * transient 503 sd_busy this page retries through. */
    if (token === 'storage') return 'the device could not write the staging record to the card';
    if (token === 'activation_disabled') {
      return 'activation is disabled on this device; POST /api/ota/enable-activation to set the runtime latch';
    }
    if (token === 'self_test_failed') return 'the device flash self-test failed; nothing was programmed';
    /* The dispatcher's own request-shape refusals, ahead of any verdict. */
    if (token === 'chunk') return `that chunk is larger than the ${CHUNK_BYTES} bytes the device accepts per request`;
    if (token === 'header') return 'the chunk at offset 0 must carry the whole 256-byte container header';
    if (token === 'status') return 'the device could not read its own OTA status';
    if (token === 'sd_auth') return 'the SD auth token was rejected';
    /* An upload has already spent its bounded retry by the time this prints;
     * any other step has not, so it is worth pressing again. */
    if (token === 'sd_busy') {
      return `the SD volume is busy (a cache fill or upload in flight); an upload retries ${SD_BUSY_RETRIES} times on its own, any other step is worth retrying by hand`;
    }
    if (token === 'sd_capacity') return 'the card is full';
    if (token === 'sd_unavailable') return 'no SD card is mounted';
    return describe(result);
  }

  /* ---- container header parse ---------------------------------------------
   * Reflected CRC-32, polynomial 0xedb88320, init and final XOR all-ones -- the
   * same convention aius_ota.c's crc32_over() and the boot ledger use, so this
   * page computes the number the device is about to compare against. */
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

  const hex = (bytes) => Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');

  const isZeroBlock = (bytes) => bytes.every((byte) => byte === 0);

  /* A NUL-padded ASCII field: at least one character, printable up to the first
   * NUL, and nothing but NUL after it (aius_ota.c's text_field_valid). */
  function textField(bytes, offset, width) {
    const field = bytes.subarray(offset, offset + width);
    if (field[0] === 0) return { ok: false, value: '' };
    let index = 0;
    while (index < width && field[index] !== 0) {
      if (field[index] < 0x20 || field[index] > 0x7e) return { ok: false, value: '' };
      index++;
    }
    for (let tail = index; tail < width; tail++) {
      if (field[tail] !== 0) return { ok: false, value: '' };
    }
    return { ok: true, value: String.fromCharCode(...field.subarray(0, index)) };
  }

  const refuse = (fault, message) => ({ ok: false, fault, message });

  /**
   * Parses and fully validates a container header, in aius_ota_header_parse()'s
   * rule order, then the two rules only a whole file can answer: that the file
   * is exactly header + payload, and that the image is for THIS board.
   *
   * @param {Uint8Array} bytes the first HEADER_BYTES of the container
   * @param {number} fileSize the container file's whole length in bytes
   * @param {string} deviceTargetId the board identity, '' when unknown
   * @returns {{ok: boolean, fault?: string, message?: string, header?: object}}
   */
  function parseContainerHeader(bytes, fileSize, deviceTargetId) {
    if (!(bytes instanceof Uint8Array) || bytes.length !== HEADER_BYTES) {
      return refuse('short_container', `a container is at least ${HEADER_BYTES} bytes of header`);
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

    const magic = view.getUint32(OFFSET.MAGIC, true);
    if (magic !== MAGIC) {
      return refuse('magic', `not an Aius OTA container (magic 0x${magic.toString(16).padStart(8, '0')})`);
    }

    const headerCrc = view.getUint32(OFFSET.CRC32, true);
    const computedCrc = crc32(bytes, OFFSET.CRC32);
    if (headerCrc !== computedCrc) {
      return refuse(
        'header_crc',
        `header CRC-32 is 0x${headerCrc.toString(16).padStart(8, '0')}, the bytes compute 0x${computedCrc
          .toString(16)
          .padStart(8, '0')}`,
      );
    }

    const formatVersion = view.getUint16(OFFSET.FORMAT_VERSION, true);
    if (formatVersion !== FORMAT_VERSION) {
      return refuse('format_version', `container format version ${formatVersion}, this page speaks ${FORMAT_VERSION}`);
    }

    const headerLength = view.getUint16(OFFSET.HEADER_LENGTH, true);
    if (headerLength !== HEADER_BYTES) {
      return refuse('header_length', `header length ${headerLength}, version 1 pins it at ${HEADER_BYTES}`);
    }

    const imageLength = view.getUint32(OFFSET.IMAGE_LENGTH, true);
    if (imageLength === 0) return refuse('image_length', 'the container declares an empty payload');
    if (imageLength > MAX_IMAGE_BYTES) {
      return refuse(
        'image_length',
        `payload is ${formatBytes(imageLength)}, past the ${formatBytes(MAX_IMAGE_BYTES)} this board can stage`,
      );
    }

    const scheme = view.getUint16(OFFSET.SIGNATURE_SCHEME, true);
    if (scheme === 0) return refuse('signature_scheme', 'the container is unsigned (signature scheme 0)');
    if (!SCHEME_NAMES[scheme]) return refuse('signature_scheme', `unknown signature scheme ${scheme}`);

    /* reserved0 is NOT reserved-must-be-zero: its low byte is the slot tag
     * (aius_ota.c:229-233), so refusing a non-zero word here would refuse every
     * slot-tagged container the legacy per-slot rule produces. Same three rules
     * the device applies, in its order: the tag's domain, then the high byte,
     * then reserved1. */
    const slotId = bytes[OFFSET.RESERVED0];
    if (slotId > SLOT_B) {
      return refuse('slot_id', `container slot tag ${slotId}; the domain is 0 (untagged), 1 (slot A), 2 (slot B)`);
    }
    if (bytes[OFFSET.RESERVED0 + 1] !== 0 || !isZeroBlock(bytes.subarray(OFFSET.RESERVED1, OFFSET.RESERVED1 + RESERVED1_BYTES))) {
      return refuse('reserved', 'a reserved field is non-zero');
    }

    const target = textField(bytes, OFFSET.TARGET_ID, TARGET_ID_BYTES);
    if (!target.ok) return refuse('target_id', 'the target identity is empty or not printable ASCII');

    const revision = textField(bytes, OFFSET.BUILD_REVISION, BUILD_REVISION_BYTES);
    if (!revision.ok) return refuse('build_revision', 'the build revision is empty or not printable ASCII');

    const buildUtc = Number(view.getBigUint64(OFFSET.BUILD_UTC, true));
    if (buildUtc < MIN_BUILD_UTC || buildUtc >= MAX_BUILD_UTC) {
      return refuse('build_utc', `build timestamp ${buildUtc} is outside 2020-01-01 .. 2100-01-01`);
    }

    const payloadDigest = bytes.subarray(OFFSET.PAYLOAD_DIGEST, OFFSET.PAYLOAD_DIGEST + DIGEST_BYTES);
    if (isZeroBlock(payloadDigest)) return refuse('payload_digest', 'the payload digest is all zero -- nobody filled it in');

    const signature = bytes.subarray(OFFSET.SIGNATURE, OFFSET.SIGNATURE + SIGNATURE_BYTES);
    if (isZeroBlock(signature)) return refuse('signature', 'the signature is all zero -- the build pipeline did not sign this image');

    /* The container file is header + exactly image_length payload bytes; a
     * truncated or padded file would stage a digest nobody can satisfy. */
    const containerLength = HEADER_BYTES + imageLength;
    if (fileSize !== containerLength) {
      return refuse(
        'container_length',
        `the file is ${formatBytes(fileSize)}, the header declares a ${formatBytes(containerLength)} container`,
      );
    }

    /* The device would refuse this itself with UNSUPPORTED; refusing here
     * spends no upload doing it. */
    if (deviceTargetId !== '' && target.value !== deviceTargetId) {
      return refuse('target_mismatch', `the container targets "${target.value}", this device is "${deviceTargetId}"`);
    }

    return {
      ok: true,
      header: {
        magic,
        formatVersion,
        headerLength,
        imageLength,
        containerLength,
        signatureScheme: scheme,
        signatureSchemeName: SCHEME_NAMES[scheme],
        slotId,
        slotName: SLOT_NAMES[slotId],
        buildUtc,
        targetId: target.value,
        buildRevision: revision.value,
        payloadDigestHex: hex(payloadDigest),
        signatureHex: hex(signature),
        headerCrc32: headerCrc,
      },
    };
  }

  const formatUtc = (seconds) => `${new Date(seconds * 1000).toISOString().slice(0, 19).replace('T', ' ')} UTC`;

  function renderIdentity(header) {
    const put = (id, text) => {
      $(id).textContent = text;
    };
    if (!header) {
      for (const id of ['hdr-target', 'hdr-revision', 'hdr-built', 'hdr-image-length', 'hdr-container', 'hdr-scheme', 'hdr-slot', 'hdr-digest', 'hdr-signature', 'hdr-crc']) {
        put(id, '--');
      }
      return;
    }
    put('hdr-target', header.targetId);
    put('hdr-revision', header.buildRevision);
    put('hdr-built', formatUtc(header.buildUtc));
    put('hdr-image-length', `${formatBytes(header.imageLength)} (${header.imageLength} bytes)`);
    put('hdr-container', `${formatBytes(header.containerLength)} (${header.containerLength} bytes)`);
    put('hdr-scheme', `${header.signatureSchemeName} (${header.signatureScheme})`);
    put('hdr-slot', `${header.slotName} (${header.slotId})`);
    put('hdr-digest', header.payloadDigestHex);
    put('hdr-signature', header.signatureHex);
    put('hdr-crc', `0x${header.headerCrc32.toString(16).padStart(8, '0')}`);
  }

  /* ---- the step log -------------------------------------------------------- */

  const timestamp = () => new Date();

  function resetLog() {
    $('log').textContent = '';
    state.steps.clear();
  }

  /* Appends a step at the moment it starts, pending. createElement, not
   * innerHTML: a build revision and a fault name both come off the wire. */
  function logStep(key, text) {
    const entry = document.createElement('li');
    entry.className = 'ota-log__entry';
    entry.dataset.step = key;
    entry.dataset.tone = 'pending';

    const at = timestamp();
    const time = document.createElement('time');
    time.className = 'ota-log__time';
    time.dateTime = at.toISOString();
    time.textContent = at.toTimeString().slice(0, 8);

    const label = document.createElement('span');
    label.className = 'ota-log__text';
    label.textContent = text || STEP_TEXT[key];

    entry.append(time, label);
    $('log').append(entry);
    state.steps.set(key, entry);
    return entry;
  }

  const stepEntry = (key) => state.steps.get(key) || logStep(key);

  /* Live narration inside an already-appended step (the upload's byte
   * offsets), leaving its timestamp and tone alone. */
  function updateStep(key, text) {
    stepEntry(key).querySelector('.ota-log__text').textContent = text;
  }

  /* Settles a step green or red. A failure carries the fault name -- the word
   * the device used, so an operator can grep the firmware for it. */
  function settleStep(key, tone, text, fault) {
    const entry = stepEntry(key);
    entry.dataset.tone = tone;
    if (text) entry.querySelector('.ota-log__text').textContent = text;
    if (fault) {
      /* One fault line per entry: a step settled twice states its LAST fault
       * rather than growing a column of them. */
      const existing = entry.querySelector('.ota-log__detail');
      const detail = existing || document.createElement('span');
      detail.className = 'ota-log__detail';
      detail.textContent = `fault: ${fault}`;
      if (!existing) entry.append(detail);
    }
    return entry;
  }

  function logNote(text) {
    const entry = logStep('note', text);
    entry.dataset.tone = 'note';
    state.steps.delete('note'); /* notes are not steps; never reused */
    return entry;
  }

  /* The final entry: rendered as soon as an image is armed, and disabled until
   * /api/ota says this device's runtime activation latch is set. */
  function ensureActivateStep() {
    if (state.steps.has('activate')) return state.steps.get('activate');
    const entry = logStep('activate', STEP_TEXT.activate);

    const action = document.createElement('button');
    action.type = 'button';
    action.id = 'activate';
    action.className = 'ota-log__action';
    action.textContent = 'Activate';
    action.addEventListener('click', () => void guard(activateImage, cardSink('log')));

    const caption = document.createElement('span');
    caption.className = 'ota-log__caption';
    caption.id = 'activate-caption';

    entry.append(action, caption);
    renderActivateGate();
    return entry;
  }

  function renderActivateGate() {
    const entry = state.steps.get('activate');
    if (!entry) return;
    const available = Boolean(state.ota && state.ota.activation_available === true) && !state.absent;
    const action = entry.querySelector('.ota-log__action');
    const caption = entry.querySelector('.ota-log__caption');
    action.disabled = !available;
    /* activation_available is the device's RUNTIME LATCH, not a missing
     * feature: it is false at every boot and true only after an operator sends
     * POST /api/ota/enable-activation following the bench recovery proof
     * (aius_optical_app.cpp:1567-1571, optical_main.cpp:4727-4730). So the
     * caption tells the operator the act that enables this button, rather than
     * describing an unshipped slice. */
    caption.textContent = available
      ? 'The device will program flash and reboot into the new image.'
      : 'Activation is disabled on this device: /api/ota reports activation_available false. Send POST /api/ota/enable-activation (with the SD auth token) to set the runtime latch, then this step enables.';
  }

  /* ---- /api/ota poll ------------------------------------------------------- */

  /* The device answers 404 ota_absent when OTA is compiled out or the latch
   * has never been armed -- a real answer, not a failed request. */
  const isOtaAbsent = (result) =>
    result.status === 404 && result.body && faultName(result.body.error) === 'absent';
  const isOtaAnswer = (result) => isOtaAbsent(result) || (result.ok && result.body);

  /* Manual callers (the flow steps) get the answer at once; a 503 is shedding,
   * so it keeps the last record. The background poll goes through the shared
   * poller and shows "unreachable" only after three consecutive failures. */
  async function pollOta() {
    const result = await request('/api/ota', { headers: { Accept: 'application/json' } });
    if (result.status === 503) return state.ota;
    if (isOtaAnswer(result)) return applyOtaAnswer(result);
    markOtaUnreachable(result);
    return null;
  }

  function markOtaUnreachable(result) {
    state.absent = false;
    state.ota = null;
    setPill($('ota-pill'), 'unreachable');
    $('ota-grid').dataset.stale = 'true';
    setCardStatus('ota', `No answer from /api/ota -- ${describe(result)}. Values below are stale.`);
    renderControls();
  }

  function applyOtaAnswer(result) {
    if (isOtaAbsent(result)) {
      state.absent = true;
      state.ota = null;
      renderOta();
      return null;
    }
    state.absent = false;
    state.ota = result.body;
    renderOta();
    return result.body;
  }

  function renderOta() {
    const put = (id, text) => {
      $(id).textContent = text;
    };
    if (state.absent) {
      delete $('ota-grid').dataset.stale;
      setPill($('ota-pill'), 'unavailable');
      $('absent').textContent =
        'OTA IS NOT AVAILABLE ON THIS DEVICE. The running firmware was built without the OTA option, or its feature latch has never been armed, so /api/ota answers ota_absent. Nothing on this page can upgrade it; every control below is disabled.';
      $('absent').hidden = false;
      for (const id of ['ota-state', 'ota-latch', 'ota-staged', 'ota-next', 'ota-fault', 'ota-key', 'ota-activation']) {
        put(id, 'unavailable');
      }
      put('ota-target', state.deviceTargetId || 'unknown');
      $('test-key').hidden = true;
      setCardStatus('ota', 'The device reports no OTA surface.');
      renderControls();
      renderActivateGate();
      return;
    }

    $('absent').hidden = true;
    const ota = state.ota || {};
    delete $('ota-grid').dataset.stale;

    const name = stateName(ota.state);
    setPill($('ota-pill'), name === 'failed' ? 'error' : name);
    put('ota-state', name);
    put('ota-latch', ota.armed === true ? 'armed' : 'disarmed');

    /* Container-absolute in, payload out: the grid's Staged row is what the
     * operator compares against the container's "Payload length" cell, so both
     * counts drop the header the device counted in. Next offset stays in the
     * device's own coordinate -- it is the number a resume seeks to. */
    const staged = payloadBytes(ota.staged_bytes);
    const total = payloadBytes(ota.total_bytes);
    put('ota-staged', total > 0 ? `${formatBytes(staged)} of ${formatBytes(total)}` : formatBytes(staged));
    put('ota-next', `${Number(ota.next_offset) || 0} bytes (container-absolute)`);

    const fault = faultName(ota.fault);
    const verifierFault = Number(ota.verifier_fault) || 0;
    put('ota-fault', fault === '' || fault === 'ok' ? 'none' : verifierFault !== 0 ? `${fault} (verifier ${verifierFault})` : fault);
    $('ota-fault').dataset.tone = fault === '' || fault === 'ok' ? 'ok' : 'warn';

    /* key and image are nested objects in the OtaStatus document; a stale poll
     * or an unreachable device leaves them undefined. */
    const key = ota.key || {};
    const image = ota.image || {};
    const fingerprint = typeof key.fingerprint === 'string' && key.fingerprint !== '' ? key.fingerprint : 'unreported';
    put('ota-key', key.test_key === true ? `${fingerprint} (TEST KEY)` : fingerprint);
    put('ota-activation', ota.activation_available === true ? 'available' : 'disabled (enable-activation not sent)');
    if (typeof image.target_id === 'string' && image.target_id !== '' && state.deviceTargetId === '') {
      state.deviceTargetId = image.target_id;
    }
    put('ota-target', state.deviceTargetId || 'unknown');

    /* The plan's own warning, on the page it applies to: the committed dev
     * keypair's seed is published, so a TEST KEY device accepts firmware
     * signed by anyone holding the repository. */
    const banner = $('test-key');
    if (key.test_key === true) {
      banner.textContent = `TEST KEY: this device trusts the published development signing key (${fingerprint}). Anyone with the repository can sign firmware it will accept. Rotate the key before this device leaves the bench.`;
      banner.hidden = false;
    } else {
      banner.hidden = true;
    }

    setCardStatus(
      'ota',
      ota.armed === true
        ? `Device reports ${name}, latch armed.`
        : `Device reports ${name}. The OTA latch is DISARMED: staging verbs will be refused until an operator arms it.`,
    );
    if (name === 'armed' || name === 'activation_requested') ensureActivateStep();
    renderActivateGate();
    renderControls();
  }

  function renderControls() {
    const blocked = state.absent || state.running;
    $('image-file').disabled = blocked;
    $('start').disabled = blocked || state.header === null;
    $('abort').disabled = blocked;
    $('token').disabled = state.absent;
  }

  /* ---- selecting a container ---------------------------------------------- */

  async function selectContainer() {
    clearCardError('image');
    clearCardError('log');
    resetLog();
    state.file = null;
    state.header = null;
    renderIdentity(null);

    const file = $('image-file').files[0];
    if (!file) {
      setPill($('image-pill'), 'no image');
      setCardStatus('image', 'No container selected.');
      setCardStatus('log', 'Nothing attempted yet.');
      renderControls();
      return;
    }
    state.file = file;

    let bytes;
    try {
      bytes = new Uint8Array(await file.slice(0, HEADER_BYTES).arrayBuffer());
    } catch (readError) {
      setPill($('image-pill'), 'invalid');
      showCardError('image', `Could not read ${file.name}: ${readError && readError.message ? readError.message : String(readError)}`);
      renderControls();
      return;
    }

    const parsed = parseContainerHeader(bytes, file.size, state.deviceTargetId);
    if (!parsed.ok) {
      setPill($('image-pill'), 'invalid');
      setCardStatus('image', `${file.name} refused before upload.`);
      showCardError('image', `Refused: ${parsed.message}.`);
      settleStep('parsed', 'fail', `${file.name} refused: ${parsed.message}`, parsed.fault);
      setCardStatus('log', 'The container was refused here; nothing was sent to the device.');
      renderControls();
      return;
    }

    state.header = parsed.header;
    renderIdentity(parsed.header);
    setPill($('image-pill'), 'loaded');
    setCardStatus(
      'image',
      `${file.name}: ${parsed.header.buildRevision} for ${parsed.header.targetId}, ${formatBytes(parsed.header.imageLength)} payload.`,
    );
    settleStep(
      'parsed',
      'ok',
      `Selected ${file.name}: ${parsed.header.buildRevision} for ${parsed.header.targetId}, ${formatBytes(
        parsed.header.imageLength,
      )} payload, ${parsed.header.signatureSchemeName} signature, ${parsed.header.slotName}.`,
    );
    setCardStatus('log', 'Ready to upload.');
    renderControls();
  }

  /* ---- the run ------------------------------------------------------------- */

  /* Where a re-upload continues. The device's next_offset is container
   * absolute, so it resumes the header half and the payload half alike -- but
   * only for THIS container: a next_offset belonging to a different image
   * would resume a digest nobody can satisfy, so a mismatch restarts at 0 and
   * the operator's escape hatch for a device mid-staging something else is
   * Abort.
   *
   * THE "SAME CONTAINER" TEST IS AGAINST containerLength, NOT imageLength.
   * total_bytes is image_length + AIUS_OTA_HEADER_BYTES
   * (optical_main.cpp:4706-4709); comparing it to the payload length made the
   * test fail for every real device, so every re-run silently restarted at 0
   * while the log promised a resume (defect 39's semantic half). */
  function resumeOffset(ota, header) {
    if (!ota) return 0;
    const name = stateName(ota.state);
    if (name !== 'offered' && name !== 'staging') return 0;
    if (Number(ota.total_bytes) !== header.containerLength) return 0;
    const next = Number(ota.next_offset) || 0;
    if (!Number.isFinite(next) || next < HEADER_BYTES) return 0;
    if (next >= header.containerLength) return 0;
    return next;
  }

  async function runUpgrade() {
    clearCardError('log');
    if (state.header === null || state.file === null) {
      showCardError('log', 'Select a valid container first -- nothing was sent to the device.');
      return;
    }
    if (tokenMissing('log', 'stage firmware')) return;

    state.running = true;
    renderControls();
    setPill($('log-pill'), 'sending');
    try {
      const ota = await pollOta();
      if (state.absent || !ota) {
        settleStep('upload', 'fail', 'The device did not answer /api/ota; nothing was uploaded.', state.absent ? 'absent' : 'unreachable');
        setCardStatus('log', 'The run stopped before the first chunk.');
        setPill($('log-pill'), 'error');
        return;
      }
      /* The identity may have arrived after the file was picked, so the target
       * rule is re-applied against what the device says NOW. */
      if (state.deviceTargetId !== '' && state.header.targetId !== state.deviceTargetId) {
        const message = `the container targets "${state.header.targetId}", this device is "${state.deviceTargetId}"`;
        settleStep('parsed', 'fail', `Refused: ${message}.`, 'target_mismatch');
        showCardError('image', `Refused: ${message}.`);
        setPill($('image-pill'), 'invalid');
        setPill($('log-pill'), 'error');
        state.header = null;
        setCardStatus('log', 'Nothing was sent to the device.');
        return;
      }
      if (!(await uploadContainer(ota))) return;
      if (!(await confirmStaged())) return;
      if (!(await verifyImage())) return;
      if (!(await armImage())) return;
      setPill($('log-pill'), 'armed');
      setCardStatus('log', 'The image is armed. The activation step below is the device-gated slice.');
    } finally {
      state.running = false;
      renderControls();
    }
  }

  /* Is this refusal the device's "come back in a moment"? Exactly one shape
   * qualifies: 503 with the sd_busy token. A 503 carrying any other token (an
   * ota_status read failure, say) is a stopped run like every other refusal --
   * retrying a fault the device latched only spends chunks. */
  const isTransientBusy = (result) =>
    result.status === 503 && faultName(result.body && result.body.error ? result.body.error : '') === 'sd_busy';

  /* One chunk, with the bounded sd_busy retry around it. Always answers a
   * result -- the last one seen -- so the caller's refusal handling is
   * unchanged and a run that exhausts the retries stops exactly where it did
   * before, at the SAME offset (nothing was absorbed, so the device's
   * next_offset is still the one the loop is standing on). */
  async function putChunk(offset, end, total, chunk) {
    let result = null;
    for (let attempt = 0; attempt <= SD_BUSY_RETRIES; attempt++) {
      if (attempt > 0) {
        updateStep(
          'upload',
          `Device busy at ${formatBytes(offset)} of ${formatBytes(total)}; retry ${attempt} of ${SD_BUSY_RETRIES}`,
        );
        await sleep(SD_BUSY_RETRY_MS);
      }
      result = await request('/api/ota/image', {
        method: 'PUT',
        headers: mutationHeaders({
          'Content-Type': 'application/octet-stream',
          'Content-Range': `bytes ${offset}-${end - 1}/${total}`,
        }),
        body: chunk,
      });
      if (result.ok || !isTransientBusy(result)) return result;
    }
    return result;
  }

  async function uploadContainer(ota) {
    const header = state.header;
    const file = state.file;
    const total = header.containerLength;
    let offset = resumeOffset(ota, header);

    logStep('upload', offset > 0 ? `Resuming upload at ${formatBytes(offset)} of ${formatBytes(total)}` : `Uploading ${formatBytes(total)}`);
    if (offset > 0) {
      setCardStatus('log', `Resuming at the offset the device reports (${offset} bytes).`);
    }

    while (offset < total) {
      /* The header is its own chunk at offset 0: the device offers it before
       * it can absorb a single payload byte. */
      const end = offset === 0 ? HEADER_BYTES : Math.min(total, offset + CHUNK_BYTES);
      let chunk;
      try {
        chunk = await file.slice(offset, end).arrayBuffer();
      } catch (readError) {
        settleStep(
          'upload',
          'fail',
          `Could not read the container at ${offset} bytes: ${readError && readError.message ? readError.message : String(readError)}.`,
          'file_read',
        );
        setPill($('log-pill'), 'error');
        return false;
      }
      const result = await putChunk(offset, end, total, chunk);
      if (!result.ok) {
        settleStep(
          'upload',
          'fail',
          `Upload stopped at ${formatBytes(offset)} of ${formatBytes(total)} -- ${describeOtaFailure(result)}.`,
          faultFor(result, state.ota),
        );
        await pollOta();
        setPill($('log-pill'), 'error');
        setCardStatus('log', `Upload the same container again to resume from ${Number(state.ota && state.ota.next_offset) || offset} bytes.`);
        return false;
      }
      /* The final chunk may answer {"ok":true} with no next_offset, exactly as
       * any Content-Range upload's last chunk does. */
      const next = Number(result.body && result.body.next_offset != null ? result.body.next_offset : end);
      offset = Number.isFinite(next) && next > offset ? next : end;
      updateStep('upload', `Uploading ${formatBytes(offset)} of ${formatBytes(total)} (offset ${offset})`);
      setCardStatus('log', `Uploaded ${offset} of ${total} bytes.`);
    }

    settleStep('upload', 'ok', `Uploaded ${formatBytes(total)} in ${CHUNK_BYTES}-byte chunks (offset ${offset}).`);
    return true;
  }

  async function confirmStaged() {
    logStep('staged', STEP_TEXT.staged);
    const ota = await pollOta();
    const name = ota ? stateName(ota.state) : 'unreachable';
    if (name !== 'staged') {
      const latched = ota ? faultName(ota.fault) : 'unreachable';
      settleStep('staged', 'fail', `The device reports ${name} after the last chunk, not staged.`, latched !== '' && latched !== 'ok' ? latched : name);
      setPill($('log-pill'), 'error');
      return false;
    }
    settleStep('staged', 'ok', `Staged ${formatBytes(payloadBytes(ota.staged_bytes))}; the device holds the whole payload.`);
    return true;
  }

  /* One verb, three steps in a fixed order: aius_ota_verify() finalizes the
   * incremental digest first, only then hands the authentication digest to
   * the verifier, and only after that judges the authenticated payload's IVT
   * against the device's flash window. So a digest refusal means the
   * signature check never ran, and an IVT refusal means both passed. */
  async function verifyImage() {
    logStep('digest', STEP_TEXT.digest);
    const result = await request('/api/ota/verify', { method: 'POST', headers: mutationHeaders({}) });
    const ota = await pollOta();
    const name = ota ? stateName(ota.state) : 'unreachable';
    const fault = ota ? faultName(ota.fault) : '';

    if (fault === 'digest') {
      settleStep('digest', 'fail', 'The staged bytes do not hash to the digest the header claims.', 'digest');
      setPill($('log-pill'), 'error');
      setCardStatus('log', 'No retry can fix a digest mismatch: re-upload the container.');
      return false;
    }
    if (fault === 'signature') {
      settleStep('digest', 'ok', 'Payload digest matches the header.');
      const verifierFault = Number(ota.verifier_fault) || 0;
      logStep('signature', STEP_TEXT.signature);
      settleStep(
        'signature',
        'fail',
        verifierFault !== 0
          ? `The verifier refused the signature (verifier fault ${verifierFault}).`
          : 'The verifier refused the signature.',
        'signature',
      );
      setPill($('log-pill'), 'error');
      return false;
    }
    if (fault === 'ivt') {
      settleStep('digest', 'ok', 'Payload digest matches the header.');
      logStep('signature', STEP_TEXT.signature);
      settleStep('signature', 'ok', 'Signature over the header-binding digest accepted; target and build identity are authenticated too.');
      logStep('ivt', STEP_TEXT.ivt);
      settleStep('ivt', 'fail', `${describeOtaFailure(result)}.`, 'ivt');
      setPill($('log-pill'), 'error');
      setCardStatus('log', 'No retry can fix a wrong-window image: sign the build for this device posture and re-upload.');
      return false;
    }
    if (!result.ok || name !== 'verified') {
      settleStep(
        'digest',
        'fail',
        `Verification did not run: the device reports ${name} -- ${describeOtaFailure(result)}.`,
        faultFor(result, ota),
      );
      setPill($('log-pill'), 'error');
      return false;
    }
    settleStep('digest', 'ok', 'Payload digest matches the header.');
    logStep('signature', STEP_TEXT.signature);
    settleStep('signature', 'ok', 'Signature over the header-binding digest accepted; target and build identity are authenticated too.');
    logStep('ivt', STEP_TEXT.ivt);
    settleStep('ivt', 'ok', 'Image vector table lies inside the device flash window.');
    return true;
  }

  async function armImage() {
    logStep('armed', STEP_TEXT.armed);
    const result = await request('/api/ota/arm', { method: 'POST', headers: mutationHeaders({}) });
    const ota = await pollOta();
    const name = ota ? stateName(ota.state) : 'unreachable';
    if (!result.ok || name !== 'armed') {
      settleStep('armed', 'fail', `The device reports ${name} -- ${describeOtaFailure(result)}.`, faultFor(result, ota));
      setPill($('log-pill'), 'error');
      return false;
    }
    settleStep('armed', 'ok', 'Image armed. Nothing is written to flash until it is activated.');
    ensureActivateStep();
    return true;
  }

  async function activateImage() {
    clearCardError('log');
    if (tokenMissing('log', 'activate firmware')) return;
    const result = await request('/api/ota/activate', { method: 'POST', headers: mutationHeaders({}) });
    if (!result.ok) {
      settleStep('activate', 'fail', `The device refused activation -- ${describeOtaFailure(result)}.`, faultFor(result, state.ota));
      return;
    }
    settleStep('activate', 'ok', 'Activation requested; the device programs flash and reboots into the new image.');
    await pollOta();
  }

  async function abortUpgrade() {
    clearCardError('log');
    if (!window.confirm('Abort the staged upgrade? The device discards everything uploaded so far.')) {
      setCardStatus('log', 'Abort cancelled; the staged image is untouched.');
      return;
    }
    if (tokenMissing('log', 'abort a staged upgrade')) return;
    const result = await request('/api/ota/abort', { method: 'POST', headers: mutationHeaders({}) });
    if (!result.ok) {
      showCardError('log', `Could not abort -- ${describeOtaFailure(result)}.`);
      return;
    }
    logNote('Staged upgrade aborted; the device is back to idle.');
    setPill($('log-pill'), 'idle');
    setCardStatus('log', 'Aborted. Upload a container to start again.');
    await pollOta();
  }

  /* ---- the device's own identity ------------------------------------------
   * /api/status is the board's word on which build it is. Unknown is not a
   * refusal: the device re-checks target_id at offer time and answers
   * UNSUPPORTED itself, so the page says so rather than blocking. */
  async function refreshIdentity() {
    const result = await request('/api/status', { headers: { Accept: 'application/json' } });
    if (!result.ok || !result.body) {
      renderSlotsUnreachable();
      return;
    }
    const target = result.body.target_id;
    if (typeof target === 'string' && target !== '') {
      state.deviceTargetId = target;
      $('ota-target').textContent = target;
    }
    renderSlots(result.body);
  }

  /* ---- the A/B slots (Phase 4y) ---------------------------------------------
   * /api/status `ab`: `slot` is the running PHYSICAL slot (1 = A, 2 = B) and
   * `revisions` the boot record's per-slot 12-hex revision keyed "1" / "2",
   * null when unknown. The running revision prints in that same form: the
   * status build_revision reads `<rev>+<env>`, so its part before '+' (review
   * N4), and the two slots compare at a glance. The device is the authority
   * on the switch; the page mirrors only the refusals the status already
   * proves, so the button never offers what the device will refuse. */
  const SLOT_LETTERS = Object.freeze({ 1: 'A', 2: 'B' });
  const REVISION_PATTERN = /^[0-9a-f]{12}$/;

  const slotRevision = (value) => (typeof value === 'string' && REVISION_PATTERN.test(value) ? value : '');

  /**
   * The slots card's facts from one /api/status body.
   *
   * @param {object} status the /api/status body
   * @returns {{ab: boolean, running: number, other: number, runningRevision: string,
   *   otherRevision: string, blocked: string}} blocked is '' when the switch is offered
   */
  function slotFacts(status) {
    const ab = status && typeof status.ab === 'object' ? status.ab : null;
    if (!ab || !SLOT_LETTERS[ab.slot]) {
      return { ab: false, running: 0, other: 0, runningRevision: '', otherRevision: '', blocked: 'this build has no A/B slots' };
    }
    const running = ab.slot;
    const other = running === 1 ? 2 : 1;
    const revisions = ab.revisions && typeof ab.revisions === 'object' ? ab.revisions : {};
    const fromBuild = String(status.build_revision || '').split('+')[0];
    const runningRevision = slotRevision(fromBuild) || slotRevision(revisions[String(running)]);
    const otherRevision = slotRevision(revisions[String(other)]);
    let blocked = '';
    if (ab.record_valid !== true) blocked = 'no boot record yet; the first healthy boot writes it';
    else if (ab.pending_slot !== 0) blocked = 'a trial boot is still open; wait for it to commit';
    else if (ab.active_slot !== running) blocked = 'the boot record does not name the running slot';
    else if (ab.booted_slot === 0) blocked = 'the chooser on this board predates slot switching';
    return { ab: true, running, other, runningRevision, otherRevision, blocked };
  }

  function renderSlots(status) {
    const facts = slotFacts(status);
    state.slots = facts;
    if (!facts.ab) {
      $('slot-running').textContent = 'single image';
      $('slot-running-revision').textContent = '--';
      $('slot-other').textContent = '--';
      $('slot-other-revision').textContent = '--';
      setPill($('slots-pill'), 'no A/B');
    } else {
      $('slot-running').textContent = `slot ${SLOT_LETTERS[facts.running]}`;
      $('slot-running-revision').textContent = facts.runningRevision || 'unknown';
      $('slot-other').textContent = `slot ${SLOT_LETTERS[facts.other]}`;
      $('slot-other-revision').textContent = facts.otherRevision || 'unknown';
      setPill($('slots-pill'), `slot ${SLOT_LETTERS[facts.running]}`);
    }
    $('boot-other').disabled = facts.blocked !== '';
    setCardStatus(
      'slots',
      facts.blocked !== ''
        ? `Switching is unavailable: ${facts.blocked}.`
        : `Boot slot ${SLOT_LETTERS[facts.other]} on request; it trial-boots and falls back to slot ${SLOT_LETTERS[facts.running]} on its own if it never comes up healthy.`,
    );
  }

  function renderSlotsUnreachable() {
    state.slots = null;
    $('boot-other').disabled = true;
    setPill($('slots-pill'), 'unreachable');
    setCardStatus('slots', 'Could not read /api/status; the slot state is unknown.');
  }

  /**
   * The switch's confirm text: the target slot and revision, the no-way-back
   * warning, and, running slot B, that slot A's boot header goes unchecked.
   *
   * @param {{running: number, other: number, otherRevision: string}} facts
   * @returns {string}
   */
  function bootOtherConfirmText(facts) {
    const target = SLOT_LETTERS[facts.other];
    const lines = [
      `Boot slot ${target} (revision ${facts.otherRevision || 'unknown'})?`,
      `The board reboots now and trial-boots slot ${target}; if it never comes up healthy it falls back to slot ${SLOT_LETTERS[facts.running]} on its own.`,
      'An image without this feature has no way back except an OTA.',
    ];
    if (facts.running === 2) {
      lines.push(`Slot ${target}'s boot header is not checked first while running slot B; the chooser falls back if it is bad.`);
    }
    return lines.join('\n\n');
  }

  function describeBootOtherFailure(result) {
    const token = faultName(result.body && result.body.error ? result.body.error : '');
    if (token === 'unsupported') return 'this build or board has no A/B slots';
    if (token === 'state') {
      return 'the device refused: a trial is open, the boot record does not name this slot, another reboot or flash act is pending, or an upgrade is staged (abort it first)';
    }
    if (token === 'ivt') return "the other slot's boot header is not a bootable image; nothing was changed";
    if (token === 'sd_flush_pending') return 'a settings write has not reached the card yet; retry in a moment';
    return describeOtaFailure(result);
  }

  async function bootOtherSlot() {
    clearCardError('slots');
    const facts = state.slots;
    if (!facts || facts.blocked !== '') return;
    if (!window.confirm(bootOtherConfirmText(facts))) {
      setCardStatus('slots', 'Switch cancelled; nothing was changed.');
      return;
    }
    if (tokenMissing('slots', 'boot the other slot')) return;
    const result = await request('/api/ota/boot-other', { method: 'POST', headers: mutationHeaders({}) });
    if (!result.ok) {
      showCardError('slots', `The device refused the switch -- ${describeBootOtherFailure(result)}.`);
      return;
    }
    $('boot-other').disabled = true;
    setPill($('slots-pill'), 'rebooting');
    setCardStatus('slots', `Rebooting into slot ${SLOT_LETTERS[facts.other]}; reload this page once the board answers again.`);
  }

  /* ---- boot ---------------------------------------------------------------- */

  $('image-file').addEventListener('change', () => void guard(selectContainer, cardSink('image')));
  $('start').addEventListener('click', () => void guard(runUpgrade, cardSink('log')));
  $('abort').addEventListener('click', () => void guard(abortUpgrade, cardSink('log')));
  $('boot-other').addEventListener('click', () => void guard(bootOtherSlot, cardSink('slots')));
  $('token').value = readStoredToken();
  $('token').addEventListener('input', () => storeToken(authToken()));

  if (!CONTRACT_MODE) {
    void guard(refreshIdentity, cardSink('ota'));
    shared.createPoller({
      fetch: () => request('/api/ota', { headers: { Accept: 'application/json' } }),
      cadenceMs: OTA_POLL_MS,
      immediate: true,
      isAnswer: isOtaAnswer,
      onAnswer: applyOtaAnswer,
      onUnreachable: markOtaUnreachable,
      onError: cardSink('ota'),
    });
  }
  document.body.dataset.uiContractReady = 'aius-firmware-upgrade';
})();
