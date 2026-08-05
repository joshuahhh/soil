#include <fstream>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <sys/stat.h>
#ifdef _WIN32
	#include <direct.h>
#endif

#include "SDL.h"
#ifdef EMSCRIPTEN
#   include <GLES2/gl2.h>
#   include "emscripten.h"
#else
#   if defined(_WIN32) || defined(__linux__)
#      include <glad/glad.h>
#      include "gl2/src/glad.c"
#   else
#      define GL_GLEXT_PROTOTYPES
#   endif
#endif

#include <math.h>
#include <Eigen/Dense>
using namespace Eigen;
#include "rocktree_util.h"
#include "earth_camera.h"
#include "earth_core.h"

#include <SDL_opengl.h>

SDL_Window* sdl_window;

// initial camera state, overridable via command line: main [lat lon [altitude_m]]
static camera_t camera = {
	{ 1329866.230289, -4643494.267515, 4154677.131562 }, // nyc
	{ 0.219862, 0.419329, 0.312226 },
	0.25 * M_PI, // scroll wheel zooms by narrowing/widening this
};

static earth_core_t earth;

// sticky movement-speed multiplier, halved/doubled with -/=
static double speed_gain = 1.0;

// toggled with L: tint the parts of a mesh red where a finer tile is wanted
// but not yet downloaded
static bool debug_lod_mode = false;

static bool mouse_captured = true;

// --nograb: never capture the cursor and ignore camera input; for profiling
// runs that shouldn't interfere with whatever else the machine is doing
static bool no_grab = false;

// --bg RRGGBB: override the sky/clear color, e.g. ff00ff to make cracks
// (background leaking through geometry) unmistakable
static int sky_color = 0x83b5fc;

// benchmark rig (see bench.sh): fixed view, log per-frame distance to the
// fully loaded reference frame until the scene converges
static bool bench_mode = false, bench_capture = false;
static const char *bench_ref_path = nullptr, *bench_csv_path = nullptr;


Uint64 NOW = SDL_GetPerformanceCounter();
Uint64 LAST = 0;
double deltaTime = 0;

// gather this frame's input from sdl, fly the camera, and run the core
void updateFrame(render_ctx_t &ctx) {
	if (!earth.ready()) return;

	camera_input_t in;
	auto state = SDL_GetKeyboardState(NULL);
	in.forward = state[SDL_SCANCODE_W];
	in.left = state[SDL_SCANCODE_A];
	in.back = state[SDL_SCANCODE_S];
	in.right = state[SDL_SCANCODE_D];
	in.raise = state[SDL_SCANCODE_E]; // e = elevate, matching editor flycams
	in.lower = state[SDL_SCANCODE_Q];
	in.view_frame = state[SDL_SCANCODE_SPACE]; // hold: dolly/boom instead of cruise/pedestal
	in.slow = state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT];
	in.speed_gain = speed_gain;
	int mouse_x = 0, mouse_y = 0;
	if (mouse_captured) SDL_GetRelativeMouseState(&mouse_x, &mouse_y);
	in.yaw = mouse_x * 0.001;
	in.pitch = -mouse_y * 0.001;
	// arrows pan/tilt at fixed angular rates so the mouse is optional
	auto rot = (in.slow ? 0.1 : 1.0) * (deltaTime / 1000.0);
	if (state[SDL_SCANCODE_RIGHT]) in.yaw += 1.2 * rot;
	if (state[SDL_SCANCODE_LEFT]) in.yaw -= 1.2 * rot;
	if (state[SDL_SCANCODE_UP]) in.pitch += 0.9 * rot;
	if (state[SDL_SCANCODE_DOWN]) in.pitch -= 0.9 * rot;
	// z/x zoom by narrowing/widening the fov, like the scroll wheel
	if (state[SDL_SCANCODE_Z] != state[SDL_SCANCODE_X]) {
		camera.fov *= pow(0.9, (deltaTime / 150.0) * (state[SDL_SCANCODE_Z] ? 1 : -1));
		camera.fov = fmax(1.0 * M_PI / 180.0, fmin(camera.fov, 100.0 * M_PI / 180.0));
	}
	if (no_grab) {
		in.forward = in.left = in.back = in.right = in.raise = in.lower = false;
		in.yaw = in.pitch = 0;
	}
	in.dt_ms = deltaTime;
	applyCameraInput(camera, in, earth.radius());

	// print position every 2 seconds
	{
		static double ms = 0;
		ms += deltaTime;
		if (ms > 2000) {
			ms = 0;
			printf("pos: %f %f %f, dir: %f %f %f\n",
				camera.eye.x(), camera.eye.y(), camera.eye.z(),
				camera.direction.x(), camera.direction.y(), camera.direction.z());
		}
	}

	int width, height;
	renderDrawableSize(ctx, sdl_window, &width, &height);
	renderFrameBegin(ctx, sdl_window, width, height, sky_color);
	earth.debug_lod = debug_lod_mode;
	earth.updateAndDraw(ctx, camera, width, height, deltaTime);
	renderFrameEnd(ctx);
}

