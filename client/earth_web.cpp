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
//   view.fly(yaw, pitch, fwd, back, left, right, up, down, slow, viewFrame, gain, dtMs);
//   view.frame(dtMs);                              // from requestAnimationFrame

#include <fstream>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/bind.h>
#include <GLES2/gl2.h>

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
		attr.antialias = 0;
		attr.majorVersion = 1;
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
	void setSkyColor(int rgb) { sky_color = rgb; }
	void setDebugLod(bool on) { earth.debug_lod = on; }

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
		renderFrameBegin(ctx, nullptr, w, h, sky_color);
		earth.updateAndDraw(ctx, camera, w, h, dt_ms);
		renderFrameEnd(ctx);
	}

	// pose in degrees/meters, the vocabulary of 2d maps
	val getPose() {
		auto p = cameraToPose(camera, planetRadius());
		val o = val::object();
		o.set("lat", p.lat * 180.0 / M_PI);
		o.set("lon", p.lon * 180.0 / M_PI);
		o.set("alt", p.alt);
		o.set("heading", p.heading * 180.0 / M_PI);
		o.set("tilt", p.tilt * 180.0 / M_PI);
		o.set("fov", camera.fov * 180.0 / M_PI);
		return o;
	}

	void setPose(double lat, double lon, double alt, double heading, double tilt) {
		geo_pose_t p;
		p.lat = lat * M_PI / 180.0;
		p.lon = lon * M_PI / 180.0;
		p.alt = alt;
		p.heading = heading * M_PI / 180.0;
		p.tilt = tilt * M_PI / 180.0;
		camera = poseToCamera(p, planetRadius(), camera.fov);
	}

	void setFov(double deg) {
		camera.fov = fmax(1.0, fmin(deg, 100.0)) * M_PI / 180.0;
	}

	// built-in flying controls; the host translates its pointer/keyboard
	// events into this (yaw/pitch in radians for this frame). view_frame
	// switches forward/back and raise/lower from ground-frame cruise/pedestal
	// to view-frame dolly/boom; gain is the host's sticky speed multiplier
	void fly(double yaw, double pitch, bool forward, bool back, bool left, bool right,
			bool raise, bool lower, bool slow, bool view_frame, double gain, double dt_ms) {
		if (!earth.ready()) return;
		camera_input_t in;
		in.yaw = yaw;
		in.pitch = pitch;
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
		applyCameraInput(camera, in, planetRadius());
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

EMSCRIPTEN_BINDINGS(earth) {
	emscripten::class_<EarthView>("EarthView")
		.function("frame", &EarthView::frame)
		.function("ready", &EarthView::ready)
		.function("sceneComplete", &EarthView::sceneComplete)
		.function("getPose", &EarthView::getPose)
		.function("setPose", &EarthView::setPose)
		.function("setFov", &EarthView::setFov)
		.function("setSkyColor", &EarthView::setSkyColor)
		.function("setDebugLod", &EarthView::setDebugLod)
		.function("fly", &EarthView::fly);
	emscripten::function("createView", &createView, emscripten::allow_raw_pointers());
	emscripten::function("deliverFetch", &deliverFetch);
}
