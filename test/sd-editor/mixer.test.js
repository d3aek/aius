// @vitest-environment happy-dom

/* Regression for the bench skin (firmware/sd/www/index.html + mixer.js). The
 * skin runs the UNCHANGED dsp-editor.js against its own markup and adds views
 * over the editor's form, so the harness is dsp-editor.test.js's: the real
 * page body, the real sd-shared.js, dsp-editor.js and mixer.js evaluated in
 * order, and `fetch` stubbed with the device contract. The proofs are the two
 * seams the skin adds: the markup still carries every id the editor binds, and
 * the faders, readouts, chips and flow figure read and write the editor's own
 * fields rather than a model of their own.
 */

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { beforeEach, describe, expect, it, vi } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const www = resolve(here, '../../firmware/sd/www');
const fixture = (name) => JSON.parse(readFileSync(resolve(here, 'fixtures', name), 'utf8'));
const read = (name) => readFileSync(resolve(www, name), 'utf8');
const bodyOf = (html) =>
  html.slice(html.indexOf('<body') , html.indexOf('</body>')).replace(/^<body[^>]*>/, '').replace(/<script[\s\S]*?<\/script>/g, '');
const idsOf = (html) => new Set(Array.from(bodyOf(html).matchAll(/\sid="([^"]+)"/g), (match) => match[1]));

const classicHtml = read('classic.html');
const mixerHtml = read('index.html');
const sources = [read('sd-shared.js'), read('dsp-editor.js'), read('mixer.js')];

const el = (id) => document.getElementById(id);
const fieldOf = (path) => el(`f:${path}`);
/* Microtasks, then one zero-delay timer turn (mixer.js syncs and mirrors
 * bonded pairs on one, after the editor's own handlers), then microtasks. */
const settle = async (rounds = 30) => {
  for (let index = 0; index < rounds; index++) await Promise.resolve();
  await new Promise((resolve) => setTimeout(resolve, 0));
  await new Promise((resolve) => setTimeout(resolve, 0));
  for (let index = 0; index < rounds; index++) await Promise.resolve();
};

const jsonResponse = (status, body) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => body,
  text: async () => JSON.stringify(body),
});
const htmlResponse = (status, html) => ({
  ok: status >= 200 && status < 300,
  status,
  json: async () => {
    throw new SyntaxError('not JSON');
  },
  text: async () => html,
});

const PRESETS_INDEX =
  '<!DOCTYPE html><html><body><h1>/sd/www/presets/</h1><ul><li><a href="../">../</a></li>' +
  '<li><a href="flat.json">flat.json</a> 691</li><li><a href="night.json">night.json</a> 700</li></ul>' +
  '<p>2 shown, 0 hidden, truncated: no.</p></body></html>';
const STATUS_LIVE = { carrier: 1, tx_gate: { open: 1, carrier_locked: 1 } };

function stubDevice(config, presetFiles = {}) {
  globalThis.fetch = vi.fn(async (url, init = {}) => {
    const method = init.method || 'GET';
    if (url === '/api/dsp/config' && method === 'GET') return jsonResponse(200, config);
    if (url === '/api/meters') return jsonResponse(200, fixture('meters-snapshot.json'));
    if (url === '/api/status/brief') return jsonResponse(200, STATUS_LIVE);
    if (url === '/sd/www/presets/') return htmlResponse(200, PRESETS_INDEX);
    if (url.startsWith('/sd/www/presets/')) {
      const name = decodeURIComponent(url.slice('/sd/www/presets/'.length)).replace(/\.json$/, '');
      return name in presetFiles ? jsonResponse(200, presetFiles[name]) : jsonResponse(404, { ok: false, error: 'sd_not_found' });
    }
    return jsonResponse(404, { ok: false, error: 'not_found' });
  });
}

/* A canvas that draws nothing: the plot's maths is dsp-editor.test.js's job. */
function installCanvas() {
  window.HTMLCanvasElement.prototype.getContext = () =>
    new Proxy({}, { get: (target, key) => (key in target ? target[key] : () => {}), set: (target, key, value) => ((target[key] = value), true) });
}

