import { DEFAULT_VIEW } from './config.js';
import { S } from './state.js';
import { view, canvas, rightEl } from './engine.js';
import { saveView } from './saveview.js';
import { map, pickDebug, setPickDebug } from './map.js';
import { ov, clampScale, overlayBox, layoutOverlay, viewToPhoto,
  toggleOverlayBlend } from './photo.js';
import { updateHint, toast } from './hud.js';
import { toggleKeys } from './keytable.js';
import { toggleCutPanel } from './nearcut.js';
import { tunnelOn, tunnelClosing, setTunnel } from './tunnel.js';
import { setRectData, unrollTube, applyTube } from './tube.js';
import { followOn, debugSmooth, setDebugSmooth, setFollow, toggleFollow } from './path.js';
import { saveAlign, capture, captureClick, stopSolve } from './align.js';

export { keys, miniature, setMiniature, debugLod, setDebugLod, mipmaps, setMipmaps, dragging,
  initInput };

// --- keyboard -> built-in flying controls -----------------------------------
// ground frame: WASD cruise/truck, Q/E pedestal. hold Space for the view
// frame: W/S dolly along the view, Q/E boom along view-up. arrows pan/tilt
// so no mouse is needed. Z/X zoom (fov). -/= sticky speed gain. shift slow.
// the keys are the page's, not the canvas's: nudging a slider or clicking
// a button in a panel used to take them away until you clicked the view
// again. only a field you can type into keeps them for itself
const keys = {};
const typingIn = (t) => !!t && (t.tagName === 'TEXTAREA' || t.isContentEditable
  || t.tagName === 'SELECT'
  || (t.tagName === 'INPUT' && !['range', 'checkbox', 'color', 'button', 'radio'].includes(t.type)));
const handled = new Set(['KeyW','KeyA','KeyS','KeyD','KeyQ','KeyE','KeyR','KeyF','KeyZ','KeyX','KeyG','KeyO','KeyC','KeyK','KeyL','KeyM','KeyT','KeyP','KeyV','KeyN','KeyB','KeyY','KeyI',
  'Space','ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Minus','Equal','Digit0','Numpad0','Home','KeyH']);
