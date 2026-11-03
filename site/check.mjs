#!/usr/bin/env node
// Aius site output check.
//
// Owns: two gates over a built site directory. (1) Link check: every internal
// href/src in every .html file resolves to a file in the output; a missing
// in-page anchor on an internal target is reported as a warning. (2) Forbidden
// literals: no line of the forbidden file (publishing/filter/forbidden.txt
// format: one literal per line, # comments) appears, case-insensitively, in
// any output text file. Exit 1 on a broken link or a forbidden hit.
//
// Usage: node site/check.mjs <out-dir> [--forbidden <file>]

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TEXT_EXT = /\.(html|css|js|mjs|svg|json|txt|xml)$/i;
const SCHEME_RE = /^[a-z][a-z0-9+.-]*:/i;

function walk(dir) {
  return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? walk(p) : [p];
  });
}

/**
 * Read a forbidden-literal file: one literal per line, blank lines and `#` comments ignored.
 * @param {string} file
 * @returns {string[]}
 */
export function readForbidden(file) {
  return fs
    .readFileSync(file, 'utf8')
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith('#'));
}

/**
 * Check a built site.
 * @param {string} out site output directory
 * @param {string[]} forbidden literals to reject
 * @returns {{ files: number, links: number, broken: string[], anchors: string[], hits: string[] }}
 */
export function checkSite(out, forbidden) {
  const files = walk(out);
  const ids = new Map();
  const idsOf = (file) => {
    if (!ids.has(file)) {
      const html = fs.readFileSync(file, 'utf8');
      ids.set(file, new Set([...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1])));
    }
    return ids.get(file);
  };
  const broken = [];
  const anchors = [];
  const hits = [];
  let links = 0;
  const needles = forbidden.map((f) => f.toLowerCase());
  for (const file of files) {
    const rel = path.relative(out, file);
    if (TEXT_EXT.test(file)) {
      const lines = fs.readFileSync(file, 'utf8').split('\n');
      lines.forEach((line, n) => {
        const low = line.toLowerCase();
        for (const needle of needles) if (low.includes(needle)) hits.push(`${rel}:${n + 1}: ${needle}`);
      });
    }
    if (!file.endsWith('.html')) continue;
    const html = fs.readFileSync(file, 'utf8').replace(/<pre class="mermaid">[\s\S]*?<\/pre>/g, '');
    for (const m of html.matchAll(/\b(?:href|src)="([^"]*)"/g)) {
      const url = m[1].replace(/&amp;/g, '&');
      if (url === '' || SCHEME_RE.test(url) || url.startsWith('//')) continue;
      links++;
      const [pathPart, frag] = url.split('#');
      let target = pathPart === '' ? file : path.resolve(path.dirname(file), decodeURIComponent(pathPart.split('?')[0]));
      if (fs.existsSync(target) && fs.statSync(target).isDirectory()) target = path.join(target, 'index.html');
      if (!target.startsWith(path.resolve(out)) || !fs.existsSync(target)) {
        broken.push(`${rel}: ${url}`);
        continue;
      }
      if (frag && target.endsWith('.html') && !idsOf(target).has(decodeURIComponent(frag))) {
        anchors.push(`${rel}: ${url}`);
      }
    }
  }
  return { files: files.length, links, broken, anchors, hits };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const args = process.argv.slice(2);
  const out = args[0];
  const fi = args.indexOf('--forbidden');
  const defaultForbidden = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../publishing/filter/forbidden.txt');
  const forbiddenFile = fi >= 0 ? args[fi + 1] : defaultForbidden;
  if (!out) {
    console.error('usage: node site/check.mjs <out-dir> [--forbidden <file>]');
    process.exit(2);
  }
  const forbidden = fs.existsSync(forbiddenFile) ? readForbidden(forbiddenFile) : [];
  if (!forbidden.length) console.log(`note: no forbidden list at ${forbiddenFile}; literal gate skipped`);
  const r = checkSite(path.resolve(out), forbidden);
  for (const b of r.broken) console.log(`broken: ${b}`);
  for (const a of r.anchors) console.log(`anchor warning: ${a}`);
  for (const h of r.hits) console.log(`forbidden: ${h}`);
  console.log(
    `${r.files} files, ${r.links} internal links, ${r.broken.length} broken, ${r.anchors.length} anchor warnings, ${r.hits.length} forbidden hits`,
  );
  process.exit(r.broken.length || r.hits.length ? 1 : 0);
}
