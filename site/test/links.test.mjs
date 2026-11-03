// Link rewriter and renderer URL wiring for the Aius site build.
// Run: node --test site/test/  (node:test, no npm dependency, like the build itself).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createLinkRewriter } from '../lib/links.mjs';
import { renderMarkdown, slugify, splitFrontMatter } from '../lib/markdown.mjs';

const TREE = new Set(['README.md', 'docs/reference/a.md', 'docs/reference/images/x.png', 'firmware/main.c']);
const DIRS = new Set(['docs', 'docs/reference', 'docs/reference/images', 'firmware', 'evidence']);
const pages = new Map([
  ['README.md', 'docs/overview.html'],
  ['docs/reference/a.md', 'docs/reference/a.html'],
]);
const rewrite = createLinkRewriter({
  repoUrl: 'https://github.com/d3aek/aius',
  branch: 'master',
  pages,
  exists: (p) => TREE.has(p) || DIRS.has(p),
  isDir: (p) => DIRS.has(p),
});

test('manifest doc links map to rendered pages and keep the fragment', () => {
  const assets = new Set();
  assert.equal(rewrite('../../README.md#licence', 'docs/reference/a.md', 'link', assets), '../overview.html#licence');
  assert.equal(rewrite('docs/reference/a.md', 'README.md', 'link', assets), 'reference/a.html');
  assert.equal(rewrite('a.md', 'docs/reference/a.md', 'link', assets), 'a.html');
});

test('images in the tree are copied and linked relative to the page', () => {
  const assets = new Set();
  assert.equal(rewrite('docs/reference/images/x.png', 'README.md', 'img', assets), 'reference/images/x.png');
  assert.equal(rewrite('images/x.png', 'docs/reference/a.md', 'img', assets), 'images/x.png');
  assert.deepEqual([...assets], ['docs/reference/images/x.png']);
});

test('unlisted files go to the GitHub blob URL, directories to tree', () => {
  const assets = new Set();
  assert.equal(
    rewrite('../../firmware/main.c', 'docs/reference/a.md', 'link', assets),
    'https://github.com/d3aek/aius/blob/master/firmware/main.c',
  );
  assert.equal(rewrite('evidence/', 'README.md', 'link', assets), 'https://github.com/d3aek/aius/tree/master/evidence');
  assert.equal(
    rewrite('../plans/x.md#phase-2', 'docs/reference/a.md', 'link', assets),
    'https://github.com/d3aek/aius/blob/master/docs/plans/x.md#phase-2',
  );
  assert.equal(assets.size, 0);
});

test('external URLs and bare fragments pass through', () => {
  const assets = new Set();
  for (const u of ['https://example.com/a.md', 'mailto:a@b.c', '#section', '//cdn.example/x.js']) {
    assert.equal(rewrite(u, 'README.md', 'link', assets), u);
  }
});

test('renderer routes markdown and raw HTML URLs through the hook and strips front-matter', () => {
  const { meta, body } = splitFrontMatter('---\ntitle: "Hello: world"\ntype: reference\n---\n\n# Head\n');
  assert.equal(meta.title, 'Hello: world');
  assert.equal(body.trim(), '# Head');
  const seen = [];
  const { html, hasMermaid } = renderMarkdown(
    '[a](x.md) ![alt\ntext](i.png)\n\n<img src="j.png"\n  width="50%">\n\n```mermaid\nA-->B\n```\n',
    {
      rewriteUrl: (u, kind) => {
        seen.push(`${kind}:${u}`);
        return `R/${u}`;
      },
    },
  );
  assert.deepEqual(seen, ['link:x.md', 'img:i.png', 'img:j.png']);
  assert.match(html, /<a href="R\/x.md">a<\/a>/);
  assert.match(html, /<img src="R\/i.png" alt="alt text"/);
  assert.match(html, /<pre class="mermaid">A--&gt;B<\/pre>/);
  assert.equal(hasMermaid, true);
});

test('heading ids follow GitHub slugs, so cross-doc anchors resolve', () => {
  assert.equal(slugify('S/PDIF output — the transmit half of the hot path'), 'spdif-output--the-transmit-half-of-the-hot-path');
  const { html } = renderMarkdown('## Phase 3: TX ring\n\n## Phase 3: TX ring\n');
  assert.match(html, /id="phase-3-tx-ring"/);
  assert.match(html, /id="phase-3-tx-ring-1"/);
});

test('snake_case identifiers do not italicise; tables and nested lists render', () => {
  const { html } = renderMarkdown('use a_b_c and *em*\n\n| a | b |\n|---|--:|\n| `x|y` | 2 |\n\n- one\n  - two\n- three\n');
  assert.match(html, /a_b_c/);
  assert.match(html, /<em>em<\/em>/);
  assert.match(html, /<td style="text-align:right">2<\/td>/);
  assert.match(html, /<li>one\n<ul>\n<li>two<\/li>/);
});
