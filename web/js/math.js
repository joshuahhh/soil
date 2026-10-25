export { D2R, planetR, setPlanetR, vsub, vmul, vadd, vdot, vcross, vnorm, geoUpV, geoNorthV,
  ecefOf, poseBasis, projectWith, sunPosition, smoothSpine, makeSpine, spineAt, rawAt,
  spineLatLon, spineHeading, nearestS, paramSpecs, lmStep };

// ecef metres. no dom, no engine. the projection mirrors earth_camera.h's
// poseToCamera and rocktree_math.h's lookAt/perspective (as does pickNdc), so
// the solver needs no wasm round trips: change them together
const D2R = Math.PI / 180;
let planetR = 6371010; // refreshed from the engine as soon as it's ready
const setPlanetR = (r) => { planetR = r; };

const vsub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const vmul = (a, s) => [a[0] * s, a[1] * s, a[2] * s];
const vadd = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const vdot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const vcross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0]];
const vnorm = (a) => {
  const n = Math.hypot(a[0], a[1], a[2]);
  return n > 1e-12 ? vmul(a, 1 / n) : null;
};
const geoUpV = (lat, lon) => [Math.cos(lat) * Math.cos(lon),
  Math.cos(lat) * Math.sin(lon), Math.sin(lat)]; // radians
const geoNorthV = (lat, lon) => [-Math.sin(lat) * Math.cos(lon),
  -Math.sin(lat) * Math.sin(lon), Math.cos(lat)];
// exact: pickNdc reports the hit against the same radius
const ecefOf = (pr) => vmul(geoUpV(pr.lat * D2R, pr.lon * D2R), planetR + pr.alt);

// the basis lookAt builds for a pose: eye, boresight, screen-right, view-up
const poseBasis = (p) => {
  const lat = p.lat * D2R, lon = p.lon * D2R;
  const up = geoUpV(lat, lon);
  const north = geoNorthV(lat, lon);
  const east = vcross(north, up);
  const horiz = vadd(vmul(north, Math.cos(p.heading * D2R)),
    vmul(east, Math.sin(p.heading * D2R)));
  const dir = vnorm(vadd(vmul(horiz, Math.cos(p.tilt * D2R)),
    vmul(up, Math.sin(p.tilt * D2R))));
  if (!dir) return null;
  const right = vnorm(vcross(dir, up)); // straight up/down: right is undefined
  if (!right) return null;
  const vup = vcross(right, dir);
  const eye = vmul(up, planetR + p.alt);
  // roll turns the screen axes about the boresight, as the engine's view matrix does
  const r = (p.roll || 0) * D2R;
  if (!r) return { eye, dir, right, vup };
  const c = Math.cos(r), sn = Math.sin(r);
  return { eye, dir,
    right: vsub(vmul(right, c), vmul(vup, sn)),
    vup: vadd(vmul(vup, c), vmul(right, sn)) };
};
// ecef -> viewport pixels; null when the point is behind the camera
const projectWith = (b, fovDeg, P, W, H) => {
  const v = vsub(P, b.eye);
  const z = vdot(v, b.dir);
  if (!(z > 1e-6)) return null;
  const t = Math.tan(fovDeg * D2R / 2);
  const ndcX = vdot(v, b.right) / z / (t * (W / H));
  const ndcY = vdot(v, b.vup) / z / t;
  return { x: (ndcX * 0.5 + 0.5) * W, y: (0.5 - ndcY * 0.5) * H };
};

// --- the sun ------------------------------------------------------------
// noaa's low-precision solar position, good to a fraction of a degree
const sunPosition = (date, lat, lon) => {
  const rad = Math.PI / 180;
  const jd = date.getTime() / 86400000 + 2440587.5;
  const t = (jd - 2451545) / 36525;
  const L = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360;
  const M = (357.52911 + t * (35999.05029 - 0.0001537 * t)) * rad;
  const C = Math.sin(M) * (1.914602 - t * (0.004817 + 0.000014 * t)) + Math.sin(2 * M) * (0.019993 - 0.000101 * t) + Math.sin(3 * M) * 0.000289;
  const lam = (L + C - 0.00569 - 0.00478 * Math.sin((125.04 - 1934.136 * t) * rad)) * rad;
  const eps = (23.439291 - 0.0130042 * t) * rad;
  const dec = Math.asin(Math.sin(eps) * Math.sin(lam));
  const ra = Math.atan2(Math.cos(eps) * Math.sin(lam), Math.cos(lam));
  const gmst = (280.46061837 + 360.98564736629 * (jd - 2451545)) % 360;
  const ha = ((gmst + lon) * rad - ra);
  const la = lat * rad;
  const el = Math.asin(Math.sin(la) * Math.sin(dec) + Math.cos(la) * Math.cos(dec) * Math.cos(ha));
  const az = Math.atan2(-Math.sin(ha), Math.cos(la) * Math.tan(dec) - Math.sin(la) * Math.cos(ha));
  return { az: ((az / rad) + 360) % 360, el: el / rad };
};

