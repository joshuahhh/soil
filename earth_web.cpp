// web library shell: attach the renderer to any canvas element and drive it
// from js. unlike the classic emscripten app build (main.cpp), this owns no
// window, no input, and no main loop — the host page schedules frames and
// optionally supplies a fetch function to control transport and caching.
//
// build: ./build.sh weblib  ->  web/earth.js + web/earth.wasm
// usage: see web/demo.html
//
//   const Module = await createEarthModule();
//   const view = Module.createView('#canvas', async (path, id) => {
//     ...resolve bytes however you like (indexeddb, proxy, ...)...
//     Module.deliverFetch(id, true, bytesUint8Array);
//   });
//   view.setPose(lat, lon, alt, heading, tilt);   // degrees/meters
//   view.fly(yaw, pitch, roll, fwd, back, left, right, up, down, slow, viewFrame, gain, dtMs);
//   view.frame(dtMs);                              // from requestAnimationFrame
//   view.setOrtho(on);                             // orthographic projection
//   view.orbit(headingDeg, tiltDeg);               // revolve around the screen-center point
//   view.zoomOrtho(factor);                        // ortho zoom (boresight dolly), >1 zooms in
//   view.pickCenter();                             // {lat,lon,alt,dist,src} under the crosshair, or null
//   view.pickNdc(nx, ny);                          // the same pick through any viewport point (ndc, perspective only)
//   view.planetRadius();                           // meters; the datum setPose's alt and the picks are measured from

#include <fstream>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/bind.h>
#include <GLES3/gl3.h>

#include <math.h>
#include <Eigen/Dense>
using namespace Eigen;
#include "rocktree_util.h"
#include "earth_camera.h"
#include "earth_core.h"

using emscripten::val;

// fetches delegated to a js function (path, token); js answers by calling
// Module.deliverFetch(token, ok, uint8array). single-threaded on the js main
// thread, so no locking.
//
// the request ids passed into fetch() are NOT globally unique — getBulk,
// getNode and getPlanetoid each run their own counter and only use the id to
// find their own map entry — so the pending map is keyed by a token of our
// own and remembers each request's (thunk, id) pair
struct js_fetcher_t : fetcher_t {
	val fn = val::undefined();
	std::map<int, std::pair<fetch_thunk_t, int>> pending;
	int next_token = 0;

	void fetch(const char *path, int i, fetch_thunk_t thunk) override {
		auto token = next_token++;
		pending[token] = { thunk, i };
		fn(std::string(path), token);
	}

	void deliver(int token, bool ok, val bytes) {
		auto it = pending.find(token);
		if (it == pending.end()) return;
		auto thunk = it->second.first;
		auto i = it->second.second;
		pending.erase(it);
		if (!ok) {
			thunk(i, 1, nullptr, 0);
			return;
		}
		auto vec = emscripten::convertJSArrayToNumberVector<uint8_t>(bytes);
		thunk(i, 0, vec.data(), vec.size());
	}
};
static js_fetcher_t js_fetcher;

// must be set before createView: msaa is a webgl context attribute, not
// state that can be toggled on a live context. on by default — the shimmer
// on distant building silhouettes is geometric aliasing, which no amount of
// texture filtering reaches (see tools/shimmer.mjs). ?msaa=0 opts out
static bool g_antialias = true;

struct EarthView {
	render_ctx_t ctx = {};
	camera_t camera = {
		{ 1329866.230289, -4643494.267515, 4154677.131562 }, // nyc
		{ 0.219862, 0.419329, 0.312226 },
		0.25 * M_PI,
	};
	earth_core_t earth;
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl = 0;
	int sky_color = 0x83b5fc;

	void init(const std::string &canvas_selector) {
		EmscriptenWebGLContextAttributes attr;
		emscripten_webgl_init_context_attributes(&attr);
		attr.depth = 1;
		attr.antialias = g_antialias;
		// webgl 2 (gles 3.0). note the textures turn out to be power-of-two
		// (256x512 and friends, measured over 295 tiles), so webgl 1 could
		// have mipmapped them too — this buys the bitwise mask ops, the
		// gpu-timing extension, and multiview for stereo, not mipmapping
		attr.majorVersion = 2;
		gl = emscripten_webgl_create_context(canvas_selector.c_str(), &attr);
		if (gl <= 0) {
			fprintf(stderr, "webgl context creation failed on '%s' (%d)\n", canvas_selector.c_str(), (int)gl);
			abort();
		}
		emscripten_webgl_make_context_current(gl);
		renderInit(ctx, nullptr);
		earth.load();
	}

