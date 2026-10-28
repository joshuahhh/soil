// the planet engine, independent of windowing and input: owns the rocktree,
// runs the per-frame culling/lod walk, schedules downloads, maintains the
// lru cache, and emits draws through the render backend interface. the shell
// (earth_web.cpp, on a web canvas) owns the camera and the frame lifecycle
// and calls updateAndDraw between renderFrameBegin/renderFrameEnd.
//
// process-wide singletons remain in rocktree_util.h (thread pools, request
// maps), so one instance per process for now.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <math.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "rocktree_util.h"
#include "earth_camera.h"
#include "earth_tube.h"

struct earth_core_t {
	rocktree_t *planetoid = nullptr;

	// true when the last walk saw everything the view wants fully downloaded
	bool scene_complete = false;
	struct download_stats_t {
		// last walk's want/have node counts, for download progress display
		int nodes_wanted = 0, nodes_loaded = 0;
		// the rest of the download picture, reported with the 2s sections
		// line. 'stuck' counts nodes the walk still wants whose request went
		// out and hasn't come back (the line's 'downloading'): normally the
		// ones in flight. a failed transfer comes back as a stub and is
		// retried after a backoff ('failed' counts those), so the thing to
		// watch is stuck staying up with nothing in flight — requests that
		// nothing answered
		int nodes_stub = 0, nodes_stuck = 0, bulk_blocked = 0, max_level = 0;
		int nodes_failed = 0, bulks_started = 0;
		int nodes_shadow = 0; // of the wanted, those kept only to cast shadows
	};
	download_stats_t stat;
	// draw n-times coarser than the standard target. the web shell sets it
	// live (see setLodScale) because the volume of data a view asks for is
	// the one thing that decides whether a deep zoom can finish at all, and
	// that is worth being able to feel
	double lod_scale = 1.0;
	// tint octants where a finer tile is wanted but not yet drawn (L key)
	bool debug_lod = false;

	// the atmosphere: aerial perspective on the terrain and a sky with the
	// sun in it. the host sets the sun (azimuth/elevation at the camera —
	// it has the clock and the place) and the visibility; the colours are
	// derived here from the sky colour and the sun's height, so the haze
	// the terrain fades into and the sky's horizon band are one colour
	bool sky_on = true, haze_on = true, tint_on = true;

	// glamour mode (EarthView::frame accumulates frames while the camera
	// sits still): one sample's offsets, all zero outside the mode. a
	// sub-pixel offset for the projection, in pixels, and a lens offset
	// for the eye, in metres along the camera's right and up, aimed so the
	// point at lens_focus stays put — thin-lens depth of field, one sample
	// at a time. the walk's cache key ignores both. sun and shadow are the
	// shadow pass's (see sun shadows, below)
	struct glamour_sample_t {
		double jitter_x = 0, jitter_y = 0, lens_x = 0, lens_y = 0;
		double sun_x = 0, sun_y = 0, shadow_x = 0, shadow_y = 0;
	};
	glamour_sample_t glamour;
	double lens_focus = 0;
	bool walk_reused = false; // did the last frame reuse the cached walk?

	// sun shadows (see shadowPass): on, and how dark the shade is. glamour
	// mode jitters the sun across a disc (glamour.sun_*: radians, along two
	// axes across it) and the map's box by fractions of a texel
	// (glamour.shadow_*), a sample at a time, for soft penumbras and an
	// anti-aliased map
	bool shadows_on = true;
	double shadow_strength = 0.45;
	static constexpr int shadow_size = 4096;
	double shadow_focus = 0; // metres to the terrain under the crosshair, refreshed when the camera moves
	double sun_soft = 0; // glamour's sun disc radius (radians), which the walk's casters allow for
	static constexpr double caster_texels = 2; // how coarse an off-screen caster may be, in map texels
	// the map's box (see shadowFit): square across the sun (sx, sy) about
	// O, and along it from shadow_near (toward the sun) to far. not live
	// without shadows
	static constexpr double shadow_near = -8000.0;
	struct shadow_fit_t {
		bool live = false;
		Vector3d L = Vector3d::Zero(), sx = Vector3d::Zero(), sy = Vector3d::Zero(), O = Vector3d::Zero();
		double half = 0, texel_m = 1, far = 0;
		// the frustum it is fitted to: its box in the sun's frame (along
		// sx, sy, L) and how deep it reaches along the view
		Vector3d lo = Vector3d::Zero(), hi = Vector3d::Zero();
		double depth = 0;
		bool operator==(const shadow_fit_t &o) const {
			return live == o.live && half == o.half && far == o.far && L == o.L && O == o.O;
		}
	};
	// what the last shadow map was rendered from: when none of it has
	// changed it is still right, and the pass is skipped
	Vector3d shadow_key_l = Vector3d::Zero(), shadow_key_o = Vector3d::Zero();
	double shadow_key_half = 0;
	int shadow_key_drawn = -1;
	double sky_dark = 0; // the tunnel's dimming of the sky (EarthView::frame)
	int uploads_this_frame = 0; // meshes sent to the gpu this frame
	double visibility_m = 60000;
	static constexpr double scale_height_m = 12000; // the haze's exponential fall-off with height
	double sun_az = 225.0 * M_PI / 180.0, sun_el = 35.0 * M_PI / 180.0;
	int sky_rgb = 0x83b5fc;
	// print per-frame scheduler state (bench mode)
	bool log_sched = false;

	// terrain-hug ground column: when the shell sets this, selection always
	// includes the finest tiles in the vertical column under the given point
	// even when the view frustum culls them — the ground query needs real
	// mesh there, not whichever coarse ancestor happens to be on screen
	// (see groundRadiusUnder)
	bool ground_column_on = false;
	Vector3d ground_column_point = Vector3d::Zero();

	// the tube, tunnel and body modes (earth_tube.h), with the engine
	// handed in for what they measure against the meshes
	tube_t tube;
	earth_core_t() {
		tube.engine.planet_radius = [this] { return planetRadius(); };
		tube.engine.mesh_hit = [this](const Vector3d &origin, const Vector3d &dir, Vector3d &hit, double &mpt) {
			return raycast(origin, dir, hit, &mpt) == raycast_mesh;
		};
		tube.engine.ground_radius_under = [this](const Vector3d &p) { return groundRadiusUnder(p); };
	}

	// --- stl export -----------------------------------------------------
	// a geodetic rect whose terrain the page saves as a solid: its
	// triangles go to the alpha wrap in earth_stl.h. while it is set the walk wants every node touching
	// it down to export_mpt, wherever the camera is — the ground column's
	// trick over an area — so the rect fills in at the detail the export
	// asks for and stays resident (wanted nodes are never evicted). the
	// rect's frame: x east, y north, equirectangular about its centre so
	// its edges are exactly x = ±half_e, y = ±half_n; z the height above
	// the planetoid sphere, which is the mesh's own datum
	bool export_on = false;
	double export_latc = 0, export_lonc = 0; // radians
	double export_half_e = 0, export_half_n = 0;
	double export_mpt = 1;
	Vector3d export_up = Vector3d::UnitZ(), export_east = Vector3d::UnitX(), export_north = Vector3d::UnitY();
	int export_epoch = 0; // bumped on every change, for the walk's cache key
	// past this many nodes the walk stops descending for the export: a
	// big rect at fine detail would otherwise want more tiles than the
	// heap holds. the export then takes whatever level the cap allowed
	static constexpr size_t export_node_cap = 3000;
	// the last walk's nodes in the rect (indices into potential_nodes),
	// and its tally of them: wanted, in, and whether the cap cut it short
	std::vector<size_t> export_idx;
	int stat_export_wanted = 0, stat_export_loaded = 0;
	bool stat_export_capped = false;
	double stat_export_mpt = 0; // the finest node the rect has, m per texel

	void setExportRect(double lat0, double lon0, double lat1, double lon1, double mpt) {
		export_on = true;
		export_latc = (lat0 + lat1) / 2;
		export_lonc = (lon0 + lon1) / 2;
		auto R_p = planetoid && planetoid->downloaded ? (double)planetoid->radius : 6371010.0;
		export_half_n = R_p * fabs(lat1 - lat0) / 2;
		export_half_e = R_p * cos(export_latc) * fabs(lon1 - lon0) / 2;
		export_mpt = fmax(0.01, mpt);
		export_up = Vector3d(cos(export_latc) * cos(export_lonc), cos(export_latc) * sin(export_lonc), sin(export_latc));
		export_east = Vector3d(-sin(export_lonc), cos(export_lonc), 0);
		export_north = export_up.cross(export_east);
		export_epoch++;
	}
	void clearExportRect() {
		export_on = false;
		export_idx.clear();
		stat_export_wanted = stat_export_loaded = 0;
		stat_export_capped = false;
		export_epoch++;
	}

	// a tangent-plane test, loose by the node's own radius, so the
	// equirectangular frame's departure from the plane over a few km
	// doesn't matter; the height band is tubeRectTouches'
	bool exportRectTouches(const OrientedBoundingBox &obb) const {
		auto R_p = (double)planetoid->radius;
		auto margin = obb.extents.norm();
		auto h = obb.center.dot(export_up) - R_p;
		return !(fabs(obb.center.dot(export_east)) > export_half_e + margin
			|| fabs(obb.center.dot(export_north)) > export_half_n + margin
			|| h > 9000 + margin || h < -1500 - margin);
	}