// --- the spine ------------------------------------------------------------
// the track as the follow camera (path.js) sees it, on a local tangent plane
const SPINE_STEP = 10;       // m between resampled points

// box filter by prefix sum; the window shrinks at the ends
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
  // segment gaps are crossed, not jumped: the camera has to travel through them
  const raw = [];
  for (const seg of path.segs) for (const p of seg) raw.push(p);
  if (raw.length < 2) return null;
  // one plane for the whole track; the east scale drifts ~1% per degree of latitude
  const lat0 = raw.reduce((t, p) => t + p.lat, 0) / raw.length;
  const lon0 = raw[0].lon;
  const mLat = D2R * planetR, mLon = mLat * Math.cos(lat0 * D2R);
  const xy = raw.map((p) => [(p.lon - lon0) * mLon, (p.lat - lat0) * mLat]);
  // each sample keeps its source point: the engine's altitudes are indexed by it
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
  // unsmoothed: smoothing happens per frame, at the view's window
  const rx = Float64Array.from(samples, (p) => p.x);
  const ry = Float64Array.from(samples, (p) => p.y);
  // arc length along the raw points, which the ribbon is drawn through: the
  // cursor and progress readout speak this, not the spine's length
  const cum = new Float64Array(xy.length);
  for (let i = 1; i < xy.length; i++)
    cum[i] = cum[i - 1] + Math.hypot(xy[i][0] - xy[i - 1][0], xy[i][1] - xy[i - 1][1]);
  return { rx, ry, idx: samples.map((p) => p.i), raw, cum, step: SPINE_STEP,
    len: (samples.length - 1) * SPINE_STEP, rawLen: cum[cum.length - 1],
    lat0, lon0, mLat, mLon };
};

// s/step is arc length on a smoothed copy to well under a percent
const spineAt = (spine, s, sm) => {
  const n = sm.xs.length;
  const u = Math.min(n - 1, Math.max(0, s / spine.step));
  const i = Math.min(n - 2, Math.floor(u)), f = u - i;
  return [sm.xs[i] + (sm.xs[i + 1] - sm.xs[i]) * f,
    sm.ys[i] + (sm.ys[i + 1] - sm.ys[i]) * f];
};
// metres along the raw track -> fractional point index (what the ribbon is
// built from, so a cursor placed by it sits on the line) and the point
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
// degrees clockwise from north, over a chord rather than one segment
const spineHeading = (spine, s, sm, chord) => {
  const a = spineAt(spine, Math.max(0, s - chord), sm);
  const b = spineAt(spine, Math.min(spine.len, s + chord), sm);
  const dx = b[0] - a[0], dy = b[1] - a[1];
  return Math.hypot(dx, dy) < 1e-6 ? null : Math.atan2(dx, dy) / D2R;
};
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

// --- the solver -----------------------------------------------------------
// levenberg–marquardt with a numeric jacobian. local by design: it refines a
// roughly right view. parameters are metres e/n/u rather than degrees, so the
// jacobian's columns have comparable scale anywhere on the globe
const paramSpecs = (freeFlags) => {
  const s = [];
  if (freeFlags.pos) s.push('e', 'n', 'u');
  if (freeFlags.aim) s.push('h', 't');
  if (freeFlags.fov) s.push('f');
  // opt-in: a free roll will happily take a bad fit the rest of the way
  if (freeFlags.roll) s.push('r');
  // opt-in too: it trades off against fov, and central pairs say nothing about it
  if (freeFlags.dist) s.push('k');
  return s;
};
const stepFor = (spec, base) => (spec === 'e' || spec === 'n' || spec === 'u')
  ? Math.max(0.02, Math.abs(base.alt) * 1e-4) // metres
  : spec === 'k' ? 2e-3 // lens, dimensionless
  : 1e-3; // degrees
