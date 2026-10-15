// POV-Ray render worker.
//
// Each render runs in a fresh instance of this worker, which hosts the
// Emscripten module (`povray.js`/`povray.wasm`). POV-Ray's `main()` runs on a
// pthread (PROXY_TO_PTHREAD), so this worker's own event loop stays free to
// service file-system calls and to poll the preview framebuffer.
//
// Emscripten starts its pthread workers from this same script (it uses
// `self.location` as the script URL), so a worker named "em-pthread" only
// needs to load the module; `povray.js` bootstraps itself in that case.

'use strict';

importScripts('povray.js');

if (self.name !== 'em-pthread') {
  let povray = null;        // Emscripten module instance
  let previewTimer = 0;
  let lastSerial = -1;
  let finished = false;
  let workDir = '/';
  const inputPaths = new Set();  // files present before the render started

  const post = (msg, transfer) => self.postMessage(msg, transfer || []);

  self.onmessage = (event) => {
    const msg = event.data;
    if (msg.type === 'render') {
      render(msg).catch((err) => {
        post({ type: 'error', message: String(err && err.stack || err) });
      });
    } else if (msg.type === 'cancel') {
      if (povray) povray._povweb_cancel();
    }
  };

  // Lines prefixed with \x01 are structured events from webconsole.cpp.
  function handleStdout(line) {
    if (line.charCodeAt(0) === 1) {
      const space = line.indexOf(' ');
      const kind = line.slice(1, space < 0 ? undefined : space);
      const rest = space < 0 ? '' : line.slice(space + 1);
      if (kind === 'STATUS') post({ type: 'status', text: rest });
      else if (kind === 'STATE') post({ type: 'state', state: rest });
      else if (kind === 'FRAME') {
        const [current, total] = rest.split(' ').map(Number);
        post({ type: 'frame', current, total });
      }
      return;
    }
    post({ type: 'log', stream: 'stdout', text: line });
  }

  function mkdirTree(FS, path) {
    let cur = '';
    for (const part of path.split('/')) {
      if (!part) continue;
      cur += '/' + part;
      try { FS.mkdir(cur); } catch (e) { /* exists */ }
    }
  }

  function writeFile(FS, path, data) {
    const dir = path.slice(0, path.lastIndexOf('/')) || '/';
    mkdirTree(FS, dir);
    try { FS.unlink(path); } catch (e) { /* did not exist (or was a lazy file) */ }
    FS.writeFile(path, typeof data === 'string' ? data : new Uint8Array(data));
  }

  function pollPreview(force) {
    if (!povray) return;
    const serial = povray._povweb_frame_serial();
    if (!force && serial === lastSerial) return;
    const ptr = povray._povweb_framebuffer();
    const width = povray._povweb_width();
    const height = povray._povweb_height();
    if (!ptr || !width || !height) return;
    lastSerial = serial;
    // Copy out of the shared heap; ImageData cannot wrap shared memory.
    const pixels = new Uint8ClampedArray(povray.wasmMemory.buffer, ptr, width * height * 4).slice();
    post({ type: 'preview', width, height, pixels: pixels.buffer }, [pixels.buffer]);
  }

  // Gather regular files under `dir` (recursively), skipping the inputs.
  function collectFiles(FS, dir, prefix, skip, out) {
    let names = [];
    try { names = FS.readdir(dir); } catch (e) { return out; }
    for (const name of names.sort()) {
      // Skip POV-Ray's internal continue-trace state file.
      if (name === '.' || name === '..' || name.endsWith('.pov-state')) continue;
      const path = dir + '/' + name;
      // Never stat inputs: stat'ing a lazy file would download it.
      if (skip.has(path)) continue;
      const mode = FS.stat(path).mode;
      if (FS.isDir(mode)) {
        collectFiles(FS, path, prefix + name + '/', skip, out);
      } else if (FS.isFile(mode)) {
        out.push({ name: prefix + name, data: FS.readFile(path).buffer });
      }
    }
    return out;
  }

  function finish(code) {
    if (finished) return;
    finished = true;
    clearInterval(previewTimer);
    pollPreview(true);
    let outputs = [];
    try {
      // Rendered images, then any files the scene itself wrote (#fopen, etc.).
      collectFiles(povray.FS, '/out', '', inputPaths, outputs);
      collectFiles(povray.FS, workDir, '', inputPaths, outputs);
    } catch (e) {
      post({ type: 'log', stream: 'stderr', text: 'Could not read output files: ' + e });
    }
    post({ type: 'done', code, outputs }, outputs.map((o) => o.data));
  }

  async function render(msg) {
    const base = new URL('.', self.location.href).href;

    povray = await createPovray({
      print: handleStdout,
      printErr: (text) => post({ type: 'log', stream: 'stderr', text }),
      locateFile: (path) => base + path,
      // Keep the worker alive after main() returns; we report the result instead of throwing.
      quit: (status) => finish(status),
      onExit: (status) => finish(status),
      onAbort: (what) => {
        post({ type: 'log', stream: 'stderr', text: 'POV-Ray aborted: ' + what });
        finish(-1);
      },
      preRun: [(Module) => {
        const FS = Module.FS;

        // Example scenes, fetched on demand when POV-Ray opens them.
        for (const file of msg.lazyFiles || []) {
          const path = file.path;
          const slash = path.lastIndexOf('/');
          const dir = path.slice(0, slash);
          mkdirTree(FS, dir);
          try {
            FS.createLazyFile(dir, path.slice(slash + 1), file.url, true, false);
          } catch (e) { /* duplicate */ }
          inputPaths.add(path);
        }

        // Files of the current project (edited scene text, user-supplied assets).
        for (const file of msg.files || []) {
          writeFile(FS, file.path, file.data);
          inputPaths.add(file.path);
        }

        // An (empty) user configuration file keeps POV-Ray from warning about it.
        writeFile(FS, '/home/web_user/.povray/3.8/povray.conf', '');

        mkdirTree(FS, '/out');
        mkdirTree(FS, msg.cwd);
        FS.chdir(msg.cwd);
        workDir = msg.cwd;
      }],
    });

    post({ type: 'started' });
    previewTimer = setInterval(pollPreview, 100);
    povray.callMain(msg.args);
  }
}
