// one pose, settled on the real gpu, saved as a png — for looking at a
// rendering change without the whole capture run. full chrome in headless
// mode (the gpu, no window), the warm tile cache that loadprof.mjs --warm
// keeps, and a hard deadline over the lot.
//
//   node tools/shot.mjs seattle-skyline out.png                # a shots.json entry
//   node tools/shot.mjs --session ~/x.json out.png             # a ⌘C session blob's pose
//   node tools/shot.mjs manhattan-down out.png --q sun=250,15  # any page query parameter
//   node tools/shot.mjs seattle-skyline out.png --glamour 128  # accumulate that many samples first
//   --cold      a fresh browser profile instead of the warm one
//   --width/--height   viewport (default 1280x800)
import fs from 'node:fs/promises';
import path from 'node:path';
import { chromium } from 'playwright';
import { serveWeb, WARM_PORT } from './serve.mjs';
import { OUT, readShots, shotQuery } from './lib.mjs';

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i < 0 ? d : (argv[i + 1] ?? true); };
const has = (n) => argv.includes('--' + n);
const width = Number(flag('width', 1280)), height = Number(flag('height', 800));
const glamour = Number(flag('glamour', 0));
const valued = new Set(['session', 'q', 'glamour', 'width', 'height']);
const positional = argv.filter((a, i) => !a.startsWith('--') && !(i > 0 && valued.has(argv[i - 1].slice(2))));

let shot, out;
if (flag('session')) {
  const v = JSON.parse(await fs.readFile(flag('session'), 'utf8')).ls['earth-view'];
  shot = { lat: v.lat, lon: v.lon, alt: v.alt, heading: v.heading, tilt: v.tilt, fov: v.fov, ortho: !!v.ortho };
  [out] = positional;
} else {
  [shot] = await readShots([positional[0]]);
  if (!shot) { console.error('no such shot:', positional[0]); process.exit(1); }
  out = positional[1];
}
if (!out) { console.error('usage: shot.mjs <shot|--session f> <out.png> [--q k=v] [--glamour n]'); process.exit(1); }

const q = shotQuery(shot);
q.set('fetchlog', '0');
for (let i = 0; i < argv.length; i++) if (argv[i] === '--q') { const [k, v] = argv[i + 1].split('='); q.set(k, v ?? '1'); }

const deadline = setTimeout(() => { console.error('shot: deadline (180 s) — giving up'); process.exit(2); }, 180000);
const warm = !has('cold');
const { base, close } = await serveWeb({ port: warm ? WARM_PORT : 0 });
const launch = { headless: true, channel: 'chromium', viewport: { width, height }, deviceScaleFactor: 1 };
const ctx = warm
  ? await chromium.launchPersistentContext(path.join(OUT, 'profile'), launch)
  : await (await chromium.launch(launch)).newContext({ viewport: launch.viewport });
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', (e) => errors.push('PAGEERROR ' + e.message.split('\n')[0]));
page.on('console', (m) => {
  const t = m.text();
  // a shader that doesn't compile prints its log and aborts the module
  if (m.type() === 'error' && !/Failed to load resource/.test(t)) errors.push(t.slice(0, 200));
  else if (/^ERROR: |incomplete|glamour:/.test(t)) errors.push(t.slice(0, 200));
});
await page.goto(`${base}/index.html?${q}`, { waitUntil: 'load' });
await page.waitForFunction('window.__test && window.__test.stats() !== null', null, { timeout: 60000 })
  .catch(() => errors.push('page never became ready'));
let settled = { complete: false, ms: 0 };
if (!errors.length) {
  settled = await page.evaluate(() => window.__test.settle({ timeoutMs: 90000 }));
  if (glamour > 0) {
    await page.evaluate((n) => window.__demo.glamour({ on: true, samples: n }), glamour);
    await page.waitForFunction((n) => window.__test.stats().accumSamples >= n, glamour, { timeout: 30000 })
      .catch(() => errors.push('glamour did not reach ' + glamour + ' samples'));
  }
  const uri = await page.evaluate(() => window.__test.shot());
  await fs.writeFile(out, Buffer.from(uri.split(',')[1], 'base64'));
}
// the gpu timings are a second's average, read back frames late: give
// them two seconds of frames at the pose before reading (the cpu frame time
// is averaged over the same two seconds, from the reset here)
await page.evaluate(() => window.__test && window.__test.frameTiming()).catch(() => null);
if (!errors.length) await page.waitForTimeout(2200);
const st = await page.evaluate(() => {
  const s = window.__test && window.__test.stats();
  return s && { ...s, ...window.__test.frameTiming() };
}).catch(() => null);
console.log(`${path.basename(out)}: ${settled.complete ? 'complete' : 'INCOMPLETE'}`
  + (st ? ` ${st.nodesLoaded}/${st.nodesWanted} tiles, ${st.nodesDrawn} drawn, frame ${st.frameMs.toFixed(1)} ms cpu` : '')
  + (st && st.gpuMs >= 0 ? `, ${st.gpuMs.toFixed(2)} ms gpu (shadow ${st.gpuShadowMs.toFixed(2)}, scene ${st.gpuSceneMs.toFixed(2)},`
    + ` occlusion ${st.gpuOcclusionMs.toFixed(2)}, post ${st.gpuPostMs.toFixed(2)})` : '')
  + (glamour ? `, ${st && st.accumSamples} samples` : '')
  + (errors.length ? '\n  ' + [...new Set(errors)].join('\n  ') : ''));
await ctx.close();
close();
clearTimeout(deadline);
process.exit(errors.length ? 1 : 0);
