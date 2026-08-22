Reverse-engineering undocumented parts of Google Earth, forked from [retroplasma/earth-reverse-engineering](https://github.com/retroplasma/earth-reverse-engineering) (now archived) and violently vibe-coded.
This fork is web-only: the C++ engine compiles to WebAssembly
and renders in a split-screen demo next to a MapLibre slippy map.

#### Layout

- repo root — the engine (C++ → wasm via emscripten, `earth_web.cpp` +
  `rocktree_*.h`) and its build/deploy scripts
- [web/](./web/) — the split-screen app (`index.html`), the phone camera-match
  page (`ar.html`), and the build's js/wasm output
- [proto/](./proto/) — protobuf schema for Google Earth's "rocktree" data

#### Build

```
./setup.sh   # once per machine: fetches pinned deps into ./deps
./build.sh
```

`setup.sh` downloads pinned releases of eigen and protobuf and compiles
libprotobuf to wasm; it needs emscripten and curl. `build.sh` then produces
`web/earth.js` + `web/earth.wasm` (modularized, pthreads). Serve `web/`:

```
./serve.py
```

(a thin http.server wrapper on port 8000 that disables caching, so browsers
never run a stale wasm after a rebuild)

`coi-serviceworker.js` injects the cross-origin-isolation headers the pthread
build needs (expect one automatic reload on first visit).

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

**Draping is the interesting part.** A track laid at its own elevations
wanders through hillsides: GPS altitude is worth tens of meters on a good day,
and the geoid separation it's measured against is worth a hundred more. So
each point is re-measured against the mesh with the same downward raycast
terrain-hug uses (`groundRadiusUnder`) and pinned to the ground there. That
measurement only answers where fine mesh is currently resident *and drawn*, so
it runs a couple dozen points a frame and keeps what it learns: a track fills
in onto the terrain over the first seconds and stays put after. Turn the
switch off to see the file's own elevations instead — the gap between the two
is a fair picture of what a GPS watch actually knows about altitude.

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

It steers off a smoothed copy of the track, never the track itself. A GPS fix
wanders a few meters between samples, and taking the heading from one raw
segment hands every one of those wobbles to the camera, which reads as the
world shivering. So the track is resampled at a fixed 10 m step, boxcar-
smoothed twice (two passes ≈ gaussian) over a 60 m window, and the heading is
taken from a 300 m chord across *that* rather than from any one segment — with
a half-second temporal ease on top for what survives. The aim height comes from
`getPathGroundAlt`, which reports the drape's measurement where it has one and
NaN where it hasn't, so the camera rides the terrain rather than the track's
GPS altitudes, easing between the two as the measurements land.

**V rides the track.** The camera goes on rails: it looks at a point that runs
along the path, W/S drive that point forward and back, the arrows swing the
camera around it (R/F change how far off it stands), and A/D do nothing —
there is nowhere sideways to go. The azimuth is measured from the *path's* own
heading rather than from north, so rounding a bend carries the camera around
with it and you keep looking at the same shoulder of the trail.

The camera never steers off the track itself. A GPS fix wanders a few meters
between samples, so the heading between two consecutive points swings wildly
even standing still, and handing that to a camera reads as the world
shivering. So the track is resampled at a fixed 10 m step, boxcar-smoothed
twice (two passes ≈ gaussian), and the heading is taken from a ±150 m chord
across *that* — never from one segment. A temporal ease on the heading and on
the target's altitude absorbs the rest, the altitude one mattering because it
steps whenever a drape measurement lands.

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

That second one matters more than it looks. `sceneComplete()` is not something
you can simply wait on: a download that keeps failing resets its node to a
*stub* rather than to a failed state (`setFailedDownloading` in
[rocktree_types.h](./rocktree_types.h)), so it is retried forever and the flag
can never arrive — which is why the interactive tube path has its own 20s
bailout. The driver therefore always has a timeout, and a capture that trips it
is still written, just labelled `INCOMPLETE` with its tile count, rather than
silently passing off a half-loaded frame as a golden.

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

#### Deploy

Push to main. [.github/workflows/deploy.yml](.github/workflows/deploy.yml)
builds from source and publishes `web/` to GitHub Pages as an artifact, taking
about 90 seconds. No build products are committed; the deploy job needs the
build job, so a failed build publishes nothing.

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

Note the web build still uses `rocktree_gl.h` (GLES2), `http.h`,
`rocktree_http.h`, and `threads.h` — those look native-ish but are shared.

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
