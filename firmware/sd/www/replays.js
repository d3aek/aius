/* Aius SD-hosted replay manager.
 *
 * OWNS: the whole /sd/www/replays.html page -- the replay library and its
 * per-row actions, the selection and start/stop controls, the resumable
 * chunked upload with its progress and cancel, the 1 Hz playback poll, the
 * on-demand library refresh (no timer), and the 4 Hz meter render.
 *
 * DOES NOT OWN: the shared chrome behaviours -- pills, the never-throw fetch
 * discipline, the formatters, SD-auth token storage and the waveform renderer
 * are `window.aiusSdShared` (sd-shared.js, loaded first by the page) and are
 * consumed, never redefined here. Nor anything the firmware decides: the
 * device is the authority on what a replay is and when it may start.
 *
 * This file is served off the Teensy's SD card. It is deliberately a plain
 * classic script with no imports, no build step, and no shared UI kit -- the
 * device cannot reach a host application's module mounts. Same constraint as
 * dsp-editor.js; keep the pages looking like one product.
 */
(() => {
  const CONTRACT_MODE = new URLSearchParams(window.location.search).has('contract');
  'use strict';

  const $ = (id) => document.getElementById(id);
  const shared = window.aiusSdShared;
  const { setPill, request, guard, describe, formatBytes, formatDb, formatUs, readStoredToken, storeToken, drawWaveform } =
    shared;

  const STATUS_POLL_MS = 1000; /* the replay status is an epoch-latched snapshot */
  const METER_POLL_MS = 250; /* faster than the status tick so the envelope moves (UX finding R-N7) */
  const UPLOAD_CHUNK_BYTES = 4096; /* one HTTP body per SD append job */
  const REPLAY_ID_PATTERN = /^[A-Za-z0-9_-]{1,48}$/; /* AIUS_HTTP_MAX_REPLAY_ID and its charset */

  const state = {
    files: [],
    selected: '', /* the operator's choice; the polls must never move it */
    activeId: '', /* the id the device reports playing, for the row marker */
    uploadTotals: new Map(), /* id -> intended byte total, known only for this session's uploads */
    upload: null, /* { id, total, offset, cancelled } while a chunk loop runs */
  };

  /* ---- per-card status and error lines ------------------------------------
   * Every acting card owns a status line (role=status) and an error line
   * (role=alert), and a failure lands in the card whose control was pressed --
   * a Delete refusal used to surface at the foot of a different card (UX
   * finding P4). The POLLS deliberately report through the pill and the status
   * line only: routing a once-a-second failure into the alert line would
   * overwrite whatever the operator's last button press had to say. */

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

  /* MUTATIONS ONLY. Every GET this page makes is a public route on the device
   * (dispatch_replays()'s `public_route`), so a token on one would be a
   * credential sprayed for nothing (UX finding P7). */
  function mutationHeaders(extra) {
    const token = authToken();
    return token === '' ? Object.assign({}, extra) : Object.assign({}, extra, { 'X-Aius-SD-Auth': token });
  }

  function tokenMissing(card, action) {
    if (authToken() !== '') return false;
    showCardError(card, `SD auth token required to ${action}. Read it over USB with the AIUS AUTH command.`);
    return true;
  }

  /* The refusal tokens dispatch_replays() and dispatch_sd_upload() can emit,
   * said in operator terms. Anything else falls back to the shared HTTP
   * description rather than being guessed at. */
  function describeReplayFailure(result) {
    const token = result.body && result.body.error ? result.body.error : '';
    if (token === 'replay_carrier_present') {
      return 'the optical input is live and owns the audio path; the device will not displace it';
    }
    if (token === 'replay_not_found') return 'no such replay on the card';
    if (token === 'replay_active') return 'that replay is playing; stop it first';
    if (token === 'replay_start') return 'the device refused the start (the id may be unusable)';
    if (token === 'replay_body') return 'the device rejected the request body or the id';
    if (token === 'sd_auth') return 'the SD auth token was rejected';
    if (token === 'sd_busy') return 'the SD volume is busy (replay refill or upload in flight); retry shortly';
    if (token === 'sd_upload') return 'the SD volume is busy or refused the path';
    if (token === 'sd_capacity') return 'the card is full';
    if (token === 'sd_unavailable') return 'no SD card is mounted';
    return describe(result);
  }

  const percentOf = (value, total) => (total > 0 ? `${Math.min(100, Math.round((100 * value) / total))} %` : '--');

  /* ---- library ------------------------------------------------------------ */

  /* Manual refreshes (buttons, post-mutation) show a failure at once; the
   * background poll goes through the shared poller instead and shows
   * "unreachable" only after three consecutive failures. A 503 is shedding,
   * never a failure: both paths leave the last listing and pill untouched. */
  async function refreshFiles() {
    const result = await request('/api/replays', { headers: { Accept: 'application/json' } });
    if (result.status === 503) return;
    if (!result.ok || !result.body) {
      markFilesUnreachable(result);
      return;
    }
    renderFilesAnswer(result);
  }

  function markFilesUnreachable(result) {
    setPill($('library-state'), 'unreachable');
    $('files').dataset.stale = 'true';
    setCardStatus('library', `Listing not refreshed -- ${describe(result)}. The rows below are the last good listing.`);
  }

  function renderFilesAnswer(result) {
    /* A missing card answers 200 with an error token instead of failing the
     * request (dispatch_replays()'s pre-cutover listing behaviour), so "no
     * files on the card" and "no card" arrive with the same HTTP status and
     * must not read the same. */
    if (result.body.ok === false || result.body.error) {
      setPill($('library-state'), 'error');
      $('files').dataset.stale = 'true';
      setCardStatus('library', `The device cannot list replays -- ${describeReplayFailure(result)}.`);
      return;
    }
    delete $('files').dataset.stale;
    state.files = Array.isArray(result.body.files) ? result.body.files : [];
    renderFiles(state.files);
    const playable = state.files.filter((file) => file.complete).length;
    setCardStatus(
      'library',
      `${state.files.length} file${state.files.length === 1 ? '' : 's'} on the card, ${playable} playable.`,
    );
    /* The listing states its own completeness; a silently partial list would
     * read as "those are all your captures". */
    if (result.body.truncated) {
      setPill($('library-state'), 'truncated');
      showCardError('library', 'The card holds more replays than one listing can carry; this list is INCOMPLETE.');
      return;
    }
    setPill($('library-state'), 'idle');
  }

  function renderFiles(files) {
    const list = $('files');
    list.textContent = '';
    if (files.length === 0) {
      const empty = document.createElement('p');
      empty.className = 'muted';
      empty.textContent = 'No replay files on the SD card.';
      list.append(empty);
    } else {
      for (const file of files) list.append(fileRow(file));
    }
    renderSelect(files);
  }

  /* createElement, not innerHTML: an id comes off the card's directory and is
   * never markup here, so there is nothing to escape and nothing to get wrong
   * (UX consolidation row 11). */
  function fileRow(file) {
    const row = document.createElement('div');
    row.className = 'file';
    row.dataset.id = file.id;

    const name = document.createElement('strong');
    name.textContent = file.id;
    if (file.id === state.activeId) {
      const marker = document.createElement('span');
      marker.className = 'file__playing';
      marker.textContent = 'playing';
      name.append(' ', marker);
    }

    const meta = document.createElement('small');
    meta.textContent = describeFile(file);
    if (!file.complete) meta.dataset.tone = 'warn';

    const play = document.createElement('button');
    play.type = 'button';
    play.dataset.action = 'play';
    play.textContent = 'Play';
    /* An incomplete capture is not playable: its words stop mid-stream. The
     * page used to offer Play on one anyway (UX finding P6). */
    play.disabled = !file.complete;
    if (!file.complete) play.title = 'Incomplete upload -- finish it by uploading the same ID again.';
    play.addEventListener('click', () => void guard(() => startReplay(file.id, 'library'), cardSink('library')));

    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'danger';
    remove.dataset.action = 'delete';
    remove.textContent = 'Delete';
    remove.addEventListener('click', () => void guard(() => deleteReplay(file.id), cardSink('library')));

    row.append(name, meta, play, remove);
    return row;
  }

  /* A `.part` row serves entry.size() as BOTH `bytes` and `upload_next_offset`
   * (optical_main.cpp's replay listing), so the device cannot know the total
   * the operator intended -- "upload N/N" was the old page reading that as a
   * finished transfer. "of M" is therefore claimed only for an upload this
   * page session started; otherwise the honest statement is what is on the
   * card so far. */
  function describeFile(file) {
    const bytes = Number(file.bytes) || 0;
    if (file.complete) return `${formatBytes(bytes)} · ${Number(file.duration_seconds || 0).toFixed(1)} s`;
    const written = Number(file.upload_next_offset != null ? file.upload_next_offset : bytes) || 0;
    const total = state.uploadTotals.get(file.id);
    return total
      ? `incomplete upload, ${formatBytes(written)} of ${formatBytes(total)} (resume with the same ID)`
      : `incomplete upload, ${formatBytes(written)} on the card (resume with the same ID)`;
  }

  /* The 5 s refresh rebuilt this select from scratch and wiped the operator's
   * choice mid-gesture (UX finding P2). The selection is page state now: the
   * device's active replay only ever seeds an empty one. */
  function renderSelect(files) {
    const select = $('selected');
    const playable = files.filter((file) => file.complete);
    const lost = state.selected !== '' && !playable.some((file) => file.id === state.selected);
    const lostId = state.selected;
    if (lost) state.selected = '';
    if (state.selected === '' && state.activeId !== '' && playable.some((file) => file.id === state.activeId)) {
      state.selected = state.activeId;
    }

    select.textContent = '';
    const none = document.createElement('option');
    none.value = '';
    none.textContent = playable.length > 0 ? 'No replay selected' : 'No playable replay on the card';
    select.append(none);
    for (const file of playable) {
      const option = document.createElement('option');
      option.value = file.id;
      option.textContent = file.id;
      select.append(option);
    }
    select.value = state.selected;
    /* The alert line, not the status line: the 1 Hz status poll would paint
     * over a status-line notice within a second of it appearing. */
    if (lost) showCardError('playback', `"${lostId}" is no longer on the card; nothing is selected.`);
  }

  /* ---- playback ----------------------------------------------------------- */

  async function refreshStatus() {
    const result = await request('/api/replays/status', { headers: { Accept: 'application/json' } });
    if (result.status === 503) return;
    if (!result.ok || !result.body) {
      markStatusUnreachable(result);
      return;
    }
    renderStatusAnswer(result);
  }

  function markStatusUnreachable(result) {
    /* A frozen grid beside a confident "playing" pill was how a dead device
     * read a second ago (UX finding P4). */
    setPill($('state'), 'unreachable');
    $('playback-grid').dataset.stale = 'true';
    setCardStatus('playback', `No answer from /api/replays/status -- ${describe(result)}. Values below are stale.`);
  }

  function renderStatusAnswer(result) {
    delete $('playback-grid').dataset.stale;
    renderStatus(result.body);
  }

  function renderStatus(status) {
    state.activeId = status.id || '';
    setPill($('state'), status.state || 'unknown');
    $('active').textContent = status.id || 'none';

    const fileBytes = Number(status.file_bytes) || 0;
    const offset = Number(status.offset_bytes) || 0;
    $('position').textContent =
      fileBytes > 0 ? `${formatBytes(offset)} / ${formatBytes(fileBytes)} (${percentOf(offset, fileBytes)})` : '--';

    const queue = Number(status.queue_words) || 0;
    const watermark = Number(status.watermark_words) || 0;
    $('queue').textContent =
      watermark > 0 ? `${queue} / ${watermark} words (${percentOf(queue, watermark)})` : `${queue} words`;

    const underruns = Number(status.underruns) || 0;
    $('underruns').textContent = String(underruns);
    /* An underrun is the one counter on this page that means the audio broke;
     * it never stood out before (UX finding R-N8). */
    if (underruns > 0) $('underruns').dataset.tone = 'warn';
    else delete $('underruns').dataset.tone;

    $('loops').textContent = String(Number(status.loops_completed) || 0);
    $('classifier').textContent = status.input_class || 'unknown';
    $('stop-reason').textContent = status.stop_reason || 'none';
    $('decoder').textContent = `decoder errors ${Number(status.decoder_errors) || 0} · refill faults ${
      Number(status.refill_faults) || 0
    }`;
    $('buffer').value = watermark > 0 ? Math.min(100, (100 * queue) / watermark) : 0;
    setCardStatus(
      'playback',
      status.id ? `Device reports ${status.state} on "${status.id}".` : `Device reports ${status.state}.`,
    );
  }

  async function startReplay(id, card) {
    clearCardError(card);
    if (!id) {
      /* Start with nothing selected used to be a silent no-op (UX finding P2). */
      showCardError(card, 'Select a replay first -- nothing was sent to the device.');
      return;
    }
    if (tokenMissing(card, 'start a replay')) return;
    setCardStatus('playback', `Starting "${id}"...`);
    const result = await request('/api/replays/start', {
      method: 'POST',
      headers: mutationHeaders({ 'Content-Type': 'application/json' }),
      body: JSON.stringify({ id, loop: $('loop').checked }),
    });
    if (!result.ok) {
      showCardError(card, `Could not start "${id}" -- ${describeReplayFailure(result)}.`);
      setCardStatus('playback', 'Playback unchanged.');
      await refreshFiles();
      return;
    }
    state.selected = id;
    await refreshStatus();
    await refreshFiles();
  }

  async function stopReplay() {
    clearCardError('playback');
    if (tokenMissing('playback', 'stop a replay')) return;
    const result = await request('/api/replays/stop', { method: 'POST', headers: mutationHeaders({}) });
    if (!result.ok) {
      showCardError('playback', `Could not stop the replay -- ${describeReplayFailure(result)}.`);
      return;
    }
    await refreshStatus();
  }

  async function deleteReplay(id) {
    clearCardError('library');
    /* One click, no guard, forty pixels from Play was the worst way to lose a
     * capture (UX finding P3). */
    if (!window.confirm(`Delete replay "${id}" from the SD card? This cannot be undone.`)) {
      setCardStatus('library', `Delete of "${id}" cancelled.`);
      return;
    }
    if (tokenMissing('library', 'delete a replay')) return;
    setCardStatus('library', `Deleting "${id}"...`);
    const result = await request('/api/replays', {
      method: 'DELETE',
      headers: mutationHeaders({ 'Content-Type': 'application/json' }),
      body: JSON.stringify({ id }),
    });
    if (!result.ok) {
      showCardError('library', `Could not delete "${id}" -- ${describeReplayFailure(result)}.`);
      await refreshFiles();
      return;
    }
    state.uploadTotals.delete(id);
    /* Refresh first, or its count message eats the confirmation. */
    await refreshFiles();
    setCardStatus('library', `Deleted "${id}".`);
  }

  /* ---- upload ------------------------------------------------------------- */

  /* Where the card says this id got to. The engine verifies the claimed offset
   * against the .part's real size and faults on a mismatch, so a resume MUST
   * start at the listed offset -- restarting at 0 over a non-empty .part is a
   * 500, not an overwrite. */
  function resumeOffsetFor(id) {
    const existing = state.files.find((file) => file.id === id && !file.complete);
    if (!existing) return 0;
    return Number(existing.upload_next_offset != null ? existing.upload_next_offset : existing.bytes) || 0;
  }

  function setUploadBusy(busy) {
    $('upload-start').disabled = busy;
    $('upload-cancel').disabled = !busy;
  }

  function renderUploadProgress(offset, total) {
    const done = total > 0 ? Math.min(100, (100 * offset) / total) : 0;
    $('upload-progress').value = done;
    $('upload-percent').textContent = `${done.toFixed(0)} %`;
  }

  async function runUpload() {
    clearCardError('upload');
    const file = $('file').files[0];
    const id = $('replay-id').value.trim();
    if (!file) {
      showCardError('upload', 'Choose a capture file first.');
      return;
    }
    if (!REPLAY_ID_PATTERN.test(id)) {
      showCardError('upload', 'Replay ID must be 1 to 48 characters of A-Z, a-z, 0-9, underscore or hyphen.');
      return;
    }
    if (tokenMissing('upload', 'upload a capture')) return;

    const resume = resumeOffsetFor(id);
    if (resume > file.size) {
      showCardError(
        'upload',
        `The card already holds ${formatBytes(resume)} for "${id}", more than this ${formatBytes(
          file.size,
        )} file. Delete that replay or choose another ID.`,
      );
      return;
    }

    state.uploadTotals.set(id, file.size);
    state.upload = { id, total: file.size, offset: resume, cancelled: false };
    setUploadBusy(true);
    setPill($('upload-state'), 'sending');
    renderUploadProgress(resume, file.size);
    setCardStatus(
      'upload',
      resume > 0
        ? `Resuming "${id}" at ${formatBytes(resume)} of ${formatBytes(file.size)}...`
        : `Uploading ${file.name} as "${id}" (${formatBytes(file.size)})...`,
    );

    let offset = resume;
    while (offset < file.size) {
      if (state.upload.cancelled) {
        setPill($('upload-state'), 'stopped');
        setUploadBusy(false);
        state.upload = null;
        await refreshFiles();
        setCardStatus(
          'upload',
          `Upload cancelled at ${formatBytes(offset)} of ${formatBytes(file.size)}. Upload the same ID again to resume.`,
        );
        return;
      }
      const end = Math.min(file.size, offset + UPLOAD_CHUNK_BYTES);
      const chunk = await file.slice(offset, end).arrayBuffer();
      const result = await request(`/sd/replays/${encodeURIComponent(id)}`, {
        method: 'PUT',
        headers: mutationHeaders({
          'Content-Type': 'application/octet-stream',
          'Content-Range': `bytes ${offset}-${end - 1}/${file.size}`,
        }),
        body: chunk,
      });
      if (!result.ok) {
        setPill($('upload-state'), 'error');
        showCardError(
          'upload',
          `Upload paused at ${formatBytes(offset)} of ${formatBytes(file.size)} -- ${describeReplayFailure(
            result,
          )}. Upload the same ID again to resume.`,
        );
        setUploadBusy(false);
        state.upload = null;
        await refreshFiles();
        return;
      }
      /* The final chunk answers {"ok":true} with no next_offset. */
      offset = Number(result.body && result.body.next_offset != null ? result.body.next_offset : end);
      state.upload.offset = offset;
      renderUploadProgress(offset, file.size);
      setCardStatus('upload', `Uploading "${id}": ${formatBytes(offset)} of ${formatBytes(file.size)}.`);
    }

    setPill($('upload-state'), 'idle');
    renderUploadProgress(file.size, file.size);
    setUploadBusy(false);
    state.upload = null;
    await refreshFiles();
    /* After the refresh, or its count message eats the completion notice. */
    setCardStatus('upload', `Upload complete: "${id}", ${formatBytes(file.size)} written to the card.`);
  }

  function cancelUpload() {
    if (!state.upload) return;
    state.upload.cancelled = true;
    setCardStatus('upload', 'Cancelling after the chunk in flight...');
  }

  /* ---- meters ------------------------------------------------------------- */

  /* GET /api/meters wire shape, settled in docs/plans/aius-dsp-config-api.md
     §Meters endpoint: waveform points are per-mille integers 0..1000 of full
     scale (non-negative magnitudes, capped at 128 points, drawn mirrored about
     the centre line by the shared renderer), output peak/rms are [L, R]
     arrays, levels are dBFS floored at -120, and sequence 0 means "never
     published". */
  const meterPair = (values) => {
    const pair = Array.isArray(values) ? values : [];
    return `${formatDb(pair[0])} / ${formatDb(pair[1])}`;
  };

  async function refreshMeters() {
    const result = await request('/api/meters', { headers: { Accept: 'application/json' } });
    if (result.status === 503) return;
    if (!result.ok || !result.body) {
      markMetersUnreachable(result);
      return;
    }
    renderMeters(result.body);
  }

  function markMetersUnreachable(result) {
    setPill($('meter-state'), 'unreachable');
    $('meter-summary').textContent = `No meter snapshot -- ${describe(result)}.`;
    drawWaveform($('waveform'), []);
  }

  function renderMeters(meters) {
    const published = Number(meters.sequence) > 0;
    drawWaveform($('waveform'), published ? meters.waveform : []);
    if (!published) {
      setPill($('meter-state'), 'no data');
      $('meter-summary').textContent = 'No meter snapshot published yet.';
      return;
    }
    setPill($('meter-state'), 'live');
    const output = meters.output || {};
    $('meter-summary').textContent = `Output peak ${meterPair(output.peak_dbfs)} dBFS · RMS ${meterPair(
      output.rms_dbfs,
    )} dBFS · latency ${formatUs(Number(meters.estimated_latency_us))}`;
  }

  /* ---- boot --------------------------------------------------------------- */

  $('start').addEventListener('click', () => void guard(() => startReplay(state.selected, 'playback'), cardSink('playback')));
  $('stop').addEventListener('click', () => void guard(stopReplay, cardSink('playback')));
  $('refresh').addEventListener('click', () => void guard(refreshFiles, cardSink('library')));
  $('selected').addEventListener('change', () => {
    state.selected = $('selected').value;
  });
  $('upload').addEventListener('submit', (event) => {
    event.preventDefault();
    void guard(runUpload, cardSink('upload'));
  });
  $('upload-cancel').addEventListener('click', cancelUpload);
  $('token').value = readStoredToken();
  $('token').addEventListener('input', () => storeToken(authToken()));

  if (!CONTRACT_MODE) {
    const startPoller = (url, cadenceMs, card, onAnswer, onUnreachable) =>
      shared.createPoller({
        fetch: () => request(url, { headers: { Accept: 'application/json' } }),
        cadenceMs,
        immediate: true,
        onAnswer,
        onUnreachable,
        onError: cardSink(card),
      });
    startPoller('/api/replays/status', STATUS_POLL_MS, 'playback', renderStatusAnswer, markStatusUnreachable);
    /* Replay is test-only: the listing walks the card and its reads cancel the
     * cache fill, so it loads once, after a start/delete, and when the tab
     * returns -- never on a timer. */
    void guard(refreshFiles, cardSink('library'));
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') void guard(refreshFiles, cardSink('library'));
    });
    startPoller('/api/meters', METER_POLL_MS, 'playback', (result) => renderMeters(result.body), markMetersUnreachable);
  }
  document.body.dataset.uiContractReady = 'aius-firmware-replays';
})();