	double planetRadius() {
		return earth.planetoid && earth.planetoid->downloaded ? earth.planetoid->radius : 6371010.0;
	}

	bool ready() { return earth.ready(); }
	bool sceneComplete() { return earth.scene_complete; }

	// tube mode (the inception effect): roll a geodetic rectangle of terrain
	// into a cylinder. corners in degrees, any order; ewAxis picks whether
	// the tube axis runs east-west or north-south; curl 0..1 animates flat
	// slab -> closed tube. getTubeInfo reports the derived geometry so the
	// host can place the camera (radius = tube axis height)
	void setTubeRect(double lat0, double lon0, double lat1, double lon1, bool ewAxis) {
		earth.setTubeRect(lat0 * M_PI / 180.0, lon0 * M_PI / 180.0,
			lat1 * M_PI / 180.0, lon1 * M_PI / 180.0, ewAxis);
	}
	// rolling or unrolling switches the hug datum (sphere radius vs tube
	// radial), so a locked gap from the other mode is meaningless: re-lock
	void setTubeEnabled(bool on) { earth.tube_on = on; follow_height = NAN; }
	// force the ground measurement with whatever mesh is resident — the
	// host's escape hatch when a stuck download keeps coverage incomplete
	void lockTubeGround() { earth.tubeLockGround(false); }
	void setTubeCurl(double c) { earth.tube_curl = fmax(0.0, fmin(c, 1.0)); }
	// the camera in unrolled tube coordinates (see earth_core::tubePose):
	// where on the original rect the imagery around the camera came from.
	// null outside tube mode — hosts fall back to getPose for the marker
	val getTubePose() {
		double lat, lon, heading;
		if (!earth.tubePose(camera.eye, camera.direction, lat, lon, heading))
			return val::null();
		val o = val::object();
		o.set("lat", lat * 180.0 / M_PI);
		o.set("lon", lon * 180.0 / M_PI);
		o.set("heading", heading * 180.0 / M_PI);
		return o;
	}

	val getTubeInfo() {
		val o = val::object();
		o.set("radius", earth.tube_radius);
		o.set("halfLen", earth.tube_half_len);
		o.set("halfWid", earth.tube_half_wid);
		// ground level at the rect center in the same terms as setPose's alt
		// (meters above the planetoid sphere) — spawn heights add onto this.
		// only meaningful once groundLocked (measured off the loaded mesh)
		o.set("groundAlt", earth.tube_ground_radius - planetRadius());
		o.set("groundLocked", earth.tube_ground_locked);
		// last walk's want/have node counts — in tube mode that's exactly
		// the rect's download progress
		o.set("nodesWanted", earth.stat_nodes_wanted);
		o.set("nodesLoaded", earth.stat_nodes_loaded);
		return o;
	}
	// --- path overlay ---------------------------------------------------
	// a dropped track: flat [lat, lon, ele, ...] triples in degrees/meters
	// (a nan lat starts a new polyline, which is how a gpx's track segments
	// come across). style is separate so the panel's sliders don't re-upload
	// the points; drape pins the line to the mesh instead of to the file's
	// own elevations (see earth_core's path overlay)
	void setPath(val lla) {
		earth.setPath(emscripten::convertJSArrayToNumberVector<double>(lla));
	}
	void clearPath() { earth.setPath({}); }
	void setPathStyle(int rgb, double opacity, double width_px, bool drape) {
		earth.setPathStyle((rgb >> 16 & 0xff) / 255.0f, (rgb >> 8 & 0xff) / 255.0f,
			(rgb & 0xff) / 255.0f, (float)opacity, (float)width_px, drape);
	}

	// ground altitude measured under path point i (meters above the
	// planetoid sphere, the same datum as setPose's alt), or nan where the
	// drape hasn't reached that point yet. the follow camera reads it so it
	// rides the terrain rather than the track's gps altitudes; nan rather
	// than a fallback because the caller has the file's own elevations and
	// is the one that should decide when to stop using them
	double getPathGroundAlt(int i) {
		if (i < 0 || (size_t)i >= earth.path_ground.size()) return NAN;
		if (!earth.path_drape || earth.path_ground[i] <= 0) return NAN;
		return earth.path_ground[i] - planetRadius();
	}

