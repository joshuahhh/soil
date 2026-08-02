#include <fstream>
#include <time.h>
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

#include <SDL_opengl.h>

SDL_Window* sdl_window;

// initial camera state, overridable via command line: main [lat lon [altitude_m]]
static Vector3d initial_eye = { 1329866.230289, -4643494.267515, 4154677.131562 }; // nyc
static Vector3d initial_direction = { 0.219862, 0.419329, 0.312226 };

// scroll wheel zooms by narrowing/widening the field of view
static double camera_fov = 0.25 * M_PI;

// set when V is pressed; the view is saved from drawPlanet, which owns the camera state
static bool save_view_requested = false;

// toggled with L: tint the parts of a mesh red where a finer tile is wanted
// but not yet downloaded
static bool debug_lod_mode = false;

static rocktree_t *_planetoid = NULL;

void loadPlanet() {
	auto planetoid = new rocktree_t();
	planetoid->downloaded = false;
	_planetoid = planetoid;

	getPlanetoid([=](std::unique_ptr<PlanetoidMetadata> _metadata) {		
		if (!_metadata) fprintf(stderr, "%s", "no planetoid\n"), abort();
		populatePlanetoid(planetoid, std::move(_metadata));
		
		auto bulk = planetoid->root_bulk;
		assert(bulk->dl_state == dl_state_stub);		
		bulk->setStartedDownloading();
		getBulk(bulk->request, bulk, [=](auto _) { /* todo rm cb */ });
	});
}

void initGL(gl_ctx_t &ctx) {
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	ctx.program = makeShader(
		"uniform mat4 transform;"
		"uniform vec2 uv_offset;"
		"uniform vec2 uv_scale;"
		"uniform bool octant_mask[8];"
		"uniform bool stale_mask[8];"
		"attribute vec3 position;"
		"attribute float octant;"
		"attribute vec2 texcoords;"
		"varying vec2 v_texcoords;"
		"varying float v_stale;"
		"void main() {"
		"	float mask = octant_mask[int(octant)] ? 0.0 : 1.0;"
		"	v_stale = stale_mask[int(octant)] ? 1.0 : 0.0;"
		"	v_texcoords = (texcoords + uv_offset) * uv_scale * mask;"
		"	gl_Position = transform * vec4(position, 1.0) * mask;"
		"}",

		"#ifdef GL_ES\n"
		"precision mediump float;\n"
		"#endif\n"
		"uniform sampler2D texture;"
		"uniform bool debug_lod;"
		"varying vec2 v_texcoords;"
		"varying float v_stale;"
		"void main() {"
		"	vec3 c = texture2D(texture, v_texcoords).rgb;"
		"	if (debug_lod) c = mix(c, vec3(1.0, 0.0, 0.0), v_stale * 0.5);"
		"	gl_FragColor = vec4(c, 1.0);"
		"}"
	);
	glUseProgram(ctx.program);
	ctx.transform_loc = glGetUniformLocation(ctx.program, "transform");
	ctx.uv_offset_loc = glGetUniformLocation(ctx.program, "uv_offset");
	ctx.uv_scale_loc = glGetUniformLocation(ctx.program, "uv_scale");
	ctx.octant_mask_loc = glGetUniformLocation(ctx.program, "octant_mask");
	ctx.stale_mask_loc = glGetUniformLocation(ctx.program, "stale_mask");
	ctx.debug_lod_loc = glGetUniformLocation(ctx.program, "debug_lod");
	ctx.texture_loc = glGetUniformLocation(ctx.program, "texture");
	ctx.position_loc = glGetAttribLocation(ctx.program, "position");
	ctx.octant_loc = glGetAttribLocation(ctx.program, "octant");
	ctx.texcoords_loc = glGetAttribLocation(ctx.program, "texcoords");

	glEnableVertexAttribArray(ctx.position_loc);
	glEnableVertexAttribArray(ctx.octant_loc);
	glEnableVertexAttribArray(ctx.texcoords_loc);
}

