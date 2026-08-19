// regression capture: one browser, compared against blessed references.
// answers "did my change alter rendering?" — see browsers.mjs for the
// cross-browser question, which needs no references at all.
//
//   node tools/capture.mjs                  # capture every shot into out/current
//   node tools/capture.mjs seattle-skyline  # just one (repeatable)
//   node tools/capture.mjs --bless          # promote out/current -> reference
//   node tools/capture.mjs --gpu            # headed, so a real gpu rasterises
//   node tools/capture.mjs --width 1600
import fs from 'node:fs/promises';
import path from 'node:path';
import { chromium } from 'playwright';
import { serveWeb } from './serve.mjs';
import { HERE, OUT, readShots, captureShot } from './lib.mjs';

const CUR = path.join(OUT, 'current');
const REF = path.join(HERE, 'reference');

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i < 0 ? d : (argv[i + 1] ?? true); };
const width = Number(flag('width', 1024));
const height = Number(flag('height', 640));
const timeoutMs = Number(flag('timeout', 120000));
// --gpu drops headless, which is what puts a real gpu behind the capture:
// playwright's headless chromium rasterises with swiftshader on the cpu. the
// two produce different pixels, so a gpu capture can only be compared against
// a gpu reference — the renderer string lands in each sidecar so the report
// can say when a pair doesn't match
const gpu = argv.includes('--gpu');

if (argv.includes('--bless')) {
  await fs.rm(REF, { recursive: true, force: true });
  await fs.cp(CUR, REF, { recursive: true });
  console.log(`blessed ${(await fs.readdir(REF)).filter((f) => f.endsWith('.png')).length} shots as reference`);
  process.exit(0);
}

const shots = await readShots(argv.filter((a) => !a.startsWith('--') && !/^\d+$/.test(a)));
if (!shots.length) { console.error('no shots matched'); process.exit(1); }

await fs.mkdir(CUR, { recursive: true });
const { base, close } = await serveWeb();
const browser = await chromium.launch({ headless: !gpu });
console.log(`serving ${base}, capturing ${shots.length} shot(s) at ${width}x${height}\n`);

let bad = 0;
for (const shot of shots) {
  // a fresh context per shot: no shared indexeddb tile cache, so every run
  // exercises the real download path rather than whatever the last shot left
  // warm, and localstorage can't leak a pose between shots
  const ctx = await browser.newContext({ viewport: { width, height }, deviceScaleFactor: 1 });
  const page = await ctx.newPage();
  const { png, meta } = await captureShot(page, base, shot, { timeoutMs });
  if (png) await fs.writeFile(path.join(CUR, shot.name + '.png'), png);
  await fs.writeFile(path.join(CUR, shot.name + '.json'),
    JSON.stringify({ ...meta, width, height }, null, 2));

  const tag = !meta.rendered ? 'NO FRAME' : meta.complete ? 'complete' : 'INCOMPLETE';
  if (tag !== 'complete') bad++;
  console.log(`${shot.name.padEnd(20)} ${tag.padEnd(11)} ` +
    `${meta.nodesLoaded}/${meta.nodesWanted} tiles, ${meta.nodesDrawn} drawn, ` +
    `${(meta.settleMs / 1000).toFixed(1)}s, draw ${(meta.timings.drawMs ?? 0).toFixed(2)}ms/f`);
  await ctx.close();
}

await browser.close();
close();
console.log(`\nwrote ${shots.length} shot(s) to tools/out/current` +
  (bad ? `\n${bad} did not fully load — check the tile counts before trusting those` : ''));