/* The document is rebuilt per test but `window` and `document` are not, so a
 * previous boot's listeners and intervals would answer for a page that no
 * longer exists. Each boot drops the last one's. */
const windowListeners = [];
const documentListeners = [];
const intervals = [];
const addWindowListener = window.addEventListener.bind(window);
const addDocumentListener = document.addEventListener.bind(document);
const nativeSetInterval = window.setInterval.bind(window);

async function boot(config = fixture('dsp-config-default.json'), presetFiles = {}) {
  for (const [type, handler] of windowListeners.splice(0)) window.removeEventListener(type, handler);
  for (const [type, handler, options] of documentListeners.splice(0)) document.removeEventListener(type, handler, options);
  for (const id of intervals.splice(0)) clearInterval(id);
  window.addEventListener = (type, handler, options) => {
    windowListeners.push([type, handler]);
    return addWindowListener(type, handler, options);
  };
  document.addEventListener = (type, handler, options) => {
    documentListeners.push([type, handler, options]);
    return addDocumentListener(type, handler, options);
  };
  window.setInterval = (handler, ms) => {
    const id = nativeSetInterval(handler, ms);
    intervals.push(id);
    return id;
  };
  stubDevice(config, presetFiles);
  document.body.innerHTML = bodyOf(mixerHtml);
  installCanvas();
  try {
    for (const source of sources) new Function(source)();
  } finally {
    window.addEventListener = addWindowListener;
    document.addEventListener = addDocumentListener;
    window.setInterval = nativeSetInterval;
  }
  await settle();
}

const fader = (key) => el(`fader:${key}`);
const faderText = (key) => fader(key).closest('.fader').querySelector('.fader__val').textContent;

async function slide(key, position) {
  fader(key).value = String(position);
  fader(key).dispatchEvent(new Event('input'));
  await settle();
}

async function type(path, raw) {
  fieldOf(path).value = raw;
  fieldOf(path).dispatchEvent(new Event('input'));
  await settle();
}

describe('index.html (mixer) markup', () => {
  it('carries every id classic.html carries, so dsp-editor.js binds unchanged', () => {
    const missing = [...idsOf(classicHtml)].filter((id) => !idsOf(mixerHtml).has(id));
    expect(missing).toEqual([]);
  });
});

describe('mixer faders', () => {
  beforeEach(async () => {
    await boot();
  });

  it('show the device values once the config is read', () => {
    expect(faderText('c')).toBe('0.71');
    expect(faderText('s')).toBe('0.71');
    expect(faderText('lfe')).toBe('0.32');
    expect(faderText('f')).toBe('1.00');
    expect(fader('f').disabled).toBe(true);
    /* Crossover, delay and headroom are exact values, not faders. */
    expect(Array.from(el('mixer-faders').querySelectorAll('input'), (range) => range.id)).toEqual([
      'fader:c',
      'fader:f',
      'fader:s',
      'fader:lfe',
    ]);
    expect(el('mixer-exact').contains(fieldOf('matrix.lfe_crossover_hz'))).toBe(true);
  });

  it('write the editor field and stage an unapplied edit', async () => {
    await slide('c', 1000);
    expect(fieldOf('matrix.center_gain_linear').value).toBe('4');
    expect(faderText('c')).toBe('4.00');
    expect(el('dirty-state').hidden).toBe(false);
    expect(el('mixer-eqn').textContent).toContain('4.00·C');
  });

  it('turn the bottom stop into an explicit off, which the field accepts', async () => {
    await slide('lfe', 0);
    expect(fieldOf('matrix.lfe_gain_linear').value).toBe('0');
    expect(el('e:matrix.lfe_gain_linear').textContent).toBe('');
  });

  it('follow a value typed into the exact field', async () => {
    await type('matrix.lfe_gain_linear', '1');
    expect(faderText('lfe')).toBe('1.00');
    expect(Number(fader('lfe').value)).toBe(Math.round((40 / (40 + 20 * Math.log10(4))) * 1000));
  });

  it('follow a field rewritten with no event, as a device read does', async () => {
    fieldOf('matrix.surround_gain_linear').value = '0.25';
    await new Promise((resolve) => setTimeout(resolve, 250));
    expect(faderText('s')).toBe('0.25');
  });

  it('show the headroom the clip-free policy resolves', async () => {
    el('matrix-clipfree').click();
    await settle();
    expect(fieldOf('matrix.clipfree').checked).toBe(true);
    expect(fieldOf('matrix.headroom_linear').disabled).toBe(true);
    /* 1 / (1 + 0.707 + 0.707 + 0.316) = 0.366 */
    expect(el('mixer-eqn').textContent).toContain('0.37·HR');
    expect(el('mixer-verdict').dataset.tone).toBe('ok');
  });

  it('open the exact values when a matrix field goes out of range', async () => {
    expect(el('mixer-exact').open).toBe(false);
    await type('matrix.center_gain_linear', '9');
    expect(el('mixer-exact').open).toBe(true);
    expect(fader('c').closest('.fader').dataset.invalid).toBe('true');
  });
});

