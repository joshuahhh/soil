export { parseObj, bakeBody };

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
