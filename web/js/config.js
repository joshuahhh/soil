export { QS, TEST, DEFAULT_VIEW, START };

// what the page starts from: the url's parameters and the saved view. it
// imports nothing, so every module can read these at its top level
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
  try {
    const v = JSON.parse(localStorage.getItem('earth-view'));
    if (v && Number.isFinite(v.lat) && Number.isFinite(v.lon) && Number.isFinite(v.alt))
      return { ...DEFAULT_VIEW, ...v };
  } catch (e) {}
  return DEFAULT_VIEW;
})();
