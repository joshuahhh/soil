import { S } from './state.js';
import { view, canvas } from './engine.js';
import { saveView } from './saveview.js';
import { updateHint } from './hud.js';
import { mode, defineMode, setMode } from './mode.js';
import { logScale, bindRange } from './ui.js';

export { refreshTunnel, tunnelClosing, curlTarget, endTunnel, setTunnel, tunnelState,
  restoreTunnel, initTunnel };

// --- tunnel mode ----------------------------------------------------------
// the tube without the rect: the ground ahead curls up in front of the
// camera, over it and down behind, whichever way it faces — the camera
// on the axis, looking along the circle, new land rolling up ahead as
// you fly (earth_core tunnelUpdate). one parameter, the circumference;
// T toggles it, the panel's slider sets it
const tunnelAxisBtn = document.getElementById('tunnelaxis');
const tunnelLenRow = document.getElementById('tunnellenrow');
const fmtM = (m) => m >= 1000 ? `${(m / 1000).toFixed(m >= 10000 ? 0 : 1)} km` : `${Math.round(m)} m`;
// the length slider runs from 1.5 circumferences (where the other axis
// sits) to 60, past the tube's node cap — to feel where it gives out
const K_MIN = 1.5, K_MAX = 60;
// the panel's sliders, bound in initTunnel
let tunnelControls = [];
const refreshTunnel = () => {
  for (const refresh of tunnelControls) refresh();
  tunnelAxisBtn.textContent = S.tunnelAcross ? 'forward and backward' : 'left to right';
  tunnelLenRow.style.display = S.tunnelAcross ? 'none' : '';
};
// going in, the ground rolls up around you from flat; going out it
// unrolls back to flat first (tunnelClosing, the tick eases curl to 0)
// and the machinery goes off once it's there. animate: false skips both —
// a reload lands straight in the tunnel it left
let tunnelClosing = false;
let curlTarget = 1; // 0 only while the tunnel unrolls on its way out
const tunnelHooks = {
  enter: ({ animate = true }) => {
    view.setTubeBody(false); // the tunnel is always the cylinder
    view.setTunnelAcross(S.tunnelAcross);
    view.setTunnelLag(S.tunnelLag * 1000);
    view.setTunnelLength(S.tunnelK);
    view.setTunnel(true, S.tunnelC);
    S.curl = animate ? 0 : 1;
    curlTarget = 1;
    view.setTubeCurl(S.curl);
    S.following = true;
  },
  exit: () => {
    tunnelClosing = false;
    curlTarget = 1;
    view.setTunnel(false, S.tunnelC);
    view.setTubeEnabled(false);
    document.body.classList.remove('tunnel');
  },
};
// the end of the way out, once the frame loop has eased the curl down
const endTunnel = () => setMode('fly');
const setTunnel = (on, animate = true) => {
  if (on) {
    if (mode === 'tunnel' && !tunnelClosing) return;
    if (tunnelClosing) {
      // caught on the way out: back up from wherever it got to
      tunnelClosing = false;
      curlTarget = 1;
    } else {
      setMode('tunnel', { animate });
    }
  } else {
    if (mode !== 'tunnel' || tunnelClosing) return;
    if (animate) {
      tunnelClosing = true;
      curlTarget = 0;
    } else {
      endTunnel();
    }
  }
  document.body.classList.toggle('tunnel', on);
  refreshTunnel();
  updateHint();
  saveView();
  canvas.focus();
};
// the tunnel's part of the saved view: its settings, and whether it was on
// (not if it was on its way out)
const tunnelState = () => ({ on: mode === 'tunnel' && !tunnelClosing, c: S.tunnelC,
  across: S.tunnelAcross, lag: S.tunnelLag, k: S.tunnelK });
// tubeRolled: the saved view also had the rect rolled, which wins
const restoreTunnel = (t, tubeRolled) => {
  if (t && Number.isFinite(t.c)) S.tunnelC = t.c;
  if (t && t.across === false) S.tunnelAcross = false;
  if (t && Number.isFinite(t.lag)) S.tunnelLag = Math.max(0, t.lag);
  if (t && Number.isFinite(t.k)) S.tunnelK = Math.max(K_MIN, Math.min(K_MAX, t.k));
  refreshTunnel();
  if (t && t.on && !tubeRolled) setTunnel(true, false);
};
const initTunnel = () => {
  defineMode('tunnel', tunnelHooks);

  tunnelAxisBtn.addEventListener('click', () => {
    S.tunnelAcross = !S.tunnelAcross;
    view.setTunnelAcross(S.tunnelAcross);
    refreshTunnel();
    saveView();
    canvas.focus();
  });

  // each slider hands its own setting to the engine
  const applied = (send) => () => { send(); refreshTunnel(); saveView(); };
  tunnelControls = [
    bindRange('tunnelc', S, 'tunnelC', { scale: logScale(200, 20000),
      apply: applied(() => view.setTunnelCircumference(S.tunnelC)),
      fmt: (c) => `${fmtM(c)} around`, amount: 'tunnelamount' }),
    // zero at its left end, then a tenth of a second to ten
    bindRange('tunnellag', S, 'tunnelLag', { scale: logScale(0.1, 10, { off: true }),
      apply: applied(() => view.setTunnelLag(S.tunnelLag * 1000)),
      fmt: (t) => t > 0 ? `${t.toFixed(t < 1 ? 2 : 1)} s to turn` : 'turns at once' }),
    bindRange('tunnellen', S, 'tunnelK', { scale: logScale(K_MIN, K_MAX),
      apply: applied(() => view.setTunnelLength(S.tunnelK)),
      fmt: (k) => `${fmtM(k * S.tunnelC)} ahead · ${k.toFixed(k < 10 ? 1 : 0)}×` }),
  ];
  document.getElementById('tunneloff').addEventListener('click', () => setTunnel(false));
};