bool quit = false;

#ifndef EMSCRIPTEN
// rows are stored bottom-up (opengl order) in memory, flipped on disk so the
// ppm views right side up
static bool writePpm(const char *path, int w, int h, const uint8_t *rgb) {
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = h - 1; y >= 0; y--)
		fwrite(rgb + (size_t)y * w * 3, 1, (size_t)w * 3, f);
	fclose(f);
	return true;
}

static uint8_t *readPpm(const char *path, int *w, int *h) {
	FILE *f = fopen(path, "rb");
	if (!f) return nullptr;
	int maxval;
	if (fscanf(f, "P6 %d %d %d", w, h, &maxval) != 3 || maxval != 255) {
		fclose(f);
		return nullptr;
	}
	fgetc(f); // the single whitespace byte after maxval
	auto buf = (uint8_t *)malloc((size_t)*w * *h * 3);
	auto ok = true;
	for (int y = *h - 1; y >= 0; y--)
		ok &= fread(buf + (size_t)y * *w * 3, 1, (size_t)*w * 3, f) == (size_t)*w * 3;
	fclose(f);
	if (!ok) {
		free(buf);
		return nullptr;
	}
	return buf;
}

// called once per frame after drawPlanet, before the buffer swap. capture
// mode: wait for the scene to converge, save the frame as reference, quit.
// measure mode: log rmse against the reference every frame until converged
void benchFrame(render_ctx_t &ctx) {
	static uint32_t start_ms = SDL_GetTicks();
	static uint32_t first_complete_ms = 0;
	static int complete_streak = 0;
	static uint8_t *ref = nullptr;
	static int ref_w = 0, ref_h = 0;
	static FILE *csv = nullptr;
	static std::vector<uint8_t> pixels;

	auto elapsed = SDL_GetTicks() - start_ms;
	int w, h;
	renderDrawableSize(ctx, sdl_window, &w, &h);
	pixels.resize((size_t)w * h * 3);
	if (!renderReadPixels(ctx, w, h, pixels.data())) return;

	if (!bench_capture) {
		if (!ref) {
			ref = readPpm(bench_ref_path, &ref_w, &ref_h);
			if (!ref) {
				fprintf(stderr, "bench: could not read reference %s\n", bench_ref_path);
				exit(1);
			}
			csv = fopen(bench_csv_path, "w");
			if (!csv) {
				fprintf(stderr, "bench: could not open %s for writing\n", bench_csv_path);
				exit(1);
			}
			fprintf(csv, "ms,rmse\n");
		}
		if (ref_w != w || ref_h != h) {
			fprintf(stderr, "bench: window %dx%d doesn't match reference %dx%d "
				"(don't resize the window between capture and bench runs)\n", w, h, ref_w, ref_h);
			exit(1);
		}
		double sum = 0;
		auto n = (size_t)w * h * 3;
		for (size_t i = 0; i < n; i++) {
			auto d = (double)pixels[i] - (double)ref[i];
			sum += d * d;
		}
		fprintf(csv, "%u,%f\n", elapsed, sqrt(sum / n));
	}

	if (earth.scene_complete) {
		if (complete_streak++ == 0) first_complete_ms = elapsed;
	} else {
		complete_streak = 0;
	}

	// a few extra complete frames so lazily buffered meshes are all drawn and
	// a spurious single complete frame doesn't end the run early
	const auto settle_frames = 30;
	const uint32_t timeout_ms = 180 * 1000;
	if (complete_streak < settle_frames && elapsed < timeout_ms) return;

	if (elapsed >= timeout_ms)
		fprintf(stderr, "bench: timed out before the scene completed\n");
	if (bench_capture) {
		if (writePpm(bench_ref_path, w, h, pixels.data()))
			printf("bench: reference %s (%dx%d), scene complete at %u ms\n",
				bench_ref_path, w, h, first_complete_ms);
		else
			fprintf(stderr, "bench: could not write %s\n", bench_ref_path);
	} else {
		fclose(csv);
		printf("bench: %s, scene complete at %u ms\n", bench_csv_path, first_complete_ms);
	}
	quit = true;
}
#endif

