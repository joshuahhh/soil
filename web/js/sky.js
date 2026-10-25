import { QS, TEST, START } from './config.js';
import { sunPosition } from './sun.js';
import { view, canvas } from './engine.js';
import { saveView } from './saveview.js';

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
const apertureFromSlider = (v) => v === 0 ? 0 : Math.round(100 * Math.pow(2, (v - 100) / 100 * 8)) / 100; // 0, then ~0.4..100 m, log
const sliderFromAperture = (m) => m <= 0 ? 0 : Math.round(100 + Math.log2(m / 100) / 8 * 100);
const applyGlamour = () => {
  view.setGlamour(glamour.on, glamour.aperture, glamour.samples, glamour.soft);
  document.getElementById('soft').value = Math.round(glamour.soft / 2.7 * 100);
  document.getElementById('softamount').textContent = `${glamour.soft.toFixed(2)}°`;
  document.getElementById('glamouron').checked = glamour.on;
  document.getElementById('aperture').value = sliderFromAperture(glamour.aperture);
  document.getElementById('apertureamount').textContent = glamour.aperture ? `${glamour.aperture} m` : 'off';
  gateGlamourRows();
  saveView();
};
// the sliders only act while accumulating, and soft sun only with
// shadows on (the sky panel's switch): locked and dimmed otherwise
const gateGlamourRows = () => {
  const gate = (row, input, on) => {
    document.getElementById(row).classList.toggle('off', !on);
    document.getElementById(input).disabled = !on;
  };
  gate('aperturerow', 'aperture', glamour.on);
  gate('softrow', 'soft', glamour.on && sky.shadows);
};

const skyEl = (id) => document.getElementById(id);
const visFromSlider = (v) => Math.round(5 * Math.pow(2, v / 100 * 6)); // 5..320 km, log
const sliderFromVis = (km) => Math.round(Math.log2(km / 5) / 6 * 100);
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
const refreshSunTime = () => {
  const following = sky.hour == null;
  const h = following ? (view.ready() ? solarHourNow() : 12) : sky.hour;
  skyEl('suntime').value = Math.round(h * 60);
  skyEl('suntime').disabled = following;
  skyEl('suntime').classList.toggle('off', following);
  skyEl('sunamount').textContent = hhmm(h);
  skyEl('sunclock').checked = following;
};
const applySky = () => {
  view.setAtmosphere(sky.sky, sky.haze, sky.tint, sky.vis);
  view.setShadows(sky.shadows, sky.shade);
  // the slider's 0..1 is up to three times the occlusion's natural weight
  view.setOcclusion(sky.occl, sky.occlAmt * 3);
  skyEl('occlon').checked = sky.occl;
  skyEl('occl').value = Math.round(sky.occlAmt * 100);
  skyEl('occlamount').textContent = `${Math.round(sky.occlAmt * 100)}%`;
  skyEl('skyon').checked = sky.sky;
  skyEl('tinton').checked = sky.tint;
  skyEl('hazeon').checked = sky.haze;
  skyEl('vis').value = sliderFromVis(sky.vis);
  skyEl('visamount').textContent = `${sky.vis} km`;
  skyEl('shadowson').checked = sky.shadows;
  skyEl('shade').value = Math.round(sky.shade * 100);
  skyEl('shadeamount').textContent = `${Math.round(sky.shade * 100)}%`;
  // a slider whose switch is off: dimmed and locked, value kept
  for (const [id, on] of [['vis', sky.haze], ['visamount', sky.haze], ['shade', sky.shadows], ['shadeamount', sky.shadows],
    ['occl', sky.occl], ['occlamount', sky.occl]]) {
    skyEl(id).classList.toggle('off', !on);
    if (skyEl(id).tagName === 'INPUT') skyEl(id).disabled = !on;
  }
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

  document.getElementById('glamouron').addEventListener('change', (e) => { glamour.on = e.target.checked; applyGlamour(); });
  document.getElementById('aperture').addEventListener('input', (e) => { glamour.aperture = apertureFromSlider(+e.target.value); applyGlamour(); });
  // 0..2.7 degrees of sun radius, the real one a tenth of the way along
  document.getElementById('soft').addEventListener('input', (e) => { glamour.soft = Math.round(+e.target.value * 2.7) / 100; applyGlamour(); });
  document.getElementById('glamourclose').addEventListener('click', () => { document.body.classList.remove('glamourpanel'); canvas.focus(); });
  setInterval(() => {
    if (!view.ready() || !document.body.classList.contains('glamourpanel')) return;
    const st = view.getStats();
    document.getElementById('glamoursamples').textContent = glamour.on ? `${st.accumSamples}/${st.accumMax}` : '';
  }, 150);

  skyEl('shade').addEventListener('input', (e) => { sky.shade = +e.target.value / 100; applySky(); });
  skyEl('occl').addEventListener('input', (e) => { sky.occlAmt = +e.target.value / 100; applySky(); });
  for (const [id, key] of [['skyon', 'sky'], ['hazeon', 'haze'], ['tinton', 'tint'], ['shadowson', 'shadows'], ['occlon', 'occl']])
    skyEl(id).addEventListener('change', (e) => { sky[key] = e.target.checked; applySky(); });
  skyEl('vis').addEventListener('input', (e) => { sky.vis = visFromSlider(+e.target.value); applySky(); });
  skyEl('suntime').addEventListener('input', (e) => { sky.hour = +e.target.value / 60; applySky(); });
  // off the clock: the slider stays where the clock had put it
  skyEl('sunclock').addEventListener('change', (e) => {
    sky.hour = e.target.checked ? null : (view.ready() ? solarHourNow() : 14);
    applySky();
  });
  skyEl('skyclose').addEventListener('click', () => { document.body.classList.remove('skypanel'); canvas.focus(); });
  setInterval(() => { updateSun(); if (sky.hour == null) refreshSunTime(); }, 5000);

  // ?lod=2 draws half as fine, which is a quarter of the tiles — the volume
  // of data a view asks for is what decides whether a deep zoom ever finishes,
  // and this is the only lever on it. live from the console too, via __demo
  if (QS.has('lod')) {
    const v = parseFloat(QS.get('lod'));
    if (Number.isFinite(v) && v > 0) view.setLodScale(v);
  }
};
