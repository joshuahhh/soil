// shared bits between the two harnesses:
//   capture.mjs   — one browser, against blessed references: "did my change
//                   alter rendering?"
//   browsers.mjs  — every browser, against each other in the same run: "does
//                   it still render right everywhere?"
// they ask different questions, so they compare against different things, but
// they drive the page identically — that's what lives here.
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export const HERE = path.dirname(fileURLToPath(import.meta.url));
export const OUT = path.join(HERE, 'out');

export async function readShots(only = []) {
  const all = JSON.parse(await fs.readFile(path.join(HERE, 'shots.json'), 'utf8'));
  return only.length ? all.filter((s) => only.includes(s.name)) : all;
}

export function shotQuery(shot) {
  const q = new URLSearchParams({ test: '1' });
  for (const k of ['lat', 'lon', 'alt', 'heading', 'tilt', 'fov'])
    if (shot[k] !== undefined) q.set(k, String(shot[k]));
  if (shot.ortho) q.set('ortho', '1');
  if (shot.hug) q.set('hug', '1');
  if (shot.jpg) q.set('jpg', '1'); // force the no-s3tc texture path
  return q;
}

// drive one page to a settled frame and read everything back. always returns a
// png, even on timeout — a stuck tile is a normal outcome (a failed download
// resets its node to a stub and is retried forever, so sceneComplete can never
// arrive), and a half-loaded frame is still evidence as long as it's labelled
export async function captureShot(page, base, shot, { timeoutMs = 120000 } = {}) {
  const errors = [];
  page.on('pageerror', (e) => errors.push('PAGEERROR ' + e.message.split('\n')[0]));
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text().split('\n')[0]); });

  const t0 = Date.now();
  let settled = { complete: false, ms: 0, stats: null }, dataUrl = null, nodes = [], env = null;
  try {
    await page.goto(`${base}/index.html?${shotQuery(shot)}`, { waitUntil: 'load' });
    await page.waitForFunction('window.__test && window.__test.stats() !== null',
      null, { timeout: 60000 });
    settled = await page.evaluate((ms) => window.__test.settle({ timeoutMs: ms }), timeoutMs);
    dataUrl = await page.evaluate(() => window.__test.shot());
    nodes = await page.evaluate(() => window.__test.drawnNodes());
    // what rasterised this, plus the two capability branches the engine
    // switches on: s3tc picks compressed vs jpeg textures (rocktree_gl
    // renderInit), highp picks the fragment precision branch
    env = await page.evaluate(() => {
      // ask for the context the engine actually created: once a canvas holds a
      // webgl2 context, getContext('webgl') returns null, and probing the wrong
      // type silently reports "no s3tc, no highp" for a canvas that has both
      const c = document.getElementById('globe');
      const gl = c.getContext('webgl2') || c.getContext('webgl');
      const d = gl && gl.getExtension('WEBGL_debug_renderer_info');
      return {
        renderer: gl ? (d ? gl.getParameter(d.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER)) : null,
        contextVersion: gl ? (typeof WebGL2RenderingContext !== 'undefined'
          && gl instanceof WebGL2RenderingContext ? 2 : 1) : null,
        s3tc: !!(gl && gl.getExtension('WEBGL_compressed_texture_s3tc')),
        highp: !!(gl && gl.getShaderPrecisionFormat(gl.FRAGMENT_SHADER, gl.HIGH_FLOAT).precision),
        webgl2: !!document.createElement('canvas').getContext('webgl2'),
      };
    });
  } catch (e) {
    errors.push('DRIVER ' + e.message.split('\n')[0]);
  }

  const st = settled.stats || {};
  return {
    png: dataUrl ? Buffer.from(dataUrl.split(',')[1], 'base64') : null,
    meta: {
      name: shot.name, note: shot.note, view: shot,
      env,
      // a shot that never produced a frame is the loudest possible signal;
      // don't let it look like a merely-incomplete one
      rendered: !!dataUrl,
      complete: settled.complete,
      settleMs: Math.round(settled.ms),
      wallMs: Date.now() - t0,
      nodesWanted: st.nodesWanted, nodesLoaded: st.nodesLoaded, nodesDrawn: st.nodesDrawn,
      timings: { bfsMs: st.bfsMs, dlMs: st.dlMs, evictMs: st.evictMs, drawMs: st.drawMs, fps: st.fps },
      counters: { octantsWalked: st.octantsWalked, nodesCulled: st.nodesCulled, nodesLodTested: st.nodesLodTested },
      drawnNodes: nodes.map((n) => `${n.path} ${n.mask.toString(16).padStart(2, '0')}`),
      errors: [...new Set(errors)].slice(0, 20),
    },
  };
}

