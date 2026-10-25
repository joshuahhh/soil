export { STORES, FILES, loadStore, saveStore, freezeStorage, QS, TEST, DEFAULT_VIEW, START, S };

// --- what the page keeps ----------------------------------------------------
// three json objects in localstorage, each written by its own part of the
// page and read back by the same part:
//   earth-view   the camera and the view's switches (saveview.js)
//   earth-path   the dropped track's style, and how you ride it (path.js)
//   earth-align  the photo's placement, its point pairs, the solver's
//                settings (align.js)
// and two files in indexeddb's 'app' store, too big for localstorage: the
// dropped track ('gpx-file', path.js) and the reference photo ('ref-image',
// drop.js and align.js). this is exactly what a session bundle carries
// (session.js); the tile cache, in the same database, is left out of it
const STORES = ['earth-view', 'earth-path', 'earth-align'];
const FILES = ['gpx-file', 'ref-image'];

// set once a pasted session has been written to storage, and never cleared.
// from that moment what's in storage describes a session this page is not
// showing, and every saver would be writing the one it's replacing over it.
// the view saver is the one that actually bites: it runs both on
// beforeunload — which the reload after a paste fires itself — and once a
// second off the frame loop, so leaving it live loses the pasted pose
// always, or intermittently, depending on which one gets there first
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

// what the page starts from: the url's parameters and the saved view. it
// imports only persist.js, which imports nothing, so every module can read
// these at its top level
const QS = new URLSearchParams(location.search);
// headless capture mode (?test=1&lat=..&lon=..&alt=..&heading=..&tilt=..&fov=..):
// the pose comes from the url instead of localstorage, nothing is saved back,
// the 3d view takes the whole window so the canvas size is just the viewport,
// and the frame step is a fixed dt. that last one doesn't make a run bit-exact
// — tile arrival is still wall-clock — but it does stop the eviction sweep and
// every animation ease from depending on the machine's frame rate.
// window.__test below is the driver's api
const TEST = QS.has('test') ? {
  dt: Number(QS.get('dt')) || 1000 / 60,
  pose: ['lat', 'lon', 'alt', 'heading', 'tilt', 'fov'].reduce((o, k) => {
    if (QS.has(k)) o[k] = Number(QS.get(k));
    return o;
  }, {
    // flags, not numbers, so they take the same route but parse differently
    ortho: QS.get('ortho') === '1',
    hug: QS.get('hug') === '1',
  }),
} : null;

// view state (camera pose, fov, speed gain, map zoom) persists in
// localstorage so a reload puts you back where you were
// the seattle skyline from out on elliott bay
const DEFAULT_VIEW = { lat: 47.5980, lon: -122.3750, alt: 800, heading: 65, tilt: -12, fov: 45, zoom: 12, gain: 1, ortho: false, miniature: false, tube: null, full3d: false };
const START = (() => {
  // test mode ignores saved state entirely: a capture must not depend on
  // whatever pose the last interactive session happened to leave behind
  if (TEST) return { ...DEFAULT_VIEW, ...TEST.pose, full3d: true };
  const v = loadStore('earth-view');
  if (v && Number.isFinite(v.lat) && Number.isFinite(v.lon) && Number.isFinite(v.alt))
    return { ...DEFAULT_VIEW, ...v };
  return DEFAULT_VIEW;
})();

// the page's shared mutable state: what more than one module writes. state
// that only one module writes stays in that module and is exported from it,
// which makes it read-only everywhere else (an import can't be assigned to),
// so S.x reads as "anything may change this" and an imported x as "only its
// own module does"
const S = {
  gain: 1, // the speed (- =; input.js), re-initialized from the saved view once it loads
  // the pointer's look-around, gathered between frames: input.js adds the
  // drag, the frame loop the arrow keys, and the frame (or the follow
  // camera) spends it
  yaw: 0, pitch: 0,
  // the map follows the camera (map.js) until it is grabbed; anything that
  // puts the camera somewhere new turns the following back on
  following: true,
  followVel: null, prevMark: null, // the follow ease's velocity lead
  curl: 0, // the roll, for the tube and the tunnel alike: 0 flat .. 1 closed
  // the tunnel's settings (tunnel.js; the saved view and __demo set them too)
  tunnelC: 2000, // metres around
  tunnelAcross: true, // the roll runs along the heading (else around it)
  tunnelLag: 0, // seconds for the tube to swing round after a turn; 0 at once
  tunnelK: 10, // looking down the pipe: how far it runs each way, in circumferences
  pendingShot: null, // runs at the end of the next frame, once (shots)
  refImageKey: null, // name:size of the stored photo, to spot a re-drop
};
