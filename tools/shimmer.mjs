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
import { OUT, readShots, shotQuery } from './lib.mjs';

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

const { base, close } = await serveWeb();
const browser = await chromium.launch();
const DIR = path.join(OUT, 'shimmer');
await fs.mkdir(DIR, { recursive: true });

for (const shot of shots) {
  console.log(`\n${shot.name} — nudging heading by ${NUDGE}deg\n` + '-'.repeat(64));
  console.log('config'.padEnd(30) + 'whole frame'.padStart(13) + 'distant half'.padStart(15));
  // one context for every config, unlike capture.mjs: these render the same
  // scene, so they should share the page's indexeddb tile cache. only the
  // first config pays the download; the rest start warm. (msaa still needs a
  // fresh page per config — it's a context attribute fixed at creation.)
  const ctx = await browser.newContext({ viewport: { width: W, height: H }, deviceScaleFactor: 1 });
  for (const cfg of CONFIGS) {
    const q = shotQuery(shot);
    if (cfg.msaa) q.set('msaa', '1');
    const page = await ctx.newPage();
    await page.goto(`${base}/index.html?${q}`, { waitUntil: 'load' });
    await page.waitForFunction('window.__test && window.__test.stats() !== null', null, { timeout: 60000 });
    await page.evaluate((on) => window.__test.setMipmaps(on), cfg.mips);
    await page.evaluate(() => window.__test.settle({ timeoutMs: 150000 }));

    const a = await page.evaluate(() => window.__test.shot());
    // move, let it re-settle (a nudge this small shouldn't change the tile set)
    await page.evaluate((d) => window.__test.nudgeHeading(d), NUDGE);
    await page.evaluate(() => window.__test.settle({ timeoutMs: 60000, stable: 3 }));
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

    await fs.writeFile(path.join(DIR, `${shot.name}-${cfg.mips?'mip':'nomip'}-${cfg.msaa?'msaa':'nomsaa'}.png`),
      Buffer.from(b.split(',')[1], 'base64'));
    console.log(cfg.name.padEnd(30) + score.all.toFixed(2).padStart(13) + score.far.toFixed(2).padStart(15));
    await page.close();
  }
  await ctx.close();
}
await browser.close();
close();
console.log(`\nlower is steadier. frames in ${DIR}`);
