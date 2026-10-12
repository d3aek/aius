/* Shared behaviours for the SD-served pages (index.html, classic.html, status.html,
 * upgrade.html).
 *
 * OWNS: the pill setter (text + data-state stamp, one vocabulary), the
 * never-throw fetch discipline, the number formatters, SD-auth token storage,
 * and the waveform renderer -- the behaviours the 2026-07-27 UX consolidation
 * found duplicated or divergent between the pages (consolidation catalogue
 * rows 1, 6, 7, 8, 9 in the task #7 slice 5 record).
 *
 * DOES NOT OWN: any page's DOM, polling cadence, or model. Pages consume
 * `window.aiusSdShared` and never mutate it. Classic script, no imports --
 * same SD-serving constraint as every file here. Load this BEFORE the page's
 * own script.
 */
(() => {
  'use strict';

  /* One credential, one name, one home. The header goes only on MUTATING
   * requests -- pages must not spray it on GETs. Accepted trade (task #7
   * interview): a stored token is readable by any script on this origin. */
  const TOKEN_STORAGE_KEY = 'aius-sd-auth';

  const METER_FLOOR_DBFS = -120; /* every emitted level is finite, floored here */
  const WAVEFORM_MAX_POINTS = 128; /* AIUS_METER_WAVEFORM_MAX */
  const WAVEFORM_FULL_SCALE = 1000; /* per-mille of full scale */

  /* Sets a status pill's text AND its data-state so the shared tone map in
   * sd-chrome.css colours it. The state IS the text: one vocabulary --
   * loading/idle/live/no data/edited/sending/pending/applied/saving/playing/
   * stopped/stale/invalid/error/unreachable -- so a new word is a deliberate
   * vocabulary change, not a page-local invention. */
  function setPill(element, text) {
    element.textContent = text;
    element.dataset.state = text;
  }

  /* Never throws: a failure comes back as data, so fire-and-forget callers
   * (click handlers, tickers) always run their own cleanup/render. */
  /* A hung fetch (half-open TCP, a listener with no free slot) would wedge a
   * self-scheduled poller forever, so a GET is aborted after REQUEST_TIMEOUT_MS
   * and answers { ok: false, status: 0, timeout: true }: a FAILURE (counts toward
   * the poller's three), never a 503 skip. Override per call with
   * options.timeoutMs (0 disables). Mutating requests (PUT/POST/DELETE) default
   * to no timeout: a first SD save legitimately takes over 7 s. */
  const REQUEST_TIMEOUT_MS = 4000;

  async function request(url, options) {
    const { timeoutMs, ...init } = options || {};
    const method = String(init.method || 'GET').toUpperCase();
    const limit = timeoutMs === undefined ? (method === 'GET' ? REQUEST_TIMEOUT_MS : 0) : timeoutMs;
    let timer = null;
    const controller = typeof AbortController === 'function' ? new AbortController() : null;
    if (controller && limit > 0 && !init.signal) init.signal = controller.signal;
    try {
      const attempt = (async () => {
        const response = await fetch(url, init);
        let body = null;
        try {
          body = await response.json();
        } catch (parseError) {
          body = null;
        }
        const retryAfter = response.headers && response.headers.get ? response.headers.get('Retry-After') : null;
        return { ok: response.ok, status: response.status, body, retryAfter };
      })();
      if (!(limit > 0)) return await attempt;
      attempt.catch(() => {}); /* an abandoned attempt's late rejection is not unhandled */
      const expiry = new Promise((resolve) => {
        timer = setTimeout(() => {
          if (controller) controller.abort();
          resolve({ ok: false, status: 0, body: null, timeout: true, transport: `timeout after ${limit} ms` });
        }, limit);
      });
      return await Promise.race([attempt, expiry]);
    } catch (error) {
      return { ok: false, status: 0, body: null, transport: error && error.message ? error.message : String(error) };
    } finally {
      if (timer !== null) clearTimeout(timer);
    }
  }

  /* Body-agnostic sibling for endpoints that answer HTML (the SD directory
   * index) while refusals stay JSON: raw text plus the error token when one
   * parses out. Never throws. */
  async function requestRaw(url, options) {
    try {
      const response = await fetch(url, options);
      const text = await response.text();
      let token = '';
      try {
        const parsed = JSON.parse(text);
        if (parsed && typeof parsed.error === 'string') token = parsed.error;
      } catch (parseError) {
        /* HTML or other non-JSON body: no token. */
      }
      return { ok: response.ok, status: response.status, text, token };
    } catch (error) {
      return {
        ok: false,
        status: 0,
        text: '',
        token: '',
        transport: error && error.message ? error.message : String(error),
      };
    }
  }

  const RETRY_AFTER_MIN_MS = 250;
  const RETRY_AFTER_MAX_MS = 5000;
  const UNREACHABLE_AFTER_FAILURES = 3;

  /* Retry-After is delta-seconds (the firmware never sends a date). Clamped so a
   * 0 does not spin and a large value does not park a page for minutes. */
  const retryAfterMs = (seconds) => {
    const parsed = Number(seconds);
    if (seconds === null || seconds === undefined || seconds === '' || !Number.isFinite(parsed)) return RETRY_AFTER_MIN_MS * 2;
    return Math.min(RETRY_AFTER_MAX_MS, Math.max(RETRY_AFTER_MIN_MS, Math.round(parsed * 1000)));
  };

  /* The one poller: SELF-SCHEDULING (the next request is armed only after the
   * previous one answered, so two never overlap on the wire) and tolerant of a
   * best-effort device.
   *   - 503 (audio_busy / sd_busy shedding): a silent skip. Nothing renders, so
   *     the last values and pill stay; the next poll waits Retry-After clamped to
   *     0.25-5 s (the cadence when the header is absent). Not a failure.
   *   - any other non-answer (transport error, timeout, other status): a
   *     failure. `onUnreachable` runs only at the 3rd consecutive one and on each
   *     one after it; the earlier ones are silent too.
   *   - an answer (default: ok with a JSON body): `onAnswer`, failures reset to 0.
   * options: { fetch: () => Promise<result>, cadenceMs, immediate, onAnswer,
   *   onUnreachable, onError, isAnswer, unreachableDelayMs }.
   * Returns { stop, failures }. A throw in a callback goes to onError and the
   * loop re-arms at the plain cadence. */
  function createPoller(options) {
    const isAnswer = options.isAnswer || ((result) => result.ok && result.body);
    let timer = null;
    let stopped = false;
    let failures = 0;
    const tick = async () => {
      let delay = options.cadenceMs;
      try {
        const result = await options.fetch();
        if (result.status === 503) {
          const asked = Number(result.retryAfter);
          if (result.retryAfter !== null && result.retryAfter !== undefined && result.retryAfter !== '' && Number.isFinite(asked)) {
            delay = retryAfterMs(asked);
          }
        } else if (isAnswer(result)) {
          failures = 0;
          await options.onAnswer(result);
        } else {
          failures += 1;
          if (failures >= UNREACHABLE_AFTER_FAILURES) {
            await options.onUnreachable(result, failures);
            if (options.unreachableDelayMs) delay = options.unreachableDelayMs;
          }
        }
      } catch (error) {
        if (options.onError) options.onError(`Page error: ${error && error.message ? error.message : String(error)}`);
      }
      if (!stopped) timer = setTimeout(() => void tick(), delay);
    };
    if (options.immediate) void tick();
    else timer = setTimeout(() => void tick(), options.cadenceMs);
    return {
      stop() {
        stopped = true;
        if (timer !== null) clearTimeout(timer);
        timer = null;
      },
      failures: () => failures,
    };
  }

  const describe = (result) =>
    result.transport
      ? `network error: ${result.transport}`
      : `HTTP ${result.status}${result.body && result.body.error ? ` (${result.body.error})` : result.token ? ` (${result.token})` : ''}`;

  /* Runs an async handler and routes any defect into the page's own error
   * sink instead of a silent unhandled rejection. */
  async function guard(run, showError) {
    try {
      await run();
    } catch (error) {
      showError(`Page error: ${error && error.message ? error.message : String(error)}`);
    }
  }

  const formatDb = (value) => {
    if (value === null || value === undefined || Number.isNaN(value)) return '--';
    if (!Number.isFinite(value) || value <= METER_FLOOR_DBFS) return '-inf';
    return value.toFixed(1);
  };

  /* KiB below 1 MiB so a 4 KB file never reads "0.00 MiB"; GiB from 1 GiB so
   * a 32 GB card never reads "30436.5 MiB". */
  const formatBytes = (bytes) => {
    if (!Number.isFinite(bytes) || bytes < 0) return '--';
    if (bytes < 1048576) return `${(bytes / 1024).toFixed(bytes < 10240 ? 1 : 0)} KiB`;
    if (bytes < 1073741824) return `${(bytes / 1048576).toFixed(bytes < 10485760 ? 2 : 1)} MiB`;
    return `${(bytes / 1073741824).toFixed(1)} GiB`;
  };

  /* Microseconds until they stop being legible, then milliseconds. */
  const formatUs = (us) => {
    if (!Number.isFinite(us) || us < 0) return '--';
    if (us < 1000) return `${Math.round(us)} µs`;
    return `${(us / 1000).toFixed(1)} ms`;
  };

  /* localStorage can throw (storage disabled, private mode); the token box
   * degrades to per-session entry rather than taking the page down. */
  function readStoredToken() {
    try {
      return localStorage.getItem(TOKEN_STORAGE_KEY) || '';
    } catch (storageError) {
      return '';
    }
  }

  function storeToken(value) {
    try {
      if (value === '') localStorage.removeItem(TOKEN_STORAGE_KEY);
      else localStorage.setItem(TOKEN_STORAGE_KEY, value);
    } catch (storageError) {
      /* nothing to do: the field still holds the value for this session */
    }
  }

  /* The one waveform renderer (UX consolidation row 6): centre line ALWAYS
   * drawn so an idle canvas reads as an instrument at zero rather than a
   * dead element, mirrored per-mille envelope when points arrive. It owns its
   * backing fit because the status page has no other canvas manager. */
  function drawWaveform(canvas, points) {
    const dpr = Math.max(1, Math.min(3, Number(window.devicePixelRatio) || 1));
    const cssWidth = canvas.clientWidth;
    const cssHeight = canvas.clientHeight;
    if (cssWidth > 0 && cssHeight > 0) {
      const width = Math.round(cssWidth * dpr);
      const height = Math.round(cssHeight * dpr);
      if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
      }
      canvas.dataset.dpr = String(dpr);
    }
    const scale = Number(canvas.dataset.dpr) || 1;
    const context = canvas.getContext('2d');
    if (!context) return;
    const { width, height } = canvas;
    const middle = height / 2;
    context.clearRect(0, 0, width, height);
    context.strokeStyle = '#2a3542';
    context.lineWidth = scale;
    context.beginPath();
    context.moveTo(0, middle);
    context.lineTo(width, middle);
    context.stroke();

    const values = (Array.isArray(points) ? points : [])
      .slice(0, WAVEFORM_MAX_POINTS)
      .map(Number)
      .filter(Number.isFinite)
      .map((value) => Math.max(0, Math.min(1, value / WAVEFORM_FULL_SCALE)));
    if (values.length === 0) return;
    const xOf = (index) => (values.length < 2 ? 0 : (index * width) / (values.length - 1));
    context.strokeStyle = '#74d7ff';
    context.lineWidth = scale;
    context.fillStyle = 'rgba(116,215,255,0.22)';
    context.beginPath();
    values.forEach((value, index) => {
      const y = middle - value * middle;
      if (index === 0) context.moveTo(xOf(index), y);
      else context.lineTo(xOf(index), y);
    });
    for (let index = values.length - 1; index >= 0; index--) {
      context.lineTo(xOf(index), middle + values[index] * middle);
    }
    context.closePath();
    context.fill();
    context.stroke();
  }

  window.aiusSdShared = Object.freeze({
    TOKEN_STORAGE_KEY,
    setPill,
    request,
    requestRaw,
    createPoller,
    retryAfterMs,
    describe,
    guard,
    formatDb,
    formatBytes,
    formatUs,
    readStoredToken,
    storeToken,
    drawWaveform,
  });
})();
