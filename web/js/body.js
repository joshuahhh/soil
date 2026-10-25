import { parseObj, bakeBody } from './bodybake.js';
import { view } from './engine.js';

export { bodyPromise, setBodyMesh, ensureBody };

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
