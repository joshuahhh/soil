// cross-browser render check: every shot in every browser, in one run,
// compared against each other rather than against stored references.
//
//   node tools/browsers.mjs                 # all shots, all browsers
//   node tools/browsers.mjs seattle-skyline # one shot
//   node tools/browsers.mjs --open
//
// there is nothing to bless and nothing to drift: the comparison is between
// engines in the same run, so imagery updates hit every column equally and
// cancel out. chromium is the anchor column only because it has to be one;
// it isn't a source of truth.
//
// the per-browser diff numbers will never be zero — different rasterisers
// round differently, and webkit on apple silicon has no s3tc so it renders the
// jpeg texture path outright. they are a sort key, not a verdict. the point is
// that a human looks at columns side by side, which catches the failures no
// assertion would think to check for: garbled uv interpolation, z-fighting, a
// channel swap, missing crack-fill lines, geometry inside out.
import fs from 'node:fs/promises';
import path from 'node:path';
import { chromium, firefox, webkit } from 'playwright';
import { serveWeb } from './serve.mjs';
import { OUT, readShots, captureShot, STYLE } from './lib.mjs';

const ENGINES = { chromium, firefox, webkit };
const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i < 0 ? d : (argv[i + 1] ?? true); };
const width = Number(flag('width', 1024));
const height = Number(flag('height', 640));
const timeoutMs = Number(flag('timeout', 120000));
const want = String(flag('browsers', 'chromium,firefox,webkit')).split(',');
const shots = await readShots(argv.filter((a) => !a.startsWith('--') && !/^\d+$/.test(a)));
if (!shots.length) { console.error('no shots matched'); process.exit(1); }

const DIR = path.join(OUT, 'browsers');
await fs.rm(DIR, { recursive: true, force: true });
await fs.mkdir(DIR, { recursive: true });

const { base, close } = await serveWeb();
const results = {}; // shot -> browser -> meta
const uris = {};    // shot -> browser -> data uri

for (const name of want) {
  const type = ENGINES[name];
  if (!type) { console.error(`unknown browser ${name}`); continue; }
  let browser;
  try {
    browser = await type.launch();
  } catch (e) {
    // a browser that won't even start is itself a result worth reporting
    console.log(`${name.padEnd(9)} LAUNCH FAILED — ${e.message.split('\n')[0]}`);
    for (const s of shots) ((results[s.name] ??= {})[name] = { launchFailed: e.message.split('\n')[0] });
    continue;
  }
  console.log(`\n${name}`);
  for (const shot of shots) {
    const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1 });
    const page = await ctx.newPage();
    const { png, meta } = await captureShot(page, base, shot, { timeoutMs });
    await fs.mkdir(path.join(DIR, name), { recursive: true });
    if (png) {
      await fs.writeFile(path.join(DIR, name, shot.name + '.png'), png);
      (uris[shot.name] ??= {})[name] = 'data:image/png;base64,' + png.toString('base64');
    }
    await fs.writeFile(path.join(DIR, name, shot.name + '.json'),
      JSON.stringify({ ...meta, width, height }, null, 2));
    (results[shot.name] ??= {})[name] = { ...meta, width, height };
    const tag = !meta.rendered ? 'NO FRAME' : meta.complete ? 'ok' : 'INCOMPLETE';
    console.log(`  ${shot.name.padEnd(20)} ${tag.padEnd(11)} ` +
      `${meta.nodesLoaded}/${meta.nodesWanted} tiles, ${meta.nodesDrawn} drawn` +
      (meta.errors.length ? `  [${meta.errors.length} console errors]` : ''));
    await ctx.close();
  }
  await browser.close();
}
close();

const cards = shots.map((s) => ({
  name: s.name, note: s.note,
  browsers: want.map((b) => ({ browser: b, meta: results[s.name]?.[b] || null, uri: uris[s.name]?.[b] || null })),
}));

