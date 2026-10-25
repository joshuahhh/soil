import { S } from './config.js';
import { view, canvas } from './engine.js';
import { saveView, mode, defineMode, setMode, inTube } from './view.js';
import { updateHint, logScale, bindRange, toast } from './hud.js';
import { map } from './map.js';

export { initTube, bodyPromise, setBodyMesh, refreshTunnel, tunnelClosing, curlTarget, endTunnel,
  setTunnel, tunnelState, restoreTunnel, tubeRect, tubePending, rectShape, rollRect, clearRect,
  advanceTube, tubeState, restoreTube };

// --- the body's geometry image -------------------------------------------
// the figure body mode wraps the rect onto (body.js), baked into the
// geometry image the engine samples: a grid over
// (u around the figure, v feet to head) of skin positions and normals.
// each texel is a ray cast outward at that height and angle from the
// centre of the figure's cross-section there (not one fixed spine: the
// head and neck lean forward of the hips, and a ray from the wrong
// centre grazes the skin instead of crossing it), and the outermost hit
// is the skin. one loop per height would web the legs into a gown and
// stretch a wing from torso to fingertip, so the rows are banded (see
// bakeBody): below the crotch the rect's width is split between the
// two legs, and where the arms hang free its outer fifths wrap the
// arms, each part with rays from its own centre. rays that still miss
// are bridged by interpolating the radius across the gap within the
// loop. the shader keeps its samples and the terrain's triangles
// within one loop, so the parts don't smear into each other.
// no dom, no engine
const parseObj = (text) => {
  const v = [], f = [];
  for (const line of text.split('\n')) {
    if (line.charCodeAt(0) === 118 && line.charCodeAt(1) === 32) { // 'v '
      const p = line.slice(2).trim().split(/\s+/);
      v.push(+p[0], +p[1], +p[2]);
    } else if (line.charCodeAt(0) === 102 && line.charCodeAt(1) === 32) { // 'f '
      // v, v/vt, v//vn, v/vt/vn: parseInt stops at the slash. negative
      // (relative) indices are rare and not handled
      const idx = line.slice(2).trim().split(/\s+/).map((t) => parseInt(t, 10) - 1);
      for (let i = 1; i + 1 < idx.length; i++) f.push(idx[0], idx[i], idx[i + 1]);
    }
  }
  return { v: Float32Array.from(v), f: Uint32Array.from(f) };
};
const bakeBody = (mesh, W = 256, H = 256) => {
  const { v, f } = mesh;
  const n = v.length / 3;
  // body units: feet at 0, height 1, the vertex mean at x = z = 0 (a
  // rough centring; the rays below find their own origin per row)
  let ymin = Infinity, ymax = -Infinity, sx = 0, sz = 0;
  for (let i = 0; i < n; i++) {
    const y = v[3 * i + 1];
    if (y < ymin) ymin = y;
    if (y > ymax) ymax = y;
    sx += v[3 * i];
    sz += v[3 * i + 2];
  }
  const cx = sx / n, cz = sz / n, s = 1 / (ymax - ymin);
  const P = new Float32Array(n * 3);
  for (let i = 0; i < n; i++) {
    P[3 * i] = (v[3 * i] - cx) * s;
    P[3 * i + 1] = (v[3 * i + 1] - ymin) * s;
    P[3 * i + 2] = (v[3 * i + 2] - cz) * s;
  }
  // triangles by the rows (heights) they span
  const nt = f.length / 3;
  const rows = Array.from({ length: H }, () => []);
  for (let t = 0; t < nt; t++) {
    const y0 = P[3 * f[3 * t] + 1], y1 = P[3 * f[3 * t + 1] + 1], y2 = P[3 * f[3 * t + 2] + 1];
    const lo = Math.max(0, Math.floor(Math.min(y0, y1, y2) * H - 0.5));
    const hi = Math.min(H - 1, Math.ceil(Math.max(y0, y1, y2) * H - 0.5));
    for (let j = lo; j <= hi; j++) rows[j].push(t);
  }
  // the mesh's cross-section at each row's height: one segment per
  // triangle the plane cuts, in (x, z); and each segment's class, by
  // the closed loop it belongs to (segments sharing an endpoint): the
  // longest loop is the torso (0), any other loop off to a side is an
  // arm (+1 toward +x, -1 toward -x), anything else stray (2). a fixed
  // width cutoff can't tell the underside of an arm at the armpit from
  // the torso's own side, and taking it as torso put a ledge there
  const secs = [], classes = [];
  for (let j = 0; j < H; j++) {
    const y = (j + 0.5) / H;
    const seg = [];
    for (const t of rows[j]) {
      let k = 0;
      const pts = [0, 0, 0, 0];
      for (let e = 0; e < 3 && k < 4; e++) {
        const a = f[3 * t + e], b = f[3 * t + (e + 1) % 3];
        const ya = P[3 * a + 1], yb = P[3 * b + 1];
        if ((ya <= y) === (yb <= y)) continue;
        const w = (y - ya) / (yb - ya);
        pts[k++] = P[3 * a] + (P[3 * b] - P[3 * a]) * w;
        pts[k++] = P[3 * a + 2] + (P[3 * b + 2] - P[3 * a + 2]) * w;
      }
      if (k !== 4) continue;
      seg.push(pts[0], pts[1], pts[2], pts[3]);
    }
    secs.push(seg);
    const ns = seg.length / 4;
    const parent = Int32Array.from({ length: ns }, (_, i) => i);
    const find = (i) => { while (parent[i] !== i) { parent[i] = parent[parent[i]]; i = parent[i]; } return i; };
    const byPt = new Map();
    for (let k = 0; k < ns; k++) {
      for (const e of [0, 2]) {
        // shared mesh edges give the same cut point from either
        // triangle up to rounding; key on a coarse grid
        const key = Math.round(seg[4 * k + e] * 1e3) + ',' + Math.round(seg[4 * k + e + 1] * 1e3);
        const other = byPt.get(key);
        if (other === undefined) byPt.set(key, k);
        else parent[find(k)] = find(other);
      }
    }
    const comp = new Map(); // root -> [length, sum x*length]
    for (let k = 0; k < ns; k++) {
      const r = find(k);
      const len = Math.hypot(seg[4 * k + 2] - seg[4 * k], seg[4 * k + 3] - seg[4 * k + 1]);
      const c = comp.get(r) || [0, 0];
      c[0] += len;
      c[1] += len * (seg[4 * k] + seg[4 * k + 2]) / 2;
      comp.set(r, c);
    }
    let torso = -1, longest = 0;
    for (const [r, c] of comp) if (c[0] > longest) { longest = c[0]; torso = r; }
    const cls = new Int8Array(ns);
    for (let k = 0; k < ns; k++) {
      const r = find(k);
      if (r === torso) { cls[k] = 0; continue; }
      const cx = comp.get(r)[1] / comp.get(r)[0];
      cls[k] = cx > 0.1 ? 1 : cx < -0.1 ? -1 : 2;
    }
    classes.push(cls);
  }
  // the length-weighted centre of a section's pieces passing a filter on
  // their midpoint x and class, the origin its rays are cast from
  const centre = (j, filt) => {
    const seg = secs[j], cls = classes[j];
    let wsum = 0, mx = 0, mz = 0;
    for (let k = 0; k < seg.length; k += 4) {
      const midx = (seg[k] + seg[k + 2]) / 2;
      if (!filt(midx, cls[k / 4])) continue;
      const len = Math.hypot(seg[k + 2] - seg[k], seg[k + 3] - seg[k + 1]);
      wsum += len;
      mx += len * midx;
      mz += len * (seg[k + 1] + seg[k + 3]) / 2;
    }
    return wsum > 0 ? [mx / wsum, mz / wsum] : null;
  };
  // the outermost hit of the ray from (ox, oz) in direction (dx, dz)
  // among the pieces passing the filter; 0 if none
  const cast = (j, ox, oz, dx, dz, filt) => {
    const seg = secs[j], cls = classes[j];
    let best = 0;
    for (let k = 0; k < seg.length; k += 4) {
      if (!filt((seg[k] + seg[k + 2]) / 2, cls[k / 4])) continue;
      const ax = seg[k] - ox, az = seg[k + 1] - oz, ex = seg[k + 2] - seg[k], ez = seg[k + 3] - seg[k + 1];
      const den = ex * dz - ez * dx;
      if (Math.abs(den) < 1e-12) continue;
      const u = (dx * az - dz * ax) / den;
      if (u < 0 || u > 1) continue;
      const t = (ex * az - ez * ax) / den;
      if (t > best) best = t;
    }
    return best;
  };
  const ALL = () => true;
  const TORSO = (x, c) => c === 0;
  const ARM = (side) => (x, c) => c === side;
  // one loop per height can't make two legs: a rectangle with its long
  // edges glued is a cylinder, and rays cast from between the legs miss
  // front and back and web the two into a gown. so the rows are in
  // bands, and a band's loops split the rect's width between the
  // parts of the figure at that height, each part with rays from its own
  // centre:
  //   torso  (band 0): one loop, u = 0 at the back, the seam down the spine
  //   legs   (band 1): below the crotch; the u < 0.5 half wraps the +x leg
  //          (the one beneath the torso's u < 0.5 side), the rest the other
  //   arms   (band 2): where the arms hang free of the torso; the outer
  //          fifths of the width wrap the arms, the middle the torso
  // the crotch is the highest row below the hips where rays straight
  // forward and back from the section's centre both miss — the gap
  // between the legs. the arm rows are those above it where both arms
  // are loops of their own (the base mesh stands in an a-pose, so that
  // is from the fingertips to the armpits), taken as one contiguous range
  let crotch = -1;
  for (let j = Math.floor(0.6 * H); j >= 0; j--) {
    const c = centre(j, ALL);
    if (!c) continue;
    if (!cast(j, c[0], c[1], 0, 1, ALL) && !cast(j, c[0], c[1], 0, -1, ALL)) { crotch = j; break; }
  }
  let armLo = -1, armHi = -2;
  for (let j = crotch + 1; j < H; j++) {
    if (!centre(j, ARM(1)) || !centre(j, ARM(-1)) || !centre(j, TORSO)) continue;
    if (armLo < 0) armLo = j;
    armHi = j;
  }
  const aW = Math.round(0.2 * W);
  const bandOf = (j) => j <= crotch ? 1 : j >= armLo && j <= armHi ? 2 : 0;
  // a loop: its texel range, which pieces of the section it sees, and
  // its ray direction by angle. the legs and arms start at the inner
  // side (facing the torso) and go inner -> front -> outer -> back,
  // mirror images of each other
  const mirror = (side) => (th) => [-side * Math.cos(th), Math.sin(th)];
  const around = (th) => [Math.sin(th), -Math.cos(th)];
  const LOOPS = [
    [{ i0: 0, i1: W, filt: TORSO, dir: around }],
    [{ i0: 0, i1: W / 2, filt: (x) => x > 0, dir: mirror(1) },
      { i0: W / 2, i1: W, filt: (x) => x < 0, dir: mirror(-1) }],
    [{ i0: 0, i1: aW, filt: ARM(1), dir: mirror(1) },
      { i0: aW, i1: W - aW, filt: TORSO, dir: around },
      { i0: W - aW, i1: W, filt: ARM(-1), dir: mirror(-1) }],
  ];
  const loopAt = (i, j) => { const ls = LOOPS[bandOf(j)]; for (const l of ls) if (i < l.i1) return l; return ls[ls.length - 1]; };
  // per-row ray origins (one per loop), smoothed over a few rows within
  // the band so the origin slides up the figure rather than jumping
  const ox = new Float32Array(3 * H), oz = new Float32Array(3 * H);
  for (let j = 0; j < H; j++) {
    LOOPS[bandOf(j)].forEach((l, k) => {
      const c = centre(j, l.filt);
      if (c) { ox[3 * j + k] = c[0]; oz[3 * j + k] = c[1]; }
      else if (j > 0 && bandOf(j - 1) === bandOf(j)) { ox[3 * j + k] = ox[3 * (j - 1) + k]; oz[3 * j + k] = oz[3 * (j - 1) + k]; }
    });
  }
  const sox = new Float32Array(3 * H), soz = new Float32Array(3 * H);
  for (let j = 0; j < H; j++) {
    let lo = j, hi = j;
    while (lo > 0 && j - lo < 4 && bandOf(lo - 1) === bandOf(j)) lo--;
    while (hi < H - 1 && hi - j < 4 && bandOf(hi + 1) === bandOf(j)) hi++;
    for (let k = 0; k < 3; k++) {
      let x = 0, z = 0;
      for (let m = lo; m <= hi; m++) { x += ox[3 * m + k]; z += oz[3 * m + k]; }
      sox[3 * j + k] = x / (hi - lo + 1);
      soz[3 * j + k] = z / (hi - lo + 1);
    }
  }
  const pos = new Float32Array(W * H * 4);
  const cen = new Float32Array(W * H * 2); // each texel's ray origin, for the normals
  const rad = new Float32Array(W), hit = new Uint8Array(W);
  for (let j = 0; j < H; j++) {
    const y = (j + 0.5) / H;
    hit.fill(0);
    LOOPS[bandOf(j)].forEach((l, k) => {
      const L = l.i1 - l.i0;
      const cxj = sox[3 * j + k], czj = soz[3 * j + k];
      for (let i = l.i0; i < l.i1; i++) {
        const [dx, dz] = l.dir(2 * Math.PI * (i - l.i0 + 0.5) / L);
        const best = cast(j, cxj, czj, dx, dz, l.filt);
        if (best > 0) { rad[i] = best; hit[i] = 1; }
      }
      // bridge the misses, linear in radius across each gap, wrapping
      // within the loop
      let nh = 0;
      for (let i = l.i0; i < l.i1; i++) nh += hit[i];
      if (nh === 0) for (let i = l.i0; i < l.i1; i++) rad[i] = 0.002;
      else if (nh < L) {
        for (let i = l.i0; i < l.i1; i++) {
          if (hit[i]) continue;
          let a = i, b = i, da = 0, db = 0;
          while (!hit[a]) { a = l.i0 + (a - l.i0 + L - 1) % L; da++; }
          while (!hit[b]) { b = l.i0 + (b - l.i0 + 1) % L; db++; }
          rad[i] = (rad[a] * db + rad[b] * da) / (da + db);
        }
      }
      for (let i = l.i0; i < l.i1; i++) {
        const [dx, dz] = l.dir(2 * Math.PI * (i - l.i0 + 0.5) / L);
        const q = 4 * (j * W + i);
        pos[q] = cxj + rad[i] * dx;
        pos[q + 1] = y;
        pos[q + 2] = czj + rad[i] * dz;
        pos[q + 3] = hit[i];
        cen[2 * (j * W + i)] = cxj;
        cen[2 * (j * W + i) + 1] = czj;
      }
    });
  }
  // normals from the image itself: central differences, u wrapping
  // within the texel's own loop, v one-sided at a band's edge (the rows
  // either side belong to different loops and a difference across them
  // is a stretch, not a tangent); outward, checked against the radial
  // from the ray origin, since a webbed gap can fold either way
  const nrm = new Float32Array(W * H * 4);
  const at = (i, j) => 4 * (j * W + i);
  const uN = (i, j, d) => { const l = loopAt(i, j); const L = l.i1 - l.i0; return l.i0 + (i - l.i0 + d + L) % L; };
  for (let j = 0; j < H; j++) {
    const jp = j === H - 1 || bandOf(j + 1) !== bandOf(j) ? j : j + 1;
    const jm = j === 0 || bandOf(j - 1) !== bandOf(j) ? j : j - 1;
    for (let i = 0; i < W; i++) {
      const a = at(uN(i, j, 1), j), b = at(uN(i, j, -1), j), c = at(i, jp), d = at(i, jm);
      const ux = pos[a] - pos[b], uy = pos[a + 1] - pos[b + 1], uz = pos[a + 2] - pos[b + 2];
      const vx = pos[c] - pos[d], vy = pos[c + 1] - pos[d + 1], vz = pos[c + 2] - pos[d + 2];
      let nx = vy * uz - vz * uy, ny = vz * ux - vx * uz, nz = vx * uy - vy * ux;
      const k = at(i, j);
      const rx = pos[k] - cen[2 * (j * W + i)], rz = pos[k + 2] - cen[2 * (j * W + i) + 1];
      const len = Math.hypot(nx, ny, nz);
      if (len < 1e-9) { const rl = Math.hypot(rx, rz) || 1; nx = rx / rl; ny = 0; nz = rz / rl; }
      else { nx /= len; ny /= len; nz /= len; }
      if (nx * rx + nz * rz < 0) { nx = -nx; ny = -ny; nz = -nz; }
      nrm[k] = nx; nrm[k + 1] = ny; nrm[k + 2] = nz; nrm[k + 3] = 0;
    }
  }
  // the band edges in (u, v), for the shader to keep its samples and the
  // terrain's triangles within one loop: v below which the rows are
  // legs, the v range of the arm rows, and the arms' share of u
  const bands = [(crotch + 1) / H, armLo >= 0 ? armLo / H : 0, armLo >= 0 ? (armHi + 1) / H : 0, aW / W];
  return { w: W, h: H, pos, nrm, bands };
};