describe('mixer readouts', () => {
  it('state the worst-case overshoot and whether the limiter catches it', async () => {
    await boot();
    /* 0.5 x (1 + 0.707 + 0.707 + 0.316) = 1.365, +2.7 dB, limiter on */
    expect(el('mixer-verdict').dataset.tone).toBe('warn');
    expect(el('mixer-verdict').textContent).toContain('+2.7 dB');
    fieldOf('limiter.enabled').checked = false;
    fieldOf('limiter.enabled').dispatchEvent(new Event('input'));
    await settle();
    expect(el('mixer-verdict').dataset.tone).toBe('bad');
    await type('matrix.headroom_linear', '0.2');
    expect(el('mixer-verdict').dataset.tone).toBe('ok');
  });

  it('toggle layout-aware headroom through its chip', async () => {
    await boot();
    expect(el('mixer-layout-aware').getAttribute('aria-pressed')).toBe('true');
    el('mixer-layout-aware').click();
    await settle();
    expect(fieldOf('matrix.layout_aware').checked).toBe(false);
    expect(el('mixer-layout-aware').getAttribute('aria-pressed')).toBe('false');
  });
});

describe('mixer preset chips', () => {
  it('list the card presets and load one into the form', async () => {
    const night = fixture('dsp-config-default.json');
    const preset = {
      features: night.features,
      input_channels: night.input_channels,
      output_eq: night.output_eq,
      matrix: { ...night.matrix, center_gain_linear: 1.5 },
      limiter: night.limiter,
    };
    await boot(fixture('dsp-config-default.json'), { night: preset });
    const chips = Array.from(el('mixer-presets').querySelectorAll('[data-preset]'), (chip) => chip.dataset.preset);
    expect(chips.sort()).toEqual(['flat', 'night']);
    el('mixer-presets').querySelector('[data-preset="night"]').click();
    await settle(60);
    expect(faderText('c')).toBe('1.50');
    expect(el('mixer-presets').querySelector('[data-preset="night"]').getAttribute('aria-pressed')).toBe('true');
    await slide('c', 500);
    expect(el('mixer-presets').querySelector('[data-preset="night"]').getAttribute('aria-pressed')).toBe('false');
  });
});

