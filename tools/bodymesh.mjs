// extract the skin from makehuman's base mesh into web/body.obj: the file
// the body mode (index.html, bakeBody) wraps the tube rect onto.
//
//   curl -sLO https://raw.githubusercontent.com/makehumancommunity/makehuman/master/makehuman/data/3dobjs/base.obj
//   node tools/bodymesh.mjs base.obj
//
// base.obj is the hm08 basemesh, released CC0 in september 2020 (the
// notice is in the file's own header). it carries a lot besides the skin
// — tights/skirt/hair helper meshes, per-joint marker cubes, eyes, teeth,
// tongue — as separate `g` groups; only `g body` is kept. quads are split
// to triangles, uv/normal indices are dropped (the bake casts its own
// rays and takes normals from what it builds), and coordinates are
// rounded to 4 places, which is well under the bake's texel size
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const src = process.argv[2];
if (!src) { console.error('usage: node tools/bodymesh.mjs base.obj'); process.exit(1); }
const out = join(dirname(fileURLToPath(import.meta.url)), '..', 'web', 'body.obj');

const verts = [];
const faces = []; // triangles, 1-based vertex indices into verts
let keep = false;
for (const line of readFileSync(src, 'utf8').split('\n')) {
  if (line.startsWith('v ')) {
    verts.push(line.slice(2).trim().split(/\s+/).map(Number));
  } else if (line.startsWith('g ')) {
    keep = line.trim() === 'g body';
  } else if (keep && line.startsWith('f ')) {
    const idx = line.slice(2).trim().split(/\s+/).map((s) => parseInt(s, 10));
    for (let i = 1; i + 1 < idx.length; i++) faces.push([idx[0], idx[i], idx[i + 1]]);
  }
}
// reindex to just the vertices the skin uses
const remap = new Map();
const kept = [];
for (const f of faces) for (const i of f) if (!remap.has(i)) { remap.set(i, kept.length + 1); kept.push(verts[i - 1]); }
const lines = ['# makehuman hm08 basemesh, skin only — CC0 (see tools/bodymesh.mjs)'];
for (const v of kept) lines.push('v ' + v.map((x) => x.toFixed(4).replace(/\.?0+$/, '')).join(' '));
for (const f of faces) lines.push('f ' + f.map((i) => remap.get(i)).join(' '));
writeFileSync(out, lines.join('\n') + '\n');
console.log(`${out}: ${kept.length} vertices, ${faces.length} triangles`);
