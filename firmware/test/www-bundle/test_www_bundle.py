"""Host tests for firmware/tools/www_bundle.py and aius-sd-sync.sh (Phase 4g).

Run: python3 -m unittest discover -s firmware/test/www-bundle
"""
import gzip
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
import www_bundle  # noqa: E402

SYNC = TOOLS / "aius-sd-sync.sh"
REAL_WWW = Path(__file__).resolve().parents[2] / "sd" / "www"
TOKEN = "0123456789abcdef"


def make_tree(root, overrides=None):
    """A minimal www tree with all four pages; overrides maps name -> text."""
    files = {}
    for page in www_bundle.PAGES:
        stem = page[:-5]
        files[page] = (
            f'<html><head><link rel="stylesheet" href="{stem}.css">'
            f'</head><body><script src="a.js"></script>'
            f'<script src="{stem}.js"></script></body></html>'
        )
        files[f"{stem}.css"] = f".{stem} {{ color: red }}\n"
        files[f"{stem}.js"] = f"var {stem}Loaded = 1;\n"
    files["a.js"] = "var a = 1;\n"
    files["other.txt"] = "not inlined\n"
    files.update(overrides or {})
    for name, text in files.items():
        path = Path(root) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.www = self.tmp / "www"
        self.out = self.tmp / "out"

    def read_gz(self, page):
        return gzip.decompress((self.out / (page + ".gz")).read_bytes()).decode("utf-8")

    def test_all_three_pages_bundled_including_mixer(self):
        make_tree(self.www)
        www_bundle.build(self.www, self.out)
        names = sorted(p.name for p in self.out.iterdir())
        self.assertEqual(
            names,
            sorted(p + ".gz" for p in
                   ("index.html", "status.html", "upgrade.html")),
        )
        self.assertNotIn("replays.html", www_bundle.PAGES)
        self.assertNotIn("classic.html", www_bundle.PAGES)

    def test_deterministic_output(self):
        make_tree(self.www)
        www_bundle.build(self.www, self.out)
        first = {p.name: p.read_bytes() for p in self.out.iterdir()}
        out2 = self.tmp / "out2"
        www_bundle.build(self.www, out2)
        second = {p.name: p.read_bytes() for p in out2.iterdir()}
        self.assertEqual(first, second)
        for data in first.values():
            self.assertEqual(data[4:8], b"\0\0\0\0")  # gzip mtime field

    def test_document_order_and_no_external_refs(self):
        make_tree(self.www)
        www_bundle.build(self.www, self.out)
        text = self.read_gz("index.html")
        css = text.index(".index { color: red }")
        a = text.index("var a = 1;")
        own = text.index("var indexLoaded = 1;")
        self.assertLess(css, a)
        self.assertLess(a, own)
        self.assertNotIn("<link", text)
        self.assertNotIn(" src=", text)

    def test_round_trip_equals_inlined_page(self):
        make_tree(self.www)
        www_bundle.build(self.www, self.out)
        for page in www_bundle.PAGES:
            self.assertEqual(self.read_gz(page), www_bundle.bundle_page(self.www, page))

    def test_closing_tags_escaped(self):
        make_tree(self.www, {
            "a.js": 'var s = "</script><b>"; var t = "</SCRIPT >";\n',
            "index.css": "a::after { content: '</style>' }\n",
        })
        www_bundle.build(self.www, self.out)
        text = self.read_gz("index.html")
        self.assertEqual(text.lower().count("</script"), 2)  # the two real closers
        self.assertEqual(text.lower().count("</style"), 1)
        self.assertIn("<\\/script>", text)
        self.assertIn("<\\/SCRIPT", text)
        self.assertIn("<\\/style>", text)

    def test_html_comment_open_in_script_fails_naming_page_and_asset(self):
        make_tree(self.www, {"a.js": 'var s = "<!--";\n'})
        with self.assertRaises(www_bundle.BundleError) as ctx:
            www_bundle.build(self.www, self.out)
        self.assertIn("index.html", str(ctx.exception))
        self.assertIn("a.js", str(ctx.exception))
        self.assertEqual(www_bundle.main(["--www", str(self.www), "--out", str(self.out)]), 1)

    def test_out_is_required_and_source_tree_untouched(self):
        make_tree(self.www)
        before = sorted(p.name for p in self.www.iterdir())
        with self.assertRaises(SystemExit) as ctx:
            www_bundle.main(["--www", str(self.www)])
        self.assertEqual(ctx.exception.code, 2)
        self.assertEqual(sorted(p.name for p in self.www.iterdir()), before)

    def test_inlined_lists_each_page_assets(self):
        make_tree(self.www)
        pairs = www_bundle.inlined(self.www)
        self.assertIn(("status.html", "a.js"), pairs)
        self.assertIn(("status.html", "status.css"), pairs)
        self.assertNotIn(("status.html", "index.js"), pairs)

    def test_missing_asset_fails(self):
        make_tree(self.www)
        (self.www / "status.js").unlink()
        with self.assertRaises(www_bundle.BundleError):
            www_bundle.build(self.www, self.out)
        self.assertEqual(www_bundle.main(["--www", str(self.www), "--out", str(self.out)]), 1)

    def test_missing_page_fails(self):
        make_tree(self.www)
        (self.www / "upgrade.html").unlink()
        with self.assertRaises(www_bundle.BundleError):
            www_bundle.build(self.www, self.out)

    def test_current_script_src_rejected(self):
        for body in ("var u = document.currentScript.src;",
                     "var u = document.currentScript . src;",
                     "var u = document.currentScript['src'];"):
            make_tree(self.www, {"a.js": body + "\n"})
            with self.assertRaises(www_bundle.BundleError, msg=body):
                www_bundle.build(self.www, self.out)

    def test_sources_unmodified(self):
        make_tree(self.www)
        before = {p.name: p.read_bytes() for p in self.www.iterdir()}
        www_bundle.build(self.www, self.out)
        after = {p.name: p.read_bytes() for p in self.www.iterdir()}
        self.assertEqual(before, after)

    def test_real_tree_bundles(self):
        www_bundle.build(REAL_WWW, self.out)
        for page in www_bundle.PAGES:
            text = self.read_gz(page)
            self.assertNotIn('rel="stylesheet"', text)
            self.assertNotRegex(text, r'<script[^>]*\bsrc=')
        self.assertEqual(len(list(self.out.glob("*.html.gz"))), 3)
        self.assertFalse((REAL_WWW / "classic.html").exists())

    def test_real_tree_every_page_inlines_the_shared_chrome(self):
        # Every stylesheet link of every real page resolves (a missing one
        # exits 1), and each page inlines sd-chrome.css first (mixer Phase 7).
        pairs = {}
        for page in www_bundle.PAGES:
            assets = []
            www_bundle.bundle_page(REAL_WWW, page, assets)
            pairs[page] = [asset for asset in assets if asset.endswith(".css")]
        for page, sheets in pairs.items():
            self.assertEqual(sheets[:1], ["sd-chrome.css"], page)
        self.assertFalse(any("replays" in sheet for sheets in pairs.values() for sheet in sheets))
        self.assertFalse(list(REAL_WWW.glob("replays.*")))


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.www = self.tmp / "www"
        make_tree(self.www)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.plan = self.tmp / "plan"
        self.log = self.tmp / "curl.log"
        self.sleeps = self.tmp / "sleep.log"
        (self.bin / "curl").write_text(CURL_STUB)
        (self.bin / "sleep").write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$1" >> "$STUB_SLEEPS"\n')
        for name in ("curl", "sleep"):
            (self.bin / name).chmod(0o755)

    def run_sync(self, plan, *extra):
        """plan: lines 'CODE [RETRY_AFTER]'; the last line repeats."""
        self.plan.write_text("\n".join(plan) + "\n")
        env = dict(os.environ)
        env.update(
            PATH=f"{self.bin}{os.pathsep}{env['PATH']}",
            STUB_PLAN=str(self.plan), STUB_LOG=str(self.log),
            STUB_SLEEPS=str(self.sleeps), STUB_COUNT=str(self.tmp / "count"),
        )
        return subprocess.run(
            [str(SYNC), "--device", "http://stub", "--www", str(self.www),
             "--token", TOKEN, *extra],
            env=env, capture_output=True, text=True, timeout=60,
        )

    def sleep_values(self):
        return self.sleeps.read_text().split() if self.sleeps.exists() else []

    def uploads(self):
        return self.log.read_text().split()

    def test_full_sync_uploads_bundles_beside_sources(self):
        result = self.run_sync(["200"])
        self.assertEqual(result.returncode, 0, result.stderr)
        urls = [u.rsplit("/", 1)[1] for u in self.uploads()]
        for page in www_bundle.PAGES:
            self.assertIn(page, urls)
            self.assertIn(page + ".gz", urls)
        self.assertEqual(self.sleep_values(), [])

    def test_file_mode_uploads_page_and_its_bundle(self):
        result = self.run_sync(["200"], "--file", "status.html")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([u.rsplit("/", 1)[1] for u in self.uploads()],
                         ["status.html", "status.html.gz"])

    def test_subdirectory_path_sent_literally(self):
        font = self.www / "fonts" / "x.woff2"
        font.parent.mkdir()
        font.write_bytes(b"font")
        result = self.run_sync(["200"], "--file", "fonts/x.woff2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.uploads(), ["http://stub/sd/www/fonts/x.woff2"])

    def test_503_retry_after_honoured_then_success(self):
        result = self.run_sync(["503 3", "503 2", "200"], "--file", "other.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.sleep_values(), ["3", "2"])
        self.assertEqual(len(self.uploads()), 3)

    def test_503_gives_up_after_15_seconds(self):
        result = self.run_sync(["503 5"], "--file", "other.txt")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.sleep_values(), ["5", "5", "5"])
        self.assertEqual(len(self.uploads()), 4)
        self.assertIn("other.txt", result.stderr)

    def test_503s_do_not_consume_connection_drop_tries(self):
        result = self.run_sync(["503 1", "503 1", "503 1", "000", "200"], "--file", "other.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.uploads()), 5)

    def test_non_503_failures_get_four_tries(self):
        result = self.run_sync(["000"], "--file", "other.txt")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(self.uploads()), 4)
        self.assertEqual(self.sleep_values(), ["1", "1", "1"])

    def test_file_mode_asset_uploads_every_bundle_inlining_it(self):
        result = self.run_sync(["200"], "--file", "a.js")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([u.rsplit("/", 1)[1] for u in self.uploads()],
                         ["a.js"] + [p + ".gz" for p in www_bundle.PAGES])

    def test_file_mode_page_specific_asset_uploads_only_its_bundle(self):
        result = self.run_sync(["200"], "--file", "status.js")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([u.rsplit("/", 1)[1] for u in self.uploads()],
                         ["status.js", "status.html.gz"])

    def test_file_mode_page_and_asset_upload_bundle_once(self):
        result = self.run_sync(["200"], "--file", "status.html", "--file", "status.js")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([u.rsplit("/", 1)[1] for u in self.uploads()].count("status.html.gz"), 1)

    def test_file_mode_non_inlined_file_uploads_no_bundle(self):
        result = self.run_sync(["200"], "--file", "other.txt")
        self.assertEqual([u.rsplit("/", 1)[1] for u in self.uploads()], ["other.txt"])

    def test_file_mode_non_inlined_file_ignores_a_missing_page(self):
        (self.www / "upgrade.html").unlink()
        result = self.run_sync(["200"], "--file", "other.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("bundle", result.stderr)
        self.assertEqual([u.rsplit("/", 1)[1] for u in self.uploads()], ["other.txt"])

    def test_file_mode_page_with_missing_asset_fails_before_upload(self):
        (self.www / "upgrade.js").unlink()
        result = self.run_sync(["200"], "--file", "status.html")
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.log.exists())

    def test_503_retry_after_over_budget_gives_up_at_once(self):
        result = self.run_sync(["503 16"], "--file", "other.txt")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.sleep_values(), [])
        self.assertEqual(len(self.uploads()), 1)

    def requests(self):
        path = Path(str(self.log) + ".methods")
        return path.read_text().splitlines() if path.exists() else []

    def run_delete_only(self, plan, *extra):
        """--delete without --www or --file."""
        self.plan.write_text("\n".join(plan) + "\n")
        env = dict(os.environ)
        env.update(
            PATH=f"{self.bin}{os.pathsep}{env['PATH']}",
            STUB_PLAN=str(self.plan), STUB_LOG=str(self.log),
            STUB_SLEEPS=str(self.sleeps), STUB_COUNT=str(self.tmp / "count"),
        )
        return subprocess.run(
            [str(SYNC), "--device", "http://stub", "--token", TOKEN, *extra],
            env=env, capture_output=True, text=True, timeout=60,
        )

    def test_delete_page_sends_delete_for_page_and_its_bundle(self):
        result = self.run_delete_only(["200"], "--delete", "classic.html")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests(), ["DELETE http://stub/sd/www/classic.html",
                                           "DELETE http://stub/sd/www/classic.html.gz"])
        self.assertTrue(Path(str(self.log) + ".status").exists())

    def test_delete_non_page_sends_one_delete(self):
        result = self.run_delete_only(["200"], "--delete", "other.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests(), ["DELETE http://stub/sd/www/other.txt"])

    def test_delete_404_is_already_gone(self):
        result = self.run_delete_only(["404"], "--delete", "classic.html")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(self.requests()), 2)

    def test_delete_other_failure_fails(self):
        result = self.run_delete_only(["500"], "--delete", "other.txt")
        self.assertEqual(result.returncode, 1)

    def test_delete_without_file_uploads_nothing(self):
        result = self.run_sync(["200"], "--delete", "other.txt")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.requests(), ["DELETE http://stub/sd/www/other.txt"])

    def test_delete_refuses_presets(self):
        result = self.run_delete_only(["200"], "--delete", "presets/a.json")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.requests(), [])

    def test_bundle_failure_stops_before_any_upload(self):
        (self.www / "upgrade.js").unlink()
        result = self.run_sync(["200"])
        self.assertEqual(result.returncode, 1)
        self.assertFalse(self.log.exists())


