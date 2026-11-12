import { QS, START } from './config.js';
import { view, canvas, rightEl } from './engine.js';
import { D2R, ecefOf, poseBasis, vsub, vdot } from './math.js';
import { mode, ortho, saveView } from './view.js';
import { updateHint, bindRange, percent } from './hud.js';

export { poisOn, setPois, updatePois, poiSettings, poiState, initPois };

// --- wikipedia places (J) ------------------------------------------------
// en.wikipedia's search with nearcoord: ranks the geotagged articles in a
// radius by popularity, so each ring asks only for its top n: the far rings
// bring just what is famous enough to be worth a flag from far away, the
// nearest one nearly everything around. prominence is langlinkscount, the
// other-language editions with an article (seattle 159, tacoma 89, rainier
// 67, a park 2)
const RINGS = [{ km: 0.8, n: 100 }, { km: 3, n: 60 }, { km: 12, n: 80 }, { km: 50, n: 100 },
  { km: 200, n: 100 }];
// not places, or places whose coordinate is a centroid hundreds of km across
const NOT_PLACES = ['Infobox company', 'Infobox event', 'Infobox civilian attack',
  'Infobox legislature', 'Infobox organization', 'Infobox U.S. county', 'Infobox U.S. state',
  'Infobox school district', 'Infobox radio station', 'Infobox television station',
  'Infobox television season', 'Infobox television', 'Infobox newspaper',
  'Infobox government agency', 'Infobox court']
  .map((t) => `-hastemplate:"${t}"`).join(' ');
// a range's coordinate is its highest peak, which then loses its own flag to
// it. the rest is what no longer stands: search can't ask whether a thing
// still exists, but its short description usually says ("stadium in
// seattle (1976–2000)", "former army post")
const NOT_PLACE_DESC = /^mountain range|\(\d{3,4}\s*[–-]\s*\d{2,4}\)|\b(former|defunct|demolished|destroyed)\b/i;
const MAX_DIM = 30000; // m, the {{coord}} dim: bigger is a region, not a spot
const MAX_POLE = 150; // m: the pole tested for occlusion, however tall it's drawn
// behind something near (a building, round the corner) a place is still
// worth knowing about, so it's drawn faint at a lower score; farther off,
// behind a ridge or the horizon, it's gone
const NEAR_BEHIND = 3000; // m
const BEHIND_PENALTY = 1.2;
const STEM = 18; // px of flagpole
const DROP_KM = 400;

let poisOn = false;
// alpha: how fast a place's claim fades with distance (the nearby..famous
// dial). density: how close labels may pack, and how many
const settings = { alpha: 0.7, density: 0.7 };
const pois = new Map(); // pageid -> place
const ringCell = RINGS.map(() => null); // the grid cell each ring last fetched
let fetching = false, retryAt = 0;

const labelOf = (title) => title.replace(/ \(.*\)$/, '').replace(/,.*$/, '');

// the query centre snaps to a grid a quarter ring across, so a ring
// refetches only once the view has moved that far, and a revisit is a
// repeat url the browser has cached (the api's maxage)
const cellFor = (ring, lat, lon) => {
  const dLat = ring.km / 4 / 111.32;
  const dLon = dLat / Math.max(0.05, Math.cos(lat * D2R));
  const i = Math.round(lat / dLat), j = Math.round(lon / dLon);
  return { key: `${i},${j}`, lat: +(i * dLat).toFixed(5), lon: +(j * dLon).toFixed(5) };
};

const fetchRing = async (ring, cell) => {
  const r = ring.km < 1 ? `${Math.round(ring.km * 1000)}m` : `${ring.km}km`;
  const q = new URLSearchParams({
    action: 'query', format: 'json', formatversion: 2, origin: '*', maxage: 86400, smaxage: 86400,
    generator: 'search', gsrsearch: `nearcoord:${r},${cell.lat},${cell.lon} ${NOT_PLACES}`,
    gsrlimit: ring.n, gsrprop: '', gsrinfo: '',
    prop: 'coordinates|langlinkscount|description', coprop: 'type|dim', colimit: 'max',
  });
  const resp = await fetch('https://en.wikipedia.org/w/api.php?' + q);
  if (!resp.ok) throw new Error(`wikipedia ${resp.status}`);
  const pages = ((await resp.json()).query || {}).pages || [];
  for (const pg of pages) {
    const c = pg.coordinates && pg.coordinates[0];
    if (!c || pois.has(pg.pageid)) continue;
    if (c.dim > MAX_DIM || c.type === 'country' || c.type === 'adm1st' || c.type === 'adm2nd') continue;
    if (NOT_PLACE_DESC.test(pg.description || '')) continue;
    pois.set(pg.pageid, { title: pg.title, label: labelOf(pg.title), desc: pg.description || '',
      lat: c.lat, lon: c.lon, links: pg.langlinkscount || 0,
      ground: NaN, probedAt: -1e9, behind: null, testedAt: -1e9, el: null });
  }
};

