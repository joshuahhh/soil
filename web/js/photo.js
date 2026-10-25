import { S } from './state.js';
import { idbDel } from './idb.js';
import { canvas, rightEl } from './engine.js';
import { saveAlign, clearPairs } from './align.js';
import { updateHint } from './hud.js';

export { overlayImg, overlayBlend, ov, photoShown, clampScale, applyOverlayBlend, overlayBox, layoutOverlay,
  viewToPhoto, clampLens, distortUV, photoToViewK, warpEl, setLens, fitOverlay, setOverlay,
  clearOverlay, toggleOverlayBlend, initPhoto };

// --- reference image overlay ----------------------------------------------
// drop an image file anywhere on the page and it lays over the 3d view, so
// you can steer the camera until the render lines up with the photo. the
// slider blends it against the render; the ✕ in the corner removes it.
// the file never leaves the browser, but it is kept (in indexeddb, next to
// the tile cache) so a reload comes back to the same photo — the point pairs
// hanging off it would be meaningless without it
const overlayImg = document.getElementById('overlay');
const overlayBlend = document.getElementById('overlayblend');
let overlayUrl = null;
// up once it has decoded, until the ✕ (P only fades it): what body.overlay shows
let photoShown = false;
// where the photo sits over the viewport. scale is its height as a fraction
// of the viewport's, cx/cy its center as a fraction of the viewport — all
// relative, so a window resize rescales the photo with the frustum instead
// of sliding the picture through it
// k is the lens: a division-model distortion about the photo's centre,
// negative for barrel. it belongs to the photo, so it lives here with the
// photo's placement rather than in the camera pose
let ov = { scale: 1, cx: 0.5, cy: 0.5, k: 0 };
// how far the photo can be blown up: 16x the viewport height is enough to
// put a window frame or a roof ridge under the pointer when placing a pair,
// which is what the big end is for. the slider's own units are log across
// that range — linear, it would spend four fifths of its track above 4x and
// leave the small end unadjustable
const SCALE_MIN = 0.05, SCALE_MAX = 16;
const clampScale = (s) => Math.max(SCALE_MIN, Math.min(SCALE_MAX, s));
const applyOverlayBlend = () => {
  overlayImg.style.opacity = overlayBlend.value / 100;
  warpEl.style.opacity = overlayBlend.value / 100;
};
// the photo's box in viewport pixels; null until the image has decoded
const overlayBox = () => {
  if (!overlayImg.naturalWidth || !overlayImg.naturalHeight) return null;
  const W = rightEl.clientWidth, H = rightEl.clientHeight;
  const h = ov.scale * H, w = h * overlayImg.naturalWidth / overlayImg.naturalHeight;
  return { x: ov.cx * W - w / 2, y: ov.cy * H - h / 2, w, h };
};
const layoutOverlay = () => {
  const b = overlayBox();
  if (!b) return;
  overlayImg.style.left = b.x + 'px';
  overlayImg.style.top = b.y + 'px';
  overlayImg.style.width = b.w + 'px';
  overlayImg.style.height = b.h + 'px';
  // the warp canvas is the box grown by its margin: m in total, half a
  // side each way, matching the ±(1+m) span it is drawn over
  const m = warpMargin;
  warpEl.style.left = (b.x - b.w * m / 2) + 'px';
  warpEl.style.top = (b.y - b.h * m / 2) + 'px';
  warpEl.style.width = b.w * (1 + m) + 'px';
  warpEl.style.height = b.h * (1 + m) + 'px';
};
// photo coordinates (0..1 across the image) <-> viewport pixels. pairs are
// stored in photo coordinates so moving or resizing the photo carries them.
// these two are the box only: the image as displayed, which is the photo
// undistorted when k is set. a pair's own (u, v) are on the photo as
// shot, so they go through undistortUV on the way out and distortUV on
// the way in — a pair placed under one k stays on its feature under another
const photoToView = (u, v) => {
  const b = overlayBox();
  return b ? { x: b.x + u * b.w, y: b.y + v * b.h } : null;
};
const viewToPhoto = (x, y) => {
  const b = overlayBox();
  return b ? { u: (x - b.x) / b.w, v: (y - b.y) / b.h } : null;
};
// the lens. division model (fitzgibbon): r_u = r_d / (1 + k r_d²), radii
// from the centre with the photo's half-height as the unit, so k means the
// same thing whatever the picture's pixel size. one parameter, invertible
// in closed form (the inverse is a quadratic in r_d), and near-linear in k,
// which is what makes it fit alongside fov. the centre of distortion is
// taken to be the image centre — true of an uncropped photo
const lensAspect = () => overlayImg.naturalWidth / overlayImg.naturalHeight;
// how far k may go. the model folds at r = 1/√|k|. pincushion turns back
// from there, so the photo's corner (r = √(a² + 1)) must stay inside, with
// room. barrel sends that radius to infinity instead: past it the photo
// has no place on screen, which the warp shows as blank and a pair there
// as absent — so barrel may go until the fold reaches the half-width, and
// only the corners are lost
const lensLimits = () => {
  const a = overlayImg.naturalWidth ? lensAspect() : 1.5;
  return { barrel: -1 / (a * a), pincushion: 0.6 / (a * a + 1) };
};
const clampLens = (k) => {
  const l = lensLimits();
  return Math.max(l.barrel, Math.min(l.pincushion, Number(k) || 0));
};
// null past the fold: a point of the photo this barrel puts nowhere
const undistortUV = (u, v, k) => {
  if (!k) return { u, v };
  const a = lensAspect();
  const X = (u - 0.5) * 2 * a, Y = (v - 0.5) * 2;
  const d = 1 + k * (X * X + Y * Y);
  if (d <= 1e-6) return null;
  const f = 1 / d;
  return { u: 0.5 + X * f / (2 * a), v: 0.5 + Y * f / 2 };
};
// the inverse: the radius as shot of the point the undistorted picture
// has at r_u. null where the model has no answer: beyond 1/(4k) of
// pincushion, or a barrel corner pushed out past where the inverse is real
const distortRadius = (ru, k) => {
  const q = 4 * k * ru * ru;
  if (q > 1) return null;
  // (1 - sqrt(1 - q)) / (2 k r_u), with the series where the subtraction
  // would lose its digits
  return Math.abs(q) < 1e-6 ? ru * (1 + k * ru * ru) : (1 - Math.sqrt(1 - q)) / (2 * k * ru);
};
const distortUV = (u, v, k) => {
  if (!k) return { u, v };
  const a = lensAspect();
  const X = (u - 0.5) * 2 * a, Y = (v - 0.5) * 2;
  const ru = Math.hypot(X, Y);
  if (ru < 1e-9) return { u, v };
  const rd = distortRadius(ru, k);
  if (rd === null) return null;
  const sc = rd / ru;
  return { u: 0.5 + X * sc / (2 * a), v: 0.5 + Y * sc / 2 };
};
// where a photo point (as shot) shows on screen under lens k
const photoToViewK = (u, v, k) => {
  const p = undistortUV(u, v, k);
  return p ? photoToView(p.u, p.v) : null;
};

