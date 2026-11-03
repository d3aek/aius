// Per-page "On this page" table of contents for rendered docs.
// Owns: picking h2/h3 headings and emitting the two static blocks (desktop
// rail, phone details). Heading text arrives already stripped of inline markup.

import { escapeHtml } from './markdown.mjs';

export const MIN_H2 = 3;

/**
 * @param {{ level: number, id: string, text: string }[]} headings
 * @returns {{ rail: string, details: string }} empty strings when the page is too short
 */
export function buildToc(headings) {
  const items = headings.filter((h) => h.level === 2 || h.level === 3);
  if (items.filter((h) => h.level === 2).length < MIN_H2) return { rail: '', details: '' };
  const list = items
    .map((h) => `<li class="toc__item toc__item--h${h.level}"><a class="toc__link" href="#${escapeHtml(h.id)}">${escapeHtml(h.text)}</a></li>`)
    .join('\n');
  return {
    rail: `    <aside class="toc toc--rail" aria-label="On this page">\n      <p class="toc__title mono">On this page</p>\n      <ul class="toc__list">\n${list}\n      </ul>\n    </aside>`,
    details: `      <details class="toc toc--fold">\n        <summary class="toc__title mono">On this page</summary>\n        <ul class="toc__list">\n${list}\n        </ul>\n      </details>`,
  };
}