// --- body mode ------------------------------------------------------------
// B wraps the rect onto a figure instead of rolling it into a tube: the
// same rect, the same load-then-lock-then-roll pipeline, a different warp
// (earth_core.h tube_body). the figure is web/body.obj — makehuman's base
// mesh, CC0, see tools/bodymesh.mjs — fetched the first time it's wanted
// and baked (bodybake.js) into the geometry image the engine samples.
// dropping an .obj (y up, any scale, any size) swaps the figure
let bodyPromise = null;
// parse, bake, and hand the image to the engine; false for a mesh with
// nothing in it. takes effect on the next frame, mid-flight included
const setBodyMesh = (text) => {
  const mesh = parseObj(text);
  if (!mesh.f.length || mesh.v.length < 9) return false;
  const img = bakeBody(mesh);
  const buf = view.bodyBuffer(img.w, img.h);
  buf.pos.set(img.pos);
  buf.nrm.set(img.nrm);
  view.bodyCommit(img.bands[0], img.bands[1], img.bands[2], img.bands[3]);
  return true;
};
const ensureBody = () => {
  if (view.bodyReady()) return Promise.resolve(true);
  if (!bodyPromise) {
    bodyPromise = fetch('body.obj').then((r) => {
      if (!r.ok) throw new Error(r.statusText);
      return r.text();
    }).then((text) => setBodyMesh(text)).catch((err) => {
      console.warn('body mesh:', err);
      return false;
    }).finally(() => { bodyPromise = null; });
  }
  return bodyPromise;
};

