Reverse-engineering undocumented parts of Google Earth, forked from [retroplasma/earth-reverse-engineering](https://github.com/retroplasma/earth-reverse-engineering) (now archived) and violently vibe-coded.
This fork is web-only: the C++ engine compiles to WebAssembly
and renders in a split-screen demo next to a MapLibre slippy map.

#### Layout

- repo root — the engine (C++ → wasm via emscripten, `earth_web.cpp` +
  `rocktree_*.h`) and its build/deploy scripts
- [web/](./web/) — the app (`index.html`) and the build's js/wasm output
- [proto/](./proto/) — protobuf schema for Google Earth's "rocktree" data

#### Build

```
./build.sh
```

Produces `web/earth.js` + `web/earth.wasm` (modularized, pthreads). Serve
`web/`, e.g.:

```
python3 -m http.server -d web 8000
```

`coi-serviceworker.js` injects the cross-origin-isolation headers the pthread
build needs (expect one automatic reload on first visit). Requires emscripten
plus a protobuf build for it (`config_emscripten.sh` sets the paths).

#### Deploy

```
./deploy-soil.sh "message"
```

Builds, sanity-checks the artifacts, copies them into the sibling `soil` repo
(GitHub Pages), pushes, waits for the Pages build of that exact commit, and
smoke-tests the live wasm. Each step is gated on the previous one succeeding.

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
