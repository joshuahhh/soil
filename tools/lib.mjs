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
  if (shot.tunnel) q.set('tunnel', String(shot.tunnel)); // tunnel mode, circumference in m
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

// A/B comparison page: a list of image pairs with one mode selector for the
// whole page (side by side / wipe / blink / diff). shimmer.mjs uses it; the
// golden report has its own copy with extra per-card machinery.
//
// cards: [{ name, note, badges: [{cls, text}], a: {label, uri}, b: {label, uri} }]
export function comparisonPage({ title, intro, defaultMode = 'blink', cards, extraHtml = '' }) {
  return `<title>${title}</title>
<style>${STYLE}
  :root { --imgmax: calc(100vh - 260px); }
  .stage img { max-width:100%; max-height:var(--imgmax); width:auto; height:auto; margin:0 auto; }
  .stage .pane { display:flex; flex-direction:column; align-items:center; }
  .stage .pane h3 { align-self:stretch; }
  .wipe { position:relative; display:inline-block; line-height:0; cursor:col-resize; touch-action:none; }
  .wipe .top { position:absolute; top:0; left:0; bottom:0; width:var(--pos,50%); overflow:hidden; }
  .wipe .top img { position:absolute; top:0; left:0; max-width:none; max-height:none;
                   width:var(--w); height:var(--h); }
  .wipe .line { position:absolute; top:0; bottom:0; left:var(--pos,50%); width:2px; margin-left:-1px;
                background:var(--accent); pointer-events:none; box-shadow:0 0 0 1px rgba(0,0,0,.5); }
  .wipe .tag { position:absolute; top:8px; font:11px ui-monospace,Menlo,monospace;
               background:rgba(0,0,0,.6); color:#fff; padding:2px 7px; border-radius:3px; pointer-events:none; }
  .wipe .tag.l { left:8px; } .wipe .tag.r { right:8px; }
  table.metric { border-collapse:collapse; margin:0; font:12px ui-monospace,Menlo,monospace; }
  table.metric th { text-align:right; padding:4px 14px; color:var(--dim); font-weight:600;
                    border-bottom:1px solid var(--line); }
  table.metric th:first-child { text-align:left; }
  table.metric td { text-align:right; padding:4px 14px; }
  table.metric td:first-child { text-align:left; color:var(--dim); }
  table.metric tr.best td { color:var(--ok); }
</style>
<header>
  <h1>${title}</h1>
  <span class="sub">${intro}</span>
  <span class="modes" id="modes">
    <button data-mode="side">side by side</button>
    <button data-mode="wipe">wipe</button>
    <button data-mode="blink">blink</button>
    <button data-mode="heat">diff</button>
  </span>
</header>
${extraHtml}
<main id="main"></main>
<script type="module">
const CARDS = ${JSON.stringify(cards)};
const DEFAULT_MODE = ${JSON.stringify(defaultMode)};

// one blink clock for the page, so cards flip together rather than drifting
let blinkOn = false;
const blinkSubs = new Set();
setInterval(() => { blinkOn = !blinkOn; blinkSubs.forEach((f) => f(blinkOn)); }, 500);

const load = (uri) => new Promise((r) => { const i = new Image(); i.onload = () => r(i); i.src = uri; });
const main = document.getElementById('main');
const rendered = [];

for (const c of CARDS) {
  const el = document.createElement('div');
  el.className = 'card';
  el.innerHTML = \`<div class="head"><span class="name">\${c.name}</span>
      \${(c.badges || []).map((b) => \`<span class="badge \${b.cls || ''}">\${b.text}</span>\`).join('')}
    </div>
    \${c.note ? \`<div class="note">\${c.note}</div>\` : ''}
    <div class="stage"></div>\`;
  main.appendChild(el);
  const stage = el.querySelector('.stage');

  const [ia, ib] = await Promise.all([load(c.a.uri), load(c.b.uri)]);
  const W = ia.naturalWidth, H = ia.naturalHeight;
  let heatUri = '';
  if (ib.naturalWidth === W && ib.naturalHeight === H) {
    const px = (img) => { const cv = new OffscreenCanvas(W, H);
      const x = cv.getContext('2d', { willReadFrequently: true }); x.drawImage(img, 0, 0);
      return x.getImageData(0, 0, W, H).data; };
    const pa = px(ia), pb = px(ib);
    const heat = new OffscreenCanvas(W, H), hx = heat.getContext('2d');
    const out = hx.createImageData(W, H);
    for (let i = 0; i < pa.length; i += 4) {
      const d = Math.max(Math.abs(pa[i]-pb[i]), Math.abs(pa[i+1]-pb[i+1]), Math.abs(pa[i+2]-pb[i+2]));
      const t = Math.min(1, d / 64);
      out.data[i] = 20 + t * 235; out.data[i+1] = 20 + (1-t) * 40;
      out.data[i+2] = 24 + (1-t) * 40; out.data[i+3] = 255;
    }
    hx.putImageData(out, 0, 0);
    heatUri = URL.createObjectURL(await heat.convertToBlob({ type: 'image/png' }));
  }

  let cleanup = null;
  const render = (mode) => {
    if (cleanup) { cleanup(); cleanup = null; }
    stage.classList.toggle('side', mode === 'side');
    if (mode === 'side') {
      stage.innerHTML = \`<div class="pane"><h3>\${c.a.label}</h3><img src="\${c.a.uri}"></div>
                         <div class="pane"><h3>\${c.b.label}</h3><img src="\${c.b.uri}"></div>\`;
    } else if (mode === 'heat') {
      stage.innerHTML = heatUri
        ? \`<div class="pane"><h3>changed pixels</h3><img src="\${heatUri}"></div>\`
        : '<div class="missing">sizes differ</div>';
    } else if (mode === 'blink') {
      stage.innerHTML = \`<div class="pane"><h3 data-l></h3><img data-b></div>\`;
      const img = stage.querySelector('[data-b]'), lab = stage.querySelector('[data-l]');
      const sub = (on) => { img.src = on ? c.b.uri : c.a.uri; lab.textContent = on ? c.b.label : c.a.label; };
      blinkSubs.add(sub); sub(blinkOn);
      cleanup = () => blinkSubs.delete(sub);
    } else {
      stage.innerHTML = \`<div class="pane"><h3>drag across the image</h3>
        <div class="wipe"><img class="base" src="\${c.a.uri}">
          <div class="top"><img src="\${c.b.uri}"></div><div class="line"></div>
          <span class="tag l">\${c.a.label}</span><span class="tag r">\${c.b.label}</span>
        </div></div>\`;
      const wipe = stage.querySelector('.wipe'), base = stage.querySelector('.base');
      const sync = () => { wipe.style.setProperty('--w', base.clientWidth + 'px');
                           wipe.style.setProperty('--h', base.clientHeight + 'px'); };
      const ro = new ResizeObserver(sync); ro.observe(base); sync();
      const move = (e) => { const r = base.getBoundingClientRect();
        wipe.style.setProperty('--pos', Math.max(0, Math.min(r.width, e.clientX - r.left)) + 'px'); };
      wipe.addEventListener('pointermove', move);
      wipe.addEventListener('pointerdown', move);
      cleanup = () => ro.disconnect();
    }
  };
  rendered.push(render);
  render(DEFAULT_MODE);
}

const fit = () => {
  const hdr = document.querySelector('header').offsetHeight;
  const extra = document.querySelector('.metricbox')?.offsetHeight || 0;
  let chrome = 0;
  for (const card of document.querySelectorAll('.card')) {
    const img = card.querySelector('.stage img');
    if (img) chrome = Math.max(chrome, card.offsetHeight - img.getBoundingClientRect().height);
  }
  document.documentElement.style.setProperty('--imgmax',
    Math.max(180, innerHeight - hdr - extra - chrome - 56) + 'px');
};

document.querySelectorAll('#modes button').forEach((b) => {
  b.setAttribute('aria-pressed', String(b.dataset.mode === DEFAULT_MODE));
  b.onclick = () => {
    document.querySelectorAll('#modes button')
      .forEach((o) => o.setAttribute('aria-pressed', String(o === b)));
    for (const r of rendered) r(b.dataset.mode);
    fit();
  };
});
fit();
addEventListener('resize', fit);
window.__ready = true;
</script>`;
}
