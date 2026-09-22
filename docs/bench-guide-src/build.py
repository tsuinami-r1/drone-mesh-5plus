#!/usr/bin/env python3
"""Build docs/Level1-Station-v3-C5PHY-Bench-Guide.pdf from its HTML source.

The HTML template next to this script carries <!--INLINE:file.svg--> markers
that are replaced with the diagrams from docs/, so the PDF and the diagrams
in the Markdown docs never drift apart. The page is then printed with a
headless Chromium (the same tool that produced the v2 guide).

    python3 docs/bench-guide-src/build.py [--chrome /path/to/chrome]

Chromium is found from --chrome, $CHROME, or the usual names on PATH and the
Playwright browser cache.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DOCS = os.path.dirname(HERE)
TEMPLATE = os.path.join(HERE, "Level1-Station-v3-C5PHY-Bench-Guide.html")
OUTPUT = os.path.join(DOCS, "Level1-Station-v3-C5PHY-Bench-Guide.pdf")


def find_chrome(explicit):
    candidates = [explicit, os.environ.get("CHROME")]
    candidates += [shutil.which(n) for n in ("chromium", "chromium-browser", "google-chrome", "chrome")]
    candidates += sorted(glob.glob(os.path.expanduser("~/.cache/ms-playwright/chromium-*/chrome-linux/chrome")))
    candidates += sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux/chrome"))
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return None


def inline_svgs(html):
    marker = "<!--INLINE:"
    while marker in html:
        start = html.index(marker)
        end = html.index("-->", start)
        name = html[start + len(marker):end].strip()
        with open(os.path.join(DOCS, name), encoding="utf-8") as f:
            svg = f.read()
        html = html[:start] + svg + html[end + 3:]
    return html


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chrome", help="Chromium/Chrome executable")
    ap.add_argument("--keep-html", help="also write the self-contained HTML here")
    args = ap.parse_args()

    chrome = find_chrome(args.chrome)
    if not chrome:
        sys.exit("no Chromium found: pass --chrome or set $CHROME")

    with open(TEMPLATE, encoding="utf-8") as f:
        html = inline_svgs(f.read())

    with tempfile.TemporaryDirectory() as tmp:
        page = os.path.join(tmp, "guide.html")
        with open(page, "w", encoding="utf-8") as f:
            f.write(html)
        if args.keep_html:
            with open(args.keep_html, "w", encoding="utf-8") as f:
                f.write(html)
        cmd = [
            chrome, "--headless", "--no-sandbox", "--disable-gpu",
            "--no-pdf-header-footer", "--print-to-pdf=" + OUTPUT, "file://" + page,
        ]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(OUTPUT):
            sys.exit("chromium failed:\n" + r.stderr)
    print("wrote", OUTPUT, os.path.getsize(OUTPUT), "bytes")


if __name__ == "__main__":
    main()