	void setSkyColor(int rgb) { sky_color = rgb; }
	void setDebugLod(bool on) { earth.debug_lod = on; }
	// M key: mipmaps + anisotropy on the uncompressed texture path. the dxt
	// path has no mip chain to switch to (crn ships a single level and
	// glGenerateMipmap rejects compressed textures), so pair this with ?jpg
	// to actually see the difference
	void setMipmaps(bool on) { renderSetMipmaps(ctx, on); }

	// --- test/profiling hooks -------------------------------------------
	// the engine's own frame accounting, read back rather than only printed
	// to the console. the section times are the last 2s window's per-frame
	// averages (see earth_core's avg_*); note `draw` is command-submission
	// time, not gpu time — webgl is asynchronous, so it measures the cost of
	// crossing into the browser, which is what the per-node bind sequence
	// dominates. the node counts are the current frame's
	val getStats() {
		val o = val::object();
		o.set("bfsMs", earth.avg_bfs);
		o.set("dlMs", earth.avg_dl);
		o.set("evictMs", earth.avg_evict);
		o.set("drawMs", earth.avg_draw);
		o.set("fps", earth.avg_fps);
		o.set("octantsWalked", (double)earth.avg_oct);
		o.set("nodesCulled", (double)earth.avg_cull);
		o.set("nodesLodTested", (double)earth.avg_lod);
		o.set("nodesWanted", earth.stat_nodes_wanted);
		o.set("nodesLoaded", earth.stat_nodes_loaded);
		o.set("nodesDrawn", (int)earth.drawn_nodes.size());
		o.set("sceneComplete", earth.scene_complete);
		return o;
	}

	// the set of nodes the last walk actually drew, with the octant mask each
	// was drawn under. this is the direct output of lod selection + frustum
	// culling + eviction, so a golden capture of it is a far sharper
	// regression signal than pixels: it is text, it diffs readably, and it
	// is immune to gpu-dependent dxt decoding. sorted by path so the list is
	// stable across runs (drawn_nodes' order follows hash-map iteration)
	val getDrawnNodes() {
		std::vector<std::pair<std::string, uint8_t>> rows;
		rows.reserve(earth.drawn_nodes.size());
		for (auto &kv : earth.drawn_nodes)
			rows.push_back({ kv.first->request.node_key().path(), kv.second });
		std::sort(rows.begin(), rows.end());
		val arr = val::array();
		for (size_t i = 0; i < rows.size(); i++) {
			val o = val::object();
			o.set("path", rows[i].first);
			o.set("level", (int)rows[i].first.size());
			o.set("mask", (int)rows[i].second);
			arr.set((int)i, o);
		}
		return arr;
	}

	// one frame; the host calls this from requestAnimationFrame. drawable
	// size follows the canvas backing store (host sets canvas.width/height)
	void frame(double dt_ms) {
		if (!earth.ready()) return;
		emscripten_webgl_make_context_current(gl);
		int w, h;
		emscripten_webgl_get_drawing_buffer_size(gl, &w, &h);
		if (w <= 0 || h <= 0) return;
		ctx.width = w;
		ctx.height = h;
		// terrain hug wants the finest tiles under the camera selected even
		// when the view frustum culls them (see earth_core ground column)
		earth.ground_column_on = terrain_follow && !camera.ortho && !camera.airplane;
		earth.ground_column_point = camera.eye;
		renderFrameBegin(ctx, nullptr, w, h, sky_color);
		earth.updateAndDraw(ctx, camera, w, h, dt_ms);
		renderFrameEnd(ctx);
	}

	// pose in degrees/meters, the vocabulary of 2d maps. roll is the
	// airplane-frame bank angle (0 outside airplane mode) — not part of the
	// geodetic pose struct, reported here so hosts can persist it
	val getPose() {
		auto p = cameraToPose(camera, planetRadius());
		val o = val::object();
		o.set("lat", p.lat * 180.0 / M_PI);
		o.set("lon", p.lon * 180.0 / M_PI);
		o.set("alt", p.alt);
		o.set("heading", p.heading * 180.0 / M_PI);
		o.set("tilt", p.tilt * 180.0 / M_PI);
		o.set("fov", camera.fov * 180.0 / M_PI);
		double roll = 0;
		if (camera.airplane) {
			auto up = camera.eye.normalized();
			Vector3d right = camera.direction.cross(up);
			if (right.norm() > 1e-9) {
				right.normalize();
				Vector3d level_up = right.cross(camera.direction).normalized();
				roll = atan2(camera.body_up.dot(right), camera.body_up.dot(level_up));
			}
		}
		o.set("roll", roll * 180.0 / M_PI);
		return o;
	}

