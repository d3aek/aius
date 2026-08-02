/* Aius SD-hosted device status.
 *
 * OWNS: the whole /sd/www/status.html page -- one GET /api/status per second
 * rendered into five cards (audio path, configuration, network, storage,
 * board), the input source derived from the carrier and the transmit gate,
 * and the signal-flow line that says whether anything is reaching the output.
 *
 * DOES NOT OWN: any mutation. Every route this page touches is a public read,
 * so there is no auth token here at all -- the two sibling pages own the
 * writes. The shared chrome behaviours (pills, the never-throw fetch,
 * formatters) are `window.aiusSdShared` (sd-shared.js, loaded first) and are
 * consumed, never redefined.
 *
 * This file is served off the Teensy's SD card. It is deliberately a plain
 * classic script with no imports, no build step, and no shared UI kit -- the
 * device cannot reach a host application's module mounts. Same constraint as
 * dsp-editor.js and replays.js; keep the pages looking like one product.
 */
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const shared = window.aiusSdShared;
  const { setPill, request, guard, describe, formatBytes } = shared;

  const STATUS_POLL_MS = 1000; /* the status JSON is an epoch-latched snapshot */

  /* Numeric vocabularies mirrored from the firmware the way dsp-editor.js
   * mirrors AIUS_DSP_MODEL_MAX_JSON_BYTES: the wire carries the raw number and
   * nothing on it says what the number means.
   *   state   optical_main.cpp current_state(): 2 no carrier, 3 PCM, 4 bitstream
   *   format  AiusPipelineClass: 0 unknown, 1 PCM, 2 IEC 61937 bitstream
   *   codec   pipeline_diag.codec_pc: 0 unknown, 1 AC-3, 11/12/13 DTS,
   *           21 E-AC-3
   *   the network and checkpoint vocabularies are the ones swagger's Status
   *   schema documents for network_state, network_last_fault and
   *   last_checkpoint. */
  const AUDIO_STATES = { 2: 'no carrier', 3: 'PCM' };
  const STREAM_FORMATS = { 0: 'unknown', 1: 'PCM (IEC 60958)' };
  const NETWORK_STATES = ['stack-start', 'dhcp-wait', 'online', 'teardown', 'backoff', 'recovery-safe'];
  const NETWORK_FAULTS = [
    'none',
    'dhcp-timeout',
    'link-lost',
    'stack-start',
    'dhcp-start',
    'listener',
    'udp',
    'socket',
    'teardown',
    'callback-overflow',
    'driver-protocol',
  ];
  const CHECKPOINTS = [
    'reset-capture',
    'usb-rx',
    'usb-tx',
    'metrics',
    'network',
    'http',
    'udp',
    'replay',
    'sd',
    'epoch-complete',
  ];

  const PILLS = ['audio-pill', 'config-pill', 'network-pill', 'storage-pill', 'board-pill'];
  const GRIDS = ['audio-grid', 'config-grid', 'network-grid', 'storage-grid', 'board-grid'];

  /* The booleans arrive as 0/1 integers from the C serializer while swagger
   * types some of them as booleans; accept either rather than trusting one. */
  const truthy = (value) => value === true || value === 1 || value === '1' || value === 'true';

  const named = (table, value) => {
    const key = Number(value);
    return Number.isFinite(key) && table[key] !== undefined ? `${table[key]} (${key})` : String(value ?? '--');
  };

  const codecName = (status) => {
    const pipeline = status.pipeline_diag || {};
    const codecPc = Number(pipeline.codec_pc);
    if (codecPc === 1) return 'AC-3';
    if (codecPc === 11 || codecPc === 12 || codecPc === 13) return 'DTS';
    if (codecPc === 21) return 'E-AC-3';
    return 'unknown';
  };

  const bitstreamLabel = (status) => `bitstream (${codecName(status)})`;

  const audioState = (status) => {
    const key = Number(status.state);
    if (key === 4) return `${bitstreamLabel(status)} (4)`;
    return named(AUDIO_STATES, status.state);
  };

  const streamFormat = (status) => {
    const key = Number(status.format);
    if (key === 2) return bitstreamLabel(status);
    return Number.isFinite(key) && STREAM_FORMATS[key] !== undefined
      ? STREAM_FORMATS[key]
      : String(status.format ?? '--');
  };

  const listed = (list, value) => {
    const index = Number(value);
    return Number.isFinite(index) && list[index] !== undefined ? `${list[index]} (${index})` : String(value ?? '--');
  };

  const put = (id, text) => {
    $(id).textContent = text;
  };

  function tone(id, warn) {
    if (warn) $(id).dataset.tone = 'warn';
    else delete $(id).dataset.tone;
  }

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

  /* The one question this page exists to answer at a glance, and the device
   * never answers it directly: it serves the inputs. A locked carrier owns the
   * audio path (a replay start against one is refused with 409
   * replay_carrier_present), otherwise the gate's latched replay term says a
   * replay is feeding it, otherwise nothing is. */
  function inputSource(status) {
    if (truthy(status.carrier)) return 'optical carrier';
    const gate = status.tx_gate || {};
    if (truthy(gate.replay_running)) return 'replay';
    return 'none';
  }

  /* input -> DSP -> gate -> output, with each stage saying what it is doing.
   * The gate's own JSON is three latched integers; this is what they mean
   * together. */
  function renderFlow(status) {
    const flow = $('flow');
    const gate = status.tx_gate || {};
    const open = truthy(gate.open);
    const source = inputSource(status);
    const format = streamFormat(status);
    const stages = [
      ['input', source, source !== 'none'],
      ['DSP', format, Number(status.format) > 0],
      ['gate', open ? 'open' : 'dark', open],
      ['output', open ? 'live' : 'silent', open],
    ];
    flow.textContent = '';
    stages.forEach((stage, index) => {
      if (index > 0) {
        const arrow = document.createElement('span');
        arrow.textContent = '→';
        /* Decoration: the stage order is already in the reading order. */
        arrow.setAttribute('aria-hidden', 'true');
        flow.append(arrow);
      }
      const cell = document.createElement('span');
      cell.dataset.tone = stage[2] ? 'ok' : 'off';
      const label = document.createElement('b');
      label.textContent = stage[0];
      /* The space is a real text node, not the label's margin: an assistive
       * reader gets "gate open", not "gateopen". */
      cell.append(label, ` ${stage[1]}`);
      flow.append(cell);
    });
  }

  function renderAudio(status) {
    renderFlow(status);
    put('input-source', inputSource(status));
    put('audio-state', audioState(status));
    put('carrier', truthy(status.carrier) ? 'locked' : 'absent');
    put('format', streamFormat(status));
    const rate = Number(status.sample_rate) || 0;
    put('sample-rate', rate > 0 ? `${rate} Hz` : '-- (no carrier)');
    put('blocks', String(Number(status.blocks) || 0));
    const overruns = Number(status.overruns) || 0;
    put('overruns', String(overruns));
    tone('overruns', overruns > 0);
    const gate = status.tx_gate || {};
    put('gate-open', truthy(gate.open) ? 'open' : 'dark');
    put('gate-carrier', truthy(gate.carrier_locked) ? 'locked' : 'unlocked');
    put('gate-replay', truthy(gate.replay_running) ? 'running' : 'stopped');
  }

  function renderConfiguration(status) {
    put('dsp-source', status.source || 'unknown');
    const valid = truthy(status.config_valid);
    put('config-valid', valid ? 'yes' : 'no');
    tone('config-valid', !valid);
    const error = status.config_error ? String(status.config_error) : '';
    put('config-error', error === '' ? 'none' : `${error} (code ${Number(status.config_error_code) || 0})`);
    tone('config-error', error !== '');
    put('config-errors', String(Number(status.config_error_count) || 0));
  }

  function renderNetwork(status) {
    put('network-mode', status.network_mode || 'unknown');
    put('ip', status.ip || '--');
    const link = truthy(status.link);
    put('link', link ? 'up' : 'down');
    tone('link', !link);
    put('network-state', listed(NETWORK_STATES, status.network_state));
    const fault = Number(status.network_last_fault) || 0;
    put('network-fault', listed(NETWORK_FAULTS, status.network_last_fault));
    tone('network-fault', fault > 0);
    put('network-generation', String(Number(status.network_generation) || 0));
    put('network-restarts', String(Number(status.network_restarts) || 0));
  }

  function renderStorage(status) {
    const present = truthy(status.sd_present);
    put('sd-present', present ? 'mounted' : 'absent');
    tone('sd-present', !present);
    put('sd-filesystem', status.sd_filesystem || 'unknown');
    put('sd-free', formatBytes(Number(status.sd_free_bytes)));
    put('sd-used', `${formatBytes(Number(status.sd_used_bytes))} of ${formatBytes(Number(status.sd_total_bytes))}`);
  }

  function renderBoard(status) {
    put('boot-generation', String(Number(status.boot_generation) || 0));
    const reset = Number(status.reset_source) || 0;
    put('reset-source', `${reset} (0x${reset.toString(16).toUpperCase()})`);
    const early = Number(status.watchdog_early_resets) || 0;
    put('watchdog-early', String(early));
    tone('watchdog-early', early > 0);
    const safe = truthy(status.recovery_safe);
    put('recovery-safe', safe ? 'yes -- bounded USB only' : 'no');
    tone('recovery-safe', safe);
    put('last-checkpoint', listed(CHECKPOINTS, status.last_checkpoint));
    put('loop-epoch', String(Number(status.loop_epoch) || 0));
    /* cpu_temp_valid is the sampler's own verdict; without it the two figures
     * are whatever was last in the variables. */
    if (truthy(status.cpu_temp_valid)) {
      put('cpu-temp', `${Number(status.cpu_temp_c).toFixed(1)} °C`);
      put('cpu-temp-max', `${Number(status.cpu_temp_max_c).toFixed(1)} °C`);
    } else {
      put('cpu-temp', 'not sampled');
      put('cpu-temp-max', 'not sampled');
    }
    put('build-revision', status.build_revision || 'unknown');
    put('build-utc', status.build_utc || 'unknown');
  }

  function setAllPills(text) {
    for (const id of PILLS) setPill($(id), text);
  }

  function setGridsStale(stale) {
    for (const id of GRIDS) {
      if (stale) $(id).dataset.stale = 'true';
      else delete $(id).dataset.stale;
    }
    if (stale) $('flow').dataset.stale = 'true';
    else delete $('flow').dataset.stale;
  }

  async function poll() {
    const result = await request('/api/status', { headers: { Accept: 'application/json' } });
    if (!result.ok || !result.body) {
      /* Every grid keeps its last good values, so every grid dims: a frozen
       * "PCM / gate open" beside a live-looking pill is the defect this page
       * inherits from the replay manager's poll (UX finding P4). */
      setAllPills('unreachable');
      setGridsStale(true);
      setCardStatus('poll', `No answer from /api/status -- ${describe(result)}. Every value below is stale.`);
      return;
    }
    const status = result.body;
    renderAudio(status);
    renderConfiguration(status);
    renderNetwork(status);
    renderStorage(status);
    renderBoard(status);
    setAllPills('live');
    setGridsStale(false);
    clearCardError('poll');
    setCardStatus('poll', `Input source: ${inputSource(status)}. Snapshot at epoch ${Number(status.loop_epoch) || 0}.`);
  }

  void guard(poll, cardSink('poll'));
  setInterval(() => void guard(poll, cardSink('poll')), STATUS_POLL_MS);
})();
