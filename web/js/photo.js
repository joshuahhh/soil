import { S, loadStore, saveStore } from './config.js';
import { ecefOf, poseBasis, projectWith, paramSpecs, lmStep } from './math.js';
import { idbDel, idbGet } from './tiles.js';
import { canvas, rightEl, view } from './engine.js';
import { mode, ortho, setHug } from './view.js';
import { updateHint } from './hud.js';

export { initPhoto, overlayImg, overlayBlend, ov, photoShown, clampScale, applyOverlayBlend,
  overlayBox, layoutOverlay, viewToPhoto, warpEl, setOverlay, toggleOverlayBlend, alignOn, pairs,
  svgEl, saveAlign, clearPairs, alignRefresh, lastErrs, updateMarkers, updateStatus, dofCount,
  capture, captureClick, solving, solveStep, stopSolve, startSolve };

// --- reference image overlay ----------------------------------------------
const overlayImg = document.getElementById('overlay');
const overlayBlend = document.getElementById('overlayblend');
let overlayUrl = null;
// up once it has decoded, until the ✕ (P only fades it): what body.overlay shows
let photoShown = false;
// scale: height as a fraction of the viewport's; cx/cy: centre, as fractions
// (relative, so a resize scales the photo with the frustum). k: the lens,
// negative for barrel — the photo's, not the camera's
let ov = { scale: 1, cx: 0.5, cy: 0.5, k: 0 };
// 16x puts a window frame under the pointer when placing a pair
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
  // the warp canvas: the box grown by m in total, m/2 each side (as renderWarp)
  const m = warpMargin;
  warpEl.style.left = (b.x - b.w * m / 2) + 'px';
  warpEl.style.top = (b.y - b.h * m / 2) + 'px';
  warpEl.style.width = b.w * (1 + m) + 'px';
  warpEl.style.height = b.h * (1 + m) + 'px';
};
// photo coords (0..1) <-> viewport px, on the photo as displayed (undistorted).
// pairs are stored as shot: they go through undistortUV / distortUV
const photoToView = (u, v) => {
  const b = overlayBox();
  return b ? { x: b.x + u * b.w, y: b.y + v * b.h } : null;
};
const viewToPhoto = (x, y) => {
  const b = overlayBox();
  return b ? { u: (x - b.x) / b.w, v: (y - b.y) / b.h } : null;
};
// the lens. division model (fitzgibbon): r_u = r_d / (1 + k r_d²), r from the
// image centre in half-heights. invertible in closed form, near-linear in k
const lensAspect = () => overlayImg.naturalWidth / overlayImg.naturalHeight;
// the model folds at r = 1/√|k|. pincushion must keep the corner (r = √(a²+1))
// well inside; barrel may fold as close as the half-width, losing the corners
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
// the inverse, r_u -> r_d: the smaller root of k r_u r_d² − r_d + r_u = 0.
// null where it has no real root (4k r_u² > 1)
const distortRadius = (ru, k) => {
  const q = 4 * k * ru * ru;
  if (q > 1) return null;
  // the series where the subtraction would cancel
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

// the undistorted photo when k is set: a grid of cells, each drawn from the
// source through the affine of its three corners. redrawn only when k changes.
// barrel pushes the corners out, so the canvas has a (capped) margin
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
  // the corner's outward push as a fraction of the box, ×2 for both sides
  warpMargin = k < 0 ? Math.min(1.0, 2 * Math.max(0, (ruc > 0 ? ruc / rc : 9) - 1)) : 0;
  const m = warpMargin;
  const natW = overlayImg.naturalWidth, natH = overlayImg.naturalHeight;
  const sc = Math.min(1, 2048 / natW);
  // spans ±a(1+m) by ±(1+m). every size must use the same (1 + m) as
  // layoutOverlay, or the pairs slide off their features as k changes
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
const lensAmountEl = document.getElementById('lensamount');
const lensSlider = document.getElementById('lensk');
const refreshLens = () => {
  const k = ov.k || 0;
  const lim = lensLimits();
  lensSlider.min = lim.barrel;
  lensSlider.max = lim.pincushion;
  lensSlider.value = k;
  lensAmountEl.textContent = !k ? 'k = 0 · none'
    : `k = ${k < 0 ? '−' : '+'}${Math.abs(k).toFixed(3)} · ${k < 0 ? 'barrel' : 'pincushion'}`;
};
const setLens = (k) => {
  k = clampLens(k);
  if (Math.abs(k) < 1e-4) k = 0;
  if (k === ov.k) return;
  ov.k = k;
  renderWarp(); // the markers follow on their own, per frame
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
  // shown only once decoded: before that it flashes at full resolution
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
// the ✕: the photo and its pairs go
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
const toggleOverlayBlend = () => {
  if (!photoShown) return;
  overlayBlend.value = +overlayBlend.value >= 100 ? 0 : 100;
  applyOverlayBlend();
  saveAlign();
  canvas.focus();
};
const initPhotoOverlay = () => {
  lensSlider.addEventListener('input', () => { setLens(lensSlider.value); saveAlign(); });
  document.getElementById('lensreset').addEventListener('click', () => {
    setLens(0);
    saveAlign();
    canvas.focus();
  });

  overlayBlend.addEventListener('input', applyOverlayBlend);
  document.getElementById('overlayblendtoggle').addEventListener('click', toggleOverlayBlend);
};

// --- point-pair alignment -------------------------------------------------
// perspective only, never in the tube: the fit assumes a pinhole image of
// fixed geometry

// --- state ---
let pairs = [];       // { u, v, lat, lon, alt } — photo coords + mesh anchor
let selPair = -1;
// hovered from its row or its markers: both light up
let hovPair = -1;
const freeFlags = { pos: true, aim: true, fov: true, roll: false, dist: false };
const listEl = document.getElementById('aplist');
const statusEl = document.getElementById('apstatus');
const warnEl = document.getElementById('apwarn');
const bannerEl = document.getElementById('apbanner');
const svgEl = document.getElementById('pairs');
const solveBtn = document.getElementById('apsolve');
const addBtn = document.getElementById('apadd');

// paints sel/hov without a rebuild, which would reset the error text and
// yank the delete buttons
const applyPairStates = () => {
  markerEls.forEach((m, i) => {
    m.g.classList.toggle('sel', i === selPair);
    m.g.classList.toggle('hov', i === hovPair);
  });
  rowEls.forEach((r, i) => {
    r.row.classList.toggle('sel', i === selPair);
    r.row.classList.toggle('hov', i === hovPair);
  });
};
const setSel = (i) => { selPair = i; applyPairStates(); };
const setHover = (i) => {
  if (hovPair === i) return;
  hovPair = i;
  applyPairStates();
};

function saveAlign() {
  saveStore('earth-align', {
    pairs, ov, free: freeFlags, key: S.refImageKey,
    opacity: +overlayBlend.value,
  });
}
function clearPairs() {
  pairs = [];
  selPair = -1;
  stopSolve();
  buildMarkers();
  renderList();
  saveAlign();
}
// called from updateHint, so every mode switch re-evaluates it
let alignOn = false;
function alignRefresh() {
  alignOn = photoShown && !ortho && (mode === 'fly' || mode === 'follow');
  document.body.classList.toggle('align', alignOn);
  if (!alignOn) { cancelCapture(); stopSolve(); }
  document.getElementById('apfreepos').checked = freeFlags.pos;
  document.getElementById('apfreeaim').checked = freeFlags.aim;
  document.getElementById('apfreefov').checked = freeFlags.fov;
  document.getElementById('apfreeroll').checked = freeFlags.roll;
  document.getElementById('apfreedist').checked = freeFlags.dist;
  // the lens panel also shows while the solver fits k, to set it by eye
  document.body.classList.toggle('lenswanted', freeFlags.dist && alignOn);
  buildMarkers();
  renderList();
}

// --- markers ---
// rebuilt when the pair set changes; positions refresh every frame in tick
let markerEls = [];
const svgNode = (tag, attrs) => {
  const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const k in attrs) el.setAttribute(k, attrs[k]);
  return el;
};
// paint order puts the grab discs last, so an end wins over the line or number
function buildMarkers() {
  svgEl.textContent = '';
  hovPair = -1;
  markerEls = pairs.map((pr, i) => {
    const g = svgNode('g', {});
    // narrow: a hit shape swallows camera drags started on it
    const hitLine = svgNode('line', { class: 'hit pick', 'stroke-width': 12 });
    const line = svgNode('line', { class: 'res', stroke: '#fff', 'stroke-width': 1.5,
      'stroke-dasharray': '4 3', opacity: .9 });
    const photoHalo = svgNode('circle', { r: 12, class: 'halo' });
    const photo = svgNode('circle', { r: 6, fill: 'none', stroke: '#ff2fd0',
      'stroke-width': 2.5 });
    const photoDot = svgNode('circle', { r: 1.5, fill: '#ff2fd0' });
    const meshHalo = svgNode('circle', { r: 12, class: 'halo' });
    const mesh = svgNode('circle', { r: 6, fill: 'none', stroke: '#22d3ee',
      'stroke-width': 2.5 });
    const meshDot = svgNode('circle', { r: 1.5, fill: '#22d3ee' });
    const lbl = svgNode('text', { class: 'lbl pick' });
    lbl.textContent = i + 1;
    const photoHit = svgNode('circle', { r: 13, class: 'hit grab' });
    const meshHit = svgNode('circle', { r: 13, class: 'hit grab' });
    for (const el of [hitLine, line, photoHalo, photo, photoDot,
                      meshHalo, mesh, meshDot, lbl, photoHit, meshHit])
      g.appendChild(el);
    svgEl.appendChild(g);
    hookDrag(photoHit, i, 'photo');
    hookDrag(meshHit, i, 'mesh');
    for (const el of [hitLine, lbl]) el.addEventListener('click', () => setSel(selPair === i ? -1 : i));
    for (const el of [hitLine, lbl, photoHit, meshHit]) {
      el.addEventListener('pointerenter', () => setHover(i));
      el.addEventListener('pointerleave', () => setHover(-1));
    }
    return { g, hitLine, line, photoHalo, photo, photoDot,
             meshHalo, mesh, meshDot, lbl, photoHit, meshHit };
  });
  applyPairStates();
}
let dragPair = null;
function hookDrag(el, i, which) {
  el.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    e.stopPropagation();
    dragPair = { i, which };
    setSel(i);
    el.setPointerCapture(e.pointerId);
  });
  el.addEventListener('pointermove', (e) => {
    if (!dragPair || dragPair.i !== i) return;
    const r = rightEl.getBoundingClientRect();
    const x = e.clientX - r.left, y = e.clientY - r.top;
    const pr = pairs[i];
    if (!pr) return;
    if (which === 'photo') {
      const p = viewToPhoto(x, y);
      const q = p && distortUV(p.u, p.v, ov.k); // back onto the photo as shot
      if (q) { pr.u = q.u; pr.v = q.v; }
    } else {
      const hit = view.pickNdc((x / r.width) * 2 - 1, 1 - (y / r.height) * 2);
      if (hit) { pr.lat = hit.lat; pr.lon = hit.lon; pr.alt = hit.alt; }
    }
  });
  const end = () => { if (dragPair) { dragPair = null; saveAlign(); } };
  el.addEventListener('pointerup', end);
  el.addEventListener('pointercancel', end);
}

