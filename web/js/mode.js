import { view } from './engine.js';
import { updateHint } from './hud.js';

export { mode, inTube, viewName, defineMode, setMode, ortho, setOrtho, hug, setHug };

// --- the view's mode -----------------------------------------------------
// what the camera is doing, exactly one at a time:
//   fly     free flight over the globe
//   follow  on rails along the dropped track (path.js)
//   tunnel  the ground ahead curled up around the camera (tunnel.js)
//   tube    the drawn rect rolled into a tube (tube.js)
//   body    the drawn rect wrapped onto the figure (tube.js)
// setMode is the one place it changes, so whatever asks for a switch — a
// key, a button, a rect drawn on the map, the saved view coming back — the
// mode being left is always left properly before the next is entered.
//
// what isn't a mode: the projection (ortho, which only fly and follow
// allow), terrain hug (fly, tube and tunnel), and the rect (tube.js),
// which stays drawn whether or not it is rolled. and two states that live
// inside a mode and with its module: the tube's staged load before it
// rolls (tubePending), and the tunnel's animated way out (tunnelClosing),
// during which the mode is still tunnel
let mode = 'fly';
const inTube = () => mode === 'tube' || mode === 'body';
// the name the hint and the key table go by: fly splits by projection
const viewName = () => mode === 'fly' ? (ortho ? 'ortho' : 'persp') : mode;

// each mode but fly brings its own { enter(opts), exit() }. two modes may
// share one: the tube and the body are the same machinery with a different
// warp, so going from one to the other re-enters without leaving, and so
// does asking again for the mode already in force (the tube re-rolls when
// its axis flips)
const modes = {};
const defineMode = (name, hooks) => { modes[name] = hooks; };

// no ortho inside the tube or the tunnel: the ortho extent and zoom are
// anchored on normal terrain below, and a parallel projection of the tube
// reads as mush. the hug fights the rails over altitude (they write the
// pose outright), and the wall hug is the cylinder's, not the body's
const ORTHO_OK = new Set(['fly', 'follow']);
const HUG_OK = new Set(['fly', 'tube', 'tunnel']);

// by the time either hook runs the mode is already the new one, so an exit
// that redraws its panel draws it as it now is
const setMode = (next, opts = {}) => {
  const from = mode, a = modes[from], b = modes[next];
  mode = next;
  if (from !== next && a !== b && a) a.exit();
  if (!ORTHO_OK.has(next)) setOrtho(false);
  if (!HUG_OK.has(next)) setHug(false);
  if (b) b.enter(opts);
  updateHint();
};

// the two switches the mode constrains. asking for one the mode doesn't
// allow does nothing; each says whether it changed anything
let ortho = false, hug = false;
const setOrtho = (on) => {
  if (on === ortho || (on && !ORTHO_OK.has(mode))) return false;
  ortho = on;
  view.setOrtho(on);
  return true;
};
// terrain hug: cruise at a fixed height above the mesh — over the sphere
// normally, radially above the wall inside the tube
const setHug = (on) => {
  if (on === hug || (on && !HUG_OK.has(mode))) return false;
  hug = on;
  view.setTerrainFollow(on);
  return true;
};
