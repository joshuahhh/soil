import { TEST } from './config.js';
import { S } from './state.js';
import { D2R, planetR, vsub, vmul, vadd, vdot, vcross, vnorm, geoUpV, geoNorthV } from './geo.js';
import { smoothSpine, makeSpine, spineAt, rawAt, spineLatLon, spineHeading,
  nearestS } from './spine.js';
import { loadStore, saveStore } from './persist.js';
import { idbGet, idbPutChecked, idbDel } from './idb.js';
import { view, canvas } from './engine.js';
import { map } from './map.js';
import { keys } from './input.js';
import { mode, ortho, defineMode, setMode } from './mode.js';

export { path, parseGpx, pushPath, savePath, setPath, loadGpxFile, spine, buildSpine,
  debugSmooth, setDebugSmooth, setFollow, toggleFollow, followStep, initPath };

// --- dropped track (gpx) --------------------------------------------------
// drop a .gpx on the window and the track is drawn twice: as a line on the
// map, and over the terrain in the 3d view (the engine's path overlay —
// see earth_core.h, which pins it to the mesh rather than to the file's own
// elevations). the panel is the handle on it: color, opacity, width, and
// the ✕ that gets rid of it. like the reference photo, the file never
// leaves the browser but is kept in indexeddb so a reload comes back to it
let path = null;   // { name, segs: [[{lat, lon, ele}, ...], ...], pts, km }
const pathStyle = { color: '#ff7f0e', opacity: 0.9, width: 3, drape: true };
const pathEls = {};
for (const k of ['name', 'stats', 'progress', 'color', 'opacity', 'width', 'drape',
  'close', 'fit', 'go', 'follow'])
  pathEls[k] = document.getElementById('path' + k);

