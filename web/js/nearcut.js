import { view, canvas } from './engine.js';
import { saveView } from './saveview.js';

export { showCut, toggleCutPanel, initNearCut };

// --- near cut (H) ---------------------------------------------------------
// the slider is log across 0.5m..2km, with the bottom notch meaning off: a
// window is a metre or two away and a courtyard wall fifty, and both need
// to be settable on the same track
const cutDist = document.getElementById('cutdist');
const cutAmount = document.getElementById('cutamount');
const CUT_MIN = 0.5, CUT_MAX = 2000;
const cutFromSlider = (v) => v <= 0 ? 0
  : CUT_MIN * Math.pow(CUT_MAX / CUT_MIN, v / 1000);
const sliderFromCut = (m) => m <= 0 ? 0
  : Math.round(1000 * Math.log(m / CUT_MIN) / Math.log(CUT_MAX / CUT_MIN));
const showCut = () => {
  const m = view.getNearCut();
  cutDist.value = sliderFromCut(m);
  cutAmount.textContent = m <= 0 ? 'off' : m < 10 ? m.toFixed(1) + ' m'
    : Math.round(m) + ' m';
  document.body.classList.add('cut');
};
// H shows the panel (with whatever cut is set); H again hides the panel
// but leaves the cut alone — ✕ is what turns it off
const toggleCutPanel = () => {
  if (document.body.classList.contains('cut')) document.body.classList.remove('cut');
  else showCut();
};
const initNearCut = () => {
  cutDist.addEventListener('input', () => {
    view.setNearCut(cutFromSlider(+cutDist.value));
    showCut();
  });
  cutDist.addEventListener('change', saveView);
  document.getElementById('cutoff').addEventListener('click', () => {
    view.setNearCut(0);
    document.body.classList.remove('cut');
    saveView();
    canvas.focus();
  });
};