describe('EQ channel tabs', () => {
  const CHANNELS = ['fl', 'fr', 'fc', 'lfe', 'sl', 'sr'];
  const tab = (key) => el(`eq-tab:${key}`);
  const selected = () => el('eq-tablist').querySelector('[aria-selected="true"]').id.slice('eq-tab:'.length);
  const shown = (channel) => !el('card-input-channels').hidden && !el(`eq-panel:${channel}`).hidden && !el(`eq-panel:${channel}`).parentElement.hidden;
  const shownChannels = () => CHANNELS.filter(shown);

  async function choose(path, value) {
    fieldOf(path).value = value;
    fieldOf(path).dispatchEvent(new Event('change'));
    await settle();
  }

  it('group the channels one tab per curve, pairs bonded and showing the left member', async () => {
    await boot();
    expect(Array.from(el('eq-tablist').children, (button) => button.textContent)).toEqual(['FL+FR', 'C', 'LFE', 'SL+SR', 'OUT']);
    for (const key of CHANNELS) {
      const panel = el(`eq-panel:${key}`);
      expect(panel.contains(fieldOf(`input_channels.${key}.gain_db`))).toBe(true);
      for (const slot of [0, 1]) {
        for (const part of ['type', 'frequency_hz', 'q', 'gain_db']) {
          expect(panel.contains(fieldOf(`input_channels.${key}.eq.${slot}.${part}`))).toBe(true);
        }
      }
    }
    expect(el('input-channels').querySelector('.eq-table--channels')).toBeNull();
    expect(selected()).toBe('front');
    expect(shownChannels()).toEqual(['fl']);
  });

  it('select the matching curve from a tab, and a tab from the curve', async () => {
    await boot();
    tab('centre').click();
    await settle();
    expect(shownChannels()).toEqual(['fc']);
    expect(el('chip:centre').getAttribute('aria-pressed')).toBe('true');
    tab('output').click();
    await settle();
    expect(shownChannels()).toEqual([]);
    expect(el('card-output-eq').hidden).toBe(false);
    expect(el('chip:output').getAttribute('aria-pressed')).toBe('true');
    el('chip:surround').click();
    await settle();
    expect(selected()).toBe('surround');
    expect(shownChannels()).toEqual(['sl']);
  });

  it('copy every edit of a linked pair to its right member, resets included', async () => {
    await boot();
    await choose('input_channels.fl.eq.0.type', 'peaking');
    await type('input_channels.fl.eq.0.gain_db', '3');
    await type('input_channels.fl.gain_db', '-2');
    expect(fieldOf('input_channels.fr.eq.0.type').value).toBe('peaking');
    expect(fieldOf('input_channels.fr.eq.0.gain_db').value).toBe('3');
    expect(fieldOf('input_channels.fr.eq.0.frequency_hz').value).toBe(fieldOf('input_channels.fl.eq.0.frequency_hz').value);
    expect(fieldOf('input_channels.fr.gain_db').value).toBe('-2');
    el('sr:input_channels.fl.eq.0').click();
    await settle();
    expect(fieldOf('input_channels.fr.eq.0.type').value).toBe(fieldOf('input_channels.fl.eq.0.type').value);
    expect(tab('front').closest('[role="tablist"]')).not.toBeNull();
    expect(el('eq-group:front').dataset.linked).toBe('true');
  });

  it('show a pair that differs on the device unlinked, and copy left to right on linking', async () => {
    const config = fixture('dsp-config-default.json');
    config.input_channels.fr.gain_db = -3;
    await boot(config);
    expect(el('eq-group:front').dataset.linked).toBe('false');
    expect(shownChannels()).toEqual(['fl', 'fr']);
    expect(el('eq-group:front').querySelector('.tabs__bond-note').textContent).toContain('differ');
    el('eq-group:front').querySelector('.chip--bond').click();
    await settle();
    expect(fieldOf('input_channels.fr.gain_db').value).toBe(fieldOf('input_channels.fl.gain_db').value);
    expect(el('eq-group:front').dataset.linked).toBe('true');
    expect(shownChannels()).toEqual(['fl']);
  });

  it('leave the right member alone once unlinked', async () => {
    await boot();
    el('eq-group:surround').querySelector('.chip--bond').click();
    await settle();
    await type('input_channels.sl.gain_db', '-6');
    expect(fieldOf('input_channels.sr.gain_db').value).not.toBe('-6');
    expect(el('eq-group:surround').dataset.linked).toBe('false');
  });

  it('move between tabs with the arrow keys', async () => {
    await boot();
    tab('front').dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true }));
    await settle();
    expect(selected()).toBe('output');
  });

  it('mark a closed tab that holds sections or a bad value, and open it from the error link', async () => {
    await boot();
    await choose('input_channels.lfe.eq.0.type', 'peaking');
    expect(tab('lfe').dataset.eq).toBe('true');
    await type('input_channels.lfe.eq.0.gain_db', '99');
    expect(tab('lfe').dataset.invalid).toBe('true');
    el('token').value = 'abcdef0123456789';
    el('token').dispatchEvent(new Event('input'));
    el('apply').click();
    await settle();
    const link = el('errors').querySelector('a[href="#f:input_channels.lfe.eq.0.gain_db"]');
    expect(link).not.toBeNull();
    link.click();
    await settle();
    expect(selected()).toBe('lfe');
  });
});