// per frame; returns the rms px error over the pairs on screen
let lastErrs = [];
const updateMarkers = () => {
  const W = rightEl.clientWidth, H = rightEl.clientHeight;
  const p = view.getPose();
  const basis = poseBasis(p);
  let sum = 0, n = 0;
  lastErrs = pairs.map((pr, i) => {
    const m = markerEls[i];
    const tgt = photoToViewK(pr.u, pr.v, ov.k);
    const cur = basis ? projectWith(basis, p.fov, ecefOf(pr), W, H) : null;
    if (!m) return null;
    // hit shapes hide with their ends, or they catch the pointer over nothing
    const show = (el, on) => el.setAttribute('visibility', on ? 'visible' : 'hidden');
    for (const el of [m.photo, m.photoDot, m.photoHalo, m.photoHit, m.lbl]) show(el, !!tgt);
    for (const el of [m.mesh, m.meshDot, m.meshHalo, m.meshHit]) show(el, !!cur);
    for (const el of [m.line, m.hitLine]) show(el, !!(tgt && cur));
    const big = i === selPair || i === hovPair;
    m.photo.setAttribute('r', big ? 8 : 6);
    m.mesh.setAttribute('r', big ? 8 : 6);
    if (tgt) {
      for (const el of [m.photo, m.photoDot, m.photoHalo, m.photoHit]) {
        el.setAttribute('cx', tgt.x);
        el.setAttribute('cy', tgt.y);
      }
      m.lbl.setAttribute('x', tgt.x + 10);
      m.lbl.setAttribute('y', tgt.y - 8);
    }
    if (cur) {
      for (const el of [m.mesh, m.meshDot, m.meshHalo, m.meshHit]) {
        el.setAttribute('cx', cur.x);
        el.setAttribute('cy', cur.y);
      }
    }
    if (tgt && cur) {
      for (const el of [m.line, m.hitLine]) {
        el.setAttribute('x1', tgt.x);
        el.setAttribute('y1', tgt.y);
        el.setAttribute('x2', cur.x);
        el.setAttribute('y2', cur.y);
      }
      const d = Math.hypot(cur.x - tgt.x, cur.y - tgt.y);
      sum += d * d;
      n++;
      return d;
    }
    return null;
  });
  return n ? Math.sqrt(sum / n) : null;
};

