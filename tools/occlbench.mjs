// occlusion cost, a/b'd fairly: gpu timer queries measure elapsed gpu time,
// which includes whatever else the gpu is doing (another tab, another app),
// so runs taken minutes apart aren't comparable. this loads one view once
// and cycles the configurations live, several rounds, reading each one's
// one-second gpu average, and reports medians — every configuration sees
// the same background.
//
//   node tools/occlbench.mjs [shot] [--width 2560 --height 1600] [--rounds 4]
import { chromium } from 'playwright';
import { serveWeb } from './serve.mjs';
import { readShots, shotQuery } from './lib.mjs';

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i < 0 ? d : argv[i + 1]; };
const width = Number(flag('width', 2560)), height = Number(flag('height', 1600)), rounds = Number(flag('rounds', 4));
const [shot] = await readShots([argv.find((a, i) => !a.startsWith('--') && !(i && argv[i - 1].startsWith('--'))) || 'manhattan-down']);
const configs = [
  { name: 'off', occl: false },
  { name: 'full, 16 taps', occl: true, res: 1, taps: 16 },
  { name: 'half, 16 taps', occl: true, res: 2, taps: 16 },
  { name: 'half, 8 taps', occl: true, res: 2, taps: 8 },
];

const deadline = setTimeout(() => { console.error('occlbench: deadline'); process.exit(2); }, 600000);
const { base, close } = await serveWeb();
const browser = await chromium.launch({ headless: true, channel: 'chromium' });
const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: 1 });
const q = shotQuery(shot); q.set('fetchlog', '0'); q.set('idb', '0');
await page.goto(`${base}/index.html?${q}`, { waitUntil: 'load' });
await page.waitForFunction('window.__test && window.__test.stats() !== null', null, { timeout: 60000 });
await page.evaluate(() => window.__test.settle({ timeoutMs: 90000 }));
const results = Object.fromEntries(configs.map((c) => [c.name, []]));
for (let r = 0; r < rounds; r++)
  for (const c of configs) {
    await page.evaluate((c) => {
      window.__demo.sky({ occl: c.occl });
      if (c.occl) window.__demo.view.setOcclusionQuality(c.res, c.taps);
    }, c);
    await page.waitForTimeout(2300); // a full one-second window after the switch
    results[c.name].push(await page.evaluate(() => window.__test.stats()));
  }
const med = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
console.log(`${shot.name}, ${width}x${height}, ${rounds} rounds — median gpu ms per frame`);
console.log('config'.padEnd(16) + 'total'.padStart(8) + 'scene'.padStart(8) + 'occl'.padStart(8) + 'post'.padStart(8));
for (const c of configs) {
  const rs = results[c.name];
  const m = (k) => med(rs.map((s) => s[k])).toFixed(2).padStart(8);
  console.log(c.name.padEnd(16) + m('gpuMs') + m('gpuSceneMs') + m('gpuOcclusionMs') + m('gpuPostMs'));
}
await browser.close(); close();
clearTimeout(deadline);
