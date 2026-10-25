import { D2R, planetR } from './geo.js';

export { paramSpecs, lmStep };

// --- the solver -----------------------------------------------------------
// levenberg–marquardt on the pose: gauss-newton where the local model is
// trusted, gradient descent where it isn't, with a numeric jacobian (the
// projection is six cheap parameters, so finite differences cost nothing).
// it's a local method by design — this is for fine-tuning a view that is
// already roughly right, not for solving the pose from scratch.
//
// the parameters are local deltas rather than the pose itself: metres east /
// north / up instead of degrees of latitude and longitude, so every column
// of the jacobian has comparable scale and the normal equations stay
// conditioned wherever on the globe you are
const paramSpecs = (freeFlags) => {
  const s = [];
  if (freeFlags.pos) s.push('e', 'n', 'u');
  if (freeFlags.aim) s.push('h', 't');
  if (freeFlags.fov) s.push('f');
  // off by default: a hand-held photo is usually a little banked, but so is
  // a fit that has gone wrong, and a free roll will happily take a bad
  // solution the rest of the way. tick it when the horizon in the picture
  // isn't level
  if (freeFlags.roll) s.push('r');
  // off by default for the same reason, and one more: it is the twin of
  // fov (a barrel lens looks like a wider field at the edges), and pairs
  // near the centre of the picture say nothing about it at all. tick it
  // when the pairs at the edges won't settle while the middle has
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
  // the same limits the engine enforces, so an accepted step survives
  // being handed to setPose/setFov unchanged
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
// one levenberg–marquardt step from pose base over the parameters specs:
// resid(pose) is the residual vector there (null where it can't be
// measured), clampK the lens's limits, and lm the damping and the progress
// so far ({ lambda, iters, stalls }), which this updates. returns { cand,
// done }: cand the accepted pose if a step was taken, done when there's
// nothing left to do
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
  // raise the damping until a step actually improves the fit; each failure
  // pulls the step shorter and more gradient-like
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
