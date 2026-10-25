import { view, canvas } from './engine.js';
import { saveView } from './saveview.js';
import { logScale, bindRange } from './ui.js';

export { showCut, toggleCutPanel, initNearCut };

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
