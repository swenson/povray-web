POV-Ray for the Web
===================

This directory contains a WebAssembly build of POV-Ray and a small browser
front-end for it. POV-Ray itself is compiled unchanged from `source/`, using
the Unix platform layer (`platform/unix`, `vfe/unix`) and the libraries
bundled in `libraries/` (zlib, libpng, libjpeg).

Features:

- Edit scenes in the browser, render them with all CPU cores, and watch a
  live preview while rendering.
- Open scene files from local disk: one or more files, a whole folder, or by
  drag and drop. Include files, image maps, fonts, etc. opened alongside the
  scene are available to it.
- Save scenes back to disk (in place, where the browser supports the File
  System Access API), save the rendered image, and download any other files
  the scene writes (animation frames, `#fopen` output).
- Browse and render all example scenes from `distribution/scenes`. Files an
  example needs from its directory are only downloaded when POV-Ray opens them.

Building
--------

Install and activate the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html),
then run the build script from anywhere:

    source /path/to/emsdk/emsdk_env.sh
    web/build.sh

The site is assembled in `build-web/site/`. Extra arguments are passed to
CMake, e.g. `web/build.sh -DPOVWEB_BUILT_BY="Your Name"`.

To build manually:

    emcmake cmake -S web -B build-web -DCMAKE_BUILD_TYPE=Release
    cmake --build build-web -j

Running
-------

POV-Ray uses threads, which in the browser need `SharedArrayBuffer`, which in
turn is only available when the page is *cross-origin isolated*: it must be
served with

    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp

`web/tools/serve.py` serves the site with these headers:

    python3 web/tools/serve.py          # http://127.0.0.1:8000/

On hosts that cannot set headers (e.g. GitHub Pages), the page registers
`coi-serviceworker.js`, which adds them and reloads the page once. This
requires HTTPS (or `localhost`).

Deploying to GitHub Pages
-------------------------

The workflow `.github/workflows/web_pages.yml` builds the site on every pull
request into the `gh-pages` branch, and builds and deploys it on every push
to `gh-pages` (or when run manually on that branch). One-time setup in the
repository settings:

1. *Settings > Pages > Build and deployment > Source*: select **GitHub Actions**.
2. *Settings > Environments > github-pages > Deployment branches and tags*:
   allow the `gh-pages` branch (by default only the default branch may deploy).

The site is then published at `https://<owner>.github.io/<repository>/`.

Static hosts cache files independently, so `web/tools/stamp_version.py`
stamps a hash of the build into `index.html`, and the page appends it to the
URLs of all code and data files. A deployment therefore never mixes files
from two builds.

How it works
------------

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt` | Emscripten build of POV-Ray and the bundled libraries; assembles the site. |
| `config/config.h` | Replaces the autoconf-generated `config.h` of the Unix build. |
| `etc/` | `povray.ini` / `povray.conf`, packaged into the virtual file system at `/povray/etc`. |
| `src/webconsole.cpp` | Entry point, replacing `vfe/unix/unixconsole.cpp`. |
| `shell/` | The web page (`index.html`, `app.js`, `style.css`) and workers. |
| `tools/make_manifest.py` | Copies the example scenes into the site and writes `scenes.json`. |
| `tools/stamp_version.py` | Writes `index.html` with the build version stamped into asset URLs. |
| `tools/serve.py` | Local web server with the required headers. |

- The page starts one long-lived worker (`render-worker.js`) that hosts the
  WebAssembly module. POV-Ray's `main()` runs on a pthread
  (`-sPROXY_TO_PTHREAD`) and loops, waiting for render jobs. Each render
  hands it a list of command-line arguments plus the scene files, and
  receives the output files back when the render ends.
- The instance is deliberately reused rather than created per render:
  browsers do not reliably reclaim the large shared memory each instance
  reserves, so creating one per render eventually fails with "could not
  allocate memory". The worker is only replaced if POV-Ray crashes or a
  render does not stop when asked.
- Standard include files and INI files are preloaded into the virtual file
  system (`povray.data`). Example scenes are mounted as lazy files under
  `/povray/scenes`; files opened from disk are written to `/work`. Render
  output goes to `/out`.
- The preview is drawn from an RGBA framebuffer in shared WebAssembly memory,
  which the worker polls and forwards to the page.

Limitations
-----------

- TIFF and OpenEXR images are not supported (the libraries are not bundled).
- Shell-outs (`Pre_Scene_Command` etc.) are not available.
- Rendering is slower than a native build.
- Memory is limited to 4 GB.
