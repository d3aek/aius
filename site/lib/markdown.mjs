// Dependency-free Markdown renderer for the Aius site build (site/build.mjs).
//
// Owns: GitHub-flavoured Markdown to HTML for the subset the Aius docs use:
// ATX headings with GitHub-style ids, paragraphs, nested lists, pipe tables,
// fenced code (```mermaid becomes <pre class="mermaid"> for client-side
// rendering), blockquotes, rules, raw HTML blocks, inline code, emphasis,
// strikethrough, links, images, reference links and bare-URL autolinks.
// Does not own: URL policy. Every href/src (Markdown or raw HTML) goes through
// the caller's `rewriteUrl(url, kind)` hook, kind 'img' or 'link'.

const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' };
const INLINE_TAGS = new Set([
  'a', 'abbr', 'b', 'br', 'code', 'del', 'em', 'i', 'img', 'kbd', 'mark', 's', 'small', 'span', 'strong', 'sub',
  'sup', 'u', 'wbr',
]);
const LIST_RE = /^( *)([-*+]|\d{1,9}[.)])( +|$)/;
const FENCE_RE = /^( {0,3})(`{3,}|~{3,})\s*([\w+-]*)/;
const HEADING_RE = /^ {0,3}(#{1,6})\s+(.*?)(?:\s+#+)?\s*$/;
const HR_RE = /^ {0,3}([-*_])(?:\s*\1){2,}\s*$/;
const TABLE_SEP_RE = /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$/;
const HTML_BLOCK_RE = /^ {0,3}(<!--|<\/?[a-zA-Z][\w-]*(\s|\/?>|$))/;
const REF_DEF_RE = /^ {0,3}\[([^\]]+)\]:\s*<?(\S+?)>?(?:\s+["'(](.*)["')])?\s*$/;

/**
 * Escape text for HTML element content and double-quoted attributes.
 * @param {string} text
 * @returns {string}
 */
export function escapeHtml(text) {
  return text.replace(/[&<>"]/g, (c) => ESC[c]);
}

/**
 * GitHub-style heading anchor: lower-case, punctuation dropped, each space a hyphen.
 * @param {string} text plain heading text
 * @returns {string}
 */
export function slugify(text) {
  return text
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\s_-]/gu, '')
    .replace(/\s/g, '-');
}

/**
 * Split YAML front-matter from a Markdown source. Only flat `key: value` lines are read.
 * @param {string} src
 * @returns {{ meta: Record<string, string>, body: string }}
 */
export function splitFrontMatter(src) {
  const m = /^---\r?\n([\s\S]*?)\r?\n---[ \t]*(\r?\n|$)/.exec(src);
  if (!m) return { meta: {}, body: src };
  const meta = {};
  for (const line of m[1].split(/\r?\n/)) {
    const kv = /^([A-Za-z_][\w-]*):\s*(.*?)\s*$/.exec(line);
    if (kv) meta[kv[1]] = kv[2].replace(/^(["'])(.*)\1$/, '$2');
  }
  return { meta, body: src.slice(m[0].length) };
}

/**
 * Rewrite every src/href attribute in a raw HTML fragment.
 * @param {string} html
 * @param {(url: string, kind: 'img' | 'link') => string} rewriteUrl
 * @returns {string}
 */
export function rewriteHtmlUrls(html, rewriteUrl) {
  return html.replace(/\b(src|href)\s*=\s*("([^"]*)"|'([^']*)')/gi, (_all, name, _q, dq, sq) => {
    const url = dq ?? sq;
    return `${name}="${escapeHtml(rewriteUrl(url, name.toLowerCase() === 'src' ? 'img' : 'link'))}"`;
  });
}

/**
 * Render a Markdown document body (front-matter already stripped).
 * @param {string} src
 * @param {{ rewriteUrl?: (url: string, kind: 'img' | 'link') => string }} [options]
 * @returns {{ html: string, headings: { level: number, id: string, text: string }[], hasMermaid: boolean }}
 */
export function renderMarkdown(src, options = {}) {
  const ctx = {
    url: options.rewriteUrl ?? ((u) => u),
    refs: new Map(),
    slugs: new Map(),
    headings: [],
    hasMermaid: false,
  };
  const lines = [];
  let inFence = null;
  for (const raw of src.replace(/\r\n?/g, '\n').replace(/\t/g, '    ').split('\n')) {
    const fence = FENCE_RE.exec(raw);
    if (inFence) {
      if (new RegExp(`^ {0,3}${inFence[0]}{${inFence.length},}\\s*$`).test(raw)) inFence = null;
    } else if (fence) {
      inFence = fence[2];
    } else {
      const ref = REF_DEF_RE.exec(raw);
      if (ref) {
        ctx.refs.set(ref[1].toLowerCase(), { url: ref[2], title: ref[3] });
        continue;
      }
    }
    lines.push(raw);
  }
  const html = renderBlocks(lines, ctx, false);
  return { html, headings: ctx.headings, hasMermaid: ctx.hasMermaid };
}

const indentOf = (line) => line.search(/\S|$/);
const isBlank = (line) => /^\s*$/.test(line);

function isBlockStart(line) {
  return (
    FENCE_RE.test(line) || HEADING_RE.test(line) || HR_RE.test(line) || /^ {0,3}>/.test(line) || LIST_RE.test(line)
  );
}

function renderBlocks(lines, ctx, tight) {
  const out = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (isBlank(line)) {
      i++;
      continue;
    }
    let m = FENCE_RE.exec(line);
    if (m) {
      const fence = m[2];
      const close = new RegExp(`^ {0,3}${fence[0]}{${fence.length},}\\s*$`);
      const indent = m[1].length;
      const body = [];
      i++;
      while (i < lines.length && !close.test(lines[i])) {
        body.push(lines[i].slice(Math.min(indent, indentOf(lines[i]))));
        i++;
      }
      i++;
      const code = escapeHtml(body.join('\n'));
      if (m[3] === 'mermaid') {
        ctx.hasMermaid = true;
        out.push(`<pre class="mermaid">${code}</pre>`);
      } else {
        out.push(`<pre><code${m[3] ? ` class="language-${escapeHtml(m[3])}"` : ''}>${code}</code></pre>`);
      }
      continue;
    }
    m = HEADING_RE.exec(line);
    if (m) {
      out.push(renderHeading(m[1].length, m[2], ctx));
      i++;
      continue;
    }
    if (HR_RE.test(line)) {
      out.push('<hr>');
      i++;
      continue;
    }
    if (/^ {0,3}>/.test(line)) {
      const inner = [];
      while (i < lines.length && !isBlank(lines[i])) {
        if (/^ {0,3}>/.test(lines[i])) inner.push(lines[i].replace(/^ {0,3}> ?/, ''));
        else if (isBlockStart(lines[i])) break;
        else inner.push(lines[i]);
        i++;
      }
      out.push(`<blockquote>\n${renderBlocks(inner, ctx, false)}\n</blockquote>`);
      continue;
    }
    if (line.includes('|') && i + 1 < lines.length && TABLE_SEP_RE.test(lines[i + 1])) {
      const end = renderTable(lines, i, ctx, out);
      if (end > i) {
        i = end;
        continue;
      }
    }
    if (LIST_RE.test(line) && !(LIST_RE.exec(line)[3] === '' && isBlank(lines[i + 1] ?? ''))) {
      i = renderList(lines, i, ctx, out);
      continue;
    }
    if (HTML_BLOCK_RE.test(line)) {
      const block = [];
      while (i < lines.length && !isBlank(lines[i])) block.push(lines[i++]);
      out.push(rewriteHtmlUrls(block.join('\n'), ctx.url));
      continue;
    }
    const para = [line.trim()];
    i++;
    while (i < lines.length && !isBlank(lines[i]) && !isBlockStart(lines[i])) {
      if (lines[i].includes('|') && i + 1 < lines.length && TABLE_SEP_RE.test(lines[i + 1])) break;
      para.push(lines[i].replace(/^ +/, ''));
      i++;
    }
    const inline = renderInline(para.join('\n'), ctx);
    out.push(tight ? inline : `<p>${inline}</p>`);
  }
  return out.join('\n');
}

function plainText(html) {
  return html
    .replace(/<[^>]+>/g, '')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, '&');
}

function renderHeading(level, text, ctx) {
  const inner = renderInline(text, ctx);
  const plain = plainText(inner);
  let id = slugify(plain);
  const seen = ctx.slugs.get(id);
  ctx.slugs.set(id, (seen ?? -1) + 1);
  if (seen !== undefined) id = `${id}-${seen + 1}`;
  ctx.headings.push({ level, id, text: plain });
  return `<h${level} id="${escapeHtml(id)}">${inner}</h${level}>`;
}

function splitRow(line) {
  let s = line.trim();
  if (s.startsWith('|')) s = s.slice(1);
  if (s.endsWith('|') && !s.endsWith('\\|')) s = s.slice(0, -1);
  const cells = [];
  let cur = '';
  let code = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (c === '\\' && s[i + 1] === '|') {
      cur += '|';
      i++;
    } else if (c === '`') {
      code = code ? 0 : 1;
      cur += c;
    } else if (c === '|' && !code) {
      cells.push(cur.trim());
      cur = '';
    } else {
      cur += c;
    }
  }
  cells.push(cur.trim());
  return cells;
}