// one request at a time, nearest ring first
const maybeFetch = (at, now) => {
  if (fetching || now < retryAt) return;
  const i = RINGS.findIndex((r, k) => ringCell[k] !== cellFor(r, at.lat, at.lon).key);
  if (i < 0) return;
  const cell = cellFor(RINGS[i], at.lat, at.lon);
  fetching = true;
  fetchRing(RINGS[i], cell).then(() => {
    ringCell[i] = cell.key;
    const cosLat = Math.cos(at.lat * D2R);
    for (const [id, poi] of pois) {
      const km = Math.hypot(poi.lat - at.lat, (poi.lon - at.lon) * cosLat) * 111.32;
      if (km > DROP_KM) { if (poi.el) poi.el.remove(); pois.delete(id); }
    }
  }, (e) => {
    console.warn('pois:', e);
    retryAt = now + 10000;
  }).finally(() => { fetching = false; });
};

// in ortho the eye is wherever the dolly left it, so the rings follow the
// ground at the screen's centre instead
let centre = null, centreAt = -1e9;
const ringCentre = (p, now) => {
  if (!ortho) return p;
  if (now - centreAt > 500) { centreAt = now; centre = view.pickCenter() || centre; }
  return centre || p;
};

// --- placement ------------------------------------------------------------
// every frame: each place on screen gets a score, its prominence against its
// distance, and the labels go down greedily in score order, each skipping
// any spot an earlier one took. so the screen fills to about the same
// density everywhere, and in a crowded patch the nearby small places lose
// to the big ones behind them
const layer = document.createElement('div');
layer.id = 'pois';
let shown = new Set();

const labelEl = (poi) => {
  const a = document.createElement('a');
  a.className = 'poi';
  a.href = 'https://en.wikipedia.org/wiki/' + encodeURIComponent(poi.title.replace(/ /g, '_'));
  a.target = '_blank';
  a.rel = 'noopener';
  a.title = poi.desc;
  a.innerHTML = '<i class="pole"></i><span class="flag"></span>';
  a.lastChild.textContent = poi.label;
  layer.appendChild(a);
  return a;
};

const hideAll = () => {
  for (const poi of pois.values()) if (poi.el) poi.el.classList.remove('on');
  shown = new Set();
};

// ecef -> viewport pixels and the depth along the boresight, perspective or
// parallel (ortho_extent is the half-height in metres); null behind the eye
const projector = (b, p, W, H) => {
  const ext = ortho ? view.getOrthoExtent() : 0;
  const t = Math.tan(p.fov * D2R / 2);
  return (P) => {
    const v = vsub(P, b.eye);
    const z = vdot(v, b.dir);
    if (!(z > 1e-6)) return null;
    const rx = vdot(v, b.right), uy = vdot(v, b.vup);
    const sx = ext ? rx / (ext * W / H) : rx / z / (t * W / H);
    const sy = ext ? uy / ext : uy / z / t;
    // in ortho nothing is nearer than anything else: how far a place is
    // from the middle of the view stands in for distance
    const d = ext ? Math.hypot(rx, uy, ext) : Math.hypot(v[0], v[1], v[2]);
    return { x: (sx * 0.5 + 0.5) * W, y: (0.5 - sy * 0.5) * H, z, d,
      mPerPx: ext ? 2 * ext / H : 2 * z * t / H };
  };
};

