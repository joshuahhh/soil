// builds tools/out/report.html: every shot as reference-vs-current, sorted so
// the most-changed lands at the top.
//
//   node tools/report.mjs           # build the report, print a summary
//   node tools/report.mjs --open    # ...and open it
//
// the pixel diff is triage, not a verdict. it can't tell "google reshot the
// imagery" from "we broke the shader" — both just move pixels — so its only
// job is ordering the cards so a human looks at the suspicious ones first.
// the two signals that *are* categorical sit next to it: the drawn-node set
// (pure lod/culling/eviction output, immune to imagery churn and to gpu
// dxt-decode differences) and the load-completeness badge.
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.join(HERE, 'out');
const CUR = path.join(OUT, 'current');
const REF = path.join(HERE, 'reference');

const readShot = async (dir, name) => {
  try {
    const meta = JSON.parse(await fs.readFile(path.join(dir, name + '.json'), 'utf8'));
    const png = await fs.readFile(path.join(dir, name + '.png'));
    return { meta, uri: 'data:image/png;base64,' + png.toString('base64') };
  } catch { return null; }
};

const names = [...new Set([
  ...(await fs.readdir(CUR).catch(() => [])),
  ...(await fs.readdir(REF).catch(() => [])),
].filter((f) => f.endsWith('.png')).map((f) => f.slice(0, -4)))].sort();

if (!names.length) {
  console.error('nothing to report on — run `node tools/capture.mjs` first');
  process.exit(1);
}

const cards = [];
for (const name of names) {
  const cur = await readShot(CUR, name);
  const ref = await readShot(REF, name);
  // the drawn-node set diff is plain text, so it's computed here rather than
  // in the page: these are the nodes lod/culling actually selected
  let added = [], removed = [], maskChanged = [];
  if (cur && ref) {
    const parse = (m) => new Map((m.meta.drawnNodes || []).map((r) => r.split(' ')));
    const a = parse(ref), b = parse(cur);
    for (const [p, m] of b) {
      if (!a.has(p)) added.push(p);
      else if (a.get(p) !== m) maskChanged.push(`${p} ${a.get(p)}->${m}`);
    }
    for (const p of a.keys()) if (!b.has(p)) removed.push(p);
  }
  cards.push({ name, cur, ref, added, removed, maskChanged });
}

