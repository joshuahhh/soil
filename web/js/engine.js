import { QS } from './config.js';
import { fetchTile } from './tiles.js';
import { layoutOverlay } from './photo.js';

export { Module, view, canvas, rightEl, resize, initEngine };

// --- module + view --------------------------------------------------------
let Module = null, view = null; // arrive with initEngine
const canvas = document.getElementById('globe');
const rightEl = document.getElementById('right');
const resize = () => {
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(canvas.clientWidth * dpr);
  canvas.height = Math.round(canvas.clientHeight * dpr);
  // the overlay is placed in viewport-relative terms, so it follows
  // window resizes and the map pane collapsing/expanding
  layoutOverlay();
};
const initEngine = async () => {
  Module = await createEarthModule();

  resize();
  addEventListener('resize', resize);

  if (QS.has('jpg')) Module.forceJpgTextures();
  // multisampling is on by default: mipmaps only fix texture minification,
  // while the crawl on distant building silhouettes is geometric aliasing,
  // which needs this instead. ?msaa=0 opts out (for a slow gpu, or to A/B it).
  // context attribute, so it can't be a live key toggle like M
  if (QS.has('msaa')) Module.setAntialias(QS.get('msaa') !== '0');
  // per-tile decode timing on the console; a line per node, so off unless asked
  if (QS.has('timing')) Module.setLogTiming(QS.get('timing') !== '0');

  view = Module.createView('#globe', fetchTile);
};