describe('touch steppers and the matrix strip', () => {
  const STRIP_PATHS = { xo: 'matrix.lfe_crossover_hz', hr: 'matrix.headroom_linear', dly: 'matrix.surround_delay_ms' };
  const stripInput = (key) => el(`strip:${key}`);
  const buttonOf = (input, which) => input.parentElement.querySelector(`.stepper__btn--${which}`);
  const down = (button) => button.dispatchEvent(new Event('pointerdown'));
  const up = (button) => button.dispatchEvent(new Event('pointerup'));

  beforeEach(async () => {
    await boot();
  });

  it('decorate every number input once, with labelled buttons out of the tab order', () => {
    const inputs = Array.from(document.querySelectorAll('input[type="number"]')).filter((input) => !input.closest('.bench-legacy'));
    expect(inputs.length).toBeGreaterThan(30);
    for (const input of inputs) {
      expect(input.parentElement.querySelectorAll('.stepper__btn')).toHaveLength(2);
      expect(input.inputMode).toBe('decimal');
      for (const button of input.parentElement.querySelectorAll('.stepper__btn')) {
        expect(button.type).toBe('button');
        expect(button.tabIndex).toBe(-1);
        expect(button.getAttribute('aria-label')).toMatch(/^(Increase|Decrease) \S/);
      }
    }
    /* The editor still finds its error line through the input's parent. */
    expect(fieldOf('matrix.lfe_crossover_hz').parentElement.querySelector('.field__error')).not.toBeNull();
  });

  it('strip writes each matrix field through the editor and stages an edit', async () => {
    for (const [key, path] of Object.entries(STRIP_PATHS)) {
      const input = stripInput(key);
      expect(input.value).toBe(fieldOf(path).value);
      const step = Number(input.step);
      const next = String(Number((Number(input.value) + step).toFixed(3)));
      input.value = next;
      input.dispatchEvent(new Event('input'));
      await settle();
      expect(fieldOf(path).value).toBe(next);
      expect(fieldOf(path).dataset.exact).toBe('');
    }
    expect(el('dirty-state').hidden).toBe(false);
    expect(el('mixer-eqn').textContent).toContain(`low-passed at ${fieldOf(STRIP_PATHS.xo).value} Hz`);
  });

  it('strip follows a field rewritten with no event', async () => {
    fieldOf(STRIP_PATHS.dly).value = '7.5';
    await new Promise((resolve) => setTimeout(resolve, 250));
    expect(stripInput('dly').value).toBe('7.5');
  });

  it('a press steps by the field step and writes the editor field', async () => {
    const input = stripInput('xo');
    const before = Number(fieldOf(STRIP_PATHS.xo).value);
    const step = Number(input.step);
    down(buttonOf(input, 'plus'));
    up(buttonOf(input, 'plus'));
    await settle();
    expect(Number(fieldOf(STRIP_PATHS.xo).value)).toBe(before + step);
    expect(input.value).toBe(fieldOf(STRIP_PATHS.xo).value);
    down(buttonOf(input, 'minus'));
    up(buttonOf(input, 'minus'));
    await settle();
    expect(Number(fieldOf(STRIP_PATHS.xo).value)).toBe(before);
  });

  it('a step clamps at max and at min', async () => {
    const trim = fieldOf('input_channels.fc.gain_db');
    trim.value = trim.max;
    const plus = buttonOf(trim, 'plus');
    down(plus);
    up(plus);
    expect(trim.value).toBe(trim.max);
    trim.value = String(Number(trim.min) + 0.25);
    const minus = buttonOf(trim, 'minus');
    down(minus);
    up(minus);
    expect(trim.value).toBe(trim.min);
  });

  it('a hold repeats after the delay, firing input per step and one change on release', () => {
    vi.useFakeTimers();
    try {
      const target = fieldOf('input_channels.fc.gain_db');
      target.value = '-30';
      const events = { input: 0, change: 0 };
      target.addEventListener('input', () => (events.input += 1));
      target.addEventListener('change', () => (events.change += 1));
      const minus = buttonOf(target, 'minus');
      down(minus);
      expect(events.input).toBe(1);
      vi.advanceTimersByTime(399);
      expect(events.input).toBe(1);
      vi.advanceTimersByTime(1 + 80 * 4);
      expect(events.input).toBe(5);
      expect(events.change).toBe(0);
      up(minus);
      expect(events.change).toBe(1);
      vi.advanceTimersByTime(1000);
      expect(events.input).toBe(5);
      expect(Number(target.value)).toBe(-30 - 5 * Number(target.step));
    } finally {
      vi.useRealTimers();
    }
  });

  it('mirrors a stepper press on a linked EQ pair', async () => {
    fieldOf('input_channels.fl.eq.0.type').value = 'peaking';
    fieldOf('input_channels.fl.eq.0.type').dispatchEvent(new Event('change'));
    await settle();
    const gain = fieldOf('input_channels.fl.eq.0.gain_db');
    gain.value = '1';
    down(buttonOf(gain, 'plus'));
    up(buttonOf(gain, 'plus'));
    await settle();
    expect(gain.value).toBe(String(1 + Number(gain.step)));
    expect(fieldOf('input_channels.fr.eq.0.gain_db').value).toBe(gain.value);
  });

  it('disables the headroom stepper under clip-free and shows the resolved value', async () => {
    el('matrix-clipfree').click();
    await settle();
    const input = stripInput('hr');
    expect(input.disabled).toBe(true);
    expect(buttonOf(input, 'plus').disabled).toBe(true);
    expect(buttonOf(input, 'minus').disabled).toBe(true);
    expect(input.value).toBe(fieldOf(STRIP_PATHS.hr).value);
    expect(Number(input.value)).toBeCloseTo(0.366, 2);
    const held = fieldOf(STRIP_PATHS.hr).value;
    down(buttonOf(input, 'plus'));
    up(buttonOf(input, 'plus'));
    expect(fieldOf(STRIP_PATHS.hr).value).toBe(held);
    el('matrix-clipfree').click();
    await settle();
    expect(input.disabled).toBe(false);
    expect(buttonOf(input, 'plus').disabled).toBe(false);
  });
});

