// load profiler: drive one view headlessly and record how its tiles arrive —
// a timeline of want/have/inflight counts every half second, and at the end
// (complete or timed out) the list of what the walk still wanted and didn't
// have, with how long each had been waiting. the question it answers is "why
// is this view slow to finish", which a settled screenshot cannot.
//
//   node tools/loadprof.mjs --session ~/.prusik/uploads/x.json   # pose from a ⌘C session blob
//   node tools/loadprof.mjs seattle-skyline                       # a shots.json entry
//   node tools/loadprof.mjs --lat .. --lon .. --alt .. --heading .. --tilt .. --fov ..
//   --swiftshader   the headless shell's cpu rasteriser; default is the new
//               headless chrome, which drives the real gpu (no window)
//   --trace     record a per-tile timeline (fetch trace + decode timing)
//   --q k=v     any extra page query parameter (--q msaa=0, --q lod=2)
//   --noidb     bypass the indexeddb tile cache (what does the cache cost?)
//   --cpuprofile  record the page's main thread with the devtools profiler
//               while it settles, and print the functions with the most self
//               time (build with EARTH_PROFILE=1 for the engine's names)
//   --warm      reuse a persistent profile (tools/out/profile), so the
//               indexeddb tile cache carries over between runs; default is a
//               fresh context, i.e. every tile over the network
//   --timeout   ms to give up after (default 90000)
//   --width/--height   viewport (default 1280x800)
//   --lodscale  engine lod scale (1 = normal)
//   --name      label for the output json (tools/out/loadprof-<name>.json)
import fs from 'node:fs/promises';
import path from 'node:path';
import { chromium } from 'playwright';
import { serveWeb } from './serve.mjs';
import { OUT, readShots, shotQuery } from './lib.mjs';

const argv = process.argv.slice(2);
const flag = (n, d) => { const i = argv.indexOf('--' + n); return i < 0 ? d : (argv[i + 1] ?? true); };
const has = (n) => argv.includes('--' + n);
const width = Number(flag('width', 1280)), height = Number(flag('height', 800));
const timeoutMs = Number(flag('timeout', 90000));
const warm = has('warm');
// the headless shell rasterises on the cpu (swiftshader), where a big frame
// stalls the main thread and starves the fetch callbacks — a different
// problem from the one being measured. full chrome in headless mode drives
// the real gpu with no window
const launch = has('swiftshader') ? { headless: true } : { headless: true, channel: 'chromium' };

let shot;
if (flag('session')) {
  const blob = JSON.parse(await fs.readFile(flag('session'), 'utf8'));
  const v = blob.ls['earth-view'];
  shot = { name: flag('name', 'session'), lat: v.lat, lon: v.lon, alt: v.alt,
    heading: v.heading, tilt: v.tilt, fov: v.fov, ortho: !!v.ortho, hug: !!v.hug };
} else if (flag('lat')) {
  shot = { name: flag('name', 'pose') };
  for (const k of ['lat', 'lon', 'alt', 'heading', 'tilt', 'fov']) if (flag(k)) shot[k] = Number(flag(k));
} else {
  const want = argv.filter((a) => !a.startsWith('--') && !/^[\d.-]+$/.test(a));
  [shot] = await readShots(want);
  if (!shot) { console.error('no such shot'); process.exit(1); }
}
const name = flag('name', shot.name);

const q = shotQuery(shot);
if (flag('lodscale')) q.set('lod', flag('lodscale'));
if (has('noidb')) q.set('idb', '0'); // bypass the tile cache entirely
// any other page query parameter, e.g. --q msaa=0
for (let i = 0; i < argv.length; i++) if (argv[i] === '--q') { const [k, v] = argv[i + 1].split('='); q.set(k, v ?? '1'); }
if (has('roidb')) q.set('idb', 'ro'); // cache lookups only, no writes
// per-request trace on the page side and per-node decode timing on the
// engine's console, for the per-tile timeline (--trace)
if (has('trace')) { q.set('fetchtrace', '1'); q.set('timing', '1'); }
q.set('fetchlog', '0');