Uint64 NOW = SDL_GetPerformanceCounter();
Uint64 LAST = 0;
double deltaTime = 0;

void drawPlanet(gl_ctx_t &ctx) {
	auto planetoid = _planetoid;
	if (!planetoid) return;
	if (!planetoid->downloaded) return;
	if (planetoid->root_bulk->dl_state != dl_state_downloaded) return;	
	auto current_bulk = planetoid->root_bulk;
	auto planet_radius = planetoid->radius;

	Matrix4d projection, viewprojection;

	int width, height;
	SDL_GL_GetDrawableSize(sdl_window, &width, &height);
	glViewport(0, 0, width, height);
	auto sky = 0x83b5fc;
	glClearColor((sky>>16 & 0xff) / 255.0f, (sky>>8 & 0xff) / 255.0f, (sky & 0xff) / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);	

	auto mouse_state = SDL_GetMouseState(NULL, NULL);
	auto state = SDL_GetKeyboardState(NULL);
	auto key_up_pressed = state[SDL_SCANCODE_W];
	auto key_left_pressed = state[SDL_SCANCODE_A];
	auto key_down_pressed = state[SDL_SCANCODE_S];
	auto key_right_pressed = state[SDL_SCANCODE_D];
	auto key_raise_pressed = state[SDL_SCANCODE_Q];
	auto key_lower_pressed = state[SDL_SCANCODE_E];
	auto key_slow_pressed = state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT];
	auto mouse_pressed = mouse_state & SDL_BUTTON(SDL_BUTTON_LEFT);

	// from lat/lon
	//static Vector3d ecef = { ...https://www.oc.nps.edu/oc2902w/coord/llhxyz.htm };
	//static auto ecef_norm = ecef.normalized();
	//static Vector3d eye = (ecef_norm * (planet_radius + 10000));

	static Vector3d eye = initial_eye;
	static Vector3d direction = initial_direction;

	// print position every 2 seconds
	{
		static double ms = 0;
		ms += deltaTime;
		if (ms > 2000) {
			ms = 0;
			printf("pos: %f %f %f, dir: %f %f %f\n", eye.x(), eye.y(), eye.z(), direction.x(), direction.y(), direction.z());
		}
	}

	// up is the vec from the planetoid's center towards the sky
	auto up = eye.normalized();

	// projection
	float aspect_ratio = (float)width / (float)height;
	float fov = (float)camera_fov;
	auto altitude = eye.norm() - planet_radius;
	auto horizon = sqrt( altitude * (2*planet_radius + altitude) );
	// terrain of height h is visible up to horizon(h) beyond the viewer's own
	// geometric horizon (e.g. mount rainier from seattle), so extend the far
	// plane by the horizon distance of the tallest terrain on earth
	const double highest_peak = 8849; // everest
	auto peak_horizon = sqrt( highest_peak * (2*planet_radius + highest_peak) );
	auto near = horizon > 370000 ? altitude / 2 : 1.0;
	auto far = horizon + peak_horizon;
	if (near >= far) near = far - 1;
	if (isnan(far) || far < near) far = near + 1;
	projection = perspective(fov, aspect_ratio, near, far);
	
	// rotation
	int mouse_x, mouse_y;
	SDL_GetRelativeMouseState(&mouse_x, &mouse_y);
	double yaw = mouse_x * 0.001;
	double pitch = -mouse_y * 0.001;
	auto overhead = direction.dot(-up);
	if ((overhead > 0.99 && pitch < 0) || (overhead < -0.99 && pitch > 0))
		pitch = 0;
	auto pitch_axis = direction.cross(up);
	auto yaw_axis = direction.cross(pitch_axis);
	pitch_axis.normalize();
	AngleAxisd roll_angle(0, Vector3d::UnitZ());
	AngleAxisd yaw_angle(yaw, yaw_axis);
	AngleAxisd pitch_angle(pitch, pitch_axis);
	auto quat = roll_angle * yaw_angle * pitch_angle;
	auto rotation = quat.matrix();
	direction = (rotation * direction).normalized();

	// movement
	auto speed_amp = fmin(2600, powf(fmax(0, (altitude - 500)/10000)+1, 1.337)) / 6;
	auto mag = 100*(deltaTime/17.0)*(key_slow_pressed ? 0.1 : 1.0) * speed_amp;
	auto sideways = direction.cross(up).normalized();
	auto horizontal = up.cross(sideways).normalized(); // view direction projected onto the horizontal plane
	auto forwards = horizontal * mag;
	auto backwards = -horizontal * mag;
	auto left = -sideways * mag;
	auto right = sideways * mag;
	Vector3d new_eye =  eye + key_up_pressed * forwards
	                    + key_down_pressed * backwards
	                    + key_left_pressed * left
	                    + key_right_pressed * right;
	new_eye = new_eye.normalized() * eye.norm(); // WASD keeps elevation constant
	auto vertical = up * mag;
	new_eye += key_raise_pressed * vertical - key_lower_pressed * vertical;
	auto pot_altitude = new_eye.norm() - planet_radius;
	if (pot_altitude < 1000 * 1000 * 10) {
		eye = new_eye;		
	}

	if (save_view_requested) {
		save_view_requested = false;
		char filename[64];
		sprintf(filename, "view_%ld.json", (long)time(NULL));
		FILE* f = fopen(filename, "w");
		if (f) {
			fprintf(f, "{\n\t\"eye\": [%f, %f, %f],\n\t\"direction\": [%f, %f, %f],\n\t\"fov\": %f\n}\n",
				eye.x(), eye.y(), eye.z(), direction.x(), direction.y(), direction.z(), camera_fov);
			fclose(f);
			printf("saved view to %s\n", filename);
		} else {
			fprintf(stderr, "could not save view to %s\n", filename);
		}
	}

	auto view = lookAt(eye, eye + direction, up);
	viewprojection = projection * view;

	auto frustum_planes = getFrustumPlanes(viewprojection); // for obb culling

	const std::string octs[] = { "0", "1", "2", "3", "4", "5", "6", "7" };
	std::vector<std::pair<std::string, rocktree_t::bulk_t *>> valid = { std::make_pair("", current_bulk) };
	decltype(valid) next_valid;
	std::map<std::string, rocktree_t::node_t *> potential_nodes;

	// todo: abort emscripten_fetch_close() https://emscripten.org/docs/api_reference/fetch.html
	//       and/or emscripten coroutine fetch semaphore
	// todo: workers instead of shared mem https://emscripten.org/docs/api_reference/emscripten.h.html#worker-api

	// downloaded nodes are kept as an lru cache (see eviction below); stale
	// bulk metadata is kept for a fixed grace period
	auto now_ms = (double)SDL_GetTicks();
	const double keep_ms = 20 * 1000;

	std::map<std::string, rocktree_t::bulk_t *> potential_bulks;

	// node culling and level of detail using breadth-first search
	for (;;) {
		for(auto cur2 : valid) {
			auto cur = cur2.first;
			auto bulk = cur2.second;

			if (cur.size() > 0 && cur.size() % 4 == 0) {
				auto rel = cur.substr (floor((cur.size() - 1) / 4) * 4, 4);
				auto bulk_kv = bulk->bulks.find(rel);
				auto has_bulk = bulk_kv != bulk->bulks.end();
				if (!has_bulk) continue;
				auto b = bulk_kv->second.get();
				potential_bulks[cur] = b;
				if (b->dl_state == dl_state_stub) {
					b->setStartedDownloading();
					getBulk(b->request, b, [=](auto) {});			
				}
				if (b->dl_state != dl_state_downloaded) continue;
				bulk = b;
			}
			potential_bulks[cur] = bulk;
						
			for(auto o : octs) {
				auto nxt = cur + o;				
				auto nxt_rel = nxt.substr (floor((nxt.size() - 1) / 4) * 4, 4);
				auto node_kv = bulk->nodes.find(nxt_rel);
				if (node_kv == bulk->nodes.end()) // node at "nxt" doesn't exist
					continue;				
				auto node = node_kv->second.get();						

				// cull outside frustum using obb
				// todo: check if it could cull more
				if (obb_frustum_outside == classifyObbFrustum(&node->obb, frustum_planes)) {
					continue;
				}

				// level of detail
				/*{
					auto obb_center = node->obb.center;
					auto obb_max_diameter = fmax(fmax(node->obb.extents[0], node->obb.extents[1]), node->obb.extents[2]);			
					
					auto t = Affine3d().Identity();
					t.translate(Vector3d(obb_center.x(), obb_center.y(), obb_center.z()));
					t.scale(obb_max_diameter);
					Matrix4d viewprojection_d;
					for(auto i = 0; i < 16; i++) viewprojection_d.data()[i] = viewprojection.data()[i];
					auto m = viewprojection_d * t;
					auto s = m(3, 3);
					if (s < 0) s = -s; // ?
					auto diameter_in_clipspace = 2 * (obb_max_diameter / s);  // *2 because clip space is -1 to +1
					auto amplify = 4; // todo: meters per texel
					if (diameter_in_clipspace < 0.5 / amplify) {
						continue;
					}
				}*/

				{
					auto t = Affine3d().Identity();					
					t.translate(eye + (eye-node->obb.center).norm() * direction);
					auto m = viewprojection * t;
					auto s = m(3, 3);
					auto texels_per_meter = 1.0f / node->meters_per_texel;
					auto wh = 768; // width < height ? width : height;
					// zooming (narrowing the fov) magnifies the scene, so it
					// needs proportionally finer tiles. 1 at the default fov
					auto zoom = tan(0.125 * M_PI) / tan(camera_fov / 2.0);
					auto r = (2.0*(1.0/s)) * wh * zoom;
					if (texels_per_meter > r) continue;
				}

				next_valid.push_back(std::make_pair(nxt, bulk));

				if (node->can_have_data) {
					potential_nodes[nxt] = node;
				}
			}
		}
		if (next_valid.size() == 0) break;
		valid = next_valid;
		next_valid.clear();		
	}	

	// download nodes in order of importance: coarse levels before fine ones
	// (a coarse node covers a much larger area), then near before far within
	// a level. only a limited number of requests may be in flight at once, so
	// the order can adapt while the camera moves instead of everything being
	// queued in path order the first frame it becomes visible
	{
		static std::atomic<int> nodes_in_flight(0);
		const auto max_nodes_in_flight = 12;

		struct candidate_t { size_t level; double dist; rocktree_t::node_t *node; };
		std::vector<candidate_t> to_download;
		for (auto &kv : potential_nodes) {
			auto node = kv.second;
			node->last_wanted_ms = now_ms;
			if (node->dl_state != dl_state_stub) continue;
			to_download.push_back({ kv.first.size(), (node->obb.center - eye).norm(), node });
		}
		std::sort(to_download.begin(), to_download.end(), [](const candidate_t &a, const candidate_t &b) {
			return a.level != b.level ? a.level < b.level : a.dist < b.dist;
		});
		for (auto &c : to_download) {
			if (nodes_in_flight >= max_nodes_in_flight) break;
			auto node = c.node;
			nodes_in_flight++;
			node->setStartedDownloading();
			getNode(node->request, node, [node](auto) { nodes_in_flight--; });
		}
	}

	// downloaded nodes form an lru cache with a byte quota: nothing is evicted
	// until resident mesh/texture data exceeds the quota, then the least
	// recently wanted nodes are dropped first. nodes in the current potential
	// set are never evicted, even over quota
	// ~100-150KB per resident node; native gets a generous quota (disk cache
	// backstops evictions), wasm stays well under its 1GB total heap
