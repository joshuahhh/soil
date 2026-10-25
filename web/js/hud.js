import { S } from './config.js';
import { view, canvas } from './engine.js';
import { saveView, miniature, mipmaps, mode, inTube, viewName, ortho, hug } from './view.js';
import { alignRefresh, photoShown } from './photo.js';
import { tubeRect } from './tube.js';
import { path } from './path.js';

export { logScale, percent, bindRange, bindCheck, gate, updateSpeed, updateHint, toast,
  refreshRoll, initHud, keysOpen, showKeys, toggleKeys, showCut, toggleCutPanel };

// --- panel controls ---------------------------------------------------------
// the hud's sliders and switches are each bound to one value: dragging the
// control writes the value and calls the panel's apply, and the panel's
// refresh puts every value back into its control and readout (after a
// restore, a __demo call, the solver moving the lens...). a control's
// binding is one line, so adding one is the markup plus that line

// a slider that is log across min..max, its track 0..steps: the things on
// these sliders are felt in ratios. off: the bottom notch means 0 (off), and
// the scale starts one notch up. dp rounds what the slider sets, so that
// what is saved and shown is a round number
const logScale = (min, max, { steps = 1, off = false, dp = null } = {}) => {
  const k = Math.log(max / min);
  const round = dp === null ? (x) => x : (x) => Math.round(x * 10 ** dp) / 10 ** dp;
  return {
    from: (v) => off && v <= 0 ? 0 : round(min * Math.exp(k * v / steps)),
    to: (x) => off && x <= 0 ? 0 : steps * Math.log(Math.max(min, Math.min(max, x)) / min) / k,
  };
};
// a fraction on a 0..100 track
const percent = { from: (v) => v / 100, to: (x) => Math.round(x * 100) };
const linear = { from: (v) => v, to: (x) => x };

// a range input bound to obj[key] (or to opts.get/opts.set, for a value that
// lives somewhere else). fmt writes the readout, #<id>amount unless amount
// names another. on is the event that commits ('input' while dragging, by
// default). returns the refresh
const bindRange = (id, obj, key, { scale = linear, fmt, amount = id + 'amount', on = 'input',
  get = () => obj[key], set = (v) => { obj[key] = v; }, apply }) => {
  const el = document.getElementById(id), out = fmt && document.getElementById(amount);
  el.addEventListener(on, () => { set(scale.from(+el.value)); apply(); });
  return () => {
    el.value = scale.to(get());
    if (out) out.textContent = fmt(get());
  };
};
const bindCheck = (id, obj, key, apply) => {
  const el = document.getElementById(id);
  el.addEventListener('change', () => { obj[key] = el.checked; apply(); });
  return () => { el.checked = obj[key]; };
};

// what a switch (or another panel's) has turned off is dimmed and locked,
// its value kept: .off on each element, and every input in or among them
// disabled
const gate = (on, ...els) => {
  for (const el of els) {
    el.classList.toggle('off', !on);
    for (const input of el.tagName === 'INPUT' ? [el] : el.querySelectorAll('input'))
      input.disabled = !on;
  }
};

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
document.getElementById('hint').textContent = HINTS.persp;
document.getElementById('speed').textContent = 'speed 1';
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

