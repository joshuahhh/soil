import { START } from './config.js';
import { view, canvas } from './engine.js';
import { saveView, inTube, setMode } from './view.js';
import { updateHint, toast, logScale, bindRange } from './hud.js';
import { saveFile } from './io.js';
import { tubeRect, setRectData } from './tube.js';

export { stl, stlApply, setStl, saveStl, stlDemo, initStl };

// --- stl export -----------------------------------------------------------
// the rect as a solid to print, in millimetres: its triangles shrink-wrapped
// into a closed surface and cut to the rect and a flat base, on a thread of
// the engine's own (earth_stl.h — cgal's alpha wrap). overhangs survive:
// the wrap seals gaps narrower than the detail and keeps wider ones open,
// and it thickens anything thinner than twice the thicken setting, so the
// model prints. while the panel is up the engine keeps the rect loaded at
// the detail the wrap can use, wherever the camera is looking. 3 opens
// it; with it up, drawing a rect only picks the rect. the panel and a
// rolled rect don't mix: opening it unrolls the tube or the body, and
// rolling (tube.js's enter) closes it. settings are part of the saved
// view; the panel starts closed, and the saved view's restore
// (view.js) opens it
const STL_DEFAULT = { on: false, size: 150, alpha: 0.6, offset: 0.2, exag: 1, base: 3 };
const stl = { ...STL_DEFAULT, ...(START.stl || {}), on: false };
// a tile's vertices are 4-5 of its texels apart at every level (measured
// over downtown seattle), so tiles a quarter of alpha per texel put a
// vertex about every alpha, as fine as the wrap resolves
const STL_TEXELS_PER_ALPHA = 4;
// the wrap's pace, points a second, for the estimate: a downtown seattle
// block took 138 s for 484k (wasm, node, one thread), set-up and clean-up
// included
const STL_POINTS_PER_S = 3500;
const stlEl = (id) => document.getElementById(id);
// the model's numbers: the rect's size on the ground (equirectangular
// about its centre, as the engine takes it), the scale, alpha on the
// ground
const stlGeom = () => {
  if (!tubeRect) return null;
  const r = tubeRect, rad = Math.PI / 180, R = view.planetRadius();
  const w = R * Math.cos((r[0] + r[2]) / 2 * rad) * (r[3] - r[1]) * rad;
  const h = R * (r[2] - r[0]) * rad;
  const mmPerM = stl.size / Math.max(w, h);
  return { w, h, mmPerM, alphaM: stl.alpha / mmPerM };
};
// what the engine was last asked to load, so a slider that doesn't change
// it (height, base, thicken) doesn't restart the walk; and the rect's
// relief, in metres, once it is all in — what turns into the model's
// height
let stlKey = '', stlRelief = null;
const stlApply = () => {
  if (!view.ready()) return;
  const g = stl.on ? stlGeom() : null;
  const key = g ? `${tubeRect.join()}|${g.alphaM}` : '';
  if (key === stlKey) return;
  stlKey = key;
  stlRelief = null;
  if (g) view.setExportRect(tubeRect[0], tubeRect[1], tubeRect[2], tubeRect[3], g.alphaM / STL_TEXELS_PER_ALPHA);
  else view.clearExportRect();
};
const fmtKm = (m) => m >= 1000 ? (m / 1000).toFixed(m >= 10000 ? 1 : 2) : `${Math.round(m)}`;
const fmtSecs = (s) => s < 90 ? `${Math.max(1, Math.round(s))} s` : `${Math.round(s / 60)} min`;
// the wrap's time, roughly, once the rect is in: about a point per
// alpha² of what it wraps — the terrain's surface, walls and all, and
// the floor and curtains it is closed with, about the footprint again
const stlWorkEstimate = (g) => {
  if (!stlRelief) return null;
  const mm2 = g.mmPerM * g.mmPerM;
  return (stlRelief.area + 1.3 * g.w * g.h) * mm2 / (stl.alpha * stl.alpha) / STL_POINTS_PER_S;
};
let stlStatus = { state: 'idle' };
let stlControls = []; // the sliders' refreshes (initStl)
const refreshStl = () => {
  for (const refresh of stlControls) refresh();
  const running = stlStatus.state === 'running';
  const btn = stlEl('stlsave');
  btn.textContent = running ? 'cancel' : 'save .stl';
  const g = stlGeom();
  btn.disabled = !g && !running;
  if (!g) { stlEl('stlstats').textContent = 'no rect'; return; }
  const info = view.ready() ? view.getExportInfo() : null;
  const unit = Math.max(g.w, g.h) >= 1000 ? ' km' : ' m';
  const lines = [
    `${fmtKm(g.w)} × ${fmtKm(g.h)}${unit} at 1:${Math.round(1000 / g.mmPerM).toLocaleString()}`,
  ];
  const mid = [];
  if (info && info.on) mid.push(`tiles ${info.nodesLoaded}/${info.nodesWanted}${info.capped ? ' (capped)' : ''}`);
  if (stlRelief) mid.push(`~${(stl.base + (stlRelief.hi - stlRelief.lo) * g.mmPerM * stl.exag).toFixed(1)} mm tall`);
  if (mid.length) lines.push(mid.join(' · '));
  if (running) {
    const s = stlStatus;
    const pct = s.phase === 'wrap' && s.estimate > 0 ? ` ${Math.min(99, Math.floor(100 * s.steiner / s.estimate))}%` : '';
    lines.push(`${{ prep: 'gathering', wrap: 'wrapping', cut: 'cutting', write: 'writing' }[s.phase]}${pct} · ${fmtSecs((performance.now() - stlStarted) / 1000)}`);
  } else {
    const secs = stlWorkEstimate(g);
    if (secs != null) lines.push(`about ${fmtSecs(secs)} to wrap`);
  }
  stlEl('stlstats').textContent = lines.join('\n');
};
let stlStarted = 0, stlName = '', stlInfo = null;
const saveStl = () => {
  if (stlStatus.state === 'running') { view.cancelStl(); return; }
  const g = stlGeom();
  if (!g || !view.ready()) return;
  const r = tubeRect;
  // (not starting with 'solid', which some readers take to mean ascii)
  const head = `soil ${((r[0] + r[2]) / 2).toFixed(5)},${((r[1] + r[3]) / 2).toFixed(5)} `
    + `${Math.round(g.w)}x${Math.round(g.h)} m, ${stl.exag}x, mm`;
  stlInfo = view.getExportInfo();
  if (!view.startStl(g.mmPerM, stl.exag, stl.base, stl.alpha, stl.offset, head)) {
    toast('nothing loaded in the rect yet');
    return;
  }
  stlName = `soil-${((r[0] + r[2]) / 2).toFixed(4)},${((r[1] + r[3]) / 2).toFixed(4)}.stl`;
  stlStarted = performance.now();
  stlStatus = view.getStlStatus();
  refreshStl();
};
// the job's end: hand the file over, or say why not
const stlFinish = (s) => {
  const secs = (performance.now() - stlStarted) / 1000;
  if (s.state === 'done') {
    const bytes = view.takeStl();
    saveFile(new Blob([bytes], { type: 'model/stl' }), stlName);
    console.log(`stl: ${s.faces} triangles${s.closed ? '' : ' (not closed!)'}, ${s.dropped} loose pieces dropped, ${s.steiner} wrap points`
      + ` (estimated ${stlStatus.estimate}), prep ${Math.round(s.msPrep)} ms, wrap ${Math.round(s.msWrap)} ms,`
      + ` cut ${Math.round(s.msCut)} ms, ${fmtSecs(secs)} in all, tiles ${stlInfo.nodesLoaded}/${stlInfo.nodesWanted}`);
    const partial = stlInfo.nodesLoaded < stlInfo.nodesWanted ? ` with ${stlInfo.nodesLoaded}/${stlInfo.nodesWanted} tiles in` : '';
    toast(`saved ${stlName} (${(bytes.byteLength / 1e6).toFixed(1)} MB)${partial}`);
  } else {
    view.clearStl();
    if (s.state === 'failed') {
      console.log(`stl: failed: ${s.error}`);
      toast(`the stl failed: ${s.error}`);
    }
  }
};
const setStl = (on, quiet = false) => {
  if (on && inTube()) setMode('fly');
  stl.on = on;
  document.body.classList.toggle('stl', on);
  setRectData(tubeRect); // the tube's arrows go, or come back
  if (on && !tubeRect && !quiet) toast('shift-drag a rect on the map');
  stlApply();
  refreshStl();
  updateHint();
  saveView();
};
// __demo.stl: patch the settings, and read back what the export sees
const stlDemo = (patch) => {
  if (patch) { Object.assign(stl, patch); stlApply(); refreshStl(); }
  return { ...stl, geom: stlGeom(), info: view.getExportInfo(), relief: stlRelief, status: view.getStlStatus() };
};