const html = `<title>soil cross-browser render check</title>
<style>${STYLE}
  .grid { padding:16px; display:grid; gap:14px; }
  .cell h3 small { color:var(--dim); font-weight:400; text-transform:none; letter-spacing:0; }
  .fail { padding:40px 12px; text-align:center; color:var(--bad); background:#0f1215;
          border-radius:4px; font-family:ui-monospace,Menlo,monospace; font-size:12px; }
</style>
<header>
  <h1>soil cross-browser render check</h1>
  <span class="sub" id="summary">measuring…</span>
  <span class="sub">columns are compared to each other, not to a stored reference. diffs are never zero across engines — look, don't threshold.</span>
</header>
<main id="main"></main>
<script type="module">
const CARDS = ${JSON.stringify(cards)};
const ANCHOR = ${JSON.stringify(want[0])};
const scores = window.__scores = {};
const badge = (c, t) => \`<span class="badge \${c}">\${t}</span>\`;

const load = (uri) => new Promise((r, j) => { const i = new Image(); i.onload = () => r(i); i.onerror = j; i.src = uri; });
const pixels = (img, w, h) => { const c = new OffscreenCanvas(w, h);
  const x = c.getContext('2d', { willReadFrequently: true }); x.drawImage(img, 0, 0);
  return x.getImageData(0, 0, w, h).data; };

const main = document.getElementById('main');
for (const c of CARDS) {
  const el = document.createElement('div');
  el.className = 'card';
  const n = c.browsers.length;
  el.innerHTML = \`<div class="head"><span class="name">\${c.name}</span>
      <span class="badge" data-sum>…</span>
    </div>
    \${c.note ? \`<div class="note">\${c.note}</div>\` : ''}
    <div class="grid" style="grid-template-columns:repeat(\${n},1fr)"></div>
    <div class="nodes"></div>\`;
  main.appendChild(el);
  const grid = el.querySelector('.grid');

  const anchor = c.browsers.find((b) => b.browser === ANCHOR && b.uri);
  const anchorImg = anchor ? await load(anchor.uri) : null;
  const W = anchorImg?.naturalWidth, H = anchorImg?.naturalHeight;
  const anchorPx = anchorImg ? pixels(anchorImg, W, H) : null;

  let worst = 0, broken = 0;
  for (const b of c.browsers) {
    const m = b.meta;
    let diffTxt = '';
    if (b.uri && anchorPx && b.browser !== ANCHOR) {
      const img = await load(b.uri);
      if (img.naturalWidth === W && img.naturalHeight === H) {
        const p = pixels(img, W, H);
        let changed = 0;
        for (let i = 0; i < p.length; i += 4)
          if (Math.max(Math.abs(p[i]-anchorPx[i]), Math.abs(p[i+1]-anchorPx[i+1]),
                       Math.abs(p[i+2]-anchorPx[i+2])) > 8) changed++;
        const pct = 100 * changed / (W * H);
        worst = Math.max(worst, pct);
        diffTxt = \`<small>· \${pct.toFixed(1)}% vs \${ANCHOR}</small>\`;
      } else diffTxt = '<small>· size mismatch</small>';
    } else if (b.browser === ANCHOR) diffTxt = '<small>· anchor</small>';

    const caps = m?.env ? \`\${m.env.s3tc ? 's3tc' : 'JPEG PATH'} · \${m.env.highp ? 'highp' : 'MEDIUMP'}\` : '';
    if (!b.uri || m?.launchFailed || !m?.rendered) broken++;
    const cell = document.createElement('div');
    cell.className = 'cell';
    cell.innerHTML = \`<h3>\${b.browser} \${diffTxt}</h3>\` + (b.uri
      ? \`<img src="\${b.uri}">\`
      : \`<div class="fail">\${m?.launchFailed ? 'browser would not launch' : 'no frame rendered'}\\n\${(m?.errors || []).slice(0,2).join('\\n')}</div>\`);
    cell.innerHTML += \`<div style="margin-top:6px;display:flex;gap:6px;flex-wrap:wrap">\${
      m ? badge(m.rendered && m.complete ? 'ok' : 'bad',
            \`\${m.nodesLoaded ?? '-'}/\${m.nodesWanted ?? '-'} tiles\`) : ''}\${
      caps ? badge(m.env.s3tc && m.env.highp ? '' : 'warn', caps) : ''}\${
      m?.errors?.length ? badge('bad', \`\${m.errors.length} console errors\`) : ''}</div>\`;
    grid.appendChild(cell);
  }

  scores[c.name] = { worst, broken };
  el.style.order = String(-Math.round(worst * 1000) - broken * 1e7);
  const sum = el.querySelector('[data-sum]');
  sum.className = 'badge ' + (broken ? 'bad' : worst > 15 ? 'warn' : 'ok');
  sum.textContent = broken ? \`\${broken} browser(s) failed to render\`
    : \`max \${worst.toFixed(1)}% vs \${ANCHOR}\`;

  el.querySelector('.nodes').innerHTML = '<table class="t">' + c.browsers.map((b) =>
    \`<tr><td class="v">\${b.browser}</td><td>\${b.meta?.env?.renderer || b.meta?.launchFailed || '—'}</td>
     <td>drawn \${b.meta?.nodesDrawn ?? '—'}</td></tr>\`).join('') + '</table>';
}

const vals = Object.values(scores);
document.getElementById('summary').textContent =
  \`\${CARDS.length} shots x \${CARDS[0]?.browsers.length ?? 0} browsers · \` +
  \`\${vals.filter((v) => v.broken).length} with a browser that failed to render · \` +
  \`worst cross-engine diff \${Math.max(0, ...vals.map((v) => v.worst)).toFixed(1)}%\`;
window.__ready = true;
</script>`;

const file = path.join(OUT, 'browsers.html');
await fs.writeFile(file, '<!doctype html><meta charset="utf-8">' + html);

const b = await chromium.launch();
const p = await b.newPage();
await p.goto('file://' + file);
await p.waitForFunction('window.__ready === true', null, { timeout: 120000 }).catch(() => {});
const scores = await p.evaluate(() => window.__scores || {});
await b.close();

console.log('\nshot                  worst diff vs anchor   failed to render');
console.log('-'.repeat(62));
for (const [n, s] of Object.entries(scores).sort((a, b2) => b2[1].worst - a[1].worst))
  console.log(`${n.padEnd(21)} ${s.worst.toFixed(1).padStart(10)}%          ${s.broken || '-'}`);
console.log(`\nreport: ${file}`);
if (argv.includes('--open')) (await import('node:child_process')).exec(`open "${file}"`);
