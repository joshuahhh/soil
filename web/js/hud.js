import { S } from './state.js';
import { view, canvas } from './engine.js';
import { saveView } from './saveview.js';
import { miniature, mipmaps } from './input.js';
import { mode, inTube, viewName, ortho, hug } from './mode.js';
import { tubeRect } from './tube.js';
import { alignRefresh } from './align.js';
import { refreshKeys } from './keytable.js';

export { updateSpeed, updateHint, toast, refreshRoll, initHud, initPanels, initRoll };

// --- the hud ---------------------------------------------------------------
// the status chips, the panels' heads, the toast and the roll readout

// the hint line names the mode the view is in and any switch that is
// not at its default; the keys themselves are in the ? table
const HINTS = {
  persp: '',
  ortho: 'orthographic',
  follow: 'following the path',
  tube: 'in the tube',
  body: 'on the body',
  tunnel: '', // its panel says so
};
// the speed chip, and the hug chip with the height it is holding — which
// moves with R/F, so the frame loop refreshes this while hugging
const updateSpeed = () => {
  document.getElementById('speed').textContent =
    `speed ${S.gain >= 1 ? S.gain : '1/' + (1/S.gain)}`;
  const h = hug ? view.getFollowHeight() : NaN;
  const height = Number.isFinite(h) ? ` at ${h < 10 ? h.toFixed(1) : Math.round(h)} m` : '';
  document.getElementById('hug').textContent =
    hug && inTube() ? `hugging the wall${height}`
      : hug && !ortho ? `hugging terrain${height}` : '';
};
const updateHint = () => {
  document.getElementById('hint').textContent = [
    HINTS[viewName()],
    mipmaps ? '' : 'mipmaps off',
    miniature ? 'miniature' : '',
    // T and B only do anything once a rect exists
    !inTube() && tubeRect ? 'rect drawn (T rolls it into a tube, B wraps it on a body)' : '',
  ].filter(Boolean).join(' · ');
  updateSpeed();
  refreshKeys();
  // point-pair alignment is perspective-only, so every mode switch has to
  // re-decide whether its panel and markers belong on screen
  alignRefresh();
};

let toastTimer = 0;
const toast = (msg, ms = 2600) => {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.classList.add('on');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.classList.remove('on'), ms);
};

// roll is the one part of the pose with no other sign in the hud, and it can
// arrive from the solver or from banking the tube, so the panel is driven by
// what the camera actually reports rather than by whoever set it. inside the
// tube a bank is the normal way to fly and needs no announcing
const rollAmountEl = document.getElementById('rollamount');
let shownRoll = null;
const refreshRoll = () => {
  const roll = view.ready() ? (view.getPose().roll || 0) : 0;
  const on = !inTube() && mode !== 'tunnel' && Math.abs(roll) > 0.05;
  const shown = on ? Math.round(roll * 10) / 10 : null;
  if (shown === shownRoll) return;
  shownRoll = shown;
  document.body.classList.toggle('rolled', on);
  if (on) rollAmountEl.textContent = `${shown > 0 ? '+' : ''}${shown.toFixed(1)}° from level`;
};
const initHud = () => {
  document.getElementById('hint').textContent = HINTS.persp;
  document.getElementById('speed').textContent = 'speed 1';
};
const initPanels = () => {
  // a panel's head is its minimize switch: click anywhere on it (except its
  // other buttons, like ✕) and the panel shrinks to that line; the –/+ in
  // it says which way the next click goes
  for (const panel of document.querySelectorAll('.panel')) {
    const head = panel.querySelector('.head');
    const btn = document.createElement('button');
    btn.className = 'minbtn';
    btn.textContent = '–';
    head.insertBefore(btn, head.querySelector('button'));
    head.title = 'minimize';
    head.addEventListener('click', (e) => {
      const b = e.target.closest('button');
      if (b && b !== btn) return; // ✕ and friends do their own thing
      const min = panel.classList.toggle('min');
      btn.textContent = min ? '+' : '–';
      head.title = min ? 'expand' : 'minimize';
      canvas.focus();
    });
  }
};
const initRoll = () => {
  document.getElementById('rolllevel').addEventListener('click', () => {
    view.setRoll(0);
    refreshRoll();
    saveView();
    canvas.focus();
  });
};
