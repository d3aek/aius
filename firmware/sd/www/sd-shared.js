/* Shared behaviours for the SD-served pages (index.html, replays.html,
 * status.html).
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
   * replays.css colours it. The state IS the text: one vocabulary --
   * loading/idle/live/no data/edited/sending/pending/applied/saving/playing/
   * stopped/stale/invalid/error/unreachable -- so a new word is a deliberate
   * vocabulary change, not a page-local invention. */
  function setPill(element, text) {
    element.textContent = text;
    element.dataset.state = text;
  }

  /* Never throws: a failure comes back as data, so fire-and-forget callers
   * (click handlers, tickers) always run their own cleanup/render. */
  async function request(url, options) {
    try {
      const response = await fetch(url, options);
      let body = null;
      try {
        body = await response.json();
      } catch (parseError) {
        body = null;
      }
      return { ok: response.ok, status: response.status, body };
    } catch (error) {
      return { ok: false, status: 0, body: null, transport: error && error.message ? error.message : String(error) };
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
   * backing fit because the replay page has no other canvas manager. */
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