void mainloop(render_ctx_t &ctx) {
	SDL_Event sdl_event;
	while (SDL_PollEvent(&sdl_event)) {
		switch (sdl_event.type) {
			case SDL_QUIT:
				quit = true;
				break;
			case SDL_MOUSEBUTTONDOWN:
				if (!mouse_captured && !no_grab) {
					mouse_captured = true;
					SDL_SetRelativeMouseMode(SDL_TRUE);
					// discard motion accumulated while released so the camera doesn't jump
					SDL_GetRelativeMouseState(NULL, NULL);
				}
				break;
			case SDL_KEYDOWN:
				if (sdl_event.key.keysym.sym == SDLK_ESCAPE) {
					if (mouse_captured) {
						mouse_captured = false;
						SDL_SetRelativeMouseMode(SDL_FALSE);
					} else {
						quit = true;
					}
				}
				if (sdl_event.key.keysym.sym == SDLK_v && !sdl_event.key.repeat) {
					char filename[64];
					sprintf(filename, "view_%ld.json", (long)time(NULL));
					FILE* f = fopen(filename, "w");
					if (f) {
						fprintf(f, "{\n\t\"eye\": [%f, %f, %f],\n\t\"direction\": [%f, %f, %f],\n\t\"fov\": %f\n}\n",
							camera.eye.x(), camera.eye.y(), camera.eye.z(),
							camera.direction.x(), camera.direction.y(), camera.direction.z(), camera.fov);
						fclose(f);
						printf("saved view to %s\n", filename);
					} else {
						fprintf(stderr, "could not save view to %s\n", filename);
					}
				}
				if (sdl_event.key.keysym.sym == SDLK_MINUS && !sdl_event.key.repeat) {
					speed_gain = fmax(1.0 / 16.0, speed_gain / 2.0);
					printf("speed gain: x%g\n", speed_gain);
				}
				if (sdl_event.key.keysym.sym == SDLK_EQUALS && !sdl_event.key.repeat) {
					speed_gain = fmin(16.0, speed_gain * 2.0);
					printf("speed gain: x%g\n", speed_gain);
				}
				if (sdl_event.key.keysym.sym == SDLK_l && !sdl_event.key.repeat) {
					debug_lod_mode = !debug_lod_mode;
					printf("lod debug mode: %s\n", debug_lod_mode ? "on" : "off");
				}
				break;
			case SDL_MOUSEWHEEL:
				camera.fov *= pow(0.9, sdl_event.wheel.y);
				camera.fov = fmax(1.0 * M_PI / 180.0, fmin(camera.fov, 100.0 * M_PI / 180.0));
				break;
		}
	}

	LAST = NOW;
	NOW = SDL_GetPerformanceCounter();
	deltaTime = (double)((NOW - LAST)*1000 / (double)SDL_GetPerformanceFrequency());

	// fps meter in the window title, averaged over half-second windows
	{
		static double window_ms = 0;
		static int window_frames = 0;
		window_ms += deltaTime;
		window_frames++;
		if (window_ms >= 500) {
			char title[64];
			snprintf(title, sizeof(title), "Earth Client — %.0f fps (%.1f ms)",
				window_frames * 1000.0 / window_ms, window_ms / window_frames);
			SDL_SetWindowTitle(sdl_window, title);
			window_ms = 0;
			window_frames = 0;
		}
	}

	updateFrame(ctx);
#ifndef EMSCRIPTEN
	if (bench_mode) benchFrame(ctx);
#endif
	renderPresent(ctx, sdl_window);
}

