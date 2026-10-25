import { TEST, S, loadStore, saveStore } from './config.js';
import { D2R, planetR, vsub, vmul, vadd, vdot, vcross, vnorm, geoUpV, geoNorthV, smoothSpine,
  makeSpine, spineAt, rawAt, spineLatLon, spineHeading, nearestS } from './math.js';
import { idbGet, idbPutChecked, idbDel } from './tiles.js';
import { view, canvas } from './engine.js';
import { keys, mode, ortho, defineMode, setMode } from './view.js';
import { updateHint, percent, bindRange, bindCheck } from './hud.js';
import { map } from './map.js';

export { path, parseGpx, pushPath, savePath, setPath, loadGpxFile, spine, buildSpine, debugSmooth,
  setDebugSmooth, setFollow, toggleFollow, followStep, initPath };

// --- dropped track (gpx) --------------------------------------------------
let path = null;   // { name, segs: [[{lat, lon, ele}, ...], ...], pts, km }
const pathStyle = { color: '#ff7f0e', opacity: 0.9, width: 3, drape: true };
const pathEls = {};
for (const k of ['name', 'stats', 'progress', 'color', 'opacity', 'width', 'drape',
  'close', 'fit', 'go', 'follow'])
  pathEls[k] = document.getElementById('path' + k);

// plain type selectors match despite gpx's default namespace
const parseGpx = (text) => {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  if (doc.querySelector('parsererror')) return null;
  const readPts = (nodes) => [...nodes].map((pt) => {
    const ele = pt.querySelector('ele');
    // not +getAttribute: +null is 0, which lands a malformed point at 0,0
    const num = (a) => (pt.hasAttribute(a) ? parseFloat(pt.getAttribute(a)) : NaN);
    return {
      lat: num('lat'), lon: num('lon'),
      ele: ele ? parseFloat(ele.textContent) : NaN,
    };
  }).filter((p) => Number.isFinite(p.lat) && Number.isFinite(p.lon));
  const segs = [];
  for (const seg of doc.querySelectorAll('trkseg')) {
    const pts = readPts(seg.querySelectorAll('trkpt'));
    if (pts.length) segs.push(pts);
  }
  for (const rte of doc.querySelectorAll('rte')) {
    const pts = readPts(rte.querySelectorAll('rtept'));
    if (pts.length) segs.push(pts);
  }
  if (!segs.length) return null;
  const nameEl = doc.querySelector('trk > name') || doc.querySelector('metadata > name')
    || doc.querySelector('rte > name');
  return { name: (nameEl && nameEl.textContent.trim()) || '', segs };
};

const pathLengthKm = (segs) => {
  let m = 0;
  for (const seg of segs) {
    for (let i = 1; i < seg.length; i++) {
      const a = seg[i - 1], b = seg[i];
      const dlat = (b.lat - a.lat) * Math.PI / 180;
      const dlon = (b.lon - a.lon) * Math.PI / 180 * Math.cos((a.lat + b.lat) / 2 * Math.PI / 180);
      m += Math.hypot(dlat, dlon) * 6371010;
    }
  }
  return m / 1000;
};

const pathFC = () => {
  if (!path) return { type: 'FeatureCollection', features: [] };
  const feats = path.segs.map((seg) => ({ type: 'Feature', properties: {},
    geometry: { type: 'LineString', coordinates: seg.map((p) => [p.lon, p.lat]) } }));
  const first = path.segs[0][0];
  const lastSeg = path.segs[path.segs.length - 1];
  const last = lastSeg[lastSeg.length - 1];
  for (const [end, p] of [['start', first], ['end', last]])
    feats.push({ type: 'Feature', properties: { end },
      geometry: { type: 'Point', coordinates: [p.lon, p.lat] } });
  return { type: 'FeatureCollection', features: feats };
};
const setPathData = () => {
  const src = map.getSource('gpx');
  if (src) src.setData(pathFC());
  const dbg = map.getSource('gpx-smooth');
  if (dbg) dbg.setData({ type: 'FeatureCollection', features: [] });
};
const applyPathStyle = () => {
  const rgb = parseInt(pathStyle.color.slice(1), 16);
  cursorEl.style.borderColor = pathStyle.color;
  if (view.ready())
    view.setPathStyle(rgb, pathStyle.opacity, pathStyle.width, pathStyle.drape);
  if (map.getLayer('gpx-line')) {
    map.setPaintProperty('gpx-line', 'line-color', pathStyle.color);
    map.setPaintProperty('gpx-line', 'line-width', pathStyle.width);
    map.setPaintProperty('gpx-line', 'line-opacity', pathStyle.opacity);
    map.setPaintProperty('gpx-ends', 'circle-color',
      ['case', ['==', ['get', 'end'], 'start'], '#fff', pathStyle.color]);
    map.setPaintProperty('gpx-ends', 'circle-stroke-color', pathStyle.color);
    map.setPaintProperty('gpx-ends', 'circle-opacity', pathStyle.opacity);
    map.setPaintProperty('gpx-ends', 'circle-stroke-opacity', pathStyle.opacity);
  }
};
// engine format: flat [lat, lon, ele] triples, a nan triple between polylines
const pushPath = () => {
  if (!view.ready()) return;
  if (!path) { view.clearPath(); return; }
  const flat = new Float64Array(path.pts * 3 + (path.segs.length - 1) * 3);
  let i = 0;
  path.segs.forEach((seg, si) => {
    if (si) { flat[i++] = NaN; flat[i++] = NaN; flat[i++] = NaN; }
    for (const p of seg) { flat[i++] = p.lat; flat[i++] = p.lon; flat[i++] = p.ele; }
  });
  view.setPath(flat);
  applyPathStyle();
};

