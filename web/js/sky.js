import { QS, TEST, START } from './config.js';
import { sunPosition } from './sun.js';
import { view, canvas } from './engine.js';
import { saveView } from './saveview.js';
import { logScale, percent, bindRange, bindCheck, gate } from './ui.js';

export { sky, glamour, applyGlamour, applySky, initSky };

// --- the sky ------------------------------------------------------------
// where the sun is at the camera, from the clock and the place (noaa's
// low-precision solar position, good to a fraction of a degree). the
// engine's sky and aerial perspective follow it. ?sun=az,el pins it, as
// captures need (test mode pins it by default — a golden can't depend on
// the time of day); ?sky=0 turns the atmosphere off; ?vis=km sets the
// visibility. __demo.sky({ hour: 19 }) and friends from the console
// the atmosphere's state: the three passes, the visibility (km), and the
// sun's hour — local solar time at the camera, or null to follow the
// clock. a fixed afternoon by default: a place is not always wanted at
// the hour it happens to be there. part of the saved view
const SKY_DEFAULT = { sky: true, haze: true, tint: true, vis: 60, hour: 14, shadows: true, shade: 0.45, occl: false, occlAmt: 0.6 };
let sky = { ...SKY_DEFAULT, ...(START.sky || {}) };
if (QS.get('sky') === '0') sky = { ...sky, sky: false, haze: false, tint: false };
if (QS.has('vis')) sky.vis = parseFloat(QS.get('vis')) || sky.vis;
if (QS.has('shadows')) sky.shadows = QS.get('shadows') !== '0';
if (QS.has('occl')) sky.occl = QS.get('occl') !== '0';
const sunPinned = QS.has('sun') ? QS.get('sun').split(',').map(Number)
  : TEST ? [225, 35] : null;
const updateSun = () => {
  if (!view.ready()) return;
  if (sunPinned) { view.setSun(sunPinned[0], sunPinned[1]); return; }
  const p = view.getPose();
  let date = new Date();
  if (sky.hour != null) {
    // today, at that solar hour here: utc is solar time less the
    // longitude's share of the day
    date = new Date(Date.UTC(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate())
      + (sky.hour - p.lon / 15) * 3600000);
  }
  const s = sunPosition(date, p.lat, p.lon);
  view.setSun(s.az, s.el);
};
// glamour mode: while the camera sits still the engine averages frames
// drawn through sub-pixel offsets (supersampling) and, with an aperture,
// from points across a lens (thin-lens depth of field, focused on the
// crosshair). it converges in a second or two and starts over on any
// move. part of the saved view; ?glamour=1&aperture=m from the url
const GLAMOUR_DEFAULT = { on: false, aperture: 0, samples: 256, soft: 0.27 };
let glamour = { ...GLAMOUR_DEFAULT, ...(START.glamour || {}) };
if (QS.has('glamour')) glamour.on = QS.get('glamour') !== '0';
if (QS.has('aperture')) glamour.aperture = parseFloat(QS.get('aperture')) || 0;
const $ = (id) => document.getElementById(id);
// local solar time at the camera now, in hours: what the time slider
// shows while following the clock
const solarHourNow = () => {
  const p = view.getPose(), d = new Date();
  const utc = d.getUTCHours() + d.getUTCMinutes() / 60;
  return ((utc + p.lon / 15) % 24 + 24) % 24;
};
const hhmm = (h) => {
  const m = Math.round(h * 60) % 1440;
  return `${String(Math.floor(m / 60)).padStart(2, '0')}:${String(m % 60).padStart(2, '0')}`;
};
const pct = (x) => `${Math.round(x * 100)}%`;

// the glamour panel's controls, bound in initSky
let glamourControls = [];
const applyGlamour = () => {
  view.setGlamour(glamour.on, glamour.aperture, glamour.samples, glamour.soft);
  for (const refresh of glamourControls) refresh();
  gateGlamourRows();
  saveView();
};
// the sliders only act while accumulating, and soft sun only with
// shadows on (the sky panel's switch)
const gateGlamourRows = () => {
  gate(glamour.on, $('aperturerow'));
  gate(glamour.on && sky.shadows, $('softrow'));
};

