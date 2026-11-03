#!/usr/bin/env node
// Aius GitHub Pages site build.
//
// Owns: rendering the docs listed in site/docs.json into the site shell, the
// docs index page, and assembling the output directory (marketing page,
// assets, rendered docs, images the docs reference). Does not own: the
// forbidden-literal gate or the link check (site/check.mjs), publishing.
//
// Usage: node site/build.mjs --out <dir> [--root <tree>] [--manifest <file>]
//   --root      tree whose docs are rendered (default: this script's repo);
//               Phase 4 points it at the anonymised mirror tree.
//   --manifest  docs manifest (default: <this script's dir>/docs.json).
// Node 18+ only; no npm dependency, so it runs on a bare CI checkout.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { escapeHtml, renderMarkdown, splitFrontMatter } from './lib/markdown.mjs';
import { createLinkRewriter, relativeHref } from './lib/links.mjs';
import { buildToc } from './lib/toc.mjs';

const SITE_DIR = path.dirname(fileURLToPath(import.meta.url));
const SHELL_FILES = ['index.html', 'assets'];
const MERMAID_URL = 'https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.esm.min.mjs';
const LFS_POINTER = 'version https://git-lfs.github.com/spec/';
const GITHUB_ICON =
  '<svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true"><path fill="currentColor" d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"/></svg>';
const LOGO_DOTS = '<span class="logo__dots" aria-hidden="true"><i></i><i></i><i></i><i></i><i></i><i></i></span>';

function parseArgs(argv) {
  const opts = { root: path.resolve(SITE_DIR, '..'), manifest: path.join(SITE_DIR, 'docs.json'), out: null };
  for (let i = 0; i < argv.length; i++) {
    const key = argv[i].replace(/^--/, '');
    if (!(key in opts) || argv[i + 1] === undefined) throw new Error(`unknown or incomplete argument: ${argv[i]}`);
    opts[key] = path.resolve(argv[++i]);
  }
  if (!opts.out) throw new Error('usage: node site/build.mjs --out <dir> [--root <tree>] [--manifest <file>]');
  return opts;
}

/**
 * Output page path for a manifest entry: its `page`, else the tree path with `.md` as `.html`.
 * @param {{ path: string, page?: string }} doc
 * @returns {string}
 */
export function pageFor(doc) {
  return doc.page ?? doc.path.replace(/\.md$/i, '.html');
}