int main(int argc, char* argv[]) {

#ifndef EMSCRIPTEN
	// strip --nograb and --bg RRGGBB wherever they appear among the args
	// (before --bench parsing, which consumes argv positionally)
	for (auto i = 1; i < argc; i++) {
		auto eat = 0;
		if (strcmp(argv[i], "--nograb") == 0) {
			no_grab = true;
			eat = 1;
		} else if (strcmp(argv[i], "--bg") == 0 && i + 1 < argc) {
			sky_color = (int)strtol(argv[i + 1], NULL, 16);
			eat = 2;
		}
		if (!eat) continue;
		for (auto j = i + eat; j < argc; j++) argv[j - eat] = argv[j];
		argc -= eat;
		i--;
	}

	if (argc >= 2 && strncmp(argv[1], "--bench", 7) == 0) {
		bench_mode = true;
		bench_capture = strcmp(argv[1], "--bench-capture") == 0;
		if (argc != (bench_capture ? 4 : 5)) {
			fprintf(stderr, "usage: %s --bench-capture view.json ref.ppm\n"
			                "       %s --bench view.json ref.ppm out.csv\n", argv[0], argv[0]);
			exit(1);
		}
		bench_ref_path = argv[3];
		if (!bench_capture) bench_csv_path = argv[4];
		// let the view file fall through to the normal view-loading path
		argv[1] = argv[2];
		argc = 2;
	}
#endif

	if (argc == 2) {
		// restore a view saved with the S key
		FILE* f = fopen(argv[1], "r");
		if (!f) {
			fprintf(stderr, "could not open view file %s\n", argv[1]);
			exit(1);
		}
		char buf[512];
		auto len = fread(buf, 1, sizeof(buf) - 1, f);
		buf[len] = 0;
		fclose(f);
		double e0, e1, e2, d0, d1, d2, fov;
		if (sscanf(buf, " { \"eye\" : [ %lf , %lf , %lf ] , \"direction\" : [ %lf , %lf , %lf ] , \"fov\" : %lf",
				&e0, &e1, &e2, &d0, &d1, &d2, &fov) != 7) {
			fprintf(stderr, "could not parse view file %s\n", argv[1]);
			exit(1);
		}
		camera.eye = { e0, e1, e2 };
		camera.direction = { d0, d1, d2 };
		camera.fov = fov;
	} else if (argc == 3 || argc == 4) {
		const double earth_radius = 6371010; // same as planetoid radius
		geo_pose_t pose;
		pose.lat = atof(argv[1]) * M_PI / 180.0;
		pose.lon = atof(argv[2]) * M_PI / 180.0;
		pose.alt = argc == 4 ? atof(argv[3]) : 10000.0;
		pose.heading = 0; // look north...
		pose.tilt = -30 * M_PI / 180.0; // ...tilted down towards the ground
		camera = poseToCamera(pose, earth_radius, camera.fov);
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s [lat lon [altitude_m] | view.json]\n", argv[0]);
		exit(1);
	}

	int video_width = 1024;
	int video_height = 768;

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "Couldn't init SDL2: %s\n", SDL_GetError());
		exit(1);
	}
#ifdef EARTH_METAL
	sdl_window = SDL_CreateWindow("Earth Client",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		video_width, video_height,
		SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!sdl_window) {
		fprintf(stderr, "Couldn't create window: %s\n", SDL_GetError());
		exit(1);
	}
#else
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#ifdef EMSCRIPTEN
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
		SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
		SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
	sdl_window = SDL_CreateWindow("Earth Client",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		video_width, video_height,
		SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	if (!sdl_window) {
		fprintf(stderr, "Couldn't create window: %s\n", SDL_GetError());
		exit(1);
	}
	SDL_GLContext gl_context = SDL_GL_CreateContext(sdl_window);
	if (!gl_context) {
		fprintf(stderr, "Couldn't create OpenGL context: %s\n", SDL_GetError());
		exit(1);
	}
	SDL_GL_SetSwapInterval(1);

#if defined(_WIN32) || defined(__linux__)
	// init glad
	if (!gladLoadGL()) {
		fprintf(stderr, "Failed to init glad\n");
		exit(1);
	}
#endif
#endif

	auto ctx = new render_ctx_t();

	renderInit(*ctx, sdl_window);
	if (getenv("EARTH_FORCE_JPG")) texture_s3tc_supported = false; // debug: exercise the jpg texture path natively
	earth.log_sched = bench_mode;
	earth.load();

	// benchmarks and --nograb runs hold a fixed view; leave the cursor alone
	if (bench_mode || no_grab) mouse_captured = false;
	else SDL_SetRelativeMouseMode(SDL_TRUE);

#ifdef EMSCRIPTEN
	emscripten_set_main_loop_arg([](void* _ctx){
		auto ctx = (render_ctx_t *)_ctx;
		mainloop(*ctx);
	}, (void *)ctx, 0, 1);
#else
	while (!quit) mainloop(*ctx);
#endif

#ifndef EARTH_METAL
	SDL_GL_DeleteContext(gl_context);
#endif
	SDL_DestroyWindow(sdl_window);
	SDL_Quit();
	delete ctx;
	
	return 0;
}