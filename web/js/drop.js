import { S } from './state.js';
import { idbPut } from './idb.js';
import { overlayBlend, applyOverlayBlend, setOverlay } from './photo.js';
import { toast } from './hud.js';
import { setBodyMesh } from './body.js';
import { loadGpxFile } from './path.js';
import { saveAlign, clearPairs } from './align.js';
import { pasteState } from './session.js';

export { initDrop };

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