let miniature = false; // the tilt-shift look (N)
const setMiniature = (on) => { miniature = on; view.setMiniature(on, 0.015, 0.35); updateHint(); };
let debugLod = false;
// the overlay and its legend panel go together
const setDebugLod = (on) => {
  debugLod = on;
  view.setDebugLod(on);
  document.body.classList.toggle('lod', on);
  saveView(); // part of the view state: a reload comes back with it
};
let mipmaps = true;
const setMipmaps = (on) => { mipmaps = on; view.setMipmaps(on); updateHint(); };
// 0 (or Home): back to the default view — the pose/fov/speed the demo
// opens with, level (no roll), out of ortho and terrain hug, map zoom
// reset and following again. the tube is left alone: esc clears it
const resetView = () => {
  if (!view.ready()) return;
  if (followOn) setFollow(false); // else the rails put the camera straight back
  view.setPose(DEFAULT_VIEW.lat, DEFAULT_VIEW.lon, DEFAULT_VIEW.alt,
    DEFAULT_VIEW.heading, DEFAULT_VIEW.tilt);
  view.setFov(DEFAULT_VIEW.fov);
  view.setRoll(0);
  S.gain = DEFAULT_VIEW.gain;
  if (S.ortho) { S.ortho = false; view.setOrtho(false); }
  if (miniature) setMiniature(false);
  if (S.hug) { S.hug = false; view.setTerrainFollow(false); }
  map.setZoom(DEFAULT_VIEW.zoom);
  S.following = true;
  S.followVel = S.prevMark = null; // a jump, not motion: don't lead the ease
  updateHint();
};
let dragging = false;
let ovDragging = false; // shift+drag slides the reference photo instead
const initInput = () => {
  document.getElementById('lodoff').addEventListener('click', () => { setDebugLod(false); canvas.focus(); });

  addEventListener('keydown', (e) => {
    // browser/os shortcuts (cmd-R reload etc.) pass through untouched
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    if (typingIn(e.target)) return;
    if (handled.has(e.code)) e.preventDefault();
    keys[e.code] = true;
    if (e.code === 'Minus' && !e.repeat) { S.gain = Math.max(1/128, S.gain/2); updateHint(); }
    if (e.code === 'Equal' && !e.repeat) { S.gain = Math.min(128, S.gain*2); updateHint(); }
    // no ortho inside the tube: its extent/zoom anchoring assumes normal
    // terrain below, and a parallel projection of the tube reads as mush
    if (e.code === 'KeyN' && !e.repeat) setMiniature(!miniature);
    if (e.code === 'KeyO' && !e.repeat && !S.tubeOn && !tunnelOn) { S.ortho = !S.ortho; view.setOrtho(S.ortho); updateHint(); }
    // terrain hug: cruise at a fixed height above the mesh — over the
    // sphere normally, radially above the wall inside the tube
    if (e.code === 'KeyG' && !e.repeat && !followOn && !(S.tubeOn && S.tubeBody)) {
      S.hug = !S.hug;
      view.setTerrainFollow(S.hug);
      if (S.hug) stopSolve(); // the two fight over altitude — see startSolve
      updateHint();
    }
    if (e.code === 'KeyC' && !e.repeat) setPickDebug(!pickDebug);
    // ride the dropped track; does nothing without one
    if (e.code === 'KeyV' && !e.repeat) toggleFollow();
    // the point-pair panel; only does anything with a photo up, in perspective
    if (e.code === 'KeyP' && !e.repeat) toggleOverlayBlend();
    if (e.code === 'KeyH' && !e.repeat) toggleCutPanel();
    if (e.key === '?' && !e.repeat) toggleKeys();
    if (e.code === 'Escape' && document.body.classList.contains('keys')) {
      document.body.classList.remove('keys');
      return; // that is all this esc does
    }
    if ((e.code === 'Digit0' || e.code === 'Numpad0' || e.code === 'Home') && !e.repeat) resetView();
    // like the native app's L key: tint octants red where a finer tile is
    // wanted but not yet downloaded
    if (e.code === 'KeyL' && !e.repeat) setDebugLod(!debugLod);
    if (e.code === 'KeyY' && !e.repeat) document.body.classList.toggle('skypanel');
    if (e.code === 'KeyI' && !e.repeat) document.body.classList.toggle('glamourpanel');
    // K, its neighbour: draw the smoothed copy of the track the follow camera
    // actually aims down, dashed on the map beside the track it was made from
    if (e.code === 'KeyK' && !e.repeat) setDebugSmooth(!debugSmooth);
    // mipmap A/B. only bites on the uncompressed texture path, so it's the
    // ?jpg build where you can see it — and it's motion that shows it, not a
    // still frame: watch distant ground crawl as you fly
    if (e.code === 'KeyM' && !e.repeat) setMipmaps(!mipmaps);
    // roll/unroll the tube for the current rect (drawn by shift+drag on the
    // map). rolling up also flies into the tube mouth — watching the roll
    // from outside mostly shows the back of the sheet
    // clear the tube entirely: unroll, drop the rect, back to normal flight
    if (e.code === 'Escape' && followOn) setFollow(false);
    if (e.code === 'Escape' && tunnelOn) { setTunnel(false); return; }
    // T is the tunnel — unless a rect is drawn on the map, when it rolls
    // that instead (esc clears the rect)
    if (e.code === 'KeyT' && !e.repeat && !followOn && (tunnelOn || !S.tubeRect)) {
      setTunnel(!tunnelOn || tunnelClosing);
      return;
    }
    if (e.code === 'Escape' && S.tubeRect) {
      S.tubeRect = null;
      S.tubeBody = false;
      unrollTube();
      setRectData(null);
      updateHint();
    }
    // T rolls the rect into a tube, B wraps it onto the body; each key
    // undoes its own shape and switches from the other's
    if (e.code === 'KeyT' && !e.repeat && S.tubeRect && !tunnelOn) {
      if (S.tubeOn && !S.tubeBody) unrollTube();
      else { S.tubeBody = false; applyTube(S.tubeRect, true); }
      updateHint();
    }
    if (e.code === 'KeyB' && !e.repeat && S.tubeRect && !tunnelOn && !followOn) {
      if (S.tubeOn && S.tubeBody) unrollTube();
      else { S.tubeBody = true; applyTube(S.tubeRect, true); }
      updateHint();
    }
    if (e.code === 'KeyB' && !e.repeat && !S.tubeRect && !tunnelOn && !followOn)
      toast('shift-drag a rect on the map first, then B wraps it on the body');
  });
  // the two clicks that mark a pair (photo point, then mesh point); the
  // pointer handlers above keep the camera still while this is armed
  canvas.addEventListener('click', (e) => {
    if (!capture) return;
    const r = rightEl.getBoundingClientRect();
    const x = e.clientX - r.left, y = e.clientY - r.top;
    captureClick(x, y, (x / r.width) * 2 - 1, 1 - (y / r.height) * 2);
  });
  addEventListener('keyup', (e) => { keys[e.code] = false; });
  // leaving the window (cmd-tab mid-flight) loses the keyups, so let go of
  // everything rather than cruising on until the next keypress
  addEventListener('blur', () => { for (const k in keys) keys[k] = false; });

  canvas.addEventListener('pointerdown', (e) => {
    canvas.focus();
    // while marking a pair, a click on the view is the mark — don't also
    // grab the camera and swing it away from what was clicked
    if (capture) return;
    if (e.shiftKey && document.body.classList.contains('overlay')) {
      ovDragging = true;
      canvas.setPointerCapture(e.pointerId);
      return;
    }
    dragging = true;
    canvas.setPointerCapture(e.pointerId);
  });
  canvas.addEventListener('pointerup', () => {
    dragging = false;
    if (ovDragging) { ovDragging = false; saveAlign(); }
  });
  canvas.addEventListener('pointermove', (e) => {
    if (ovDragging) {
      ov.cx += e.movementX / Math.max(1, rightEl.clientWidth);
      ov.cy += e.movementY / Math.max(1, rightEl.clientHeight);
      layoutOverlay();
      return;
    }
    if (!dragging) return;
    // grab-the-world: in perspective, dragging pulls the scene with the
    // pointer by looking around; in ortho it pans instead (WASD-equivalent
    // camera translation, so the ground follows the pointer)
    // ...except on the rails, where the drag swings the standoff in either
    // projection: a pan would be overwritten by followStep on the same frame
    if (S.ortho && !followOn) {
      view.panOrtho(e.movementX, e.movementY, canvas.clientHeight);
      return;
    }
    S.yaw -= e.movementX * 0.003;
    S.pitch += e.movementY * 0.003;
  });
  // scroll on the 3d view zooms by narrowing/widening the fov, like the
  // native app (which uses fov *= 0.9^wheel; setFov clamps to 1..150 deg)
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    // shift+wheel resizes the reference photo rather than the view, the
    // companion of shift+drag's reposition
    if (e.shiftKey && document.body.classList.contains('overlay')) {
      // zoom about the pointer: whatever point of the picture is under it
      // stays under it, so you can wind in on a window frame without chasing
      // the photo back across the viewport afterwards. the centre is stored
      // as a fraction of the viewport, so solve for the one that puts that
      // photo point back where it was at the new size
      const r = rightEl.getBoundingClientRect();
      const x = e.clientX - r.left, y = e.clientY - r.top;
      const held = viewToPhoto(x, y);
      // a mouse wheel with shift held arrives as a *horizontal* scroll (the
      // browser's shift-scroll convention), and some wheels count in lines
      // rather than pixels — a trackpad gives a plain deltaY, so take
      // whichever axis has the motion and put lines on a pixel scale
      const d = (e.deltaY || e.deltaX) * (e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 400 : 1);
      ov.scale = clampScale(ov.scale * Math.pow(0.9, d / 200));
      const b = overlayBox();
      // divide by the same size overlayBox() multiplies by — the rect is
      // fractional and clientWidth/Height are rounded, and solving against
      // one while drawing against the other nudges the photo a little the
      // same way every tick, which reads as a drift while zooming
      if (held && b) {
        ov.cx = (x + (0.5 - held.u) * b.w) / rightEl.clientWidth;
        ov.cy = (y + (0.5 - held.v) * b.h) / rightEl.clientHeight;
      }
      layoutOverlay();
      saveAlign();
      return;
    }
    // in ortho, zooming is a dolly (elevation is not a real degree of
    // freedom there); in perspective it narrows the fov like the native app
    if (S.ortho) { view.zoomOrtho(Math.pow(0.9, e.deltaY / 100)); return; }
    const p = view.getPose();
    view.setFov(p.fov * Math.pow(0.9, -e.deltaY / 100));
  }, { passive: false });
};
