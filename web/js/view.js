import { QS, TEST, START, S, saveStore, DEFAULT_VIEW } from './config.js';
import { view, canvas, rightEl } from './engine.js';
import { updateHint, showCut, toast, keysOpen, showKeys, toggleKeys, toggleCutPanel } from './hud.js';
import { ov, photoShown, clampScale, overlayBox, layoutOverlay, viewToPhoto, toggleOverlayBlend,
  saveAlign, capture, captureClick, stopSolve } from './photo.js';
import { refreshTunnel, setTunnel, tunnelState, restoreTunnel, tubeState, restoreTube,
  tunnelClosing, tubeRect, rollRect, clearRect } from './tube.js';
import { map, full3d, pickDebug, setPickDebug } from './map.js';
import { debugSmooth, setDebugSmooth, setFollow, toggleFollow } from './path.js';
import { sky, glamour, applySky, applyGlamour } from './sky.js';
import { stl, setStl } from './stl.js';

export { initView, mode, inTube, viewName, defineMode, setMode, ortho, hug, setHug, saveView,
  restoreView, keys, miniature, mipmaps, setMipmaps, clay, dragging };

// --- the view's mode -----------------------------------------------------
// exactly one of: fly, follow (path.js), tunnel, tube, body (tube.js).
// setMode is the one place it changes, so the old mode is always exited
// before the new one is entered.
// not modes: ortho, terrain hug, the rect (drawn whether or not rolled),
// the tube's staged load (tubePending), and the tunnel's closing animation
// (tunnelClosing; the mode is still tunnel)
let mode = 'fly';
const inTube = () => mode === 'tube' || mode === 'body';
// the name the hint and the key table go by: fly splits by projection
const viewName = () => mode === 'fly' ? (ortho ? 'ortho' : 'persp') : mode;

// { enter(opts), exit() } per mode but fly. tube and body share theirs, so
// switching between them, or re-asking for the current mode, re-enters
// without exiting
const modes = {};
const defineMode = (name, hooks) => { modes[name] = hooks; };

// ortho's extent is anchored on flat terrain, and an ortho tube reads as
// mush. hug fights the rails over altitude, and has no body version
const ORTHO_OK = new Set(['fly', 'follow']);
const HUG_OK = new Set(['fly', 'tube', 'tunnel']);

// both hooks see the new mode already
const setMode = (next, opts = {}) => {
  const from = mode, a = modes[from], b = modes[next];
  mode = next;
  if (from !== next && a !== b && a) a.exit();
  if (!ORTHO_OK.has(next)) setOrtho(false);
  if (!HUG_OK.has(next)) setHug(false);
  if (b) b.enter(opts);
  updateHint();
};

// each returns whether it changed anything
let ortho = false, hug = false;
const setOrtho = (on) => {
  if (on === ortho || (on && !ORTHO_OK.has(mode))) return false;
  ortho = on;
  view.setOrtho(on);
  return true;
};
// in the tube, the hug is radial, above the wall
const setHug = (on) => {
  if (on === hug || (on && !HUG_OK.has(mode))) return false;
  hug = on;
  view.setTerrainFollow(on);
  return true;
};

// --- the saved view -------------------------------------------------------
// config.js reads 'earth-view' back as START
const saveView = () => {
  if (TEST) return; // captures never write back over the user's saved view
  if (!view.ready()) return;
  const p = view.getPose();
  saveStore('earth-view', {
    lat: p.lat, lon: p.lon, alt: p.alt, heading: p.heading, tilt: p.tilt,
    roll: p.roll, fov: p.fov, zoom: map.getZoom(), gain: S.gain, ortho, hug,
    miniature, full3d,
    nearCut: view.getNearCut(),
    lod: debugLod,
    sky, glamour,
    tube: tubeState(),
    stl,
    tunnel: tunnelState(),
  });
};
// map.js restores its own centre, zoom and full3d, sky.js the sky settings
const restoreView = () => {
  view.setPose(START.lat, START.lon, START.alt, START.heading, START.tilt);
  view.setFov(START.fov);
  if (Number.isFinite(START.nearCut) && START.nearCut > 0) {
    view.setNearCut(START.nearCut);
    showCut();
  }
  S.gain = START.gain;
  setOrtho(!!START.ortho);
  setHug(!!START.hug);
  if (START.miniature) setMiniature(true);
  if (START.lod) setDebugLod(true);
  if (QS.has('clay')) setClay(true);
  applySky();
  applyGlamour();
  // in the tube, roll is relative to the airplane frame, which only exists
  // after the roll-up
  const tubeRolled = !!(START.tube && START.tube.on);
  if (START.roll && !tubeRolled) view.setRoll(START.roll);
  if (restoreTube(START.tube) && START.roll) view.setRoll(START.roll);
  if (START.stl && START.stl.on) setStl(true, true);
  restoreTunnel(START.tunnel, tubeRolled);
  // ?tunnel=<circumference m>, for the render tests
  if (TEST && QS.has('tunnel')) {
    const c = parseFloat(QS.get('tunnel'));
    if (Number.isFinite(c) && c > 0) { S.tunnelC = c; refreshTunnel(); setTunnel(true, false); }
  }
};
const initSaveView = () => {
  addEventListener('beforeunload', saveView);
};