const cursorEl = document.createElement('div');
cursorEl.style.cssText = 'width:13px;height:13px;border-radius:50%;background:#fff;'
  + 'border:3px solid #ff7f0e;box-sizing:border-box;pointer-events:none;'
  + 'box-shadow:0 0 0 1px rgba(0,0,0,.45);';
const cursorMarker = new maplibregl.Marker({ element: cursorEl });
let cursorOn = false;
const setCursor = (ll) => {
  if (!ll) {
    if (cursorOn) { cursorMarker.remove(); cursorOn = false; }
    if (view.ready()) view.clearPathCursor();
    return;
  }
  cursorMarker.setLngLat([ll.lon, ll.lat]);
  if (!cursorOn) { cursorMarker.addTo(map); cursorOn = true; }
};

const savePath = () => saveStore('earth-path', {
  style: pathStyle, name: path ? path.name : null,
  // not the mode (a reload shouldn't land on rails), and not s: entering
  // follow re-derives it from the camera
  follow: { az: fol.az, el: fol.el, dist: fol.dist },
});
const pathBounds = () => {
  if (!path) return null;
  const b = new maplibregl.LngLatBounds();
  for (const seg of path.segs) for (const p of seg) b.extend([p.lon, p.lat]);
  return b;
};
const fitPath = () => {
  const b = pathBounds();
  if (!b) return;
  S.following = false; // the map is showing the track now, not the camera
  map.fitBounds(b, { padding: 40, animate: false, maxZoom: 16 });
};
// the file's altitude is enough to clear the ground while the mesh streams in
const flyToPathStart = () => {
  if (!path || !view.ready()) return;
  const seg = path.segs[0];
  const a = seg[0];
  const b = seg[Math.min(seg.length - 1, 20)];
  const heading = Math.atan2((b.lon - a.lon) * Math.cos(a.lat * Math.PI / 180),
    b.lat - a.lat) * 180 / Math.PI;
  view.setPose(a.lat, a.lon, (Number.isFinite(a.ele) ? a.ele : 0) + 400, heading, -20);
  S.following = true;
  canvas.focus();
};

let styleControls = []; // bound in initPath
const refreshPathPanel = () => {
  document.body.classList.toggle('path', !!path);
  pathEls.color.value = pathStyle.color;
  for (const refresh of styleControls) refresh();
  pathEls.drape.disabled = !!path && !path.hasEle;
  pathEls.drape.parentElement.title = pathEls.drape.disabled
    ? 'this file records no elevations, so the track has to be laid on the mesh'
    : 'lay the track on the mesh instead of at the elevations the file records';
  pathEls.follow.classList.toggle('primary', mode === 'follow');
  if (!path) return;
  pathEls.name.textContent = path.name || 'path';
  pathEls.name.title = path.name || '';
  pathEls.stats.textContent = `${path.pts} pts · ${path.km.toFixed(1)} km`
    + (path.segs.length > 1 ? ` · ${path.segs.length} segments` : '')
    + (path.saved === false ? ' · NOT SAVED' : '');
  pathEls.stats.title = path.saved === false
    ? "this track couldn't be written to the browser's storage (it's probably"
      + ' full) — a reload will come back to whatever was stored before it'
    : '';
};