await fs.mkdir(OUT, { recursive: true });
// a fixed port for warm runs: the origin is what the cache is keyed by
const { base, close } = await serveWeb({ port: warm ? 8765 : 0 });
const vp = { viewport: { width, height }, deviceScaleFactor: 1 };
const ctx = warm
  ? await chromium.launchPersistentContext(path.join(OUT, 'profile'), { ...launch, ...vp })
  : await (await chromium.launch(launch)).newContext(vp);
const page = await ctx.newPage();
const consoleLines = [];
page.on('console', (m) => consoleLines.push(`${(performance.now() / 1000).toFixed(1)} ${m.type()} ${m.text()}`));
page.on('pageerror', (e) => consoleLines.push('PAGEERROR ' + e.message));

await page.goto(`${base}/index.html?${q}`, { waitUntil: 'load' });
await page.waitForFunction('window.__test && window.__test.stats() !== null', null, { timeout: 60000 });
const renderer = await page.evaluate(() => {
  const gl = document.getElementById('globe').getContext('webgl2');
  const d = gl && gl.getExtension('WEBGL_debug_renderer_info');
  return gl ? gl.getParameter(d ? d.UNMASKED_RENDERER_WEBGL : gl.RENDERER) : null;
});
console.log(`${name}: ${JSON.stringify(shot)}\n${warm ? 'warm' : 'cold'} cache, ${width}x${height}, ${base}\n${renderer}\n`);
await page.evaluate(() => window.__test.frameTiming());

let cdp = null;
if (has('cpuprofile')) {
  cdp = await page.context().newCDPSession(page);
  await cdp.send('Profiler.enable');
  await cdp.send('Profiler.setSamplingInterval', { interval: 500 });
  await cdp.send('Profiler.start');
}

// the timeline: one row per half second until stable completion or timeout
const t0 = performance.now();
const rows = [];
let stableRun = 0, complete = false;
const hdr = '    t  loaded/wanted  stub infl stuck fail  lvl  bulk:blk infl strt  fetch:infl idb net fail   fps  eng/tick/gap ms';
console.log(hdr);
for (;;) {
  await page.waitForTimeout(500);
  const st = await page.evaluate(() => window.__test.stats());
  const fs_ = await page.evaluate(() => window.__test.fetchStats());
  const ft = await page.evaluate(() => window.__test.frameTiming());
  const t = (performance.now() - t0) / 1000;
  const dtPoll = t - (rows.length ? rows[rows.length - 1].t : 0);
  const nf = ft.frames - (rows.length ? rows[rows.length - 1].frames : ft.frames);
  const per = (ms) => nf ? (ms / nf).toFixed(1) : '-';
  const row = { t, ...st, frames: ft.frames, wallFps: nf / dtPoll, engineMs: ft.engineMs, tickMs: ft.tickMs, gapMs: ft.gapMs, fetch: { inflight: fs_.inflight, idb: fs_.idbHits, net: fs_.netHits, failed: fs_.failed,
    kinds: fs_.kinds } };
  rows.push(row);
  console.log(`${t.toFixed(1).padStart(5)}  ${String(st.nodesLoaded).padStart(5)}/${String(st.nodesWanted).padEnd(6)}`
    + ` ${String(st.nodesStub).padStart(4)} ${String(st.nodesInflight).padStart(4)} ${String(st.nodesStuck).padStart(5)}`
    + ` ${String(st.nodesFailed).padStart(4)}  ${String(st.maxLevel).padStart(3)}`
    + `  ${String(st.bulksBlocked).padStart(8)} ${String(st.bulksInflight).padStart(4)} ${String(st.bulksStarted).padStart(4)}`
    + `  ${String(fs_.inflight).padStart(10)} ${String(fs_.idbHits).padStart(3)} ${String(fs_.netHits).padStart(3)}`
    + ` ${String(fs_.failed).padStart(4)}  ${(nf / dtPoll).toFixed(0).padStart(4)}  ${per(ft.engineMs)}/${per(ft.tickMs)}/${per(ft.gapMs)}`
    + `  engine: frame ${st.frameMs.toFixed(1)} = begin ${st.beginMs.toFixed(1)} bfs ${st.bfsMs.toFixed(1)} dl ${st.dlMs.toFixed(1)} evict ${st.evictMs.toFixed(1)} draw ${st.drawMs.toFixed(1)}`);
  stableRun = st.sceneComplete ? stableRun + 1 : 0;
  if (stableRun >= 3) { complete = true; break; }
  if (t * 1000 > timeoutMs) break;
}

