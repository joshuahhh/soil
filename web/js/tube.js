import { S } from './state.js';
import { view } from './engine.js';
import { map } from './map.js';
import { toast } from './hud.js';
import { mode, inTube, defineMode, setMode } from './mode.js';
import { ensureBody } from './body.js';

export { tubeRect, tubeAxisEw, tubePending, rectShape, setRectData, rollRect, clearRect,
  advanceTube, tubeState, restoreTube, initTube };

// --- tube mode ------------------------------------------------------------
// shift+drag on the map draws a rectangle; that slab of territory is rolled
// into a tube (the inception effect) and the camera teleports to one end of
// the tube axis, looking down it. T rolls/unrolls; drawing a new rect
// replaces the old one. the roll animates via curl (0 flat .. 1 closed)
let tubeRect = null; // [latMin, lonMin, latMax, lonMax] degrees
// which way the tube axis runs; the rolled-up (glued) edges are the two
// edges parallel to it. toggled by the #axisbtn switch
let tubeAxisEw = true;
let tubePendingSince = 0;
const normRect = (a, b) => [Math.min(a.lat, b.lat), Math.min(a.lng, b.lng),
  Math.max(a.lat, b.lat), Math.max(a.lng, b.lng)];
// the rect plus topology-diagram arrows on the two glued edges (the ones
// that meet at the overhead seam), same direction on both — the cylinder
// identification, the way quotient-space diagrams mark it
const rectFC = (r) => {
  if (!r) return { type: 'FeatureCollection', features: [] };
  const feats = [{ type: 'Feature', properties: { kind: 'rect' },
    geometry: { type: 'Polygon', coordinates: [[
      [r[1], r[0]], [r[3], r[0]], [r[3], r[2]], [r[1], r[2]], [r[1], r[0]],
    ]] } }];
  const latc = (r[0] + r[2]) / 2, lonc = (r[1] + r[3]) / 2;
  const cosLat = Math.cos(latc * Math.PI / 180);
  const tri = (pts) => feats.push({ type: 'Feature', properties: { kind: 'arrow' },
    geometry: { type: 'Polygon', coordinates: [pts.concat([pts[0]])] } });
  if (tubeAxisEw) {
    // glued edges run east-west; both arrows point east
    const s = 0.09 * (r[3] - r[1]); // arrow half-length, degrees lon
    const h = 0.55 * s * cosLat; // half-height in degrees lat (same meters)
    for (const lat of [r[0], r[2]])
      tri([[lonc + s, lat], [lonc - 0.6 * s, lat + h], [lonc - 0.6 * s, lat - h]]);
  } else {
    // glued edges run north-south; both arrows point north
    const s = 0.09 * (r[2] - r[0]);
    const h = 0.55 * s / cosLat;
    for (const lon of [r[1], r[3]])
      tri([[lon, latc + s], [lon + h, latc - 0.6 * s], [lon - h, latc - 0.6 * s]]);
  }
  return { type: 'FeatureCollection', features: feats };
};
const setRectData = (r) => {
  const src = map.getSource('tube-rect');
  if (src) src.setData(rectFC(r));
};
const axisBtn = document.getElementById('axisbtn');
const updateAxisBtn = () => {
  axisBtn.textContent = `tube axis: ${tubeAxisEw ? 'E–W' : 'N–S'} (click to flip)`;
};
// fly to the near end of the tube axis: the axis runs along the rect's
// longer dimension at height = tube radius, so spawning at (end, radius)
// heading along the axis puts you dead center in the pipe, looking down it
const flyIntoTube = (r) => {
  const latc = (r[0] + r[2]) / 2, lonc = (r[1] + r[3]) / 2;
  const ew = tubeAxisEw;
  // on the tube axis (ground + radius), a step in from the mouth
  const info = view.getTubeInfo();
  const alt = info.groundAlt + Math.max(20, info.radius);
  const inset = 0.05;
  view.setPose(
    ew ? latc : r[0] + inset * (r[2] - r[0]),
    ew ? r[1] + inset * (r[3] - r[1]) : lonc,
    alt, ew ? 90 : 0, -5);
  S.following = true;
};
// the figure stands on the rect's centre, as tall as the rect is long,
// facing across it (up × axis: north for an E–W axis, west for N–S).
// spawn out in front of it at chest height, looking back level, far
// enough that the whole figure fits the default fov
const flyOntoBody = (r) => {
  const latc = (r[0] + r[2]) / 2, lonc = (r[1] + r[3]) / 2;
  const info = view.getTubeInfo();
  const h = 2 * info.halfLen; // the figure's height, m
  const D = 1.4 * h;
  const R = 6371010;
  const dlat = (D / R) * 180 / Math.PI;
  const dlon = (D / (R * Math.cos(latc * Math.PI / 180))) * 180 / Math.PI;
  if (tubeAxisEw) view.setPose(latc + dlat, lonc, info.groundAlt + 0.55 * h, 180, 0);
  else view.setPose(latc, lonc - dlon, info.groundAlt + 0.55 * h, 90, 0);
  S.following = true;
};
// T rolls the rect into a tube, B wraps it onto the body (shape is 'tube'
// or 'body'). the body's figure is fetched and baked on first use; the roll
// waits for it (a cylinder standing in meanwhile would be the wrong
// picture), and falls back to the tube if it can't be had
// the rect's shape, 'tube' or 'body': the last one it was rolled into,
// which a new rect drawn over it is rolled into as well
let rectShape = 'tube';
const rollRect = (shape, teleport) => {
  rectShape = shape;
  if (shape === 'body' && !view.bodyReady()) {
    const r = tubeRect;
    ensureBody().then((ok) => {
      if (!ok) toast('the body mesh failed to load');
      if (tubeRect === r) rollRect(ok ? 'body' : 'tube', teleport);
    });
    return;
  }
  setMode(shape, { teleport });
};
// staged pipeline: the sheet stays flat while the rect's tiles load at
// full res; once the scene is complete the engine measures ground level
// off the mesh, and only then does the roll (and camera fly-in) happen.
// 'fly' also teleports into the tube mouth; 'roll' just rolls in place
let tubePending = null; // null | 'fly' | 'roll'
// the tube and the body share these: switching between the two re-enters
// with the other warp, and leaving either goes back to flat flight over
// the rect, which stays drawn
const tubeHooks = {
  enter: ({ teleport }) => {
    const r = tubeRect;
    view.setTubeBody(mode === 'body');
    view.setTubeRect(r[0], r[1], r[2], r[3], tubeAxisEw);
    view.setTubeEnabled(true);
    view.setAirplane(true); // tube flying is airplane-frame
    S.curl = 0; // flat while the rect loads; the tick rolls it when ready
    view.setTubeCurl(0);
    tubePending = teleport ? 'fly' : 'roll';
    tubePendingSince = performance.now();
  },
  exit: () => {
    tubePending = null;
    view.setTubeEnabled(false);
    view.setAirplane(false);
  },
};
// the rect's part of the saved view, null without one
const tubeState = () => tubeRect
  ? { rect: tubeRect, on: inTube(), ewAxis: tubeAxisEw, body: rectShape === 'body' } : null;