// fresh: just dropped, so re-pick the drape default; a restore keeps the saved one
const setPath = (parsed, fresh) => {
  path = parsed;
  if (path) {
    path.pts = path.segs.reduce((n, seg) => n + seg.length, 0);
    path.km = pathLengthKm(path.segs);
    // a few points missing <ele> is common and the drape fills them in
    const withEle = path.segs.reduce((n, seg) =>
      n + seg.filter((p) => Number.isFinite(p.ele)).length, 0);
    path.hasEle = withEle >= path.pts * 0.9;
    if (fresh || !path.hasEle) pathStyle.drape = !path.hasEle;
  }
  buildSpine();
  fol.s = 0;
  fol.heading = fol.alt = null;
  if (debugSmooth && spine) setDebugSmooth(true);
  if (!spine) setFollow(false); // nothing left to ride
  setPathData();
  pushPath();
  refreshPathPanel();
  savePath();
  updateHint(); // V comes and goes with the track
};
const clearPath = () => {
  setFollow(false);
  setPath(null);
  idbDel('gpx-file', 'app');
  canvas.focus();
};

const loadGpxFile = async (file) => {
  const parsed = parseGpx(await file.text());
  if (!parsed) { alert('could not read a track out of ' + file.name); return; }
  if (!parsed.name) parsed.name = file.name.replace(/\.gpx$/i, '');
  setPath(parsed, true);
  fitPath();
  const stored = await idbPutChecked('gpx-file', file);
  // a later drop may own the store by now
  if (path === parsed) { path.saved = stored; refreshPathPanel(); }
  canvas.focus(); // the drop shouldn't cost you the flight keys
};

// --- following the path ---------------------------------------------------
// smoothing windows are fractions of the view span, not distances: a fixed
// window is only right at one zoom. at the default standoff ≈ 20 m aim, 60 m
// heading, 150 m chord. keep the aim light: heavier smoothing cuts switchback
// corners and leaves the marker off-centre through every bend
const AIM_SMOOTH_FRAC = 0.1;
const HEAD_SMOOTH_FRAC = 0.3;
const CHORD_FRAC = 0.6;
const SMOOTH_MIN = 8, SMOOTH_MAX = 4000; // m, either end of the scaling
const FOLLOW_TURN_TAU = 500; // ms: ease on the heading
const FOLLOW_ALT_TAU = 700;  // ms: on the altitude, which steps as drape measurements land
let spine = null;
// az: target-to-camera bearing relative to the track's heading (0 = chase).
// heading/alt: the eased values, null until the first frame seeds them
const fol = { s: 0, az: 0, el: 22, dist: 250, heading: null, alt: null };

// the window moves every frame with the zoom; quantizing costs a sub-metre aim step
const SMOOTH_STALE = 0.08; // refilter once the window has moved this much
const smoothCache = new Map(); // 'aim' | 'head' -> { win, xs, ys }
const smoothed = (key, win) => {
  const c = smoothCache.get(key);
  if (c && Math.abs(win - c.win) < SMOOTH_STALE * c.win) return c;
  const built = smoothSpine(spine, win);
  smoothCache.set(key, built);
  // the debug line is this copy, never a second filtering of it
  if (key === 'aim' && debugSmooth) drawSmoothLayer(built);
  return built;
};

const buildSpine = () => {
  spine = makeSpine(path);
  smoothCache.clear();
};

// metres of ground the view spans at the target: the scale for windows and speed
const followSpan = () => {
  if (ortho) {
    const e = view.getOrthoExtent();
    if (e > 0) return 2 * e;
  }
  const fov = view.ready() ? view.getPose().fov : 45;
  return Math.max(20, 2 * fol.dist * Math.tan(fov * D2R / 2));
};
const winFor = (frac, span) =>
  Math.min(SMOOTH_MAX, Math.max(SMOOTH_MIN, frac * span));
// drape-measured altitudes win outright over the file's: averaging the two
// parks the target inside the hillside
const spineAlt = (s) => {
  const at = (j) => Math.round(rawAt(spine, s + j * 25).idx);
  let sum = 0, n = 0;
  for (let j = -3; j <= 3; j++) {
    const a = view.getPathGroundAlt(at(j));
    if (Number.isFinite(a)) { sum += a; n++; }
  }
  if (n) return sum / n;
  for (let j = -3; j <= 3; j++) {
    const e = spine.raw[at(j)].ele;
    if (Number.isFinite(e)) { sum += e; n++; }
  }
  return n ? sum / n : null;
};
// --- debug: draw what the camera is actually following ---------------------
let debugSmooth = false;
const drawSmoothLayer = (sm) => {
  const src = map.getSource('gpx-smooth');
  if (!src || !spine) return;
  const coords = [];
  for (let i = 0; i < sm.xs.length; i++) {
    const ll = spineLatLon(spine, [sm.xs[i], sm.ys[i]]);
    coords.push([ll.lon, ll.lat]);
  }
  src.setData({ type: 'Feature', properties: { win: sm.win },
    geometry: { type: 'LineString', coordinates: coords } });
};
const setDebugSmooth = (on) => {
  debugSmooth = on;
  const src = map.getSource('gpx-smooth');
  if (src && !on) src.setData({ type: 'FeatureCollection', features: [] });
  if (on && spine) drawSmoothLayer(smoothed('aim', winFor(AIM_SMOOTH_FRAC, followSpan())));
  canvas.focus();
};

