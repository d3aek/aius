# Aius site

Source for the Aius GitHub Pages site: the marketing page (`index.html`, `assets/`), the docs manifest (`docs.json`) and a dependency-free build that renders the listed docs into the page shell.

## Commands: build, check, test

Run from the repository root. Node 18 or later; no npm install.

```bash
node site/build.mjs --out <dir> [--root <tree>] [--manifest <file>]
node site/check.mjs <dir> [--forbidden publishing/filter/forbidden.txt]
node --test site/test/links.test.mjs
```

- **`build.mjs`**: renders every `docs.json` entry to its page (`page`, else the tree path with `.html`), writes `docs/index.html`, copies `index.html`, `assets/` and every image a doc references. `--root` renders another tree, such as the anonymised mirror. An entry marked `optional` is skipped with a warning when absent (`docs/reference/datasheets.md` exists only in the mirror, from `publishing/overlay/`).
- **`check.mjs`**: fails on any internal `href`/`src` that does not resolve inside the output, or any forbidden literal in an output text file; a missing anchor is a warning.

## Link policy: what a doc link becomes

- **A manifest doc**: a relative link to its rendered page, fragment kept.
- **An image in the tree**: copied to the same tree path in the output and linked relatively.
- **Anything else in the tree**: `https://github.com/d3aek/aius/blob/master/<path>` (`tree/` for a directory).

Mermaid fences render client-side from the jsDelivr `mermaid@11` ESM build, loaded only on pages that have one.

## Conventions: keep the gate green

- **Keep every literal in `publishing/filter/forbidden.txt` out of `site/`.** The site source publishes through the mirror, whose gate fails the run on any surviving literal.
- **Spec values on `index.html` come from the docs.** A value the docs do not settle stays visibly marked `tbd`.