let skyControls = [];
// the time slider shows the clock's hour, locked, while following it
const refreshSunTime = () => {
  const following = sky.hour == null;
  const h = following ? (view.ready() ? solarHourNow() : 12) : sky.hour;
  $('suntime').value = Math.round(h * 60);
  gate(!following, $('suntime'));
  $('sunamount').textContent = hhmm(h);
  $('sunclock').checked = following;
};
const applySky = () => {
  view.setAtmosphere(sky.sky, sky.haze, sky.tint, sky.vis);
  view.setShadows(sky.shadows, sky.shade);
  // the slider's 0..1 is up to three times the occlusion's natural weight
  view.setOcclusion(sky.occl, sky.occlAmt * 3);
  for (const refresh of skyControls) refresh();
  // a switch stands in for its slider's name, and an off one locks it
  gate(sky.haze, $('vis'), $('visamount'));
  gate(sky.shadows, $('shade'), $('shadeamount'));
  gate(sky.occl, $('occl'), $('occlamount'));
  refreshSunTime();
  gateGlamourRows();
  updateSun();
  saveView();
};
const initSky = () => {
  if (QS.has('shadowdebug')) view.setShadowDebug(+QS.get('shadowdebug'));
  if (QS.has('aodebug')) view.setAoDebug(QS.get('aodebug') !== '0');

  // the occlusion's resolution (1 full, 2 half) and taps, for comparing
  if (QS.has('occlres') || QS.has('occltaps'))
    view.setOcclusionQuality(+(QS.get('occlres') || 2), +(QS.get('occltaps') || 16));

  glamourControls = [
    bindCheck('glamouron', glamour, 'on', applyGlamour),
    // 0, then ~0.4..100 m of eye travel
    bindRange('aperture', glamour, 'aperture', { apply: applyGlamour,
      scale: logScale(0.390625, 100, { steps: 100, off: true, dp: 2 }), fmt: (m) => m ? `${m} m` : 'off' }),
    // 0..2.7 degrees of sun radius, the real one a tenth of the way along
    bindRange('soft', glamour, 'soft', { apply: applyGlamour,
      scale: { from: (v) => Math.round(v * 2.7) / 100, to: (d) => Math.round(d / 2.7 * 100) },
      fmt: (d) => `${d.toFixed(2)}°` }),
  ];
  $('glamourclose').addEventListener('click', () => { document.body.classList.remove('glamourpanel'); canvas.focus(); });
  setInterval(() => {
    if (!view.ready() || !document.body.classList.contains('glamourpanel')) return;
    const st = view.getStats();
    document.getElementById('glamoursamples').textContent = glamour.on ? `${st.accumSamples}/${st.accumMax}` : '';
  }, 150);

  skyControls = [
    bindCheck('skyon', sky, 'sky', applySky),
    bindCheck('tinton', sky, 'tint', applySky),
    bindCheck('hazeon', sky, 'haze', applySky),
    // 5..320 km
    bindRange('vis', sky, 'vis', { apply: applySky, scale: logScale(5, 320, { steps: 100, dp: 0 }),
      fmt: (km) => `${km} km` }),
    bindCheck('shadowson', sky, 'shadows', applySky),
    bindRange('shade', sky, 'shade', { apply: applySky, scale: percent, fmt: pct }),
    bindCheck('occlon', sky, 'occl', applySky),
    bindRange('occl', sky, 'occlAmt', { apply: applySky, scale: percent, fmt: pct }),
  ];
  $('suntime').addEventListener('input', (e) => { sky.hour = +e.target.value / 60; applySky(); });
  // off the clock: the slider stays where the clock had put it
  $('sunclock').addEventListener('change', (e) => {
    sky.hour = e.target.checked ? null : (view.ready() ? solarHourNow() : 14);
    applySky();
  });
  $('skyclose').addEventListener('click', () => { document.body.classList.remove('skypanel'); canvas.focus(); });
  setInterval(() => { updateSun(); if (sky.hour == null) refreshSunTime(); }, 5000);

  // ?lod=2 draws half as fine, which is a quarter of the tiles — the volume
  // of data a view asks for is what decides whether a deep zoom ever finishes,
  // and this is the only lever on it. live from the console too, via __demo
  if (QS.has('lod')) {
    const v = parseFloat(QS.get('lod'));
    if (Number.isFinite(v) && v > 0) view.setLodScale(v);
  }
};
