export { STORES, FILES, loadStore, saveStore, freezeStorage, QS, TEST, DEFAULT_VIEW, START, S };

// --- what the page keeps ----------------------------------------------------
// localstorage: earth-view (view.js), earth-path (path.js), earth-align
// (photo.js); indexeddb 'app': the gpx and the photo. a session bundle
// (io.js) carries exactly these, and not the tile cache
const STORES = ['earth-view', 'earth-path', 'earth-align'];
const FILES = ['gpx-file', 'ref-image'];

// set once a pasted session is in storage, never cleared: any later save
// writes the replaced session back over it — the view saves on beforeunload,
// which the reload after a paste fires itself, and once a second
let frozen = false;
const freezeStorage = () => { frozen = true; };

// null for a store that isn't there or doesn't parse
const loadStore = (key) => {
  try { return JSON.parse(localStorage.getItem(key)); } catch (e) { return null; }
};
const saveStore = (key, value) => {
  if (frozen) return;
  try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) {}
};

// this file imports nothing, so any module can read these at its top level
const QS = new URLSearchParams(location.search);
// headless capture (?test=1&lat=..&lon=..&alt=..&heading=..&tilt=..&fov=..):
// pose from the url, nothing saved, full-window 3d, fixed dt. not bit-exact
// (tiles arrive by wall clock), but eviction and eases stop depending on fps
const TEST = QS.has('test') ? {
  dt: Number(QS.get('dt')) || 1000 / 60,
  pose: ['lat', 'lon', 'alt', 'heading', 'tilt', 'fov'].reduce((o, k) => {
    if (QS.has(k)) o[k] = Number(QS.get(k));
    return o;
  }, {
    ortho: QS.get('ortho') === '1',
    hug: QS.get('hug') === '1',
  }),
} : null;

// the seattle skyline from out on elliott bay
const DEFAULT_VIEW = { lat: 47.5980, lon: -122.3750, alt: 800, heading: 65, tilt: -12, fov: 45, zoom: 12, gain: 1, ortho: false, miniature: false, tube: null, full3d: false };
const START = (() => {
  if (TEST) return { ...DEFAULT_VIEW, ...TEST.pose, full3d: true };
  const v = loadStore('earth-view');
  if (v && Number.isFinite(v.lat) && Number.isFinite(v.lon) && Number.isFinite(v.alt))
    return { ...DEFAULT_VIEW, ...v };
  return DEFAULT_VIEW;
})();

// state more than one module writes. single-writer state is exported from
// its own module instead, so S.x reads as "anyone may change this"
const S = {
  gain: 1, // the speed (- =), re-initialized from the saved view once it loads
  // look-around gathered between frames (drag, arrow keys), spent by the frame
  yaw: 0, pitch: 0,
  // the map follows the camera until grabbed; moving the camera somewhere
  // new turns it back on
  following: true,
  followVel: null, prevMark: null, // the follow ease's velocity lead
  curl: 0, // the roll, for the tube and the tunnel alike: 0 flat .. 1 closed
  // the tunnel's settings (the saved view and __demo set them too)
  tunnelC: 2000, // metres around
  tunnelAcross: true, // the roll runs along the heading (else around it)
  tunnelLag: 0, // seconds for the tube to swing round after a turn; 0 at once
  tunnelK: 10, // looking down the pipe: how far it runs each way, in circumferences
  pendingShot: null, // runs at the end of the next frame, once (shots)
  refImageKey: null, // name:size of the stored photo, to spot a re-drop
};