	// the draw's octant masks over the rect's nodes: a node's bit is set
	// for each child octant that is downloaded and wanted, and its
	// triangles with all three vertices in such octants are left out, as
	// the draw leaves them out
	std::unordered_map<rocktree_t::node_t *, uint8_t> exportMasks() const {
		std::unordered_map<rocktree_t::node_t *, uint8_t> masks;
		for (auto k : export_idx) {
			auto node = potential_nodes[k];
			if (node->dl_state != dl_state_downloaded || !node->up) continue;
			masks[node->up] |= 1 << node->octant;
		}
		return masks;
	}
	// a globe point in the rect's frame (metres)
	Vector3d exportLocal(const Vector3d &g) const {
		auto R_p = (double)planetoid->radius;
		auto r = g.norm();
		auto dlon = atan2(g.y(), g.x()) - export_lonc;
		if (dlon > M_PI) dlon -= 2 * M_PI;
		if (dlon < -M_PI) dlon += 2 * M_PI;
		return Vector3d(R_p * cos(export_latc) * dlon, R_p * (asin(g.z() / r) - export_latc), r - R_p);
	}
	// every triangle the draw would draw in the rect, widened by pad
	// metres, a mesh at a time: mesh(loc) with the mesh's vertices in the
	// rect's frame, then tri(a, b, c) for each of its triangles there,
	// indices into loc. the levels overlap a little where a coarse
	// triangle straddles an octant edge; the wrap (earth_stl.h) doesn't
	// mind. strips wind every other triangle the other way, and a soup
	// doesn't mind that either
	template <typename MeshFn, typename TriFn>
	void forExportTriangles(double pad, MeshFn mesh_fn, TriFn tri_fn) {
		auto masks = exportMasks();
		auto mx = export_half_e + pad, my = export_half_n + pad;
		// the coarse ancestors' octants away from the rect are unmasked,
		// and a triangle of theirs on the far side of the planet can
		// straddle the frame's ±180° seam, its box then spanning
		// everything. no triangle that belongs in the rect reaches 5 km
		// past it
		auto far = [&](const Vector3d &p) { return fabs(p.x()) > mx + 5000 || fabs(p.y()) > my + 5000; };
		std::vector<Vector3d> loc;
		for (auto k : export_idx) {
			auto node = potential_nodes[k];
			if (node->dl_state != dl_state_downloaded) continue;
			auto mit = masks.find(node);
			uint8_t mask = mit == masks.end() ? 0 : mit->second;
			if (mask == 0xff) continue;
			auto &m = node->matrix_globe_from_mesh;
			for (auto &mesh : node->meshes) {
				auto verts = mesh.vertices.data();
				auto vcount = mesh.vertices.size() / 8;
				loc.resize(vcount);
				for (size_t v = 0; v < vcount; v++) {
					auto q = verts + v * 8;
					loc[v] = exportLocal((m * Vector4d(q[0], q[1], q[2], 1.0)).head<3>());
				}
				mesh_fn(loc);
				for (size_t t = 2; t < mesh.indices.size(); t++) {
					uint16_t a = mesh.indices[t-2], b = mesh.indices[t-1], c = mesh.indices[t];
					if (a == b || b == c || a == c) continue; // strip degenerates
					if (a >= vcount || b >= vcount || c >= vcount) continue;
					if ((mask >> verts[(size_t)a * 8 + 3] & 1)
						&& (mask >> verts[(size_t)b * 8 + 3] & 1)
						&& (mask >> verts[(size_t)c * 8 + 3] & 1)) continue;
					auto &pa = loc[a], &pb = loc[b], &pc = loc[c];
					if (far(pa) || far(pb) || far(pc)) continue;
					if (fmax(pa.x(), fmax(pb.x(), pc.x())) < -mx || fmin(pa.x(), fmin(pb.x(), pc.x())) > mx
						|| fmax(pa.y(), fmax(pb.y(), pc.y())) < -my || fmin(pa.y(), fmin(pb.y(), pc.y())) > my) continue;
					tri_fn(a, b, c);
				}
			}
		}
	}
	// those triangles as a soup: xyz per vertex, three vertices per
	// triangle. false when nothing is loaded
	bool exportSoup(double pad, std::vector<float> &pos, std::vector<uint32_t> &tris) {
		pos.clear();
		tris.clear();
		if (!ready() || !export_on) return false;
		const std::vector<Vector3d> *loc = nullptr;
		std::vector<int64_t> remap; // the mesh's vertices' places in pos, once used
		forExportTriangles(pad, [&](const std::vector<Vector3d> &l) {
			loc = &l;
			remap.assign(l.size(), -1);
		}, [&](uint16_t a, uint16_t b, uint16_t c) {
			for (auto i : {a, b, c}) {
				if (remap[i] < 0) {
					remap[i] = (int64_t)(pos.size() / 3);
					pos.push_back((float)(*loc)[i].x());
					pos.push_back((float)(*loc)[i].y());
					pos.push_back((float)(*loc)[i].z());
				}
				tris.push_back((uint32_t)remap[i]);
			}
		});
		return !tris.empty();
	}
	// the rect's lowest and highest point (metres above the sphere) and
	// the area of its surface (m², walls and all, overlaps counted twice):
	// what the model's height and the wrap's work come from. false when
	// nothing is loaded
	bool exportRelief(double &lo, double &hi, double &area) {
		lo = INFINITY;
		hi = -INFINITY;
		area = 0;
		if (!ready() || !export_on) return false;
		const std::vector<Vector3d> *loc = nullptr;
		auto in = [&](const Vector3d &p) { return fabs(p.x()) <= export_half_e && fabs(p.y()) <= export_half_n; };
		forExportTriangles(0, [&](const std::vector<Vector3d> &l) { loc = &l; },
			[&](uint16_t a, uint16_t b, uint16_t c) {
			auto &pa = (*loc)[a], &pb = (*loc)[b], &pc = (*loc)[c];
			for (auto p : {&pa, &pb, &pc}) {
				if (!in(*p)) continue;
				lo = fmin(lo, p->z());
				hi = fmax(hi, p->z());
			}
			if (in((pa + pb + pc) / 3)) area += 0.5 * (pb - pa).cross(pc - pa).norm();
		});
		return lo <= hi;
	}

	// download slots in flight, held from request through decode; the cap
	// needs to stay comfortably above the decode pool's thread count
	std::atomic<int> nodes_in_flight{0};
	// bulks get their own, much smaller slot count. a bulk is a few KB against
	// a node's ~100KB, but every one of them gates a four-level subtree, so
	// they must not be allowed to queue in front of the node data that
	// actually puts pixels on screen — which is exactly what an uncapped
	// firehose of them did: a deep zoom wants thousands, and both the http
	// connection pool and the page's indexeddb lookup serve first-come
	std::atomic<int> bulks_in_flight{0};

	// nodes actually drawn last frame with their octant masks, kept for the
	// center-pick raycast. refreshed every updateAndDraw; valid to use
	// between frames because eviction and bulk purge only run inside
	// updateAndDraw (shells are single-threaded around the frame loop)
	std::vector<std::pair<rocktree_t::node_t*, uint8_t>> drawn_nodes;

	// the nodes the last walk wanted and does not have, with why: for the
	// load profiler (tools/loadprof.mjs, EarthView::getPendingNodes) to say
	// what a view that won't finish is actually waiting on
	struct pending_t { std::string path; int state; int fails; double dist; float mpt; double wanted_ms; };
	std::vector<pending_t> pending_nodes;
	double last_walk_ms = 0;

	// the walk's results, kept between frames. the walk is a few ms a frame
	// in a heavy view and its inputs rarely change: a camera waiting for
	// tiles sits still, and the tree only changes shape when a bulk lands.
	// so the result is reused until something it depends on moves — the
	// view, the viewport, the lod scale, or the tree (g_tree_epoch) — and a
	// reused frame only re-stamps what it wants, so eviction and the bulk
	// purge still see it. not in tube or tunnel mode, whose selection has
	// inputs of its own (the rect, the curl, the ground lock) that move
	// every frame anyway
	// potential_nodes is in bfs level order, so iterating it backwards
	// visits children before parents (the order the draw loop's octant
	// masking needs)
	std::vector<rocktree_t::node_t *> potential_nodes;
	// tube mode only: per-entry keep priority, parallel to potential_nodes
	// — how far past its lod target a node was requested (r over the
	// required texels-per-meter). the floor and ground-column nodes carry
	// effectively-infinite want. used by the node-budget backstop to shed
	// the least-wanted additions first
	std::vector<double> potential_want;
	struct bulk_candidate_t { double priority; size_t level; rocktree_t::bulk_t *bulk; };
	std::vector<bulk_candidate_t> to_download_bulks; // the stub bulks the walk ran into
	std::vector<rocktree_t::bulk_t *> walk_bulks; // every bulk the walk stamped as wanted
	int walk_bulk_count = 0, walk_bulk_blocked = 0;
	bool walk_cache_valid = false;
	// what the cached walk was made from: the unmoved camera's
	// view-projection and eye, the viewport, the tree, the lod scale,
	// whether the ground column was on, the export rect and the shadow
	// map's box
	struct walk_key_t {
		Matrix4d vp = Matrix4d::Zero();
		Vector3d eye = Vector3d::Zero();
		int w = 0, h = 0, epoch = -1, export_epoch = -1;
		double lod = 0;
		bool column = false;
		shadow_fit_t fit;
		double soft = 0;
		bool operator==(const walk_key_t &o) const {
			return epoch == o.epoch && w == o.w && h == o.h && lod == o.lod
				&& column == o.column && export_epoch == o.export_epoch
				&& eye == o.eye && vp == o.vp
				&& fit == o.fit && soft == o.soft;
		}
	};
	walk_key_t walk_key;
	// the last walk's caster box (see casterPlanes), if it had one
	std::array<Vector4d, 6> caster_planes;
	bool caster_planes_live = false;

	// per-section frame timing (ms) and bfs work counters
	struct section_stats_t {
		double bfs = 0, dl = 0, evict = 0, draw = 0;
		long oct = 0, cull = 0, lod = 0;
		int frames = 0;
	};
	// summed over the current 2s window, reported at its end
	section_stats_t sec;
	// the last report's per-frame averages (frames: per second), kept
	// (rather than only printed) so shells can read them back — see
	// EarthView::getStats
	section_stats_t avg;
	double sec_report_ms = 0, stats_report_ms = 0;
	int evict_tick = 0;

	static double ticksMs() {
		using namespace std::chrono;
		return (double)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0;
	}

	void load() {
		auto p = new rocktree_t();
		p->downloaded = false;
		planetoid = p;

		getPlanetoid([=](std::unique_ptr<PlanetoidMetadata> _metadata) {
			if (!_metadata) fprintf(stderr, "%s", "no planetoid\n"), abort();
			populatePlanetoid(p, std::move(_metadata));

			auto bulk = p->root_bulk;
			assert(bulk->dl_state == dl_state_stub);
			bulk->setStartedDownloading();
			getBulk(bulk);
		});
	}

	bool ready() const {
		return planetoid && planetoid->downloaded
			&& planetoid->root_bulk->dl_state == dl_state_downloaded;
	}

	double radius() const { return planetoid->radius; }
	// the planetoid's sphere, or the earth's mean radius before it loads
	double planetRadius() const {
		return planetoid && planetoid->downloaded ? planetoid->radius : 6371010.0;
	}

	// --- picking --------------------------------------------------------

	// the wgs84 ellipsoid's geocentric radius in the direction of p (the
	// meshes are ecef on wgs84; the planetoid's single radius is a sphere
	// that runs 7 km above it at the equator and 14 km below it at the poles)
	static double ellipsoidRadiusAt(const Vector3d &p) {
		const double a = 6378137.0, b = 6356752.314245;
		auto r2 = p.squaredNorm();
		if (r2 < 1e-12) return a;
		auto sin2 = p.z() * p.z() / r2, cos2 = 1.0 - sin2;
		return sqrt((a * a * a * a * cos2 + b * b * b * b * sin2) / (a * a * cos2 + b * b * sin2));
	}

	// horizon culling: is p hidden from eye behind the occluder sphere of
	// radius r_occ (centered on the planet)? true when the segment eye->p
	// dips inside the sphere, or p itself is under it
	static bool pointOccluded(const Vector3d &eye, const Vector3d &p, double r_occ2) {
		if (p.squaredNorm() < r_occ2) return true;
		Vector3d d = p - eye;
		auto l2 = d.squaredNorm();
		if (l2 < 1e-12) return false;
		auto t0 = -eye.dot(d) / l2; // segment param of closest approach to the center
		if (t0 <= 0 || t0 >= 1) return false;
		return (eye + d * t0).squaredNorm() < r_occ2;
	}

	// a node is skipped when every corner of its obb is behind the planet.
	// coarse far-side meshes are simplified so crudely that they bulge tens
	// of km out of their true surface and would otherwise poke through the
	// near side's fine terrain and win the depth test (mostly-ocean quarter
	// -planet blobs painting sea over europe). it is also what bounds a
	// horizon view: the frustum reaches 400 km (everest's horizon past the
	// viewer's own), and everything in it that is below the horizon is
	// wanted, fetched and drawn for nothing unless this says otherwise.
	// the meshes sit on the datum sphere (not the ellipsoid: measured
	// against wgs84 their altitudes track the sphere's offset by latitude,
	// +4.4 km at seattle, +3.2 km at toronto), and no fine tile's box
	// reaches more than ~300 m under it (the dead sea is -430 m), so the
	// occluder sits 1.5 km under the datum. it used to sit 16 km under,
	// which let through everything within 450 km *past* the horizon: on the
	// toronto view, a third of the wanted tiles
	static bool obbOccluded(const Vector3d &eye, const OrientedBoundingBox &obb, double planet_radius) {
		auto r_occ = planet_radius - 1500.0;
		auto r_occ2 = r_occ * r_occ;
		for (auto i = 0; i < 8; i++)
			if (!pointOccluded(eye, obbCorner(obb, i), r_occ2)) return false;
		return true;
	}

