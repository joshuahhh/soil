import { START, S } from './config.js';
import { view, canvas, resize } from './engine.js';

export { map, marker, setMarkerHeading, full3d, pickDebug, setPickDebug, updatePickDebug,
  mapPointerDown, followMarker, initMap };

// --- maplibre -------------------------------------------------------------
let map = null, marker = null; // arrive with initMap
const markerEl = document.createElement('div');
markerEl.style.pointerEvents = 'none';
markerEl.innerHTML =
  '<svg id="camIcon" width="44" height="44" viewBox="-22 -22 44 44" style="display:block">' +
  '<path d="M0,-3 L-13,-19 A21,21 0 0 1 13,-19 Z" fill="rgba(66,133,244,.4)"/>' +
  '<circle r="5.5" fill="#4285f4" stroke="#fff" stroke-width="2.5"/></svg>';
const setMarkerHeading = (deg) => {
  const el = document.getElementById('camIcon');
  if (el) el.style.transform = `rotate(${deg}deg)`;
};

// full3d: the divider collapses the map pane; kept in the saved view
const expandBtn = document.getElementById('expandbtn');
const dividerEl = document.getElementById('divider');
let full3d = false;
const setFull3d = (on) => {
  full3d = on;
  document.body.classList.toggle('full3d', on);
  expandBtn.textContent = on ? '▶' : '◀';
  expandBtn.title = on ? 'show map' : 'expand 3d view';
  resize(); // the canvas backing store tracks its new client size
  if (!on) map.resize();
};

// pick debug (C): the orange dot is where the centre raycast hit, the orbit pivot
let pickDebug = false;
const pickEl = document.createElement('div');
pickEl.style.cssText = 'width:12px;height:12px;border-radius:50%;background:#ff7f0e;' +
  'border:2.5px solid #fff;pointer-events:none;';
const pickMarker = new maplibregl.Marker({ element: pickEl });
let pickMarkerOn = false;
const setPickDebug = (on) => {
  pickDebug = on;
  document.getElementById('cross').style.display = on ? '' : 'none';
  document.getElementById('pickinfo').style.display = on ? '' : 'none';
  if (!on && pickMarkerOn) { pickMarker.remove(); pickMarkerOn = false; }
};
const updatePickDebug = () => {
  const pick = view.pickCenter();
  if (pick) {
    // position before addTo: a marker on the map without a position throws
    // from maplibre's own update events (e.g. when the style loads)
    pickMarker.setLngLat([pick.lon, pick.lat]);
    if (!pickMarkerOn) { pickMarker.addTo(map); pickMarkerOn = true; }
    document.getElementById('pickinfo').textContent =
      `pick: ${pick.src} · dist ${Math.round(pick.dist)}m · alt ${Math.round(pick.alt)}m`;
  } else {
    if (pickMarkerOn) { pickMarker.remove(); pickMarkerOn = false; }
    document.getElementById('pickinfo').textContent = 'pick: miss';
  }
};

// --- sync -----------------------------------------------------------------
// follow pauses while the pointer is down: the per-frame jumpTo cancels
// maplibre's drag, which then lands as a click (a teleport)
let mapPointerDown = false;
const mapEl = document.getElementById('map');

// the control's own flyTo is overridden by the follow's per-frame jumpTo
const geolocate = new maplibregl.GeolocateControl({
  positionOptions: { enableHighAccuracy: true, timeout: 10000 },
  trackUserLocation: false,
});

const geocoder = new MaplibreGeocoder({
  forwardGeocode: async (config) => {
    const features = [];
    try {
      const resp = await fetch('https://nominatim.openstreetmap.org/search?'
        + new URLSearchParams({ q: config.query, format: 'geojson', limit: 5 }));
      for (const f of (await resp.json()).features) {
        const center = f.geometry.type === 'Point' ? f.geometry.coordinates
          : [(f.bbox[0] + f.bbox[2]) / 2, (f.bbox[1] + f.bbox[3]) / 2];
        features.push({ type: 'Feature', geometry: { type: 'Point', coordinates: center },
          place_name: f.properties.display_name, text: f.properties.display_name,
          place_type: ['place'], center, bbox: f.bbox, properties: f.properties });
      }
    } catch (e) {}
    return { features };
  },
}, { maplibregl, marker: false, showResultMarkers: false, flyTo: false,
  showResultsWhileTyping: false });

