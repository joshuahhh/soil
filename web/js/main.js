// the page, brought up in order. a module's top level only declares things
// (reading config.js at most, which imports nothing); whatever touches the
// page, the engine or the map waits for its init, called below once the
// engine and the map exist. that is also what lets the modules import each
// other in circles: by the time any of this runs, every module has been
// evaluated, so a name imported from anywhere is there to be called.
//
// the order is the order the page ran in as one script, and it still
// decides a few things — which keydown listener hears a key first, the
// order the map's layers stack in — so keep it unless you mean to change
// them. state shared between modules is in state.js
import { initTiles } from './tiles.js';
import { initEngine } from './engine.js';
import { initSaveView } from './saveview.js';
import { initMap } from './map.js';
import { initPhoto } from './photo.js';
import { initHud, initPanels, initRoll } from './hud.js';
import { initKeyTable } from './keytable.js';
import { initNearCut } from './nearcut.js';
import { initDrop } from './drop.js';
import { initInput } from './input.js';
import { initTunnel } from './tunnel.js';
import { initTube } from './tube.js';
import { initPath } from './path.js';
import { initAlign } from './align.js';
import { initSession } from './session.js';
import { initSky } from './sky.js';
import { initFrame } from './frame.js';
import { initShot } from './shot.js';
import { initTestApi } from './testapi.js';

initHud();
await initEngine();
initTiles();
initSaveView();
initMap();
initPhoto();
initPanels();
initKeyTable();
initNearCut();
initDrop();
initInput();
initTunnel();
initTube();
initPath();
initAlign();
initSession();
initSky();
initRoll();
initFrame();
initShot();
initTestApi();