	// bank the airplane frame to an absolute roll angle (degrees, 0 =
	// wings level); the setPose counterpart for the roll degree of freedom
	void setRoll(double deg) {
		if (!camera.airplane) return;
		alignAirplaneUp(camera);
		camera.body_up = AngleAxisd(deg * M_PI / 180.0, camera.direction) * camera.body_up;
	}

	void setPose(double lat, double lon, double alt, double heading, double tilt) {
		geo_pose_t p;
		p.lat = lat * M_PI / 180.0;
		p.lon = lon * M_PI / 180.0;
		p.alt = alt;
		p.heading = heading * M_PI / 180.0;
		p.tilt = tilt * M_PI / 180.0;
		// poseToCamera builds a fresh camera_t; carry over the state that
		// isn't part of a pose (otherwise e.g. a map-click teleport silently
		// drops orthographic mode)
		auto ortho = camera.ortho;
		auto ortho_extent = camera.ortho_extent;
		auto airplane = camera.airplane;
		camera = poseToCamera(p, planetRadius(), camera.fov);
		camera.ortho = ortho;
		camera.ortho_extent = ortho_extent;
		camera.airplane = airplane;
		if (airplane) alignAirplaneUp(camera); // teleports land wings-level
		clampOrthoTilt(camera);
	}

	void setFov(double deg) {
		camera.fov = fmax(1.0, fmin(deg, 100.0)) * M_PI / 180.0;
	}

	// orthographic projection; on entry the view extent is initialized to
	// what the fov shows at the terrain distance, so the mode switch keeps
	// the apparent scale — from then on only zooms change it
	void setOrtho(bool on) {
		camera.ortho = on;
		if (on)
			camera.ortho_extent = fmax(1.0,
				earth.centerDistanceMesh(camera) * tan(camera.fov / 2.0));
		clampOrthoTilt(camera);
	}
	bool getOrtho() { return camera.ortho; }

	// airplane controls (tube flying): yaw/pitch/roll about the camera's own
	// axes, movement in the body frame, no horizon clamps
	void setAirplane(bool on) {
		camera.airplane = on;
		if (on) alignAirplaneUp(camera);
	}

	// terrain hug: ground-frame flight where "level" means a fixed height
	// above the mesh instead of a fixed sphere radius — cruising follows
	// hills and buildings; R/F changes the offset. the offset locks to the
	// current height above the mesh on the first frame that can measure it
	bool terrain_follow = false;
	double follow_height = NAN;
	void setTerrainFollow(bool on) {
		terrain_follow = on;
		follow_height = NAN;
	}

	// ortho zoom: dolly along the boresight, which scales the derived view
	// extent by exactly 1/factor while the screen center stays put. leaves
	// the fov alone (it stays whatever perspective mode had), and altitude
	// tracks the zoom, so toggling back to perspective looks right
	void zoomOrtho(double factor) {
		if (!earth.ready() || !(factor > 0)) return;
		// the extent is the zoom; the dolly keeps the eye at a matching
		// distance so toggling back to perspective looks right
		camera.ortho_extent = fmax(1.0, camera.ortho_extent / factor);
		auto dist = earth.centerDistanceMesh(camera);
		auto new_dist = fmax(1.0, dist / factor);
		camera.eye += camera.direction * (dist - new_dist);
	}

	// ortho drag pan: grab-the-ground — translate the camera, holding
	// altitude, so the terrain under the pointer follows it. deltas in css
	// pixels of a viewport viewport_h pixels high. horizontal drags map to
	// screen-right meters directly; vertical drags divide by sin(tilt),
	// the foreshortening of ground distance in an oblique view
	void panOrtho(double dx_px, double dy_px, double viewport_h) {
		if (!earth.ready() || viewport_h <= 0) return;
		auto half_extent = camera.ortho_extent > 0 ? camera.ortho_extent
			: fmax(1.0, earth.centerDistanceMesh(camera) * tan(camera.fov / 2.0));
		auto mpp = 2.0 * half_extent / viewport_h; // meters per pixel
		auto up = camera.eye.normalized();
		Vector3d sideways = camera.direction.cross(up);
		if (sideways.norm() < 1e-9) return; // looking straight down the axis
		sideways.normalize();
		Vector3d horizontal = up.cross(sideways).normalized();
		auto sin_tilt = fmax(0.05, fabs(camera.direction.dot(up)));
		Vector3d new_eye = camera.eye - sideways * (dx_px * mpp)
			+ horizontal * (dy_px * mpp / sin_tilt);
		camera.eye = new_eye.normalized() * camera.eye.norm(); // hold altitude
	}

