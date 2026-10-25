import { START } from './config.js';
import { S } from './state.js';
import { view, canvas, resize } from './engine.js';

export { map, marker, setMarkerHeading, full3d, pickDebug, setPickDebug, updatePickDebug,
  mapPointerDown, followMarker, initMap };

// --- maplibre -------------------------------------------------------------
let map = null, marker = null; // arrive with initMap
// camera marker: a dot with a view cone that rotates with the heading.
// pointer-events none so clicks pass through to the map (teleport)
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

// expand the 3d view to the whole window: the arrow on the middle divider
// collapses the map pane; the divider stays at the left edge and its arrow
// flips to bring the map back. sticky across reloads via the saved view
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

// pick debug (C to toggle): a reticle at the exact center of the 3d view,
// an orange dot on the map at what the center raycast hit (the orbit
// pivot), and a readout of the hit distance/altitude and whether it came
// from terrain triangles or the sea-level sphere fallback. if the orbit
// pivot looks wrong on screen, the orange dot shows where it actually is
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
// the marker always shows where the camera is. the map is a free viewport:
// dragging it never touches the camera; clicking it teleports the camera
// there. while flying, the map follows the marker (smoothly easing over
// when follow mode re-engages); grabbing the map stops the following

// while the pointer is down on the map, pause follow updates entirely:
// the per-frame jumpTo would cancel maplibre's drag gesture mid-flight
// (the aborted drag then even lands as a click -> surprise teleport)
let mapPointerDown = false;
const mapEl = document.getElementById('map');

// the standard geolocate control (crosshair button); its geolocate event
// teleports the camera like a map click, keeping altitude/heading/tilt.
// the control's own flyTo is immediately overridden by follow mode's
// per-frame jumpTo, which brings the map along instead
const geolocate = new maplibregl.GeolocateControl({
  positionOptions: { enableHighAccuracy: true, timeout: 10000 },
  trackUserLocation: false,
});

// place search: the standard maplibre-gl-geocoder control backed by OSM's
// nominatim api. flyTo is off — picking a result teleports the camera like
// a map click, and follow mode brings the map along on its own
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

// per-frame exponential ease of the map center toward the marker: smooth
// when follow mode re-engages far away, and tight tracking during flight.
// no built-in map animations — they lock the map for their whole duration,
// which falls apart when the camera covers a large distance every frame.
// an exponential ease chasing a moving target always lags it by
// velocity*tau, so aim at the marker's position led by its (smoothed)
// velocity — steady fast flight then keeps the marker dead-center
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
  // monotonicity: the lead aims ahead of the marker, and when the camera
  // slows it goes stale for a moment — never let the center actually pass
  // the marker, or stopping visibly springs back. if this step would cross
  // it, land exactly on it
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

  // the whole strip is the target, not just the arrow (the arrow is only the
  // affordance); the button's own click bubbles up to here
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
  // both live in maplibre's top-left slot inside #map, which is display: none
  // once the 3d view has the whole window. move the slot to the body: the
  // controls only hold the map object, not their place in its dom
  document.body.appendChild(map.getContainer().querySelector('.maplibregl-ctrl-top-left'));
  geocoder.on('result', ({ result }) => {
    if (!view.ready()) return;
    const p = view.getPose();
    // altitude scaled to the result's footprint (a city needs more height
    // than a street corner), clamped to something flyable
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

  // locked-mode wheel zoom: handle it ourselves with an unanimated setZoom,
  // which zooms around the center — where the follow keeps the marker — so
  // it can't fight the per-frame follow updates. free mode gets maplibre's
  // native pointer-anchored zoom
  document.getElementById('map').addEventListener('wheel', (e) => {
    if (!S.following) return;
    e.preventDefault();
    e.stopPropagation();
    map.setZoom(map.getZoom() - e.deltaY * 0.005);
  }, { capture: true, passive: false });
};