// and back: a rect that was rolled rolls up again from flat, in place (the
// camera pose is already wherever it was saved). says whether it did
const restoreTube = (t) => {
  if (!t || !t.rect) return false;
  tubeRect = t.rect;
  tubeAxisEw = t.ewAxis !== false;
  rectShape = t.body ? 'body' : 'tube';
  updateAxisBtn();
  setRectData(tubeRect);
  if (t.on) rollRect(rectShape, false);
  else view.setTubeRect(tubeRect[0], tubeRect[1], tubeRect[2], tubeRect[3], tubeAxisEw);
  return !!t.on;
};
// esc: unroll, drop the rect, back to normal flight
const clearRect = () => {
  tubeRect = null;
  rectShape = 'tube';
  if (inTube()) setMode('fly');
  setRectData(null);
};
// stage 2+3, from the frame loop: the engine locks ground level as soon as
// the rect is covered at measurement resolution (well before full res),
// then we fly in and roll while the rest keeps streaming into the rolled
// tube. after 20s (a download stuck or failed), force the measurement with
// what's resident. returns the tube's info, for the loading readout
const advanceTube = (now) => {
  let ti = view.getTubeInfo();
  if (tubePending) {
    if (now - tubePendingSince > 20000 && !ti.groundLocked) {
      view.lockTubeGround();
      ti = view.getTubeInfo();
    }
    if (ti.groundLocked) {
      if (tubePending === 'fly') (mode === 'body' ? flyOntoBody : flyIntoTube)(tubeRect);
      tubePending = null;
    }
  }
  return ti;
};
let drawAnchor = null;
const initTube = () => {
  defineMode('tube', tubeHooks);
  defineMode('body', tubeHooks);

  map.on('load', () => {
    map.addSource('tube-rect', { type: 'geojson', data: rectFC(null) });
    map.addLayer({ id: 'tube-rect-fill', type: 'fill', source: 'tube-rect',
      filter: ['==', ['get', 'kind'], 'rect'],
      paint: { 'fill-color': '#ff7f0e', 'fill-opacity': 0.12 } });
    map.addLayer({ id: 'tube-rect-line', type: 'line', source: 'tube-rect',
      filter: ['==', ['get', 'kind'], 'rect'],
      paint: { 'line-color': '#ff7f0e', 'line-width': 2 } });
    map.addLayer({ id: 'tube-rect-arrows', type: 'fill', source: 'tube-rect',
      filter: ['==', ['get', 'kind'], 'arrow'],
      paint: { 'fill-color': '#ff7f0e', 'fill-opacity': 0.9 } });
    setRectData(tubeRect);
  });

  updateAxisBtn();
  axisBtn.addEventListener('click', () => {
    tubeAxisEw = !tubeAxisEw;
    updateAxisBtn();
    if (tubeRect) {
      setRectData(tubeRect); // arrows move to the other edge pair
      if (inTube()) rollRect(mode, true); // re-roll around the new axis
      else view.setTubeRect(tubeRect[0], tubeRect[1], tubeRect[2], tubeRect[3], tubeAxisEw);
    }
  });

  map.boxZoom.disable(); // shift+drag draws the tube rect instead

  map.on('mousedown', (e) => {
    if (!e.originalEvent.shiftKey) return;
    e.preventDefault(); // keep dragPan out of the gesture
    drawAnchor = e.lngLat;
  });
  map.on('mousemove', (e) => {
    if (drawAnchor) setRectData(normRect(drawAnchor, e.lngLat));
  });
  map.on('mouseup', (e) => {
    if (!drawAnchor) return;
    const r = normRect(drawAnchor, e.lngLat);
    drawAnchor = null;
    // an accidental shift-click, not a rect (under ~30 m each way)
    if (r[2] - r[0] < 3e-4 && r[3] - r[1] < 3e-4) { setRectData(tubeRect); return; }
    tubeRect = r;
    setRectData(r);
    if (view.ready()) rollRect(rectShape, true);
  });
};