CURL_STUB = r"""#!/usr/bin/env bash
# Stub curl: logs the URL, answers from STUB_PLAN (last line repeats).
hdr=''; out=''; url=''; method=''
while (($# > 0)); do
    case "$1" in
        --dump-header) hdr=$2; shift 2 ;;
        --output) out=$2; shift 2 ;;
        --request) method=$2; shift 2 ;;
        --write-out|--header|--upload-file) shift 2 ;;
        --*) shift ;;
        *) url=$1; shift ;;
    esac
done
if [[ "$url" == */api/status ]]; then
    # The drained poll: logged apart, never consumes the plan.
    printf '%s\n' "$url" >> "$STUB_LOG.status"
    [[ -z "$out" ]] || printf '{"sd_flush": {"drained": true}}' > "$out"
    printf '200'
    exit 0
fi
printf '%s %s\n' "${method:-GET}" "$url" >> "$STUB_LOG.methods"
n=0
[[ -f "$STUB_COUNT" ]] && n=$(<"$STUB_COUNT")
n=$((n + 1))
printf '%s' "$n" > "$STUB_COUNT"
printf '%s\n' "$url" >> "$STUB_LOG"
mapfile -t plan < "$STUB_PLAN"
idx=$((n - 1))
((idx < ${#plan[@]})) || idx=$((${#plan[@]} - 1))
read -r code ra <<< "${plan[$idx]}"
if [[ -n "$hdr" ]]; then
    { printf 'HTTP/1.1 %s X\r\n' "$code"; [[ -z "${ra:-}" ]] || printf 'Retry-After: %s\r\n' "$ra"; printf '\r\n'; } > "$hdr"
fi
[[ -z "$out" ]] || printf '{"ok":true}' > "$out"
printf '%s' "$code"
"""

if __name__ == "__main__":
    unittest.main()
