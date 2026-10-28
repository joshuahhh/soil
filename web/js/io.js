import { S, STORES, FILES, freezeStorage } from './config.js';
import { idbPut, idbGet, idbPutChecked, idbDel } from './tiles.js';
import { canvas, rightEl } from './engine.js';
import { saveView } from './view.js';
import { toast } from './hud.js';
import { overlayBlend, applyOverlayBlend, setOverlay, saveAlign, clearPairs, overlayImg,
  overlayBox, warpEl, svgEl } from './photo.js';
import { setBodyMesh } from './tube.js';
import { loadGpxFile, savePath } from './path.js';

export { initIO, copyState, saveState, pasteState, saveFile };

// --- dropped files --------------------------------------------------------
// counted: dragenter on a child fires before dragleave on its parent
let dragDepth = 0;
// a drag shows types, not names, and a .gpx usually has no type: let typeless through
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
    const gpx = dropped.find((f) => /\.gpx$/i.test(f.name));
    if (gpx) {
      e.preventDefault();
      loadGpxFile(gpx);
      return;
    }
    const obj = dropped.find((f) => /\.obj$/i.test(f.name));
    if (obj) {
      e.preventDefault();
      obj.text().then((text) => {
        if (setBodyMesh(text)) toast(`body: ${obj.name}`);
        else toast('that .obj has no faces');
      });
      return;
    }
    // a session file, as ⌘S writes it
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
    // the same name and size is a reload of the same photo: it keeps its pairs
    const key = file.name + ':' + file.size;
    const same = key === S.refImageKey;
    S.refImageKey = key;
    if (!same) {
      clearPairs();
      overlayBlend.value = 100;
      applyOverlayBlend();
    }
    setOverlay(file, same);
    idbPut('ref-image', file, 'app');
    saveAlign();
  });
};

// --- move a session between origins ---------------------------------------
// ⌘C / ⌘V, ⌘S / drop: the whole session as one json blob. the tile cache is
// left out: megabytes that re-download themselves
const STATE_TAG = 'soil-session-v1';
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
  // the savers run on their own schedule; flush them first
  saveView();
  savePath();
  saveAlign();
  // parsed, so the file reads and diffs as json, not escaped strings
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
    console.warn('session: clipboard write failed', e);
    console.log(text);
    toast('could not reach the clipboard — the session json is in the console');
  }
  return text;
};
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
// destructive on purpose: a key the session lacks is removed, not left to mix in
const applyState = async (s) => {
  freezeStorage(); // before the first write: nothing may save over it now
  for (const k of STORES) {
    // strings come from files written before entries were stored parsed
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
    // no reload: it would show the session without its files and say nothing.
    // the savers stay frozen either way
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
    e.preventDefault();
    saveState();
  });

  addEventListener('copy', (e) => {
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
    if (!text || text.indexOf(STATE_TAG) < 0) return;
    e.preventDefault();
    pasteState(text);
  });
};

// --- screenshot ------------------------------------------------------------
// runs as S.pendingShot, inside the frame that drew the gl canvas (the only
// time it can be read); the overlays are composited on top
const shotLayers = () => [overlayImg, warpEl, svgEl, document.getElementById('cross')];
// an <img> of an svg can't see the stylesheet: inline each element's computed style
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
// cropped to the photo's visible box; no markers, photo at full opacity
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
      // the photo as displayed, over the render where the warp leaves gaps
      const photo = layer();
      photo.drawImage(render.canvas, 0, 0);
      const el = document.body.classList.contains('distorted') ? warpEl : overlayImg;
      const r = el.getBoundingClientRect(), base = rightEl.getBoundingClientRect();
      photo.drawImage(el, (r.left - base.left - x0) * f, (r.top - base.top - y0) * f,
                      r.width * f, r.height * f);
      toast('making gif…');
      gifenc ??= await import('https://cdn.jsdelivr.net/npm/gifenc@1.0.3/dist/gifenc.esm.js');
      const { GIFEncoder, quantize, applyPalette } = gifenc;
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
