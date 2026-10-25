import { QS, TEST, S } from './config.js';
import { setPlanetR } from './math.js';
import { fetchTile } from './tiles.js';
import { saveView, restoreView, mode, inTube, ortho, hug, keys, dragging } from './view.js';
import { updateSpeed, updateHint, refreshRoll } from './hud.js';
import { layoutOverlay, alignOn, updateMarkers, updateStatus, solving, solveStep, stopSolve } from './photo.js';
import { tunnelClosing, curlTarget, endTunnel, tubePending, advanceTube, bodyPromise } from './tube.js';
import { marker, setMarkerHeading, pickDebug, updatePickDebug, mapPointerDown, followMarker } from './map.js';
import { pushPath, spine, buildSpine, followStep } from './path.js';

export { Module, view, canvas, rightEl, resize, initEngine, frameCount, frameTiming };

// --- module + view --------------------------------------------------------
let Module = null, view = null; // arrive with initEngine
const canvas = document.getElementById('globe');
const rightEl = document.getElementById('right');
const resize = () => {
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(canvas.clientWidth * dpr);
  canvas.height = Math.round(canvas.clientHeight * dpr);
  layoutOverlay();
};
const initEngine = async () => {
  Module = await createEarthModule();

  resize();
  addEventListener('resize', resize);

  if (QS.has('jpg')) Module.forceJpgTextures();
  // msaa is on by default: the crawl on distant silhouettes is geometric
  // aliasing, which mipmaps don't touch. a context attribute, so no live key
  if (QS.has('msaa')) Module.setAntialias(QS.get('msaa') !== '0');
  // per-tile decode timing on the console; a line per node, so off unless asked
  if (QS.has('timing')) Module.setLogTiming(QS.get('timing') !== '0');

  view = Module.createView('#globe', fetchTile);
  // main runs its other inits synchronously before the first tick
  requestAnimationFrame(tick);
};