	// revolve the camera around whatever is at the center of the screen
	// (raycast against the drawn terrain, sphere fallback). heading turns
	// about the pivot's local up (positive increases heading); tilt swings
	// about the horizontal screen-right axis through the pivot (positive
	// tilts toward the horizon, clamped by the tilt limits)
	void orbit(double heading_deg, double tilt_deg) {
		if (!earth.ready()) return;
		Vector3d pivot;
		if (!earth.raycast(camera.eye, camera.direction, pivot)) return;
		if (heading_deg != 0) orbitCamera(camera, pivot, -heading_deg * M_PI / 180.0);
		if (tilt_deg != 0) orbitCameraTilt(camera, pivot, tilt_deg * M_PI / 180.0);
	}

	// what the camera is looking at, in degrees/meters; null when the view
	// hits no loaded terrain (no fallback — see earth_core::raycast). dist
	// is meters from the eye — both for the pick debug overlay and tests
	val pickCenter() {
		Vector3d hit;
		if (!earth.ready()) return val::null();
		if (earth.raycast(camera.eye, camera.direction, hit) == earth_core_t::raycast_miss)
			return val::null();
		auto r = hit.norm();
		val o = val::object();
		o.set("lat", asin(hit.z() / r) * 180.0 / M_PI);
		o.set("lon", atan2(hit.y(), hit.x()) * 180.0 / M_PI);
		o.set("alt", r - planetRadius());
		o.set("dist", (hit - camera.eye).norm());
		o.set("src", std::string("mesh"));
		return o;
	}

	// the same pick through an arbitrary point of the viewport, in normalized
	// device coordinates (-1..1, +y up). perspective only — the host's point-
	// pair alignment is a pinhole fit, and neither a parallel projection nor
	// the rolled tube's warp is one. the reported lat/lon/alt is spherical, so
	// the host can rebuild the exact ecef point as geoUp(lat,lon)*(R+alt)
	val pickNdc(double nx, double ny) {
		if (!earth.ready() || camera.ortho || earth.tube_on) return val::null();
		int w, h;
		emscripten_webgl_get_drawing_buffer_size(gl, &w, &h);
		if (w <= 0 || h <= 0) return val::null();
		// the frustum ray, built from the same basis lookAt derives (right =
		// direction x up, view-up = right x direction)
		Vector3d world_up = camera.eye.normalized();
		Vector3d right = camera.direction.cross(world_up);
		if (right.norm() < 1e-9) return val::null(); // looking straight down the axis
		right.normalize();
		Vector3d up = right.cross(camera.direction).normalized();
		auto t = tan(camera.fov / 2.0);
		Vector3d dir = (camera.direction + right * (nx * t * ((double)w / h))
			+ up * (ny * t)).normalized();
		Vector3d hit;
		if (earth.raycast(camera.eye, dir, hit) == earth_core_t::raycast_miss)
			return val::null();
		auto r = hit.norm();
		val o = val::object();
		o.set("lat", asin(hit.z() / r) * 180.0 / M_PI);
		o.set("lon", atan2(hit.y(), hit.x()) * 180.0 / M_PI);
		o.set("alt", r - planetRadius());
		o.set("dist", (hit - camera.eye).norm());
		return o;
	}