// --- tunnel mode ----------------------------------------------------------
// the tube without the rect: the ground ahead curls up in front of the
// camera, over it and down behind, whichever way it faces — the camera
// on the axis, looking along the circle, new land rolling up ahead as
// you fly (earth_core tunnelUpdate). one parameter, the circumference;
// T toggles it, the panel's slider sets it
const tunnelAxisBtn = document.getElementById('tunnelaxis');
const tunnelLenRow = document.getElementById('tunnellenrow');
const fmtM = (m) => m >= 1000 ? `${(m / 1000).toFixed(m >= 10000 ? 0 : 1)} km` : `${Math.round(m)} m`;
// the length slider runs from 1.5 circumferences (where the other axis
// sits) to 60, past the tube's node cap — to feel where it gives out
const K_MIN = 1.5, K_MAX = 60;
// the panel's sliders, bound in initTunnel
let tunnelControls = [];
const refreshTunnel = () => {
  for (const refresh of tunnelControls) refresh();
  tunnelAxisBtn.textContent = S.tunnelAcross ? 'forward and backward' : 'left to right';
  tunnelLenRow.style.display = S.tunnelAcross ? 'none' : '';
};
// going in, the ground rolls up around you from flat; going out it
// unrolls back to flat first (tunnelClosing, the tick eases curl to 0)
// and the machinery goes off once it's there. animate: false skips both —
// a reload lands straight in the tunnel it left
let tunnelClosing = false;
let curlTarget = 1; // 0 only while the tunnel unrolls on its way out
const tunnelHooks = {
  enter: ({ animate = true }) => {
    view.setTubeBody(false); // the tunnel is always the cylinder
    view.setTunnelAcross(S.tunnelAcross);
    view.setTunnelLag(S.tunnelLag * 1000);
    view.setTunnelLength(S.tunnelK);
    view.setTunnel(true, S.tunnelC);
    S.curl = animate ? 0 : 1;
    curlTarget = 1;
    view.setTubeCurl(S.curl);
    S.following = true;
  },
  exit: () => {
    tunnelClosing = false;
    curlTarget = 1;
    view.setTunnel(false, S.tunnelC);
    view.setTubeEnabled(false);
    document.body.classList.remove('tunnel');
  },
};
// the end of the way out, once the frame loop has eased the curl down
const endTunnel = () => setMode('fly');
const setTunnel = (on, animate = true) => {
  if (on) {
    if (mode === 'tunnel' && !tunnelClosing) return;
    if (tunnelClosing) {
      // caught on the way out: back up from wherever it got to
      tunnelClosing = false;
      curlTarget = 1;
    } else {
      setMode('tunnel', { animate });
    }
  } else {
    if (mode !== 'tunnel' || tunnelClosing) return;
    if (animate) {
      tunnelClosing = true;
      curlTarget = 0;
    } else {
      endTunnel();
    }
  }
  document.body.classList.toggle('tunnel', on);
  refreshTunnel();
  updateHint();
  saveView();
  canvas.focus();
};
// the tunnel's part of the saved view: its settings, and whether it was on
// (not if it was on its way out)
const tunnelState = () => ({ on: mode === 'tunnel' && !tunnelClosing, c: S.tunnelC,
  across: S.tunnelAcross, lag: S.tunnelLag, k: S.tunnelK });
