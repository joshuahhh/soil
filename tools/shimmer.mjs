// measures temporal shimmer: how much the image churns for a fixed, tiny
// camera movement.
//
//   node tools/shimmer.mjs                    # every config on the default shot
//   node tools/shimmer.mjs rainier-oblique
//
// a still frame can't show shimmer, and eyeballing motion can't compare four
// configurations fairly. so: settle a view, capture, nudge the camera by a
// fixed sub-degree amount, capture again, and score the mean absolute pixel
// change. the nudge is identical across configs, and so is the scene, so the
// absolute number means nothing but the ordering is real — a renderer that
// filters properly changes smoothly under a small move, an aliasing one
// flickers.
//
// the score is reported over the distant half of the frame as well as the
// whole thing, because minification aliasing is a distance problem and the
// near field dilutes it.
import fs from 'node:fs/promises';
import path from 'node:path';
import { chromium } from 'playwright';
import { serveWeb } from './serve.mjs';
import { OUT, readShots, shotQuery, comparisonPage } from './lib.mjs';

const argv = process.argv.slice(2);
const only = argv.filter((a) => !a.startsWith('--'));
const shots = await readShots(only.length ? only : ['seattle-skyline']);
const NUDGE = 0.05; // degrees of heading — about one pixel at 1024px / 45deg fov
const W = 1024, H = 640;

const CONFIGS = [
  { name: 'baseline (no mips, no msaa)', mips: false, msaa: false },
  { name: 'mipmaps + 16x aniso',         mips: true,  msaa: false },
  { name: 'msaa only',                   mips: false, msaa: true  },
  { name: 'mipmaps + msaa',              mips: true,  msaa: true  },
];

const DIR = path.join(OUT, 'shimmer');
await fs.mkdir(DIR, { recursive: true });
// --report-only rebuilds the page from the last run's saved frames without
// re-measuring, which otherwise costs a full settle per config
const reportOnly = argv.includes('--report-only');
let bad = 0;
let results = [];

if (reportOnly) {
  results = JSON.parse(await fs.readFile(path.join(DIR, 'results.json'), 'utf8'));
  bad = results.filter((r) => !r.ok).length;
  console.log(`rebuilding from ${results.length} saved config(s)`);
} else {
const { base, close } = await serveWeb();
const browser = await chromium.launch();

for (const shot of shots) {
  console.log(`\n${shot.name} — nudging heading by ${NUDGE}deg\n` + '-'.repeat(64));
  console.log('config'.padEnd(30) + 'whole frame'.padStart(13) + 'distant half'.padStart(15) + '      loaded');
  // one context for every config, unlike capture.mjs: these render the same
  // scene, so they should share the page's indexeddb tile cache. only the
  // first config pays the download; the rest start warm. (msaa still needs a
  // fresh page per config — it's a context attribute fixed at creation.)
  const ctx = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: 1 });
  for (const cfg of CONFIGS) {
    const q = shotQuery(shot);
    q.set('msaa', cfg.msaa ? '1' : '0'); // explicit both ways: msaa now defaults on
    const page = await ctx.newPage();
    await page.goto(`${base}/index.html?${q}`, { waitUntil: 'load' });
    await page.waitForFunction('window.__test && window.__test.stats() !== null', null, { timeout: 60000 });
    await page.evaluate((on) => window.__test.setMipmaps(on), cfg.mips);
    const s1 = await page.evaluate(() => window.__test.settle({ timeoutMs: 180000 }));

    const a = await page.evaluate(() => window.__test.shot());
    // move, let it re-settle (a nudge this small shouldn't change the tile set)
    await page.evaluate((d) => window.__test.nudgeHeading(d), NUDGE);
    const s2 = await page.evaluate(() => window.__test.settle({ timeoutMs: 90000, stable: 3 }));
    const b = await page.evaluate(() => window.__test.shot());

    const score = await page.evaluate(async ([ua, ub]) => {
      const load = (u) => new Promise((r) => { const i = new Image(); i.onload = () => r(i); i.src = u; });
      const [ia, ib] = await Promise.all([load(ua), load(ub)]);
      const px = (img) => { const c = new OffscreenCanvas(img.width, img.height);
        const x = c.getContext('2d', { willReadFrequently: true }); x.drawImage(img, 0, 0);
        return x.getImageData(0, 0, img.width, img.height).data; };
      const pa = px(ia), pb = px(ib), w = ia.width, h = ia.height;
      let all = 0, allN = 0, far = 0, farN = 0;
      for (let y = 0; y < h; y++) {
        for (let x2 = 0; x2 < w; x2++) {
          const i = (y * w + x2) * 4;
          // skip flat sky: it has no detail to alias and would dilute the score
          const isSky = pa[i] > 120 && pa[i+2] > 200 && pa[i+1] > 160;
          if (isSky) continue;
          const d = (Math.abs(pa[i]-pb[i]) + Math.abs(pa[i+1]-pb[i+1]) + Math.abs(pa[i+2]-pb[i+2])) / 3;
          all += d; allN++;
          if (y < h * 0.5) { far += d; farN++; } // upper half = farther away
        }
      }
      return { all: allN ? all / allN : 0, far: farN ? far / farN : 0 };
    }, [a, b]);

    // both halves of the pair, not just the second: blinking between them is
    // the only way to actually see the shimmer the score is measuring
    const tag = `${cfg.mips ? 'mip' : 'nomip'}-${cfg.msaa ? 'msaa' : 'nomsaa'}`;
    for (const [suffix, uri] of [['before', a], ['after', b]])
      await fs.writeFile(path.join(DIR, `${shot.name}-${tag}-${suffix}.png`),
        Buffer.from(uri.split(',')[1], 'base64'));
    // the substantive condition is that every wanted tile was present for both
    // captures. sceneComplete holding for five consecutive frames is stricter
    // than that and gives false alarms: the nudge can shift the wanted set by
    // a tile, so the flag flickers while the scene is in fact fully loaded.
    // track it, but don't let it invalidate a score on its own
    const loaded = (st) => st?.nodesWanted > 0 && st.nodesLoaded === st.nodesWanted;
    const ok = loaded(s1.stats) && loaded(s2.stats);
    const steady = s1.complete && s2.complete;
    console.log(cfg.name.padEnd(30) + score.all.toFixed(2).padStart(13)
      + score.far.toFixed(2).padStart(15)
      + (ok ? `      ${s1.stats.nodesLoaded}/${s1.stats.nodesWanted} then ${s2.stats.nodesLoaded}/${s2.stats.nodesWanted} tiles${steady ? '' : ' (flag flickered)'}`
            : `   *** PARTLY LOADED (${s1.stats?.nodesLoaded}/${s1.stats?.nodesWanted} then ${s2.stats?.nodesLoaded}/${s2.stats?.nodesWanted}) — score is meaningless ***`));
    if (!ok) bad++;
    results.push({ shot: shot.name, cfg, tag, score, ok, steady,
      tiles: [`${s1.stats?.nodesLoaded}/${s1.stats?.nodesWanted}`,
              `${s2.stats?.nodesLoaded}/${s2.stats?.nodesWanted}`] });
    await page.close();
  }
  await ctx.close();
}
await browser.close();
close();
await fs.writeFile(path.join(DIR, 'results.json'), JSON.stringify(results, null, 2));
}