#ifdef EMSCRIPTEN
	const size_t mem_quota = (size_t)256 << 20;
#else
	const size_t mem_quota = (size_t)1024 << 20;
#endif
	size_t resident_bytes = 0;
	struct evict_t { double last_wanted_ms; size_t bytes; rocktree_t::node_t *n; };
	std::vector<evict_t> evictable;
	auto node_bytes = [](rocktree_t::node_t *n) {
		auto bytes = sizeof(*n);
		for (auto &m : n->meshes)
			bytes += m.vertices.size() + m.indices.size() * sizeof(uint16_t) + m.texture.size();
		return bytes;
	};

	std::vector<rocktree_t::bulk_t*> x = {current_bulk};
	auto buf_cnt = 0, obs_n_cnt = 0, total_n = 0;
	while(!x.empty()) {
		auto cur_bulk = x[0]; x.erase(x.begin());
		// prepare next iteration
		for (auto &kv : cur_bulk->bulks) {
			auto b = kv.second.get();
			if (b->dl_state != dl_state_downloaded) continue;
			x.emplace(x.begin(), b);
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
			auto p = n->request.node_key().path();
			auto has = potential_nodes.find(p) != potential_nodes.end();
			if (!has) evictable.push_back({ n->last_wanted_ms, bytes, n });
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
			n->_data = nullptr;
			n->matrix_globe_from_mesh = Matrix4d::Zero();
			n->meshes.clear();
			n->setDeleted();

			resident_bytes -= e.bytes;
		}
	}

	// post order dfs purge obsolete bulks
	auto total_b = 0, obs_b_cnt = 0;	
	std::function<void(rocktree_t::bulk_t *)> po;
	po = [&po, &potential_bulks, &obs_b_cnt, &total_b, now_ms, keep_ms](rocktree_t::bulk_t * b){
		for (auto &kv : b->bulks){
			auto b = kv.second.get();
			if (b->dl_state == dl_state_downloaded)
				po(b);
		}
		total_b++;
		auto p = b->request.node_key().path();
		auto has = potential_bulks.find(p) != potential_bulks.end();
		if (has) {
			b->last_wanted_ms = now_ms;
		} else if (now_ms - b->last_wanted_ms > keep_ms) {
			if (b->busy_ctr == 0) {
				b->nodes.clear();
				b->bulks.clear();
				b->setDeleted();
			}			
		}
	};

	po(current_bulk);

	// log stuff about buffers
	{
		static double ms = 0;
		ms += deltaTime;
		if (ms > 2000) {
			ms = 0;
			printf("buffered: %d, tot_n: %d, tot_b: %d, pot_n: %lu, pot_b: %lu, obs n: %d, obs b: %d, mem: %zu MB\n",
				buf_cnt, total_n, total_b, potential_nodes.size(), potential_bulks.size(), obs_n_cnt, obs_b_cnt,
				resident_bytes >> 20
			);
		}
	}

	// 8-bit octant mask flags of nodes
	std::map<std::string, uint8_t> mask_map;

	glUniform1i(ctx.debug_lod_loc, debug_lod_mode);

	for (auto kv = potential_nodes.rbegin(); kv != potential_nodes.rend(); ++kv) { // reverse order
		auto full_path = kv->first;
		auto node = kv->second;
		auto level = strlen(full_path.c_str());
		assert(level > 0);		
		assert(node->can_have_data);
		if (node->dl_state != dl_state_downloaded) continue;

		// set octant mask of previous node
		auto octant = (int)(full_path[level - 1] - '0');
		auto prev = full_path.substr (0, level - 1);
		mask_map[prev] |= 1 << octant;

		// skip if node is masked completely
		if (mask_map[full_path] == 0xff) continue;

		// octants where a finer tile is wanted but not drawn (still
		// downloading). children are drawn before parents here, so mask_map
		// for this node is already complete
		uint8_t stale_mask = 0;
		if (debug_lod_mode) {
			for (auto o = 0; o < 8; o++) {
				if (mask_map[full_path] & (1 << o)) continue;
				if (potential_nodes.find(full_path + octs[o]) != potential_nodes.end())
					stale_mask |= 1 << o;
			}
		}

		// float transform matrix
		Matrix4d transform = viewprojection * node->matrix_globe_from_mesh;
		Matrix4f transform_float;
		for(auto i = 0; i < 16; ++i) transform_float.data()[i] = (float)(transform.data()[i]);

		// buffer, bind, draw
		glUniformMatrix4fv(ctx.transform_loc, 1, GL_FALSE, transform_float.data());
		for (auto &mesh : node->meshes) {
			if (!mesh.buffered) bufferMesh(mesh);
			bindAndDrawMesh(mesh, mask_map[full_path], stale_mask, ctx);
		}
		//bufs[full_path] = node;
	}
}