const followHooks = {
  enter: () => {
    fol.heading = fol.alt = null; // don't ease out of a stale value
    fol.s = nearestS(spine, view.getPose());
    S.following = true; // let the map track the camera along
    canvas.focus();
    document.body.classList.add('riding');
    refreshPathPanel();
    savePath();
  },
  exit: () => {
    fol.heading = fol.alt = null;
    setCursor(null);
    document.body.classList.remove('riding');
    refreshPathPanel();
    savePath();
  },
};
// only from free flight: not inside the tube or the tunnel
const setFollow = (on) => {
  if (on && (!spine || !view.ready() || (mode !== 'fly' && mode !== 'follow'))) on = false;
  if (on === (mode === 'follow')) { refreshPathPanel(); return; }
  setMode(on ? 'follow' : 'fly');
};
const toggleFollow = () => setFollow(mode !== 'follow');

// one frame of the follow camera, in place of the flight controls
const followStep = (dt, slow) => {
  const k = (slow ? 0.15 : 1) * dt / 1000;
  // on the rails S.yaw/pitch carry only the pointer: the tick keeps the arrows out
  if (S.yaw || S.pitch) {
    fol.az += S.yaw / D2R;
    fol.el = Math.min(85, Math.max(-10, fol.el + S.pitch / D2R));
  }
  // scale travel to the span, not the standoff: Z/X move the fov, and in
  // ortho the wheel and Z/X move the extent, neither of which is the standoff
  const span = followSpan();
  const speed = Math.max(15, span * 0.6) * S.gain;
  if (keys.KeyW) fol.s += speed * k;
  if (keys.KeyS) fol.s -= speed * k;
  fol.s = Math.min(spine.rawLen, Math.max(0, fol.s));
  if (keys.ArrowRight) fol.az += 60 * k;
  if (keys.ArrowLeft) fol.az -= 60 * k;
  if (keys.ArrowUp) fol.el = Math.min(85, fol.el + 40 * k);
  if (keys.ArrowDown) fol.el = Math.max(-10, fol.el - 40 * k);
  if (!!keys.KeyR !== !!keys.KeyF) {
    // >1 zooms in, matching zoomOrtho's sense; R backs off, F closes in
    const f = Math.pow(1.8, (keys.KeyR ? -1 : 1) * (slow ? 0.15 : 1) * dt / 1000);
    // ortho ignores the standoff, so R/F move the extent; dist rides along
    // so the framing survives a switch back to perspective
    if (ortho) view.zoomOrtho(f);
    fol.dist = Math.min(50000, Math.max(15, fol.dist / f));
  }

  const aimWin = winFor(AIM_SMOOTH_FRAC, span);
  const headWin = winFor(HEAD_SMOOTH_FRAC, span);
  const chord = winFor(CHORD_FRAC, span);
  const aimSm = smoothed('aim', aimWin), headSm = smoothed('head', headWin);
  // the spine runs a hair shorter than the raw track, so s is scaled across
  const sSpine = fol.s * (spine.len / Math.max(1, spine.rawLen));
  const h = spineHeading(spine, sSpine, headSm, chord);
  if (h !== null) {
    if (fol.heading === null) fol.heading = h;
    else {
      // shortest way round, so crossing north doesn't spin the long way
      const d = ((h - fol.heading + 540) % 360) - 180;
      fol.heading += d * (1 - Math.exp(-dt / FOLLOW_TURN_TAU));
    }
  }
  const a = spineAlt(fol.s);
  if (a !== null)
    fol.alt = fol.alt === null ? a
      : fol.alt + (a - fol.alt) * (1 - Math.exp(-dt / FOLLOW_ALT_TAU));

  // the marker goes on the raw track, where the ribbon is drawn; the 3d disc
  // by point index, so the engine puts it on the drawn geometry, drape and all
  const ll = rawAt(spine, fol.s);
  setCursor(ll);
  view.setPathCursorIndex(ll.idx);
  pathEls.progress.textContent = `${(fol.s / 1000).toFixed(2)} of `
    + `${(spine.rawLen / 1000).toFixed(2)} km · `
    + `${Math.round(100 * fol.s / Math.max(1, spine.rawLen))}%`
    + (debugSmooth ? ` · smooth ${Math.round(aimWin)}/${Math.round(headWin)} m` : '');
  // but the camera aims down the smoothed copy, so it doesn't shake
  const aim = spineLatLon(spine, spineAt(spine, sSpine, aimSm));
  const lat = aim.lat * D2R, lon = aim.lon * D2R;
  const up = geoUpV(lat, lon), north = geoNorthV(lat, lon), east = vcross(north, up);
  const bearing = ((fol.heading || 0) + fol.az) * D2R;
  const horiz = vadd(vmul(north, Math.cos(bearing)), vmul(east, Math.sin(bearing)));
  const target = vmul(up, planetR + (fol.alt || 0));
  const el = fol.el * D2R;
  const eye = vadd(target, vadd(vmul(horiz, -fol.dist * Math.cos(el)),
    vmul(up, fol.dist * Math.sin(el))));
  // the aim is recomputed at the eye: the planet curves between eye and target
  const r = Math.hypot(eye[0], eye[1], eye[2]);
  const eu = vmul(eye, 1 / r);
  const clamp1 = (v) => Math.min(1, Math.max(-1, v));
  const clat = Math.asin(clamp1(eu[2])), clon = Math.atan2(eu[1], eu[0]);
  const v = vnorm(vsub(target, eye));
  if (!v) return;
  const cnorth = geoNorthV(clat, clon), ceast = vcross(cnorth, eu);
  view.setPose(clat / D2R, clon / D2R, r - planetR,
    Math.atan2(vdot(v, ceast), vdot(v, cnorth)) / D2R,
    Math.asin(clamp1(vdot(v, eu))) / D2R);
};

