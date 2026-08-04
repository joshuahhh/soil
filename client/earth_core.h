// the planet engine, independent of windowing and input: owns the rocktree,
// runs the per-frame culling/lod walk, schedules downloads, maintains the
// lru cache, and emits draws through the render backend interface. shells
// (native window in main.cpp, web canvas later) own the camera and the frame
// lifecycle and call updateAndDraw between renderFrameBegin/renderFrameEnd.
//
// single-tu style: include after rocktree_util.h and earth_camera.h.
// process-wide singletons remain in rocktree_util.h (thread pools, request
// maps), so one instance per process for now.

struct earth_core_t {
	rocktree_t *planetoid = nullptr;

	// true when the last walk saw everything the view wants fully downloaded
	bool scene_complete = false;
	// tint octants where a finer tile is wanted but not yet drawn (L key)
	bool debug_lod = false;
	// print per-frame scheduler state (bench mode)
	bool log_sched = false;

	// download slots in flight, held from request through decode; the cap
	// needs to stay comfortably above the decode pool's thread count
	std::atomic<int> nodes_in_flight{0};

	// per-section frame timing and bfs work counters, reported every 2s
	double sec_bfs = 0, sec_dl = 0, sec_evict = 0, sec_draw = 0;
	long cnt_oct = 0, cnt_cull = 0, cnt_lod = 0;
	int sec_frames = 0;
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
			getBulk(bulk->request, bulk, [=](auto _) { /* todo rm cb */ });
		});
	}

	bool ready() const {
		return planetoid && planetoid->downloaded
			&& planetoid->root_bulk->dl_state == dl_state_downloaded;
	}

	double radius() const { return planetoid->radius; }

	// cull, schedule downloads, evict, draw. the shell has already begun the
	// frame (renderFrameBegin) and will end it after this returns
	void updateAndDraw(render_ctx_t &ctx, const camera_t &cam, int width, int height, double dt_ms) {
		if (!ready()) return;
		auto current_bulk = planetoid->root_bulk;
		auto planet_radius = planetoid->radius;

		auto &eye = cam.eye;
		auto &direction = cam.direction;
		auto up = eye.normalized();

		// projection
		float aspect_ratio = (float)width / (float)height;
		float fov = (float)cam.fov;
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
		Matrix4d projection = perspective(fov, aspect_ratio, near, far);

		auto t0 = ticksMs();

		auto view = lookAt(eye, eye + direction, up);
		Matrix4d viewprojection = projection * view;

		auto frustum_planes = getFrustumPlanes(viewprojection); // for obb culling

		const std::string octs[] = { "0", "1", "2", "3", "4", "5", "6", "7" };
		std::vector<std::pair<std::string, rocktree_t::bulk_t *>> valid = { std::make_pair("", current_bulk) };
		decltype(valid) next_valid;
		// filled in bfs level order, so iterating it backwards visits children
		// before parents (the order the draw loop's octant masking needs)
		std::vector<std::pair<std::string, rocktree_t::node_t *>> potential_nodes;

		// downloaded nodes are kept as an lru cache (see eviction below); stale
		// bulk metadata is kept for a generous grace period: it's small, and
		// purging it makes the lod walk unable to reach still-resident meshes
		// when the camera looks back, flashing the scene coarse until the
		// metadata re-downloads
		auto now_ms = ticksMs();
		const double keep_ms = 300 * 1000;

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
					{
						auto t = Affine3d().Identity();
						t.translate(eye + (eye-node->obb.center).norm() * direction);
						auto m = viewprojection * t;
						auto s = m(3, 3);
						auto texels_per_meter = 1.0f / node->meters_per_texel;
						auto wh = width < height ? width : height;
						// zooming (narrowing the fov) magnifies the scene, so it
						// needs proportionally finer tiles. 1 at the default fov
						auto zoom = tan(0.125 * M_PI) / tan(cam.fov / 2.0);
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
		auto t1 = ticksMs();
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
				getNode(node->request, node, [node, this](auto) { nodes_in_flight--; });
			}
			if (log_sched && (to_download.size() > 0 || nodes_in_flight > 0))
				printf("timing: sched at=%.0f stubs=%zu started=%d inflight=%d pot=%zu\n",
					now_ms, to_download.size(), started, (int)nodes_in_flight, potential_nodes.size());
		}
		scene_complete = all_loaded;

		auto t2 = ticksMs();
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
		if (evict_tick++ % 15 == 0) {
#ifdef EMSCRIPTEN
		// the wasm heap is a fixed 1GB; leave headroom for decode buffers
		// and the module itself. too small a quota makes looking around
		// evict whatever was just behind you
		const size_t mem_quota = (size_t)512 << 20;
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
		stats_report_ms += dt_ms;
		if (stats_report_ms > 2000) {
			stats_report_ms = 0;
			printf("buffered: %d, tot_n: %d, tot_b: %d, pot_n: %lu, pot_b: %d, obs n: %d, obs b: %d, mem: %zu MB\n",
				buf_cnt, total_n, total_b, potential_nodes.size(), potential_bulk_count, obs_n_cnt, obs_b_cnt,
				resident_bytes >> 20
			);
		}
		} // end amortized eviction sweep

		auto t3 = ticksMs();
		sec_evict += t3 - t2;

		// 8-bit octant mask flags of nodes
		std::unordered_map<std::string, uint8_t> mask_map;

		// potential-set membership by path, only needed for lod debug tinting
		std::unordered_set<std::string> potential_set;
		if (debug_lod)
			for (auto &kv : potential_nodes) potential_set.insert(kv.first);

		renderSetDebugLod(ctx, debug_lod);

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
			if (debug_lod) {
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
			renderSetTransform(ctx, transform_float.data());
			for (auto &mesh : node->meshes) {
				if (!mesh.buffered) bufferMesh(mesh);
				bindAndDrawMesh(mesh, self_mask, stale_mask, ctx);
			}
		}

		sec_draw += ticksMs() - t3;
		sec_frames++;
		sec_report_ms += dt_ms;
		if (sec_report_ms > 2000 && sec_frames > 0) {
			sec_report_ms = 0;
			printf("sections avg ms: bfs %.2f, dl %.2f, evict %.2f, draw %.2f (%d frames; per frame: oct %ld, cull %ld, lod %ld)\n",
				sec_bfs / sec_frames, sec_dl / sec_frames, sec_evict / sec_frames, sec_draw / sec_frames,
				sec_frames, cnt_oct / sec_frames, cnt_cull / sec_frames, cnt_lod / sec_frames);
			sec_bfs = sec_dl = sec_evict = sec_draw = 0;
			cnt_oct = cnt_cull = cnt_lod = 0;
			sec_frames = 0;
		}
	}
};