// --- report ---------------------------------------------------------------
// two questions, so two kinds of card. within a config, blinking the nudge
// pair *is* the shimmer: the same scene one pixel apart, so whatever jumps is
// the aliasing you'd see as crawl in motion. across configs, the same pose
// side by side shows what each one costs in sharpness.
const scored = results.filter((r) => r.ok);
const best = scored.length ? Math.min(...scored.map((r) => r.score.far)) : null;
const rows = results.map((r) =>
  `<tr class="${r.ok && r.score.far === best ? 'best' : ''}"><td>${r.cfg.name}</td>` +
  `<td>${r.score.all.toFixed(2)}</td><td>${r.score.far.toFixed(2)}</td>` +
  `<td>${r.ok ? (r.tiles || []).join(' then ') : 'PARTLY LOADED'}${
     r.ok && r.steady === false ? ' (flag flickered)' : ''}</td></tr>`).join('');

const uriFor = async (r, suffix) => 'data:image/png;base64,' +
  (await fs.readFile(path.join(DIR, `${r.shot}-${r.tag}-${suffix}.png`))).toString('base64');
for (const r of results) { r.a = await uriFor(r, 'before'); r.b = await uriFor(r, 'after'); }

const cards = results.map((r) => ({
  name: `${r.cfg.name} — the crawl`,
  note: 'the same scene one pixel apart. in blink, whatever jumps is aliasing — that motion '
      + 'is exactly what reads as shimmer while flying.',
  badges: [
    { cls: !r.ok ? 'bad' : r.score.far === best ? 'ok' : r.score.far > best * 1.1 ? 'warn' : '',
      text: `distant-half churn ${r.score.far.toFixed(2)}` },
    ...(r.ok ? [] : [{ cls: 'bad', text: 'scene only partly loaded — score is meaningless' }]),
  ],
  a: { label: 'before nudge', uri: r.a },
  b: { label: 'after nudge', uri: r.b },
}));

// and the head-to-heads that answer "what does each option actually change"
const find = (mips, msaa) => results.find((r) => r.cfg.mips === mips && r.cfg.msaa === msaa);
const pair = (x, y, name, note) => (x && y) ? [{ name, note, badges: [],
  a: { label: x.cfg.name, uri: x.a }, b: { label: y.cfg.name, uri: y.a } }] : [];
cards.push(
  ...pair(find(true, false), find(true, true), 'msaa off vs on — same pose',
    'static quality rather than shimmer. look at tower silhouettes and dense housing — then check '
    + 'tile seams against sky, where msaa can leave the crack-fill lines only partly opaque, and '
    + 'octant-mask boundaries, which use discard and so get no antialiasing at all.'),
  ...pair(find(false, false), find(true, false), 'mipmaps off vs on — same pose',
    'texture minification only, and only on the uncompressed tiles — the dxt half has no mip chain.'),
);

const html = comparisonPage({
  title: 'soil shimmer report',
  intro: `${results[0]?.shot ?? ''} · heading nudged ${NUDGE}deg · blink is the shimmer, lower churn is steadier`,
  defaultMode: 'blink',
  cards,
  extraHtml: `<div class="metricbox" style="padding:14px 24px;border-bottom:1px solid var(--line)">
    <table class="metric"><tr><th>config</th><th>whole frame</th><th>distant half</th><th>load</th></tr>
    ${rows}</table></div>`,
});
const file = path.join(OUT, 'shimmer.html');
await fs.writeFile(file, '<!doctype html><meta charset="utf-8">' + html);
console.log(`\nlower is steadier. report: ${file}`);
if (bad) console.log(`${bad} config(s) did not fully load — rerun before believing the table`);
if (argv.includes('--open')) (await import('node:child_process')).exec(`open "${file}"`);