// --- the pair list ---
// rebuilt only when the pair set changes: at frame rate it would yank the
// delete buttons out from under a click
let rowEls = [];
function renderList() {
  listEl.textContent = '';
  rowEls = pairs.map((pr, i) => {
    const row = document.createElement('div');
    row.className = 'pair';
    const n = document.createElement('span');
    n.className = 'n';
    n.textContent = i + 1;
    const uv = document.createElement('span');
    uv.textContent = pr.u.toFixed(2) + ',' + pr.v.toFixed(2);
    const err = document.createElement('span');
    err.className = 'err';
    const del = document.createElement('button');
    del.className = 'x';
    del.textContent = '✕';
    del.title = 'delete this pair';
    del.addEventListener('click', (e) => {
      e.stopPropagation();
      pairs.splice(i, 1);
      if (selPair >= pairs.length) selPair = -1;
      hovPair = -1; // the row under the pointer is a different pair now
      buildMarkers();
      renderList();
      saveAlign();
    });
    row.append(n, uv, err, del);
    row.addEventListener('click', () => setSel(selPair === i ? -1 : i));
    row.addEventListener('pointerenter', () => setHover(i));
    row.addEventListener('pointerleave', () => setHover(-1));
    listEl.appendChild(row);
    return { row, err };
  });
  applyPairStates();
  updateStatus(null);
}
// the live readout: per-pair pixel error and the overall rms
function updateStatus(rms) {
  rowEls.forEach((r, i) => {
    const e = lastErrs[i];
    r.err.textContent = e == null ? 'off-screen' : Math.round(e) + 'px';
    r.row.classList.toggle('off', e == null);
  });
  const k = dofCount();
  const need = Math.ceil(k / 2);
  const bits = [`${pairs.length} pair${pairs.length === 1 ? '' : 's'} · ${k} unknown${k === 1 ? '' : 's'}`];
  if (rms != null) bits.push(`rms ${rms.toFixed(1)}px`);
  statusEl.textContent = bits.join(' · ');
  warnEl.textContent = !k ? 'nothing to solve for — tick something above'
    : (pairs.length && pairs.length < need)
      ? `under-determined: ${need} pairs would pin all ${k} down` : '';
}
const dofCount = () => (freeFlags.pos ? 3 : 0) + (freeFlags.aim ? 2 : 0)
  + (freeFlags.fov ? 1 : 0) + (freeFlags.roll ? 1 : 0) + (freeFlags.dist ? 1 : 0);

