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
	// last walk's want/have node counts, for download progress display
	int stat_nodes_wanted = 0, stat_nodes_loaded = 0;
	// tint octants where a finer tile is wanted but not yet drawn (L key)
	bool debug_lod = false;
	// print per-frame scheduler state (bench mode)
	bool log_sched = false;

	// tube mode: a geodetic rectangle of terrain is rolled into a cylinder in
	// the vertex shader (the inception effect). selection switches from
	// frustum culling + camera-driven lod to "every node whose obb touches
	// the rect, finest resolution available" — the rect is fixed, so there is
	// no point chasing the camera. curl animates flat slab (0) to fully
	// closed tube (1); the roll radius is chosen so the rect's width is
	// exactly the closed tube's circumference
	bool tube_on = false;
	double tube_curl = 1.0;
	double tube_radius = 0; // fully-curled tube radius (m)
	double tube_half_len = 0, tube_half_wid = 0; // rect half extents (m)
	double tube_ground_radius = 0; // ecef radius of the ground at the rect center
	bool tube_ground_locked = false; // true once measured from the loaded mesh
	bool tube_ew_axis = true; // tube axis east-west (else north-south)
	Vector3d tube_up = Vector3d::UnitZ(), tube_axis = Vector3d::UnitX();
	Matrix4d tube_globe_from_local = Matrix4d::Identity();
	Matrix4d tube_local_from_globe = Matrix4d::Identity();

	void tubeRebuild() {
		Matrix4d g = Matrix4d::Identity();
		g.block<3,1>(0,0) = tube_axis;
		g.block<3,1>(0,1) = tube_up.cross(tube_axis);
		g.block<3,1>(0,2) = tube_up;
		g.block<3,1>(0,3) = tube_up * tube_ground_radius;
		tube_globe_from_local = g;
		tube_local_from_globe = g.inverse();
	}

	// corners in radians, any order. the local frame sits on the ground at
	// the rect center: x along the tube axis (the rect's longer dimension),
	// y across it, z up. the z zero-point must be the actual terrain surface
	// — the roll radius ρ = R - z, so a reference even tens of meters off
	// shifts the whole sheet radially (and km off — e.g. the sphere radius
	// vs the wgs84 ellipsoid, -4.5km at seattle — rolls the rect into a huge
	// cylinder with invisible relief). staged: the sheet stays flat (curl 0,
	// where the zero-point cancels out and tiles render exactly in place)
	// while the rect loads, then tubeLockGround() measures the surface and
	// the host rolls the tube up. the ellipsoid seed here only sets the
	// height to cast measurement rays down from.
	// small-rect tangent-plane approximation: over a few km the curvature
	// drop below the plane is centimeters
	void setTubeRect(double lat0, double lon0, double lat1, double lon1, bool ew_axis) {
		auto R_p = planetoid && planetoid->downloaded ? (double)planetoid->radius : 6371010.0;
		auto latc = (lat0 + lat1) / 2, lonc = (lon0 + lon1) / 2;
		auto ns_half = R_p * fabs(lat1 - lat0) / 2;
		auto ew_half = R_p * cos(latc) * fabs(lon1 - lon0) / 2;
		Vector3d up(cos(latc) * cos(lonc), cos(latc) * sin(lonc), sin(latc));
		Vector3d east(-sin(lonc), cos(lonc), 0);
		Vector3d north = up.cross(east);
		tube_ew_axis = ew_axis;
		tube_up = up;
		tube_axis = ew_axis ? east : north;
		tube_half_len = ew_axis ? ew_half : ns_half;
		tube_half_wid = ew_axis ? ns_half : ew_half;
		tube_radius = fmax(1.0, tube_half_wid / M_PI); // 2*half_wid = 2*pi*R
		tube_ground_locked = false;
		// wgs84 geocentric radius at this latitude: within terrain height of
		// the surface everywhere, plenty for a ray start
		const double wa = 6378137.0, wb = 6356752.314245;
		auto ca = cos(latc), sa = sin(latc);
		tube_ground_radius = sqrt((pow(wa * wa * ca, 2) + pow(wb * wb * sa, 2))
			/ (pow(wa * ca, 2) + pow(wb * sa, 2)));
		tubeRebuild();
	}

	// where the camera is in the rolled tube's own coordinates, unrolled
	// back onto the rect: cylindrical coordinates around the tube axis
	// (angle theta, axial x), mapped to the spot on the original rectangle
	// whose imagery is wrapped there — what a 2d map should show in tube
	// mode instead of the ecef point under the camera. the view direction
	// unrolls by -theta the same way for the heading. degenerates smoothly
	// to the ordinary pose as curl -> 0. angles in radians
	bool tubePose(const Vector3d &eye, const Vector3d &direction,
			double &lat, double &lon, double &heading) {
		if (!tube_on || tube_radius <= 0) return false;
		auto R_eff = tube_radius / fmax(tube_curl, 1e-3);
		Vector3d l = (tube_local_from_globe
			* Vector4d(eye.x(), eye.y(), eye.z(), 1.0)).head<3>();
		Vector3d d = (tube_local_from_globe
			* Vector4d(direction.x(), direction.y(), direction.z(), 0.0)).head<3>();
		auto theta = atan2(l.y(), R_eff - l.z());
		auto y = theta * R_eff; // unrolled across-offset
		// unrolled across-component of the view direction: its projection on
		// the tangential direction at angle theta
		auto dy = d.y() * cos(theta) + d.z() * sin(theta);
		// local (x, y) to east/north displacement by axis orientation
		// (across = up x axis, so for a north-south axis, across is -east)
		auto dE = tube_ew_axis ? l.x() : -y;
		auto dN = tube_ew_axis ? y : l.x();
		auto hE = tube_ew_axis ? d.x() : -dy;
		auto hN = tube_ew_axis ? dy : d.x();
		auto latc = asin(fmax(-1.0, fmin(1.0, tube_up.z())));
		auto lonc = atan2(tube_up.y(), tube_up.x());
		auto R_p = planetoid && planetoid->downloaded ? (double)planetoid->radius : 6371010.0;
		lat = latc + dN / R_p;
		lon = lonc + dE / (R_p * cos(latc));
		heading = atan2(hE, hN);
		return true;
	}

	// terrain elevation converges several lod levels before the imagery
	// does: mesh at this resolution already carries the ground to within a
	// few meters, so the roll can start ~two orders of magnitude fewer tiles
	// in, and full res streams into the rolled tube afterwards
	static constexpr double tube_lock_mpt = 4.0;

	// the elevation analysis: raycast a 7x7 grid over the rect (cpu, against
	// the resident meshes) and take the median hit radius as the zero-point
	// — robust to a roof or treetop at any one sample. strict mode (retried
	// every frame while loading) only locks once every sample hits mesh
	// finer than tube_lock_mpt — that coverage test needs no download
	// bookkeeping. non-strict is the host's timeout escape hatch: take the
	// median of whatever is resident. the 0.9 keeps samples off the edges
	void tubeLockGround(bool strict) {
		std::vector<double> rs;
		Vector3d across = tube_up.cross(tube_axis);
		for (auto i = 0; i < 7; i++) {
			for (auto j = 0; j < 7; j++) {
				Vector3d p = tube_up * (tube_ground_radius + 10000.0)
					+ tube_axis * ((i / 3.0 - 1.0) * 0.9 * tube_half_len)
					+ across * ((j / 3.0 - 1.0) * 0.9 * tube_half_wid);
				Vector3d hit;
				double mpt;
				if (raycast(p, -tube_up, hit, &mpt) != raycast_mesh) {
					if (strict) return; // a sample has no mesh yet
					continue;
				}
				if (strict && mpt > tube_lock_mpt) return; // only coarse mesh here yet
				rs.push_back(hit.norm());
			}
		}
		if (rs.empty()) return; // nothing resident at all; retried next frame
		std::nth_element(rs.begin(), rs.begin() + rs.size() / 2, rs.end());
		tube_ground_radius = rs[rs.size() / 2];
		tube_ground_locked = true;
		tubeRebuild();
	}

	// download slots in flight, held from request through decode; the cap
	// needs to stay comfortably above the decode pool's thread count
	std::atomic<int> nodes_in_flight{0};

	// nodes actually drawn last frame with their octant masks, kept for the
	// center-pick raycast. refreshed every updateAndDraw; valid to use
	// between frames because eviction and bulk purge only run inside
	// updateAndDraw (shells are single-threaded around the frame loop)
	std::vector<std::pair<rocktree_t::node_t*, uint8_t>> drawn_nodes;

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

	// --- picking --------------------------------------------------------

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
		Vector3d o = (mesh_from_globe * Vector4d(origin.x(), origin.y(), origin.z(), 1.0)).head<3>();
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

	// distance along the boresight to the actual drawn terrain, for the
	// ortho view extent and ortho zoom/pan anchoring. the sphere-datum
	// centerDistance sits ~4.5km off the real mesh (sphere radius vs the
	// ellipsoidal terrain), which collapses the derived ortho extent as soon
	// as the eye dips inside the reference sphere — kilometers above the
	// visible ground. sphere formula kept only as a last-resort fallback
	// (looking at the sky, tiles not yet loaded)
	double centerDistanceMesh(const camera_t &cam) {
		Vector3d hit;
		if (raycast(cam.eye, cam.direction, hit) == raycast_mesh)
			return (hit - cam.eye).norm();
		return centerDistance(cam, planetoid->radius);
	}

	// cull, schedule downloads, evict, draw. the shell has already begun the
	// frame (renderFrameBegin) and will end it after this returns
	void updateAndDraw(render_ctx_t &ctx, const camera_t &cam, int width, int height, double dt_ms) {
		if (!ready()) return;
		auto current_bulk = planetoid->root_bulk;
		auto planet_radius = planetoid->radius;

		// stage 2 of the tube pipeline: as tiles stream in, keep attempting
		// the ground measurement against last frame's drawn meshes; it locks
		// as soon as the whole rect is covered at measurement resolution —
		// long before full res. the host watches groundLocked, rolls the
		// tube up, and the remaining detail streams into the rolled tube
		if (tube_on && !tube_ground_locked)
			tubeLockGround(true);

		auto &eye = cam.eye;
		auto &direction = cam.direction;
		// airplane mode owns its up vector (roll); otherwise up is gravity
		auto up = cam.airplane ? cam.body_up : Vector3d(eye.normalized());

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
		Matrix4d projection;
		double ortho_half_extent = 0; // vertical half-extent in meters (ortho only)
		if (cam.ortho) {
			// explicit extent state set on mode entry / zooms; derive from the
			// terrain distance only when unset (e.g. the native shell)
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

		// downloaded nodes and bulk metadata are both lru caches with byte
		// quotas (see the eviction sweep below); nothing is ever dropped on
		// a clock
		auto now_ms = ticksMs();

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
					if (tube_on) {
						// tube mode culls against the rect, not the view: the
						// whole slab is drawn wherever the camera looks. box
						// test in the rect's local frame, obb conservatively
						// widened to its bounding sphere; z allows terrain
						// heights (everest) plus a little below sea level
						Vector3d c = (tube_local_from_globe * Vector4d(
							node->obb.center.x(), node->obb.center.y(), node->obb.center.z(), 1.0)).head<3>();
						auto margin = node->obb.extents.norm();
						if (fabs(c.x()) > tube_half_len + margin
							|| fabs(c.y()) > tube_half_wid + margin
							|| c.z() > 9000 + margin || c.z() < -1500 - margin) {
							continue;
						}
					} else if (obb_frustum_outside == classifyObbFrustum(&node->obb, frustum_planes)) {
						continue;
					}
					cnt_lod++;

					// level of detail: tube mode always descends — the fixed
					// rect wants the finest imagery available, regardless of
					// where the camera is (the node cap below bounds the total)
					if (!tube_on) {
						auto texels_per_meter = 1.0f / node->meters_per_texel;
						auto wh = width < height ? width : height;
						double r;
						if (cam.ortho) {
							// parallel projection: pixels per meter is the same at
							// every depth, set entirely by the view extent
							r = 2.0 * wh * tan(0.125 * M_PI) / ortho_half_extent;
						} else {
							auto t = Affine3d().Identity();
							t.translate(eye + (eye-node->obb.center).norm() * direction);
							auto m = viewprojection * t;
							auto s = m(3, 3);
							// zooming (narrowing the fov) magnifies the scene, so it
							// needs proportionally finer tiles. 1 at the default fov
							auto zoom = tan(0.125 * M_PI) / tan(cam.fov / 2.0);
							r = (2.0*(1.0/s)) * wh * zoom;
						}
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
			// full resolution over a large rect can outgrow the memory quota
			// (the potential set is never evicted); stop descending at a
			// uniform level once the set is big enough
			if (tube_on && potential_nodes.size() > 3000) break;
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
			stat_nodes_wanted = (int)potential_nodes.size();
			stat_nodes_loaded = 0;
			for (auto &kv : potential_nodes) {
				auto node = kv.second;
				if (node->dl_state != dl_state_downloaded) all_loaded = false;
				else stat_nodes_loaded++;
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

		// bulk metadata (the octree index) is also an lru cache with its own
		// byte budget — never purged on a clock. a bulk is only purgeable
		// once it has no downloading/downloaded children (busy_ctr == 0), so
		// purges cascade bottom-up under sustained pressure
#ifdef EMSCRIPTEN
		const size_t bulk_quota = (size_t)256 << 20;
#else
		const size_t bulk_quota = (size_t)512 << 20;
#endif
		size_t bulk_bytes_total = 0;
		struct purge_t { double last_wanted_ms; size_t bytes; rocktree_t::bulk_t *b; };
		std::vector<purge_t> purgeable;
		auto bulk_bytes = [](rocktree_t::bulk_t *b) {
			// rough: map/protobuf overhead per child entry
			return sizeof(*b) + b->nodes.size() * (sizeof(rocktree_t::node_t) + 256)
				+ b->bulks.size() * 128;
		};

		std::vector<rocktree_t::bulk_t*> x = {current_bulk};
		auto buf_cnt = 0, obs_n_cnt = 0, obs_b_cnt = 0, total_n = 0, total_b = 0;
		while(!x.empty()) {
			auto cur_bulk = x[0]; x.erase(x.begin());
			total_b++;
			auto bbytes = bulk_bytes(cur_bulk);
			bulk_bytes_total += bbytes;
			if (cur_bulk->parent && cur_bulk->last_wanted_ms != now_ms && cur_bulk->busy_ctr == 0)
				purgeable.push_back({ cur_bulk->last_wanted_ms, bbytes, cur_bulk });
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

		// purge least recently wanted bulks until metadata is back under its
		// budget. clearing the child maps only ever destroys stubs: a bulk
		// with any downloading or downloaded child has busy_ctr > 0 and was
		// not collected above
		if (bulk_bytes_total > bulk_quota) {
			std::sort(purgeable.begin(), purgeable.end(), [](const purge_t &a, const purge_t &b) {
				return a.last_wanted_ms < b.last_wanted_ms;
			});
			for (auto &e : purgeable) {
				if (bulk_bytes_total <= bulk_quota) break;
				auto b = e.b;
				obs_b_cnt++;
				b->nodes.clear();
				b->bulks.clear();
				b->setDeleted();
				bulk_bytes_total -= e.bytes;
			}
		}

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

		// tube warp uniforms: the local->clip matrix folds the camera in, so
		// the shader works in small rect-local coordinates end to end. the
		// effective roll radius grows as curl shrinks (flat at curl 0); the
		// floor keeps it finite for the shader
		if (tube_on) {
			Matrix4d l2c = viewprojection * tube_globe_from_local;
			Matrix4f l2cf = l2c.cast<float>();
			auto r_eff = tube_radius / fmax(tube_curl, 1e-3);
			renderSetTube(ctx, true, l2cf.data(),
				(float)r_eff, (float)tube_half_len, (float)tube_half_wid);
		} else {
			renderSetTube(ctx, false, nullptr, 0, 0, 0);
		}

		drawn_nodes.clear();

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

			drawn_nodes.push_back({ node, self_mask });

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
			if (tube_on) {
				// mesh -> rect-local frame, composed in double then narrowed:
				// local coordinates are km-scale, safe in float
				Matrix4f m2l = (tube_local_from_globe * node->matrix_globe_from_mesh).cast<float>();
				renderSetTubeNode(ctx, m2l.data());
			}
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
