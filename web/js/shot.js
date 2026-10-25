import { S } from './state.js';
import { canvas, rightEl } from './engine.js';
import { overlayImg, overlayBox, warpEl } from './photo.js';
import { toast } from './hud.js';
import { svgEl } from './align.js';
import { fileStamp, saveFile } from './session.js';

export { initShot };

// --- screenshot ------------------------------------------------------------
// the 3d view as it is on screen, at the canvas's own pixel size: the render,
// then whatever is drawn over it and showing — the photo (plain, or through
// the lens), the pair markers, the reticle — at its place and opacity.
// panels and readouts are left out. the gl canvas is copied inside the frame
// that drew it; the rest is composited afterwards
const shotLayers = () => [overlayImg, warpEl, svgEl, document.getElementById('cross')];
// an svg from the page as a bitmap. its look comes from the stylesheet,
// which an <img> of it can't see, so the copy carries each element's
// computed style inline
const SVG_PROPS = ['display', 'visibility', 'opacity', 'fill', 'stroke', 'stroke-width',
  'stroke-dasharray', 'stroke-linecap', 'font-size', 'font-weight', 'font-family'];
const rasterSvg = (el, w, h) => new Promise((resolve, reject) => {
  const copy = el.cloneNode(true);
  const src = el.querySelectorAll('*'), dst = copy.querySelectorAll('*');
  for (let i = 0; i < src.length; i++) {
    const cs = getComputedStyle(src[i]);
    for (const p of SVG_PROPS) dst[i].style.setProperty(p, cs.getPropertyValue(p));
  }
  copy.setAttribute('xmlns', 'http://www.w3.org/2000/svg');
  copy.setAttribute('width', w);
  copy.setAttribute('height', h);
  const img = new Image();
  img.onload = () => resolve(img);
  img.onerror = reject;
  img.src = 'data:image/svg+xml;charset=utf-8,'
    + encodeURIComponent(new XMLSerializer().serializeToString(copy));
});
const takeShot = () => {
  S.pendingShot = async () => {
    const out = document.createElement('canvas');
    out.width = canvas.width;
    out.height = canvas.height;
    const ctx = out.getContext('2d');
    // jpeg has no alpha: what's clear on the canvas shows the page behind
    ctx.fillStyle = '#fff';
    ctx.fillRect(0, 0, out.width, out.height);
    ctx.drawImage(canvas, 0, 0);
    const base = rightEl.getBoundingClientRect();
    const s = canvas.width / canvas.clientWidth; // css px -> canvas px
    for (const el of shotLayers()) {
      const cs = getComputedStyle(el);
      if (cs.display === 'none' || cs.visibility === 'hidden' || !+cs.opacity) continue;
      if (el instanceof HTMLImageElement && !el.naturalWidth) continue;
      const r = el.getBoundingClientRect();
      let src = el;
      if (el instanceof SVGSVGElement) {
        try { src = await rasterSvg(el, r.width, r.height); } catch (e) {
          console.warn('screenshot: svg layer skipped', el.id, e);
          continue;
        }
      }
      ctx.globalAlpha = +cs.opacity;
      ctx.drawImage(src, (r.left - base.left) * s, (r.top - base.top) * s, r.width * s, r.height * s);
    }
    ctx.globalAlpha = 1;
    out.toBlob((blob) => {
      saveFile(blob, `soil-shot-${fileStamp()}.jpg`);
      toast(`screenshot saved (${out.width}×${out.height})`);
    }, 'image/jpeg', 0.92);
  };
};