	// ray/obb slab test in the box frame (orientation columns are the box
	// axes). true if the ray enters before t_max; entry t may be 0 when the
	// origin is inside the box
	static bool rayHitsObb(const Vector3d &origin, const Vector3d &dir,
			const OrientedBoundingBox &obb, double t_max) {
		Vector3d o = obb.orientation.transpose() * (origin - obb.center);
		Vector3d d = obb.orientation.transpose() * dir;
		double t0 = 0, t1 = t_max;
		for (int i = 0; i < 3; i++) {
			if (fabs(d[i]) < 1e-12) {
				if (fabs(o[i]) > obb.extents[i]) return false;
				continue;
			}
			auto ta = (-obb.extents[i] - o[i]) / d[i];
			auto tb = ( obb.extents[i] - o[i]) / d[i];
			if (ta > tb) std::swap(ta, tb);
			t0 = fmax(t0, ta);
			t1 = fmin(t1, tb);
			if (t0 > t1) return false;
		}
		return true;
	}

	// möller–trumbore over the node's triangle strips, in mesh space. the
	// ray is transformed without normalizing the direction, so t keeps its
	// world-space meaning and stays comparable across nodes. mirrors the
	// shader's octant masking: a triangle is skipped only when all three
	// vertices are in masked octants (that surface is covered by a drawn
	// finer node — hitting it instead would pick a point slightly off the
	// visible terrain)
	static void rayNodeNearest(const Vector3d &origin, const Vector3d &dir,
			const rocktree_t::node_t *node, uint8_t octant_mask, double &best_t,
			const rocktree_t::node_t *&best_node) {
		Matrix4d mesh_from_globe = node->matrix_globe_from_mesh.inverse();
		Vector3d o = transformPoint(mesh_from_globe, origin);
		Vector3d d = (mesh_from_globe * Vector4d(dir.x(), dir.y(), dir.z(), 0.0)).head<3>();
		for (auto &mesh : node->meshes) {
			auto verts = mesh.vertices.data();
			auto vcount = mesh.vertices.size() / 8;
			auto pos = [&](uint16_t idx) {
				auto p = verts + (size_t)idx * 8; // 8-byte vertex, xyz in bytes 0-2
				return Vector3d(p[0], p[1], p[2]);
			};
			for (size_t i = 2; i < mesh.indices.size(); i++) {
				auto ia = mesh.indices[i-2], ib = mesh.indices[i-1], ic = mesh.indices[i];
				if (ia == ib || ib == ic || ia == ic) continue; // strip degenerates
				if (ia >= vcount || ib >= vcount || ic >= vcount) continue;
				if ((octant_mask >> verts[(size_t)ia * 8 + 3] & 1)
					&& (octant_mask >> verts[(size_t)ib * 8 + 3] & 1)
					&& (octant_mask >> verts[(size_t)ic * 8 + 3] & 1)) continue;
				Vector3d v0 = pos(ia);
				Vector3d e1 = pos(ib) - v0;
				Vector3d e2 = pos(ic) - v0;
				Vector3d p = d.cross(e2);
				auto det = e1.dot(p);
				if (fabs(det) < 1e-12) continue;
				auto inv = 1.0 / det;
				Vector3d tv = o - v0;
				auto u = tv.dot(p) * inv;
				if (u < 0 || u > 1) continue;
				Vector3d q = tv.cross(e1);
				auto v = d.dot(q) * inv;
				if (v < 0 || u + v > 1) continue;
				auto t = e2.dot(q) * inv;
				if (t > 0 && t < best_t) { best_t = t; best_node = node; }
			}
		}
	}

	// cast a world-space ray (dir unit) against the drawn scene: obb prune,
	// then exact triangles. misses when no resident mesh is on the ray — no
	// fallback: the old sea-level-sphere fallback sat ~4.5km off the real
	// terrain (sphere radius vs the ellipsoidal mesh), so everything that
	// consumed it (orbit pivots, picks) got phantom points kilometers from
	// the visible ground. hit_mpt (optional) reports the resolution of the
	// node the winning triangle came from, meters per texel — how much the
	// hit can be trusted for measurement
	enum raycast_result : int { raycast_miss = 0, raycast_mesh = 1 };
	raycast_result raycast(const Vector3d &origin, const Vector3d &dir, Vector3d &hit,
			double *hit_mpt = nullptr) {
		if (!ready()) return raycast_miss;
		double best_t = INFINITY;
		const rocktree_t::node_t *best_node = nullptr;
		for (auto &kv : drawn_nodes) {
			auto node = kv.first;
			if (node->dl_state != dl_state_downloaded) continue;
			if (!rayHitsObb(origin, dir, node->obb, best_t)) continue;
			rayNodeNearest(origin, dir, node, kv.second, best_t, best_node);
		}
		if (isinf(best_t) || !best_node) return raycast_miss;
		hit = origin + dir * best_t;
		if (hit_mpt) *hit_mpt = best_node->meters_per_texel;
		return raycast_mesh;
	}

	// ecef radius of the mesh directly under (or over) a point: ray straight
	// down from 2km above it, which clears any building. -1 when nothing
	// trustworthy is resident there. the resolution gate matters: the ray
	// tests drawn (frustum-visible) nodes, and looking at the horizon often
	// culls the fine tile directly below the camera — the ray then hits a
	// coarse ancestor whose surface sits tens of meters off (terrain hug
	// would plunge toward it). better to answer "don't know" and let the
	// caller hold altitude until fine mesh is visible again
	double groundRadiusUnder(const Vector3d &p) {
		Vector3d up = p.normalized();
		Vector3d hit;
		double mpt;
		if (raycast(up * (p.norm() + 2000.0), -up, hit, &mpt) != raycast_mesh) return -1;
		if (mpt > 10.0) return -1; // coarse filler, not the real surface
		return hit.norm();
	}

	// --- path overlay ---------------------------------------------------
	// a track dropped on the page (gpx), drawn over the terrain as a ribbon.
	// points are kept as an up vector plus a radius so a point can be moved
	// up and down its own vertical without re-deriving the geodetic frame.
	//
	// draping. the file's elevations are the obvious source, and they are
	// what we start from, but gps altitude is worth tens of meters on a good
	// day and the geoid separation it is measured against is worth a hundred
	// more, so a track laid at its own elevations wanders through hillsides.
	// so each point is re-measured against the mesh (groundRadiusUnder) and
	// pinned to the ground there. that measurement only answers where fine
	// mesh is currently resident and drawn, which is a moving target, so it
	// runs a few points a frame and keeps what it learns: a track fills in
	// onto the terrain over the first seconds and stays put after.
	std::vector<Vector3d> path_up;   // unit up per point
	std::vector<double> path_r;      // ecef radius at the file's elevation
	std::vector<double> path_ground; // measured ground radius, -1 = unmeasured
	std::vector<uint8_t> path_brk;   // 1: this point starts a new polyline
	Vector3d path_origin = Vector3d::Zero(); // vertices are relative to this
	bool path_drape = true;
	float path_rgba[4] = { 1.0f, 0.45f, 0.05f, 0.9f };
	float path_width = 3.0f;
	// the follow cursor: one point of the track the host wants marked (its
	// follow camera's aim point), drawn as a disc in the path's own color
	bool path_cursor_on = false;
	Vector3d path_cursor = Vector3d::Zero();
	std::vector<float> path_verts;   // the built ribbon, 8 floats a vertex
	bool path_dirty = false;         // geometry needs rebuilding + reuploading
	size_t path_probe_at = 0;        // round-robin cursor for the drape probes
	size_t path_unmeasured = 0;      // points still waiting for a ground probe
	int path_rebuild_wait = 0;

	// how far above the measured ground the ribbon floats. the depth bias
	// below does most of the work of keeping it out of the terrain's
	// z-fighting range; this is what makes it read as lying *on* the ground
	// rather than sunk into a slope
	static constexpr double path_lift = 1.5;
	// drape probes per frame. each is a raycast against the drawn set, so
	// the cost is bounded by this rather than by the length of the track
	static constexpr size_t path_probes_per_frame = 24;
	// ndc depth nudge toward the camera (see the path vertex shader)
	static constexpr float path_depth_bias = 0.0008f;

	// lat/lon/elevation triples, degrees and meters; a nan lat starts a new
	// polyline (a gpx track break). elevation is measured from the planetoid
	// sphere, which is the datum the mesh itself sits on
	void setPath(const std::vector<double> &lla) {
		path_up.clear(); path_r.clear(); path_ground.clear(); path_brk.clear();
		path_probe_at = 0;
		auto R = planetRadius();
		bool brk = true;
		for (size_t i = 0; i + 2 < lla.size(); i += 3) {
			auto lat = lla[i], lon = lla[i + 1], ele = lla[i + 2];
			if (isnan(lat) || isnan(lon)) { brk = true; continue; }
			path_up.push_back(geoUp(lat * M_PI / 180.0, lon * M_PI / 180.0));
			path_r.push_back(R + (isnan(ele) ? 0.0 : ele));
			path_ground.push_back(-1);
			path_brk.push_back(brk ? 1 : 0);
			brk = false;
		}
		// vertices go to the gpu in float, and ecef coordinates are 6.4e6
		// meters — half a meter of precision, which a line rendered against
		// the terrain shows as a wobble. the same trick the meshes use:
		// subtract a nearby origin and fold it into the transform in double
		path_origin = path_up.empty() ? Vector3d(Vector3d::Zero()) : Vector3d(path_up[0] * path_r[0]);
		path_unmeasured = path_up.size();
		path_verts.clear();
		// force the rebuild on the very next frame rather than the batched
		// one: until it happens the ribbon on the gpu is the previous track
		path_dirty = true;
		path_rebuild_wait = 1000;
	}

	// degrees/meters like setPath's points; alt is measured from the sphere,
	// so the host passes whatever altitude it is aiming at (see getPathGroundAlt)
	// put the cursor exactly on the drawn ribbon, at a fractional index into
	// the path's points: the ribbon is straight between consecutive points,
	// so interpolating between the same two positions it was built from puts
	// the disc on the line by construction — drape, lift and all. the host
	// follows a smoothed copy of the track, and a dot placed from that copy
	// visibly leaves the line wherever the smoothing cut a corner
	void setPathCursorIndex(double idx) {
		auto n = path_up.size();
		if (n == 0) { path_cursor_on = false; return; }
		auto c = fmax(0.0, fmin(idx, (double)(n - 1)));
		size_t i = (size_t)c;
		size_t j = i + 1 < n ? i + 1 : i;
		auto f = c - (double)i;
		// across a break there is no segment to sit on: hold the last point
		// of the polyline that ended rather than float over the gap
		if (j != i && path_brk[j]) { j = i; f = 0; }
		Vector3d a = path_up[i] * pathPointRadius(i);
		Vector3d b = path_up[j] * pathPointRadius(j);
		path_cursor = a + (b - a) * f;
		path_cursor_on = true;
	}

	void clearPathCursor() { path_cursor_on = false; }

	void setPathStyle(float r, float g, float b, float alpha, float width, bool drape) {
		path_rgba[0] = r; path_rgba[1] = g; path_rgba[2] = b; path_rgba[3] = alpha;
		path_width = width;
		if (drape != path_drape) { path_drape = drape; path_dirty = true; }
	}

	double pathPointRadius(size_t i) const {
		return path_drape && path_ground[i] > 0 ? path_ground[i] + path_lift : path_r[i];
	}