// the undistorted photo, when k is set: the <img> can't be warped, so a
// canvas is drawn in its place — a grid of cells over the undistorted
// picture, each filled from the source through the affine that carries
// its three corners. redrawn only when k changes, never per frame. barrel
// pushes the corners outward, so the canvas is the box plus a margin for
// them, capped: a strong lens sends a corner off to infinity
const warpEl = document.getElementById('overlaywarp');
let warpMargin = 0;
const renderWarp = () => {
  const k = ov.k || 0;
  const on = !!k && overlayImg.naturalWidth > 0;
  document.body.classList.toggle('distorted', on);
  if (!on) return;
  const a = lensAspect();
  const rc = Math.hypot(a, 1); // the corner, as shot
  const ruc = rc / (1 + k * rc * rc);
  // the corner's outward push, as a fraction of the box; capped, and
  // doubled because it is spent half on each side
  warpMargin = k < 0 ? Math.min(1.0, 2 * Math.max(0, (ruc > 0 ? ruc / rc : 9) - 1)) : 0;
  const m = warpMargin;
  // canvas pixels: the photo's own, capped — it is a viewport-sized picture
  const natW = overlayImg.naturalWidth, natH = overlayImg.naturalHeight;
  const sc = Math.min(1, 2048 / natW);
  // the canvas spans ±a(1+m) by ±(1+m): the box grown by m in total,
  // half on each side. every size below has to use the same (1 + m), or
  // the picture is drawn at a different scale from the box the markers
  // are laid out in (it was: barrel was a fraction too big, and the
  // pairs slid off their features as k changed)
  const cw = Math.round(natW * sc * (1 + m)), ch = Math.round(natH * sc * (1 + m));
  warpEl.width = cw; warpEl.height = ch;
  const ctx = warpEl.getContext('2d');
  ctx.clearRect(0, 0, cw, ch);
  // undistorted normalised coords (X in ±a(1+m), Y in ±(1+m)) -> canvas px
  const toCanvas = (X, Y) => [(X / (2 * a * (1 + m)) + 0.5) * cw, (Y / (2 * (1 + m)) + 0.5) * ch];
  // canvas px -> photo px as shot, through the lens; null off the model
  const toSource = (X, Y) => {
    const ru = Math.hypot(X, Y);
    if (ru < 1e-9) return [natW / 2, natH / 2];
    const rd = distortRadius(ru, k);
    if (rd === null) return null;
    const s = rd / ru;
    return [(X * s / (2 * a) + 0.5) * natW, (Y * s / 2 + 0.5) * natH];
  };
  const N = 36;
  const spanX = 2 * a * (1 + m), spanY = 2 * (1 + m);
  for (let j = 0; j < N; j++) for (let i = 0; i < N; i++) {
    const X0 = -a * (1 + m) + spanX * i / N, X1 = X0 + spanX / N;
    const Y0 = -(1 + m) + spanY * j / N, Y1 = Y0 + spanY / N;
    const s0 = toSource(X0, Y0), s1 = toSource(X1, Y0), s2 = toSource(X0, Y1);
    if (!s0 || !s1 || !s2) continue;
    // cells whose source lies wholly outside the photo have nothing to draw
    if (Math.max(s0[0], s1[0], s2[0]) < 0 || Math.min(s0[0], s1[0], s2[0]) > natW
      || Math.max(s0[1], s1[1], s2[1]) < 0 || Math.min(s0[1], s1[1], s2[1]) > natH) continue;
    const t0 = toCanvas(X0, Y0), t1 = toCanvas(X1, Y0), t2 = toCanvas(X0, Y1);
    // the affine with M s_i = t_i: M_lin = [t1-t0 t2-t0] [s1-s0 s2-s0]^-1
    const ax = s1[0] - s0[0], ay = s1[1] - s0[1], bx = s2[0] - s0[0], by = s2[1] - s0[1];
    const det = ax * by - ay * bx;
    if (Math.abs(det) < 1e-12) continue;
    const px = t1[0] - t0[0], py = t1[1] - t0[1], qx = t2[0] - t0[0], qy = t2[1] - t0[1];
    // [p q] · inv([a b]) with inv = [by -bx; -ay ax] / det
    const m11 = (px * by - qx * ay) / det, m12 = (-px * bx + qx * ax) / det;
    const m21 = (py * by - qy * ay) / det, m22 = (-py * bx + qy * ax) / det;
    const e = t0[0] - (m11 * s0[0] + m12 * s0[1]);
    const f = t0[1] - (m21 * s0[0] + m22 * s0[1]);
    ctx.save();
    ctx.beginPath();
    // a hair of overlap, or the cell seams show as a lattice of gaps
    ctx.rect(t0[0] - 0.5, t0[1] - 0.5, t1[0] - t0[0] + 1, t2[1] - t0[1] + 1);
    ctx.clip();
    ctx.setTransform(m11, m21, m12, m22, e, f);
    ctx.drawImage(overlayImg, 0, 0);
    ctx.restore();
  }
};
// ov.k, and everything that follows from it: the warp, its panel, the
// markers (alignRefresh's per-frame pass moves those on its own)
const lensAmountEl = document.getElementById('lensamount');
const lensSlider = document.getElementById('lensk');
const refreshLens = () => {
  const k = ov.k || 0;
  const lim = lensLimits();
  lensSlider.min = lim.barrel;
  lensSlider.max = lim.pincushion;
  lensSlider.value = k;
  // 'k' and the sign carry the number; the word is the reading of it
  lensAmountEl.textContent = !k ? 'k = 0 · none'
    : `k = ${k < 0 ? '−' : '+'}${Math.abs(k).toFixed(3)} · ${k < 0 ? 'barrel' : 'pincushion'}`;
};
const setLens = (k) => {
  k = clampLens(k);
  if (Math.abs(k) < 1e-4) k = 0;
  if (k === ov.k) return;
  ov.k = k;
  renderWarp();
  layoutOverlay();
  refreshLens();
};
// "fit": the largest scale that keeps the whole photo inside the viewport
const fitOverlay = () => {
  if (!overlayImg.naturalWidth) return;
  const W = rightEl.clientWidth, H = rightEl.clientHeight;
  ov.scale = Math.min(1, W / (H * (overlayImg.naturalWidth / overlayImg.naturalHeight)));
  ov.cx = ov.cy = 0.5;
  layoutOverlay();
};
const setOverlay = (file, keepPlacement) => {
  if (overlayUrl) URL.revokeObjectURL(overlayUrl);
  overlayUrl = URL.createObjectURL(file);
  // the photo is only shown once it has decoded: its placement needs the
  // natural size, and revealing it before that flashes the image at full
  // resolution across the viewport
  overlayImg.onload = () => {
    if (!keepPlacement) { fitOverlay(); ov.k = 0; } // a new photo, a new lens
    renderWarp();
    refreshLens();
    layoutOverlay();
    photoShown = true;
    document.body.classList.add('overlay');
    applyOverlayBlend();
    updateHint(); // which re-decides the pairs (alignRefresh)
  };
  overlayImg.onerror = () => {
    overlayUrl = null;
    idbDel('ref-image', 'app');
  };
  overlayImg.src = overlayUrl;
  canvas.focus(); // the drop shouldn't cost you the flight keys
};
// the ✕ is the "get rid of it" button: the photo goes, and so do the point
// pairs, which are anchored to points of that picture and mean nothing
// without it. the panel's "clear pairs" is the narrower one
const clearOverlay = () => {
  photoShown = false;
  document.body.classList.remove('overlay');
  document.body.classList.remove('distorted');
  ov.k = 0;
  refreshLens();
  overlayImg.removeAttribute('src');
  if (overlayUrl) { URL.revokeObjectURL(overlayUrl); overlayUrl = null; }
  idbDel('ref-image', 'app');
  S.refImageKey = null;
  clearPairs();
  updateHint();
  saveAlign();
  canvas.focus();
};
// the word "opacity" is a switch: full if the photo is faded at all, off if
// it's full. quicker than dragging the slider end to end while comparing
const toggleOverlayBlend = () => {
  if (!photoShown) return;
  overlayBlend.value = +overlayBlend.value >= 100 ? 0 : 100;
  applyOverlayBlend();
  saveAlign();
  canvas.focus();
};
const initPhoto = () => {
  lensSlider.addEventListener('input', () => { setLens(lensSlider.value); saveAlign(); });
  document.getElementById('lensreset').addEventListener('click', () => {
    setLens(0);
    saveAlign();
    canvas.focus();
  });

  overlayBlend.addEventListener('input', applyOverlayBlend);
  document.getElementById('overlayblendtoggle').addEventListener('click', toggleOverlayBlend);
};