// --- photo ⇄ render gif ---------------------------------------------------
// a loop that holds on the photo, fades to the render it was aligned
// against, holds, and fades back. cropped to the photo's box (as much of it
// as is on screen), so it is the picture and the landscape behind it and
// nothing else: no pair markers, no reticle, and the photo at full opacity
// whatever the slider says. the encoder is fetched the first time it's used
const GIF_MAX = 800;   // the longer side, px
const GIF_STEPS = 12;  // frames per fade
const GIF_HOLD = 1200, GIF_STEP_MS = 60;
let gifenc = null;
const takeGif = () => {
  const b = overlayBox();
  if (!b) return;
  const btn = document.getElementById('apgif');
  btn.disabled = true;
  S.pendingShot = async () => {
    try {
      const W = rightEl.clientWidth, H = rightEl.clientHeight;
      const s = canvas.width / canvas.clientWidth; // css px -> canvas px
      const x0 = Math.max(0, b.x), y0 = Math.max(0, b.y);
      const x1 = Math.min(W, b.x + b.w), y1 = Math.min(H, b.y + b.h);
      if (x1 - x0 < 1 || y1 - y0 < 1) { toast('the photo is out of view'); return; }
      const f = Math.min(s, GIF_MAX / Math.max(x1 - x0, y1 - y0)); // css px -> gif px
      const w = Math.round((x1 - x0) * f), h = Math.round((y1 - y0) * f);
      const layer = () => {
        const c = document.createElement('canvas');
        c.width = w; c.height = h;
        const ctx = c.getContext('2d', { willReadFrequently: true });
        ctx.imageSmoothingQuality = 'high';
        return ctx;
      };
      // the render first, in the frame that drew it (see takeShot)
      const render = layer();
      render.fillStyle = '#fff';
      render.fillRect(0, 0, w, h);
      render.drawImage(canvas, x0 * s, y0 * s, (x1 - x0) * s, (y1 - y0) * s, 0, 0, w, h);
      // the photo as displayed — through the lens if it has one — over the
      // render, which fills in wherever the warp leaves the box uncovered
      const photo = layer();
      photo.drawImage(render.canvas, 0, 0);
      const el = document.body.classList.contains('distorted') ? warpEl : overlayImg;
      const r = el.getBoundingClientRect(), base = rightEl.getBoundingClientRect();
      photo.drawImage(el, (r.left - base.left - x0) * f, (r.top - base.top - y0) * f,
                      r.width * f, r.height * f);
      toast('making gif…');
      gifenc ??= await import('https://cdn.jsdelivr.net/npm/gifenc@1.0.3/dist/gifenc.esm.js');
      const { GIFEncoder, quantize, applyPalette } = gifenc;
      // one quantized frame per step of the fade, eased; the way back
      // reuses them in reverse
      const mix = layer();
      const frames = [];
      for (let i = 0; i <= GIF_STEPS; i++) {
        const t = i / GIF_STEPS, e = t * t * (3 - 2 * t);
        mix.globalAlpha = 1;
        mix.drawImage(render.canvas, 0, 0);
        mix.globalAlpha = 1 - e;
        mix.drawImage(photo.canvas, 0, 0);
        const px = mix.getImageData(0, 0, w, h).data;
        const palette = quantize(px, 256);
        frames.push({ index: applyPalette(px, palette), palette });
        await new Promise((res) => setTimeout(res)); // let the page breathe
      }
      const gif = GIFEncoder();
      const order = [...frames.keys(), ...[...frames.keys()].slice(1, -1).reverse()];
      for (const i of order) {
        const { index, palette } = frames[i];
        const delay = i === 0 || i === GIF_STEPS ? GIF_HOLD : GIF_STEP_MS;
        gif.writeFrame(index, w, h, { palette, delay });
      }
      gif.finish();
      const blob = new Blob([gif.bytes()], { type: 'image/gif' });
      saveFile(blob, `soil-photo-${fileStamp()}.gif`);
      toast(`gif saved (${w}×${h}, ${(blob.size / 1e6).toFixed(1)} MB)`);
    } catch (e) {
      console.error('gif failed', e);
      toast('gif failed: ' + (e.message || e));
    } finally {
      btn.disabled = false;
    }
  };
};
const initShot = () => {
  document.getElementById('shotbtn').addEventListener('click', () => {
    takeShot();
    canvas.focus();
  });

  document.getElementById('apgif').addEventListener('click', () => {
    takeGif();
    canvas.focus();
  });
};