const initStl = () => {
  // every slider re-asks the engine; only size and detail change what it
  // loads, and stlApply does nothing for the rest
  const apply = () => { stlApply(); refreshStl(); saveView(); };
  stlControls = [
    bindRange('stlsize', stl, 'size', { apply, fmt: (s) => `${s} mm` }),
    bindRange('stlalpha', stl, 'alpha', { apply, scale: logScale(0.2, 4, { steps: 100, dp: 2 }),
      fmt: (a) => `${a.toFixed(2)} mm` }),
    bindRange('stloffset', stl, 'offset', { apply, scale: logScale(0.02, 0.5, { steps: 100, dp: 3 }),
      fmt: (o) => `${o.toFixed(2)} mm` }),
    bindRange('stlexag', stl, 'exag', { apply, scale: logScale(1, 10, { steps: 100, dp: 1 }),
      fmt: (e) => `${e.toFixed(1)}×` }),
    // half-millimetre notches
    bindRange('stlbase', stl, 'base', { apply, scale: { from: (v) => v / 2, to: (b) => Math.round(b * 2) },
      fmt: (b) => `${b.toFixed(1)} mm` }),
  ];
  stlEl('stlsave').addEventListener('click', saveStl);
  stlEl('stlclose').addEventListener('click', () => { setStl(false); canvas.focus(); });
  // the readout follows the download and the wrap; once the rect is all
  // in, one pass over its vertices gives its relief, and with it the
  // model's height. a wrap runs on with the panel closed, and its file
  // still arrives
  setInterval(() => {
    if (!view.ready()) return;
    if (stlStatus.state === 'running') {
      const s = view.getStlStatus();
      if (s.state === 'running') stlStatus = s;
      else { stlFinish(s); stlStatus = { state: 'idle' }; }
    }
    if (!stl.on) return;
    if (!stlRelief && tubeRect) {
      const info = view.getExportInfo();
      if (info.on && info.nodesWanted > 0 && info.nodesLoaded === info.nodesWanted) stlRelief = view.getExportRelief();
    }
    refreshStl();
  }, 250);
  refreshStl();
};
