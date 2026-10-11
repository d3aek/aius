#!/usr/bin/env python3
"""Builds one gzip bundle per web page (Phase 4g G1).

For each page, every stylesheet <link> and <script src> is replaced in document
order by the file's content inlined, the closing script/style tags inside that
content are escaped, and the page is gzip'd with mtime 0 so the output is
deterministic. Fonts and other assets stay separate. The sources are not
modified.

Usage:
    www_bundle.py --www firmware/sd/www --out DIR
    www_bundle.py --www firmware/sd/www --inlined

--out is required (never defaults into the source tree). Writes <page>.html.gz
per page. --inlined instead prints "<page> <asset>" per inlined asset, asset
relative to --www, and writes nothing. --referenced prints the assets existing
pages reference without reading them (never fails on a missing page or asset).
Exit 1 on a missing page or asset, a script containing "<!--" (page and asset
named), or an inlined script that reads document.currentScript.src
(the bundle has no script URL). Exit 2 on a usage error.
"""
import argparse
import gzip
import posixpath
import re
import sys
from pathlib import Path

PAGES = ("index.html", "status.html", "replays.html", "upgrade.html", "mixer.html")

_TAG = re.compile(
    r"<link\b[^>]*>|<script\b[^>]*\bsrc\s*=[^>]*>\s*</script\s*>",
    re.IGNORECASE,
)
_HREF = re.compile(r"""\bhref\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.IGNORECASE)
_SRC = re.compile(r"""\bsrc\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.IGNORECASE)
_REL_SHEET = re.compile(r"""\brel\s*=\s*["']?[^"'>]*\bstylesheet\b""", re.IGNORECASE)
_CURRENT_SCRIPT_SRC = re.compile(r"currentScript\s*(?:\.\s*src\b|\[\s*['\"]src['\"]\s*\])")
_CLOSE_SCRIPT = re.compile(r"</(script)", re.IGNORECASE)
_CLOSE_STYLE = re.compile(r"</(style)", re.IGNORECASE)
_HTML_COMMENT_OPEN = re.compile(r"<!--")
_SCRIPT_BLOCK =re.compile(r"<script\b[^>]*>(.*?)</script\s*>", re.IGNORECASE | re.DOTALL)


class BundleError(Exception):
    pass


def _read_asset(www, page, ref, assets=None):
    if re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:|^//", ref):
        raise BundleError(f"{page}: external asset cannot be inlined: {ref}")
    path = (www / ref.split("?", 1)[0].split("#", 1)[0]).resolve()
    if www.resolve() not in path.parents:
        raise BundleError(f"{page}: asset outside the web tree: {ref}")
    if not path.is_file():
        raise BundleError(f"{page}: missing asset: {ref}")
    if assets is not None:
        assets.append(path.relative_to(www.resolve()).as_posix())
    return path.read_text(encoding="utf-8")


def bundle_page(www, page, assets=None):
    """Returns the inlined page text; appends each inlined asset path to assets."""
    src = www / page
    if not src.is_file():
        raise BundleError(f"missing page: {page}")
    html = src.read_text(encoding="utf-8")

    def replace(match):
        tag = match.group(0)
        if tag[:5].lower() == "<link":
            if not _REL_SHEET.search(tag):
                return tag
            m = _HREF.search(tag)
            if not m:
                raise BundleError(f"{page}: stylesheet link without href: {tag}")
            body = _read_asset(www, page, m.group(1) or m.group(2), assets)
            body = _CLOSE_STYLE.sub(r"<\\/\1", body)
            return f"<style>\n{body}\n</style>"
        m = _SRC.search(tag.split(">", 1)[0] + ">")
        ref = m.group(1) or m.group(2)
        attrs = _SRC.sub("", tag.split(">", 1)[0][len("<script"):]).strip()
        body = _read_asset(www, page, ref, assets)
        body = _CLOSE_SCRIPT.sub(r"<\\/\1", body)
        # "<!--" can enter the double-escaped script state; rewriting it would
        # change string literals, so the build refuses it instead.
        if _HTML_COMMENT_OPEN.search(body):
            raise BundleError(f"{page}: inlined script contains <!--: {ref}")
        opener = "<script" + (" " + attrs if attrs else "") + ">"
        return f"{opener}\n{body}\n</script>"

    out = _TAG.sub(replace, html)
    for block in _SCRIPT_BLOCK.finditer(out):
        if _CURRENT_SCRIPT_SRC.search(block.group(1)):
            raise BundleError(f"{page}: an inlined script reads document.currentScript.src")
    return out


def referenced(www):
    """Returns the set of asset paths any existing page references, unread."""
    www = Path(www)
    refs = set()
    for page in PAGES:
        src = www / page
        if not src.is_file():
            continue
        for match in _TAG.finditer(src.read_text(encoding="utf-8")):
            tag = match.group(0)
            if tag[:5].lower() == "<link":
                if not _REL_SHEET.search(tag):
                    continue
                m = _HREF.search(tag)
            else:
                m = _SRC.search(tag.split(">", 1)[0] + ">")
            if m:
                ref = (m.group(1) or m.group(2)).split("?", 1)[0].split("#", 1)[0]
                refs.add(posixpath.normpath(ref))
    return refs


def build(www, out):
    www = Path(www)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    written = []
    for page in PAGES:
        text = bundle_page(www, page)
        data = gzip.compress(text.encode("utf-8"), compresslevel=9, mtime=0)
        target = out / (page + ".gz")
        target.write_bytes(data)
        written.append(target)
    return written


def inlined(www):
    """Returns [(page, asset)] for every page, in document order."""
    www = Path(www)
    pairs = []
    for page in PAGES:
        assets = []
        bundle_page(www, page, assets)
        pairs.extend((page, a) for a in assets)
    return pairs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--www", required=True)
    parser.add_argument("--out")
    parser.add_argument("--inlined", action="store_true")
    parser.add_argument("--referenced", action="store_true")
    args = parser.parse_args(argv)
    if args.referenced:
        for asset in sorted(referenced(args.www)):
            print(asset)
        return 0
    if not args.inlined and not args.out:
        parser.error("--out is required (or use --inlined)")
    try:
        if args.inlined:
            for page, asset in inlined(args.www):
                print(f"{page} {asset}")
            return 0
        written = build(args.www, args.out)
    except BundleError as err:
        print(f"www_bundle: {err}", file=sys.stderr)
        return 1
    for path in written:
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
