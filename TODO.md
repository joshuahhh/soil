# todo

- **Update Eigen to 3.4.x.** `setup.sh` pins 3.3.7 (2018), which still uses
  `std::result_of` — deprecated in C++17, gone in C++20. The build papers over
  it with `-Wno-deprecated-declarations` (see build.sh); 3.4.x would let that
  suppression go. It's a one-line version bump in setup.sh plus a cache-key
  bump in .github/workflows/deploy.yml.

- **Mipmap the DXT tiles too.** `bufferMesh` now builds a mip chain (plus 16x
  anisotropy) for uncompressed tiles, but the DXT1 path still samples level 0
  only. `glGenerateMipmap` rejects compressed textures and the CRN files carry
  a single level (`crn_get_levels` returns 1 on every tile sampled), so the
  levels have to be produced CPU-side: decode DXT1 blocks, box-downsample,
  re-encode. crnlib's compressor isn't in the tree — only `crn_decomp.h` is
  compiled — so that's a small DXT1 encoder to write, maybe 200 lines. Levels
  1..n are a third of level 0, so it stays at 4bpp rather than the ~8x memory
  of going uncompressed, which matters given the LRU byte quota.

  Worth knowing before starting: JPEG is *not* just the no-s3tc fallback the
  comments in `rocktree_gl.h` imply. Google serves a large share of nodes as
  JPEG regardless — measured over settled views, grand canyon was 200/200
  tiles JPEG, rainier 137/250, seattle 111/250, manhattan 24/100. So the
  uncompressed path already covers between a quarter and all of a scene, and
  the DXT work closes a gap that varies a lot by region.