describe('token dialog', () => {
  it('opens from the lock chip and closes once the token is saved', async () => {
    await boot();
    expect(el('token-dialog').open).toBe(false);
    el('token-open').click();
    expect(el('token-dialog').open).toBe(true);
    el('token').value = 'abcdef0123456789';
    el('token').dispatchEvent(new Event('input'));
    el('token-toggle').click();
    await settle();
    expect(el('token').readOnly).toBe(true);
    expect(el('token-dialog').open).toBe(false);
    expect(el('apply').disabled).toBe(false);
  });
});

describe('signal-flow figure', () => {
  it('scales wires by the matrix weights and lights the enabled stages', async () => {
    await boot();
    const wires = el('flow-wires-in').querySelectorAll('path');
    expect(wires).toHaveLength(6);
    const centre = el('flow-wires-in').querySelector('path.is-c');
    const before = Number(centre.getAttribute('stroke-width'));
    await slide('c', 0);
    expect(centre.dataset.muted).toBe('true');
    await slide('c', 1000);
    expect(Number(centre.getAttribute('stroke-width'))).toBeGreaterThan(before);
    const stages = Array.from(el('flow-stages').children, (stage) => stage.dataset.on);
    expect(stages).toEqual(['true', 'true', 'true', 'true']);
  });

  it('mirrors the editor needles and the transmit gate', async () => {
    await boot();
    /* Whatever the editor's ticker last painted, the box shows the same
     * fraction of its 52 px meter, in the same zone. */
    const fill = el('bar-out-l-fill');
    const meter = el('flow-outputs').querySelector('.flow__meter');
    expect(parseFloat(fill.style.width)).toBeGreaterThan(0);
    expect(Number(meter.getAttribute('width'))).toBeCloseTo((parseFloat(fill.style.width) / 100) * 52, 0);
    expect(meter.dataset.zone).toBe(fill.dataset.zone);
    expect(el('tx-gate').dataset.state).toBe('open');
    expect(el('dashboard-input').querySelector('.flow').dataset.live).toBe('true');
  });
});
