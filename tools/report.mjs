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
import { STYLE } from './lib.mjs';

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
<style>${STYLE}
  /* one comparison at a time, so a card should fit the window without
     scrolling the image out from under the header. the real value is measured
     in js (see fit) — this is only the pre-measurement starting point */
  :root { --imgmax: calc(100vh - 260px); }
  .stage img, .stage canvas { max-width:100%; max-height:var(--imgmax);
                              width:auto; height:auto; margin:0 auto; }
  .stage .pane { display:flex; flex-direction:column; align-items:center; }
  .stage .pane h3 { align-self:stretch; }
  /* wipe: the image itself is the control — no slider to aim at */
  .wipe { position:relative; display:inline-block; line-height:0; cursor:col-resize;
          touch-action:none; }
  .wipe .top { position:absolute; top:0; left:0; bottom:0; width:var(--pos,50%);
               overflow:hidden; }
  /* the clipped copy must render at exactly the base image's box, which its
     own max-width can't give it inside a narrowed parent — js supplies it */
  .wipe .top img { position:absolute; top:0; left:0; max-width:none; max-height:none;
                   width:var(--w); height:var(--h); }
  .wipe .line { position:absolute; top:0; bottom:0; left:var(--pos,50%); width:2px;
                margin-left:-1px; background:var(--accent); pointer-events:none;
                box-shadow:0 0 0 1px rgba(0,0,0,.5); }
  .wipe .tag { position:absolute; top:8px; font:11px ui-monospace,Menlo,monospace;
               background:rgba(0,0,0,.6); color:#fff; padding:2px 7px; border-radius:3px;
               pointer-events:none; }
  .wipe .tag.l { left:8px; } .wipe .tag.r { right:8px; }
</style>
<header>
  <h1>soil render report</h1>
  <span class="sub" id="summary">measuring…</span>
  <span class="sub">pixel diff orders the list; it does not judge. check the node-set and tile badges.</span>
  <span class="modes" id="modes">
    <button data-mode="side" aria-pressed="true">side by side</button>
    <button data-mode="wipe">wipe</button>
    <button data-mode="blink">blink</button>
    <button data-mode="heat">diff</button>
  </span>
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

// one blink clock for the whole page, so every card flips together instead of
// each drifting on its own interval
let blinkOn = false;
const blinkSubs = new Set();
setInterval(() => { blinkOn = !blinkOn; blinkSubs.forEach((f) => f(blinkOn)); }, 600);

const main = document.getElementById('main');
const cards = []; // { render, stop } per comparable card, driven by the header

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

  const t = (mm, k, f = (v) => v) => mm?.timings?.[k] != null ? f(mm.timings[k]) : '—';
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

  let cleanup = null; // undo whatever the last mode installed
  const render = (mode) => {
    if (cleanup) { cleanup(); cleanup = null; }
    if (mode === 'side') {
      stage.classList.add('side');
      stage.innerHTML = \`<div class="pane"><h3>reference</h3><img src="\${c.ref.uri}"></div>
                         <div class="pane"><h3>current</h3><img src="\${c.cur.uri}"></div>\`;
      return;
    }
    stage.classList.remove('side');
    if (mode === 'heat') {
      stage.innerHTML = heatUri
        ? \`<div class="pane"><h3>changed pixels</h3><img src="\${heatUri}"></div>\`
        : '<div class="missing">sizes differ — no diff</div>';
    } else if (mode === 'blink') {
      stage.innerHTML = \`<div class="pane"><h3 data-l>reference</h3><img data-b src="\${c.ref.uri}"></div>\`;
      const img = stage.querySelector('[data-b]'), lab = stage.querySelector('[data-l]');
      const sub = (on) => {
        img.src = on ? c.cur.uri : c.ref.uri;
        lab.textContent = on ? 'current' : 'reference';
      };
      blinkSubs.add(sub);
      sub(blinkOn);
      cleanup = () => blinkSubs.delete(sub);
    } else { // wipe
      stage.innerHTML = \`<div class="pane"><h3>drag across the image — left is reference, right is current</h3>
        <div class="wipe"><img class="base" src="\${c.ref.uri}">
          <div class="top"><img src="\${c.cur.uri}"></div>
          <div class="line"></div>
          <span class="tag l">reference</span><span class="tag r">current</span>
        </div></div>\`;
      const wipe = stage.querySelector('.wipe'), base = stage.querySelector('.base');
      // the clipped copy is absolutely positioned inside a narrowed box, so it
      // needs the base's rendered size handed to it explicitly — and re-handed
      // whenever the layout changes
      const sync = () => {
        wipe.style.setProperty('--w', base.clientWidth + 'px');
        wipe.style.setProperty('--h', base.clientHeight + 'px');
      };
      const ro = new ResizeObserver(sync);
      ro.observe(base);
      sync();
      const move = (e) => {
        const r = base.getBoundingClientRect();
        wipe.style.setProperty('--pos',
          Math.max(0, Math.min(r.width, e.clientX - r.left)) + 'px');
      };
      wipe.addEventListener('pointermove', move);
      wipe.addEventListener('pointerdown', move);
      cleanup = () => ro.disconnect();
    }
  };
  cards.push({ render });
  render(document.querySelector('#modes button[aria-pressed="true"]').dataset.mode);
}

// size the images so a whole card fits the window. a fixed subtraction can't
// do this: the chrome above and below an image varies with the mode and with
// how many lines the note wraps to, so measure it. chrome doesn't depend on
// the image height, so one pass settles
const fit = () => {
  const hdr = document.querySelector('header').offsetHeight;
  let chrome = 0;
  for (const card of document.querySelectorAll('.card')) {
    const img = card.querySelector('.stage img, .stage canvas');
    if (img) chrome = Math.max(chrome, card.offsetHeight - img.getBoundingClientRect().height);
  }
  document.documentElement.style.setProperty('--imgmax',
    Math.max(180, innerHeight - hdr - chrome - 56) + 'px');
};

// the mode is one decision for the whole page, not one per pair
document.querySelectorAll('#modes button').forEach((b) => b.onclick = () => {
  document.querySelectorAll('#modes button')
    .forEach((o) => o.setAttribute('aria-pressed', String(o === b)));
  for (const c of cards) c.render(b.dataset.mode);
  fit();
});

fit();
addEventListener('resize', fit);

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
