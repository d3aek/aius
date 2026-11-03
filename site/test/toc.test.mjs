// Per-page "On this page" TOC builder.
// Run: node --test site/test/

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildToc } from '../lib/toc.mjs';
import { renderMarkdown } from '../lib/markdown.mjs';

const h = (level, id, text) => ({ level, id, text });

test('fewer than three h2 headings yields no TOC', () => {
  const toc = buildToc([h(1, 't', 'T'), h(2, 'a', 'A'), h(2, 'b', 'B'), h(3, 'c', 'C')]);
  assert.deepEqual(toc, { rail: '', details: '' });
});

test('h2 and h3 are listed, h1 and h4 skipped, text escaped', () => {
  const toc = buildToc([h(1, 't', 'T'), h(2, 'a', 'A & B'), h(3, 'a1', 'Sub'), h(4, 'x', 'Deep'), h(2, 'b', 'B'), h(2, 'c', 'C')]);
  assert.match(toc.rail, /href="#a">A &amp; B</);
  assert.match(toc.rail, /toc__item--h3[^>]*><a class="toc__link" href="#a1"/);
  assert.doesNotMatch(toc.rail, /Deep|href="#t"/);
  assert.match(toc.details, /^ *<details class="toc toc--fold">/);
});

test('inline code and links are stripped from heading text via the renderer', () => {
  const md = '# T\n\n## A `code` [link](x.md)\n\n## B\n\n## C\n\n### D\n';
  const { headings } = renderMarkdown(md);
  const toc = buildToc(headings);
  assert.match(toc.rail, />A code link</);
  assert.doesNotMatch(toc.rail, /<code|<a [^>]*href="x/);
});
