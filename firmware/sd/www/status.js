/* Aius SD-hosted device status.
 *
 * OWNS: the whole /sd/www/status.html page -- one GET /api/status per second
 * rendered into four cards (configuration, network, storage, board). It polls
 * /api/status only; the meters and the audio path (input, decode state,
 * format, blocks, overruns, gate) live in the DSP editor's signal pane.
 *
 * The board card's forensic rows -- A/B slot, previous boot, crash record,
 * dark-window stamp, stall witness -- are each optional in TWO different ways
 * and the page must never collapse them: a key that is `undefined` means the
 * running image predates it (fault_record and prev_boot landed after the
 * deployed slot-B build), while `null` is the image answering "nothing to
 * report". "not reported by this image" and "none recorded" are different
 * facts, and reading the second when the first is true is how a reader
 * concludes a board is healthy without having asked it anything.
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
 * dsp-editor.js; keep the pages looking like one product.
 */
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const shared = window.aiusSdShared;
  const { setPill, request, describe, formatBytes, formatUs } = shared;

  const STATUS_POLL_MS = 1000; /* the status JSON is an epoch-latched snapshot */

  /* Numeric vocabularies mirrored from the firmware the way dsp-editor.js
   * mirrors AIUS_DSP_MODEL_MAX_JSON_BYTES: the wire carries the raw number and
   * nothing on it says what the number means. The network and checkpoint
   * vocabularies are the ones swagger's Status schema documents for
   * network_state, network_last_fault and last_checkpoint. (The audio state,
   * format and codec vocabularies live in dsp-editor.js with the audio path.) */
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
  /* AIUS_BOOT_CHECKPOINT_* in firmware/lib/aius_boot/include/aius_boot.h, all
   * ELEVEN of them. The audio checkpoint (9) was added by output-jitter Phase B
   * and this list did not follow it, so every value from 9 up was rendered one
   * name early -- an `audio` stall read as `epoch-complete`, the one word that
   * means the epoch finished. Only 10 marks a completed epoch. */
  const CHECKPOINTS = [
    'reset-capture',
    'usb-rx',
    'usb-tx',
    'metrics',
    'network',
    'http',
    'udp',
    'service',
    'sd',
    'audio',
    'epoch-complete',
  ];

  /* aius-ota-resilience.md Phase 3 (A/B slots), Phase 1 (the dark-window
   * stamp). Slot values are shared by every slot-shaped field. */
  const SLOTS = { 0: 'none', 1: 'A', 2: 'B' };
  const DARK_STATES = { 1: 'entered', 2: 'step in flight', 3: 'faulted', 4: 'completed' };
  const DARK_PHASES = {
    0: 'none',
    1: 'erase-head',
    2: 'body',
    3: 'program-head',
    4: 'ready-to-reset',
    5: 'faulted',
  };
  /* prev_boot's two out-of-band spellings (swagger PrevBoot): the ledger did
   * not decode, and the first-abort latch's no-abort / invalid values. */
  const PREV_BOOT_LEDGER_INVALID = 65535;
  const FIRST_ABORT_NONE = 254;
  const FIRST_ABORT_INVALID = 255;
  const NO_COMPARE_FAULT = 4294967295; /* ota_dark.compare_offset "no fault" */

  const PILLS = ['config-pill', 'network-pill', 'storage-pill', 'board-pill'];
  const GRIDS = ['config-grid', 'network-grid', 'storage-grid', 'board-grid'];

  /* The booleans arrive as 0/1 integers from the C serializer while swagger
   * types some of them as booleans; accept either rather than trusting one. */
  const truthy = (value) => value === true || value === 1 || value === '1' || value === 'true';

  const named = (table, value) => {
    const key = Number(value);
    return Number.isFinite(key) && table[key] !== undefined ? `${table[key]} (${key})` : String(value ?? '--');
  };

  const listed = (list, value) => {
    const index = Number(value);
    return Number.isFinite(index) && list[index] !== undefined ? `${list[index]} (${index})` : String(value ?? '--');
  };

  /* Thousands separators without toLocaleString, whose grouping depends on the
   * browser's locale: byte and step counts here are compared against figures in
   * the plans, so the rendering has to be the same everywhere. */
  const grouped = (value) => {
    const number = Number(value);
    if (!Number.isFinite(number)) return '--';
    return String(Math.trunc(number)).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  };

  const NOT_SERVED = 'not reported by this image';
  const parts = (list) => list.filter((part) => part !== '').join(' · ');

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

  /* The two facts this card kept conflating. `source`/`model_origin` describe
   * /aius/dsp.json -- what the DSP editor writes and what the audio path is
   * running. config_valid/config_error describe /aius/config.json, an optional
   * boot file of three feature flags that NO firmware path reads today
   * (grep: only aius_config.c's own defaults touch them). An editor apply never
   * creates it, so a reader who took this card's old "Configuration valid: no"
   * as a verdict on their apply was reading the wrong file's answer. */
  const DSP_SOURCES = { default: 'built-in default', sd: 'loaded from card' };
  const DSP_ORIGINS = {
    default: 'nothing applied -- factory default',
    sd: 'boot load of /aius/dsp.json',
    put: 'DSP editor apply (PUT)',
  };

  /* AIUS_CONFIG_ERR_NOT_LOADED. The parser's own vocabulary starts at 1 for
   * real refusals; 10 is the "no bytes ever reached me" default the boot load
   * stamps when /aius/config.json is absent, which is this device's normal
   * state and not a fault. */
  const CONFIG_NOT_LOADED = 10;

  function renderConfiguration(status) {
    const source = String(status.source || '');
    put('dsp-source', DSP_SOURCES[source] || source || 'unknown');
    const origin = String(status.model_origin || '');
    put('dsp-origin', DSP_ORIGINS[origin] || (origin === '' ? NOT_SERVED : origin));

    const valid = truthy(status.config_valid);
    const code = Number(status.config_error_code) || 0;
    const message = status.config_error ? String(status.config_error) : '';
    const absent = !valid && code === CONFIG_NOT_LOADED;
    if (valid) put('config-file', 'loaded');
    else if (absent) put('config-file', 'absent -- the device is running built-in defaults, which is the normal state');
    else put('config-file', `${message || 'invalid'} (code ${code}) -- the file is on the card but the parser refused it`);
    /* Only a refused file is a fault. An absent one is the expected case and
     * marking it warn is what made this card read as a standing error. */
    tone('config-file', !valid && !absent);

    const errors = Number(status.config_error_count) || 0;
    put('config-errors', absent && errors > 0 ? `${grouped(errors)} (the absent file above)` : grouped(errors));
    tone('config-errors', errors > 0 && !absent);
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
    renderSlots(status);
    renderPrevBoot(status);
    renderFaultRecord(status);
    renderOtaDark(status);
    renderStallWitness(status);
  }

  /* One row per forensic object, each answering the same three-way question:
   * does this image serve the key at all, does it have a record, and what does
   * the record say. `missing` writes the two absent cases and returns true so
   * each renderer's real body only ever runs against a real record. */
  function missing(id, value, empty) {
    if (value !== undefined && value !== null) return false;
    put(id, value === undefined ? NOT_SERVED : empty);
    tone(id, false);
    return true;
  }

  /* Which flash slot this image runs from, the ping-pong boot record the
   * application writes, and the trial ledger the first-stage chooser writes.
   * The slot the build was linked for differing from the record's committed
   * active slot IS the open-trial state: this boot is the one on probation. */
  function renderSlots(status) {
    const ab = status.ab;
    if (ab === undefined || ab === null) {
      for (const id of ['fw-slot', 'fw-record', 'fw-ledger']) missing(id, ab, 'single-image build -- no slots');
      return;
    }

    const slot = Number(ab.slot);
    const active = Number(ab.active_slot);
    const pending = Number(ab.pending_slot);
    const onTrial = truthy(ab.record_valid) && active !== 0 && slot !== active;
    put('fw-slot', `${named(SLOTS, slot)}${onTrial ? ' -- on trial, not yet committed' : ''}`);
    tone('fw-slot', onTrial);

    if (!truthy(ab.record_valid)) {
      put('fw-record', 'no valid boot record -- the fresh-migration state, every field below reads zero');
      tone('fw-record', false);
    } else {
      put(
        'fw-record',
        parts([
          `committed slot ${named(SLOTS, active)}`,
          pending === 0 ? 'no trial open' : `trial slot ${named(SLOTS, pending)}`,
          `sequence ${grouped(ab.sequence)}`,
          Number(ab.trial_budget) > 0 ? `trial budget ${grouped(ab.trial_budget)} boots` : '',
        ]),
      );
      tone('fw-record', pending !== 0);
    }

    const fellBack = truthy(ab.ledger_fell_back);
    const rolledBack = truthy(ab.rollback_observed);
    if (!truthy(ab.ledger_valid)) {
      put('fw-ledger', 'not decoded -- a cold power cycle honestly restarts the trial count');
    } else {
      put(
        'fw-ledger',
        parts([
          `${grouped(ab.ledger_boots)} trial boots launched for this record`,
          fellBack ? 'budget spent: the chooser booted the committed slot' : '',
          rolledBack ? 'this boot persisted a failed trial rollback' : '',
        ]),
      );
    }
    tone('fw-ledger', fellBack || rolledBack);
  }

  /* The previous boot's retained ledger: how far it got, and the first service
   * that refused. A named first-aborting checkpoint is the starvation the
   * watchdog eventually reaped. */
  function renderPrevBoot(status) {
    const prev = status.prev_boot;
    if (missing('prev-boot', prev, 'no record')) return;
    const final = Number(prev.final_checkpoint);
    const abort = Number(prev.first_abort);
    const aborted = abort !== FIRST_ABORT_NONE && abort !== FIRST_ABORT_INVALID;
    const finalText =
      final === PREV_BOOT_LEDGER_INVALID
        ? 'retained ledger invalid (first boot after power-on, or corruption)'
        : listed(CHECKPOINTS, final);
    const abortText = abort === FIRST_ABORT_NONE ? 'none' : abort === FIRST_ABORT_INVALID ? 'latch invalid' : listed(CHECKPOINTS, abort);
    put('prev-boot', `final checkpoint ${finalText} · first aborting checkpoint ${abortText}`);
    tone('prev-boot', aborted);
  }

  /* The core's retained crash record. Deliberately never cleared by the
   * firmware, so it can describe a fault several boots old -- say so, or a
   * reader dates it to the running image. return_address is the addr2line
   * input against the ELF of the build that faulted, which may not be this one. */
  function renderFaultRecord(status) {
    const fault = status.fault_record;
    if (missing('fault-record', fault, 'none recorded -- clean history, or a power cycle cleared it')) return;
    const exception = Number(fault.ipsr) === 0 ? 'thread mode' : `exception ${Number(fault.ipsr)}`;
    put(
      'fault-record',
      parts([
        `return ${fault.return_address}`,
        `CFSR ${fault.cfsr}`,
        `HFSR ${fault.hfsr}`,
        `MMFAR ${fault.mmfar}`,
        `BFAR ${fault.bfar}`,
        exception,
        `${Number(fault.temp_c).toFixed(1)} °C at ${grouped(fault.uptime_s)} s uptime`,
        'never cleared, so it may predate this image',
      ]),
    );
    tone('fault-record', true);
  }

  /* The activation dark window's retained stamp: what the sequencer was doing
   * when the board last went dark to program flash. `completed` is the healthy
   * terminal; `entered` or `step in flight` on a board that came back means it
   * died in there. Cycles are DWT at 600 MHz on this part. */
  function renderOtaDark(status) {
    const dark = status.ota_dark;
    if (missing('ota-dark', dark, 'no stamp retained -- fresh or power-cycled board')) return;
    const state = Number(dark.state);
    const fault = Number(dark.fault);
    const compare = Number(dark.compare_offset);
    put(
      'ota-dark',
      parts([
        named(DARK_STATES, state),
        `phase ${named(DARK_PHASES, Number(dark.phase))}`,
        truthy(dark.rehearsal) ? 'REHEARSAL -- scratch window, the boot region was never touched' : '',
        `${grouped(dark.steps_taken)} of ${grouped(dark.steps_total)} steps`,
        `${grouped(dark.programmed_bytes)} B programmed, ${grouped(dark.verified_bytes)} verified`,
        `image ${dark.image_digest_prefix}`,
        `${(Number(dark.dark_window_cycles) / 600000000).toFixed(2)} s dark`,
        fault !== 0 ? `fault ${fault} (primitive ${Number(dark.primitive_fault)}) at 0x${(Number(dark.step_address) >>> 0).toString(16).toUpperCase()}` : '',
        compare !== NO_COMPARE_FAULT ? `first bad compare at image offset ${grouped(compare)}` : '',
      ]),
    );
    tone('ota-dark', state === 3 || fault !== 0);
  }

  /* The 1 kHz priority-0 stall witness. Every counted event is a real epoch of
   * 100 ms or more; the tick deficit separates CPU theft below priority 0 (the
   * witness kept ticking, so the IABR snapshot and PC name the thief) from an
   * interrupts-masked or bus-level hold (the witness froze, and the deficit is
   * the whole verdict). */
  function renderStallWitness(status) {
    const stall = status.stall_diag;
    if (missing('stall-witness', stall, 'no witness')) return;
    const events = Number(stall.events) || 0;
    if (events === 0) {
      put('stall-witness', `no epoch over 100 ms · ${grouped(stall.witness_ticks)} witness ticks`);
      tone('stall-witness', false);
      return;
    }
    put(
      'stall-witness',
      parts([
        `${grouped(events)} epochs over 100 ms`,
        `last ${formatUs(Number(stall.last_us))}`,
        `tick deficit ${grouped(stall.last_deficit_ms)} ms, worst ${grouped(stall.max_deficit_ms)} ms`,
        `PC 0x${stall.pc}`,
        `LR 0x${stall.lr}`,
        `FlexSPI2 INTR 0x${stall.flexspi2_intr}`,
      ]),
    );
    tone('stall-witness', true);
  }

  function setAllPills(text) {
    for (const id of PILLS) setPill($(id), text);
  }

  function setGridsStale(stale) {
    for (const id of GRIDS) {
      if (stale) $(id).dataset.stale = 'true';
      else delete $(id).dataset.stale;
    }
  }

  function renderUnreachable(result) {
    /* Every grid keeps its last good values, so every grid dims: a frozen
     * "PCM / gate open" beside a live-looking pill is the defect this page
     * was an earlier page's defect here too (UX finding P4). Reached only
     * after three consecutive failures; a 503 never gets here. */
    setAllPills('unreachable');
    setGridsStale(true);
    setCardStatus('poll', `No answer from /api/status -- ${describe(result)}. Every value below is stale.`);
  }

  function renderAnswer(result) {
    const status = result.body;
    renderConfiguration(status);
    renderNetwork(status);
    renderStorage(status);
    renderBoard(status);
    setAllPills('live');
    setGridsStale(false);
    clearCardError('poll');
    setCardStatus('poll', `Snapshot at epoch ${Number(status.loop_epoch) || 0}.`);
  }

  /* The DSP model's generation is not on /api/status (the firmware keeps it off
   * that body on purpose), so this page reads it from GET /api/dsp/config at
   * load and every 10 s: `config_generation`, the same field the DSP editor
   * holds as its concurrency token. A failed read shows `--` and touches
   * nothing else in the pane; a 503 shed keeps the last value. */
  const DSP_GENERATION_POLL_MS = 10000;

  async function readDspGeneration() {
    let result;
    try {
      result = await request('/api/dsp/config', { headers: { Accept: 'application/json' } });
    } catch (error) {
      result = null;
    }
    if (result && result.status === 503) return;
    const generation = result && result.ok && result.body ? Number(result.body.config_generation) : NaN;
    put('dsp-generation', Number.isFinite(generation) ? grouped(generation) : '--');
  }

  async function pollDspGeneration() {
    await readDspGeneration();
    setTimeout(() => void pollDspGeneration(), DSP_GENERATION_POLL_MS);
  }
  void pollDspGeneration();

  shared.createPoller({
    fetch: () => request('/api/status', { headers: { Accept: 'application/json' } }),
    cadenceMs: STATUS_POLL_MS,
    immediate: true,
    onAnswer: renderAnswer,
    onUnreachable: renderUnreachable,
    onError: cardSink('poll'),
  });
})();