	// built-in flying controls; the host translates its pointer/keyboard
	// events into this (yaw/pitch in radians for this frame). view_frame
	// switches forward/back and raise/lower from ground-frame cruise/pedestal
	// to view-frame dolly/boom; gain is the host's sticky speed multiplier
	void fly(double yaw, double pitch, double roll,
			bool forward, bool back, bool left, bool right,
			bool raise, bool lower, bool slow, bool view_frame, double gain, double dt_ms) {
		if (!earth.ready()) return;
		camera_input_t in;
		in.yaw = yaw;
		in.pitch = pitch;
		in.roll = roll;
		in.forward = forward;
		in.back = back;
		in.left = left;
		in.right = right;
		in.raise = raise;
		in.lower = lower;
		in.slow = slow;
		in.view_frame = view_frame;
		in.speed_gain = gain;
		in.dt_ms = dt_ms;
		auto hugging = terrain_follow && !camera.airplane && !camera.ortho;
		// tube hug: the same G, but the camera lives in the rolled
		// cylinder's coordinates — "down" is outward along its radial, and
		// the held height is the radial gap to the wall (tube flying is
		// airplane-frame, so the two hug variants never overlap). movement
		// and roll belong to the cylindrical walker below; the airplane
		// branch only turns the head
		auto tube_hugging = terrain_follow && earth.tube_on && camera.airplane;
		// until the gap locks (wall below measurable), tube G flies as a
		// plain airplane — never strand the camera unable to move
		auto tube_locked = tube_hugging && !isnan(follow_height);
		if (hugging && !isnan(follow_height))
			in.altitude_override = fmax(5.0, follow_height); // speed from true height
		auto in2 = in;
		if (tube_locked) {
			in2.forward = in2.back = in2.left = in2.right = false;
			in2.raise = in2.lower = false;
			in2.roll = 0;
		}
		auto r_before = camera.eye.norm();
		applyCameraInput(camera, in2, planetRadius());
		if (hugging) {
			// radial movement (pedestal, view-frame boom) adjusts the offset;
			// cruise then holds it over whatever the mesh does below
			follow_height += camera.eye.norm() - r_before;
			auto g = earth.groundRadiusUnder(camera.eye);
			if (g > 0) {
				if (isnan(follow_height)) follow_height = camera.eye.norm() - g;
				follow_height = fmax(2.0, follow_height);
				// ease onto the target height so rooftop edges read as a
				// glide, not a bounce
				auto k = 1.0 - exp(-dt_ms / 250.0);
				auto r = camera.eye.norm();
				camera.eye = camera.eye.normalized() * (r + (g + follow_height - r) * k);
			}
		}
		if (tube_hugging) {
			auto c = earth.tubeCyl(camera.eye);
			double rho_wall;
			if (!tube_locked) {
				// the gap locks to the current height above the wall on the
				// first frame that can measure it; the walker takes over
				// next frame
				if (earth.tubeWallRho(c.x, c.theta, rho_wall))
					follow_height = fmax(2.0, rho_wall - c.rho);
			} else {
				// ground-frame cruise, transposed: forward/strafe slide along
				// the wall (the view projected onto the axial+circumferential
				// tangent plane), R/F changes the held gap. speed from the
				// gap, the true distance to what you're skimming
				auto zoom_scale = tan(camera.fov / 2.0) / tan(0.125 * M_PI);
				auto speed = fmax(5.0, follow_height * zoom_scale);
				auto mag = speed * (in.dt_ms / 1000.0) * (in.slow ? 0.1 : 1.0) * in.speed_gain;
				auto fwd = (in.forward ? 1.0 : 0.0) - (in.back ? 1.0 : 0.0);
				auto vert = (in.raise ? 1.0 : 0.0) - (in.lower ? 1.0 : 0.0);
				auto lat = (in.right ? 1.0 : 0.0) - (in.left ? 1.0 : 0.0);
				Vector3d up = -c.radial; // cylinder up: toward the axis
				Vector3d sideways = camera.direction.cross(up);
				// looking straight at the wall or the axis: any horizontal
				// will do, take the axial one
				if (sideways.norm() < 1e-9) sideways = earth.tube_axis;
				sideways.normalize();
				Vector3d horizontal = up.cross(sideways).normalized();
				Vector3d step = (fwd * horizontal + lat * sideways) * mag;
				// advance the cylindrical coordinates by the step's tangent
				// components; the angle moves by arc length over the radius
				auto x = c.x + step.dot(earth.tube_axis);
				Vector3d tangential = earth.tube_axis.cross(c.radial);
				auto theta = c.theta + step.dot(tangential) / fmax(1.0, c.rho);
				follow_height = fmax(2.0, follow_height + vert * mag);
				// glue: remeasure the wall under the new footprint and ease
				// onto rho = wall - gap, so rooftop edges read as a glide,
				// not a bounce. no measurement -> hold the current radius
				auto k = 1.0 - exp(-dt_ms / 250.0);
				auto rho = c.rho;
				if (earth.tubeWallRho(x, theta, rho_wall))
					rho += (fmax(1.0, rho_wall - follow_height) - rho) * k;
				camera.eye = earth.tubeCylPoint(x, theta, rho);
				// parallel transport: the frame turns with the angle it
				// traveled, so a horizontal gaze stays horizontal all the
				// way around the loop (walking the full circumference is one
				// 2*pi rotation about the axis, as it should be)
				AngleAxisd turn(theta - c.theta, earth.tube_axis);
				camera.direction = (turn * camera.direction).normalized();
				camera.body_up = (turn * camera.body_up).normalized();
				// level to the cylinder: up is inward radial, eased so
				// engaging G mid-bank rolls level instead of snapping (the
				// hug owns the roll axis). the blend only degenerates with
				// body up pointing straight at the wall; skip that frame
				// rather than normalize a near-zero vector
				Vector3d radial_new = earth.tubeCyl(camera.eye).radial;
				Vector3d bu = camera.body_up - (radial_new + camera.body_up) * k;
				if (bu.norm() > 1e-6) camera.body_up = bu.normalized();
			}
		}
	}
};