// not maplibre's animations: they lock the map for their whole duration.
// an exponential ease lags a moving target by velocity*tau, so it aims
// ahead by the smoothed velocity
const FOLLOW_TAU_MS = 120;
const followMarker = (latlng, dt) => {
  if (S.prevMark && dt > 0) {
    const dlat = latlng.lat - S.prevMark.lat;
    const dlng = latlng.lng - S.prevMark.lng;
    if (Math.abs(dlat) > 0.05 || Math.abs(dlng) > 0.05) {
      S.followVel = null; // teleport, not motion: don't feed it forward
    } else {
      const a = 1 - Math.exp(-dt / 200);
      S.followVel = S.followVel ? {
        lat: S.followVel.lat + (dlat / dt - S.followVel.lat) * a,
        lng: S.followVel.lng + (dlng / dt - S.followVel.lng) * a,
      } : { lat: dlat / dt, lng: dlng / dt };
    }
  }
  S.prevMark = latlng;
  const lead = S.followVel
    ? { lat: S.followVel.lat * FOLLOW_TAU_MS, lng: S.followVel.lng * FOLLOW_TAU_MS }
    : { lat: 0, lng: 0 };
  const c = map.getCenter();
  const k = 1 - Math.exp(-dt / FOLLOW_TAU_MS);
  let nlat = c.lat + (latlng.lat + lead.lat - c.lat) * k;
  let nlng = c.lng + (latlng.lng + lead.lng - c.lng) * k;
  // never pass the marker (the lead goes stale as the camera slows), or
  // stopping springs back
  if ((latlng.lat - c.lat) * (latlng.lat - nlat)
    + (latlng.lng - c.lng) * (latlng.lng - nlng) < 0) {
    nlat = latlng.lat;
    nlng = latlng.lng;
  }
  map.jumpTo({ center: [nlng, nlat] });
};
const initMap = () => {
  map = new maplibregl.Map({
    container: 'map',
    style: 'https://tiles.openfreemap.org/styles/liberty',
    center: [START.lon, START.lat],
    zoom: START.zoom,
    dragRotate: false, // keep the map north-up
    pitchWithRotate: false,
  });
  map.touchZoomRotate.disableRotation();
  map.keyboard.disable(); // arrows belong to the flight canvas

  marker = new maplibregl.Marker({ element: markerEl })
    .setLngLat([START.lon, START.lat]).addTo(map);

  // the whole strip is the target; the button's click bubbles here
  dividerEl.addEventListener('click', () => {
    setFull3d(!full3d);
    canvas.focus(); // keep flight keys live (and space off the button)
  });
  if (START.full3d) setFull3d(true);

  setPickDebug(pickDebug);

  mapEl.addEventListener('pointerdown', () => { mapPointerDown = true; });
  addEventListener('pointerup', () => { mapPointerDown = false; });

  map.addControl(geolocate, 'top-left');
  geolocate.on('geolocate', (pos) => {
    if (!view.ready()) return;
    const p = view.getPose();
    view.setPose(pos.coords.latitude, pos.coords.longitude, p.alt, p.heading, p.tilt);
    S.following = true;
  });

  map.addControl(geocoder, 'top-left');
  // out of #map, which is hidden in full3d; the controls don't mind
  document.body.appendChild(map.getContainer().querySelector('.maplibregl-ctrl-top-left'));
  geocoder.on('result', ({ result }) => {
    if (!view.ready()) return;
    const p = view.getPose();
    let alt = p.alt;
    if (result.bbox) {
      const [w, s, e, n] = result.bbox;
      const spanM = Math.max((n - s), (e - w) * Math.cos(result.center[1] * Math.PI / 180)) * 111000;
      alt = Math.min(20000, Math.max(200, spanM * 0.8));
    }
    view.setPose(result.center[1], result.center[0], alt, p.heading, p.tilt);
    S.following = true;
    canvas.focus(); // hand the keys back to flight controls
  });

  map.on('dragstart', () => { S.following = false; });
  map.on('click', (e) => {
    if (!view.ready()) return;
    if (e.originalEvent.shiftKey) return; // shift belongs to rect drawing
    const p = view.getPose();
    view.setPose(e.lngLat.lat, e.lngLat.lng, p.alt, p.heading, p.tilt);
  });

  // while following, zoom about the centre (the marker) so it can't fight
  // the follow; otherwise maplibre's pointer-anchored zoom
  document.getElementById('map').addEventListener('wheel', (e) => {
    if (!S.following) return;
    e.preventDefault();
    e.stopPropagation();
    map.setZoom(map.getZoom() - e.deltaY * 0.005);
  }, { capture: true, passive: false });
};