// --- the frame loop -------------------------------------------------------
let posed = false; // the start (or saved) pose has been applied
let speedAt = 0;
let lastSynced = { lat: 1e9, lon: 1e9 };
let saveTimer = 0;
let last = performance.now();
let frameCount = 0;
const frameTiming = { engineMs: 0, tickMs: 0, gapMs: 0, lastTick: 0 };
const tick = (now) => {
  const tTick0 = performance.now();
  const dt = TEST ? TEST.dt : Math.min(100, now - last);
  last = now;
  if (hug && now - speedAt > 100) { speedAt = now; updateSpeed(); }

  if (view.ready() && !posed) {
    posed = true;
    // the datum for setPose's altitude, every pick, and the alignment math
    setPlanetR(view.planetRadius());
    restoreView();
    // a track loaded before the planet metadata needs the radius too
    pushPath();
    buildSpine();
    updateHint();
  }

  const slow = !!(keys.ShiftLeft || keys.ShiftRight);
  // rad/s. in ortho the arrows orbit whatever is at screen centre (a wasm-side raycast)
  const rot = (slow ? 0.1 : 1) * dt / 1000;
  // on the rails the arrows swing round the track and W/S run along it
  const railed = mode === 'follow' && spine && view.ready();
  const orbitH = ortho ? ((keys.ArrowRight ? 1 : 0) - (keys.ArrowLeft ? 1 : 0)) : 0;
  const orbitV = ortho ? ((keys.ArrowUp ? 1 : 0) - (keys.ArrowDown ? 1 : 0)) : 0;
  if ((orbitH || orbitV) && view.ready()) view.orbit(orbitH * 45 * rot, orbitV * 34 * rot); // deg
  if (!ortho && !railed && keys.ArrowRight) S.yaw += 1.2 * rot;
  if (!ortho && !railed && keys.ArrowLeft) S.yaw -= 1.2 * rot;
  if (!ortho && !railed && keys.ArrowUp) S.pitch += 0.9 * rot;
  if (!ortho && !railed && keys.ArrowDown) S.pitch -= 0.9 * rot;
  // in ortho R/F zoom too (below): a parallel projection has no pedestal
  if (!!keys.KeyZ !== !!keys.KeyX) {
    if (ortho) {
      view.zoomOrtho(Math.pow(0.9, (dt / 150) * (keys.KeyZ ? 1 : -1)));
    } else {
      const p = view.getPose();
      view.setFov(p.fov * Math.pow(0.9, (dt / 150) * (keys.KeyZ ? -1 : 1)));
    }
  }
  if (ortho && !railed && !!keys.KeyR !== !!keys.KeyF) {
    view.zoomOrtho(Math.pow(0.9, (dt / 150) * (keys.KeyR ? 1 : -1))); // R out, F in
  }

  const flying = dragging || S.yaw !== 0 || S.pitch !== 0 || orbitH !== 0 || orbitV !== 0
    || keys.KeyW || keys.KeyS || keys.KeyA || keys.KeyD || keys.KeyQ || keys.KeyE
    || keys.KeyR || keys.KeyF;
  if (flying && !S.following) S.following = true;

  // in the tube Q/E roll, and R/F boom along the camera's own up
  const roll = inTube() ? ((keys.KeyE ? 1 : 0) - (keys.KeyQ ? 1 : 0)) * 1.5 * rot : 0;
  // damped turning under W, so forward flight curves instead of veers
  const turn = keys.KeyW ? 0.2 : 1;
  if (railed) followStep(dt, slow);
  else view.fly(S.yaw * turn, S.pitch * turn, roll * turn,
    !!keys.KeyW, !!keys.KeyS, !!keys.KeyA, !!keys.KeyD,
    (inTube() || !ortho) && !!keys.KeyR,
    (inTube() || !ortho) && !!keys.KeyF,
    slow, !!keys.Space, S.gain, dt);
  S.yaw = 0;
  S.pitch = 0;
  // for tools/loadprof.mjs: the engine's own timer can't see a stalled main thread
  const tFrame0 = performance.now();
  view.frame(dt);
  frameTiming.engineMs += performance.now() - tFrame0;

  if (view.ready() && posed) {
    const tubeInfoEl = document.getElementById('tubeinfo');
    if (inTube()) {
      const ti = advanceTube(now);
      const parts = [];
      if (tubePending || ti.nodesLoaded < ti.nodesWanted)
        parts.push(`loading ${ti.nodesLoaded}/${ti.nodesWanted} tiles`);
      if (!tubePending && S.curl !== curlTarget) parts.push(mode === 'body' ? 'wrapping…' : 'rolling…');
      tubeInfoEl.style.display = parts.length ? '' : 'none';
      if (parts.length) tubeInfoEl.textContent = (mode === 'body' ? 'body: ' : 'tube: ') + parts.join(' · ');
    } else if (mode === 'tunnel') {
      const ti = view.getTunnelInfo();
      const loading = ti.nodesLoaded < ti.nodesWanted;
      tubeInfoEl.style.display = loading ? '' : 'none';
      if (loading) tubeInfoEl.textContent = `tunnel: loading ${ti.nodesLoaded}/${ti.nodesWanted} tiles`;
    } else if (bodyPromise) {
      tubeInfoEl.style.display = '';
      tubeInfoEl.textContent = 'body: loading the figure…';
    } else {
      tubeInfoEl.style.display = 'none';
    }
    // the tunnel's way out is brisk and cut short: the flatter the sheet,
    // the more of the rect's edge (and the nothing past it) is in view
    if ((inTube() && !tubePending || mode === 'tunnel') && S.curl !== curlTarget) {
      const tau = tunnelClosing ? 150 : mode === 'tunnel' ? 300 : 700;
      S.curl += (curlTarget - S.curl) * (1 - Math.exp(-dt / tau));
      if (Math.abs(curlTarget - S.curl) < 0.001) S.curl = curlTarget;
      view.setTubeCurl(S.curl);
      if (tunnelClosing && S.curl < 0.03) endTunnel();
    }
    const p = view.getPose();
    // in the tube the marker is the camera's spot on the unrolled rect
    const tp = (inTube() || mode === 'tunnel') ? view.getTubePose() : null;
    const m = tp || p;
    setMarkerHeading(m.heading);
    if (Math.abs(m.lat - lastSynced.lat) > 1e-7 || Math.abs(m.lon - lastSynced.lon) > 1e-7) {
      lastSynced = { lat: m.lat, lon: m.lon };
      marker.setLngLat([m.lon, m.lat]);
    }
    // every frame, so the ease converges after the camera stops
    if (S.following && !mapPointerDown) followMarker({ lat: m.lat, lng: m.lon }, dt);
    if (pickDebug) updatePickDebug();

    // a few solver steps a frame, so the camera visibly settles onto the fit
    if (alignOn) {
      if (solving) {
        for (let i = 0; i < 4 && solving; i++) if (solveStep()) stopSolve();
      }
      updateStatus(updateMarkers());
    }

    saveTimer += dt;
    if (saveTimer > 1000) {
      saveTimer = 0;
      saveView();
    }
  }
  refreshRoll();
  frameCount++;
  frameTiming.tickMs += performance.now() - tTick0;
  if (frameTiming.lastTick) frameTiming.gapMs += tTick0 - frameTiming.lastTick;
  frameTiming.lastTick = tTick0;
  // a capture must read the canvas in the task that drew it: without
  // preserveDrawingBuffer it's undefined once composited
  if (S.pendingShot) {
    const f = S.pendingShot;
    S.pendingShot = null;
    f();
  }
  requestAnimationFrame(tick);
};