// tubeRolled: the saved view also had the rect rolled, which wins
const restoreTunnel = (t, tubeRolled) => {
  if (t && Number.isFinite(t.c)) S.tunnelC = t.c;
  if (t && t.across === false) S.tunnelAcross = false;
  if (t && Number.isFinite(t.lag)) S.tunnelLag = Math.max(0, t.lag);
  if (t && Number.isFinite(t.k)) S.tunnelK = Math.max(K_MIN, Math.min(K_MAX, t.k));
  refreshTunnel();
  if (t && t.on && !tubeRolled) setTunnel(true, false);
};
const initTunnel = () => {
  defineMode('tunnel', tunnelHooks);

  tunnelAxisBtn.addEventListener('click', () => {
    S.tunnelAcross = !S.tunnelAcross;
    view.setTunnelAcross(S.tunnelAcross);
    refreshTunnel();
    saveView();
    canvas.focus();
  });

  // each slider hands its own setting to the engine
  const applied = (send) => () => { send(); refreshTunnel(); saveView(); };
  tunnelControls = [
    bindRange('tunnelc', S, 'tunnelC', { scale: logScale(200, 20000),
      apply: applied(() => view.setTunnelCircumference(S.tunnelC)),
      fmt: (c) => `${fmtM(c)} around`, amount: 'tunnelamount' }),
    // zero at its left end, then a tenth of a second to ten
    bindRange('tunnellag', S, 'tunnelLag', { scale: logScale(0.1, 10, { off: true }),
      apply: applied(() => view.setTunnelLag(S.tunnelLag * 1000)),
      fmt: (t) => t > 0 ? `${t.toFixed(t < 1 ? 2 : 1)} s to turn` : 'turns at once' }),
    bindRange('tunnellen', S, 'tunnelK', { scale: logScale(K_MIN, K_MAX),
      apply: applied(() => view.setTunnelLength(S.tunnelK)),
      fmt: (k) => `${fmtM(k * S.tunnelC)} ahead · ${k.toFixed(k < 10 ? 1 : 0)}×` }),
  ];
  document.getElementById('tunneloff').addEventListener('click', () => setTunnel(false));
};

