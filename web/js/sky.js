import { QS, TEST, START } from './config.js';
import { sunPosition } from './math.js';
import { view, canvas } from './engine.js';
import { saveView } from './view.js';
import { logScale, percent, bindRange, bindCheck, gate } from './hud.js';

export { sky, glamour, applyGlamour, applySky, initSky };

// --- the sky ------------------------------------------------------------
// hour: the clock at the camera, in its own time zone, or null to follow
// the real one. a fixed afternoon by default, not the hour it happens to be
// there. day: of the year there (1 = jan 1), or null for today
const SKY_DEFAULT = { sky: true, haze: true, tint: true, vis: 60, hour: 14, day: null, shadows: true, shade: 0.45, occl: false, occlAmt: 0.6 };
let sky = { ...SKY_DEFAULT, ...(START.sky || {}) };
if (QS.get('sky') === '0') sky = { ...sky, sky: false, haze: false, tint: false };
if (QS.has('vis')) sky.vis = parseFloat(QS.get('vis')) || sky.vis;
if (QS.has('shadows')) sky.shadows = QS.get('shadows') !== '0';
if (QS.has('occl')) sky.occl = QS.get('occl') !== '0';
// test mode pins the sun: a golden can't depend on the time of day
const sunPinned = QS.has('sun') ? QS.get('sun').split(',').map(Number)
  : TEST ? [225, 35] : null;
const updateSun = () => {
  if (!view.ready()) return;
  if (sunPinned) { view.setSun(sunPinned[0], sunPinned[1]); return; }
  const p = view.getPose(), zone = zoneAt(p.lat, p.lon);
  let date = new Date();
  if (sky.hour != null) {
    const today = clockAt(zone, date);
    const local = Date.UTC(today.getUTCFullYear(), 0, sky.day ?? dayOfYear(today)) + sky.hour * 3600000;
    // twice, for a day that crosses a daylight saving change
    let t = local - offsetAt(zone, local);
    t = local - offsetAt(zone, t);
    date = new Date(t);
  }
  const s = sunPosition(date, p.lat, p.lon);
  view.setSun(s.az, s.el);
  const name = `${zone.replaceAll('_', ' ')} · ${zoneParts(zone, date).timeZoneName}`;
  $('sunzone').textContent = $('sunzone').title = name;
  $('sunsolar').textContent = hhmm((12 + s.ha / 15) % 24);
};
// glamour: while still, the engine accumulates supersampled frames and, with
// an aperture, thin-lens depth of field focused on the crosshair
const GLAMOUR_DEFAULT = { on: false, aperture: 0, samples: 256, soft: 0.27 };
let glamour = { ...GLAMOUR_DEFAULT, ...(START.glamour || {}) };
if (QS.has('glamour')) glamour.on = QS.get('glamour') !== '0';
if (QS.has('aperture')) glamour.aperture = parseFloat(QS.get('aperture')) || 0;
const $ = (id) => document.getElementById(id);
// the time zone at a place. until tz-lookup loads, or without the network,
// the nautical one: an hour per 15° of longitude (etc/gmt's sign is reversed)
let tzlookup = null;
import('https://cdn.jsdelivr.net/npm/@photostructure/tz-lookup@11.7.0/+esm').then((m) => {
  tzlookup = m.default;
  if (view?.ready()) { refreshSunTime(); updateSun(); }
}, (e) => console.warn('tz-lookup:', e));
const zoneAt = (lat, lon) => {
  if (tzlookup) try { return tzlookup(lat, lon); } catch { /* off the map */ }
  const n = Math.round(lon / 15);
  return n ? `Etc/GMT${n > 0 ? '-' : '+'}${Math.abs(n)}` : 'Etc/GMT';
};
const zoneFormats = new Map();
const zoneParts = (zone, t) => {
  if (!zoneFormats.has(zone)) zoneFormats.set(zone, new Intl.DateTimeFormat('en-US', { timeZone: zone,
    hourCycle: 'h23', year: 'numeric', month: 'numeric', day: 'numeric', hour: 'numeric', minute: 'numeric', second: 'numeric',
    timeZoneName: 'short' }));
  return Object.fromEntries(zoneFormats.get(zone).formatToParts(t).map((x) => [x.type, x.value]));
};
// a zone's wall clock at an instant, in a date's utc fields
const clockAt = (zone, t) => {
  const f = zoneParts(zone, t);
  return new Date(Date.UTC(f.year, f.month - 1, f.day, f.hour, f.minute, f.second));
};
const offsetAt = (zone, t) => clockAt(zone, t) - Math.floor(t / 1000) * 1000;
const cameraNow = () => {
  const p = view.getPose();
  return clockAt(zoneAt(p.lat, p.lon), new Date());
};
const hourNow = () => { const d = cameraNow(); return d.getUTCHours() + d.getUTCMinutes() / 60; };
const dayOfYear = (d) => Math.floor((d - Date.UTC(d.getUTCFullYear(), 0, 1)) / 86400000) + 1;
const monthDay = (day) => new Date(Date.UTC(new Date().getUTCFullYear(), 0, day))
  .toLocaleDateString('en', { month: 'short', day: 'numeric', timeZone: 'UTC' });
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
// soft sun also needs shadows on (the sky panel's switch)
const gateGlamourRows = () => {
  gate(glamour.on, $('aperturerow'));
  gate(glamour.on && sky.shadows, $('softrow'));
};

let skyControls = [];
// the time and date sliders show the clock's, locked, while following it
const refreshSunTime = () => {
  const following = sky.hour == null;
  const h = following ? (view.ready() ? hourNow() : 12) : sky.hour;
  const day = (!following && sky.day) || (view.ready() ? dayOfYear(cameraNow()) : 1);
  $('suntime').value = Math.round(h * 60);
  $('sundate').value = day;
  gate(!following, $('suntime'), $('sundate'));
  $('sunamount').textContent = hhmm(h);
  $('sundateamount').textContent = monthDay(day);
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
  $('sundate').addEventListener('input', (e) => { sky.day = +e.target.value; applySky(); });
  // off the clock: the sliders stay where the clock had put them
  $('sunclock').addEventListener('change', (e) => {
    sky.hour = e.target.checked ? null : (view.ready() ? hourNow() : 14);
    sky.day = null;
    applySky();
  });
  $('skyclose').addEventListener('click', () => { document.body.classList.remove('skypanel'); canvas.focus(); });
  setInterval(() => { updateSun(); if (sky.hour == null) refreshSunTime(); }, 5000);

  // ?lod=2 draws half as fine, a quarter of the tiles: the only lever on the
  // data volume that decides whether a deep zoom ever finishes
  if (QS.has('lod')) {
    const v = parseFloat(QS.get('lod'));
    if (Number.isFinite(v) && v > 0) view.setLodScale(v);
  }
};