// --- the key table (?) ----------------------------------------------------
// what every key does *in the current mode* — a key that does nothing here
// isn't listed, and one that does something different (arrows orbit in
// ortho, look around in perspective) says what it does here. rebuilt by
// updateHint on every mode switch
const K = (t) => t.split(' ').map((k) => `<kbd>${k}</kbd>`).join(' ');
const keysFor = () => {
  const mode = viewName();
  const g = [];
  if (mode === 'tunnel') g.push(['flying the tunnel', [
    [K('W A S D'), 'move'], [K('R F'), 'up / down'],
    [K('space') + ' +', 'move in the view frame instead'],
    [K('←↑↓→') + ' drag', 'look around'], [K('Z X') + ' wheel', 'zoom (fov)'],
    [K('- ='), 'speed'], [K('shift') + ' +', 'slow'],
    [K('G'), hug ? 'stop hugging the terrain' : 'hug the terrain'],
    [K('T') + ' ' + K('esc'), 'leave the tunnel']]]);
  if (mode === 'persp') g.push(['flying', [
    [K('W A S D'), 'move'], [K('R F'), 'up / down'],
    [K('space') + ' +', 'move in the view frame instead'],
    [K('←↑↓→') + ' drag', 'look around'], [K('Z X') + ' wheel', 'zoom (fov)'],
    [K('- ='), 'speed'], [K('shift') + ' +', 'slow'],
    [K('G'), hug ? 'stop hugging the terrain' : 'hug the terrain'], [K('0'), 'reset view']]]);
  if (mode === 'ortho') g.push(['moving', [
    [K('W A S D') + ' drag', 'pan'], [K('←↑↓→'), 'orbit around the center'],
    [K('Z X R F') + ' wheel', 'zoom'], [K('- ='), 'speed'], [K('shift') + ' +', 'slow'],
    [K('0'), 'reset view']]]);
  if (mode === 'tube') g.push(['flying the tube', [
    [K('W S'), 'thrust'], [K('A D'), 'strafe'], [K('R F'), 'up / down'],
    [K('←↑↓→') + ' drag', 'pitch + yaw'], [K('Q E'), 'roll'], [K('Z X') + ' wheel', 'zoom'],
    [K('- ='), 'speed'], [K('shift') + ' +', 'slow'],
    [K('G'), hug ? 'stop hugging the wall' : 'hug the wall'], [K('0'), 'reset view'],
    [K('T'), 'unroll the tube'], [K('B'), 'wrap it on the body instead'], [K('esc'), 'clear the tube']]]);
  if (mode === 'body') g.push(['flying around the body', [
    [K('W S'), 'thrust'], [K('A D'), 'strafe'], [K('R F'), 'up / down'],
    [K('←↑↓→') + ' drag', 'pitch + yaw'], [K('Q E'), 'roll'], [K('Z X') + ' wheel', 'zoom'],
    [K('- ='), 'speed'], [K('shift') + ' +', 'slow'], [K('0'), 'reset view'],
    [K('B'), 'unwrap'], [K('T'), 'roll it into a tube instead'], [K('esc'), 'clear the rect']]]);
  if (mode === 'follow') g.push(['following the path', [
    [K('W S'), 'run along it'], [K('←↑↓→'), 'swing the view around it'],
    [K('R F'), ortho ? 'zoom' : 'standoff'], [K('Z X'), 'zoom'], [K('shift') + ' +', 'slow'],
    [K('V') + ' ' + K('esc'), 'stop following']]]);
  const viewRows = [];
  if (mode !== 'tube' && mode !== 'body' && mode !== 'tunnel') viewRows.push([K('O'), ortho ? 'perspective' : 'orthographic']);
  if (mode === 'persp' && !tubeRect) viewRows.push([K('T'), 'tunnel']);
  viewRows.push([K('N'), miniature ? 'miniature off' : 'miniature (tilt-shift)']);
  if (!ortho) viewRows.push([K('H'), 'near cut']);
  viewRows.push([K('Y'), 'sky: time of day, haze, shadows']);
  viewRows.push([K('I'), 'glamour: refine a still frame']);
  viewRows.push([K('C L K M'), 'debug: pick · lod · smoothing · mipmaps']);
  g.push(['view', viewRows]);
  if (photoShown) {
    const rows = [[K('P'), 'photo on / off'], [K('shift') + ' drag', 'move the photo'],
      [K('shift') + ' wheel', 'size it about the pointer']];
    if (mode === 'persp') rows.push(['drag', 'either end of a pair, to adjust it'],
      [K('esc'), 'cancel adding a pair']);
    g.push(['photo', rows]);
  }
  const mapRows = [['click', 'teleport there'],
    [K('shift') + ' drag', tubeRect ? 'draw a new tube rect' : 'draw a tube rect']];
  if (mode !== 'tube' && tubeRect) mapRows.push([K('T'), 'roll the tube up']);
  if (mode !== 'body' && tubeRect) mapRows.push([K('B'), 'wrap it on a body']);
  if (mode !== 'tube' && mode !== 'body' && tubeRect) mapRows.push([K('esc'), 'clear the rect']);
  if (mode !== 'follow' && mode !== 'tube' && mode !== 'body' && path)
    mapRows.push([K('V'), 'follow the path']);
  g.push(['map', mapRows]);
  g.push(['session', [[K('⌘C ⌘V'), 'copy / paste the whole session'],
    [K('⌘S'), 'save it as a file'],
    ['drop a file', 'image → photo · .gpx → path · .obj → body · session .json → restore'], [K('?'), 'this']]]);
  return g;
};
// the table is a pure function of the view's state. it is re-derived,
// while it is open, whenever updateHint runs — which is whenever the mode,
// a view switch, the photo, the track or the rect changes — and only
// touches the dom when the result differs
let keysOpen = false, keysHtml = '';
const renderKeys = () => {
  const html = keysFor().map(([title, rows]) =>
    `<section><h4>${title}</h4><table>${rows.map(([k, what]) =>
      `<tr><td>${k}</td><td>${what}</td></tr>`).join('')}</table></section>`).join('');
  if (html === keysHtml) return;
  keysHtml = html;
  document.getElementById('keys').innerHTML = html;
};
const refreshKeys = () => { if (keysOpen) renderKeys(); };
const showKeys = (on) => {
  keysOpen = on;
  if (on) renderKeys();
  document.body.classList.toggle('keys', on);
};
const toggleKeys = () => {
  showKeys(!keysOpen);
  canvas.focus();
};
const initKeyTable = () => {
  document.getElementById('keysbtn').addEventListener('click', toggleKeys);
};

// --- near cut (H) ---------------------------------------------------------
// the slider is log across 0.5m..2km, with the bottom notch meaning off: a
// window is a metre or two away and a courtyard wall fifty, and both need
// to be settable on the same track
const CUT = logScale(0.5, 2000, { steps: 1000, off: true });
let cutShown = false;
let refreshCut = () => {}; // bound in initNearCut
const showCut = () => {
  refreshCut();
  cutShown = true;
  document.body.classList.add('cut');
};
const hideCut = () => {
  cutShown = false;
  document.body.classList.remove('cut');
};
// H shows the panel (with whatever cut is set); H again hides the panel
// but leaves the cut alone — ✕ is what turns it off
const toggleCutPanel = () => cutShown ? hideCut() : showCut();
const initNearCut = () => {
  refreshCut = bindRange('cutdist', null, null, { scale: CUT, amount: 'cutamount',
    get: () => view.getNearCut(), set: (m) => view.setNearCut(m), apply: showCut,
    fmt: (m) => m <= 0 ? 'off' : m < 10 ? m.toFixed(1) + ' m' : Math.round(m) + ' m' });
  document.getElementById('cutdist').addEventListener('change', saveView);
  document.getElementById('cutoff').addEventListener('click', () => {
    view.setNearCut(0);
    hideCut();
    saveView();
    canvas.focus();
  });
};

const initHud = () => {
  initPanels();
  initKeyTable();
  initNearCut();
  initRoll();
};
