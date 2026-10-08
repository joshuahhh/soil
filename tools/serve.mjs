// static file server for web/, used by the capture drivers. serves on an
// ephemeral port and sets the cross-origin isolation headers directly, so the
// pthread build gets SharedArrayBuffer without coi-serviceworker's
// register-and-reload cycle.
//
// require-corp rather than the credentialless that coi-serviceworker asks for:
// webkit throws on the wasm fetch under credentialless (and then recovers, so
// it only shows up as console noise), while all three engines are clean under
// require-corp. cross-origin tiles still load because kh.google.com answers
// the page's cors-mode fetches with cors headers, which satisfies require-corp
// on its own — no CORP header needed on their end
import http from 'node:http';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const WEB_SRC = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'web');
const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wasm': 'application/wasm', '.json': 'application/json', '.png': 'image/png',
  '.css': 'text/css', '.map': 'application/json',
};

// the fixed port the warm-cache tools share (loadprof --warm, shot): the
// indexeddb tile cache is keyed by origin, so it has to stay put. high and
// unremarkable, because a dev server of anything else may sit on a round one
export const WARM_PORT = 18765;

// port 0 (the default) is an ephemeral one, which means a different origin —
// and a different indexeddb — every run; a caller that wants the tile cache
// to carry over between runs passes a fixed one
export async function serveWeb({ port = 0 } = {}) {
  // serve a snapshot of web/, not web/ itself: a capture runs for minutes,
  // and a rebuild in the meantime used to swap earth.wasm under it — so
  // either the build waited for the capture or the capture ran a mix of
  // two engines. the copy is a few mb and lives in the temp dir for the
  // run; close() removes it
  const WEB = await fs.mkdtemp(path.join(os.tmpdir(), 'soil-web-'));
  await fs.cp(WEB_SRC, WEB, { recursive: true });
  const server = http.createServer(async (req, res) => {
    const rel = decodeURIComponent(new URL(req.url, 'http://x').pathname);
    const file = path.join(WEB, rel === '/' ? 'index.html' : rel);
    // don't let a crafted path escape web/
    if (!file.startsWith(WEB)) { res.writeHead(403).end(); return; }
    try {
      const body = await fs.readFile(file);
      res.writeHead(200, {
        'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream',
        'Cache-Control': 'no-store',
        'Cross-Origin-Opener-Policy': 'same-origin',
        'Cross-Origin-Embedder-Policy': 'require-corp',
        'Cross-Origin-Resource-Policy': 'same-origin',
      });
      res.end(body);
    } catch {
      res.writeHead(404).end('not found');
    }
  });
  await new Promise((r) => server.listen(port, '127.0.0.1', r));
  return {
    base: `http://127.0.0.1:${server.address().port}`,
    close: () => { server.close(); fs.rm(WEB, { recursive: true, force: true }).catch(() => {}); },
  };
}