function renderTable(lines, start, ctx, out) {
  const head = splitRow(lines[start]);
  const aligns = splitRow(lines[start + 1]).map((c) => {
    if (c.startsWith(':') && c.endsWith(':')) return 'center';
    if (c.endsWith(':')) return 'right';
    if (c.startsWith(':')) return 'left';
    return '';
  });
  const cell = (tag, text, idx) =>
    `<${tag}${aligns[idx] ? ` style="text-align:${aligns[idx]}"` : ''}>${renderInline(text, ctx)}</${tag}>`;
  const rows = [];
  let i = start + 2;
  while (i < lines.length && !isBlank(lines[i]) && lines[i].includes('|')) {
    const cells = splitRow(lines[i]);
    rows.push(`<tr>${head.map((_h, idx) => cell('td', cells[idx] ?? '', idx)).join('')}</tr>`);
    i++;
  }
  out.push(
    `<div class="prose__table"><table>\n<thead><tr>${head.map((h, idx) => cell('th', h, idx)).join('')}</tr></thead>\n` +
      `<tbody>\n${rows.join('\n')}\n</tbody></table></div>`,
  );
  return i;
}

function renderList(lines, start, ctx, out) {
  const first = LIST_RE.exec(lines[start]);
  const base = first[1].length;
  const ordered = /\d/.test(first[2]);
  const items = [];
  let loose = false;
  let i = start;
  while (i < lines.length) {
    const m = LIST_RE.exec(lines[i]);
    if (!m || m[1].length !== base || /\d/.test(m[2]) !== ordered) break;
    const offset = m[0].length || m[1].length + m[2].length + 1;
    const body = [lines[i].slice(offset)];
    i++;
    while (i < lines.length) {
      const l = lines[i];
      if (isBlank(l)) {
        let j = i;
        while (j < lines.length && isBlank(lines[j])) j++;
        if (j < lines.length && indentOf(lines[j]) >= offset) {
          for (let k = i; k < j; k++) body.push('');
          i = j;
          loose = true;
          continue;
        }
        const n = j < lines.length ? LIST_RE.exec(lines[j]) : null;
        if (n && n[1].length === base && /\d/.test(n[2]) === ordered) {
          loose = true;
          i = j;
        }
        break;
      }
      const ind = indentOf(l);
      const lm = LIST_RE.exec(l);
      if (lm && ind <= base) break;
      if (ind >= offset || (lm && ind > base)) {
        body.push(l.slice(Math.min(ind, offset)));
        i++;
        continue;
      }
      if (isBlockStart(l)) break;
      body.push(l.trim());
      i++;
    }
    items.push(body);
  }
  const tag = ordered ? 'ol' : 'ul';
  const startNum = ordered ? parseInt(first[2], 10) : 1;
  const startAttr = ordered && startNum !== 1 ? ` start="${startNum}"` : '';
  const lis = items.map((body) => {
    let text = renderBlocks(body, ctx, !loose);
    const task = /^\[([ xX])\]\s+/.exec(text);
    if (task) text = `<input type="checkbox" disabled${task[1] === ' ' ? '' : ' checked'}> ${text.slice(task[0].length)}`;
    return `<li>${text}</li>`;
  });
  out.push(`<${tag}${startAttr}>\n${lis.join('\n')}\n</${tag}>`);
  return i;
}