function shell({ rel, title, description, nav, body, mermaid, repoUrl, toc = { rail: '', details: '' } }) {
  const mermaidScript = mermaid
    ? `\n<script type="module">\n  import mermaid from "${MERMAID_URL}";\n  mermaid.initialize({ startOnLoad: true, theme: "dark", securityLevel: "strict" });\n</script>`
    : '';
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${escapeHtml(title)} · aius</title>
  <meta name="description" content="${escapeHtml(description)}">
  <meta name="theme-color" content="#0a0c0b">
  <link rel="icon" href="${rel}assets/favicon.svg" type="image/svg+xml">
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Archivo:wdth,wght@62..125,400..900&family=IBM+Plex+Mono:wght@400;500;600&display=swap" rel="stylesheet">
  <link rel="stylesheet" href="${rel}assets/style.css">
</head>
<body>

<a class="skip" href="#main">Skip to content</a>

<header class="nav">
  <div class="wrap nav__inner">
    <a class="logo" href="${rel}index.html" aria-label="aius home">
      ${LOGO_DOTS}
      <span class="logo__word">aius</span>
    </a>
    <nav class="nav__links" aria-label="Primary">
      <a href="${rel}index.html">Home</a>
      <a href="${rel}index.html#specs">Specs</a>
      <a href="${rel}docs/index.html">Docs</a>
    </nav>
    <a class="btn btn--ghost btn--sm" href="${repoUrl}">
      ${GITHUB_ICON}
      <span>Source</span>
    </a>
  </div>
</header>

<main id="main" class="docpage">
  <div class="wrap docpage__grid${toc.rail ? ' docpage__grid--toc' : ''}">
${nav}
    <article class="prose">
${toc.details ? toc.details + '\n' : ''}${body}
    </article>
${toc.rail ? toc.rail + '\n' : ''}  </div>
</main>

<footer class="foot">
  <div class="wrap foot__inner">
    <span class="logo logo--sm">${LOGO_DOTS}<span class="logo__word">aius</span></span>
    <p class="mono">Open hardware audio · working name · not affiliated with PJRC or Dolby.</p>
  </div>
</footer>${mermaidScript}
</body>
</html>
`;
}

function docNav(groups, currentPage) {
  const parts = groups.map((g) => {
    const items = g.docs
      .map((d) => {
        const page = pageFor(d);
        const current = page === currentPage ? ' aria-current="page"' : '';
        return `        <li><a class="docnav__link" href="${relativeHref(currentPage, page)}"${current}>${escapeHtml(d.title)}</a></li>`;
      })
      .join('\n');
    return `      <p class="docnav__group mono">${escapeHtml(g.title)}</p>\n      <ul class="docnav__list">\n${items}\n      </ul>`;
  });
  return `    <nav class="docnav" aria-label="Documentation">\n      <a class="docnav__home mono" href="${relativeHref(currentPage, 'docs/index.html')}">All docs</a>\n${parts.join('\n')}\n    </nav>`;
}

function copyRecursive(src, dest) {
  fs.mkdirSync(path.dirname(dest), { recursive: true });
  fs.cpSync(src, dest, { recursive: true });
}

/**
 * Build the site.
 * @param {{ root: string, manifest: string, out: string, log?: (msg: string) => void }} opts
 * @returns {{ pages: string[], assets: string[], skipped: string[], warnings: string[] }}
 */
export function buildSite({ root, manifest, out, log = console.log }) {
  const spec = JSON.parse(fs.readFileSync(manifest, 'utf8'));
  const warnings = [];
  const skipped = [];
  const groups = spec.groups
    .map((g) => ({
      title: g.title,
      docs: g.docs.filter((d) => {
        if (fs.existsSync(path.join(root, d.path))) return true;
        if (!d.optional) throw new Error(`manifest doc missing from tree: ${d.path}`);
        skipped.push(d.path);
        return false;
      }),
    }))
    .filter((g) => g.docs.length > 0);
  const docs = groups.flatMap((g) => g.docs);
  const pages = new Map(docs.map((d) => [d.path, pageFor(d)]));
  const stat = (p) => fs.statSync(path.join(root, p), { throwIfNoEntry: false });
  const rewrite = createLinkRewriter({
    repoUrl: spec.repoUrl,
    branch: spec.branch,
    pages,
    exists: (p) => stat(p) !== undefined,
    isDir: (p) => stat(p)?.isDirectory() ?? false,
  });
  const assets = new Set();

  fs.mkdirSync(out, { recursive: true });
  for (const name of SHELL_FILES) copyRecursive(path.join(SITE_DIR, name), path.join(out, name));
  fs.writeFileSync(path.join(out, '.nojekyll'), '');

  for (const doc of docs) {
    const page = pages.get(doc.path);
    const rel = relativeHref(page, 'index.html').replace(/index\.html$/, '');
    const { meta, body } = splitFrontMatter(fs.readFileSync(path.join(root, doc.path), 'utf8'));
    const rendered = renderMarkdown(body, { rewriteUrl: (u, kind) => rewrite(u, doc.path, kind, assets) });
    const h1 = rendered.headings.find((h) => h.level === 1);
    const title = meta.title || h1?.text || doc.title;
    const lead = h1 || !meta.title ? '' : `<h1>${escapeHtml(meta.title)}</h1>\n`;
    const html = shell({
      rel,
      title,
      description: `${title}: Aius documentation.`,
      nav: docNav(groups, page),
      body: lead + rendered.html,
      toc: buildToc(rendered.headings),
      mermaid: rendered.hasMermaid,
      repoUrl: spec.repoUrl,
    });
    fs.mkdirSync(path.dirname(path.join(out, page)), { recursive: true });
    fs.writeFileSync(path.join(out, page), html);
  }

  const indexPage = 'docs/index.html';
  const indexBody = [
    '<h1>Documentation</h1>',
    `<p>The Aius docs rendered from the repository. Everything else in the tree is on <a href="${spec.repoUrl}">GitHub</a>.</p>`,
    ...groups.map(
      (g) =>
        `<h2>${escapeHtml(g.title)}</h2>\n<ul>\n${g.docs
          .map((d) => `<li><a href="${relativeHref(indexPage, pageFor(d))}">${escapeHtml(d.title)}</a></li>`)
          .join('\n')}\n</ul>`,
    ),
  ].join('\n');
  fs.writeFileSync(
    path.join(out, indexPage),
    shell({
      rel: '../',
      title: 'Documentation',
      description: 'Aius documentation index.',
      nav: docNav(groups, indexPage),
      body: indexBody,
      mermaid: false,
      repoUrl: spec.repoUrl,
    }),
  );

  // Images the landing page references from the docs tree (src="docs/...") ship from there, never duplicated under site/.
  const landing = fs.readFileSync(path.join(SITE_DIR, 'index.html'), 'utf8');
  for (const m of landing.matchAll(/<img\b[^>]*\ssrc="(docs\/[^"#?]+)"/g)) {
    if (!stat(m[1])) throw new Error(`index.html references a missing image: ${m[1]}`);
    assets.add(m[1]);
  }

  for (const asset of assets) {
    const src = path.join(root, asset);
    const head = fs.readFileSync(src).subarray(0, LFS_POINTER.length).toString('utf8');
    if (head === LFS_POINTER) warnings.push(`asset is an unsmudged LFS pointer: ${asset}`);
    copyRecursive(src, path.join(out, asset));
  }

  for (const s of skipped) warnings.push(`optional manifest doc absent from tree, skipped: ${s}`);
  for (const w of warnings) log(`warning: ${w}`);
  log(`built ${docs.length} doc pages + docs index, ${assets.size} assets -> ${out}`);
  return { pages: [...pages.values(), indexPage], assets: [...assets], skipped, warnings };
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    buildSite(parseArgs(process.argv.slice(2)));
  } catch (err) {
    console.error(`build failed: ${err.message}`);
    process.exit(1);
  }
}