const initPath = () => {
  defineMode('follow', followHooks);

  map.on('load', () => {
    map.addSource('gpx', { type: 'geojson', data: pathFC() });
    map.addLayer({ id: 'gpx-line', type: 'line', source: 'gpx',
      filter: ['==', ['geometry-type'], 'LineString'],
      layout: { 'line-cap': 'round', 'line-join': 'round' },
      paint: { 'line-color': pathStyle.color, 'line-width': pathStyle.width,
        'line-opacity': pathStyle.opacity } });
    // start and end, so an end-on track still says which way it runs
    map.addLayer({ id: 'gpx-ends', type: 'circle', source: 'gpx',
      filter: ['==', ['geometry-type'], 'Point'],
      paint: { 'circle-radius': 5,
        'circle-color': ['case', ['==', ['get', 'end'], 'start'], '#fff', pathStyle.color],
        'circle-stroke-width': 2, 'circle-stroke-color': pathStyle.color,
        'circle-opacity': pathStyle.opacity, 'circle-stroke-opacity': pathStyle.opacity } });
    map.addSource('gpx-smooth', { type: 'geojson',
      data: { type: 'FeatureCollection', features: [] } });
    map.addLayer({ id: 'gpx-smooth-line', type: 'line', source: 'gpx-smooth',
      layout: { 'line-cap': 'round', 'line-join': 'round' },
      paint: { 'line-color': '#22d3ee', 'line-width': 2, 'line-dasharray': [2, 2] } });
    setPathData();
  });

  pathEls.close.addEventListener('click', clearPath);
  const restyle = () => { applyPathStyle(); savePath(); };
  // the swatch is a string, not a number: bound by hand
  pathEls.color.addEventListener('input', () => { pathStyle.color = pathEls.color.value; restyle(); });
  styleControls = [
    bindRange('pathopacity', pathStyle, 'opacity', { scale: percent, apply: restyle }),
    bindRange('pathwidth', pathStyle, 'width', { apply: restyle }),
    bindCheck('pathdrape', pathStyle, 'drape', restyle),
  ];
  pathEls.fit.addEventListener('click', fitPath);
  pathEls.go.addEventListener('click', flyToPathStart);
  pathEls.follow.addEventListener('click', toggleFollow);

  // not in test mode: a capture mustn't depend on what the last session dropped
  (async () => {
    if (TEST) return;
    const saved = loadStore('earth-path');
    if (saved && saved.style) Object.assign(pathStyle, saved.style);
    if (saved && saved.follow) Object.assign(fol, saved.follow, { heading: null, alt: null });
    refreshPathPanel();
    const file = await idbGet('gpx-file', 'app');
    if (!file) return;
    const parsed = parseGpx(await file.text());
    if (parsed) {
      if (!parsed.name) parsed.name = (saved && saved.name) || '';
      setPath(parsed); // pushPath is a no-op until the planet is ready; tick retries
    } else {
      idbDel('gpx-file', 'app');
    }
  })();
};