bool quit = false;
void mainloop(gl_ctx_t &ctx) {
	SDL_Event sdl_event;
	while (SDL_PollEvent(&sdl_event)) {
		switch (sdl_event.type) {
			case SDL_QUIT:
				quit = true;
				break;
			case SDL_KEYDOWN:
				if (sdl_event.key.keysym.sym == SDLK_ESCAPE) quit = true;
				if (sdl_event.key.keysym.sym == SDLK_v && !sdl_event.key.repeat) save_view_requested = true;
				if (sdl_event.key.keysym.sym == SDLK_l && !sdl_event.key.repeat) {
					debug_lod_mode = !debug_lod_mode;
					printf("lod debug mode: %s\n", debug_lod_mode ? "on" : "off");
				}
				break;
			case SDL_MOUSEWHEEL:
				camera_fov *= pow(0.9, sdl_event.wheel.y);
				camera_fov = fmax(1.0 * M_PI / 180.0, fmin(camera_fov, 100.0 * M_PI / 180.0));
				break;
		}
	}

	LAST = NOW;
	NOW = SDL_GetPerformanceCounter();
	deltaTime = (double)((NOW - LAST)*1000 / (double)SDL_GetPerformanceFrequency());

	drawPlanet(ctx);
	SDL_GL_SwapWindow(sdl_window);
}

