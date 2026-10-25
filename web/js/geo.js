export { D2R, planetR, setPlanetR, vsub, vmul, vadd, vdot, vcross, vnorm, geoUpV, geoNorthV, ecefOf,
  poseBasis, projectWith };

// vector helpers and the camera projection, in ecef metres. no dom, no engine:
// the alignment solver and the follow camera both work in these terms
//
// the camera projection is reimplemented here rather than called through the
// engine, so a solver iteration costs no wasm round trips — it mirrors
// earth_camera.h's poseToCamera and rocktree_math.h's lookAt/perspective,
// and pickNdc on the wasm side is built from the same basis
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
// a pair's mesh anchor as ecef meters. pickNdc reports the hit in spherical
// terms against the same radius, so this rebuilds the exact point
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
  // the camera's bank turns the screen axes about the boresight — the same
  // rotation the engine applies to gravity when it builds the view matrix,
  // so a pair drawn from this basis stays on its feature in a rolled view
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