// shared page chrome for both reports
export const STYLE = `
  :root { --bg:#15171a; --panel:#1e2126; --line:#2f343b; --fg:#e6e9ee; --dim:#98a2b0;
          --ok:#3fb950; --warn:#d29922; --bad:#f85149; --accent:#58a6ff; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--fg);
         font:14px/1.5 ui-sans-serif,-apple-system,system-ui,sans-serif; }
  header { padding:20px 24px; border-bottom:1px solid var(--line); position:sticky; top:0;
           background:var(--bg); z-index:10; display:flex; gap:20px; align-items:baseline;
           flex-wrap:wrap; }
  h1 { font-size:16px; margin:0; font-weight:600; letter-spacing:.01em; }
  .sub { color:var(--dim); font-size:13px; }
  main { padding:24px; display:flex; flex-direction:column; gap:24px; }
  .card { background:var(--panel); border:1px solid var(--line); border-radius:8px; overflow:hidden; }
  .head { padding:12px 16px; display:flex; gap:10px; align-items:center; flex-wrap:wrap;
          border-bottom:1px solid var(--line); }
  .name { font-weight:600; font-family:ui-monospace,SFMono-Regular,Menlo,monospace; }
  .badge { font-size:11px; padding:2px 8px; border-radius:999px; border:1px solid var(--line);
           color:var(--dim); font-family:ui-monospace,Menlo,monospace; white-space:nowrap; }
  .badge.ok { color:var(--ok); border-color:#23502f; }
  .badge.warn { color:var(--warn); border-color:#5a4415; }
  .badge.bad { color:var(--bad); border-color:#6b2226; }
  .note { padding:8px 16px; color:var(--dim); font-size:12.5px; border-bottom:1px solid var(--line); }
  .modes { margin-left:auto; display:flex; gap:2px; }
  .modes button { background:#262b31; color:var(--dim); border:1px solid var(--line);
                  padding:4px 11px; font:inherit; font-size:12px; cursor:pointer; }
  .modes button:first-child { border-radius:5px 0 0 5px; }
  .modes button:last-child { border-radius:0 5px 5px 0; }
  .modes button[aria-pressed="true"] { background:var(--accent); color:#0b1117; border-color:var(--accent); }
  .stage { padding:16px; display:grid; gap:12px; }
  .stage.side { grid-template-columns:1fr 1fr; }
  .stage.cols3 { grid-template-columns:repeat(3,1fr); }
  .pane { min-width:0; }
  .pane h3 { margin:0 0 6px; font-size:11px; text-transform:uppercase; letter-spacing:.08em;
             color:var(--dim); font-weight:600; display:flex; gap:8px; align-items:center; }
  img, canvas { width:100%; display:block; border-radius:4px; background:#0b0d10; }
  .wrap { position:relative; }
  .wrap .over { position:absolute; inset:0; overflow:hidden; }
  .wrap .over img { position:absolute; top:0; left:0; height:100%; width:auto; max-width:none; }
  input[type=range] { width:100%; margin-top:10px; accent-color:var(--accent); }
  .nodes { padding:0 16px 16px; font-size:12px; font-family:ui-monospace,Menlo,monospace; color:var(--dim); }
  .nodes details { margin-top:6px; }
  .nodes summary { cursor:pointer; }
  .nodes pre { margin:6px 0 0; padding:10px; background:#0f1215; border-radius:5px;
               max-height:220px; overflow:auto; font-size:11.5px; color:var(--fg); }
  .missing { padding:28px 16px; color:var(--dim); text-align:center; }
  table.t { border-collapse:collapse; font-size:11.5px; font-family:ui-monospace,Menlo,monospace; }
  table.t td { padding:1px 12px 1px 0; color:var(--dim); vertical-align:top; }
  table.t td.v { color:var(--fg); }
`;
