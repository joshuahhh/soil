import { S } from './state.js';
import { view } from './engine.js';
import { map } from './map.js';
import { updateHint, toast } from './hud.js';
import { ensureBody } from './body.js';

export { tubePendingSince, setRectData, updateAxisBtn, flyIntoTube, flyOntoBody, unrollTube,
  applyTube, initTube };

// --- tube mode ------------------------------------------------------------
// shift+drag on the map draws a rectangle; that slab of territory is rolled
// into a tube (the inception effect) and the camera teleports to one end of
// the tube axis, looking down it. T rolls/unrolls; drawing a new rect
// replaces the old one. the roll animates via curl (0 flat .. 1 closed)
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
  if (S.tubeAxisEw) {
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
  axisBtn.textContent = `tube axis: ${S.tubeAxisEw ? 'E–W' : 'N–S'} (click to flip)`;
};
// fly to the near end of the tube axis: the axis runs along the rect's
// longer dimension at height = tube radius, so spawning at (end, radius)
// heading along the axis puts you dead center in the pipe, looking down it
const flyIntoTube = (r) => {
  const latc = (r[0] + r[2]) / 2, lonc = (r[1] + r[3]) / 2;
  const ew = S.tubeAxisEw;
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
  if (S.tubeAxisEw) view.setPose(latc + dlat, lonc, info.groundAlt + 0.55 * h, 180, 0);
  else view.setPose(latc, lonc - dlon, info.groundAlt + 0.55 * h, 90, 0);
  S.following = true;
};
// back to flat flight over the rect, which stays drawn
const unrollTube = () => {
  S.tubeOn = false;
  S.tubePending = null;
  view.setTubeEnabled(false);
  view.setAirplane(false);
};
const applyTube = (r, teleport) => {
  // the body's figure is fetched and baked on first use; the roll waits
  // for it (a cylinder standing in meanwhile would be the wrong picture)
  if (S.tubeBody && !view.bodyReady()) {
    ensureBody().then((ok) => {
      if (!ok) { S.tubeBody = false; toast('the body mesh failed to load'); }
      if (S.tubeRect === r) applyTube(r, teleport);
    });
    return;
  }
  view.setTubeBody(S.tubeBody);
  view.setTubeRect(r[0], r[1], r[2], r[3], S.tubeAxisEw);
  S.tubeOn = true;
  view.setTubeEnabled(true);
  view.setAirplane(true); // tube flying is airplane-frame
  if (S.ortho) { S.ortho = false; view.setOrtho(false); } // tube is perspective-only
  if (S.hug && S.tubeBody) { S.hug = false; view.setTerrainFollow(false); } // the wall hug is the cylinder's
  S.curl = 0; // flat while the rect loads; the tick rolls it when ready
  view.setTubeCurl(0);
  S.tubePending = teleport ? 'fly' : 'roll';
  tubePendingSince = performance.now();
  updateHint();
};
let drawAnchor = null;
const initTube = () => {
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
    setRectData(S.tubeRect);
  });

  updateAxisBtn();
  axisBtn.addEventListener('click', () => {
    S.tubeAxisEw = !S.tubeAxisEw;
    updateAxisBtn();
    if (S.tubeRect) {
      setRectData(S.tubeRect); // arrows move to the other edge pair
      if (S.tubeOn) applyTube(S.tubeRect, true); // re-roll around the new axis
      else view.setTubeRect(S.tubeRect[0], S.tubeRect[1], S.tubeRect[2], S.tubeRect[3], S.tubeAxisEw);
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
    if (r[2] - r[0] < 3e-4 && r[3] - r[1] < 3e-4) { setRectData(S.tubeRect); return; }
    S.tubeRect = r;
    setRectData(r);
    if (view.ready()) applyTube(r, true);
  });
};