let countEl = null, lastCount = -1;
const updatePois = (now) => {
  if (!poisOn || !view.ready()) return;
  // the tube's and the tunnel's warps aren't projections
  if (mode !== 'fly' && mode !== 'follow') { hideAll(); return; }
  const p = view.getPose();
  maybeFetch(ringCentre(p, now), now);
  const W = rightEl.clientWidth, H = rightEl.clientHeight;
  const b = poseBasis(p);
  if (!b || !W || !H) return;
  const project = projector(b, p, W, H);

  // ground first: a ray down through the drawn mesh, a few a frame, the
  // longest-waiting first, and again every few seconds since the mesh under
  // a place refines as it loads
  const due = [];
  const cands = [];
  for (const [id, poi] of pois) {
    const known = !Number.isNaN(poi.ground);
    const s = project(ecefOf({ lat: poi.lat, lon: poi.lon, alt: known ? poi.ground : 0 }));
    if (!s || s.x < -W * 0.2 || s.x > W * 1.2 || s.y < -H * 0.2 || s.y > H * 1.2) continue;
    if (now - poi.probedAt > (known ? 3000 : 1000)) due.push(poi);
    if (!known) continue;
    const score = Math.log(2 + poi.links) - settings.alpha * Math.log(Math.max(s.d, 300) / 1000)
      + (shown.has(id) ? 0.4 : 0); // a shown label holds its spot
    cands.push({ id, poi, s, score });
  }
  due.sort((a, c) => a.probedAt - c.probedAt);
  for (const poi of due.slice(0, 8)) {
    poi.probedAt = now;
    const g = view.groundAlt(poi.lat, poi.lon);
    // an early probe can come back kilometres underground, off the bottom
    // of the screen, where it would never be probed again
    if (g > -500) poi.ground = g;
  }

  // hidden or faint when the top of its pole is behind the mesh: the drawn
  // pole, but no taller than MAX_POLE, or a far one (km tall at 200 km)
  // clears every ridge. a ray each, so a few a frame, the longest-waiting
  // first; untested ones wait
  const stale = cands.filter((c) => now - c.poi.testedAt > 300)
    .sort((a, c) => a.poi.testedAt - c.poi.testedAt);
  for (const c of stale.slice(0, 12)) {
    c.poi.testedAt = now;
    c.poi.behind = view.occluded(c.poi.lat, c.poi.lon,
      c.poi.ground + Math.min(MAX_POLE, STEM * c.s.mPerPx));
  }
  for (const c of cands) if (c.poi.behind) c.score -= BEHIND_PENALTY;
  cands.sort((a, c) => c.score - a.score);

  // a label's whole footprint, pole included, so no pole runs through a flag
  const pad = Math.round(30 * (1 - settings.density));
  const maxShown = Math.round(15 + 100 * settings.density);
  const boxes = [];
  const next = new Set();
  for (const c of cands) {
    if (next.size >= maxShown) break;
    const { poi, s } = c;
    if (poi.behind === null || (poi.behind && s.d > NEAR_BEHIND)) continue;
    const px = Math.round(11 + Math.min(4, Math.max(0, c.score - 1)));
    const w = poi.label.length * px * 0.56 + 10, h = px + 6;
    const box = [s.x - 2 - pad, s.y - STEM - h - pad, s.x + w + pad, s.y + pad];
    if (box[0] + pad < 0 || box[2] - pad > W || box[1] + pad < 0 || box[3] - pad > H) continue;
    if (boxes.some((o) => box[0] < o[2] && box[2] > o[0] && box[1] < o[3] && box[3] > o[1])) continue;
    boxes.push(box);
    next.add(c.id);
    const el = poi.el || (poi.el = labelEl(poi));
    el.style.transform = `translate(${s.x.toFixed(1)}px, ${s.y.toFixed(1)}px)`;
    el.style.fontSize = px + 'px';
    el.style.zIndex = Math.round(c.score * 10) + 100; // nearer the front when it's worth more
    el.classList.add('on');
    el.classList.toggle('behind', !!poi.behind);
  }
  for (const id of shown) if (!next.has(id)) { const poi = pois.get(id); if (poi && poi.el) poi.el.classList.remove('on'); }
  shown = next;
  if (shown.size !== lastCount) { lastCount = shown.size; countEl.textContent = lastCount; }
};

const setPois = (on) => {
  poisOn = on;
  document.body.classList.toggle('pois', on);
  if (!on) hideAll();
  updateHint();
  saveView();
};

// what the saved view keeps
const poiSettings = () => ({ on: poisOn, ...settings });

// for __demo and the tools
const poiState = (find) => ({
  on: poisOn, loaded: pois.size, rings: ringCell.slice(), ...settings,
  shown: [...shown].map((id) => { const p = pois.get(id); return { label: p.label, links: p.links }; }),
  // the places whose title matches, everything known about them
  ...(find ? { found: [...pois.values()].filter((p) => find.test(p.title))
    .map(({ el, ...p }) => ({ ...p, on: !!el && el.classList.contains('on') })) } : {}),
});

const initPois = () => {
  rightEl.appendChild(layer);
  countEl = document.getElementById('poicount');
  const saved = START.pois || {};
  if (Number.isFinite(saved.alpha)) settings.alpha = saved.alpha;
  if (Number.isFinite(saved.density)) settings.density = saved.density;
  const refresh = [
    // left nearby (distance counts for a lot), right famous (it barely does)
    bindRange('poialpha', settings, 'alpha', { apply: saveView,
      scale: { from: (v) => 1.6 - 1.5 * v / 100, to: (a) => Math.round((1.6 - a) / 1.5 * 100) } }),
    bindRange('poidensity', settings, 'density', { apply: saveView, scale: percent }),
  ];
  for (const f of refresh) f();
  document.getElementById('poioff').addEventListener('click', () => { setPois(false); canvas.focus(); });
  if (saved.on || QS.get('pois') === '1') setPois(true);
};
