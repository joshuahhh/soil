import { canvas } from './engine.js';
import { viewName, ortho, hug } from './mode.js';
import { miniature } from './input.js';
import { tubeRect } from './tube.js';

export { refreshKeys, toggleKeys, initKeyTable };

// --- the key table (?) ----------------------------------------------------
// what every key does *in the current mode* — a key that does nothing here
// isn't listed, and one that does something different (arrows orbit in
// ortho, look around in perspective) says what it does here. rebuilt by
// updateHint on every mode switch
const K = (t) => t.split(' ').map((k) => `<kbd>${k}</kbd>`).join(' ');
const keysFor = () => {
  const mode = viewName();
  const g = [];
  if (mode === 'tunnel') g.push(['flying the tunnel', [
    [K('W A S D'), 'move'], [K('R F'), 'up / down'],
    [K('space') + ' +', 'move in the view frame instead'],
    [K('←↑↓→') + ' drag', 'look around'], [K('Z X') + ' wheel', 'zoom (fov)'],
    [K('- ='), 'speed'], [K('shift') + ' +', 'slow'],
    [K('G'), hug ? 'stop hugging the terrain' : 'hug the terrain'],
    [K('T') + ' ' + K('esc'), 'leave the tunnel']]]);
  if (mode === 'persp') g.push(['flying', [
    [K('W A S D'), 'move'], [K('R F'), 'up / down'],
    [K('space') + ' +', 'move in the view frame instead'],
    [K('←↑↓→') + ' drag', 'look around'], [K('Z X') + ' wheel', 'zoom (fov)'],
    [K('- ='), 'speed'], [K('shift') + ' +', 'slow'],
    [K('G'), hug ? 'stop hugging the terrain' : 'hug the terrain'], [K('0'), 'reset view']]]);
  if (mode === 'ortho') g.push(['moving', [
    [K('W A S D') + ' drag', 'pan'], [K('←↑↓→'), 'orbit around the center'],
    [K('Z X R F') + ' wheel', 'zoom'], [K('- ='), 'speed'], [K('shift') + ' +', 'slow'],
    [K('0'), 'reset view']]]);
  if (mode === 'tube') g.push(['flying the tube', [
    [K('W S'), 'thrust'], [K('A D'), 'strafe'], [K('R F'), 'up / down'],
    [K('←↑↓→') + ' drag', 'pitch + yaw'], [K('Q E'), 'roll'], [K('Z X') + ' wheel', 'zoom'],
    [K('- ='), 'speed'], [K('shift') + ' +', 'slow'],
    [K('G'), hug ? 'stop hugging the wall' : 'hug the wall'], [K('0'), 'reset view'],
    [K('T'), 'unroll the tube'], [K('B'), 'wrap it on the body instead'], [K('esc'), 'clear the tube']]]);
  if (mode === 'body') g.push(['flying around the body', [
    [K('W S'), 'thrust'], [K('A D'), 'strafe'], [K('R F'), 'up / down'],
    [K('←↑↓→') + ' drag', 'pitch + yaw'], [K('Q E'), 'roll'], [K('Z X') + ' wheel', 'zoom'],
    [K('- ='), 'speed'], [K('shift') + ' +', 'slow'], [K('0'), 'reset view'],
    [K('B'), 'unwrap'], [K('T'), 'roll it into a tube instead'], [K('esc'), 'clear the rect']]]);
  if (mode === 'follow') g.push(['following the path', [
    [K('W S'), 'run along it'], [K('←↑↓→'), 'swing the view around it'],
    [K('R F'), ortho ? 'zoom' : 'standoff'], [K('Z X'), 'zoom'], [K('shift') + ' +', 'slow'],
    [K('V') + ' ' + K('esc'), 'stop following']]]);
  const viewRows = [];
  if (mode !== 'tube' && mode !== 'body' && mode !== 'tunnel') viewRows.push([K('O'), ortho ? 'perspective' : 'orthographic']);
  if (mode === 'persp' && !tubeRect) viewRows.push([K('T'), 'tunnel']);
  viewRows.push([K('N'), miniature ? 'miniature off' : 'miniature (tilt-shift)']);
  if (!ortho) viewRows.push([K('H'), 'near cut']);
  viewRows.push([K('Y'), 'sky: time of day, haze, shadows']);
  viewRows.push([K('I'), 'glamour: refine a still frame']);
  viewRows.push([K('C L K M'), 'debug: pick · lod · smoothing · mipmaps']);
  g.push(['view', viewRows]);
  if (document.body.classList.contains('overlay')) {
    const rows = [[K('P'), 'photo on / off'], [K('shift') + ' drag', 'move the photo'],
      [K('shift') + ' wheel', 'size it about the pointer']];
    if (mode === 'persp') rows.push(['drag', 'either end of a pair, to adjust it'],
      [K('esc'), 'cancel adding a pair']);
    g.push(['photo', rows]);
  }
  const mapRows = [['click', 'teleport there'],
    [K('shift') + ' drag', tubeRect ? 'draw a new tube rect' : 'draw a tube rect']];
  if (mode !== 'tube' && tubeRect) mapRows.push([K('T'), 'roll the tube up']);
  if (mode !== 'body' && tubeRect) mapRows.push([K('B'), 'wrap it on a body']);
  if (mode !== 'tube' && mode !== 'body' && tubeRect) mapRows.push([K('esc'), 'clear the rect']);
  if (mode !== 'follow' && mode !== 'tube' && mode !== 'body' && document.body.classList.contains('path'))
    mapRows.push([K('V'), 'follow the path']);
  g.push(['map', mapRows]);
  g.push(['session', [[K('⌘C ⌘V'), 'copy / paste the whole session'],
    [K('⌘S'), 'save it as a file'],
    ['drop a file', 'image → photo · .gpx → path · .obj → body · session .json → restore'], [K('?'), 'this']]]);
  return g;
};
// the table is a pure function of the view's state, and that state changes
// from a dozen places (a key, a drop, a rect on the map). rather than have
// each of them remember to redraw it, the frame loop re-derives it a few
// times a second while it is open and only touches the dom when the
// result differs — poor man's react, at the cost of building a short
// string every 250ms
let keysHtml = '', keysAt = 0;
const renderKeys = () => {
  const html = keysFor().map(([title, rows]) =>
    `<section><h4>${title}</h4><table>${rows.map(([k, what]) =>
      `<tr><td>${k}</td><td>${what}</td></tr>`).join('')}</table></section>`).join('');
  if (html === keysHtml) return;
  keysHtml = html;
  document.getElementById('keys').innerHTML = html;
};
const refreshKeys = (now) => {
  if (!document.body.classList.contains('keys') || now - keysAt < 250) return;
  keysAt = now;
  renderKeys();
};
const toggleKeys = () => {
  if (!document.body.classList.contains('keys')) renderKeys();
  document.body.classList.toggle('keys');
  canvas.focus();
};
const initKeyTable = () => {
  document.getElementById('keysbtn').addEventListener('click', toggleKeys);
};