	// measure a few more points against the mesh. round-robin over the ones
	// still unmeasured: a probe only succeeds where fine mesh is drawn, so
	// the ones that fail are simply the ones not being looked at yet
	void pathProbeGround() {
		if (!path_drape || !path_unmeasured) return;
		auto n = path_up.size();
		size_t looked = 0, tried = 0;
		while (looked < n && tried < path_probes_per_frame) {
			auto i = path_probe_at % n;
			path_probe_at++;
			looked++;
			if (path_ground[i] > 0) continue;
			tried++;
			auto r = groundRadiusUnder(path_up[i] * path_r[i]);
			if (r > 0) {
				path_ground[i] = r;
				path_unmeasured--;
				path_dirty = true;
			}
		}
	}

	// build the ribbon: a quad per segment, plus a square patch at each
	// interior point to fill the wedge a bend leaves open between two quads
	// (the shader tells them apart by the quad being degenerate). the
	// sideways offset happens in the vertex shader, in screen pixels, so
	// what is stored here is just the segment's two ends
	void pathRebuild() {
		path_verts.clear();
		auto n = path_up.size();
		if (n < 2) return;
		path_verts.reserve(n * 2 * 48);
		auto pt = [&](size_t i) {
			Vector3d p = path_up[i] * pathPointRadius(i) - path_origin;
			return Vector3f((float)p.x(), (float)p.y(), (float)p.z());
		};
		// (end, side) corner bits; two triangles making the quad
		static const float corners[6][2] = {
			{ 0, -1 }, { 0, 1 }, { 1, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 } };
		auto quad = [&](const Vector3f &a, const Vector3f &b) {
			for (auto &c : corners) {
				const float v[8] = { a.x(), a.y(), a.z(), b.x(), b.y(), b.z(), c[0], c[1] };
				path_verts.insert(path_verts.end(), v, v + 8);
			}
		};
		for (size_t i = 0; i + 1 < n; i++)
			if (!path_brk[i + 1]) quad(pt(i), pt(i + 1));
		for (size_t i = 1; i + 1 < n; i++)
			if (!path_brk[i] && !path_brk[i + 1]) { auto p = pt(i); quad(p, p); }
	}

	// distance along the boresight to the actual drawn terrain, for the
	// ortho view extent and ortho zoom/pan anchoring. the sphere-datum
	// centerDistance sits ~4.5km off the real mesh (sphere radius vs the
	// ellipsoidal terrain), which collapses the derived ortho extent as soon
	// as the eye dips inside the reference sphere — kilometers above the
	// visible ground. sphere formula kept only as a last-resort fallback
	// (looking at the sky, tiles not yet loaded)
	double centerDistanceMesh(const camera_t &cam) {
		Vector3d hit;
		// past the near cut, like everything else that looks along a ray
		if (raycast(cam.eye + cam.direction * cam.near_cut, cam.direction, hit) == raycast_mesh)
			return (hit - cam.eye).norm();
		return centerDistance(cam, planetoid->radius);
	}

	// this frame, for the phases and the passes (see frame_t). kept, because
	// the shell's renderFrameEnd reads it after updateAndDraw returns
	frame_t frame;

	// the camera for this frame: its up vector and lens offset, the clip
	// range and projection, the jitter, and the miniature's focus
	frame_t setupFrame(const render_ctx_t &ctx, const camera_t &cam, int width, int height) {
		auto planet_radius = planetRadius();
		Vector3d eye = cam.eye;
		Vector3d direction = cam.direction;
		// airplane mode owns its up vector (roll); otherwise up is gravity,
		// turned about the boresight by the camera's roll. lookAt
		// orthogonalizes, so rotating the world up is enough to bank the
		// picture without touching where the camera is or what it looks at
		auto up = cam.airplane ? cam.body_up
			: Vector3d(AngleAxisd(cam.roll, direction) * eye.normalized());
		// the lens offset (glamour mode): the eye steps sideways on the
		// aperture and turns to keep the focus point on the boresight
		auto key_view = lookAt(eye, eye + direction, up); // the unmoved camera, for the walk's cache key
		Vector3d key_right = direction.cross(up).normalized();
		Vector3d key_up = key_right.cross(direction).normalized();
		if ((glamour.lens_x != 0 || glamour.lens_y != 0) && lens_focus > 0) {
			Vector3d right = direction.cross(up).normalized();
			Vector3d cup = right.cross(direction).normalized();
			Vector3d focus_pt = eye + direction * lens_focus;
			eye += right * glamour.lens_x + cup * glamour.lens_y;
			direction = (focus_pt - eye).normalized();
		}

		// projection
		float aspect_ratio = (float)width / (float)height;
		float fov = (float)cam.fov;
		// (the unmoved eye: the lens offset must not change the clip planes,
		// or the walk's cache key changes with every sample)
		auto altitude = cam.eye.norm() - planet_radius;
		auto horizon = sqrt( altitude * (2*planet_radius + altitude) );
		// terrain of height h is visible up to horizon(h) beyond the viewer's own
		// geometric horizon (e.g. mount rainier from seattle), so extend the far
		// plane by the horizon distance of the tallest terrain on earth
		const double highest_peak = 8849; // everest
		auto peak_horizon = sqrt( highest_peak * (2*planet_radius + highest_peak) );
		auto near = horizon > 370000 ? altitude / 2 : 1.0;
		// the near cut is the near plane: whatever is closer than that just
		// isn't drawn. (a plane, so at the frame's edges the cut reaches a
		// little further along the ray than straight ahead — near enough)
		if (!cam.ortho && cam.near_cut > 0) near = fmax(near, cam.near_cut);
		auto far = horizon + peak_horizon;
		if (near >= far) near = far - 1;
		if (isnan(far) || far < near) far = near + 1;
		Matrix4d projection;
		double ortho_half_extent = 0; // vertical half-extent in meters (ortho only)
		if (cam.ortho) {
			// explicit extent state set on mode entry / zooms; derive from the
			// terrain distance only when unset
			ortho_half_extent = cam.ortho_extent > 0 ? cam.ortho_extent
				: fmax(1.0, centerDistanceMesh(cam) * tan(fov / 2.0));
			// the parallel beam is a box, not a cone from the eye: in a tilted
			// view its bottom edge reaches ground beside or behind the camera
			// plane, and its top edge starts half an extent above the eye, so
			// it sees past the eye's own horizon. open the clip range up to a
			// full horizon each way, plus the beam's half-diagonal on the far
			// side (a negative near is fine in ortho)
			far += ortho_half_extent * sqrt(1.0 + aspect_ratio * aspect_ratio);
			near = -far;
			projection = orthographic(ortho_half_extent * aspect_ratio, ortho_half_extent, near, far);
		} else {
			projection = perspective(fov, aspect_ratio, near, far);
		}

		frame_t f;
		// the miniature pass focuses on the terrain under the crosshair
		if (ctx.dof.on) {
			auto focus = fmax(1.0, centerDistanceMesh(cam));
			f.focus = (float)focus;
			// the host's sharp band is a fraction of the focus distance *at
			// the default 45-degree fov*. what the eye judges is how much of
			// the frame is sharp, and the depth a frame spans scales with its
			// angular height — so scale the band with it, and zooming (Z/X)
			// keeps the look. in ortho the same quantity is the half-extent
			// over the focus distance
			auto half_angle = cam.ortho ? ortho_half_extent / focus : tan(fov / 2.0);
			f.focus_range = ctx.dof.range_host * (float)(half_angle / tan(0.125 * M_PI));
		}

		Matrix4d key_vp = projection * key_view; // before the jitter
		// the sub-pixel jitter (glamour mode): a shift of the whole image by
		// a fraction of a pixel, in the projection's own terms
		if (glamour.jitter_x != 0 || glamour.jitter_y != 0) {
			if (cam.ortho) { projection(0, 3) += glamour.jitter_x * 2.0 / width; projection(1, 3) += glamour.jitter_y * 2.0 / height; }
			else { projection(0, 2) += glamour.jitter_x * 2.0 / width; projection(1, 2) += glamour.jitter_y * 2.0 / height; }
		}
		auto view = lookAt(eye, eye + direction, up);
		Matrix4d viewprojection = projection * view;
		Vector3d right = direction.cross(up).normalized();
		Vector3d cam_up = right.cross(direction).normalized();

		f.width = width; f.height = height;
		f.eye = eye; f.direction = direction; f.right = right; f.cam_up = cam_up;
		f.near = near; f.far = far; f.ortho = cam.ortho; f.ortho_half_extent = ortho_half_extent;
		f.tan_half = tan(cam.fov / 2.0);
		f.aspect_ratio = aspect_ratio;
		f.viewprojection = viewprojection; f.key_vp = key_vp;
		f.key_eye = cam.eye; f.key_direction = cam.direction; f.key_right = key_right; f.key_up = key_up;
		f.tube = tube.on;
		f.debug_lod = debug_lod;
		return f;
	}

