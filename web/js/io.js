import { S, STORES, FILES, freezeStorage } from './config.js';
import { idbPut, idbGet, idbPutChecked, idbDel } from './tiles.js';
import { canvas, rightEl } from './engine.js';
import { saveView } from './view.js';
import { toast } from './hud.js';
import { overlayBlend, applyOverlayBlend, setOverlay, saveAlign, clearPairs, overlayImg,
  overlayBox, warpEl, svgEl } from './photo.js';
import { setBodyMesh } from './tube.js';
import { loadGpxFile, savePath } from './path.js';

export { initIO, copyState, saveState, pasteState };

// --- dropped files --------------------------------------------------------
// dragover has to be cancelled or the browser navigates to the dropped file.
// a depth counter keeps the highlight steady as the drag crosses children
// (dragenter on a child fires before dragleave on its parent)
let dragDepth = 0;
// a drag only exposes item types, not names, and a .gpx usually arrives
// with no type at all — so anything typeless is let through and sorted out
// on the drop, where the filename is finally visible
const isFileDrag = (e) => [...(e.dataTransfer ? e.dataTransfer.items : [])]
  .some((it) => it.kind === 'file'
    && (!it.type || it.type.startsWith('image/') || it.type.includes('xml')
      || it.type.includes('json')));
const initDrop = () => {
  addEventListener('dragenter', (e) => {
    if (!isFileDrag(e)) return;
    e.preventDefault();
    if (++dragDepth === 1) document.body.classList.add('dropping');
  });
  addEventListener('dragover', (e) => { if (isFileDrag(e)) e.preventDefault(); });
  addEventListener('dragleave', () => {
    if (dragDepth && --dragDepth === 0) document.body.classList.remove('dropping');
  });
  addEventListener('drop', (e) => {
    dragDepth = 0;
    document.body.classList.remove('dropping');
    const dropped = [...(e.dataTransfer ? e.dataTransfer.files : [])];
    // a track goes to the path overlay, everything else is a reference photo
    const gpx = dropped.find((f) => /\.gpx$/i.test(f.name));
    if (gpx) {
      e.preventDefault();
      loadGpxFile(gpx);
      return;
    }
    // a mesh replaces the figure the body mode wraps the rect onto
    const obj = dropped.find((f) => /\.obj$/i.test(f.name));
    if (obj) {
      e.preventDefault();
      obj.text().then((text) => {
        if (setBodyMesh(text)) toast(`body: ${obj.name}`);
        else toast('that .obj has no faces');
      });
      return;
    }
    // a session file (the thing ⌘S writes) is the drop-shaped twin of ⌘V
    const json = dropped.find((f) => /\.json$/i.test(f.name) || f.type.includes('json'));
    if (json) {
      e.preventDefault();
      json.text().then((text) => {
        if (!pasteState(text)) toast('that json is not a session file');
      });
      return;
    }
    const file = dropped.find((f) => f.type.startsWith('image/'));
    if (!file) return;
    e.preventDefault();
    // pairs are anchored to points of a particular photo, so a different
    // photo drops them — but re-dropping the same file (same name and size)
    // is a reload, not a new subject, and keeps them
    const key = file.name + ':' + file.size;
    const same = key === S.refImageKey;
    S.refImageKey = key;
    if (!same) {
      clearPairs();
      // a new subject starts fully visible: you look at the photo first and
      // fade it once you're lining the view up
      overlayBlend.value = 100;
      applyOverlayBlend();
    }
    setOverlay(file, same);
    idbPut('ref-image', file, 'app');
    saveAlign();
  });
};

