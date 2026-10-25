import { S } from './state.js';
import { view, canvas } from './engine.js';
import { saveView } from './saveview.js';
import { updateHint } from './hud.js';
import { mode, defineMode, setMode } from './mode.js';

export { refreshTunnel, tunnelClosing, curlTarget, endTunnel, setTunnel, tunnelState,
  restoreTunnel, initTunnel };

// --- tunnel mode ----------------------------------------------------------
// the tube without the rect: the ground ahead curls up in front of the
// camera, over it and down behind, whichever way it faces — the camera
// on the axis, looking along the circle, new land rolling up ahead as
// you fly (earth_core tunnelUpdate). one parameter, the circumference;
// T toggles it, the panel's slider sets it
const tunnelAxisBtn = document.getElementById('tunnelaxis');
const TUNNEL_MIN = 200, TUNNEL_MAX = 20000;
const tunnelSlider = document.getElementById('tunnelc');
const tunnelAmountEl = document.getElementById('tunnelamount');
// the slider is log across its range: a tunnel is felt in ratios
const tunnelFromSlider = (v) => TUNNEL_MIN * Math.pow(TUNNEL_MAX / TUNNEL_MIN, v);
const tunnelToSlider = (c) => Math.log(c / TUNNEL_MIN) / Math.log(TUNNEL_MAX / TUNNEL_MIN);
const fmtM = (m) => m >= 1000 ? `${(m / 1000).toFixed(m >= 10000 ? 0 : 1)} km` : `${Math.round(m)} m`;
// the lag slider: zero at its left end, then log from a tenth of a
// second to ten
const LAG_MIN = 0.1, LAG_MAX = 10;
const tunnelLagSlider = document.getElementById('tunnellag');
const tunnelLagAmountEl = document.getElementById('tunnellagamount');
const lagFromSlider = (v) => v <= 0 ? 0 : LAG_MIN * Math.pow(LAG_MAX / LAG_MIN, v);
const lagToSlider = (t) => t <= 0 ? 0 : Math.log(Math.max(LAG_MIN, Math.min(LAG_MAX, t)) / LAG_MIN) / Math.log(LAG_MAX / LAG_MIN);
// the length slider: log from 1.5 circumferences (where the other axis
// sits) to 60, past the tube's node cap — to feel where it gives out
const K_MIN = 1.5, K_MAX = 60;
const tunnelLenSlider = document.getElementById('tunnellen');
const tunnelLenAmountEl = document.getElementById('tunnellenamount');
const tunnelLenRow = document.getElementById('tunnellenrow');
const kFromSlider = (v) => K_MIN * Math.pow(K_MAX / K_MIN, v);
const kToSlider = (k) => Math.log(Math.max(K_MIN, Math.min(K_MAX, k)) / K_MIN) / Math.log(K_MAX / K_MIN);
const refreshTunnel = () => {
  tunnelSlider.value = tunnelToSlider(Math.max(TUNNEL_MIN, Math.min(TUNNEL_MAX, S.tunnelC)));
  tunnelAmountEl.textContent = `${fmtM(S.tunnelC)} around`;
  tunnelLagSlider.value = lagToSlider(S.tunnelLag);
  tunnelLagAmountEl.textContent = S.tunnelLag > 0 ? `${S.tunnelLag.toFixed(S.tunnelLag < 1 ? 2 : 1)} s to turn` : 'turns at once';
  tunnelAxisBtn.textContent = S.tunnelAcross ? 'forward and backward' : 'left to right';
  tunnelLenSlider.value = kToSlider(S.tunnelK);
  tunnelLenAmountEl.textContent = `${fmtM(S.tunnelK * S.tunnelC)} ahead · ${S.tunnelK.toFixed(S.tunnelK < 10 ? 1 : 0)}×`;
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

  tunnelSlider.addEventListener('input', () => {
    S.tunnelC = tunnelFromSlider(+tunnelSlider.value);
    view.setTunnelCircumference(S.tunnelC);
    refreshTunnel();
    saveView();
  });
  tunnelLagSlider.addEventListener('input', () => {
    S.tunnelLag = lagFromSlider(+tunnelLagSlider.value);
    view.setTunnelLag(S.tunnelLag * 1000);
    refreshTunnel();
    saveView();
  });
  tunnelLenSlider.addEventListener('input', () => {
    S.tunnelK = kFromSlider(+tunnelLenSlider.value);
    view.setTunnelLength(S.tunnelK);
    refreshTunnel();
    saveView();
  });
  document.getElementById('tunneloff').addEventListener('click', () => setTunnel(false));
};
