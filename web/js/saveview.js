import { QS, TEST, START } from './config.js';
import { S } from './state.js';
import { saveStore } from './persist.js';
import { view } from './engine.js';
import { map, full3d } from './map.js';
import { ortho, setOrtho, hug, setHug } from './mode.js';
import { miniature, setMiniature, debugLod, setDebugLod } from './input.js';
import { showCut } from './nearcut.js';
import { refreshTunnel, setTunnel, tunnelState, restoreTunnel } from './tunnel.js';
import { tubeState, restoreTube } from './tube.js';
import { sky, glamour, applySky, applyGlamour } from './sky.js';

export { saveView, restoreView, initSaveView };

// --- the saved view -------------------------------------------------------
// the camera pose and the view's switches, to localstorage's 'earth-view'
// (config.js reads it back as START, and restoreView applies it). the tube
// and the tunnel keep their own parts, next to their state
const saveView = () => {
  if (TEST) return; // captures never write back over the user's saved view
  if (!view.ready()) return;
  const p = view.getPose();
  saveStore('earth-view', {
    lat: p.lat, lon: p.lon, alt: p.alt, heading: p.heading, tilt: p.tilt,
    roll: p.roll, fov: p.fov, zoom: map.getZoom(), gain: S.gain, ortho, hug,
    miniature, full3d,
    nearCut: view.getNearCut(),
    lod: debugLod,
    sky, glamour,
    tube: tubeState(),
    tunnel: tunnelState(),
  });
};
// the same, applied, from the first frame the engine is ready in. the map's
// centre, zoom and full3d are taken at its creation (map.js), and the sky
// and glamour settings at load (sky.js)
const restoreView = () => {
  view.setPose(START.lat, START.lon, START.alt, START.heading, START.tilt);
  view.setFov(START.fov);
  if (Number.isFinite(START.nearCut) && START.nearCut > 0) {
    view.setNearCut(START.nearCut);
    showCut();
  }
  S.gain = START.gain;
  setOrtho(!!START.ortho);
  setHug(!!START.hug);
  if (START.miniature) setMiniature(true);
  if (START.lod) setDebugLod(true);
  applySky();
  applyGlamour();
  // a banked camera comes back banked. inside the tube the bank belongs to
  // the airplane frame and is restored after the roll-up instead, because
  // that is what creates the frame to bank
  const tubeRolled = !!(START.tube && START.tube.on);
  if (START.roll && !tubeRolled) view.setRoll(START.roll);
  if (restoreTube(START.tube) && START.roll) view.setRoll(START.roll);
  restoreTunnel(START.tunnel, tubeRolled);
  // ?tunnel=<circumference m> (test mode): straight into the tunnel at
  // the given pose, for the render tests
  if (TEST && QS.has('tunnel')) {
    const c = parseFloat(QS.get('tunnel'));
    if (Number.isFinite(c) && c > 0) { S.tunnelC = c; refreshTunnel(); setTunnel(true, false); }
  }
};
const initSaveView = () => {
  addEventListener('beforeunload', saveView);
};
