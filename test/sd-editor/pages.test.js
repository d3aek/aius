// @vitest-environment happy-dom

/* Every SD page under firmware/sd/www shares one chrome (sd-chrome.css, mixer
 * plan Phase 7): its stylesheet links must resolve to files on the card, it
 * must link the shared sheet first, and its header must carry the same four
 * page links in the same order, with exactly its own marked current. A page
 * that links a missing sheet renders unstyled on the board, which no other
 * suite would notice (happy-dom loads no stylesheets). */

import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const here = dirname(fileURLToPath(import.meta.url));
const www = resolve(here, '../../firmware/sd/www');
const PAGES = readdirSync(www).filter((name) => name.endsWith('.html')).sort();
const PAGE_FILES = ['index.html', 'status.html', 'upgrade.html'];
const NAV = ['index.html', 'status.html', 'upgrade.html'];
const NAV_LABELS = ['dsp', 'status', 'firmware'];

/* A <template> parses inertly: happy-dom fetches nothing for its links. */
const parse = (page) => {
  const template = document.createElement('template');
  template.innerHTML = readFileSync(resolve(www, page), 'utf8');
  return template.content;
};
const sheetsOf = (doc) => Array.from(doc.querySelectorAll('link[rel~="stylesheet"]')).map((link) => link.getAttribute('href'));

describe('SD pages share one chrome', () => {
  it('finds the three pages', () => {
    expect(PAGES).toEqual([...PAGE_FILES].sort());
  });

  it('no served page links classic.html', () => {
    for (const page of PAGES) {
      const hrefs = Array.from(parse(page).querySelectorAll('a[href]'), (anchor) => anchor.getAttribute('href'));
      expect(hrefs.filter((href) => /classic\.html/.test(href)), `${page} links classic.html`).toEqual([]);
    }
  });

  it.each(PAGES)('%s links stylesheets that exist on the card, sd-chrome.css first', (page) => {
    const sheets = sheetsOf(parse(page));
    expect(sheets.length).toBeGreaterThan(0);
    expect(sheets[0]).toBe('sd-chrome.css');
    for (const href of sheets) {
      expect(href, `${page} links ${href}`).toMatch(/^[a-z0-9-]+\.css$/);
      expect(existsSync(resolve(www, href)), `${page} links missing ${href}`).toBe(true);
    }
  });

  it.each(PAGES)('%s header.bench-nav carries bench-nav--wrap so phones keep the links', (page) => {
    const header = parse(page).querySelector('header.bench-nav');
    expect(header, `${page} has no header.bench-nav`).not.toBeNull();
    expect(header.classList.contains('bench-nav--wrap'), `${page} header lacks bench-nav--wrap`).toBe(true);
  });

  it.each(['status.html', 'upgrade.html'])('%s has no visible page title, only one hidden h1', (page) => {
    const doc = parse(page);
    expect(doc.textContent).not.toContain('Aius / SD card');
    expect(doc.querySelector('.page-head__title, .page-head__lede')).toBeNull();
    const headings = doc.querySelectorAll('h1');
    expect(headings).toHaveLength(1);
    expect(headings[0].classList.contains('u-visually-hidden')).toBe(true);
    expect(headings[0].textContent.trim().length).toBeGreaterThan(0);
  });

  it('ships no replay page or sheet, and no page links one', () => {
    expect(readdirSync(www).filter((name) => /^replays\./.test(name))).toEqual([]);
    for (const page of PAGES) {
      expect(readFileSync(resolve(www, page), 'utf8'), `${page} mentions replays`).not.toMatch(/replays\.(html|css|js)/);
    }
  });

  it.each(PAGES)('%s carries the same three nav links, its own marked current when linked', (page) => {
    const doc = parse(page);
    const links = Array.from(doc.querySelectorAll('header.bench-nav a:not(.logo)'));
    expect(links.map((anchor) => anchor.getAttribute('href'))).toEqual(NAV);
    expect(links.map((anchor) => anchor.textContent.trim())).toEqual(NAV_LABELS);
    const current = links.filter((anchor) => anchor.getAttribute('aria-current') === 'page');
    expect(current.map((anchor) => anchor.getAttribute('href'))).toEqual(NAV.includes(page) ? [page] : []);
  });
});

/* happy-dom loads no stylesheets, so the action bar's stick-to-top rule is pinned on the CSS
   text; the headless render (plan Phase 4xa) is the behavioural proof. */
describe('the DSP action bar sticks to the top at every width', () => {
  const mixer = readFileSync(resolve(www, 'mixer.css'), 'utf8').replace(/\/\*[\s\S]*?\*\//g, '');
  const rules = (selector) => Array.from(mixer.matchAll(/([^{}]+)\{([^{}]*)\}/g))
    .filter((match) => match[1].split(',').some((part) => part.trim() === selector))
    .map((match) => match[2]);

  it('declares sticky, top 0, an opaque background and a z-index', () => {
    const body = rules('.bench .action-bar').join(';');
    expect(body).toMatch(/position:\s*sticky/);
    expect(body).toMatch(/top:\s*0\b/);
    expect(body).toMatch(/z-index:\s*\d+/);
    expect(body).toMatch(/background:\s*var\(--aius-surface-page\)/);
  });

  it('never turns the bar static or glass again at a narrower width', () => {
    const body = rules('.bench .action-bar').join(';');
    expect(body).not.toMatch(/position:\s*(static|relative)/);
    expect(body).not.toMatch(/backdrop-filter/);
  });
});
