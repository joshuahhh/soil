import { D2R, planetR } from './geo.js';

export { smoothSpine, makeSpine, spineAt, rawAt, spineLatLon, spineHeading, nearestS };

// --- the spine ------------------------------------------------------------
// the track as the follow camera sees it (path.js): one line resampled at a
// fixed step on a local tangent plane, the smoothed copies of it the camera
// steers off, and the lookups along it. no dom, no engine
const SPINE_STEP = 10;       // m between resampled points

// box filter by prefix sum, O(n) whatever the window; the window shrinks at
// the ends rather than the values being clamped into it
const boxcar = (a, half) => {
  const n = a.length, pre = new Float64Array(n + 1), out = new Float64Array(n);
  for (let i = 0; i < n; i++) pre[i + 1] = pre[i] + a[i];
  for (let i = 0; i < n; i++) {
    const lo = Math.max(0, i - half), hi = Math.min(n - 1, i + half);
    out[i] = (pre[hi + 1] - pre[lo]) / (hi - lo + 1);
  }
  return out;
};
const halfFor = (win) => Math.max(1, Math.round(win / SPINE_STEP / 2));
// two boxcar passes (≈ gaussian) over the spine at a window of win metres
const smoothSpine = (spine, win) => {
  const half = halfFor(win);
  let xs = spine.rx, ys = spine.ry;
  for (let pass = 0; pass < 2; pass++) { xs = boxcar(xs, half); ys = boxcar(ys, half); }
  return { win, xs, ys };
};

const makeSpine = (path) => {
  if (!path) return null;
  // one line to follow: the gaps between track segments are crossed rather
  // than jumped, since the camera has to travel through them somehow
  const raw = [];
  for (const seg of path.segs) for (const p of seg) raw.push(p);
  if (raw.length < 2) return null;
  // one local tangent plane for the whole track: meters east/north of its
  // mean latitude. the east scale drifts with cos(lat) away from that — a
  // percent over a degree — which is nothing against a camera standoff, and
  // it keeps resampling and smoothing to plain 2d arithmetic
  const lat0 = raw.reduce((t, p) => t + p.lat, 0) / raw.length;
  const lon0 = raw[0].lon;
  const mLat = D2R * planetR, mLon = mLat * Math.cos(lat0 * D2R);
  const xy = raw.map((p) => [(p.lon - lon0) * mLon, (p.lat - lat0) * mLat]);
  // resample at a fixed step, remembering which original point each sample
  // came from — the engine's altitudes are indexed by that
  const samples = [{ x: xy[0][0], y: xy[0][1], i: 0 }];
  let dAcc = 0, dNext = SPINE_STEP;
  for (let i = 1; i < xy.length; i++) {
    const dx = xy[i][0] - xy[i - 1][0], dy = xy[i][1] - xy[i - 1][1];
    const len = Math.hypot(dx, dy);
    if (!(len > 0)) continue;
    while (dNext <= dAcc + len) {
      const t = (dNext - dAcc) / len;
      samples.push({ x: xy[i - 1][0] + dx * t, y: xy[i - 1][1] + dy * t, i });
      dNext += SPINE_STEP;
    }
    dAcc += len;
  }
  if (samples.length < 2) return null;
  // resampled but *unsmoothed*: the smoothing happens per frame, at
  // whatever window the current view scale asks for
  const rx = Float64Array.from(samples, (p) => p.x);
  const ry = Float64Array.from(samples, (p) => p.y);
  // arc length along the *raw* points, which is what the ribbon is drawn
  // through and therefore what the cursor and the progress readout have to
  // speak. the spine is only ever consulted for which way the track heads
  const cum = new Float64Array(xy.length);
  for (let i = 1; i < xy.length; i++)
    cum[i] = cum[i - 1] + Math.hypot(xy[i][0] - xy[i - 1][0], xy[i][1] - xy[i - 1][1]);
  return { rx, ry, idx: samples.map((p) => p.i), raw, cum, step: SPINE_STEP,
    len: (samples.length - 1) * SPINE_STEP, rawLen: cum[cum.length - 1],
    lat0, lon0, mLat, mLon };
};

// the smoothing leaves the samples very slightly unevenly spaced, so s/step
// is an approximation of arc length — by well under a percent, and nothing
// here needs it to be exact
// a point on one of the smoothed copies, interpolated between the samples
// either side so it doesn't step as s crosses one
const spineAt = (spine, s, sm) => {
  const n = sm.xs.length;
  const u = Math.min(n - 1, Math.max(0, s / spine.step));
  const i = Math.min(n - 2, Math.floor(u)), f = u - i;
  return [sm.xs[i] + (sm.xs[i + 1] - sm.xs[i]) * f,
    sm.ys[i] + (sm.ys[i + 1] - sm.ys[i]) * f];
};
// metres along the raw track -> fractional point index, and the point
// itself. this is the number the ribbon is built from, so a cursor placed
// with it sits on the drawn line by construction
const rawAt = (spine, s) => {
  const cum = spine.cum;
  let lo = 0, hi = cum.length - 1;
  const d = Math.max(0, Math.min(s, spine.rawLen));
  while (lo < hi) { // first index whose cumulative length is >= d
    const mid = (lo + hi) >> 1;
    if (cum[mid] < d) lo = mid + 1; else hi = mid;
  }
  const i = Math.max(0, lo - 1);
  const leg = cum[i + 1] - cum[i];
  const f = leg > 0 ? (d - cum[i]) / leg : 0;
  const a = spine.raw[i], b = spine.raw[Math.min(i + 1, spine.raw.length - 1)];
  return { idx: i + f,
    lat: a.lat + (b.lat - a.lat) * f, lon: a.lon + (b.lon - a.lon) * f };
};
const spineLatLon = (spine, xy) => ({ lat: spine.lat0 + xy[1] / spine.mLat,
  lon: spine.lon0 + xy[0] / spine.mLon });
// which way the track heads at s, degrees clockwise from north, over a
// chord rather than a segment (this is the anti-jitter measurement)
const spineHeading = (spine, s, sm, chord) => {
  const a = spineAt(spine, Math.max(0, s - chord), sm);
  const b = spineAt(spine, Math.min(spine.len, s + chord), sm);
  const dx = b[0] - a[0], dy = b[1] - a[1];
  return Math.hypot(dx, dy) < 1e-6 ? null : Math.atan2(dx, dy) / D2R;
};
// where on the spine a camera at p is standing, so V picks you up where
// you are rather than teleporting to the trailhead
const nearestS = (spine, p) => {
  const x = (p.lon - spine.lon0) * spine.mLon, y = (p.lat - spine.lat0) * spine.mLat;
  let best = 0, bd = Infinity;
  for (let i = 0; i < spine.raw.length; i++) {
    const rx = (spine.raw[i].lon - spine.lon0) * spine.mLon;
    const ry = (spine.raw[i].lat - spine.lat0) * spine.mLat;
    const d = (rx - x) ** 2 + (ry - y) ** 2;
    if (d < bd) { bd = d; best = i; }
  }
  return spine.cum[best];
};