// --- capturing a new pair ---
// two clicks: the point in the photo, then the same feature on the terrain
let capture = null;
let blendBeforeCapture = null;
const setBanner = (t) => { bannerEl.textContent = t; };
function startCapture() {
  if (!alignOn) return;
  capture = { stage: 'photo' };
  document.body.classList.add('capturing');
  blendBeforeCapture = +overlayBlend.value;
  overlayBlend.value = 100;
  applyOverlayBlend();
  setBanner('click the point in the photo   (esc to cancel)');
  addBtn.textContent = 'cancel';
  canvas.focus();
}
function cancelCapture() {
  if (!capture) return;
  capture = null;
  document.body.classList.remove('capturing');
  if (blendBeforeCapture != null) {
    overlayBlend.value = blendBeforeCapture;
    applyOverlayBlend();
    blendBeforeCapture = null;
  }
  addBtn.textContent = '+ add pair';
}
const captureClick = (x, y, ndcX, ndcY) => {
  if (capture.stage === 'photo') {
    // clicked on the photo as displayed; stored and bounds-checked as shot
    const shown = viewToPhoto(x, y);
    const p = shown && distortUV(shown.u, shown.v, ov.k);
    if (!p || p.u < 0 || p.u > 1 || p.v < 0 || p.v > 1) {
      setBanner('that is outside the photo — click inside it');
      return;
    }
    capture = { stage: 'mesh', u: p.u, v: p.v };
    overlayBlend.value = 0;
    applyOverlayBlend();
    setBanner('now click the same feature on the terrain   (esc to cancel)');
    return;
  }
  const hit = view.pickNdc(ndcX, ndcY);
  if (!hit) {
    setBanner('no terrain there — it may still be loading. try again');
    return;
  }
  pairs.push({ u: capture.u, v: capture.v, lat: hit.lat, lon: hit.lon, alt: hit.alt });
  selPair = pairs.length - 1;
  cancelCapture();
  buildMarkers();
  renderList();
  saveAlign();
};

