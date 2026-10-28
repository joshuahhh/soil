Reverse-engineering undocumented parts of Google Earth, forked from [retroplasma/earth-reverse-engineering](https://github.com/retroplasma/earth-reverse-engineering) (now archived) and violently vibe-coded.
This fork is web-only: the C++ engine compiles to WebAssembly
and renders in a split-screen demo next to a MapLibre slippy map.

#### Layout

- repo root — the engine (C++ → wasm via emscripten, `earth_web.cpp` +
  `rocktree_*.h`) and its build/deploy scripts
- [web/](./web/) — the split-screen app (`index.html`, its code in `js/` as
  native es modules brought up by `js/main.js`, its look in `style.css`), the
  phone camera-match page (`ar.html`), and the build's js/wasm output
- [proto/](./proto/) — protobuf schema for Google Earth's "rocktree" data

#### Build

```
./setup.sh   # once per machine: fetches pinned deps into ./deps
./build.sh
```

`setup.sh` downloads pinned releases of eigen, cgal and protobuf and compiles
libprotobuf to wasm; it needs emscripten and curl. `build.sh` then produces
`web/earth.js` + `web/earth.wasm` (modularized, pthreads). Serve `web/`:

```
./serve.py
```

(a thin http.server wrapper on port 8000 that disables caching, so browsers
never run a stale wasm after a rebuild)

`coi-serviceworker.js` injects the cross-origin-isolation headers the pthread
build needs (expect one automatic reload on first visit).

#### Tile cache and console

Downloaded tiles are cached in indexeddb, keyed by rocktree path, under a
**byte budget with least-recently-used eviction**: a quarter of the quota the
browser offers, held between 512 MiB and 4 GiB. A tile averages about 4 KB, so
even the floor is a hundred thousand of them.

The budget pointedly ignores `estimate().usage`. Firefox pools storage by
*group* rather than by origin — every localhost port reports the same usage —
so on a dev machine that number describes whatever else the browser has stored
under `localhost`, which this page can neither see nor evict. An earlier
version subtracted it as "not ours" and pinned the cache to its floor because
an unrelated project was sitting on ten gigabytes. Having a budget at all is
the whole point: an unbounded cache eventually takes the
origin's entire quota, and once that happens *every* write fails, not just
tile writes — the dropped gpx and the reference photo live in the same
database, so a full cache surfaces as the page quietly restoring the file
before last rather than as an error. That is a miserable thing to diagnose,
which is why those two writes are now checked and report failure (console,
plus `NOT SAVED` in the path panel).

**Writes are batched, and that is load-bearing.** Indexeddb runs the
transactions on a store in the order they were created, and a lookup cannot
start while a write ahead of it holds the store. One readwrite transaction per
downloaded tile (plus one for its meta record) put thousands of them in front
of the lookups a settling view issues, and the queue grew faster than it
drained: a lookup that took 100 ms at the start of a load took two seconds a
minute in, every download slot was spent waiting in it, and a long session
read as tiles that never arrive. Measured on a 7° horizon view over toronto
(`tools/loadprof.mjs`, ~1850 tiles): 60–90 s to settle with the cache, 8 s
with it bypassed. So downloaded tiles are collected and committed a few
hundred ms at a time in one transaction with relaxed durability (no fsync per
commit — a lost tile re-downloads), and the lookups a frame issues go out
together in one readonly transaction. Same view, cache on: under 10 s cold,
and a warm load serves every tile from the store. `?idb=0` bypasses the cache
and `?idb=ro` looks up but never writes, for measuring it.

Lookups have a deadline, too. A lookup that fails falls through to the
network, but one that simply doesn't answer used to hold the engine's request
for as long as it took — and the first one of a session took a minute in
firefox, which initialises a multi-gigabyte origin (everything stored under
`localhost`, not just this page) before the first transaction runs. The root
bulk was in that lookup, and nothing draws without it. A batch that hasn't
answered in three seconds is given up on (its tiles come from the network, a
late answer is dropped) and the cache is bypassed until it does answer. And
nothing is evicted until the budget has actually been read: a session used to
begin by evicting a hundred megabytes against the provisional floor, a slow
readwrite transaction that every lookup then queued behind.

Sizes can't be read back from indexeddb without reading the values, which is
the thing being avoided, so each tile gets a small `tilemeta` record holding
its size and last use. That's held in memory for the session and mirrored to
the store — rate-limited to one write per tile per five minutes, since an
exact last-use is worth nothing here and a write per cache hit would cost more
than the ordering it buys. An eviction pass sorts once and clears down to 85%
of the budget, so the next pass is a long way off. A write that fails anyway
(the accounting says there's room, the browser disagrees, and the browser is
the one that counts) triggers a harder pass.

Tiles written before the bookkeeping existed have no size the budget can see,
so db version 3 gets rid of them — but *not* in the upgrade transaction. A
`clear()` on several gigabytes holds `open()` until it finishes, and since the
whole page used to sit behind that await, the result was two blank panes, an
empty console, and no sign anything was happening. Nothing may hold up the
page: the connection is awaited by nobody, every helper tolerates a null `db`
and falls through to the network, and the leftover tiles are deleted by a
background key-cursor pass in chunks of 2000 with a gap between transactions.
Interrupting that just means the next session picks up where it stopped.
`__demo.clearTiles()` empties everything by hand.

Per-tile decode timing (`timing: decode node ... dur=...`) is off by default —
it's a line per node and a settling view decodes hundreds. `?timing` turns it
back on. The 2-second `sections avg ms:` line is unconditional; that one is
the profiling readout, not chatter.

#### Moving a session between builds

The deployed page and a local one are separate origins, so they share nothing:
two localstorages, two indexeddbs. `⌘C` (`ctrl-C`) anywhere on the page — with
nothing selected and no field focused — copies the whole session to the
clipboard as one json blob: the saved view, the path and align settings, and
the two dropped files (gpx and reference photo, as data urls). `⌘V` on the
other build unpacks it and reloads into that session. `window.__demo.copyState()`
and `pasteState(text)` are the same two from the console.

Pasting is destructive on purpose: a key the incoming session doesn't carry is
removed rather than left behind, so what you land in belongs to one session and
not a mix of two. The tile cache is left out — megabytes of bytes that
re-download themselves, and skipping them keeps the blob to roughly the size of
the photo.

#### Dropped tracks

Drop a `.gpx` on the window and the track is drawn twice: as a line on the
map, and over the terrain in the 3d view. A panel in the hud carries color,
opacity, width, the drape switch, and the ✕ that removes it; the file itself
is kept in indexeddb next to the tile cache, so a reload comes back to it.
(An image dropped instead is still the reference photo — the drop is sorted
by extension.)

The 3d line is a ribbon of triangles, not `GL_LINES`: WebGL clamps line width
to 1, so a polyline you can style has to be geometry. Each segment is a quad
whose corners the vertex shader pushes sideways in *screen pixels* — constant
width with distance — plus a small square patch at each interior point to fill
the wedge a bend leaves open. The shader also clips the segment against the
near plane in clip space, since projecting a vertex with `w <= 0` flings it to
the wrong side of the screen and draws the segment as a streak across the view.
It draws after the terrain with depth test on but depth writes off (the ribbon
overlaps itself at every join, and a translucent line that occludes itself
comes out blotchy), and with a small ndc depth nudge toward the camera.

**Draping is the interesting part.** The file's elevations are what a track is
drawn at by default — they are what it actually recorded, and the sphere the
mesh sits on is the datum they are already measured against. But GPS altitude
is worth tens of meters on a good day and the geoid separation under it a
hundred more, so a track laid at its own elevations wanders through hillsides.
The drape switch answers that: each point is re-measured against the mesh with
the same downward raycast terrain-hug uses (`groundRadiusUnder`) and pinned to
the ground there. That measurement only answers where fine mesh is currently
resident *and drawn*, so it runs a couple dozen points a frame and keeps what
it learns — a track settles onto the terrain over the first seconds and stays
put after.

Flipping between the two is a fair picture of what a GPS watch actually knows
about altitude. A file that records no elevations has nothing to flip to: the
switch comes up on and locked, since the alternative is the whole track lying
at sea level, buried under the terrain and correctly hidden by the depth
test.

Vertices go to the gpu in float, and ECEF coordinates are 6.4e6 meters — half
a meter of precision, which shows as a wobble against the terrain. The path
uses the same trick the meshes do: subtract a nearby origin and fold it back
into the transform in double.

Nothing is drawn in tube mode: the ground has been rolled out from under the
track, and a line drawn in globe space would hang in the air where the terrain
used to be.

**Follow mode** (`V`, or the panel's button) puts the camera on rails: it aims
at a point that runs along the track, from a standoff expressed in the path's
own frame rather than the world's. W/S drive that point along the track, the
arrows swing the camera around it (left/right *relative to the track's
heading*, so rounding a bend carries the camera with it and you keep looking at
the same shoulder of the trail), and R/F change the standoff distance. A/D do
nothing — on rails there is nowhere sideways to go.

Where you are is marked in three places while riding: a disc on the line
itself in the 3d view, a dot on the map, and a distance in the panel. The 3d
disc is the same quad the ribbon is made of, carved into a circle by the
fragment shader — a white core inside a ring of the path's color, matching the
map's marker — and depth-tested like the line, so a shoulder of terrain hides
the dot exactly when it hides the track.

It is placed by *fractional point index*, not by position, and that detail is
the difference between a marker and an almost-marker. The camera follows the
smoothed spine; the ribbon is drawn through the raw points. Placing the disc
from the camera's own position put it wherever the smoothing had cut the
corner — visibly off the line at every bend. Handing the engine an index
instead lets it interpolate between the two positions the ribbon was built
from, which is on the drawn segment by construction, drape and lift included.
Following is therefore measured in metres along the raw track, so the panel's
progress agrees with the length it reports for the file; only the heading
still comes from the spine, since jitter lives in the direction between
consecutive fixes rather than in where they are.

It steers off a smoothed copy of the track, never the track itself. A GPS fix
wanders a few meters between samples, and taking the heading from one raw
segment hands every one of those wobbles to the camera, which reads as the
world shivering. So the track is resampled at a fixed 10 m step, boxcar-
smoothed twice (two passes ≈ gaussian), and the heading is taken from a 300 m
chord across *that* rather than from any one segment — with a half-second
temporal ease on top for what survives.

The camera and the heading want different amounts of it. The heading takes a
window three times the camera's, heavy enough to ignore a wandering fix
entirely; the camera aims down a lighter one that kills the wander but still
hugs the shape. Aiming down the heading's copy would leave the marker — which
sits on the drawn line, where it belongs — visibly off-centre through every
switchback, since that is exactly where heavy smoothing cuts the corner.

Follow mode has **one notion of scale**: how much ground the view spans at the
target, from the ortho extent where there is one and from the standoff and fov
otherwise. Travel speed is cut from it, so motion looks the same on screen at
any zoom, and so are the smoothing windows below. Speed used to scale to the
standoff alone — proportional to the span at a fixed fov, so it felt right
until you touched a zoom, since Z/X moves the fov and in ortho the wheel and
Z/X move the extent, none of which is the standoff.

The windows are **fractions of that span** rather than fixed distances,
because a fixed window is only ever right at one zoom: 20 m of
smoothing is the difference between a steady camera and a shaky one from 60 m
out, and nothing at all from 2 km up, where the same track reads as rigidly
locked to its marker. Both the R/F standoff and the Z/X zoom feed it.

The window therefore moves every frame, but it is quantized rather than
followed exactly: each smoothed copy is filtered once and kept until the
window has moved 8%, which costs a sub-metre step in the aim and saves
refiltering the track for a change nobody can see. Two boxcars over the spine
is O(n) and only runs while you are actually zooming, where evaluating a
kernel per frame would run always.

Since the smoothed copy exists only as the thing the camera aims down, there
is nothing to look at when the marker floats off-centre. **K** (next to L, the
lod debug; unlisted in the hud, unlike L) draws it on the map beside the
track, dashed, and puts the two window sizes in the progress readout. It draws
the aim copy *itself* rather than filtering its own — a debug view that can
disagree with the thing it depicts is worse than none. The gap between the two
lines is the float. The aim height comes from
`getPathGroundAlt`, which reports the drape's measurement where it has one and
NaN where it hasn't, so the camera rides the terrain rather than the track's
GPS altitudes, easing between the two as the measurements land.

#### Sky and haze

The terrain is drawn under an atmosphere: a sky pass with the sun in it, and
aerial perspective on the ground — what is far fades toward the haze, per
channel, the red surviving the trip best, which is why distance goes
blue-grey. **Y** opens the `sky` panel: `time` and `date` put the sun by the
clock and calendar at the camera, in its own time zone (daylight saving and
all), or *follow the clock* takes them from the real one (the sliders then
show where the clock has them, locked); below them, the zone the camera is
in and the true solar time there; `sky` and `sun tint`
switch those passes; `haze` switches the aerial perspective, its slider the
visibility at sea level (5–320 km); `shadows` the sun shadows, its slider how
dark the shade is; `occlusion` the ambient occlusion, its slider its weight. A switch that gates a slider stands in for the slider's
name, and an off switch dims and locks its slider without losing the value.
The default is a fixed afternoon, not the clock: a place is not always wanted
at the hour it happens to be there. It is all part of the saved view, and
`?sky=0`, `?vis=km` and `?sun=az,el` set it from the url; test mode pins the
sun so goldens don't depend on the time of day.

The haze is an optical depth, not a distance: the air thins with height
(scale height 12 km), so the depth of the way to a fragment is its length
times the mean density between the eye's height and the fragment's — which
the fragment shader recovers from its depth and the camera frame. That is
what keeps the planet clear from orbit, with a hazy limb, where a plain
distance fog made it a pale disc. The sky thins away with altitude the same
way, to black, leaving the limb: the atmosphere seen edge-on just above the
horizon line, which sits below level by the dip the altitude gives it. The
haze colour and the sky's horizon band are one colour, derived each frame
from the sky colour and the sun's height — warm near the horizon, dim past
it — so the ground fades into the sky it is under. The sun itself is the
NOAA low-precision position from the date, computed on the page.

**Sun shadows** (the `shadows` row on the same panel) are a 4096² depth map rendered from the sun over what is drawn,
fitted to the near part of the view — the frustum out to a couple of times
the distance to the terrain under the crosshair, at most 40 km — with its
size quantized and its centre snapped to whole texels, so it doesn't crawl
as the camera moves; it is re-rendered only when something changed (a still
camera costs nothing). The terrain shader samples it with hardware pcf and a
normal offset, and adds a facing term from the screen-space normal, relative
to flat ground: only faces actually turned from the sun darken — walls, the
far side of a ridge — so mountains get relief from the real sun direction
everywhere, not just inside the map's box. The shade is lit by the sky, so
it goes a little blue. Casters must be drawn to cast: a tower behind the
camera, out of the frustum, casts nothing.

Two things the photogrammetry does to it. Its water is bumpy (reflections
defeat the stereo matching), so a low sun picks out facets and blobs on a
bay — real geometry, which a sun above ~20° doesn't show. And the imagery has
its own shadows baked in from whenever it was flown; ours land on top, which
reads fine at a low sun (long ours, short theirs) and is what `shade` is for.
The shadow pass drops any triangle straddling a masked octant, where the
colour pass merely pushes it back: a coarse triangle sitting above the fine
surface would otherwise shade it. `?shadowdebug=2` shows the facing term
alone, `3` the cast shadows alone.

#### Glamour mode

**I** opens the `glamour` panel. Its sliders — `lens blur`, `soft sun` —
only act while *accumulate* is on, and are locked and dimmed otherwise
(`soft sun` also while shadows are off). With it on, the engine keeps
rendering while the camera sits still and averages the frames, each drawn
through a different sub-pixel offset (a Halton sequence, so the average is a
proper supersample — edges, crack lines and distant detail all settle) and,
with an *aperture*, from a different point on a lens disc, aimed so the point
under the crosshair stays put: thin-lens depth of field, physically, with
real bokeh rather than a depth-weighted blur. It converges in about a second
(256 samples by default), then holds. Anything that changes the picture — a
move, a tile arriving, the drawn set changing — starts it over. Sample 0 is
the plain frame with the occlusion (below) on it, which is applied from the
first sample: what switching it on changes at once — about a tenth darker on
average, all of it in street canyons and at the feet of buildings — is that,
before anything has accumulated.
The miniature's blur pass composes with it (the pass is what gets summed).
Each sample also jitters the sun across its disc (`soft sun`: the real
one's 0.27°, or larger for an overcast softness) and the shadow map's box
across a texel, so shadow edges come out as true penumbras, and turns the
ambient occlusion's sampling pattern, so its noise averages away.

**Ambient occlusion** (the sky panel's `occlusion` row) runs in real time
too: a spiral of taps through the depth buffer out to a radius in metres (a
twenty-fifth of the distance, 0.5–150 m — street canyons need the top of
that), each occluding by how far it rises above the surface's plane — past
~15°, so the photogrammetry's bumpy water mostly doesn't count — and not at
all past the radius, so silhouettes don't halo. It is computed at half
resolution, storing each texel's depth beside its occlusion; the spiral
turns per pixel, a depth-aware 5×5 blur at that same resolution removes the
pattern (a 3×3 one applied at full size left it in as a stipple), and a 2×2
depth-aware upscale puts it on the scene — falling back to the texel nearest
in depth where none matches, since falling back to none left white gaps
along edges. In glamour mode the blur is skipped: the average over samples,
each spun differently, does the smoothing. It fades out with
distance twice: where the haze has taken over (it is applied after the haze,
and darkening what the haze had washed out drew dark lace over distant
mountains), and where the depth buffer can no longer resolve its radius
(24 bits over a one-metre near plane resolve hundreds of metres at 100 km;
occlusion measured from that is quantization). For it the scene is drawn
offscreen into 4× multisampled renderbuffers, resolved into the textures
the passes read, so the context's antialiasing survives — and the miniature
and glamour mode, which used to draw straight into textures, got theirs
back.

It is **off by default**, because it isn't cheap, and most of what it
costs isn't the occlusion. `node tools/occlbench.mjs [shot]` measures it
fairly — gpu timer queries count elapsed gpu time, including whatever else
the gpu is doing (another tab, another app), so runs minutes apart drifted by
2× and only interleaved ones compare; it loads a view once and cycles the
configurations live. At 2560×1600, gpu ms per frame:

| | total | scene | occlusion | resolve |
| --- | --- | --- | --- | --- |
| manhattan, off | 6.2 | 6.2 | – | – |
| full resolution, 16 taps | 19.0 | 9.3 | 6.8 | 2.9 |
| half resolution, 16 taps | 15.4 | 8.2 | 4.4 | 2.9 |
| half resolution, 8 taps | 15.0 | 8.1 | 4.0 | 2.9 |
| seattle, off | 17.3 | 17.3 | – | – |
| half resolution, 16 taps | 31.2 | 19.5 | 6.5 | 5.2 |

Half resolution (the setting; `?occlres=1` for full, `?occltaps=` for the
taps) saves 2–2.5 ms of the pass. The rest is structural: the scene gets
2–3 ms slower drawn into an offscreen multisampled buffer than into the
canvas, and the resolve costs 3–5 more. That's apple's tile-based gpus: the
canvas's own msaa resolves on chip for almost nothing, while an offscreen
multisampled buffer writes all its samples out to memory and resolves them
in a separate pass (`glInvalidateFramebuffer` after the resolve, the usual
hint that they can be dropped, changed nothing here). Glamour mode, which
renders offscreen anyway, pays only the occlusion pass. `?occl=1` turns it
on from the url; `?aodebug=1` shows the occlusion alone, which doubles as a clay render.
`lens blur` is the aperture, in metres of eye travel: off is a pinhole, supersampling only;
half a metre is a camera; tens of metres is a tilt-shift toy. Part of the
saved view; `?glamour=1&aperture=m` from the url.

The running average lives in a float texture (`EXT_color_buffer_float`;
without it the mode stays off and says so), each sample blended in at weight
1/n. Not a sum: a half float summing 128 frames rounds every addition to a
sixteenth once past 64, and the error, varying with the value, drew contour
bands across the sky. The
scene itself is drawn to the miniature's offscreen target, which it shares.
The walk's cache key deliberately ignores both offsets — and the clip planes
are taken from the unmoved eye — because a key that moved with the lens
re-ran the walk every sample and reset the average every frame. Not in tube
or tunnel mode, whose walk never reuses.

#### Body mode

Draw a rect on the map (shift-drag) and **B** wraps it onto a human figure
standing on the rect's centre, the way **T** rolls it into a tube. It is the
tube pipeline with a different warp: the same rect-driven tile selection, the
same ground lock before anything moves, the same curl animation — here a
morph from the flat sheet to the skin — and the same airplane-frame flight
once it's up. The camera spawns out in front of the figure at chest height,
far enough back to see all of it. B again unwraps; T switches to the tube; esc
clears the rect. Only one of the two shapes is ever up.

Unlike the tube, the land around the figure stays: it is drawn flat, and the
rect itself, the ground whose skin is now up on the figure, is painted
near-black — a facet normal from the screen-space slope, catching a little
of a fixed light so the relief still reads. Without the surroundings there
was nothing on screen but the figure, and turning the camera was
indistinguishable from sliding the figure across the view. The walk keeps a
tile if it is in view (drawn flat) or touches the rect (drawn wrapped); a tile
straddling the edge is drawn both ways, the wrapped pass discarding what lies
outside the rect.

The figure is [web/body.obj](./web/body.obj), the skin of MakeHuman's base
mesh (explicitly CC0, per its own header;
[tools/bodymesh.mjs](./tools/bodymesh.mjs) extracts it from the upstream
file). It is fetched the first time B is pressed and baked in the page into a
**geometry image**: a 256×256 grid over (angle around the figure, height from
feet to head) holding a skin position and outward normal per cell. Each cell
is a ray cast outward at that height and angle from the centre of the mesh's
cross-section there, and the outermost hit is the skin. The centre is
per-row and smoothed, not a single spine: the head and neck lean well forward
of the hips, and a ray from the wrong centre grazes the skin instead of
crossing it.

One loop per height can't make limbs. A rectangle with its long edges glued
is a cylinder; rays cast from between the legs miss front and back, and
bridging those misses webbed the two legs into a gown, while at an arm's
height the outermost hit is the hand and the sheet stretched from torso to
fingertip in a flat wing. So the rows are in **bands**, and a band's loops
split the rect's width between the parts of the figure at that height, each
part with rays from its own centre. Below the crotch (the highest row where
rays straight forward and back both miss) the left half of the width wraps
the leg beneath the torso's left side and the right half the other. Where the
arms hang free of the torso the outer fifth of the width on each side wraps
that arm and the middle the torso. Which piece of a cross-section is torso and
which is arm goes by connected loop, not by a width cutoff: the longest closed
loop is the torso, any other loop off to a side is an arm. A cutoff couldn't
tell the underside of an arm at the armpit from the torso's own side, and
taking it as torso put a ledge there that the terrain stretched across. Everywhere else it is one loop with the
seam down the spine. Each limb's loop starts at its inner side, so the joins
sit against the body.

The joins are where the terrain would smear: a tile triangle whose vertices
land on different legs spans the gap between them. So the shader knows the
bands too. It clamps its samples to the texel's own loop, and every vertex
carries the loop's id as a varying, which interpolates to a fraction inside
any triangle that spans two loops, and the fragment shader drops those. The
figure therefore has tears at the crotch, the shoulders and the armpits, one
triangle wide, and no webs. Rays that still miss inside a loop are bridged
across the gap within it.

The vertex shader then maps a rect-local point the obvious way — its
along-axis coordinate to height, its across coordinate to angle, its terrain
height pushed out along the normal at true scale — sampling the two images
bilinearly by hand (float textures don't filter in WebGL2). The figure is as
tall as the rect is long, so a rect about 1.7× longer than it is wide wraps
with the least stretch; anything else pulls the imagery around the waist or up
the body. The crown and the soles are where the parameterisation collapses,
and the far ends of the rect crumple there. The seam runs down the back.
Downloads in tube and tunnel mode are ordered by the same distance through
the roll the lod uses, with nodes whose rolled position can't be on screen
pushed to the back. Ranked by straight-line distance, as the flat view is, the
wall in front of the camera — ground a long way off, flat — queued behind the
tiles under and behind the camera that weren't on screen, and turning the
tunnel on went blurry for seconds (the lod overlay showed it: everything in
view yellow, *not yet requested*). Now the view in front resolves first.

`tube_t::bodyWarp` is the CPU twin the lod walk measures distances
through, and the map marker is placed by the nearest skin cell to the camera,
unwrapped to its spot on the rect.

Drop any `.obj` (y up, any scale) on the window and it replaces the figure,
mid-flight included. G (the tube's wall hug) is off here: the walker is the
cylinder's.

#### STL export

**3** opens the stl panel, which saves the rect as a closed solid for
printing. Binary STL in millimetres, x east, y north, the base on z = 0. The
panel sets the model's longer side, its detail, how much it's thickened, a
vertical exaggeration, and the base's thickness under the lowest ground; the
readout gives the scale, the rect's tile progress, and once everything is
in, the model's height and about how long the wrap will take. While it runs
the button cancels it, and the readout shows how far along it is.

With the panel up, shift-drag picks the rect without rolling it — the drag
is the export's then, and T and B still roll it. Closed, the drag rolls a
tube as it always has.

**The mesh, wrapped.** Google's tiles are a patchwork of levels that
overlap, share no vertices across tile edges, and leave cracks between
them, so they can't be stitched into a solid as they are. The export
shrink-wraps them instead, with [CGAL's 3D alpha
wrapping](https://doc.cgal.org/latest/Alpha_wrap_3/) ([earth_stl.h](earth_stl.h)):
a ball of radius alpha (the panel's *detail*) is rolled over the triangles
from outside, so a gap narrower than alpha is sealed and a wider one stays
open, and the surface it leaves stands *thicken* off the triangles, so
nothing comes out thinner than twice that. The output is closed, manifold,
and doesn't cross itself, whatever the input was. Overhangs survive: tree
canopies, the street under an overpass, a building's cantilevered top. The
defaults (0.6 mm, 0.2 mm) are sized for a 0.4 mm FDM nozzle.

The terrain is a sheet, not a solid, and wrapped on its own it would come
out as a thin skin. So the triangles are clipped to the rect plus a margin
of a few alpha, curtains drop from the clipped edge to a floor below the
model, and the floor closes it; what the ball wraps is then a solid. After
the wrap the solid is cut to the rect and the base's bottom, which throws
the margin, curtains and floor away, so the sides are the wrap's real
cross-section (a tower on the rect's edge shows its section) and the bottom
is one plane. The cut can leave loose bits — a canopy whose trunk was
outside — and only the largest piece is kept.

It's slow: a 300 m block of downtown Seattle at 150 mm took two and a half
minutes in the browser at the defaults (350k wrap points, the first 40 s of
it CGAL setting up). The work goes as the area wrapped over
alpha², walls included, and alpha finer than the tiles' own triangles buys
little. It runs on a thread of its own. CGAL is compiled into the engine
for this (`earth_stl.cpp`, built separately, since it's a minute to compile
and needs wasm exceptions for its filtered predicates and round-to-nearest
interval arithmetic, wasm having no rounding modes); `setup.sh` fetches it,
and boost's headers come from emscripten's port. CGAL's alpha wrap is
GPL-3.0-or-later (or a commercial licence), which then covers the build.

While the panel is up the walk wants every node touching the rect down to
the detail the wrap can use, whatever the camera is looking at — the ground
column's trick over an area — so the rect fills in from wherever you are.
That detail is a quarter of alpha per texel: a tile's vertices sit 4–5 of
its texels apart at every level (measured over downtown seattle), so that
puts a vertex about every alpha. A big rect at fine detail would want more
tiles than the heap holds, so the descent stops at 3000 nodes and the
readout says `(capped)`; a smaller rect or a coarser detail gets it back.
The rect's nodes outside the view stay resident without being drawn.

The triangles handed to the wrap are the ones the draw would draw: a node's
triangles in octants its loaded children cover are left out. Heights are
measured from the planetoid sphere, the mesh's own datum, so the earth's
curvature is out of the model. `view.getExportSoup(padM)` returns the same
soup, for looking at offline.

Open water prints its floor. The mesh carries bathymetry under it — a rect
in the middle of Elliott Bay comes out 40–90 m down, sloping toward the
deep water, and the renderer's own picks agree.

#### Render tests

[tools/](./tools/) captures the 3d view headlessly and builds a side-by-side
report against a set of committed reference images. It tests the web build —
the thing we actually ship — by driving the real page in Chromium.

```
cd tools && npm install && npx playwright install chromium firefox webkit  # once
node tools/capture.mjs          # capture every shot into tools/out/current
node tools/report.mjs --open    # build + open the comparison report
node tools/capture.mjs --bless  # promote the current captures to reference
node tools/browsers.mjs --open  # render every shot in all three browsers
```

There are two harnesses because there are two questions. `capture.mjs` +
`report.mjs` ask *"did my change alter rendering?"* — one browser, against
blessed references. `browsers.mjs` asks *"does it still render right
everywhere?"* — every browser, against each other, no references at all. Both
answer with images, since an image is the assertion that needs no advance
knowledge of how a thing might break.

The views live in [tools/shots.json](./tools/shots.json) — add one and it gets
captured. `capture.mjs` takes a shot name to do just that one.

The report sorts by pixel diff, most-changed first, with per-card wipe, blink,
and diff-heatmap views. **The pixel diff is triage, not a verdict**: it cannot
distinguish "Google reshot the imagery" from "we broke the shader", so its only
job is to put the suspicious shots in front of a human first. Two signals next
to it are categorical:

- **the drawn-node set** — the direct output of lod selection, frustum culling
  and eviction, exported via `getDrawnNodes()`. Text, so it diffs readably, and
  immune to both imagery churn and gpu-dependent DXT decoding. If pixels moved
  but the node set is identical, it's almost certainly new imagery.
- **the tile-load badge** — `nodesLoaded/nodesWanted` on every shot.

That second one matters more than it looks. `getStats().sceneComplete` is not something
you can simply wait on: a download that keeps failing resets its node to a
*stub* rather than to a failed state (`setFailedDownloading` in
[rocktree_types.h](./rocktree_types.h)), so it is retried forever and the flag
can never arrive — which is why the interactive tube path has its own 20s
bailout. The driver therefore always has a timeout, and a capture that trips it
is still written, just labelled `INCOMPLETE` with its tile count, rather than
silently passing off a half-loaded frame as a golden.

#### The lod overlay

**L** tints every octant where a finer tile is wanted and not yet drawn, and
the colour says where that tile's request is — which is the question the last
round of debugging kept having to answer from console logs:

- **yellow** — not yet requested: queued behind the 32 download slots
- **blue** — being looked up in the tile cache
- **red** — on the network
- **green** — bytes in hand, in the decode pool
- **magenta** — a request failed; waiting out the retry delay

The engine only knows stub / downloading / downloaded, so the page's fetch
hook reports the cache-vs-network step back per request
(`Module.noteFetchPhase`), and the engine packs a phase per octant into the
mask uniform the shader already had. A view that sits blue is the cache being
slow; one that sits red is the network; a lot of yellow for a long time is
the slot count or a slow pipeline ahead of it.

`?test=1` on [web/index.html](./web/index.html) is what makes this work: the
pose comes from the query string instead of localStorage, nothing is saved
back, the 3d view takes the whole window, and `view.frame()` gets a fixed dt so
eviction and the animation eases don't depend on the machine's frame rate.
`window.__test` (`settle`, `shot`, `stats`, `drawnNodes`) is the driver's API
and is also handy by hand from the devtools console.

Two runs come out bit-identical (0.00%), because captures are pinned to one
config: headless Chromium, which rasterises with **SwiftShader on the CPU**,
not the machine's GPU. That's deliberate — no driver variance, and it would run
on a GPU-less CI box — but it means the goldens are only comparable against
that same config. Each sidecar records the renderer string, and the report
flags a pair whose renderers differ, since a rasteriser change moves pixels on
its own. `--gpu` captures headed instead (ANGLE/Metal on a Mac) if you want a
real-GPU set; it needs its own references.

`tools/reference/` is committed (~7MB for six shots); `tools/out/` is not.

#### Cross-browser check

`node tools/browsers.mjs` renders every shot in Chromium, Firefox and WebKit
and builds `tools/out/browsers.html` with the engines side by side.

This has no reference images and nothing to bless: the columns are compared
**to each other within one run**, so imagery drift hits every column equally
and cancels out. Chromium is the anchor column only because there has to be
one; it isn't a source of truth.

Cross-engine diffs are never zero — rasterisers round differently — so the
percentages are a sort key, not a threshold. The point is that a person looks
at three columns at once, which is the only thing that catches the failures no
assertion would think to check for: garbled UV interpolation, z-fighting, a
channel swap, missing crack-fill lines, geometry inside out. `--browsers
chromium,firefox` narrows it.

Two things worth knowing from the current run. All three engines report s3tc,
highp and WebGL2, and Firefox and WebKit use the **real GPU** (Apple M1 / Apple
GPU) where headless Chromium uses SwiftShader — so Firefox doubles as a
free real-hardware capture. And drawn-node counts come out *identical* across
all three on every shot, which is the useful invariant: tile selection is
browser-independent, so any visual difference between columns is rasterisation
alone, never engine logic.

Because no available browser lacks s3tc, the `seattle-jpg` shot passes `?jpg`
(`forceJpgTextures`) to force the no-s3tc JPEG texture path in `renderInit`
explicitly. The shader's `mediump` branch still can't be reached here at all:
desktop GPUs execute `mediump` as fp32 regardless of what the shader declares,
so the fp16 UV garbling it guards against only appears on real mobile silicon.

There are no fixtures: tiles come from Google's live servers, so imagery does
drift under you. That's a deliberate trade — recognising "the imagery updated"
by eye is easy, and a frozen tile corpus is real ongoing weight.

#### Shimmer

`node tools/shimmer.mjs [shot]` measures temporal aliasing — the crawling you
see on distant detail while the camera moves, which a still frame cannot show
and which eyeballing cannot compare fairly across configurations.

It settles a view, captures, nudges the heading by a fixed 0.05 degrees (about
one pixel), captures again, and scores the mean absolute pixel change over the
non-sky pixels — whole frame, and the upper half separately, since minification
aliasing is a distance problem the near field dilutes. The absolute number is
meaningless; the ordering across configs is the point, because the scene and
the nudge are identical.

Every config is gated on the scene actually finishing: a score taken on a
half-loaded frame measures which tiles happened to arrive, not how the renderer
filters, and the row is marked meaningless rather than reported. That gate is
load-bearing — without it the MSAA rows moved by more than the effect being
measured.

Result on `seattle-skyline` (lower is steadier):

| config | whole frame | distant half |
| --- | --- | --- |
| baseline | 7.83 | 13.41 |
| mipmaps + 16x aniso | 7.83 | 13.40 |
| msaa only | 6.84 | 11.52 |
| mipmaps + msaa | 6.83 | 11.51 |

Mipmaps do nothing here and MSAA does the work, which says the shimmer on
distant neighbourhoods is **geometric** — sub-pixel building silhouettes — not
texture minification. Texture filtering cannot touch that. Multisampling is on
by default as a result (4x is granted in all three browsers, and 4x is also
`MAX_SAMPLES`); `?msaa=0` opts out. It's a context attribute, so unlike the M
key for mipmaps it needs a page load rather than a live toggle.

One caveat on the metric: MSAA blurs, and blurrier images differ less under any
change, so some of that 13% is the measurement rather than the phenomenon — the
blink view in the report is the check. And on cost: `fps` stayed at 60 and draw
submission was flat or lower with it on, but that's SwiftShader at 1024x640
hitting vsync, which means "not the bottleneck here", not "free". Retina
resolution or a 90Hz stereo headset is a different budget, and WebGL 2 now
offers `EXT_disjoint_timer_query_webgl2` if a real GPU number is ever needed.

It writes `tools/out/shimmer.html`, which defaults to **blink** — and blinking
the nudge pair *is* the shimmer, since the two frames are the same scene one
pixel apart, so whatever jumps is the aliasing you'd see as crawl in motion.
Below those it puts the head-to-heads at a fixed pose (msaa off vs on, mipmaps
off vs on) for judging static quality instead.

Unlike `capture.mjs`, this shares one browser context across configs so the
IndexedDB tile cache is reused; only the first config pays the download. Even
so a run is minutes, so `--report-only` rebuilds the page from the last run's
saved frames without re-measuring.

Each config reports the tile counts for both captures. A score taken on a
partly-loaded scene measures which tiles happened to arrive rather than how
the renderer filters, so that's checked rather than assumed — but only tile
completeness invalidates a score. `sceneComplete` holding across consecutive
frames is stricter than that and gives false alarms, because a nudge can shift
the wanted set by a tile and make the flag flicker on a scene that is in fact
fully loaded.

#### Profiling

The engine's per-frame accounting is exported to JS as `view.getStats()`:
section times (`bfsMs`, `dlMs`, `evictMs`, `drawMs`), the bfs work counters,
and the node want/have/drawn counts. It's also still printed to the console
every 2s. `drawMs` is command-*submission* time, not GPU time — WebGL is
asynchronous, so it measures the cost of crossing into the browser, which the
per-node bind sequence in [rocktree_gl.h](./rocktree_gl.h) dominates. Real GPU
timing would need `EXT_disjoint_timer_query_webgl2`, which is WebGL2-only.

`capture.mjs` records these into each shot's sidecar json, so the report shows
them as reference→current alongside the images.

**GPU time** is measured with `EXT_disjoint_timer_query_webgl2` (Chrome on a
Mac has it, with 64-bit counters). The frame is cut into sections — the
shadow pass, the scene, the occlusion, the other post passes (the msaa
resolve, glamour's accumulation, the miniature's blur) — each a run of
time-elapsed queries, since those can't nest, read back a few frames later
when the gpu has finished and averaged per frame over a second. They're in
`getStats()` (`gpuMs`, `gpuShadowMs`, `gpuSceneMs`, `gpuOcclusionMs`,
`gpuPostMs`; -1 without the extension), on the console's 2-second line, and
printed by `tools/shot.mjs`. The shadow pass reads 0 for a still camera,
which reuses the last map.

#### Load profiler

`node tools/loadprof.mjs` answers *"why is this view slow to finish?"* — a
question a settled screenshot can't. It drives one pose headlessly and prints
a timeline, twice a second, of what the engine wants, has, and has in flight,
what the page's fetch layer is waiting on (cache lookup vs network, per kind),
and the frame's wall-clock cost split by section; then, complete or timed out,
the list of what the walk still wanted and didn't have, grouped by level and
state with how long each had been waiting.

```
node tools/loadprof.mjs --session ~/path/to/session.json   # a ⌘C session blob's pose
node tools/loadprof.mjs seattle-skyline                    # a shots.json entry
node tools/loadprof.mjs --lat .. --lon .. --alt .. --heading .. --tilt .. --fov ..
```

`--noidb` bypasses the tile cache, `--roidb` looks up but never writes,
`--warm` keeps a persistent browser profile (and a fixed port — the cache is
keyed by origin) so the store carries over between runs, `--trace` records a
per-tile timeline (issue → cache → network → engine → decode), and
`--cpuprofile` samples the main thread with the devtools profiler and prints
the functions with the most self time; build with `EARTH_PROFILE=1 ./build.sh`
to see the engine's function names in it rather than `wasm-function[1234]`.
`--q k=v` passes any page query parameter through.

`node tools/shot.mjs <shot> <out.png>` is the one-pose version: settle on the
real gpu, save a png — `--session`, `--q k=v` and `--glamour n` (accumulate
that many samples first) as above, a 180 s deadline, and page errors (a
shader that fails to compile) printed rather than swallowed. The warm tools
share a fixed port, 18765, since the tile cache is keyed by origin.

It runs in **full chrome in headless mode** (playwright's `channel:
'chromium'`), not the headless shell the capture harness uses, and that is not
a detail: the shell rasterises on the cpu with swiftshader, where a big
multisampled frame blocks the main thread for hundreds of ms, and a blocked
main thread starves the very fetch and indexeddb callbacks being measured. The
first profile of the toronto view, taken on the shell, showed tiles arriving in
bursts of 48 every fifteen seconds; the same page on the gpu, in the same
process, showed the real problem (the cache's write queue, above). Full chrome
drives the real gpu — Apple M-series Metal here — with no window.

What the load of a view costs, measured with it:

- the **cache's write backlog** was the whole story (above). Everything below
  is second order next to it.
- a horizon view at a narrow fov is **inherently big**: the 7° toronto pose
  wants ~1850 tiles, 110 MB, because lod is by distance and every tile out to
  the horizon is seen edge-on, drawn hundreds of texels tall into a pixel.
  That's the standard rule and the one that keeps the horizontal resolution
  right; it's just what the view asks for. On this connection it's bandwidth
  bound at ~7 s.
- **horizon culling** was letting through everything within 450 km *past* the
  horizon: the occluder sphere sat 16 km under the datum. The meshes sit on the
  datum sphere (not the ellipsoid — their altitudes against wgs84 track the
  sphere's offset by latitude, +4.4 km at seattle) and no fine tile's box
  reaches more than ~300 m under it, so it now sits 1.5 km under. The slack
  still counts twice (the eye's horizon on the smaller sphere, then the depth
  behind it), so terrain out to ~290 km still passes; a third of this view's
  wanted tiles were beyond the horizon before, 7% fewer are wanted now, and
  the drawn-node sets on the reference shots lose only far nodes with no
  pixel change.
- with the cache fixed the **main thread** is what's left: at 1300 drawn nodes
  the walk was 4–10 ms a frame and draw submission 10+ ms, and a saturated
  main thread starves fetch callbacks just as the cache did. The walk's
  result is now kept between frames and reused while the camera, viewport and
  tree are unchanged (`g_tree_epoch`), which takes it to zero for a camera
  that sits still waiting for tiles. The per-node draw sequence went from 15
  webgl calls to 11 (a vao per mesh, the octant masks as one int each, the
  sampler unit set once, the mipmap filter re-sent only when the M key flips
  it); what remains is dominated by the uniform uploads and the draw calls
  themselves, which only batching across nodes would remove.

#### Deploy

Push to main. [.github/workflows/deploy.yml](.github/workflows/deploy.yml)
builds from source and publishes `web/` to GitHub Pages as an artifact. No
build products are committed; the deploy job needs the build job, so a failed
build publishes nothing.

The page isn't built — `index.html`, `style.css` and `js/` ship as written,
native es modules and no bundler — so the toolchain exists only to produce
`earth.js`/`earth.wasm`. Those are cached under a key hashing the c++ sources
and the scripts that build them, and a run that changes neither
skips the build outright: checkout, restore, upload. That's most pushes, and
it's the difference between about 90 seconds and about 15. Touching a header
(or aging the entry out of the cache) builds and repopulates it.

The build is five steps and two caches of its own (emsdk, its ports, the deps
`setup.sh` compiles), and it lives in
[.github/actions/build-engine](.github/actions/build-engine/action.yml) — a
composite action, which is how a multi-step thing becomes one step with one
condition. Whether to build is then a single decision in one place, rather
than the same `if` copied down five steps.

To watch a run:

```
gh run watch --repo joshuahhh/soil
```

#### Stuff we've removed

This fork used to carry upstream's native desktop client and a standalone
emscripten app. We only use the web version these days, so they were stripped.
Everything below lives in git history — commit `e92c1f1` is the last one that
has it all. (Back then the engine lived in a `client/` subdirectory, since
unwrapped into the repo root; the paths below are the historical ones.)

- `client/main.cpp` — the native SDL app (macOS Metal / OpenGL, Linux GL),
  which was also the standalone `./build.sh emscripten` target; `client/shell.html`
  was its web shell
- `client/rocktree_metal.h` — the Metal render backend (default on macOS
  natively), plus the `EARTH_METAL` backend selection in `rocktree_types.h` /
  `rocktree_util.h` and the `-DEARTH_USE_GL` A/B switch
- `client/gl2/` — glad OpenGL loader for Windows/Linux native builds
- `client/bench.sh`, `client/bench_plot.py` — the perf-regression harness
  (`./main --bench` against reference frame captures). The untracked bench
  data (`client/bench/`, `view_*.json` at the repo root) was never in git;
  the view captures are archived in `../native-bench-archive.tar.gz`
- the native and `emscripten` branches of `client/build.sh`
- `exporter/` — upstream's Node.js model exporter (lat/long → octant lookup,
  octant → textured `.obj` dump for Blender), untouched since upstream
- `header.png` — the old README header image

Removed later (`git log -- http.h` finds the commit):

- the native fetch path: `http.h`, the `./cache` disk cache in
  `rocktree_http.h`, and the thread pool that fetched through it
- the `EARTH_*` environment switches (`EARTH_AUDIT` with `EARTH_DUMP_TEX` and
  `EARTH_PROBE`, `EARTH_TRACE`, `EARTH_NO_MASK`, `EARTH_NO_LINES`,
  `EARTH_FULL_LAYERS`, `EARTH_LINE_WIDTH`, `EARTH_LOD_SCALE`), which nothing
  sets in the wasm build

Note the web build still uses `rocktree_gl.h` (GLES2), `rocktree_http.h`, and
`threads.h` — those look native-ish but are shared.

#### Reverse-engineering notes (upstream)

URL structure:
```
"https://kh.google.com/rt/🅐/🅑"
 - 🅐: planet
       - "earth"
       - "mars"
       - ...
 - 🅑: resource
       - "PlanetoidMetadata"
       - "BulkMetadata/pb=!1m2!1s❶!2u❷"
          - ❶: octant path
          - ❷: epoch
       - "NodeData/pb=!1m2!1s❸!2u❹!2e❺(!3u❻)!4b0"
          - ❸: octant path
          - ❹: epoch
          - ❺: texture format
          - ❻: imagery epoch (sometimes)
```

Misc:
```
General info:
 - Everything is stored in an octree.

Roles of resources:
 - PlanetoidMetadata points to first BulkMetaData.
 - BulkMetaData points to other BulkMetaData and to NodeData.
 - NodeData contains actual meshes and textures.

Versioning:
 - BulkMetaData and NodeData are versioned using epoch numbers.
 - PlanetoidMetadata provides epoch of first BulkMetaData.
 - BulkMetaData provides epochs of underlying resources.
 - Current version of a resource can be determined recursively.

NodeData:
 - Mesh: packed XYZ, UV, octant mask, normals
 - Texture: JPG, CRN-DXT1
 - Raw format: see rocktree.proto and rocktree_decoder.h
 - Other optimizations: BVH
BulkMetaData:
 - Oriented Bounding Box
    - Dump OBB to obj: https://gist.github.com/retroplasma/5698808bfaa63ffd03f751a84fa6ce14
    - Latlong to octant using OBB (unstable): https://github.com/retroplasma/earth-reverse-engineering/blob/443a3622ce9cb12cd4460cc6dc7999cc703ae67f/experimental_latlong_to_octant.js
```