// --- keyboard -> built-in flying controls -----------------------------------
// on the page, not the canvas, so a panel's slider or button doesn't take
// the keys away; only a text field does
const keys = {};
const typingIn = (t) => !!t && (t.tagName === 'TEXTAREA' || t.isContentEditable
  || t.tagName === 'SELECT'
  || (t.tagName === 'INPUT' && !['range', 'checkbox', 'color', 'button', 'radio'].includes(t.type)));
const handled = new Set(['KeyW','KeyA','KeyS','KeyD','KeyQ','KeyE','KeyR','KeyF','KeyZ','KeyX','KeyG','KeyO','KeyC','KeyK','KeyL','KeyM','KeyT','KeyP','KeyV','KeyN','KeyB','KeyY','KeyI','KeyU',
  'Space','ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Minus','Equal','Digit0','Numpad0','Home','KeyH','Digit3']);
let miniature = false; // the tilt-shift look (N)
const setMiniature = (on) => { miniature = on; view.setMiniature(on, 0.015, 0.35); updateHint(); };
let debugLod = false;
const setDebugLod = (on) => {
  debugLod = on;
  view.setDebugLod(on);
  document.body.classList.toggle('lod', on);
  saveView();
};
let mipmaps = true;
const setMipmaps = (on) => { mipmaps = on; view.setMipmaps(on); updateHint(); };
let clay = false; // every mesh one colour, lit (U)
const setClay = (on) => { clay = on; view.setClay(on, 0xccc7bd); updateHint(); };
// leaves the tube alone: esc clears it
const resetView = () => {
  if (!view.ready()) return;
  if (mode === 'follow') setFollow(false); // else the rails put the camera straight back
  view.setPose(DEFAULT_VIEW.lat, DEFAULT_VIEW.lon, DEFAULT_VIEW.alt,
    DEFAULT_VIEW.heading, DEFAULT_VIEW.tilt);
  view.setFov(DEFAULT_VIEW.fov);
  view.setRoll(0);
  S.gain = DEFAULT_VIEW.gain;
  setOrtho(false);
  if (miniature) setMiniature(false);
  setHug(false);
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
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    if (typingIn(e.target)) return;
    if (handled.has(e.code)) e.preventDefault();
    keys[e.code] = true;
    if (e.code === 'Minus' && !e.repeat) { S.gain = Math.max(1/128, S.gain/2); updateHint(); }
    if (e.code === 'Equal' && !e.repeat) { S.gain = Math.min(128, S.gain*2); updateHint(); }
    if (e.code === 'KeyN' && !e.repeat) setMiniature(!miniature);
    if (e.code === 'KeyU' && !e.repeat) setClay(!clay);
    if (e.code === 'KeyO' && !e.repeat && setOrtho(!ortho)) updateHint();
    if (e.code === 'KeyG' && !e.repeat && setHug(!hug)) {
      if (hug) stopSolve(); // the two fight over altitude — see startSolve
      updateHint();
    }
    if (e.code === 'KeyC' && !e.repeat) setPickDebug(!pickDebug);
    if (e.code === 'KeyV' && !e.repeat) toggleFollow();
    if (e.code === 'KeyP' && !e.repeat) toggleOverlayBlend();
    if (e.code === 'KeyH' && !e.repeat) toggleCutPanel();
    if (e.key === '?' && !e.repeat) toggleKeys();
    if (e.code === 'Escape' && keysOpen) {
      showKeys(false);
      return;
    }
    if ((e.code === 'Digit0' || e.code === 'Numpad0' || e.code === 'Home') && !e.repeat) resetView();
    // as in the native app: red where a finer tile is wanted but not loaded
    if (e.code === 'KeyL' && !e.repeat) setDebugLod(!debugLod);
    if (e.code === 'KeyY' && !e.repeat) document.body.classList.toggle('skypanel');
    if (e.code === 'KeyI' && !e.repeat) document.body.classList.toggle('glamourpanel');
    // the smoothed track the follow camera actually aims down
    if (e.code === 'KeyK' && !e.repeat) setDebugSmooth(!debugSmooth);
    // only visible on the uncompressed (?jpg) path, and only in motion
    if (e.code === 'KeyM' && !e.repeat) setMipmaps(!mipmaps);
    if (e.code === 'Escape' && mode === 'follow') setFollow(false);
    if (e.code === 'Escape' && mode === 'tunnel') { setTunnel(false); return; }
    if (e.code === 'KeyT' && !e.repeat && mode !== 'follow' && (mode === 'tunnel' || !tubeRect)) {
      setTunnel(mode !== 'tunnel' || tunnelClosing);
      return;
    }
    if (e.code === 'Escape' && tubeRect) {
      clearRect();
      updateHint();
    }
    if (e.code === 'Digit3' && !e.repeat) setStl(!stl.on);
    // true: fly into the mouth; from outside, the roll mostly shows the
    // back of the sheet
    if (e.code === 'KeyT' && !e.repeat && tubeRect && mode !== 'tunnel') {
      if (mode === 'tube') setMode('fly');
      else rollRect('tube', true);
      updateHint();
    }
    if (e.code === 'KeyB' && !e.repeat && tubeRect && mode !== 'tunnel' && mode !== 'follow') {
      if (mode === 'body') setMode('fly');
      else rollRect('body', true);
      updateHint();
    }
    if (e.code === 'KeyB' && !e.repeat && !tubeRect && mode !== 'tunnel' && mode !== 'follow')
      toast('shift-drag a rect on the map first, then B wraps it on the body');
  });
  // marking a pair: photo point, then mesh point
  canvas.addEventListener('click', (e) => {
    if (!capture) return;
    const r = rightEl.getBoundingClientRect();
    const x = e.clientX - r.left, y = e.clientY - r.top;
    captureClick(x, y, (x / r.width) * 2 - 1, 1 - (y / r.height) * 2);
  });
  addEventListener('keyup', (e) => { keys[e.code] = false; });
  // cmd-tab mid-flight loses the keyups
  addEventListener('blur', () => { for (const k in keys) keys[k] = false; });

  canvas.addEventListener('pointerdown', (e) => {
    canvas.focus();
    // the click is the mark; don't swing the camera off it
    if (capture) return;
    if (e.shiftKey && photoShown) {
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
    // no ortho pan on the rails: followStep would overwrite it that frame
    if (ortho && mode !== 'follow') {
      view.panOrtho(e.movementX, e.movementY, canvas.clientHeight);
      return;
    }
    S.yaw -= e.movementX * 0.003;
    S.pitch += e.movementY * 0.003;
  });
  // fov zoom as in the native app (setFov clamps to 1..150 deg)
  canvas.addEventListener('wheel', (e) => {
    e.preventDefault();
    if (e.shiftKey && photoShown) {
      // zoom about the pointer
      const r = rightEl.getBoundingClientRect();
      const x = e.clientX - r.left, y = e.clientY - r.top;
      const held = viewToPhoto(x, y);
      // a mouse wheel with shift held scrolls in deltaX, and may count lines
      const d = (e.deltaY || e.deltaX) * (e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 400 : 1);
      ov.scale = clampScale(ov.scale * Math.pow(0.9, d / 200));
      const b = overlayBox();
      // divide by the same (rounded) clientWidth/Height overlayBox uses; the
      // fractional rect here makes the photo drift while zooming
      if (held && b) {
        ov.cx = (x + (0.5 - held.u) * b.w) / rightEl.clientWidth;
        ov.cy = (y + (0.5 - held.v) * b.h) / rightEl.clientHeight;
      }
      layoutOverlay();
      saveAlign();
      return;
    }
    // in ortho, a dolly: elevation isn't a real degree of freedom there
    if (ortho) { view.zoomOrtho(Math.pow(0.9, e.deltaY / 100)); return; }
    const p = view.getPose();
    view.setFov(p.fov * Math.pow(0.9, -e.deltaY / 100));
  }, { passive: false });
};

const initView = () => {
  initSaveView();
  initInput();
};