// --- tube mode ------------------------------------------------------------
// shift+drag on the map draws a rectangle; that slab of territory is rolled
// into a tube (the inception effect) and the camera teleports to one end of
// the tube axis, looking down it. T rolls/unrolls; drawing a new rect
// replaces the old one. the roll animates via curl (0 flat .. 1 closed)
let tubeRect = null; // [latMin, lonMin, latMax, lonMax] degrees
// which way the tube axis runs; the rolled-up (glued) edges are the two
// edges parallel to it. toggled by the #axisbtn switch
let tubeAxisEw = true;
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
  if (tubeAxisEw) {
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
  axisBtn.textContent = `tube axis: ${tubeAxisEw ? 'E–W' : 'N–S'} (click to flip)`;
};
// fly to the near end of the tube axis: the axis runs along the rect's
// longer dimension at height = tube radius, so spawning at (end, radius)
// heading along the axis puts you dead center in the pipe, looking down it
const flyIntoTube = (r) => {
  const latc = (r[0] + r[2]) / 2, lonc = (r[1] + r[3]) / 2;
  const ew = tubeAxisEw;
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
  if (tubeAxisEw) view.setPose(latc + dlat, lonc, info.groundAlt + 0.55 * h, 180, 0);
  else view.setPose(latc, lonc - dlon, info.groundAlt + 0.55 * h, 90, 0);
  S.following = true;
};
// T rolls the rect into a tube, B wraps it onto the body (shape is 'tube'
// or 'body'). the body's figure is fetched and baked on first use; the roll
// waits for it (a cylinder standing in meanwhile would be the wrong
// picture), and falls back to the tube if it can't be had
// the rect's shape, 'tube' or 'body': the last one it was rolled into,
// which a new rect drawn over it is rolled into as well
let rectShape = 'tube';
const rollRect = (shape, teleport) => {
  rectShape = shape;
  if (shape === 'body' && !view.bodyReady()) {
    const r = tubeRect;
    ensureBody().then((ok) => {
      if (!ok) toast('the body mesh failed to load');
      if (tubeRect === r) rollRect(ok ? 'body' : 'tube', teleport);
    });
    return;
  }
  setMode(shape, { teleport });
};
// staged pipeline: the sheet stays flat while the rect's tiles load at
// full res; once the scene is complete the engine measures ground level
// off the mesh, and only then does the roll (and camera fly-in) happen.
// 'fly' also teleports into the tube mouth; 'roll' just rolls in place
let tubePending = null; // null | 'fly' | 'roll'
// the tube and the body share these: switching between the two re-enters
// with the other warp, and leaving either goes back to flat flight over
// the rect, which stays drawn
const tubeHooks = {
  enter: ({ teleport }) => {
    const r = tubeRect;
    view.setTubeBody(mode === 'body');
    view.setTubeRect(r[0], r[1], r[2], r[3], tubeAxisEw);
    view.setTubeEnabled(true);
    view.setAirplane(true); // tube flying is airplane-frame
    S.curl = 0; // flat while the rect loads; the tick rolls it when ready
    view.setTubeCurl(0);
    tubePending = teleport ? 'fly' : 'roll';
    tubePendingSince = performance.now();
  },
  exit: () => {
    tubePending = null;
    view.setTubeEnabled(false);
    view.setAirplane(false);
  },
};
// the rect's part of the saved view, null without one
const tubeState = () => tubeRect
  ? { rect: tubeRect, on: inTube(), ewAxis: tubeAxisEw, body: rectShape === 'body' } : null;