// gpx is xml with a default namespace; a plain type selector matches an
// element in any namespace, so no namespace juggling is needed here.
// track segments and routes each become their own polyline — a gap in the
// recording is a gap in the track, not a straight line across the county
const parseGpx = (text) => {
  const doc = new DOMParser().parseFromString(text, 'application/xml');
  if (doc.querySelector('parsererror')) return null;
  const readPts = (nodes) => [...nodes].map((pt) => {
    const ele = pt.querySelector('ele');
    // an attribute that isn't there reads as null, and +null is 0 — which
    // would put a malformed point in the gulf of guinea rather than drop it
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

// great-circle length, for the readout. the elevation profile is not part
// of it: the file's elevations are the thing we trust least here
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
// one place for the style, applied to both views. the map layers may not
// exist yet (style still loading), which setPathData's guard also covers
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
// the engine wants flat [lat, lon, ele] triples with a nan triple marking
// the break between polylines. re-sent only when the points change; the
// style has its own call so a slider drag doesn't re-upload the track
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

// where you are along the track, on the map: the 3d view marks the same
// point with a disc on the line itself (the engine's path cursor), but the
// map is where "how far along am i" is actually a readable question
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
  // how you like to stand relative to the track, but neither the mode
  // itself (coming back to a reload already on rails would be a shock)
  // nor where along it you were: entering follow always re-derives that
  // from where the camera actually is, so saving it reads back as dead
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
// put the 3d camera at the start of the track, looking the way it heads.
// altitude comes from the file, which is enough to be above the ground and
// let the mesh stream in — the drape then puts the line on the terrain
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

const refreshPathPanel = () => {
  document.body.classList.toggle('path', !!path);
  pathEls.color.value = pathStyle.color;
  pathEls.opacity.value = Math.round(pathStyle.opacity * 100);
  pathEls.width.value = pathStyle.width;
  // no elevations in the file means no alternative to draping: show it on
  // and locked rather than offering a switch that would bury the track
  pathEls.drape.checked = pathStyle.drape;
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

// `fresh` marks a file the user just dropped, as opposed to one being
// restored: a drop re-picks the drape default, a restore keeps whatever
// was chosen last time
const setPath = (parsed, fresh) => {
  path = parsed;
  if (path) {
    path.pts = path.segs.reduce((n, seg) => n + seg.length, 0);
    path.km = pathLengthKm(path.segs);
    // elevations are data the file actually recorded, so they win by
    // default and draping is the fallback for when there are none. a few
    // points missing an <ele> is common enough to tolerate — each of those
    // reads as sea level, and the drape measurement fills them in — but a
    // track that is mostly missing them has nothing to plot against
    const withEle = path.segs.reduce((n, seg) =>
      n + seg.filter((p) => Number.isFinite(p.ele)).length, 0);
    path.hasEle = withEle >= path.pts * 0.9;
    if (fresh || !path.hasEle) pathStyle.drape = !path.hasEle;
  }
  buildSpine();
  // a different track means a different journey: start at its beginning
  // rather than however far along the last one you had got, and drop the
  // eased heading and altitude so they don't drift in from the old place
  fol.s = 0;
  fol.heading = fol.alt = null;
  // the debug line belongs to a particular track: redraw it for this one
  // (setPathData has just cleared the old one)
  if (debugSmooth && spine) setDebugSmooth(true);
  if (!spine) setFollow(false); // nothing left to ride
  setPathData();
  pushPath();
  refreshPathPanel();
  savePath();
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
  // and say so in the panel if it didn't stick, rather than letting the
  // next reload quietly serve whatever track was stored before this one
  const stored = await idbPutChecked('gpx-file', file);
  // another file may have been dropped while that was in flight; it owns
  // the store now, and this result says nothing about it
  if (path === parsed) { path.saved = stored; refreshPathPanel(); }
  canvas.focus(); // the drop shouldn't cost you the flight keys
};

// --- following the path ---------------------------------------------------
// V puts the camera on rails. it aims at a point that runs along the track
// (W/S drive it) from a standoff expressed in the *path's* frame rather
// than the world's: azimuth is measured from the direction the track heads,
// so rounding a bend carries the camera around with it and you keep looking
// at the same shoulder of the trail. the arrows move that standoff (left/
// right around the track, up/down in elevation), R/F change its distance.
// A/D do nothing — on rails there is nowhere sideways to go.
//
// it steers off a smoothed copy of the track, never the track itself. a gps
// fix wanders a few meters between samples, and taking the heading from one
// raw segment hands every one of those wobbles to the camera, which reads
// as the world shivering. so the track is resampled at a fixed step and
// boxcar-smoothed twice (two passes ≈ gaussian), and the heading comes from
// a chord across a couple hundred meters of *that* rather than from any one
// segment. a temporal ease on top absorbs what survives.
// the smoothing windows are fractions of what the view spans, not fixed
// distances. a fixed window is only ever right at one zoom: 20 m of
// smoothing is the difference between a steady camera and a shaky one from
// 60 m out, and nothing at all from 2 km up, where the same track reads as
// rigidly locked to its marker. at the default standoff the fractions come
// to about 20 m for the aim, 60 m for the heading, a 150 m chord.
//
// the camera aims down the lighter one: a gps fix wanders a few metres and
// the camera must not inherit that, but heavy smoothing cuts a switchback's
// corner by tens of metres, and aiming there leaves the marker (which is on
// the drawn line, where it belongs) off-centre through every bend
const AIM_SMOOTH_FRAC = 0.1;
const HEAD_SMOOTH_FRAC = 0.3;
const CHORD_FRAC = 0.6;
const SMOOTH_MIN = 8, SMOOTH_MAX = 4000; // m, either end of the scaling
const FOLLOW_TURN_TAU = 500; // ms: ease on the heading
const FOLLOW_ALT_TAU = 700;  // ms: and on the target's altitude, which steps
                             // as the drape measurements land
let spine = null;
// s: meters along the spine. az: bearing from the target to the camera,
// relative to the path's heading (0 = behind it, looking the way it goes).
// el: how far above the target the camera stands. heading/alt: the eased
// values actually used, null until the first frame seeds them
const fol = { s: 0, az: 0, el: 22, dist: 250, heading: null, alt: null };

// the smoothed copies, filtered once and kept until the window they were
// filtered at has moved appreciably. the window follows the zoom, so it
// technically changes every frame — but quantizing it costs a sub-metre
// step in the aim and saves refiltering the track for a change nobody can
// see. two boxcars over the whole spine is O(n) and only runs while you are
// actually zooming; a per-frame kernel would run always
const SMOOTH_STALE = 0.08; // refilter once the window has moved this much
const smoothCache = new Map(); // 'aim' | 'head' -> { win, xs, ys }
const smoothed = (key, win) => {
  const c = smoothCache.get(key);
  if (c && Math.abs(win - c.win) < SMOOTH_STALE * c.win) return c;
  const built = smoothSpine(spine, win);
  smoothCache.set(key, built);
  // the debug line is the aim copy itself, not a second filtering of it:
  // a debug view that can disagree with the thing it depicts is worse than
  // none at all
  if (key === 'aim' && debugSmooth) drawSmoothLayer(built);
  return built;
};

const buildSpine = () => {
  spine = makeSpine(path);
  smoothCache.clear();
};

// how much ground the view spans at the target, in metres — the scale the
// smoothing windows are cut from. a parallel projection says so outright;
// in perspective it comes from the standoff and the fov, so both the R/F
// standoff and the Z/X zoom feed it
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
// the ground the camera should be looking at, averaged over a stretch of
// track so one bad fix doesn't dip the view. the engine answers with the
// draped altitude where the mesh under that point has been measured and
// with nothing where it hasn't, so measured points win outright and the
// file's own elevations only stand in until the drape reaches there —
// averaging the two together would split the difference and park the
// target inside the hillside
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
// the smoothed copy is invisible by construction — it exists only as the
// thing the camera aims down — so when the marker floats off-centre there
// is nothing to look at to see why. this draws it on the map beside the
// track: dashed, in a contrasting color, at whatever window the current
// zoom is asking for. the gap between the two lines *is* the float
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
  // draw whatever the camera is following right now; if it has not been
  // following anything yet, filter at the window it would use
  if (on && spine) drawSmoothLayer(smoothed('aim', winFor(AIM_SMOOTH_FRAC, followSpan())));
  canvas.focus();
};

// riding the track replaces the flight controls (see the tick). terrain
// hug goes on the way in (mode.js): it re-eases the eye toward a held
// height above the mesh inside every fly() call, and the rails write the
// pose outright, so the two fight over the same thing. ortho stays — a
// parallel projection has no quarrel with the rails, it just changes what
// R/F and a drag have to do (see followStep)
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

// one frame of the follow camera; replaces the flight controls entirely
// while it is on (see the tick)
const followStep = (dt, slow) => {
  const k = (slow ? 0.15 : 1) * dt / 1000;
  // a drag on the view swings the standoff, the same grab-the-world way it
  // turns the head in free flight: pull right and the track goes right,
  // which means the camera walks left around it. yaw/pitch carry only the
  // pointer here — the tick keeps the arrow keys out of them on the rails
  if (S.yaw || S.pitch) {
    fol.az += S.yaw / D2R;
    fol.el = Math.min(85, Math.max(-10, fol.el + S.pitch / D2R));
  }
  // one notion of scale for everything that has to follow the zoom: how
  // much ground the view spans (see followSpan). travel is scaled to it so
  // motion looks the same on screen at any zoom — from 20 m out a hiking
  // pace is right, from 5 km up it would take all day. this used to scale
  // to the standoff alone, which is proportional to the span at a fixed fov
  // and so felt right until you touched a zoom: Z/X moves the fov, and in
  // ortho the wheel and Z/X move the extent, none of which is the standoff
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
    // a parallel projection doesn't care how far back the camera stands, so
    // in ortho the standoff is invisible and R/F have to move the extent
    // instead — the way they do outside follow. the standoff still rides
    // along, so the framing survives a switch back to perspective and the
    // travel speed (which scales with it) tracks what you can actually see
    if (ortho) view.zoomOrtho(f);
    fol.dist = Math.min(50000, Math.max(15, fol.dist / f));
  }

  // the smoothing windows for this frame, cut from that same span: zoom out
  // and the camera rides a longer average of the track, which is what keeps
  // the same track from reading as rigid at altitude and shaky up close
  const aimWin = winFor(AIM_SMOOTH_FRAC, span);
  const headWin = winFor(HEAD_SMOOTH_FRAC, span);
  const chord = winFor(CHORD_FRAC, span);
  const aimSm = smoothed('aim', aimWin), headSm = smoothed('head', headWin);
  // the heading is the one thing still taken from the smoothed spine — the
  // jitter that matters is in the *direction* between raw points, not in
  // their positions. its arc length runs a hair short of the raw track's
  // (smoothing cuts corners), so the position is scaled across
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

  // the aim point is on the raw track, not the smoothed copy: the ribbon is
  // drawn through the raw points, and a marker placed off the smoothed one
  // visibly leaves the line at every bend. the 3d disc is placed by point
  // index so the engine can put it on the drawn geometry exactly, drape and
  // all; the map dot and the camera get the interpolated position
  const ll = rawAt(spine, fol.s);
  setCursor(ll);
  view.setPathCursorIndex(ll.idx);
  pathEls.progress.textContent = `${(fol.s / 1000).toFixed(2)} of `
    + `${(spine.rawLen / 1000).toFixed(2)} km · `
    + `${Math.round(100 * fol.s / Math.max(1, spine.rawLen))}%`
    + (debugSmooth ? ` · smooth ${Math.round(aimWin)}/${Math.round(headWin)} m` : '');
  // the camera aims down its own lightly smoothed copy, never the raw
  // track: the marker has to be on the drawn line, the camera has to not
  // shake, and those are different requirements
  const aim = spineLatLon(spine, spineAt(spine, sSpine, aimSm));
  const lat = aim.lat * D2R, lon = aim.lon * D2R;
  const up = geoUpV(lat, lon), north = geoNorthV(lat, lon), east = vcross(north, up);
  const bearing = ((fol.heading || 0) + fol.az) * D2R;
  const horiz = vadd(vmul(north, Math.cos(bearing)), vmul(east, Math.sin(bearing)));
  const target = vmul(up, planetR + (fol.alt || 0));
  // the camera stands off *behind* the target along that bearing, lifted by
  // the elevation angle — so az 0 is a chase view down the track
  const el = fol.el * D2R;
  const eye = vadd(target, vadd(vmul(horiz, -fol.dist * Math.cos(el)),
    vmul(up, fol.dist * Math.sin(el))));
  // back into the pose vocabulary: where that put the camera, and the aim
  // from there to the target — which is not quite the bearing above once
  // the planet has curved between the two
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
    // start and end, so a track you are looking at end-on still says which
    // way it runs
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
  pathEls.color.addEventListener('input', () => {
    pathStyle.color = pathEls.color.value;
    applyPathStyle();
    savePath();
  });
  pathEls.opacity.addEventListener('input', () => {
    pathStyle.opacity = +pathEls.opacity.value / 100;
    applyPathStyle();
    savePath();
  });
  pathEls.width.addEventListener('input', () => {
    pathStyle.width = +pathEls.width.value;
    applyPathStyle();
    savePath();
  });
  pathEls.drape.addEventListener('change', () => {
    pathStyle.drape = pathEls.drape.checked;
    applyPathStyle();
    savePath();
  });
  pathEls.fit.addEventListener('click', fitPath);
  pathEls.go.addEventListener('click', flyToPathStart);
  pathEls.follow.addEventListener('click', toggleFollow);

  // restore. skipped in test mode for the same reason the saved pose is: a
  // capture must not depend on what the last interactive session dropped
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