const applyDelta = (base, specs, d, clampK) => {
  const p = { lat: base.lat, lon: base.lon, alt: base.alt,
    heading: base.heading, tilt: base.tilt, fov: base.fov,
    roll: base.roll || 0, k: base.k || 0 };
  const mPerDeg = (planetR + Math.max(0, base.alt)) * D2R;
  specs.forEach((s, i) => {
    const x = d[i];
    if (s === 'e') p.lon += x / Math.max(1e-6, mPerDeg * Math.cos(base.lat * D2R));
    else if (s === 'n') p.lat += x / mPerDeg;
    else if (s === 'u') p.alt += x;
    else if (s === 'h') p.heading += x;
    else if (s === 't') p.tilt += x;
    else if (s === 'f') p.fov += x;
    else if (s === 'r') p.roll += x;
    else if (s === 'k') p.k += x;
  });
  p.k = clampK(p.k); // see photo.js lensLimits
  // the engine's limits, so an accepted step survives setPose/setFov unchanged
  p.lat = Math.max(-89.9, Math.min(89.9, p.lat));
  p.alt = Math.max(1, p.alt);
  p.tilt = Math.max(-89.5, Math.min(89.5, p.tilt));
  p.fov = Math.max(1, Math.min(150, p.fov));
  return p;
};
const costOf = (r) => r ? r.reduce((a, x) => a + x * x, 0) : Infinity;
// dense gauss-jordan with partial pivoting; k is at most 6
const solveLinear = (A, b) => {
  const k = b.length;
  const M = A.map((row, i) => row.concat([b[i]]));
  for (let c = 0; c < k; c++) {
    let piv = c;
    for (let r = c + 1; r < k; r++)
      if (Math.abs(M[r][c]) > Math.abs(M[piv][c])) piv = r;
    if (!(Math.abs(M[piv][c]) > 1e-14)) return null; // singular
    [M[c], M[piv]] = [M[piv], M[c]];
    for (let r = 0; r < k; r++) {
      if (r === c) continue;
      const f = M[r][c] / M[c][c];
      for (let j = c; j <= k; j++) M[r][j] -= f * M[c][j];
    }
  }
  const x = [];
  for (let i = 0; i < k; i++) x.push(M[i][k] / M[i][i]);
  return x.every(Number.isFinite) ? x : null;
};
// resid(pose) -> residuals, null where unmeasurable. lm = { lambda, iters,
// stalls }, updated in place. returns { cand (if a step was taken), done }
const lmStep = (base, specs, resid, clampK, lm) => {
  const k = specs.length;
  const r0 = resid(base);
  if (!r0) return { done: true }; // nothing usable to fit from here
  const cost0 = costOf(r0);
  const n = r0.length;

  const J = [];
  for (let j = 0; j < k; j++) {
    const h = stepFor(specs[j], base);
    const d = new Array(k).fill(0);
    d[j] = h;
    let rp = resid(applyDelta(base, specs, d, clampK));
    let denom = h;
    if (!rp) { // forward step fell off a clamp: difference backwards instead
      d[j] = -h;
      rp = resid(applyDelta(base, specs, d, clampK));
      denom = -h;
    }
    const col = new Array(n).fill(0);
    if (rp) for (let i = 0; i < n; i++) col[i] = (rp[i] - r0[i]) / denom;
    J.push(col);
  }
  const A = [], g = [];
  for (let a = 0; a < k; a++) {
    A.push(new Array(k).fill(0));
    let ga = 0;
    for (let i = 0; i < n; i++) ga += J[a][i] * r0[i];
    g.push(-ga);
  }
  for (let a = 0; a < k; a++) for (let b = a; b < k; b++) {
    let s = 0;
    for (let i = 0; i < n; i++) s += J[a][i] * J[b][i];
    A[a][b] = A[b][a] = s;
  }
  // raise the damping until a step improves the fit
  for (let attempt = 0; attempt < 8; attempt++) {
    const Ad = A.map((row, a) => row.map((x, b) =>
      a === b ? x * (1 + lm.lambda) + 1e-9 : x));
    const step = solveLinear(Ad, g);
    if (step) {
      const cand = applyDelta(base, specs, step, clampK);
      const cost = costOf(resid(cand));
      if (cost < cost0) {
        lm.lambda = Math.max(1e-9, lm.lambda / 3);
        const rel = (cost0 - cost) / Math.max(cost0, 1e-12);
        lm.stalls = rel < 1e-7 ? lm.stalls + 1 : 0;
        return { cand, done: lm.stalls >= 3 || ++lm.iters >= 400 };
      }
    }
    lm.lambda *= 6;
    if (lm.lambda > 1e12) return { done: true }; // no downhill direction left: converged
  }
  return { done: false };
};