// and back: a rect that was rolled rolls up again from flat, in place (the
// camera pose is already wherever it was saved). says whether it did
const restoreTube = (t) => {
  if (!t || !t.rect) return false;
  tubeRect = t.rect;
  tubeAxisEw = t.ewAxis !== false;
  rectShape = t.body ? 'body' : 'tube';
  updateAxisBtn();
  setRectData(tubeRect);
  if (t.on) rollRect(rectShape, false);
  else view.setTubeRect(tubeRect[0], tubeRect[1], tubeRect[2], tubeRect[3], tubeAxisEw);
  return !!t.on;
};
// esc: unroll, drop the rect, back to normal flight
const clearRect = () => {
  tubeRect = null;
  rectShape = 'tube';
  if (inTube()) setMode('fly');
  setRectData(null);
};
// stage 2+3, from the frame loop: the engine locks ground level as soon as
// the rect is covered at measurement resolution (well before full res),
// then we fly in and roll while the rest keeps streaming into the rolled
// tube. after 20s (a download stuck or failed), force the measurement with
// what's resident. returns the tube's info, for the loading readout
const advanceTube = (now) => {
  let ti = view.getTubeInfo();
  if (tubePending) {
    if (now - tubePendingSince > 20000 && !ti.groundLocked) {
      view.lockTubeGround();
      ti = view.getTubeInfo();
    }
    if (ti.groundLocked) {
      if (tubePending === 'fly') (mode === 'body' ? flyOntoBody : flyIntoTube)(tubeRect);
      tubePending = null;
    }
  }
  return ti;
};
let drawAnchor = null;
const initTubeRect = () => {
  defineMode('tube', tubeHooks);
  defineMode('body', tubeHooks);

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
    setRectData(tubeRect);
  });

  updateAxisBtn();
  axisBtn.addEventListener('click', () => {
    tubeAxisEw = !tubeAxisEw;
    updateAxisBtn();
    if (tubeRect) {
      setRectData(tubeRect); // arrows move to the other edge pair
      if (inTube()) rollRect(mode, true); // re-roll around the new axis
      else view.setTubeRect(tubeRect[0], tubeRect[1], tubeRect[2], tubeRect[3], tubeAxisEw);
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
    if (r[2] - r[0] < 3e-4 && r[3] - r[1] < 3e-4) { setRectData(tubeRect); return; }
    tubeRect = r;
    setRectData(r);
    updateHint(); // T and B mean something now
    if (view.ready()) rollRect(rectShape, true);
  });
};

const initTube = () => {
  initTunnel();
  initTubeRect();
};
