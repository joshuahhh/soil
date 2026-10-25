export { STORES, FILES, loadStore, saveStore, freezeStorage };

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
