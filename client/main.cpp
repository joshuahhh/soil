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
// true when the last frame drew everything the current view wants: no bulk
// metadata or node data still missing/in flight for the potential set
static bool scene_complete = false;

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
	// crack-fill boundary lines (see bindAndDrawMesh) can only land on
	// background pixels, so width just sets how wide a slit they can plug
	// (wider also costs more fill rate). webgl typically clamps this to 1
	auto lw = getenv("EARTH_LINE_WIDTH");
	glLineWidth(lw ? (float)atof(lw) : 2.0f);
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
		"varying float v_mask;"
		"void main() {"
		// masking: a triangle is dropped only when ALL its vertices are in
		// masked octants (v_mask interpolates to 0 -> fragment discard).
		// triangles straddling an octant boundary used to be collapsed
		// entirely, retreating the surface a triangle-row from the boundary
		// and opening hairline cracks the finer tile never covers; instead
		// they are drawn, with masked vertices pushed slightly away in depth
		// so the finer tile wins wherever they overlap it
		"	float mask = octant_mask[int(octant)] ? 0.0 : 1.0;"
		"	v_mask = mask;"
		"	v_stale = stale_mask[int(octant)] ? 1.0 : 0.0;"
		"	v_texcoords = (texcoords + uv_offset) * uv_scale;"
		"	vec4 p = transform * vec4(position, 1.0);"
		"	p.z += (1.0 - mask) * 0.002 * p.w;"
		"	gl_Position = p;"
		"}",

		"#ifdef GL_ES\n"
		"precision mediump float;\n"
		"#endif\n"
		"uniform sampler2D texture;"
		"uniform bool debug_lod;"
		"varying vec2 v_texcoords;"
		"varying float v_stale;"
		"varying float v_mask;"
		"void main() {"
		"	if (v_mask < 0.004) discard;"
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
	auto sky = sky_color;
	glClearColor((sky>>16 & 0xff) / 255.0f, (sky>>8 & 0xff) / 255.0f, (sky & 0xff) / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);	

	auto state = SDL_GetKeyboardState(NULL);
	auto key_up_pressed = state[SDL_SCANCODE_W];
	auto key_left_pressed = state[SDL_SCANCODE_A];
	auto key_down_pressed = state[SDL_SCANCODE_S];
	auto key_right_pressed = state[SDL_SCANCODE_D];
	auto key_raise_pressed = state[SDL_SCANCODE_Q];
	auto key_lower_pressed = state[SDL_SCANCODE_E];
	auto key_slow_pressed = state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT];
	if (no_grab)
		key_up_pressed = key_left_pressed = key_down_pressed = key_right_pressed
			= key_raise_pressed = key_lower_pressed = 0;

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
	int mouse_x = 0, mouse_y = 0;
	if (mouse_captured) SDL_GetRelativeMouseState(&mouse_x, &mouse_y);
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

	// movement: speed proportional to altitude, so apparent (screen-space)
	// motion is constant and approaching the ground eases in exponentially.
	// altitude is a proxy for distance to the terrain being looked at; good
	// enough until we track real terrain height under the camera
	const auto altitude_per_second = 1.0;
	const auto min_speed = 5.0; // m/s floor so we don't freeze at ground level
	auto speed = fmax(min_speed, altitude * altitude_per_second);
	auto mag = speed * (deltaTime/1000.0) * (key_slow_pressed ? 0.1 : 1.0);
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

	// per-section frame timing and bfs work counters, reported every 2s
	static double sec_bfs = 0, sec_dl = 0, sec_evict = 0, sec_draw = 0;
	static long cnt_oct = 0, cnt_cull = 0, cnt_lod = 0;
	static int sec_frames = 0;
	auto ticks_ms = [] {
		return (double)SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency();
	};
	auto t0 = ticks_ms();

	auto view = lookAt(eye, eye + direction, up);
	viewprojection = projection * view;

	auto frustum_planes = getFrustumPlanes(viewprojection); // for obb culling

	const std::string octs[] = { "0", "1", "2", "3", "4", "5", "6", "7" };
	std::vector<std::pair<std::string, rocktree_t::bulk_t *>> valid = { std::make_pair("", current_bulk) };
	decltype(valid) next_valid;
	// filled in bfs level order, so iterating it backwards visits children
	// before parents (the order the draw loop's octant masking needs)
	std::vector<std::pair<std::string, rocktree_t::node_t *>> potential_nodes;

	// todo: abort emscripten_fetch_close() https://emscripten.org/docs/api_reference/fetch.html
	//       and/or emscripten coroutine fetch semaphore
	// todo: workers instead of shared mem https://emscripten.org/docs/api_reference/emscripten.h.html#worker-api

	// downloaded nodes are kept as an lru cache (see eviction below); stale
	// bulk metadata is kept for a fixed grace period
	auto now_ms = (double)SDL_GetTicks();
	const double keep_ms = 20 * 1000;

	// wanted bulks/nodes are marked by stamping last_wanted_ms with this
	// frame's time during the walk; membership tests elsewhere (eviction,
	// bulk purge) compare against the stamp instead of consulting a map
	auto potential_bulk_count = 0;

	// node culling and level of detail using breadth-first search
	bool all_loaded = true;
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
				b->last_wanted_ms = now_ms;
				if (b->dl_state == dl_state_stub) {
					b->setStartedDownloading();
					getBulk(b->request, b, [=](auto) {});			
				}
				if (b->dl_state != dl_state_downloaded) {
					all_loaded = false;
					continue;
				}
				bulk = b;
			}
			bulk->last_wanted_ms = now_ms;
			potential_bulk_count++;
						
			for(auto o : octs) {
				cnt_oct++;
				auto nxt = cur + o;
				auto nxt_rel = nxt.substr (floor((nxt.size() - 1) / 4) * 4, 4);
				auto node_kv = bulk->nodes.find(nxt_rel);
				if (node_kv == bulk->nodes.end()) // node at "nxt" doesn't exist
					continue;
				auto node = node_kv->second.get();

				// cull outside frustum using obb
				// todo: check if it could cull more
				cnt_cull++;
				if (obb_frustum_outside == classifyObbFrustum(&node->obb, frustum_planes)) {
					continue;
				}
				cnt_lod++;

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
					auto wh = width < height ? width : height;
					// zooming (narrowing the fov) magnifies the scene, so it
					// needs proportionally finer tiles. 1 at the default fov
					auto zoom = tan(0.125 * M_PI) / tan(camera_fov / 2.0);
					auto r = (2.0*(1.0/s)) * wh * zoom;
					if (texels_per_meter > r) continue;
				}

				next_valid.push_back(std::make_pair(nxt, bulk));

				if (node->can_have_data) {
					node->last_wanted_ms = now_ms;
					potential_nodes.emplace_back(std::move(nxt), node);
				}
			}
		}
		if (next_valid.size() == 0) break;
		valid = next_valid;
		next_valid.clear();
	}
	auto t1 = ticks_ms();
	sec_bfs += t1 - t0;

	// download nodes in order of importance: by apparent size on screen,
	// biggest first. distance to the node's bounding sphere over its diameter
	// is the inverse of angular size, so it ranks a fine tile right in front
	// of the camera above a coarse tile at the horizon, while a huge coarse
	// tile still wins over everything when nothing is loaded yet. only a
	// limited number of requests may be in flight at once, so the order can
	// adapt while the camera moves instead of everything being queued in path
	// order the first frame it becomes visible
	{
		static std::atomic<int> nodes_in_flight(0);
		// a slot is held from request through decode, so this needs to stay
		// comfortably above the decode pool's thread count to keep it fed
		const auto max_nodes_in_flight = 32;

		struct candidate_t { double priority; size_t level; rocktree_t::node_t *node; };
		std::vector<candidate_t> to_download;
		for (auto &kv : potential_nodes) {
			auto node = kv.second;
			if (node->dl_state != dl_state_downloaded) all_loaded = false;
			if (node->dl_state != dl_state_stub) continue;
			auto radius = node->obb.extents.norm();
			auto dist = fmax(0.0, (node->obb.center - eye).norm() - radius);
			to_download.push_back({ dist / (2 * radius), kv.first.size(), node });
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
			node->setStartedDownloading();
			getNode(node->request, node, [node](auto) { nodes_in_flight--; });
		}
		if (bench_mode && (to_download.size() > 0 || nodes_in_flight > 0))
			printf("timing: sched at=%u stubs=%zu started=%d inflight=%d pot=%zu\n",
				SDL_GetTicks(), to_download.size(), started, (int)nodes_in_flight, potential_nodes.size());
	}
	scene_complete = all_loaded;

	auto t2 = ticks_ms();
	sec_dl += t2 - t1;

	// downloaded nodes form an lru cache with a byte quota: nothing is evicted
	// until resident mesh/texture data exceeds the quota, then the least
	// recently wanted nodes are dropped first. nodes in the current potential
	// set are never evicted, even over quota
	// ~100-150KB per resident node; native gets a generous quota (disk cache
	// backstops evictions), wasm stays well under its 1GB total heap
	//
	// the sweep walks every node entry of every resident bulk (including
	// stubs), which costs tens of ms in heavy views — and it only needs to be
	// roughly current, so it runs every 15th frame
	static int evict_tick = 0;
	if (evict_tick++ % 15 == 0) {
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
	po = [&po, &obs_b_cnt, &total_b, now_ms, keep_ms](rocktree_t::bulk_t * b){
		for (auto &kv : b->bulks){
			auto b = kv.second.get();
			if (b->dl_state == dl_state_downloaded)
				po(b);
		}
		total_b++;
		// wanted bulks got last_wanted_ms stamped during this frame's bfs
		if (now_ms - b->last_wanted_ms > keep_ms) {
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
			printf("buffered: %d, tot_n: %d, tot_b: %d, pot_n: %lu, pot_b: %d, obs n: %d, obs b: %d, mem: %zu MB\n",
				buf_cnt, total_n, total_b, potential_nodes.size(), potential_bulk_count, obs_n_cnt, obs_b_cnt,
				resident_bytes >> 20
			);
		}
	}
	} // end amortized eviction sweep

	auto t3 = ticks_ms();
	sec_evict += t3 - t2;

	// 8-bit octant mask flags of nodes
	std::unordered_map<std::string, uint8_t> mask_map;

	// potential-set membership by path, only needed for lod debug tinting
	std::unordered_set<std::string> potential_set;
	if (debug_lod_mode)
		for (auto &kv : potential_nodes) potential_set.insert(kv.first);

	glUniform1i(ctx.debug_lod_loc, debug_lod_mode);

	// reverse level order: children before parents
	for (auto kv = potential_nodes.rbegin(); kv != potential_nodes.rend(); ++kv) {
		auto &full_path = kv->first;
		auto node = kv->second;
		auto level = full_path.size();
		assert(level > 0);
		assert(node->can_have_data);
		if (node->dl_state != dl_state_downloaded) continue;

		// set octant mask of previous node
		auto octant = (int)(full_path[level - 1] - '0');
		mask_map[full_path.substr(0, level - 1)] |= 1 << octant;

		// skip if node is masked completely
		static const bool no_mask_debug = getenv("EARTH_NO_MASK") != nullptr;
		auto self_mask = no_mask_debug ? (uint8_t)0 : mask_map[full_path];
		if (self_mask == 0xff) continue;

		// octants where a finer tile is wanted but not drawn (still
		// downloading). children are drawn before parents here, so mask_map
		// for this node is already complete
		uint8_t stale_mask = 0;
		if (debug_lod_mode) {
			for (auto o = 0; o < 8; o++) {
				if (self_mask & (1 << o)) continue;
				if (potential_set.count(full_path + octs[o]))
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
			bindAndDrawMesh(mesh, self_mask, stale_mask, ctx);
		}
		//bufs[full_path] = node;
	}

	sec_draw += ticks_ms() - t3;
	sec_frames++;
	{
		static double ms = 0;
		ms += deltaTime;
		if (ms > 2000 && sec_frames > 0) {
			ms = 0;
			printf("sections avg ms: bfs %.2f, dl %.2f, evict %.2f, draw %.2f (%d frames; per frame: oct %ld, cull %ld, lod %ld)\n",
				sec_bfs / sec_frames, sec_dl / sec_frames, sec_evict / sec_frames, sec_draw / sec_frames,
				sec_frames, cnt_oct / sec_frames, cnt_cull / sec_frames, cnt_lod / sec_frames);
			sec_bfs = sec_dl = sec_evict = sec_draw = 0;
			cnt_oct = cnt_cull = cnt_lod = 0;
			sec_frames = 0;
		}
	}
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
void benchFrame() {
	static uint32_t start_ms = SDL_GetTicks();
	static uint32_t first_complete_ms = 0;
	static int complete_streak = 0;
	static uint8_t *ref = nullptr;
	static int ref_w = 0, ref_h = 0;
	static FILE *csv = nullptr;
	static std::vector<uint8_t> pixels;

	auto elapsed = SDL_GetTicks() - start_ms;
	int w, h;
	SDL_GL_GetDrawableSize(sdl_window, &w, &h);
	pixels.resize((size_t)w * h * 3);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

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

	if (scene_complete) {
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

void mainloop(gl_ctx_t &ctx) {
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

	drawPlanet(ctx);
#ifndef EMSCRIPTEN
	if (bench_mode) benchFrame();
#endif
	SDL_GL_SwapWindow(sdl_window);
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

	// benchmarks and --nograb runs hold a fixed view; leave the cursor alone
	if (bench_mode || no_grab) mouse_captured = false;
	else SDL_SetRelativeMouseMode(SDL_TRUE);

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