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
  // the overlay is placed in viewport-relative terms, so it follows
  // window resizes and the map pane collapsing/expanding
  layoutOverlay();
};
const initEngine = async () => {
  Module = await createEarthModule();

  resize();
  addEventListener('resize', resize);

  if (QS.has('jpg')) Module.forceJpgTextures();
  // multisampling is on by default: mipmaps only fix texture minification,
  // while the crawl on distant building silhouettes is geometric aliasing,
  // which needs this instead. ?msaa=0 opts out (for a slow gpu, or to A/B it).
  // context attribute, so it can't be a live key toggle like M
  if (QS.has('msaa')) Module.setAntialias(QS.get('msaa') !== '0');
  // per-tile decode timing on the console; a line per node, so off unless asked
  if (QS.has('timing')) Module.setLogTiming(QS.get('timing') !== '0');

  view = Module.createView('#globe', fetchTile);
  // the frame loop's first callback comes after main has run every other
  // init, which it does synchronously after this resolves
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
    // once the planet metadata is in, jump to the start (or saved) pose
    posed = true;
    // the datum setPose's altitude and every pick are measured from; the
    // alignment math rebuilds ecef points against it
    setPlanetR(view.planetRadius());
    restoreView();
    // a track dropped (or restored) before the planet metadata arrived is
    // still sitting in js: its ecef points need the planetoid radius, and
    // so does the spine's tangent plane
    pushPath();
    buildSpine();
    updateHint();
  }

  const slow = !!(keys.ShiftLeft || keys.ShiftRight);
  // arrows pan/tilt at fixed angular rates (rad/s), shift-slowed. in
  // ortho mode all four arrows instead orbit around whatever is at the
  // center of the screen (raycast into the scene on the wasm side):
  // left/right around its up axis, up/down in tilt
  const rot = (slow ? 0.1 : 1) * dt / 1000;
  // on the rails the arrows swing the camera around the track instead of
  // aiming it, and W/S run along the track instead of flying
  const railed = mode === 'follow' && spine && view.ready();
  const orbitH = ortho ? ((keys.ArrowRight ? 1 : 0) - (keys.ArrowLeft ? 1 : 0)) : 0;
  const orbitV = ortho ? ((keys.ArrowUp ? 1 : 0) - (keys.ArrowDown ? 1 : 0)) : 0;
  if ((orbitH || orbitV) && view.ready()) view.orbit(orbitH * 45 * rot, orbitV * 34 * rot); // deg
  if (!ortho && !railed && keys.ArrowRight) S.yaw += 1.2 * rot;
  if (!ortho && !railed && keys.ArrowLeft) S.yaw -= 1.2 * rot;
  if (!ortho && !railed && keys.ArrowUp) S.pitch += 0.9 * rot;
  if (!ortho && !railed && keys.ArrowDown) S.pitch -= 0.9 * rot;
  // z/x zoom like the wheel; in ortho R/F join them as zoom too (below),
  // since a parallel projection has no meaningful elevation to pedestal
  // along — so the up/down keys would otherwise sit dead there
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

  // in tube mode Q/E become the roll axis (airplane style) and R/F boom
  // along the camera's own up — the body-frame counterpart of A/D
  const roll = inTube() ? ((keys.KeyE ? 1 : 0) - (keys.KeyQ ? 1 : 0)) * 1.5 * rot : 0;
  // thrusting steadies the hand: turning (keys and drag alike) drops to
  // a fifth while W is held, so forward flight curves instead of veers
  const turn = keys.KeyW ? 0.2 : 1;
  if (railed) followStep(dt, slow);
  else view.fly(S.yaw * turn, S.pitch * turn, roll * turn,
    !!keys.KeyW, !!keys.KeyS, !!keys.KeyA, !!keys.KeyD,
    (inTube() || !ortho) && !!keys.KeyR,
    (inTube() || !ortho) && !!keys.KeyF,
    slow, !!keys.Space, S.gain, dt);
  S.yaw = 0;
  S.pitch = 0;
  // wall-clock accounting for the load profiler (tools/loadprof.mjs):
  // time inside the engine's frame, the whole tick, and the gap since the
  // last one. the engine's own section timer can't see a stalled main
  // thread; this can
  const tFrame0 = performance.now();
  view.frame(dt);
  frameTiming.engineMs += performance.now() - tFrame0;

  if (view.ready() && posed) {
    // the tube's staged load (tube.js), and what the tube, the tunnel or
    // the body is still waiting on
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
    // ease the roll toward closed: the time constant reads as the ground
    // curling up rather than snapping, ~.7s for the rect tube, ~.3s for
    // the tunnel. the tunnel's way out is brisker still and cut short:
    // the flatter the sheet, the more of the rect's edge (and the nothing
    // past it) is in view, so it goes straight to the ground in ~half a
    // second rather than easing all the way
    if ((inTube() && !tubePending || mode === 'tunnel') && S.curl !== curlTarget) {
      const tau = tunnelClosing ? 150 : mode === 'tunnel' ? 300 : 700;
      S.curl += (curlTarget - S.curl) * (1 - Math.exp(-dt / tau));
      if (Math.abs(curlTarget - S.curl) < 0.001) S.curl = curlTarget;
      view.setTubeCurl(S.curl);
      if (tunnelClosing && S.curl < 0.03) endTunnel();
    }
    const p = view.getPose();
    // in tube mode the marker shows the camera in unrolled tube
    // coordinates — the spot on the rect whose imagery is wrapped around
    // the camera — rather than the now-meaningless ecef point under it
    const tp = (inTube() || mode === 'tunnel') ? view.getTubePose() : null;
    const m = tp || p;
    setMarkerHeading(m.heading);
    if (Math.abs(m.lat - lastSynced.lat) > 1e-7 || Math.abs(m.lon - lastSynced.lon) > 1e-7) {
      lastSynced = { lat: m.lat, lon: m.lon };
      marker.setLngLat([m.lon, m.lat]);
    }
    // runs every frame (not just on movement) so the ease finishes
    // converging even after the camera stops; paused while the pointer is
    // down on the map so drag gestures aren't cancelled mid-flight
    if (S.following && !mapPointerDown) followMarker({ lat: m.lat, lng: m.lon }, dt);
    if (pickDebug) updatePickDebug();

    // point-pair alignment: run the solver (a few steps a frame, so the
    // camera visibly settles onto the fit instead of snapping to it), then
    // redraw the pair markers against wherever the camera ended up
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
  // a capture has to read the canvas in the same task that drew it: the
  // context is created without preserveDrawingBuffer, so the backbuffer is
  // undefined once the browser composites the frame
  if (S.pendingShot) {
    const f = S.pendingShot;
    S.pendingShot = null;
    f();
  }
  requestAnimationFrame(tick);
};