const html = `<title>soil render report</title>
<style>
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
  .card { background:var(--panel); border:1px solid var(--line); border-radius:8px;
          overflow:hidden; }
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
  .pane { min-width:0; }
  .pane h3 { margin:0 0 6px; font-size:11px; text-transform:uppercase; letter-spacing:.08em;
             color:var(--dim); font-weight:600; }
  img, canvas { width:100%; display:block; border-radius:4px; background:#0b0d10; }
  .wrap { position:relative; }
  .wrap .over { position:absolute; inset:0; overflow:hidden; }
  .wrap .over img { position:absolute; top:0; left:0; height:100%; width:auto; max-width:none; }
  input[type=range] { width:100%; margin-top:10px; accent-color:var(--accent); }
  .nodes { padding:0 16px 16px; font-size:12px; font-family:ui-monospace,Menlo,monospace;
           color:var(--dim); }
  .nodes details { margin-top:6px; }
  .nodes summary { cursor:pointer; }
  .nodes pre { margin:6px 0 0; padding:10px; background:#0f1215; border-radius:5px;
               max-height:220px; overflow:auto; font-size:11.5px; color:var(--fg); }
  .missing { padding:28px 16px; color:var(--dim); text-align:center; }
  table.t { border-collapse:collapse; font-size:11.5px; font-family:ui-monospace,Menlo,monospace; }
  table.t td { padding:1px 12px 1px 0; color:var(--dim); }
  table.t td.v { color:var(--fg); }
</style>
<header>
  <h1>soil render report</h1>
  <span class="sub" id="summary">measuring…</span>
  <span class="sub">pixel diff orders the list; it does not judge. check the node-set and tile badges.</span>
</header>
<main id="main"></main>
<script type="module">
const CARDS = ${JSON.stringify(cards)};
const scores = window.__scores = {};

const badge = (cls, text) => \`<span class="badge \${cls}">\${text}</span>\`;
// a golden is only comparable against a capture from the same rasteriser:
// swiftshader, angle/metal and a mobile gpu each decode dxt and round
// interpolation differently, so a renderer change moves pixels on its own
const envMismatch = (c) => c.ref?.meta?.env?.renderer && c.cur?.meta?.env?.renderer
  && c.ref.meta.env.renderer !== c.cur.meta.env.renderer;
const metaBadges = (m, label) => {
  if (!m) return '';
  const loaded = m.nodesLoaded === m.nodesWanted;
  return badge(m.complete && loaded ? 'ok' : 'bad',
      \`\${label} \${m.nodesLoaded}/\${m.nodesWanted} tiles\${m.complete ? '' : ' · TIMED OUT'}\`);
};

const main = document.getElementById('main');
for (const c of CARDS) {
  const el = document.createElement('div');
  el.className = 'card';
  el.dataset.name = c.name;
  const m = c.cur?.meta || c.ref?.meta;
  const nodeDelta = c.added.length + c.removed.length + c.maskChanged.length;
  el.innerHTML = \`
    <div class="head">
      <span class="name">\${c.name}</span>
      \${!c.ref ? badge('warn', 'no reference') : ''}
      \${!c.cur ? badge('warn', 'no current capture') : ''}
      \${metaBadges(c.cur?.meta, 'current')}
      \${c.ref && c.cur ? badge(nodeDelta ? 'warn' : 'ok',
          nodeDelta ? \`node set: +\${c.added.length} -\${c.removed.length} ~\${c.maskChanged.length}\` : 'node set identical') : ''}
      \${envMismatch(c) ? badge('bad', 'renderer differs from reference — pixel diff is not meaningful') : ''}
      <span class="badge" data-diff>diff …</span>
      <span class="modes">
        <button data-mode="side" aria-pressed="true">side by side</button>
        <button data-mode="wipe">wipe</button>
        <button data-mode="blink">blink</button>
        <button data-mode="heat">diff</button>
      </span>
    </div>
    \${m?.note ? \`<div class="note">\${m.note}</div>\` : ''}
    <div class="stage side"></div>
    <div class="nodes"></div>\`;
  main.appendChild(el);

  const stage = el.querySelector('.stage');
  const diffBadge = el.querySelector('[data-diff]');

  if (!c.ref || !c.cur) {
    stage.classList.remove('side');
    const one = c.cur || c.ref;
    stage.innerHTML = one
      ? \`<div class="pane"><h3>\${c.cur ? 'current' : 'reference'}</h3><img src="\${one.uri}"></div>\`
      : '<div class="missing">nothing captured</div>';
    diffBadge.remove();
    continue;
  }

  // decode both once, then every mode is a cheap redraw off these
  const load = (uri) => new Promise((r) => { const i = new Image(); i.onload = () => r(i); i.src = uri; });
  const [refImg, curImg] = await Promise.all([load(c.ref.uri), load(c.cur.uri)]);
  const W = curImg.naturalWidth, H = curImg.naturalHeight;

  // per-pixel max-channel delta. the threshold ignores the low-amplitude
  // noise that dxt decoding and jpeg tiles produce between runs; what's
  // reported is the share of pixels that moved enough to see
  let pctChanged = 0, heatUri = '';
  if (refImg.naturalWidth === W && refImg.naturalHeight === H) {
    const g = (img) => { const cv = new OffscreenCanvas(W, H); const x = cv.getContext('2d', { willReadFrequently: true });
                         x.drawImage(img, 0, 0); return x.getImageData(0, 0, W, H).data; };
    const a = g(refImg), b = g(curImg);
    const heat = new OffscreenCanvas(W, H);
    const hx = heat.getContext('2d');
    const out = hx.createImageData(W, H);
    let changed = 0;
    for (let i = 0; i < a.length; i += 4) {
      const d = Math.max(Math.abs(a[i] - b[i]), Math.abs(a[i+1] - b[i+1]), Math.abs(a[i+2] - b[i+2]));
      if (d > 8) changed++;
      // dim the unchanged image underneath so the hot pixels read clearly
      const t = Math.min(1, d / 64);
      out.data[i]   = 20 + t * 235;
      out.data[i+1] = 20 + (1 - t) * 40;
      out.data[i+2] = 24 + (1 - t) * 40;
      out.data[i+3] = 255;
    }
    hx.putImageData(out, 0, 0);
    pctChanged = 100 * changed / (W * H);
    heatUri = URL.createObjectURL(await heat.convertToBlob({ type: 'image/png' }));
  } else {
    pctChanged = NaN;
  }
  scores[c.name] = { pctChanged, nodeDelta, added: c.added.length,
                     removed: c.removed.length, maskChanged: c.maskChanged.length,
                     complete: c.cur.meta.complete };
  el.style.order = String(-Math.round((Number.isNaN(pctChanged) ? 100 : pctChanged) * 1000));
  diffBadge.className = 'badge ' + (Number.isNaN(pctChanged) ? 'bad' : pctChanged > 2 ? 'warn' : pctChanged > 0.05 ? '' : 'ok');
  diffBadge.textContent = Number.isNaN(pctChanged) ? 'size mismatch' : \`diff \${pctChanged.toFixed(2)}% px\`;

  const t = (m, k, f = (v) => v) => m?.timings?.[k] != null ? f(m.timings[k]) : '—';
  el.querySelector('.nodes').innerHTML = \`
    <table class="t"><tr>
      <td>drawn nodes</td><td class="v">\${c.ref.meta.nodesDrawn} → \${c.cur.meta.nodesDrawn}</td>
      <td>bfs</td><td class="v">\${t(c.ref.meta,'bfsMs',v=>v.toFixed(2))} → \${t(c.cur.meta,'bfsMs',v=>v.toFixed(2))} ms</td>
      <td>draw submit</td><td class="v">\${t(c.ref.meta,'drawMs',v=>v.toFixed(2))} → \${t(c.cur.meta,'drawMs',v=>v.toFixed(2))} ms</td>
      <td>evict</td><td class="v">\${t(c.ref.meta,'evictMs',v=>v.toFixed(2))} → \${t(c.cur.meta,'evictMs',v=>v.toFixed(2))} ms</td>
    </tr><tr>
      <td>renderer</td><td class="v" colspan="7">\${c.cur.meta.env?.renderer || 'unrecorded'}\${
        envMismatch(c) ? \`<br>reference: \${c.ref.meta.env.renderer}\` : ''}\${
        c.cur.meta.env ? \` · s3tc \${c.cur.meta.env.s3tc ? 'yes' : 'NO (jpeg fallback)'} · highp \${c.cur.meta.env.highp ? 'yes' : 'NO (mediump path)'}\` : ''}</td>
    </tr></table>
    \${nodeDelta ? \`<details><summary>node set changed (\${nodeDelta})</summary><pre>\${
      [...c.added.map(p => '+ ' + p), ...c.removed.map(p => '- ' + p),
       ...c.maskChanged.map(p => '~ ' + p)].join('\\n')
    }</pre></details>\` : ''}\`;

  const render = (mode) => {
    if (mode === 'side') {
      stage.classList.add('side');
      stage.innerHTML = \`<div class="pane"><h3>reference</h3><img src="\${c.ref.uri}"></div>
                         <div class="pane"><h3>current</h3><img src="\${c.cur.uri}"></div>\`;
    } else if (mode === 'heat') {
      stage.classList.remove('side');
      stage.innerHTML = heatUri
        ? \`<div class="pane"><h3>changed pixels</h3><img src="\${heatUri}"></div>\`
        : '<div class="missing">sizes differ — no diff</div>';
    } else if (mode === 'blink') {
      stage.classList.remove('side');
      stage.innerHTML = \`<div class="pane"><h3 data-l>reference</h3><img data-b src="\${c.ref.uri}"></div>\`;
      const img = stage.querySelector('[data-b]'), lab = stage.querySelector('[data-l]');
      let on = false;
      const id = setInterval(() => {
        on = !on;
        img.src = on ? c.cur.uri : c.ref.uri;
        lab.textContent = on ? 'current' : 'reference';
      }, 600);
      stage._stop = () => clearInterval(id);
    } else {
      stage.classList.remove('side');
      stage.innerHTML = \`<div class="pane"><h3>reference ← wipe → current</h3>
        <div class="wrap"><img src="\${c.ref.uri}">
          <div class="over" style="width:50%"><img src="\${c.cur.uri}" style="width:\${W}px"></div>
        </div><input type="range" min="0" max="100" value="50"></div>\`;
      const over = stage.querySelector('.over'), range = stage.querySelector('input');
      const sync = () => { over.querySelector('img').style.width = stage.querySelector('.wrap img').clientWidth + 'px'; };
      new ResizeObserver(sync).observe(stage.querySelector('.wrap'));
      range.oninput = () => { over.style.width = range.value + '%'; };
      sync();
    }
  };
  el.querySelectorAll('.modes button').forEach((b) => b.onclick = () => {
    if (stage._stop) { stage._stop(); stage._stop = null; }
    el.querySelectorAll('.modes button').forEach((o) => o.setAttribute('aria-pressed', String(o === b)));
    render(b.dataset.mode);
  });
  render('side');
}

main.style.display = 'flex';
const vals = Object.values(scores);
document.getElementById('summary').textContent =
  \`\${CARDS.length} shots · \${vals.filter(v => v.pctChanged > 0.05).length} with visible pixel change · \` +
  \`\${vals.filter(v => v.nodeDelta).length} with node-set change · \` +
  \`\${vals.filter(v => !v.complete).length} incomplete\`;
window.__ready = true;
</script>`;

await fs.mkdir(OUT, { recursive: true });
const file = path.join(OUT, 'report.html');
await fs.writeFile(file, '<!doctype html><meta charset="utf-8">' + html);

// read the scores back out of the page it just wrote, so the terminal gets the
// same triage ordering the browser shows
const browser = await chromium.launch();
const page = await browser.newPage();
await page.goto('file://' + file);
await page.waitForFunction('window.__ready === true', null, { timeout: 60000 }).catch(() => {});
const scores = await page.evaluate(() => window.__scores || {});
await browser.close();

const rows = Object.entries(scores).sort((a, b) => b[1].pctChanged - a[1].pctChanged);
console.log('\nshot                  diff%   node set        loaded');
console.log('-'.repeat(58));
for (const [name, s] of rows) {
  const nodes = s.nodeDelta ? `+${s.added} -${s.removed} ~${s.maskChanged}` : 'identical';
  console.log(`${name.padEnd(21)} ${s.pctChanged.toFixed(2).padStart(6)}  ${nodes.padEnd(15)} ${s.complete ? 'yes' : 'NO'}`);
}
console.log(`\nreport: ${file}`);
if (process.argv.includes('--open')) (await import('node:child_process')).exec(`open "${file}"`);