int main(int argc, char* argv[]) {

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
		initial_eye = { e0, e1, e2 };
		initial_direction = { d0, d1, d2 };
		camera_fov = fov;
	} else if (argc == 3 || argc == 4) {
		auto lat = atof(argv[1]) * M_PI / 180.0;
		auto lon = atof(argv[2]) * M_PI / 180.0;
		auto alt = argc == 4 ? atof(argv[3]) : 10000.0;
		const double earth_radius = 6371010; // same as planetoid radius
		Vector3d up = { cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat) };
		Vector3d north = { -sin(lat) * cos(lon), -sin(lat) * sin(lon), cos(lat) };
		auto tilt = 30 * M_PI / 180.0; // look north, tilted down towards the ground
		initial_eye = up * (earth_radius + alt);
		initial_direction = north * cos(tilt) - up * sin(tilt);
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

	auto ctx = new gl_ctx_t();

	initGL(*ctx);
	loadPlanet();

	SDL_SetRelativeMouseMode(SDL_TRUE);

#ifdef EMSCRIPTEN
	emscripten_set_main_loop_arg([](void* _ctx){	
		auto ctx = (gl_ctx_t *)_ctx;
		mainloop(*ctx);
	}, (void *)ctx, 0, 1);
#else
	while (!quit) mainloop(*ctx);
#endif

	SDL_GL_DeleteContext(gl_context);
	SDL_DestroyWindow(sdl_window);
	SDL_Quit();
	delete ctx;
	
	return 0;
}