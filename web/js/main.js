import { S } from './config.js';
import { clearTiles, initTiles } from './tiles.js';
import { view, canvas, initEngine, frameCount, frameTiming } from './engine.js';
import { initView, inTube, setMipmaps } from './view.js';
import { initHud } from './hud.js';
import { initPhoto, pairs, lastErrs, dofCount, solving, startSolve } from './photo.js';
import { initTube, setBodyMesh, refreshTunnel, tubeRect, rectShape } from './tube.js';
import { initIO, copyState, saveState, pasteState } from './io.js';
import { map, marker, initMap } from './map.js';
import { path, parseGpx, setPath, initPath } from './path.js';
import { sky, glamour, applyGlamour, applySky, initSky } from './sky.js';

// --- the test api ---------------------------------------------------------
// window.__demo, window.__test (and window.__stats, in tiles.js) are an api:
// tools/*.mjs drive the page through them
const initTestApi = () => {
  // for devtools poking and automated testing
  window.__demo = { view, map, marker, isFollowing: () => S.following,
    sky: (patch) => { Object.assign(sky, patch); applySky(); return { ...sky }; },
    glamour: (patch) => { Object.assign(glamour, patch); applyGlamour(); return { ...glamour }; },
    lod: (v) => { if (v != null) view.setLodScale(v); return view.getLodScale(); },
    tube: () => ({ rect: tubeRect, on: inTube(), body: rectShape === 'body', curl: S.curl }),
    body: (text) => text ? setBodyMesh(text) : view.bodyReady(),
    tunnel: (c) => { if (c != null) { S.tunnelC = c; view.setTunnelCircumference(c); refreshTunnel(); } return view.getTunnelInfo(); },
    path: () => path, loadGpx: (text) => setPath(parseGpx(text), true),
    clearTiles,
    align: () => ({ pairs, solving, dof: dofCount(), errs: lastErrs }),
    solve: startSolve,
    copyState, pasteState, saveState };

  // --- headless capture api -------------------------------------------------
  // the driver (tools/capture.mjs) talks to the page through this. kept out of
  // the TEST guard so it's also usable by hand from the devtools console
  window.__test = {
    stats: () => view.ready() ? view.getStats() : null,
    drawnNodes: () => view.ready() ? view.getDrawnNodes() : [],
    pendingNodes: () => view.ready() ? view.getPendingNodes() : [],
    // wall-clock ms accumulated by the tick loop since the last call: inside
    // view.frame, in the whole tick, and between ticks
    frameTiming: () => {
      const r = { engineMs: frameTiming.engineMs, tickMs: frameTiming.tickMs, gapMs: frameTiming.gapMs, frames: frameCount };
      frameTiming.engineMs = frameTiming.tickMs = frameTiming.gapMs = 0;
      return r;
    },
    // the page's own fetch counters (window.__stats), for the load profiler
    fetchStats: () => JSON.parse(JSON.stringify(window.__stats)),
    frameCount: () => frameCount,
    // shimmer measurement (tools/shimmer.mjs) drives these: toggle the texture
    // filtering, then move the camera a fixed sub-degree amount and see how
    // much the image churns
    setMipmaps,
    nudgeHeading: (deg) => {
      const p = view.getPose();
      view.setPose(p.lat, p.lon, p.alt, p.heading + deg, p.tilt);
    },
    // resolves with a png data url, captured at the end of the next frame
    shot: () => new Promise((resolve) => {
      S.pendingShot = () => resolve(canvas.toDataURL('image/png'));
    }),
    // settle until the scene reports itself fully loaded for `stable`
    // consecutive frames, or `timeoutMs` elapses. sceneComplete alone is not
    // enough to wait on: a download that keeps failing resets its node to a
    // stub rather than to a failed state (see rocktree_types.h
    // setFailedDownloading), so it is retried forever and the flag can stay
    // false indefinitely. the timeout is therefore load-bearing, and the
    // caller is told which way it ended rather than it being papered over
    settle: async ({ timeoutMs = 120000, stable = 5 } = {}) => {
      const t0 = performance.now();
      let run = 0;
      for (;;) {
        await new Promise((r) => requestAnimationFrame(r));
        const st = view.ready() ? view.getStats() : null;
        run = st && st.sceneComplete ? run + 1 : 0;
        if (run >= stable) return { complete: true, ms: performance.now() - t0, stats: st };
        if (performance.now() - t0 > timeoutMs)
          return { complete: false, ms: performance.now() - t0, stats: st };
      }
    },
  };
};

// the page, brought up in order. a module's top level only declares things
// (reading config.js at most); whatever touches the page, the engine or the
// map waits for its init, called below. that is what lets the modules import
// each other in circles. the order still decides which keydown listener
// hears a key first (view, then photo, then io) and how the map's layers
// stack (tube under path)
await initEngine();
initTiles();
initMap();
initHud();
initView();
initTube();
initPath();
initPhoto();
initIO();
initSky();
initTestApi();