// --- move a session between origins ---------------------------------------
// the deployed page and localhost are two origins, and nothing is shared
// across them: separate localstorages, separate indexeddbs. so the
// clipboard carries it. cmd-C anywhere on the page bundles the whole
// session — view, path settings and the two dropped files — into one json
// blob; cmd-V on the other build unpacks it and reloads into exactly that
// session. cmd-S writes the same blob to a .json, and dropping that file on
// the page reads it back. the tile cache is deliberately left out: it is megabytes of
// bytes that re-download themselves, and the only cost of leaving it
// behind is that the first minute over the same ground is slower
const STATE_TAG = 'soil-session-v1';
// files go over as data urls: json is the only thing the clipboard carries
// between two pages, and this is the one encoding of a blob that comes back
// through fetch() without a byte of parsing here
const fileToJson = (f) => new Promise((res) => {
  const r = new FileReader();
  r.onload = () => res({ name: f.name || '', type: f.type || '', data: r.result });
  r.onerror = () => res(null);
  r.readAsDataURL(f);
});
const jsonToFile = async (j, fallbackName) => {
  const blob = await (await fetch(j.data)).blob();
  return new File([blob], j.name || fallbackName, { type: j.type || blob.type });
};
const collectState = async () => {
  // the three savers write on their own schedule (beforeunload, on change),
  // so run them first or the copy carries the state as of some earlier moment
  saveView();
  savePath();
  saveAlign();
  // localstorage holds strings, but the file is for reading and diffing
  // too, so each entry goes in as the object it is — not a json string
  // escaped inside the json. (a value that isn't json stays a string)
  const ls = {};
  for (const k of STORES) {
    const v = localStorage.getItem(k);
    if (v == null) continue;
    try { ls[k] = JSON.parse(v); } catch (e) { ls[k] = v; }
  }
  const files = {};
  for (const k of FILES) {
    const f = await idbGet(k, 'app');
    if (!f) continue;
    const j = await fileToJson(f);
    if (j) files[k] = j;
    else console.warn(`session: could not read '${k}' out of idb — copying without it`);
  }
  return JSON.stringify({ tag: STATE_TAG, ls, files }, null, 2);
};
const copyState = async () => {
  const text = await collectState();
  const mb = (text.length / 1e6).toFixed(1);
  try {
    await navigator.clipboard.writeText(text);
    toast(`session copied (${mb} MB) — ⌘V it into the other build`);
  } catch (e) {
    // writeText wants a focused document and the clipboard permission; when
    // it says no there is still a perfectly good string to hand over
    console.warn('session: clipboard write failed', e);
    console.log(text);
    toast('could not reach the clipboard — the session json is in the console');
  }
  return text;
};
// ⌘S writes the same bundle to a file — for keeping a session around, or
// moving it somewhere a clipboard doesn't reach. dropping it back on the
// page is the other half
const fileStamp = () => new Date().toISOString().slice(0, 19).replace(/[T:]/g, '-');
const saveFile = (blob, name) => {
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
};
const saveState = async () => {
  const text = await collectState();
  saveFile(new Blob([text], { type: 'application/json' }), `soil-session-${fileStamp()}.json`);
  toast(`session saved (${(text.length / 1e6).toFixed(1)} MB) — drop the file on the page to restore it`);
};
// applying is destructive on purpose: a key the incoming session doesn't
// have is a key it doesn't want, so it goes. half-pasting one session over
// another leaves a mix that belongs to neither
const applyState = async (s) => {
  freezeStorage(); // before the first write: nothing may save over it now
  for (const k of STORES) {
    // objects from the current format, strings from files written before
    // it stored them parsed
    if (s.ls && s.ls[k] != null)
      localStorage.setItem(k, typeof s.ls[k] === 'string' ? s.ls[k] : JSON.stringify(s.ls[k]));
    else localStorage.removeItem(k);
  }
  const failed = [];
  for (const k of FILES) {
    const j = s.files && s.files[k];
    if (!j) { idbDel(k, 'app'); continue; }
    try {
      if (!await idbPutChecked(k, await jsonToFile(j, k))) failed.push(k);
    } catch (e) {
      console.warn(`session: could not rebuild '${k}'`, e);
      failed.push(k);
    }
  }
  return failed;
};
const pasteState = async (text) => {
  let s = null;
  try { s = JSON.parse(text); } catch (e) {}
  if (!s || s.tag !== STATE_TAG) return false;
  const failed = await applyState(s);
  if (failed.length) {
    // the localstorage half landed and the files didn't, so reloading now
    // would show a session missing its photo or track and say nothing. the
    // savers stay off for the rest of this page's life either way: what is
    // in storage is the pasted session, and flying around now shouldn't
    // quietly edit it
    toast(`pasted, but ${failed.join(' and ')} did not fit in storage — see the console`);
    return true;
  }
  toast('session pasted — reloading');
  setTimeout(() => location.reload(), 250);
  return true;
};
const initSession = () => {
  addEventListener('keydown', (e) => {
    if (e.code !== 'KeyS' || !(e.metaKey || e.ctrlKey) || e.shiftKey || e.altKey) return;
    e.preventDefault(); // or the browser offers to save the html
    saveState();
  });

  addEventListener('copy', (e) => {
    // a real selection, or a cursor in a field, means the copy is about text
    // on the page and not about the session
    const t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
    const sel = getSelection();
    if (sel && !sel.isCollapsed && String(sel).trim()) return;
    e.preventDefault();
    copyState();
  });
  addEventListener('paste', (e) => {
    const t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) return;
    const text = e.clipboardData && e.clipboardData.getData('text/plain');
    // anything that isn't one of our bundles is left to whatever else on the
    // page might want it
    if (!text || text.indexOf(STATE_TAG) < 0) return;
    e.preventDefault();
    pasteState(text);
  });
};

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

const initIO = () => {
  initDrop();
  initSession();
  initShot();
};
