// Link policy for the Aius site build (site/build.mjs).
//
// Owns: mapping a doc-relative href/src to its published form. A link to a
// manifest doc becomes a relative link to its rendered page; an image (or
// image link) that exists in the tree becomes a relative link to a copied
// asset; anything else in the tree becomes a link into the public GitHub
// repo (`blob` for files, `tree` for directories). External URLs and bare
// fragments pass through. Does not own: reading or copying files; the caller
// supplies `exists` / `isDir` and drains `assets`.

import path from 'node:path';

export const IMAGE_EXT = /\.(png|jpe?g|gif|svg|webp|avif)$/i;
const SCHEME_RE = /^[a-z][a-z0-9+.-]*:/i;

/**
 * Relative href from one output page to another output path.
 * @param {string} fromPage output-relative page path, e.g. `docs/reference/a.html`
 * @param {string} toPath output-relative target path
 * @returns {string}
 */
export function relativeHref(fromPage, toPath) {
  const rel = path.posix.relative(path.posix.dirname(fromPage), toPath);
  return rel === '' ? path.posix.basename(toPath) : rel;
}

/**
 * Build the URL rewriter for one site build.
 * @param {object} options
 * @param {string} options.repoUrl public repo URL, e.g. `https://github.com/d3aek/aius`
 * @param {string} options.branch branch the blob links point at
 * @param {Map<string, string>} options.pages tree path of each manifest doc to its output page path
 * @param {(treePath: string) => boolean} options.exists true when the tree path exists
 * @param {(treePath: string) => boolean} options.isDir true when the tree path is a directory
 * @returns {(url: string, from: string, kind: 'img' | 'link', assets: Set<string>) => string}
 */
export function createLinkRewriter({ repoUrl, branch, pages, exists, isDir }) {
  return function rewrite(url, from, kind, assets) {
    const trimmed = url.trim();
    if (trimmed === '' || SCHEME_RE.test(trimmed) || trimmed.startsWith('//') || trimmed.startsWith('#')) return url;
    const hashAt = trimmed.indexOf('#');
    const frag = hashAt >= 0 ? trimmed.slice(hashAt) : '';
    let rel = hashAt >= 0 ? trimmed.slice(0, hashAt) : trimmed;
    const queryAt = rel.indexOf('?');
    if (queryAt >= 0) rel = rel.slice(0, queryAt);
    try {
      rel = decodeURIComponent(rel);
    } catch {
      // keep the raw form when it is not valid percent-encoding
    }
    const joined = rel.startsWith('/') ? rel.slice(1) : path.posix.join(path.posix.dirname(from), rel);
    const target = path.posix.normalize(joined).replace(/\/+$/, '');
    const fromPage = pages.get(from) ?? from;
    if (target === '..' || target.startsWith('../')) return url;
    if (pages.has(target)) return relativeHref(fromPage, pages.get(target)) + frag;
    const inTree = target === '.' || target === '' ? '' : target;
    if (inTree && (kind === 'img' || IMAGE_EXT.test(inTree)) && exists(inTree) && !isDir(inTree)) {
      assets.add(inTree);
      return relativeHref(fromPage, inTree) + frag;
    }
    const mode = inTree === '' || isDir(inTree) ? 'tree' : 'blob';
    const encoded = inTree.split('/').map(encodeURIComponent).join('/');
    return `${repoUrl}/${mode}/${branch}${encoded ? `/${encoded}` : ''}${frag}`;
  };
}