if (cdp) {
  const { profile } = await cdp.send('Profiler.stop');
  const prof = path.join(OUT, `loadprof-${name}.cpuprofile`);
  await fs.writeFile(prof, JSON.stringify(profile));
  // self time per function: sample counts by node id, then by name
  const self = new Map();
  const byId = new Map(profile.nodes.map((n) => [n.id, n]));
  for (const id of profile.samples) {
    const n = byId.get(id);
    const key = `${n.callFrame.functionName || '(anonymous)'}  ${path.basename(n.callFrame.url || '')}:${n.callFrame.lineNumber}`;
    self.set(key, (self.get(key) || 0) + 1);
  }
  const total = profile.samples.length;
  console.log(`\ncpu profile (${total} samples, ${(profile.endTime - profile.startTime) / 1e6 | 0}s), top self time:`);
  for (const [k, v] of [...self.entries()].sort((a, b) => b[1] - a[1]).slice(0, 25))
    console.log(`  ${(100 * v / total).toFixed(1).padStart(5)}%  ${k}`);
  console.log(`  (full profile: ${path.relative(process.cwd(), prof)} — load it in devtools' performance panel)`);
}

const pending = await page.evaluate(() => window.__test.pendingNodes());
const fetchStats = await page.evaluate(() => window.__test.fetchStats());
const elapsed = (performance.now() - t0) / 1000;
console.log(`\n${complete ? 'complete' : 'TIMED OUT'} after ${elapsed.toFixed(1)}s; ${pending.length} nodes still wanted`);

if (pending.length) {
  // group by level and state; the waits say whether these are new wants
  // (the view is still refining) or old ones (something is stuck)
  const by = new Map();
  for (const p of pending) {
    const k = `L${String(p.level).padStart(2)} ${p.state === 2 ? 'downloading' : p.fails ? 'retrying   ' : 'stub       '}`;
    const g = by.get(k) || { n: 0, wait: [], dist: [], mpt: [] };
    g.n++; g.wait.push(p.waitMs); g.dist.push(p.dist); g.mpt.push(p.mpt);
    by.set(k, g);
  }
  const med = (a) => { const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
  console.log('\n  level state          n   wait med/max (s)   dist med (km)   mpt med');
  for (const [k, g] of [...by.entries()].sort())
    console.log(`  ${k} ${String(g.n).padStart(4)}   ${(med(g.wait) / 1000).toFixed(1).padStart(6)}/${(Math.max(...g.wait) / 1000).toFixed(1).padEnd(6)}`
      + `   ${(med(g.dist) / 1000).toFixed(1).padStart(8)}        ${med(g.mpt).toFixed(2)}`);
  const oldest = [...pending].sort((a, b) => b.waitMs - a.waitMs).slice(0, 12);
  console.log('\n  longest waits:');
  for (const p of oldest)
    console.log(`    ${p.path.padEnd(24)} L${p.level} ${p.state === 2 ? 'dl' : 'stub'} fails=${p.fails}`
      + ` wait=${(p.waitMs / 1000).toFixed(1)}s dist=${(p.dist / 1000).toFixed(1)}km mpt=${p.mpt.toFixed(2)}`);
}
console.log('\nfetch:', JSON.stringify({ issued: fetchStats.issued, delivered: fetchStats.delivered,
  failed: fetchStats.failed, idbHits: fetchStats.idbHits, netHits: fetchStats.netHits, maxMs: Math.round(fetchStats.maxMs) }));
for (const [k, v] of Object.entries(fetchStats.kinds))
  console.log(`  ${k}: n=${v.n} idb=${v.idb} net=${v.net} fail=${v.fail} idbMs/avg=${Math.round(v.idbMs / v.n)} netMs/avg=${v.net ? Math.round(v.netMs / v.net) : 0}`);
if (fetchStats.log.length) console.log('  failures:', fetchStats.log.slice(0, 10).join('\n    '));

const out = path.join(OUT, `loadprof-${name}.json`);
await fs.writeFile(out, JSON.stringify({ shot, warm, width, height, complete, elapsed, rows, pending, fetchStats, consoleLines }, null, 1));
console.log(`\nwrote ${path.relative(process.cwd(), out)}`);

await ctx.close();
close();
