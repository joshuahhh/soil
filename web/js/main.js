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

  // not behind TEST, so it works from the devtools console too
  window.__test = {
    stats: () => view.ready() ? view.getStats() : null,
    drawnNodes: () => view.ready() ? view.getDrawnNodes() : [],
    pendingNodes: () => view.ready() ? view.getPendingNodes() : [],
    // ms accumulated since the last call; reading resets it
    frameTiming: () => {
      const r = { engineMs: frameTiming.engineMs, tickMs: frameTiming.tickMs, gapMs: frameTiming.gapMs, frames: frameCount };
      frameTiming.engineMs = frameTiming.tickMs = frameTiming.gapMs = 0;
      return r;
    },
    fetchStats: () => JSON.parse(JSON.stringify(window.__stats)),
    frameCount: () => frameCount,
    // for tools/shimmer.mjs
    setMipmaps,
    nudgeHeading: (deg) => {
      const p = view.getPose();
      view.setPose(p.lat, p.lon, p.alt, p.heading + deg, p.tilt);
    },
    // captured at the end of the next frame
    shot: () => new Promise((resolve) => {
      S.pendingShot = () => resolve(canvas.toDataURL('image/png'));
    }),
    // the timeout is load-bearing: a download that keeps failing is reset to
    // a stub and retried forever (rocktree_types.h setFailedDownloading), so
    // sceneComplete can stay false indefinitely
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

// module top levels only declare things (reading config.js at most), which is
// what lets the modules import each other in circles. the init order still
// decides which keydown listener hears a key first (view, photo, io) and how
// the map's layers stack (tube under path)
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
