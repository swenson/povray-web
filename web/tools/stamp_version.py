#!/usr/bin/env python3
"""Write the site's index.html with a build version stamped into asset URLs.

Static hosts cache files independently (GitHub Pages: 10 minutes), so right
after a deployment a browser could combine a new `povray.js` with a cached,
old `povray.wasm`. To avoid that, every `@POVWEB_VERSION@` placeholder in
the index.html template is replaced by a hash of the site's code and data
files; the page appends it as a query string to everything it loads.
"""

import argparse
import hashlib
import os

# Files whose content defines a build. Example scenes are excluded: they
# change rarely and are fetched on demand.
VERSIONED_FILES = [
    "povray.js", "povray.wasm", "povray.data",
    "app.js", "render-worker.js", "style.css", "scenes.json",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", required=True, help="index.html template")
    parser.add_argument("--site", required=True, help="site output directory")
    args = parser.parse_args()

    digest = hashlib.sha256()
    for name in VERSIONED_FILES:
        with open(os.path.join(args.site, name), "rb") as f:
            digest.update(name.encode() + b"\0")
            digest.update(f.read())
    version = digest.hexdigest()[:12]

    with open(args.template, "r", encoding="utf-8") as f:
        html = f.read()
    with open(os.path.join(args.site, "index.html"), "w", encoding="utf-8") as f:
        f.write(html.replace("@POVWEB_VERSION@", version))


if __name__ == "__main__":
    main()
