import { idbGet, idbPutChecked, idbDel } from './idb.js';
import { saveView } from './saveview.js';
import { toast } from './hud.js';
import { savePath } from './path.js';
import { saveAlign } from './align.js';

export { storageFrozen, copyState, fileStamp, saveFile, saveState, pasteState, initSession };

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
const STATE_KEYS = ['earth-view', 'earth-path', 'earth-align'];
const STATE_FILES = ['gpx-file', 'ref-image'];
// set once a pasted session has been written to storage, and never cleared.
// from that moment what's in storage describes a session this page is not
// showing, and every saver below would be writing the one it's replacing
// over it. the view saver is the one that actually bites: it runs both on
// beforeunload — which the reload after a paste fires itself — and once a
// second off the frame loop, so leaving it live loses the pasted pose
// always, or intermittently, depending on which one gets there first
let storageFrozen = false;
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
  for (const k of STATE_KEYS) {
    const v = localStorage.getItem(k);
    if (v == null) continue;
    try { ls[k] = JSON.parse(v); } catch (e) { ls[k] = v; }
  }
  const files = {};
  for (const k of STATE_FILES) {
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
  storageFrozen = true; // before the first write: nothing may save over it now
  for (const k of STATE_KEYS) {
    // objects from the current format, strings from files written before
    // it stored them parsed
    if (s.ls && s.ls[k] != null)
      localStorage.setItem(k, typeof s.ls[k] === 'string' ? s.ls[k] : JSON.stringify(s.ls[k]));
    else localStorage.removeItem(k);
  }
  const failed = [];
  for (const k of STATE_FILES) {
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
