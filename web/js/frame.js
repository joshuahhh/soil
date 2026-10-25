import { QS, TEST, START } from './config.js';
import { S } from './state.js';
import { setPlanetR } from './geo.js';
import { view } from './engine.js';
import { saveView } from './saveview.js';
import { marker, setMarkerHeading, pickDebug, updatePickDebug, mapPointerDown,
  followMarker } from './map.js';
import { updateSpeed, updateHint, refreshRoll } from './hud.js';
import { refreshKeys } from './keytable.js';
import { showCut } from './nearcut.js';
import { keys, setMiniature, setDebugLod, dragging } from './input.js';
import { tunnelOn, K_MIN, K_MAX, refreshTunnel, tunnelClosing, curlTarget, endTunnel,
  setTunnel } from './tunnel.js';
import { tubePendingSince, setRectData, updateAxisBtn, flyIntoTube, flyOntoBody,
  applyTube } from './tube.js';
import { bodyPromise } from './body.js';
import { followOn, pushPath, spine, buildSpine, followStep } from './path.js';
import { updateMarkers, updateStatus, solving, solveStep, stopSolve } from './align.js';
import { applyGlamour, applySky } from './sky.js';

export { frameCount, frameTiming, initFrame };

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
  refreshKeys(now);
  if (S.hug && now - speedAt > 100) { speedAt = now; updateSpeed(); }

  if (view.ready() && !posed) {
    // once the planet metadata is in, jump to the start (or saved) pose
    posed = true;
    // the datum setPose's altitude and every pick are measured from; the
    // alignment math rebuilds ecef points against it
    setPlanetR(view.planetRadius());
    view.setPose(START.lat, START.lon, START.alt, START.heading, START.tilt);
    view.setFov(START.fov);
    if (Number.isFinite(START.nearCut) && START.nearCut > 0) {
      view.setNearCut(START.nearCut);
      showCut();
    }
    S.gain = START.gain;
    S.ortho = !!START.ortho;
    view.setOrtho(S.ortho);
    S.hug = !!START.hug;
    view.setTerrainFollow(S.hug);
    if (START.miniature) setMiniature(true);
    if (START.lod) setDebugLod(true);
    applySky();
    applyGlamour();
    // a banked camera comes back banked. inside the tube the bank belongs to
    // the airplane frame and is restored after the roll-up below instead,
    // because that is what creates the frame to bank
    if (START.roll && !(START.tube && START.tube.on)) view.setRoll(START.roll);
    // restore a saved tube rect; if it was rolled, it rolls up again from
    // flat (the camera pose is already wherever it was saved)
    if (START.tube && START.tube.rect) {
      S.tubeRect = START.tube.rect;
      S.tubeAxisEw = START.tube.ewAxis !== false;
      S.tubeBody = !!START.tube.body;
      updateAxisBtn();
      setRectData(S.tubeRect);
      if (START.tube.on) {
        applyTube(S.tubeRect, false); // reload + roll in place at the saved pose
        if (START.roll) view.setRoll(START.roll); // restore the bank angle
      } else {
        view.setTubeRect(S.tubeRect[0], S.tubeRect[1], S.tubeRect[2], S.tubeRect[3], S.tubeAxisEw);
      }
    }
    if (START.tunnel && Number.isFinite(START.tunnel.c)) S.tunnelC = START.tunnel.c;
    if (START.tunnel && START.tunnel.across === false) S.tunnelAcross = false;
    if (START.tunnel && Number.isFinite(START.tunnel.lag)) S.tunnelLag = Math.max(0, START.tunnel.lag);
    if (START.tunnel && Number.isFinite(START.tunnel.k)) S.tunnelK = Math.max(K_MIN, Math.min(K_MAX, START.tunnel.k));
    refreshTunnel();
    if (START.tunnel && START.tunnel.on && !(START.tube && START.tube.on)) setTunnel(true, false);
    // ?tunnel=<circumference m> (test mode): straight into the tunnel at
    // the given pose, for the render tests
    if (TEST && QS.has('tunnel')) {
      const c = parseFloat(QS.get('tunnel'));
      if (Number.isFinite(c) && c > 0) { S.tunnelC = c; refreshTunnel(); setTunnel(true, false); }
    }
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
  const railed = followOn && spine && view.ready();
  const orbitH = S.ortho ? ((keys.ArrowRight ? 1 : 0) - (keys.ArrowLeft ? 1 : 0)) : 0;
  const orbitV = S.ortho ? ((keys.ArrowUp ? 1 : 0) - (keys.ArrowDown ? 1 : 0)) : 0;
  if ((orbitH || orbitV) && view.ready()) view.orbit(orbitH * 45 * rot, orbitV * 34 * rot); // deg
  if (!S.ortho && !railed && keys.ArrowRight) S.yaw += 1.2 * rot;
  if (!S.ortho && !railed && keys.ArrowLeft) S.yaw -= 1.2 * rot;
  if (!S.ortho && !railed && keys.ArrowUp) S.pitch += 0.9 * rot;
  if (!S.ortho && !railed && keys.ArrowDown) S.pitch -= 0.9 * rot;
  // z/x zoom like the wheel; in ortho R/F join them as zoom too (below),
  // since a parallel projection has no meaningful elevation to pedestal
  // along — so the up/down keys would otherwise sit dead there
  if (!!keys.KeyZ !== !!keys.KeyX) {
    if (S.ortho) {
      view.zoomOrtho(Math.pow(0.9, (dt / 150) * (keys.KeyZ ? 1 : -1)));
    } else {
      const p = view.getPose();
      view.setFov(p.fov * Math.pow(0.9, (dt / 150) * (keys.KeyZ ? -1 : 1)));
    }
  }
  if (S.ortho && !railed && !!keys.KeyR !== !!keys.KeyF) {
    view.zoomOrtho(Math.pow(0.9, (dt / 150) * (keys.KeyR ? 1 : -1))); // R out, F in
  }

  const flying = dragging || S.yaw !== 0 || S.pitch !== 0 || orbitH !== 0 || orbitV !== 0
    || keys.KeyW || keys.KeyS || keys.KeyA || keys.KeyD || keys.KeyQ || keys.KeyE
    || keys.KeyR || keys.KeyF;
  if (flying && !S.following) S.following = true;

  // in tube mode Q/E become the roll axis (airplane style) and R/F boom
  // along the camera's own up — the body-frame counterpart of A/D
  const roll = S.tubeOn ? ((keys.KeyE ? 1 : 0) - (keys.KeyQ ? 1 : 0)) * 1.5 * rot : 0;
  // thrusting steadies the hand: turning (keys and drag alike) drops to
  // a fifth while W is held, so forward flight curves instead of veers
  const turn = keys.KeyW ? 0.2 : 1;
  if (railed) followStep(dt, slow);
  else view.fly(S.yaw * turn, S.pitch * turn, roll * turn,
    !!keys.KeyW, !!keys.KeyS, !!keys.KeyA, !!keys.KeyD,
    (S.tubeOn || !S.ortho) && !!keys.KeyR,
    (S.tubeOn || !S.ortho) && !!keys.KeyF,
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
    // stage 2+3 of the tube pipeline: the engine locks ground level as
    // soon as the rect is covered at measurement resolution (well before
    // full res), then we fly in and roll while the rest keeps streaming
    // into the rolled tube. after 20s (a download stuck or failed), force
    // the measurement with what's resident
    const tubeInfoEl = document.getElementById('tubeinfo');
    if (S.tubeOn) {
      let ti = view.getTubeInfo();
      if (S.tubePending) {
        if (now - tubePendingSince > 20000 && !ti.groundLocked) {
          view.lockTubeGround();
          ti = view.getTubeInfo();
        }
        if (ti.groundLocked) {
          if (S.tubePending === 'fly') (S.tubeBody ? flyOntoBody : flyIntoTube)(S.tubeRect);
          S.tubePending = null;
        }
      }
      const parts = [];
      if (S.tubePending || ti.nodesLoaded < ti.nodesWanted)
        parts.push(`loading ${ti.nodesLoaded}/${ti.nodesWanted} tiles`);
      if (!S.tubePending && S.curl !== curlTarget) parts.push(S.tubeBody ? 'wrapping…' : 'rolling…');
      tubeInfoEl.style.display = parts.length ? '' : 'none';
      if (parts.length) tubeInfoEl.textContent = (S.tubeBody ? 'body: ' : 'tube: ') + parts.join(' · ');
    } else if (tunnelOn) {
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
    if ((S.tubeOn && !S.tubePending || tunnelOn) && S.curl !== curlTarget) {
      const tau = tunnelClosing ? 150 : tunnelOn ? 300 : 700;
      S.curl += (curlTarget - S.curl) * (1 - Math.exp(-dt / tau));
      if (Math.abs(curlTarget - S.curl) < 0.001) S.curl = curlTarget;
      view.setTubeCurl(S.curl);
      if (tunnelClosing && S.curl < 0.03) endTunnel();
    }
    const p = view.getPose();
    // in tube mode the marker shows the camera in unrolled tube
    // coordinates — the spot on the rect whose imagery is wrapped around
    // the camera — rather than the now-meaningless ecef point under it
    const tp = (S.tubeOn || tunnelOn) ? view.getTubePose() : null;
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
    if (document.body.classList.contains('align')) {
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
const initFrame = () => {
  requestAnimationFrame(tick);
};