// one view per page for now: the decode pools and request maps in
// rocktree_util.h are process-wide singletons
static EarthView *the_view = nullptr;

EarthView *createView(std::string canvas_selector, val fetch_fn) {
	if (the_view) {
		fprintf(stderr, "createView: only one view per page is supported\n");
		abort();
	}
	if (!fetch_fn.isUndefined() && !fetch_fn.isNull()) {
		js_fetcher.fn = fetch_fn;
		earth_fetcher = &js_fetcher;
	} // else the built-in direct fetcher is used
	the_view = new EarthView();
	the_view->init(canvas_selector);
	return the_view;
}

void deliverFetch(int i, bool ok, val bytes) {
	js_fetcher.deliver(i, ok, bytes);
}

// pretend the gpu lacks s3tc (jpg textures); call before createView
void setAntialias(bool on) { g_antialias = on; }

void forceJpgTextures() {
	texture_s3tc_supported = false;
}

EMSCRIPTEN_BINDINGS(earth) {
	emscripten::class_<EarthView>("EarthView")
		.function("frame", &EarthView::frame)
		.function("ready", &EarthView::ready)
		.function("sceneComplete", &EarthView::sceneComplete)
		.function("getPose", &EarthView::getPose)
		.function("setPose", &EarthView::setPose)
		.function("setRoll", &EarthView::setRoll)
		.function("getTubePose", &EarthView::getTubePose)
		.function("setFov", &EarthView::setFov)
		.function("setOrtho", &EarthView::setOrtho)
		.function("getOrtho", &EarthView::getOrtho)
		.function("setAirplane", &EarthView::setAirplane)
		.function("setTerrainFollow", &EarthView::setTerrainFollow)
		.function("orbit", &EarthView::orbit)
		.function("zoomOrtho", &EarthView::zoomOrtho)
		.function("panOrtho", &EarthView::panOrtho)
		.function("pickCenter", &EarthView::pickCenter)
		.function("pickNdc", &EarthView::pickNdc)
		.function("planetRadius", &EarthView::planetRadius)
		.function("setTubeRect", &EarthView::setTubeRect)
		.function("setTubeEnabled", &EarthView::setTubeEnabled)
		.function("lockTubeGround", &EarthView::lockTubeGround)
		.function("setTubeCurl", &EarthView::setTubeCurl)
		.function("getTubeInfo", &EarthView::getTubeInfo)
		.function("setPath", &EarthView::setPath)
		.function("clearPath", &EarthView::clearPath)
		.function("setPathStyle", &EarthView::setPathStyle)
		.function("getPathGroundAlt", &EarthView::getPathGroundAlt)
		.function("setSkyColor", &EarthView::setSkyColor)
		.function("setDebugLod", &EarthView::setDebugLod)
		.function("setMipmaps", &EarthView::setMipmaps)
		.function("getStats", &EarthView::getStats)
		.function("getDrawnNodes", &EarthView::getDrawnNodes)
		.function("fly", &EarthView::fly);
	emscripten::function("createView", &createView, emscripten::allow_raw_pointers());
	emscripten::function("deliverFetch", &deliverFetch);
	emscripten::function("forceJpgTextures", &forceJpgTextures);
	emscripten::function("setAntialias", &setAntialias);
}