// --- the solver -----------------------------------------------------------
// lmStep (math.js) is the method; the residuals and the camera moves are here.
// residuals: mesh minus photo point, px. null if any pair is behind the
// camera, so the step is rejected rather than fitted to a wrapped projection
const residuals = (p, W, H) => {
  const b = poseBasis(p);
  if (!b) return null;
  const out = [];
  for (const pr of pairs) {
    const s = projectWith(b, p.fov, ecefOf(pr), W, H);
    const t = photoToViewK(pr.u, pr.v, p.k == null ? ov.k : p.k);
    if (!s || !t) return null;
    out.push(s.x - t.x, s.y - t.y);
  }
  return out.length ? out : null;
};
let solving = false;
const lm = { lambda: 1e-3, iters: 0, stalls: 0 }; // the solver's damping and progress
// one accepted step (or give-up) per call, a few per frame; true when done
const solveStep = () => {
  if (!view.ready() || !pairs.length) return true;
  const specs = paramSpecs(freeFlags);
  if (!specs.length) return true;
  const W = rightEl.clientWidth, H = rightEl.clientHeight;
  const base = view.getPose();
  base.k = ov.k || 0; // the lens is the photo's, not the camera's
  const { cand, done } = lmStep(base, specs, (p) => residuals(p, W, H), clampLens, lm);
  if (cand) {
    view.setPose(cand.lat, cand.lon, cand.alt, cand.heading, cand.tilt);
    view.setFov(cand.fov);
    // setPose keeps the old roll
    if (freeFlags.roll) view.setRoll(cand.roll);
    if (freeFlags.dist) setLens(cand.k); // redraws the warp: only on an accepted step
    S.following = true; // the map should come along
  }
  return done;
};
function stopSolve() {
  solving = false;
  solveBtn.textContent = 'solve';
}
const startSolve = () => {
  if (solving) { stopSolve(); return; }
  if (!pairs.length) return;
  // hug re-eases altitude in every fly() call, even the tick's idle one, undoing
  // each step. drop it for good: restoring it would pull the camera off the fit
  if (setHug(false)) updateHint();
  solving = true;
  lm.lambda = 1e-3;
  lm.iters = 0;
  lm.stalls = 0;
  solveBtn.textContent = 'stop';
  canvas.focus();
};