function findCodeEnd(text, i, run) {
  let j = i;
  while ((j = text.indexOf(run, j)) >= 0) {
    if (text[j + run.length] !== '`' && text[j - 1] !== '`') return j;
    j += run.length;
  }
  return -1;
}

function parseBracket(text, i) {
  let depth = 0;
  for (let j = i; j < text.length; j++) {
    const c = text[j];
    if (c === '\\') {
      j++;
    } else if (c === '`') {
      const run = /^`+/.exec(text.slice(j))[0];
      const end = findCodeEnd(text, j + run.length, run);
      if (end >= 0) j = end + run.length - 1;
    } else if (c === '[') {
      depth++;
    } else if (c === ']') {
      depth--;
      if (depth === 0) return j;
    }
  }
  return -1;
}

function parseLink(text, i, ctx) {
  const close = parseBracket(text, i);
  if (close < 0) return null;
  const label = text.slice(i + 1, close);
  let j = close + 1;
  if (text[j] === '(') {
    j++;
    while (text[j] === ' ' || text[j] === '\n') j++;
    let url = '';
    if (text[j] === '<') {
      const end = text.indexOf('>', j);
      if (end < 0) return null;
      url = text.slice(j + 1, end);
      j = end + 1;
    } else {
      let depth = 0;
      while (j < text.length && !/\s/.test(text[j])) {
        if (text[j] === '(') depth++;
        else if (text[j] === ')') {
          if (depth === 0) break;
          depth--;
        }
        url += text[j++];
      }
    }
    while (text[j] === ' ' || text[j] === '\n') j++;
    let title;
    const tm = /^(["'])(.*?)\1/s.exec(text.slice(j));
    if (tm) {
      title = tm[2];
      j += tm[0].length;
      while (text[j] === ' ' || text[j] === '\n') j++;
    }
    if (text[j] !== ')') return null;
    return { label, url, title, end: j + 1 };
  }
  const refM = /^\[([^\]]*)\]/.exec(text.slice(j));
  const key = (refM && refM[1] ? refM[1] : label).toLowerCase().replace(/\s+/g, ' ');
  const ref = ctx.refs.get(key);
  if (!ref) return null;
  return { label, url: ref.url, title: ref.title, end: refM ? j + refM[0].length : j };
}

function findClose(text, from, delim, wordBound) {
  let j = from;
  while ((j = text.indexOf(delim, j)) >= 0) {
    const before = text[j - 1];
    const after = text[j + delim.length];
    const ok =
      before !== undefined &&
      !/\s/.test(before) &&
      (delim.length > 1 || after !== delim[0]) &&
      (!wordBound || after === undefined || !/[\p{L}\p{N}]/u.test(after));
    if (ok) return j;
    j += delim.length;
  }
  return -1;
}

/**
 * Render inline Markdown.
 * @param {string} text
 * @param {{ url: (url: string, kind: string) => string, refs: Map<string, { url: string, title?: string }> }} ctx
 * @returns {string}
 */
export function renderInline(text, ctx) {
  const src = text.replace(/(?: {2,}|\\)\n/g, '\u0000');
  let out = '';
  let i = 0;
  while (i < src.length) {
    const c = src[i];
    const rest = src.slice(i);
    if (c === '\u0000') {
      out += '<br>\n';
      i++;
      continue;
    }
    if (c === '\\' && /^[!-/:-@[-`{-~]$/.test(src[i + 1] ?? '')) {
      out += escapeHtml(src[i + 1]);
      i += 2;
      continue;
    }
    if (c === '`') {
      const run = /^`+/.exec(rest)[0];
      const end = findCodeEnd(src, i + run.length, run);
      if (end >= 0) {
        let code = src.slice(i + run.length, end).replace(/\n/g, ' ');
        if (/^ .*[^ ].* $/.test(code)) code = code.slice(1, -1);
        out += `<code>${escapeHtml(code)}</code>`;
        i = end + run.length;
      } else {
        out += run;
        i += run.length;
      }
      continue;
    }
    if (c === '!' && src[i + 1] === '[') {
      const l = parseLink(src, i + 1, ctx);
      if (l) {
        const alt = escapeHtml(plainText(renderInline(l.label, ctx)).replace(/\s+/g, ' '));
        const title = l.title ? ` title="${escapeHtml(l.title)}"` : '';
        out += `<img src="${escapeHtml(ctx.url(l.url, 'img'))}" alt="${alt}"${title} loading="lazy">`;
        i = l.end;
        continue;
      }
    }
    if (c === '[') {
      const l = parseLink(src, i, ctx);
      if (l) {
        const title = l.title ? ` title="${escapeHtml(l.title)}"` : '';
        out += `<a href="${escapeHtml(ctx.url(l.url, 'link'))}"${title}>${renderInline(l.label, ctx)}</a>`;
        i = l.end;
        continue;
      }
    }
    if (c === '<') {
      const auto = /^<((?:https?|mailto):[^>\s]+)>/.exec(rest);
      if (auto) {
        out += `<a href="${escapeHtml(auto[1])}">${escapeHtml(auto[1])}</a>`;
        i += auto[0].length;
        continue;
      }
      const tag = /^<\/?([a-zA-Z][\w-]*)(\s[^<>]*)?\/?>/.exec(rest);
      if (tag && INLINE_TAGS.has(tag[1].toLowerCase())) {
        out += rewriteHtmlUrls(tag[0], ctx.url);
        i += tag[0].length;
        continue;
      }
      if (rest.startsWith('<!--')) {
        const end = src.indexOf('-->', i);
        if (end >= 0) {
          i = end + 3;
          continue;
        }
      }
    }
    if ((c === 'h' || c === 'w') && !/[\p{L}\p{N}]/u.test(src[i - 1] ?? ' ')) {
      const bare = /^(?:https?:\/\/|www\.)[^\s<]*[^\s<.,:;"')\]*_]/.exec(rest);
      if (bare) {
        const href = bare[0].startsWith('www.') ? `https://${bare[0]}` : bare[0];
        out += `<a href="${escapeHtml(href)}">${escapeHtml(bare[0])}</a>`;
        i += bare[0].length;
        continue;
      }
    }
    if (c === '*' || c === '_' || c === '~') {
      const run = /^(\*+|_+|~~)/.exec(rest)?.[0];
      if (run) {
        const wordBound = c === '_';
        const prevWord = /[\p{L}\p{N}]/u.test(src[i - 1] ?? ' ');
        const delim = c === '~' ? '~~' : run.length >= 2 ? c + c : c;
        const opens = !/\s/.test(src[i + delim.length] ?? ' ') && !(wordBound && prevWord);
        if (opens) {
          const end = findClose(src, i + delim.length, delim, wordBound);
          if (end > i + delim.length) {
            const inner = renderInline(src.slice(i + delim.length, end), ctx);
            const tagName = c === '~' ? 'del' : delim.length === 2 ? 'strong' : 'em';
            out += `<${tagName}>${inner}</${tagName}>`;
            i = end + delim.length;
            continue;
          }
        }
        out += escapeHtml(run);
        i += run.length;
        continue;
      }
    }
    if (c === '&') {
      const ent = /^&(?:#\d+|#x[\da-f]+|[a-z][a-z\d]*);/i.exec(rest);
      if (ent) {
        out += ent[0];
        i += ent[0].length;
        continue;
      }
    }
    out += escapeHtml(c);
    i++;
  }
  return out;
}