	// tube mode's node-budget backstop, between two levels of the walk:
	// distance lod bounds the set on its own for any sane rect (a floor
	// level over the rect plus fine rings near the camera), but a
	// degenerate rect or heavy zoom can overflow it. shed this level's
	// least-wanted additions instead of breaking off the whole walk: a
	// uniform break coarsens exactly the tiles with the highest demand (the
	// zoomed-at building loses its lod as the fov narrows — backwards),
	// while a want-ordered trim degrades everything evenly relative to its
	// own target. reordering within one bfs level is fine: the draw loop
	// only needs whole levels in order (children after parents when read
	// backwards)
	static constexpr size_t tube_node_cap = 3000;
	void tubeNodeBackstop(size_t level_pot_start,
			std::vector<rocktree_t::node_t *> &next_valid,
			std::vector<double> &next_want) {
		if (potential_nodes.size() <= tube_node_cap) return;
		auto budget = tube_node_cap > level_pot_start
			? tube_node_cap - level_pot_start : 0;
		std::vector<size_t> idx(potential_nodes.size() - level_pot_start);
		for (size_t i = 0; i < idx.size(); i++) idx[i] = level_pot_start + i;
		std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
			return potential_want[a] > potential_want[b]; });
		// the weakest want that stays; nothing below it may descend
		auto cutoff = budget > 0
			? potential_want[idx[budget - 1]] : 1e300;
		decltype(potential_nodes) kept_n(potential_nodes.begin(),
			potential_nodes.begin() + level_pot_start);
		std::vector<double> kept_w(potential_want.begin(),
			potential_want.begin() + level_pot_start);
		for (size_t i = 0; i < budget && i < idx.size(); i++) {
			kept_n.push_back(std::move(potential_nodes[idx[i]]));
			kept_w.push_back(potential_want[idx[i]]);
		}
		potential_nodes = std::move(kept_n);
		potential_want = std::move(kept_w);
		std::vector<rocktree_t::node_t *> nv;
		std::vector<double> nw;
		for (size_t i = 0; i < next_valid.size(); i++)
			if (next_want[i] >= cutoff) {
				nv.push_back(std::move(next_valid[i]));
				nw.push_back(next_want[i]);
			}
		next_valid = std::move(nv);
		next_want = std::move(nw);
	}

	// where the walk's casters are: whatever is between the colour pass's
	// surface in the map's reach and the sun, as far as the map's near
	// plane, at most the box of the frustum the map is fitted to. (the
	// map's own box is square and spans the frustum's air too: at a low
	// sun, casting from all of it costs a few times what the view does.)
	// a box in the sun's frame, as planes (inside: n.p + d >= 0), grown by
	// how far glamour's sun and texel jitter move the map. false when
	// nothing in reach can be shaded
	bool casterPlanes(const shadow_fit_t &fit, const std::vector<const OrientedBoundingBox *> &surface,
			const Vector3d &eye, const Vector3d &direction, std::array<Vector4d, 6> &planes) const {
		Vector3d lo = Vector3d::Constant(INFINITY), hi = Vector3d::Constant(-INFINITY);
		for (auto obb : surface) {
			if ((obb->center - eye).dot(direction) - obb->extents.norm() > fit.depth) continue;
			Vector3d a, b;
			for (int k = 0; k < 3; k++) {
				auto &ax = k == 0 ? fit.sx : k == 1 ? fit.sy : fit.L;
				auto c = obb->center.dot(ax), e = obb->extents.dot((obb->orientation.transpose() * ax).cwiseAbs());
				a[k] = fmax(c - e, fit.lo[k]); b[k] = fmin(c + e, fit.hi[k]);
			}
			if (!(a.array() <= b.array()).all()) continue;
			lo = lo.cwiseMin(a); hi = hi.cwiseMax(b);
		}
		if (!(lo.array() < hi.array()).all()) return false;
		hi.z() = fit.O.dot(fit.L) - shadow_near;
		auto pad = sun_soft * (hi.z() - lo.z()) + 2.0 * fit.texel_m;
		auto plane = [](const Vector3d &n, double d) { return Vector4d(n.x(), n.y(), n.z(), d); };
		planes = {
			plane(fit.sx, pad - lo.x()), plane(-fit.sx, hi.x() + pad),
			plane(fit.sy, pad - lo.y()), plane(-fit.sy, hi.y() + pad),
			plane(fit.L, pad - lo.z()), plane(-fit.L, hi.z() + pad),
		};
		return true;
	}

	// node culling and level of detail: a breadth-first walk of the tree
	// that stamps what the view wants and collects the nodes and bulks it
	// wants and doesn't have — or, when nothing it depends on has moved,
	// last frame's answer, re-stamped. true when no bulk it needed was
	// missing. then, with shadows, a second pass keeps what can cast onto
	// the first's tiles in the shadow map's reach (see casterPlanes),
	// wherever the camera looks, to draw into the map only (shadow_only)
	bool walk(const frame_t &f, const camera_t &cam, double now_ms, const shadow_fit_t &fit) {
		auto &eye = f.eye, &direction = f.direction;
		auto &viewprojection = f.viewprojection;
		auto width = f.width, height = f.height;
		auto aspect_ratio = f.aspect_ratio;
		auto ortho_half_extent = f.ortho_half_extent;
		auto planet_radius = planetRadius();

		// wanted bulks/nodes are marked by stamping last_wanted_ms with this
		// frame's time during the walk; membership tests elsewhere (eviction,
		// bulk purge) compare against the stamp instead of consulting a map
		auto prev_walk_ms = last_walk_ms;
		last_walk_ms = now_ms;

		// is last frame's walk still the answer? (see the members)
		auto epoch = g_tree_epoch.load();
		auto cacheable = !tube.on && !tube.tunnel_on;
		walk_key_t key = { f.key_vp, cam.eye, width, height, epoch, export_epoch, lod_scale, ground_column_on, fit, sun_soft };
		auto reuse = cacheable && walk_cache_valid && walk_key == key;
		walk_reused = reuse;
		if (reuse) {
			for (auto n : potential_nodes) n->last_wanted_ms = now_ms;
			for (auto b : walk_bulks) b->last_wanted_ms = now_ms;
			stat.bulk_blocked = walk_bulk_blocked;
			return walk_bulk_blocked == 0;
		}
		potential_nodes.clear();
		potential_want.clear();
		to_download_bulks.clear();
		walk_bulks.clear();
		stat.bulk_blocked = 0; // counted over this frame's walk only

		auto frustum_planes = getFrustumPlanes(viewprojection); // for obb culling

		auto &cast_planes = caster_planes;
		caster_planes_live = false;
		auto caster_mpt = caster_texels * fit.texel_m;

		// the down-ray of the terrain-hug ground column (cast from above any
		// building, like groundRadiusUnder's)
		Vector3d col_origin = Vector3d::Zero(), col_dir = -Vector3d::UnitZ();
		if (ground_column_on) {
			Vector3d cup = ground_column_point.normalized();
			col_origin = cup * (ground_column_point.norm() + 2000.0);
			col_dir = -cup;
		}

		// the camera in the rect-local frame, for tube-mode lod distances
		auto tv = tube.walkView(eye, direction);

		// the nodes whose children the next bfs level looks at; null is the
		// root, above the root bulk's top nodes
		std::vector<rocktree_t::node_t *> valid = { nullptr };
		decltype(valid) next_valid;
		std::vector<double> next_want; // tube mode: potential_want's counterpart for next_valid
		decltype(valid) kept; // every node the colour pass kept, and the root
		// the colour pass's surface: where it stopped descending in view, a
		// tile too fine to want (its parent is drawn there), or the end of
		// the tree or of what it has
		std::vector<const OrientedBoundingBox *> surface;

		auto potential_bulk_count = 0;
		bool all_loaded = true;

		// the export's descent, closed for good once the cap is in sight:
		// a level of the rect is about four times the one above it (it is
		// a surface, so mostly four of the eight children have data)
		auto export_floor_open = export_on;
		size_t export_count = 0, export_level_count = 0;
		stat_export_capped = false;

		// node culling and level of detail using breadth-first search: the
		// colour pass, then the casters' from where it ends
		auto casters = false;
		for (;;) {
			auto level_pot_start = potential_nodes.size();
			if (!casters) kept.insert(kept.end(), valid.begin(), valid.end());
			if (export_floor_open && export_count + 4 * export_level_count > export_node_cap) {
				export_floor_open = false;
				stat_export_capped = true;
			}
			export_level_count = 0;
			for(auto cur : valid) {
				auto bulk = cur ? cur->parent : planetoid->root_bulk;
				auto children = cur ? cur->children : bulk->children;
				// the colour pass already counted this one's bulks
				auto seen = casters && (!cur || !cur->shadow_only);

				if (cur && cur->level % 4 == 0) {
					auto b = cur->child_bulk;
					if (!b) {
						if (!casters) surface.push_back(&cur->obb);
						continue;
					}
					if (!seen) {
						b->last_wanted_ms = now_ms;
						walk_bulks.push_back(b);
						if (b->dl_state == dl_state_stub) {
							// ranked and issued after the walk, like nodes. the
							// yardstick is the node this bulk hangs under — the
							// subtree's own apparent size — so the bulks under
							// what you are looking at resolve first and the view
							// refines outward from there instead of every subtree
							// in the frustum competing at once
							double pri = 1e300;
							auto radius = cur->obb.extents.norm();
							auto dist = fmax(0.0, (cur->obb.center - eye).norm() - radius);
							if (radius > 0) pri = dist / (2 * radius);
							to_download_bulks.push_back({ pri, (size_t)cur->level, b });
						}
					}
					if (b->dl_state != dl_state_downloaded) {
						// the walk cannot see past a bulk it doesn't have: this
						// is a whole 4-level subtree that stays coarse until it
						// lands, which is what a deep zoom is waiting on
						if (!seen) stat.bulk_blocked++;
						if (!casters) surface.push_back(&cur->obb);
						all_loaded = false;
						continue;
					}
					bulk = b;
					children = b->children;
				}
				if (!seen) {
					bulk->last_wanted_ms = now_ms;
					walk_bulks.push_back(bulk);
					potential_bulk_count++;
				}
				// the end of the tree: all of it is drawn here
				if (!casters && cur && std::all_of(children, children + 8, [](auto c) { return !c; }))
					surface.push_back(&cur->obb);

				for (auto o = 0; o < 8; o++) {
					sec.oct++;
					auto node = children[o];
					if (!node) continue;
					if (casters && node->kept_ms == now_ms) continue; // the colour pass's

					// cull outside frustum using obb
					// todo: check if it could cull more
					sec.cull++;
					// ground-column nodes skip culling and the lod cutoff:
					// the ground query wants the finest mesh under the
					// camera regardless of where the view points
					auto in_column = !casters && ground_column_on
						&& rayHitsObb(col_origin, col_dir, node->obb, INFINITY);
					// the export rect, likewise wanted whatever the view
					auto in_export = !casters && export_on && exportRectTouches(node->obb);
					auto tn = tube.walkNode(node->obb);
					// tube mode culls against the rect, not the view: the
					// whole slab is drawn wherever the camera looks. body
					// mode keeps the land around the figure too — the rect
					// cut out of it, since that sheet is up on the skin — so
					// there a node is wanted if it touches the rect (drawn
					// wrapped) or is in view (drawn flat); a straddler is
					// drawn both ways
					auto in_view = false;
					if (casters) {
						// a caster needn't be finer than the map's texel, which
						// is all of it the map can hold: past that, its parent
						// casts. (one under a caster stays a caster, even on
						// screen: the colour pass draws an ancestor in its place)
						if (cur && cur->meters_per_texel <= caster_mpt) continue;
						if (obb_frustum_outside == classifyObbFrustum(&node->obb, cast_planes)) continue;
					} else if (in_column || in_export) {
					} else if (tube.on && !tube.body) {
						if (!tn.in_rect) continue;
					} else {
						in_view = obb_frustum_outside != classifyObbFrustum(&node->obb, frustum_planes)
							&& !obbOccluded(eye, node->obb, planet_radius);
						if (!in_view && !tn.in_rect) continue;
					}
					sec.lod++;

					// level of detail: the ground column always descends — it
					// wants the finest mesh there regardless of the camera
					// view. tube mode descends by distance measured through
					// the roll (below), with an unconditional floor (see
					// walkFloor)
					auto tube_floor = tube.walkFloor(node, tn);
					// the export's floor: down to its detail, finest in
					// (export_mpt / 2, export_mpt]
					auto export_floor = in_export && export_floor_open
						&& node->meters_per_texel > export_mpt * 0.5;
					auto tube_want = 1e30; // floor/column: never shed
					// the download order's default (tube mode sets its own: see walkDistance)
					node->sched_pri = fmax(0.0, (node->obb.center - eye).norm()
						- node->obb.extents.norm()) / (2 * node->obb.extents.norm());
					if (!in_column && !tube_floor && !export_floor) {
						auto texels_per_meter = 1.0f / node->meters_per_texel;
						auto wh = width < height ? width : height;
						double r;
						if (cam.ortho) {
							// parallel projection: pixels per meter is the same at
							// every depth, set entirely by the view extent
							r = 2.0 * wh * tan(0.125 * M_PI) / ortho_half_extent;
						} else {
							// zooming (narrowing the fov) magnifies the scene, so it
							// needs proportionally finer tiles. 1 at the default fov
							auto zoom = tan(0.125 * M_PI) / tan(cam.fov / 2.0);
							// distance to the node: straight-line, or in tube
							// mode through the warp (see walkDistance, which
							// also sets the node's download order)
							double dist = (eye - node->obb.center).norm();
							if (tn.in_rect) {
								bool in_cone;
								dist = tube.walkDistance(tv, tn, node, dist, in_view,
									cam.fov, aspect_ratio, in_cone);
								// zoom magnifies only what's on screen. tube
								// lod ignores the view direction on purpose
								// (the whole slab stays resident), but the
								// zoom boost must not: boosted in a full
								// circle it floods the node backstop, whose
								// uniform-level break then *coarsens* the
								// very thing being zoomed at. the rest keep
								// the default-fov target, i.e. unzoomed
								// tube quality
								if (zoom > 1.0 && !in_cone) zoom = 1.0;
							}
							auto t = Affine3d().Identity();
							t.translate(eye + dist * direction);
							auto m = viewprojection * t;
							auto s = m(3, 3);
							r = (2.0*(1.0/s)) * wh * zoom;
						}
						r /= lod_scale;
						if (texels_per_meter > r) {
							if (!casters) surface.push_back(&node->obb);
							continue;
						}
						// how far past its target this node was requested,
						// scale-free (1 = barely wanted)
						tube_want = r * node->meters_per_texel;
					}

					node->shadow_only = casters;
					node->kept_ms = now_ms;
					next_valid.push_back(node);
					if (tube.on) next_want.push_back(tube_want);

					if (in_export) { export_count++; export_level_count++; }
					if (node->can_have_data) {
						if (node->last_wanted_ms != prev_walk_ms) node->first_wanted_ms = now_ms;
						node->last_wanted_ms = now_ms;
						node->export_walk_ms = in_export ? now_ms : 0;
						potential_nodes.push_back(node);
						if (tube.on) potential_want.push_back(tube_want);
					}
				}
			}
			if (next_valid.size() == 0) {
				if (casters || !fit.live || !casterPlanes(fit, surface, eye, direction, cast_planes)) break;
				casters = caster_planes_live = true;
				valid = kept;
				continue;
			}
			if (tube.on) {
				tubeNodeBackstop(level_pot_start, next_valid, next_want);
				if (next_valid.size() == 0) break;
			}
			valid = next_valid;
			next_valid.clear();
			next_want.clear();
		}
		// (after the tube's trim, which reorders potential_nodes)
		export_idx.clear();
		if (export_on)
			for (size_t i = 0; i < potential_nodes.size(); i++)
				if (potential_nodes[i]->export_walk_ms == now_ms) export_idx.push_back(i);
		walk_bulk_count = potential_bulk_count;
		walk_bulk_blocked = stat.bulk_blocked;
		walk_cache_valid = cacheable;
		walk_key = key;
		return all_loaded;
	}

	// download nodes in order of importance: by apparent size on screen,
	// biggest first. distance to the node's bounding sphere over its diameter
	// is the inverse of angular size, so it ranks a fine tile right in front
	// of the camera above a coarse tile at the horizon, while a huge coarse
	// tile still wins over everything when nothing is loaded yet. only a
	// limited number of requests may be in flight at once, so the order can
	// adapt while the camera moves instead of everything being queued in path
	// order the first frame it becomes visible
	void scheduleDownloads(const frame_t &f, double now_ms, bool all_loaded) {
		auto &eye = f.eye;
		const auto max_nodes_in_flight = 32;
		// small on purpose: see bulks_in_flight. sixteen is enough to keep
		// the descent moving down the path you're looking along without
		// the metadata ever being what the node requests wait behind
		const auto max_bulks_in_flight = 16;
		// how long a thing that just failed has to wait before being asked
		// for again: half a second, doubling per failure, capped at half a
		// minute. set when the request goes out, so a failure that returns
		// immediately still can't be retried before the delay is up
		auto retry_delay_ms = [](int fails) {
			return 500.0 * (double)(1 << (fails < 6 ? fails : 6));
		};
		// ready if it has never failed, or if its penalty box has expired
		auto retry_ready = [&](auto *x) {
			return x->dl_fails == 0 || now_ms >= x->dl_next_try_ms;
		};

		// bulks first: they cost little and the walk cannot see past them,
		// so a slot spent here opens four levels of everything below
		std::sort(to_download_bulks.begin(), to_download_bulks.end(),
			[](const bulk_candidate_t &a, const bulk_candidate_t &b) {
				return a.priority != b.priority ? a.priority < b.priority : a.level < b.level;
			});
		stat.bulks_started = 0;
		for (auto &c : to_download_bulks) {
			if (bulks_in_flight >= max_bulks_in_flight) break;
			// (a reused walk's candidates include ones already requested)
			if (c.bulk->dl_state != dl_state_stub) continue;
			if (!retry_ready(c.bulk)) continue;
			bulks_in_flight++;
			stat.bulks_started++;
			c.bulk->dl_next_try_ms = now_ms + retry_delay_ms(c.bulk->dl_fails);
			c.bulk->setStartedDownloading();
			getBulk(c.bulk, [this] { bulks_in_flight--; });
		}

		struct candidate_t { double priority; size_t level; rocktree_t::node_t *node; };
		std::vector<candidate_t> to_download;
		stat.nodes_wanted = (int)potential_nodes.size();
		stat.nodes_loaded = stat.nodes_stub = stat.nodes_stuck = 0;
		stat.nodes_failed = stat.max_level = stat.nodes_shadow = 0;
		pending_nodes.clear();
		for (auto node : potential_nodes) {
			if (node->shadow_only) stat.nodes_shadow++;
			if (node->level > stat.max_level) stat.max_level = node->level;
			if (node->dl_state != dl_state_downloaded) {
				all_loaded = false;
				pending_nodes.push_back({ node->request.node_key().path(), (int)node->dl_state.load(), node->dl_fails,
					(node->obb.center - eye).norm(), node->meters_per_texel, node->first_wanted_ms });
			}
			else stat.nodes_loaded++;
			if (node->dl_state == dl_state_stub) stat.nodes_stub++;
			else if (node->dl_state == dl_state_downloading) stat.nodes_stuck++;
			if (node->dl_fails) stat.nodes_failed++;
			if (node->dl_state != dl_state_stub) continue;
			if (!retry_ready(node)) continue; // waiting out a failure
			auto radius = node->obb.extents.norm();
			auto dist = fmax(0.0, (node->obb.center - eye).norm() - radius);
			// (in tube mode the walk measured the distance through the
			// roll, which is what proximity means there.) a caster off
			// screen waits behind what is on it the same apparent size
			auto pri = tube.on ? node->sched_pri : dist / (2 * radius);
			to_download.push_back({ node->shadow_only ? 2.0 * pri : pri, (size_t)node->level, node });
		}
		std::sort(to_download.begin(), to_download.end(), [](const candidate_t &a, const candidate_t &b) {
			// tiles containing the camera all have priority 0; coarse first there
			return a.priority != b.priority ? a.priority < b.priority : a.level < b.level;
		});
		auto started = 0;
		for (auto &c : to_download) {
			if (nodes_in_flight >= max_nodes_in_flight) break;
			auto node = c.node;
			nodes_in_flight++;
			started++;
			node->dl_next_try_ms = now_ms + retry_delay_ms(node->dl_fails);
			node->setStartedDownloading();
			getNode(node, [this] { nodes_in_flight--; });
		}
		if (log_sched && (to_download.size() > 0 || nodes_in_flight > 0))
			printf("timing: sched at=%.0f stubs=%zu started=%d inflight=%d pot=%zu\n",
				now_ms, to_download.size(), started, (int)nodes_in_flight, potential_nodes.size());
		scene_complete = all_loaded;
		stat_export_wanted = (int)export_idx.size();
		stat_export_loaded = 0;
		stat_export_mpt = 0;
		for (auto k : export_idx) {
			auto node = potential_nodes[k];
			if (node->dl_state != dl_state_downloaded) continue;
			stat_export_loaded++;
			if (stat_export_mpt <= 0 || node->meters_per_texel < stat_export_mpt) stat_export_mpt = node->meters_per_texel;
		}
	}

	// downloaded nodes form an lru cache with a byte quota: nothing is evicted
	// until resident mesh/texture data exceeds the quota, then the least
	// recently wanted nodes are dropped first. nodes in the current potential
	// set are never evicted, even over quota
	// ~100-150KB per resident node
	//
	// the sweep walks every node entry of every resident bulk (including
	// stubs), which costs tens of ms in heavy views — and it only needs to be
	// roughly current, so it runs every 15th frame
	void evict(double now_ms, double dt_ms) {
		// the wasm heap is a fixed 1GB; leave headroom for decode buffers
		// and the module itself. too small a quota makes looking around
		// evict whatever was just behind you
		const size_t mem_quota = (size_t)512 << 20;
		size_t resident_bytes = 0;
		struct evict_t { double last_wanted_ms; size_t bytes; rocktree_t::node_t *n; };
		std::vector<evict_t> evictable;
		auto node_bytes = [](rocktree_t::node_t *n) {
			auto bytes = sizeof(*n);
			for (auto &m : n->meshes)
				bytes += m.vertices.size() + m.indices.size() * sizeof(uint16_t) + m.texture.size() + m.normals.size();
			return bytes;
		};

		// bulk metadata (the octree index) is also an lru cache with its own
		// byte budget — never purged on a clock. a bulk is only purgeable
		// once it has no downloading/downloaded children (busy_ctr == 0), so
		// purges cascade bottom-up under sustained pressure
		const size_t bulk_quota = (size_t)256 << 20;
		size_t bulk_bytes_total = 0;
		struct purge_t { double last_wanted_ms; size_t bytes; rocktree_t::bulk_t *b; };
		std::vector<purge_t> purgeable;
		auto bulk_bytes = [](rocktree_t::bulk_t *b) {
			// rough: map/protobuf overhead per child entry
			return sizeof(*b) + b->nodes.size() * (sizeof(rocktree_t::node_t) + 256)
				+ b->bulks.size() * 128;
		};

		// depth first: a stack of the bulks still to visit
		std::vector<rocktree_t::bulk_t*> x = {planetoid->root_bulk};
		auto buf_cnt = 0, obs_n_cnt = 0, obs_b_cnt = 0, total_n = 0, total_b = 0;
		while(!x.empty()) {
			auto cur_bulk = x.back(); x.pop_back();
			total_b++;
			auto bbytes = bulk_bytes(cur_bulk);
			bulk_bytes_total += bbytes;
			if (cur_bulk->parent && cur_bulk->last_wanted_ms != now_ms && cur_bulk->busy_ctr == 0)
				purgeable.push_back({ cur_bulk->last_wanted_ms, bbytes, cur_bulk });
			// prepare next iteration
			for (auto &kv : cur_bulk->bulks) {
				auto b = kv.second.get();
				if (b->dl_state != dl_state_downloaded) continue;
				x.push_back(b);
			}
			// current iteration
			for (auto &kv : cur_bulk->nodes) {
				auto n = kv.second.get();
				if (n->dl_state != dl_state_downloaded) continue;

				// just count buffers
				for (auto &m : n->meshes) { if (m.buffered) { buf_cnt++; break;}}

				total_n++;
				auto bytes = node_bytes(n);
				resident_bytes += bytes;
				if (n->last_wanted_ms != now_ms) evictable.push_back({ n->last_wanted_ms, bytes, n });
			}
		}

		// evict least recently wanted nodes until back under quota
		if (resident_bytes > mem_quota) {
			std::sort(evictable.begin(), evictable.end(), [](const evict_t &a, const evict_t &b) {
				return a.last_wanted_ms < b.last_wanted_ms;
			});
			for (auto &e : evictable) {
				if (resident_bytes <= mem_quota) break;
				auto n = e.n;
				obs_n_cnt++;

				// unbuffer
				for (auto &mesh : n->meshes) {
					if (mesh.buffered) unbufferMesh(mesh);
				}
				// clean up
				n->matrix_globe_from_mesh = Matrix4d::Zero();
				n->meshes.clear();
				n->setDeleted();

				resident_bytes -= e.bytes;
			}
		}

		// purge least recently wanted bulks until metadata is back under its
		// budget. clearing the child maps only ever destroys stubs: a bulk
		// with any downloading or downloaded child has busy_ctr > 0 and was
		// not collected above. the links from the bulk above stay good: they
		// point at the bulk itself, which stays behind as a stub
		if (bulk_bytes_total > bulk_quota) {
			std::sort(purgeable.begin(), purgeable.end(), [](const purge_t &a, const purge_t &b) {
				return a.last_wanted_ms < b.last_wanted_ms;
			});
			for (auto &e : purgeable) {
				if (bulk_bytes_total <= bulk_quota) break;
				auto b = e.b;
				obs_b_cnt++;
				b->clear();
				b->setDeleted();
				bulk_bytes_total -= e.bytes;
			}
		}

		// log stuff about buffers
		stats_report_ms += dt_ms;
		if (stats_report_ms > 2000) {
			stats_report_ms = 0;
			printf("buffered: %d, tot_n: %d, tot_b: %d, pot_n: %lu, pot_b: %d, obs n: %d, obs b: %d, mem: %zu MB\n",
				buf_cnt, total_n, total_b, potential_nodes.size(), walk_bulk_count, obs_n_cnt, obs_b_cnt,
				resident_bytes >> 20
			);
		}
	}

	static double smooth(double a, double b, double x) { x = fmin(1.0, fmax(0.0, (x - a) / (b - a))); return x * x * (3 - 2 * x); }

	// straight up, east and the sun at the eye
	void sunFrame(const Vector3d &eye, Vector3d &local_up, Vector3d &east, Vector3d &sun_dir) const {
		local_up = eye.normalized();
		east = Vector3d::UnitZ().cross(local_up);
		if (east.norm() < 1e-6) east = Vector3d::UnitX(); else east.normalize();
		Vector3d north = local_up.cross(east);
		sun_dir = cos(sun_el) * (sin(sun_az) * east + cos(sun_az) * north) + sin(sun_el) * local_up;
	}

	// the atmosphere's colours this frame: the sun in the eye's local
	// frame, the haze and the ground's tint from its height, and the
	// horizon and limb the sky pass draws
	void setAtmosphere(frame_t &f) {
		auto &eye = f.eye;
		auto planet_radius = planetRadius();
		Vector3d local_up, east, sun_dir;
		sunFrame(eye, local_up, east, sun_dir);
		auto low = 1.0 - smooth(0.0, 25.0 * M_PI / 180.0, sun_el); // 1 at the horizon, 0 at 25° up
		auto day = smooth(-8.0 * M_PI / 180.0, 3.0 * M_PI / 180.0, sun_el); // 0 at night, 1 by day
		auto bright = 0.1 + 0.9 * day;
		Vector3f zenith((sky_rgb >> 16 & 0xff) / 255.0f, (sky_rgb >> 8 & 0xff) / 255.0f, (sky_rgb & 0xff) / 255.0f);
		Vector3f haze = zenith * 0.5f + Vector3f(0.5f, 0.5f, 0.5f);
		haze = haze * (float)(1.0 - 0.55 * low) + Vector3f(1.0f, 0.72f, 0.45f) * (float)(0.55 * low);
		zenith *= (float)(bright * (1.0 - sky_dark));
		haze *= (float)(bright * (1.0 - sky_dark));
		Vector3f tint = Vector3f(1, 1, 1) * (float)(1.0 - 0.45 * low) + Vector3f(1.0f, 0.86f, 0.70f) * (float)(0.45 * low);
		tint *= (float)(0.3 + 0.7 * day);
		if (!tint_on) tint = Vector3f(1, 1, 1);
		auto alt = fmax(0.0, eye.norm() - planet_radius);
		auto dip = acos(planet_radius / (planet_radius + alt));
		// the sky thins away with altitude; what's left from high up is
		// the limb, an atmosphere's thickness (a few scale heights)
		// seen edge-on just above the horizon line
		auto sky_amount = exp(-alt / (3.0 * scale_height_m));
		auto limb = asin(fmin(1.0, (planet_radius + 4.0 * scale_height_m) / (planet_radius + alt)))
			- asin(fmin(1.0, planet_radius / (planet_radius + alt)));
		f.planet_radius = planet_radius; f.scale_height = scale_height_m;
		f.haze_on = haze_on; f.visibility_m = visibility_m;
		f.local_up = local_up; f.east = east; f.sun_dir = sun_dir; f.sun_el = sun_el;
		f.zenith = zenith; f.haze = haze; f.tint = tint;
		f.horizon_dip = dip; f.sky_amount = sky_amount; f.limb = fmax(limb, 1e-3);
	}

	// a node in the colour pass (color) and the shadow pass, each with its
	// own octant mask (0xff: not in that pass)
	struct draw_t { rocktree_t::node_t *node; uint8_t self_mask, shadow_mask; bool color; uint32_t stale_mask; };

	// what this frame draws: the downloaded part of the potential set, read
	// children first so that a node's octant mask (the octants a drawn
	// child covers) is complete by the time the node comes up. a caster
	// kept only for the shadow map covers its parent's octant there but
	// not in the colour pass, which would be left with a hole. buffers
	// whatever isn't on the gpu yet, and refreshes drawn_nodes (the colour
	// pass's)
	std::vector<draw_t> buildDrawList(render_ctx_t &ctx, const frame_t &f) {
		// 8-bit octant mask flags of nodes, filled in from the children
		for (auto node : potential_nodes) node->mask = node->shadow_mask = 0;

		// potential-set membership, only needed for lod debug tinting
		std::unordered_set<rocktree_t::node_t *> potential_set;
		if (debug_lod)
			for (auto node : potential_nodes)
				if (!node->shadow_only) potential_set.insert(node);

		// for leaving the export rect's off-screen nodes out (below)
		auto frustum_planes = getFrustumPlanes(f.viewprojection);

		drawn_nodes.clear();
		std::vector<draw_t> draws;
		draws.reserve(potential_nodes.size());

		// reverse level order: children before parents
		for (auto it = potential_nodes.rbegin(); it != potential_nodes.rend(); ++it) {
			auto node = *it;
			assert(node->level > 0);
			assert(node->can_have_data);
			if (node->dl_state != dl_state_downloaded) continue;


			// set octant mask of previous node
			if (node->up) {
				node->up->shadow_mask |= 1 << node->octant;
				if (!node->shadow_only) node->up->mask |= 1 << node->octant;
			}

			// skip if node is masked completely (the shadow mask covers
			// at least what the colour mask does)
			auto color = !node->shadow_only;
			auto self_mask = color ? node->mask : (uint8_t)0xff;
			auto shadow_mask = node->shadow_mask;
			if (shadow_mask == 0xff) continue;
			// a tile too coarse for the tunnel to roll leaves a hole (see
			// tunnelTooCoarse). the parent's mask bit is already set above,
			// so no ancestor fills the hole with its own slab
			if (tube.tunnelTooCoarse(node)) continue;
			// the export rect's nodes are wanted off screen too, but only
			// to be resident: drawing them there is thousands of draw calls
			// for nothing, unless they can cast into the map. (outside tube
			// mode — rolled, a node's place on screen isn't its obb's)
			if (node->export_walk_ms > 0 && !tube.on
				&& obb_frustum_outside == classifyObbFrustum(&node->obb, frustum_planes)) {
				if (!caster_planes_live
					|| obb_frustum_outside == classifyObbFrustum(&node->obb, caster_planes)) continue;
				color = false;
			}

			if (color && self_mask != 0xff) drawn_nodes.push_back({ node, self_mask });
			else color = false;

			// octants where a finer tile is wanted but not drawn, and why:
			// three bits per octant saying where that tile's request is
			// (the shader's legend — 1 not yet requested, 2 cache lookup,
			// 3 network, 4 decoding, 5 waiting to retry after a failure).
			// children are drawn before parents here, so this node's mask
			// is already complete
			uint32_t stale_mask = 0;
			if (debug_lod) {
				// (a child bulk still downloading is being filled in on the
				// decode pool, and nothing under it is in the potential set)
				auto b = node->child_bulk;
				auto children = !b ? node->children
					: b->dl_state == dl_state_downloaded ? b->children : nullptr;
				for (auto o = 0; children && o < 8; o++) {
					if (self_mask & (1 << o)) continue;
					auto child = children[o];
					if (!child || !potential_set.count(child)) continue;
					uint32_t phase;
					if (child->dl_state == dl_state_stub) phase = child->dl_fails ? 5 : 1;
					else switch (child->fetch_phase.load()) {
						case fetch_phase_cache: phase = 2; break;
						case fetch_phase_decode: phase = 4; break;
						default: phase = 3; break;
					}
					stale_mask |= phase << (3 * o);
				}
			}
			for (auto &mesh : node->meshes)
				if (!mesh.buffered) { bufferMesh(mesh, ctx); uploads_this_frame++; }
			draws.push_back({ node, self_mask, shadow_mask, color, stale_mask });
		}
		return draws;
	}

	// whether the terrain is shadowed this frame, the sun's view, and the
	// terrain program's shadow uniforms (renderSetShadow)
	struct shadow_t {
		bool live;
		Matrix4d sun_view;
		float box[4], shade, texel_uv, bias;
		Vector3f up, eye;
		Matrix3f rot;
	};

	// --- sun shadows -------------------------------------------------
	// a depth map from the sun over the casters (the walk keeps what can
	// shade the view's surface, on screen or not: see casterPlanes),
	// fitted to the near part of the view: the frustum from the near plane out to a few times the
	// distance to the terrain under the crosshair. the box is square in
	// the sun's frame, its size quantized and its centre snapped to whole
	// texels, so the map doesn't crawl as the camera moves; depth runs
	// from 8 km toward the sun (the casters: towers, ridges) to just past
	// the frustum. nothing changed since the last map (a still camera, no
	// new tiles, the same sun) and it is reused
	bool shadowsLive() const { return shadows_on && !tube.on && sun_el > 0.5 * M_PI / 180.0; }

	// the box for this frame's camera and sun: for a glamour sample
	// (jittered) or, for the walk, the unmoved camera's
	shadow_fit_t shadowFit(const frame_t &f, const camera_t &cam, bool sample) {
		shadow_fit_t s;
		s.live = shadowsLive();
		if (!s.live) return s;
		if (shadow_focus <= 0) shadow_focus = fmax(1.0, centerDistanceMesh(cam));
		auto &eye = sample ? f.eye : f.key_eye, &direction = sample ? f.direction : f.key_direction;
		auto &right = sample ? f.right : f.key_right, &cam_up = sample ? f.cam_up : f.key_up;
		auto near = f.near, far = f.far, ortho_half_extent = f.ortho_half_extent;
		auto aspect_ratio = f.aspect_ratio;
		// (the lens offset moves the eye's frame a hair, which mustn't
		// move the walk's box)
		Vector3d local_up = f.local_up, east = f.east, L = f.sun_dir;
		if (!sample) sunFrame(eye, local_up, east, L);
		Vector3d sx = local_up.cross(L);
		if (sx.norm() < 1e-6) sx = east; else sx.normalize();
		if (sample && (glamour.sun_x != 0 || glamour.sun_y != 0)) {
			L = (L + sx * glamour.sun_x + L.cross(sx) * glamour.sun_y).normalized();
			sx = local_up.cross(L);
			if (sx.norm() < 1e-6) sx = east; else sx.normalize();
		}
		Vector3d sy = L.cross(sx);
		auto reach = fmin(far, fmin(40000.0, fmax(1000.0, 2.5 * shadow_focus)));
		reach = pow(2.0, ceil(log2(reach) * 4.0) / 4.0); // quantized: a small zoom doesn't refit
		std::vector<Vector3d> corners;
		if (cam.ortho) {
			auto hh = ortho_half_extent, hw = ortho_half_extent * aspect_ratio;
			for (auto d : { fmax(0.0, shadow_focus - reach), shadow_focus + reach })
				for (auto i : { -1.0, 1.0 }) for (auto j : { -1.0, 1.0 })
					corners.push_back(eye + direction * d + right * (i * hw) + cam_up * (j * hh));
		} else {
			auto t = tan(cam.fov / 2.0);
			for (auto d : { fmax(1.0, (double)near), reach })
				for (auto i : { -1.0, 1.0 }) for (auto j : { -1.0, 1.0 })
					corners.push_back(eye + direction * d + right * (i * d * t * aspect_ratio) + cam_up * (j * d * t));
		}
		double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY, z0 = INFINITY, z1 = -INFINITY;
		for (auto &c : corners) {
			x0 = fmin(x0, c.dot(sx)); x1 = fmax(x1, c.dot(sx));
			y0 = fmin(y0, c.dot(sy)); y1 = fmax(y1, c.dot(sy));
			z0 = fmin(z0, c.dot(L)); z1 = fmax(z1, c.dot(L));
		}
		auto half = 0.5 * fmax(x1 - x0, y1 - y0);
		half = pow(2.0, ceil(log2(half) * 8.0) / 8.0);
		s.texel_m = 2.0 * half / shadow_size;
		auto jx = sample ? glamour.shadow_x : 0.0, jy = sample ? glamour.shadow_y : 0.0;
		auto cx = (floor(0.5 * (x0 + x1) / s.texel_m) + jx) * s.texel_m;
		auto cy = (floor(0.5 * (y0 + y1) / s.texel_m) + jy) * s.texel_m;
		// the map's origin: the box's centre across the sun, and along it
		// the frustum's most sunward point. depth runs along -L from there
		s.O = sx * cx + sy * cy + L * z1;
		s.L = L; s.sx = sx; s.sy = sy;
		s.half = half;
		s.far = (z1 - z0) + 50.0;
		s.lo = Vector3d(x0, y0, z0); s.hi = Vector3d(x1, y1, z1);
		s.depth = cam.ortho ? shadow_focus + reach : reach;
		return s;
	}

	shadow_t shadowPass(render_ctx_t &ctx, const frame_t &f, const camera_t &cam,
			const std::vector<draw_t> &draws) {
		auto shadows_live = shadowsLive();
		float shadow_box_f[4] = {1, 1, 0, 1};
		Matrix4d sun_view = Matrix4d::Identity();
		double shadow_texel_m = 1;
		if (shadows_live) {
			if (!walk_reused) shadow_focus = fmax(1.0, centerDistanceMesh(cam));
			auto s = shadowFit(f, cam, true);
			auto &L = s.L, &sx = s.sx, &sy = s.sy, &O = s.O;
			auto half = s.half;
			shadow_texel_m = s.texel_m;
			for (int k = 0; k < 3; k++) { sun_view(0, k) = sx[k]; sun_view(1, k) = sy[k]; sun_view(2, k) = L[k]; }
			sun_view(0, 3) = -sx.dot(O); sun_view(1, 3) = -sy.dot(O); sun_view(2, 3) = -L.dot(O);
			shadow_box_f[0] = shadow_box_f[1] = (float)half;
			shadow_box_f[2] = (float)shadow_near; shadow_box_f[3] = (float)s.far;
			auto stale = !walk_reused || uploads_this_frame > 0 || (int)draws.size() != shadow_key_drawn
				|| L != shadow_key_l || O != shadow_key_o || half != shadow_key_half;
			if (stale) {
				shadow_key_l = L; shadow_key_o = O; shadow_key_half = half; shadow_key_drawn = (int)draws.size();
				renderShadowBegin(ctx, shadow_size, shadow_box_f);
				for (auto &dr : draws) {
					if (dr.shadow_mask == 0xff) continue;
					Matrix4f mv = (sun_view * dr.node->matrix_globe_from_mesh).cast<float>();
					renderShadowNode(ctx, mv.data(), dr.shadow_mask);
					for (auto &mesh : dr.node->meshes) renderShadowMesh(mesh);
				}
				renderShadowEnd(ctx);
			}
		} else {
			shadow_key_drawn = -1; // the next live frame redraws the map
		}
		// fade the shade out as the sun sets: by then the sun tint has
		// taken the light down anyway, and a low sun's shadows run to the
		// horizon
		auto shade = shadow_strength * smooth(0.5 * M_PI / 180.0, 6.0 * M_PI / 180.0, sun_el);
		shadow_t sh;
		sh.live = shadows_live;
		sh.sun_view = sun_view;
		std::copy(shadow_box_f, shadow_box_f + 4, sh.box);
		sh.shade = (float)shade;
		sh.texel_uv = (float)(1.0 / shadow_size);
		sh.bias = (float)((0.3 + 0.5 * shadow_texel_m) / (shadow_box_f[3] - shadow_box_f[2]));
		sh.up = (sun_view.block<3, 3>(0, 0) * f.local_up).cast<float>();
		sh.rot = sun_view.block<3, 3>(0, 0).cast<float>();
		sh.eye = transformPoint(sun_view, f.eye).cast<float>();
		return sh;
	}

	// the terrain: a node's meshes with its octant mask, shadowed, and in
	// tube mode through the warp
	void drawTerrain(render_ctx_t &ctx, const frame_t &f, const std::vector<draw_t> &draws,
			const shadow_t &sh) {
		renderTerrainBegin(ctx, f);
		tube.setTubeUniforms(ctx, f.viewprojection);
		renderSetShadow(ctx, sh.live, sh.box, sh.shade, sh.texel_uv, sh.bias, sh.up.data(), sh.rot.data(), sh.eye.data());
		for (auto &dr : draws) {
			if (!dr.color) continue;
			auto node = dr.node;
			auto self_mask = dr.self_mask;
			auto stale_mask = dr.stale_mask;
			if (sh.live) {
				Matrix4f mv = (sh.sun_view * node->matrix_globe_from_mesh).cast<float>();
				renderSetShadowNode(ctx, mv.data());
			}

			// float transform matrix
			Matrix4d transform = f.viewprojection * node->matrix_globe_from_mesh;
			Matrix4f transform_float;
			for(auto i = 0; i < 16; ++i) transform_float.data()[i] = (float)(transform.data()[i]);

			// bind, draw (buildDrawList buffered every mesh)
			renderSetTransform(ctx, transform_float.data());
			tube.setTubeNode(ctx, node->matrix_globe_from_mesh);
			auto draw = [&] {
				for (auto &mesh : node->meshes) {
					bindAndDrawMesh(mesh, self_mask, stale_mask, ctx);
				}
			};
			if (tube.on && tube.body) tube.drawBodyNode(ctx, node->obb, draw);
			else draw();
		}
	}

	// the dropped track, over the terrain it was walked on. after the
	// meshes, so the depth test it loses against is a complete one; not
	// in tube mode, where the ground has been rolled out from under it
	// and a track drawn in globe space would hang in the air where the
	// terrain used to be
	void drawPath(render_ctx_t &ctx, const frame_t &f) {
		if (path_up.empty() || tube.on || path_rgba[3] <= 0) return;
		pathProbeGround(); // against this frame's drawn set
		// draping keeps arriving for seconds after a drop, and rebuilding
		// a long track's ribbon every frame to show one more measured
		// point is not worth it — batch a few frames' worth
		if (path_dirty && ++path_rebuild_wait >= 8) {
			path_dirty = false;
			path_rebuild_wait = 0;
			pathRebuild();
			renderPathUpload(ctx, path_verts.data(), (int)(path_verts.size() / 8));
		}
		Matrix4d to_clip = f.viewprojection;
		to_clip.col(3) = f.viewprojection * Vector4d(path_origin.x(), path_origin.y(), path_origin.z(), 1.0);
		Matrix4f to_clip_f = to_clip.cast<float>();
		renderPathDraw(ctx, to_clip_f.data(), path_rgba, path_width, path_depth_bias, f.width, f.height);
		if (path_cursor_on) {
			// the path's own color, exactly: the disc is the track's
			// position marker, not a second thing sitting on it
			Vector3f c = (path_cursor - path_origin).cast<float>();
			renderPathCursorDraw(ctx, to_clip_f.data(), path_rgba,
				fmaxf(11.0f, path_width * 3.0f), path_depth_bias, f.width, f.height, c.data());
		}
	}

	// per-section averages and the download picture, every 2s: printed,
	// and kept for EarthView::getStats
	void reportStats(render_ctx_t &ctx, double dt_ms) {
		sec.frames++;
		sec_report_ms += dt_ms;
		if (sec_report_ms > 2000 && sec.frames > 0) {
			sec_report_ms = 0;
			avg.bfs = sec.bfs / sec.frames; avg.dl = sec.dl / sec.frames;
			avg.evict = sec.evict / sec.frames; avg.draw = sec.draw / sec.frames;
			avg.oct = sec.oct / sec.frames; avg.cull = sec.cull / sec.frames;
			avg.lod = sec.lod / sec.frames;
			avg.frames = sec.frames / 2;
			printf("sections avg ms: bfs %.2f, dl %.2f, evict %.2f, draw %.2f (%d frames; per frame: oct %ld, cull %ld, lod %ld)"
				" · gpu %.2f (shadow %.2f, scene %.2f, occlusion %.2f, post %.2f)\n",
				avg.bfs, avg.dl, avg.evict, avg.draw,
				sec.frames, avg.oct, avg.cull, avg.lod,
				ctx.gpu.avg[0] + ctx.gpu.avg[1] + ctx.gpu.avg[2] + ctx.gpu.avg[3],
				ctx.gpu.avg[gpu_shadow], ctx.gpu.avg[gpu_scene], ctx.gpu.avg[gpu_occlusion], ctx.gpu.avg[gpu_post]);
			// what the view is still waiting for. the section above times the
			// cpu work; this says whether anything is actually coming. a view
			// that never completes with waiting=0 and inflight=0 is not slow,
			// it is stuck: those nodes asked once and nothing answered
			printf("tiles: want %d (casters %d) loaded %d waiting %d inflight %d downloading %d retrying %d"
				" · deepest level %d · bulks: blocked %d inflight %d started %d%s\n",
				stat.nodes_wanted, stat.nodes_shadow, stat.nodes_loaded, stat.nodes_stub,
				(int)nodes_in_flight, stat.nodes_stuck, stat.nodes_failed,
				stat.max_level, stat.bulk_blocked, (int)bulks_in_flight,
				stat.bulks_started, scene_complete ? " · complete" : "");
			sec = {};
		}
	}

	// cull, schedule downloads, evict, draw. the shell has already begun the
	// frame (renderFrameBegin) and will end it after this returns
	void updateAndDraw(render_ctx_t &ctx, const camera_t &cam, int width, int height, double dt_ms) {
		if (!ready()) return;

		// stage 2 of the tube pipeline: as tiles stream in, keep attempting
		// the ground measurement against last frame's drawn meshes; it locks
		// as soon as the whole rect is covered at measurement resolution —
		// long before full res. the host watches groundLocked, rolls the
		// tube up, and the remaining detail streams into the rolled tube
		if (tube.tunnel_on) tube.tunnelUpdate(cam, dt_ms);
		else if (tube.on && !tube.ground_locked)
			tube.tubeLockGround(true);
		uploads_this_frame = 0;
		frame = setupFrame(ctx, cam, width, height);
		setAtmosphere(frame);
		auto &f = frame;

		auto t0 = ticksMs();
		// downloaded nodes and bulk metadata are both lru caches with byte
		// quotas (see evict); nothing is ever dropped on
		// a clock
		auto now_ms = ticksMs();
		auto all_loaded = walk(f, cam, now_ms, shadowFit(f, cam, false));
		auto t1 = ticksMs();
		sec.bfs += t1 - t0;

		scheduleDownloads(f, now_ms, all_loaded);
		auto t2 = ticksMs();
		sec.dl += t2 - t1;

		if (evict_tick++ % 15 == 0) evict(now_ms, dt_ms);
		auto t3 = ticksMs();
		sec.evict += t3 - t2;

		auto draws = buildDrawList(ctx, f);
		auto sh = shadowPass(ctx, f, cam, draws);
		drawTerrain(ctx, f, draws, sh);
		if (sky_on) renderSkyDraw(ctx, f); // where the terrain left the far plane
		drawPath(ctx, f);
		sec.draw += ticksMs() - t3;
		reportStats(ctx, dt_ms);
	}
};