const pairsPlural = () => `${pairs.length} point pair${pairs.length === 1 ? '' : 's'}`;
const initAlign = () => {
  // --- wiring ---
  addBtn.addEventListener('click', () => {
    if (capture) cancelCapture(); else startCapture();
  });
  solveBtn.addEventListener('click', startSolve);
  // both discard hand-placed work, and the ✕ sits where a close button would
  document.getElementById('apclear').addEventListener('click', () => {
    if (pairs.length && !confirm(`clear ${pairsPlural()}?`)) return;
    clearPairs();
  });
  document.getElementById('apforget').addEventListener('click', () => {
    const what = pairs.length ? `the photo and its ${pairsPlural()}` : 'the photo and its placement';
    if (!confirm(`remove ${what}?\n\n(to just hide the photo, press P or click "opacity")`)) return;
    clearOverlay();
  });
  for (const [id, key] of [['apfreepos', 'pos'], ['apfreeaim', 'aim'],
                          ['apfreefov', 'fov'], ['apfreeroll', 'roll'],
                          ['apfreedist', 'dist']]) {
    document.getElementById(id).addEventListener('change', (e) => {
      freeFlags[key] = e.target.checked;
      renderList();
      saveAlign();
    });
  }
  document.getElementById('apfit').addEventListener('click', () => {
    fitOverlay();
    saveAlign();
    canvas.focus();
  });
  overlayBlend.addEventListener('change', saveAlign);
  // esc anywhere cancels a capture (the canvas's esc clears the tube; never both)
  addEventListener('keydown', (e) => {
    if (e.code === 'Escape' && capture) cancelCapture();
  });

  // --- restore --------------------------------------------------------------
  (async () => {
    const saved = loadStore('earth-align');
    if (saved) {
      if (saved.ov) Object.assign(ov, saved.ov);
      if (!Number.isFinite(ov.k)) ov.k = 0;
      if (saved.free) Object.assign(freeFlags, saved.free);
      if (Number.isFinite(saved.opacity)) {
        overlayBlend.value = saved.opacity;
        applyOverlayBlend();
      }
      S.refImageKey = saved.key || null;
      if (Array.isArray(saved.pairs))
        pairs = saved.pairs.filter((p) => p && Number.isFinite(p.u) && Number.isFinite(p.v)
          && Number.isFinite(p.lat) && Number.isFinite(p.lon) && Number.isFinite(p.alt));
    }
    const blob = await idbGet('ref-image', 'app');
    if (blob) setOverlay(blob, true); // keepPlacement: the saved ov is the truth
    else { pairs = []; S.refImageKey = null; } // orphaned without their photo
    alignRefresh();
  })();
};

const initPhoto = () => {
  initPhotoOverlay();
  initAlign();
};
