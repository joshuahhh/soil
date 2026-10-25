import { TEST } from './config.js';
import { S } from './state.js';
import { view } from './engine.js';
import { map, full3d } from './map.js';
import { miniature, debugLod } from './input.js';
import { tunnelOn, tunnelClosing } from './tunnel.js';
import { storageFrozen } from './session.js';
import { sky, glamour } from './sky.js';

export { saveView, initSaveView };

// --- the saved view -------------------------------------------------------
// the camera pose and the view's switches, to localstorage's 'earth-view'
// (config.js reads it back as START)
const saveView = () => {
  if (TEST) return; // captures never write back over the user's saved view
  if (storageFrozen) return;
  if (!view.ready()) return;
  const p = view.getPose();
  try {
    localStorage.setItem('earth-view', JSON.stringify({
      lat: p.lat, lon: p.lon, alt: p.alt, heading: p.heading, tilt: p.tilt,
      roll: p.roll, fov: p.fov, zoom: map.getZoom(), gain: S.gain, ortho: S.ortho, hug: S.hug,
        miniature, full3d,
      nearCut: view.getNearCut(),
      lod: debugLod,
      sky, glamour,
      tube: S.tubeRect ? { rect: S.tubeRect, on: S.tubeOn, ewAxis: S.tubeAxisEw, body: S.tubeBody } : null,
      tunnel: { on: tunnelOn && !tunnelClosing, c: S.tunnelC, across: S.tunnelAcross, lag: S.tunnelLag, k: S.tunnelK },
    }));
  } catch (e) {}
};
const initSaveView = () => {
  addEventListener('beforeunload', saveView);
};
