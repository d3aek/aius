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
const NAV = ['index.html', 'status.html', 'upgrade.html', 'classic.html'];

/* A <template> parses inertly: happy-dom fetches nothing for its links. */
const parse = (page) => {
  const template = document.createElement('template');
  template.innerHTML = readFileSync(resolve(www, page), 'utf8');
  return template.content;
};
const sheetsOf = (doc) => Array.from(doc.querySelectorAll('link[rel~="stylesheet"]')).map((link) => link.getAttribute('href'));

describe('SD pages share one chrome', () => {
  it('finds the four pages', () => {
    expect(PAGES).toEqual([...NAV].sort());
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

  it('ships no replay page or sheet, and no page links one', () => {
    expect(readdirSync(www).filter((name) => /^replays\./.test(name))).toEqual([]);
    for (const page of PAGES) {
      expect(readFileSync(resolve(www, page), 'utf8'), `${page} mentions replays`).not.toMatch(/replays\.(html|css|js)/);
    }
  });

  it('carries the Live processing card on status.html', () => {
    const doc = parse('status.html');
    expect(doc.querySelector('canvas#waveform')).not.toBeNull();
    expect(doc.querySelector('#meter-state')).not.toBeNull();
    expect(doc.querySelector('#meter-summary')).not.toBeNull();
  });

  it.each(PAGES)('%s carries the same four nav links, its own marked current', (page) => {
    const doc = parse(page);
    const links = Array.from(doc.querySelectorAll('header.bench-nav a:not(.logo)'));
    expect(links.map((anchor) => anchor.getAttribute('href'))).toEqual(NAV);
    const current = links.filter((anchor) => anchor.getAttribute('aria-current') === 'page');
    expect(current.map((anchor) => anchor.getAttribute('href'))).toEqual([page]);
  });
});
