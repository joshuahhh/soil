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
	// the rest of the download picture, reported with the 2s sections line.
	// 'stuck' is the one to watch: nodes the walk still wants whose request
	// went out and never came back either way. a transport error resolves
	// the request without resetting the node to a stub, so it is never
	// retried and never drawn — and it isn't in flight either, so the
	// scheduler has nothing left to say about it
	int stat_nodes_stub = 0, stat_nodes_stuck = 0, stat_bulk_blocked = 0, stat_max_level = 0;
	int stat_nodes_failed = 0, stat_bulks_started = 0;
	// <= 0 means "not decided yet": the first walk seeds it from the env
	double lod_scale = 0;
	// tint octants where a finer tile is wanted but not yet drawn (L key)
	bool debug_lod = false;
	// print per-frame scheduler state (bench mode)
	bool log_sched = false;

	// terrain-hug ground column: when the shell sets this, selection always
	// includes the finest tiles in the vertical column under the given point
	// even when the view frustum culls them — the ground query needs real
	// mesh there, not whichever coarse ancestor happens to be on screen
	// (see groundRadiusUnder)
	bool ground_column_on = false;
	Vector3d ground_column_point = Vector3d::Zero();

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
	// where the wrapped range sits across the axis: the rect spans y in
	// [offset - half_wid, offset + half_wid]. zero for a drawn rect (the
	// seam straight up over its centre line); the tunnel's across mode
	// sets a quarter circumference, which puts the seam level behind you
	double tube_wid_offset = 0;
	double tube_ground_radius = 0; // ecef radius of the ground at the rect center
	bool tube_ground_locked = false; // true once measured from the loaded mesh
	bool tube_ew_axis = true; // tube axis east-west (else north-south)
	Vector3d tube_up = Vector3d::UnitZ(), tube_axis = Vector3d::UnitX();
	Matrix4d tube_globe_from_local = Matrix4d::Identity();
	Matrix4d tube_local_from_globe = Matrix4d::Identity();

	// tunnel mode: the tube again, but the rect is not drawn on a map — it
	// is wherever the camera is, and it turns with the heading. the camera
	// flies as it does on the ground, at whatever height it likes, and the
	// ground rolls up around it: with tunnel_across (the default) the ground
	// ahead curls up in front of you, passes overhead — the seam, straight
	// up — and comes down behind, so flying forward rolls the land past you
	// and moves the seam's place on the ground along; without it the tube's
	// axis runs along the heading instead, and you look down the pipe. one
	// parameter, the circumference; the roll's radius is what puts the
	// camera on the axis (height R above ground) or on the floor (low)
	bool tunnel_on = false;
	bool tunnel_across = true;
	double tunnel_circumference = 2000;
	// looking down the pipe: how far it runs each way, in circumferences
	double tunnel_length = 10;
	// the ground under the camera, eased: the rect's zero-point has to be
	// the terrain surface (see setTubeRect), and here the rect moves every
	// frame, so the lock is a continuous measurement rather than a one-off
	double tunnel_ground_radius = 0;
	bool tunnel_ground_measured = false;
	// the heading the tube follows, low-passed: turn the camera and at
	// first you fly through the old tube, which then swings round to line
	// up with where you are going. the time constant in ms; 0 is immediate
	double tunnel_lag_ms = 0;
	Vector3d tunnel_heading = Vector3d::UnitX();
	bool tunnel_heading_set = false;

	// body mode: the tube's rect again — same selection, same ground lock,
	// same curl — but instead of rolling into a cylinder the sheet is
	// wrapped onto the skin of a figure standing on the rect's centre. the
	// figure is a geometry image the host bakes (index.html bakeBody): a
	// w×h grid over (u around the figure, v feet to head) of surface
	// positions and outward unit normals, in body units — height 1, x
	// across the figure, y up, z out of its front. the rect's length
	// (along the tube axis) becomes the figure's height, so a rect-local
	// point goes: x -> v, y -> u, and its terrain height z is pushed out
	// along the normal, unscaled. positions and normals stay resident on
	// the cpu for the lod walk's distances and the map marker
	bool tube_body = false;
	int body_w = 0, body_h = 0;
	std::vector<float> body_pos, body_nrm; // w*h*4 each, row j = v, column i = u
	bool body_dirty = false; // the gl copy is behind the cpu one
	// the image's loops (index.html bakeBody): the rows below v = [0] are
	// the two legs, half the width each; rows in v = [1]..[2] are arms and
	// torso, the arms taking the outer [3] of the width each. the shader
	// keeps its samples, and the terrain's triangles, within one loop
	double body_bands[4] = {0, 0, 0, 0};

	// the tunnel's frame from the camera: up at the eye; the axis sideways
	// (heading x up, so the rect's across direction — the one that wraps —
	// is the heading) or along the heading; ground measured under the eye.
	// every frame, before the walk. the camera's own local position is
	// then (0, 0, its height above that ground), which the roll leaves at
	// the tube's bottom: the camera is inside wherever its altitude puts it
	void tunnelUpdate(const camera_t &cam, double dt_ms) {
		auto R_p = planetoid && planetoid->downloaded ? (double)planetoid->radius : 6371010.0;
		Vector3d up = cam.eye.normalized();
		Vector3d h = cam.direction - up * cam.direction.dot(up);
		if (h.norm() < 1e-6 && tunnel_heading_set) h = tunnel_heading - up * tunnel_heading.dot(up); // straight down: keep the old heading
		if (h.norm() < 1e-6) h = up.cross(Vector3d::UnitZ());
		h.normalize();
		// the low-pass: ease the followed heading toward the camera's,
		// keeping it in the tangent plane. a 180° turn passes through
		// zero length, so a vanished vector snaps to the new heading
		if (tunnel_heading_set && tunnel_lag_ms > 0) {
			Vector3d hd = tunnel_heading - up * tunnel_heading.dot(up);
			hd += (h - hd) * (1.0 - exp(-dt_ms / tunnel_lag_ms));
			h = hd.norm() > 1e-6 ? hd.normalized() : h;
		}
		tunnel_heading = h;
		tunnel_heading_set = true;
		auto C = fmax(50.0, tunnel_circumference);
		// the ground: straight down against the drawn meshes (stored
		// unrolled, so the ray is the same as flat mode's). hold the last
		// measurement while the mesh under the camera is missing or coarse
		if (!tunnel_ground_measured) tunnel_ground_radius = R_p;
		auto g = groundRadiusUnder(cam.eye);
		if (g > 0) {
			if (!tunnel_ground_measured) { tunnel_ground_radius = g; tunnel_ground_measured = true; }
			else tunnel_ground_radius += (g - tunnel_ground_radius) * (1.0 - exp(-dt_ms / 250.0));
		}
		Vector3d axis = tunnel_across ? h.cross(up).normalized() : h;
		tube_up = up;
		tube_axis = axis;
		tube_ew_axis = fabs(axis.z()) < 0.7; // roughly east-west, for the map's arrows
		tube_half_wid = C / 2;                 // across the axis: what wraps
		// along it. looking down the pipe its far end is in view, so it
		// runs a long way (tunnel_length) and fades to black (tunnelFade) —
		// at 1.5 C the open end was a wide disc of sky a quarter of the way
		// across the view; at 10 C it is a small dark spot
		tube_half_len = fmax(500.0, (tunnel_across ? 1.5 : fmax(1.0, tunnel_length)) * C);
		// across mode: the seam level behind you (theta = -90°), so the
		// wrapped range runs from a quarter circumference behind to three
		// quarters ahead. along the axis it stays overhead
		tube_wid_offset = tunnel_across ? C / 4 : 0;
		tube_radius = fmax(1.0, tube_half_wid / M_PI);
		tube_ground_radius = tunnel_ground_radius;
		tube_ground_locked = true;
		tubeRebuild();
	}

	// where the walls start going dark, in rect units from the camera
	// along the axis (the rect is centred on it): only looking down the
	// pipe, where its ends are what you see
	double tunnelFade() const {
		return tube_on && tunnel_on && !tunnel_across ? 0.15 : 0.0;
	}

	// how far the sky is dimmed to black, 0..1: inside the closed tunnel
	// down the heading the only sky in view is through the pipe's two
	// ends, and blue there undoes the fade. eased in with the curl, and
	// only while the camera is inside (below the roof, 2R above the floor)
	double tunnelSkyDark(const Vector3d &eye) const {
		if (tunnelFade() <= 0) return 0;
		if (eye.norm() - tube_ground_radius > 2 * tube_radius) return 0;
		auto c = fmax(0.0, fmin(tube_curl, 1.0));
		return c * c;
	}

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
	// shifts the whole sheet radially. staged: the sheet stays flat (curl 0,
	// where the zero-point cancels out and tiles render exactly in place)
	// while the rect loads, then tubeLockGround() measures the surface and
	// the host rolls the tube up; the seed here only sets the height to
	// cast measurement rays down from.
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
		tube_wid_offset = 0;
		tube_ground_locked = false;
		// seed at the sphere radius: measured (straight-down mesh probes at
		// many levels and latitudes) the mesh datum IS the planetoid sphere,
		// not the wgs84 ellipsoid — mesh hits come in at sphere + terrain
		// height. the real zero-point still comes from tubeLockGround()
		tube_ground_radius = R_p;
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
		rectToGeo(l.x(), y, d.x(), dy, lat, lon, heading);
		return true;
	}

	// a point on the unrolled rect (x along the axis, y across; a direction
	// (dx, dy) in the same terms) to lat/lon/heading, for whatever
	// direction the axis runs (a tunnel's follows the heading)
	void rectToGeo(double x, double y, double dx, double dy,
			double &lat, double &lon, double &heading) {
		auto latc = asin(fmax(-1.0, fmin(1.0, tube_up.z())));
		auto lonc = atan2(tube_up.y(), tube_up.x());
		Vector3d east(-sin(lonc), cos(lonc), 0);
		Vector3d north = tube_up.cross(east);
		Vector3d across = tube_up.cross(tube_axis);
		auto dE = x * tube_axis.dot(east) + y * across.dot(east);
		auto dN = x * tube_axis.dot(north) + y * across.dot(north);
		auto hE = dx * tube_axis.dot(east) + dy * across.dot(east);
		auto hN = dx * tube_axis.dot(north) + dy * across.dot(north);
		auto R_p = planetoid && planetoid->downloaded ? (double)planetoid->radius : 6371010.0;
		lat = latc + dN / R_p;
		lon = lonc + dE / (R_p * cos(latc));
		heading = atan2(hE, hN);
	}

	// the body's geometry image, nearest texel: u wraps (the seam runs down
	// the figure's back), v clamps (the crown and the soles). the shader
	// interpolates; a texel is close enough for lod distances and the marker
	void bodyTexel(int i, int j, Vector3d &p, Vector3d &n) const {
		i = ((i % body_w) + body_w) % body_w;
		j = std::max(0, std::min(j, body_h - 1));
		auto k = (size_t)(j * body_w + i) * 4;
		p = Vector3d(body_pos[k], body_pos[k + 1], body_pos[k + 2]);
		n = Vector3d(body_nrm[k], body_nrm[k + 1], body_nrm[k + 2]);
	}
	// body units to the rect-local frame: the figure stands on the rect's
	// centre, as tall as the rect is long, facing across it (+y). the same
	// axis swap for directions, without the scale
	Vector3d bodyToLocal(const Vector3d &b) const {
		auto S = 2.0 * tube_half_len;
		return Vector3d(S * b.x(), S * b.z(), S * b.y());
	}
	static Vector3d bodyDirToLocal(const Vector3d &n) {
		return Vector3d(n.x(), n.z(), n.y());
	}
	// the body's counterpart of tubeWarp: a rect-local point onto the
	// figure's skin, its terrain height pushed out along the normal, eased
	// from the flat sheet by curl — the exact warp the shader applies
	Vector3d bodyWarp(const Vector3d &l) const {
		if (body_w < 2 || body_h < 2) return l;
		auto u = (l.y() - tube_wid_offset) / (2.0 * tube_half_wid) + 0.5;
		auto v = l.x() / (2.0 * tube_half_len) + 0.5;
		Vector3d p, n;
		bodyTexel((int)floor(u * body_w), (int)floor(v * body_h), p, n);
		Vector3d w = bodyToLocal(p) + bodyDirToLocal(n) * l.z();
		return l + (w - l) * fmax(0.0, fmin(tube_curl, 1.0));
	}
	// tubePose for the body: the texel whose skin is nearest the camera,
	// unwrapped to its spot on the rect. a coarse scan of the image and a
	// refinement around the best hit; the heading is the camera's own
	bool bodyPose(const Vector3d &eye, const Vector3d &direction,
			double &lat, double &lon, double &heading) {
		if (!tube_on || !tube_body || body_w < 2 || body_h < 2) return false;
		Vector3d l = (tube_local_from_globe
			* Vector4d(eye.x(), eye.y(), eye.z(), 1.0)).head<3>();
		auto best = 1e300;
		int bi = 0, bj = 0;
		auto consider = [&](int i, int j) {
			Vector3d p, n;
			bodyTexel(i, j, p, n);
			auto d = (bodyToLocal(p) - l).squaredNorm();
			if (d < best) { best = d; bi = i; bj = j; }
		};
		auto step = std::max(1, body_w / 64);
		for (auto j = 0; j < body_h; j += step)
			for (auto i = 0; i < body_w; i += step) consider(i, j);
		auto ci = bi, cj = bj;
		for (auto j = cj - step; j <= cj + step; j++)
			for (auto i = ci - step; i <= ci + step; i++) consider(i, j);
		auto u = (((bi % body_w) + body_w) % body_w + 0.5) / body_w;
		auto v = (std::max(0, std::min(bj, body_h - 1)) + 0.5) / body_h;
		auto x = (v - 0.5) * 2.0 * tube_half_len;
		auto y = tube_wid_offset + (u - 0.5) * 2.0 * tube_half_wid;
		auto latc = asin(fmax(-1.0, fmin(1.0, tube_up.z())));
		auto lonc = atan2(tube_up.y(), tube_up.x());
		Vector3d east(-sin(lonc), cos(lonc), 0);
		Vector3d north = tube_up.cross(east);
		rectToGeo(x, y, 0, 0, lat, lon, heading);
		heading = atan2(direction.dot(east), direction.dot(north));
		return true;
	}

	// a rect-local point mapped onto the rolled tube, in the same local
	// frame — the exact warp the vertex shader applies (rocktree_gl.h),
	// so cpu-side distances match what is actually drawn. used by the lod
	// walk to measure camera-to-node proximity through the roll: geometry
	// across the tube's diameter is a couple of radii away (overhead), not
	// half a circumference
	Vector3d tubeWarp(const Vector3d &l) const {
		auto R = tube_radius / fmax(tube_curl, 1e-3);
		auto th = l.y() / R;
		auto rho = fmax(R - l.z(), 0.05 * R);
		auto ze = R - rho;
		auto hs = sin(0.5 * th);
		return Vector3d(l.x(), rho * sin(th), 2.0 * R * hs * hs + ze * cos(th));
	}

	// does a node reach the rect? box test in the rect's local frame on
	// the node's centre c, the obb conservatively widened to its bounding
	// sphere (margin); z allows terrain heights (everest) plus a little
	// below sea level. with a negative margin: is it wholly inside
	bool tubeRectTouches(double margin, const Vector3d &c) const {
		return !(fabs(c.x()) > tube_half_len + margin
			|| fabs(c.y() - tube_wid_offset) > tube_half_wid + margin
			|| c.z() > 9000 + margin || c.z() < -1500 - margin);
	}

	// the rolled tube's cylindrical coordinate system, for tube terrain hug
	// (see earth_web fly): axial coordinate x along the tube axis, angle
	// theta around it (0 at the wall's bottom, radial_l(theta) =
	// (0, sin, -cos) in the rect-local frame), radial distance rho from the
	// axis. the frame is right-handed with local +x = tube_axis, so a
	// rotation by dtheta about tube_axis in globe space is exactly the
	// transport that carries the frame at theta to the frame at theta+dtheta
	struct tube_cyl_t {
		double x, theta, rho;
		Vector3d radial; // outward (toward the wall) unit radial, globe frame
	};
	tube_cyl_t tubeCyl(const Vector3d &p) const {
		tube_cyl_t c;
		auto R = tube_radius / fmax(tube_curl, 1e-3);
		Vector3d l = (tube_local_from_globe
			* Vector4d(p.x(), p.y(), p.z(), 1.0)).head<3>();
		c.x = l.x();
		c.theta = atan2(l.y(), R - l.z());
		c.rho = Vector2d(l.y(), l.z() - R).norm();
		Vector3d radial_l(0.0, sin(c.theta), -cos(c.theta));
		c.radial = tube_globe_from_local.block<3, 3>(0, 0) * radial_l;
		return c;
	}

	// globe position of cylindrical (x, theta, rho)
	Vector3d tubeCylPoint(double x, double theta, double rho) const {
		auto R = tube_radius / fmax(tube_curl, 1e-3);
		return (tube_globe_from_local
			* Vector4d(x, rho * sin(theta), R - rho * cos(theta), 1.0)).head<3>();
	}

	// the wall's radial distance at (x, theta): unroll the angle back onto
	// the rect (the meshes are stored unrolled; only the shader rolls them)
	// and raycast straight down from above any terrain, like tubeLockGround's
	// probes. theta wraps so walking around a closed tube's seam continues
	// seamlessly. false while the mesh there is missing or coarse filler
	// (same gate as groundRadiusUnder): better to hold and wait than glide
	// toward a surface that sits tens of meters off
	bool tubeWallRho(double x, double theta, double &rho_wall) {
		if (!tube_on || tube_radius <= 0) return false;
		auto R = tube_radius / fmax(tube_curl, 1e-3);
		auto y = tube_wid_offset + remainder(theta - tube_wid_offset / R, 2.0 * M_PI) * R;
		Vector3d origin = (tube_globe_from_local
			* Vector4d(x, y, 10000.0, 1.0)).head<3>();
		Vector3d hit;
		double mpt;
		if (raycast(origin, -tube_up, hit, &mpt) != raycast_mesh) return false;
		if (mpt > 10.0) return false;
		auto z_ground = (tube_local_from_globe
			* Vector4d(hit.x(), hit.y(), hit.z(), 1.0)).z();
		rho_wall = fmax(R - z_ground, 0.05 * R); // the warp's own clamp
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
					+ across * (tube_wid_offset + (j / 3.0 - 1.0) * 0.9 * tube_half_wid);
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
	std::vector<std::pair<std::string, rocktree_t::node_t *>> potential_nodes;
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
	Matrix4d walk_key_vp = Matrix4d::Zero();
	Vector3d walk_key_eye = Vector3d::Zero();
	int walk_key_w = 0, walk_key_h = 0, walk_key_epoch = -1;
	double walk_key_lod = 0;
	bool walk_key_column = false;

	// per-section frame timing and bfs work counters, reported every 2s
	double sec_bfs = 0, sec_dl = 0, sec_evict = 0, sec_draw = 0;
	long cnt_oct = 0, cnt_cull = 0, cnt_lod = 0;
	int sec_frames = 0;
	// the last 2s report's per-frame averages, kept (rather than only
	// printed) so shells can read them back — see EarthView::getStats
	double avg_bfs = 0, avg_dl = 0, avg_evict = 0, avg_draw = 0;
	long avg_oct = 0, avg_cull = 0, avg_lod = 0;
	int avg_fps = 0;
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
		for (auto i = 0; i < 8; i++) {
			Vector3d s(i & 1 ? obb.extents.x() : -obb.extents.x(),
				i & 2 ? obb.extents.y() : -obb.extents.y(),
				i & 4 ? obb.extents.z() : -obb.extents.z());
			if (!pointOccluded(eye, obb.center + obb.orientation * s, r_occ2)) return false;
		}
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
		auto R = planetoid && planetoid->downloaded ? planetoid->radius : 6371010.0;
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

	void setPathCursor(double lat, double lon, double alt) {
		auto R = planetoid && planetoid->downloaded ? planetoid->radius : 6371010.0;
		path_cursor = geoUp(lat * M_PI / 180.0, lon * M_PI / 180.0) * (R + alt);
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

	// EARTH_AUDIT=1: once the scene completes, compare each drawn node's
	// mesh placement (its transform applied to the center of the 0..255
	// mesh coordinate cube) against the metadata obb it was selected by —
	// content from the wrong node, or a wrong transform, shows up as a mesh
	// center many obb-radii away from where the octree says the node lives
	bool audited = false;
	void auditDrawnNodes(const Matrix4d &viewprojection, int width, int height) {
		auto bad = 0;
		FILE *f = fopen("/tmp/earth_audit.txt", "w");
		for (auto &kv : drawn_nodes) {
			auto n = kv.first;
			Vector3d c = (n->matrix_globe_from_mesh * Vector4d(128, 128, 128, 1)).head<3>();
			auto d = (c - n->obb.center).norm();
			auto r = n->obb.extents.norm();
			if (d > 2 * r) {
				bad++;
				printf("audit: node %s mesh center %.0fm from obb center (obb radius %.0f)\n",
					n->request.node_key().path().c_str(), d, r);
			}
			if (f) {
				// screen position of the obb center, for mapping artifacts in
				// a capture back to the node that drew them
				Vector4d p = viewprojection * Vector4d(n->obb.center.x(), n->obb.center.y(), n->obb.center.z(), 1.0);
				auto sx = p.w() != 0 ? (p.x() / p.w() * 0.5 + 0.5) * width : -1;
				auto sy = p.w() != 0 ? (1.0 - (p.y() / p.w() * 0.5 + 0.5)) * height : -1;
				auto path = n->request.node_key().path();
				auto &oc = n->obb.center;
				auto ocr = oc.norm();
				fprintf(f, "node %s level %zu screen %.0f,%.0f mask %02x mpt %.1f "
					"lat %.2f lon %.2f r %.0f ext %.0f,%.0f,%.0f meshes %zu",
					path.c_str(), path.size(), sx, sy, kv.second, n->meters_per_texel,
					asin(oc.z() / ocr) * 180 / M_PI, atan2(oc.y(), oc.x()) * 180 / M_PI,
					ocr, n->obb.extents.x(), n->obb.extents.y(), n->obb.extents.z(),
					n->meshes.size());
				for (auto &m : n->meshes)
					fprintf(f, " [tex %dx%d fmt %d uvoff %.1f,%.1f uvscale %.6f,%.6f verts %zu]",
						m.texture_width, m.texture_height, m.texture_format,
						m.uv_offset[0], m.uv_offset[1], m.uv_scale[0], m.uv_scale[1],
						m.vertices.size() / 8);
				fprintf(f, "\n");
			}
		}
		if (f) fclose(f);
		printf("audit: %d of %zu drawn nodes displaced; details in /tmp/earth_audit.txt\n",
			bad, drawn_nodes.size());

		// EARTH_DUMP_TEX=<dir>: decode every drawn node's dxt1 texture to a
		// ppm named by its octant path — ground truth for whether the image
		// on a tile is the image of the tile
		if (auto dir = getenv("EARTH_DUMP_TEX")) {
			for (auto &kv : drawn_nodes) {
				auto n = kv.first;
				if (n->meshes.empty()) continue;
				auto &m = n->meshes[0];
				int w = m.texture_width, h = m.texture_height;
				std::vector<uint8_t> rgb(w * h * 3);
				if (m.texture_format == rocktree_t::texture_format_rgb) {
					if (m.texture.size() < rgb.size()) continue;
					memcpy(rgb.data(), m.texture.data(), rgb.size());
				} else if (m.texture_format == rocktree_t::texture_format_dxt1) {
				for (int by = 0; by < h / 4; by++) for (int bx = 0; bx < w / 4; bx++) {
					auto p = m.texture.data() + ((size_t)by * (w / 4) + bx) * 8;
					uint16_t c0 = p[0] | p[1] << 8, c1 = p[2] | p[3] << 8;
					uint32_t bits = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
					int pal[4][3];
					auto expand = [](uint16_t c, int *o) {
						o[0] = (c >> 11) << 3; o[1] = ((c >> 5) & 63) << 2; o[2] = (c & 31) << 3;
					};
					expand(c0, pal[0]);
					expand(c1, pal[1]);
					for (int k = 0; k < 3; k++) {
						pal[2][k] = c0 > c1 ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
						pal[3][k] = c0 > c1 ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
					}
					for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
						auto *c = pal[(bits >> ((py * 4 + px) * 2)) & 3];
						auto o = ((size_t)(by * 4 + py) * w + bx * 4 + px) * 3;
						rgb[o] = c[0]; rgb[o + 1] = c[1]; rgb[o + 2] = c[2];
					}
				}
				} else continue;
				char fn[512];
				snprintf(fn, sizeof fn, "%s/%s.ppm", dir,
					n->request.node_key().path().c_str());
				if (FILE *tf = fopen(fn, "w")) {
					fprintf(tf, "P6\n%d %d\n255\n", w, h);
					fwrite(rgb.data(), 1, rgb.size(), tf);
					fclose(tf);
				}
			}
		}

		// EARTH_PROBE="x,y;x,y;...": shoot rays through these pixels and
		// report which drawn node's triangle wins — maps artifact pixels in
		// a capture directly to the node that painted them
		auto probe = getenv("EARTH_PROBE");
		if (!probe) return;
		Matrix4d inv = viewprojection.inverse();
		auto unproject = [&](double px, double py, double ndc_z) {
			Vector4d ndc(px / width * 2 - 1, 1 - py / height * 2, ndc_z, 1.0);
			Vector4d w = inv * ndc;
			return Vector3d(w.head<3>() / w.w());
		};
		double px, py;
		const char *s = probe;
		while (sscanf(s, "%lf,%lf", &px, &py) == 2) {
			Vector3d a = unproject(px, py, -1.0), b = unproject(px, py, 1.0);
			Vector3d dir = (b - a).normalized();
			double best_t = INFINITY;
			const rocktree_t::node_t *best = nullptr;
			for (auto &kv : drawn_nodes) {
				if (!rayHitsObb(a, dir, kv.first->obb, best_t)) continue;
				rayNodeNearest(a, dir, kv.first, kv.second, best_t, best);
			}
			if (best) {
				Vector3d h = a + dir * best_t;
				auto hr = h.norm();
				printf("probe %.0f,%.0f: node %s level %zu hit lat %.2f lon %.2f r %.0f (alt %+.0f) mpt %.1f\n",
					px, py, best->request.node_key().path().c_str(),
					best->request.node_key().path().size(),
					asin(h.z() / hr) * 180 / M_PI, atan2(h.y(), h.x()) * 180 / M_PI,
					hr, hr - planetoid->radius, best->meters_per_texel);
			} else {
				printf("probe %.0f,%.0f: miss\n", px, py);
			}
			s = strchr(s, ';');
			if (!s) break;
			s++;
		}
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
		if (tunnel_on) tunnelUpdate(cam, dt_ms);
		else if (tube_on && !tube_ground_locked)
			tubeLockGround(true);

		auto &eye = cam.eye;
		auto &direction = cam.direction;
		// airplane mode owns its up vector (roll); otherwise up is gravity,
		// turned about the boresight by the camera's roll. lookAt
		// orthogonalizes, so rotating the world up is enough to bank the
		// picture without touching where the camera is or what it looks at
		auto up = cam.airplane ? cam.body_up
			: Vector3d(AngleAxisd(cam.roll, direction) * eye.normalized());

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

		// the miniature pass needs the clip range to read depth back into
		// meters, and focuses on the terrain under the crosshair
		if (ctx.dof.on) {
			ctx.dof.near = (float)near;
			ctx.dof.far = (float)far;
			ctx.dof.ortho = cam.ortho;
			auto focus = fmax(1.0, centerDistanceMesh(cam));
			ctx.dof.focus = (float)focus;
			// the host's sharp band is a fraction of the focus distance *at
			// the default 45-degree fov*. what the eye judges is how much of
			// the frame is sharp, and the depth a frame spans scales with its
			// angular height — so scale the band with it, and zooming (Z/X)
			// keeps the look. in ortho the same quantity is the half-extent
			// over the focus distance
			auto half_angle = cam.ortho ? ortho_half_extent / focus : tan(fov / 2.0);
			ctx.dof.range = ctx.dof.range_host * (float)(half_angle / tan(0.125 * M_PI));
		}

		auto t0 = ticksMs();

		auto view = lookAt(eye, eye + direction, up);
		Matrix4d viewprojection = projection * view;

		auto frustum_planes = getFrustumPlanes(viewprojection); // for obb culling

		// the down-ray of the terrain-hug ground column (cast from above any
		// building, like groundRadiusUnder's)
		Vector3d col_origin = Vector3d::Zero(), col_dir = -Vector3d::UnitZ();
		if (ground_column_on) {
			Vector3d cup = ground_column_point.normalized();
			col_origin = cup * (ground_column_point.norm() + 2000.0);
			col_dir = -cup;
		}

		// the camera folded into the rect-local frame, for tube-mode lod
		// distances (the camera flies inside the rolled tube, so its local
		// position and view direction are already on the rolled side of
		// the warp)
		Vector3d tube_eye_local = Vector3d::Zero();
		Vector3d tube_dir_local = Vector3d::UnitX();
		if (tube_on) {
			tube_eye_local = (tube_local_from_globe
				* Vector4d(eye.x(), eye.y(), eye.z(), 1.0)).head<3>();
			tube_dir_local = tube_local_from_globe.block<3, 3>(0, 0) * direction;
		}

		const std::string octs[] = { "0", "1", "2", "3", "4", "5", "6", "7" };
		std::vector<std::pair<std::string, rocktree_t::bulk_t *>> valid = { std::make_pair("", current_bulk) };
		decltype(valid) next_valid;
		std::vector<double> next_want; // tube mode: potential_want's counterpart for next_valid

		// downloaded nodes and bulk metadata are both lru caches with byte
		// quotas (see the eviction sweep below); nothing is ever dropped on
		// a clock
		auto now_ms = ticksMs();
		auto prev_walk_ms = last_walk_ms;
		last_walk_ms = now_ms;

		// wanted bulks/nodes are marked by stamping last_wanted_ms with this
		// frame's time during the walk; membership tests elsewhere (eviction,
		// bulk purge) compare against the stamp instead of consulting a map
		auto potential_bulk_count = 0;
		bool all_loaded = true;

		// is last frame's walk still the answer? (see the members)
		auto epoch = g_tree_epoch.load();
		auto cacheable = !tube_on && !tunnel_on;
		auto reuse = cacheable && walk_cache_valid && walk_key_epoch == epoch
			&& walk_key_w == width && walk_key_h == height && walk_key_lod == lod_scale
			&& walk_key_column == ground_column_on && walk_key_eye == eye
			&& walk_key_vp == viewprojection;
		if (reuse) {
			for (auto &kv : potential_nodes) kv.second->last_wanted_ms = now_ms;
			for (auto b : walk_bulks) b->last_wanted_ms = now_ms;
			potential_bulk_count = walk_bulk_count;
			stat_bulk_blocked = walk_bulk_blocked;
			if (walk_bulk_blocked) all_loaded = false;
		} else {
		potential_nodes.clear();
		potential_want.clear();
		to_download_bulks.clear();
		walk_bulks.clear();
		stat_bulk_blocked = 0; // counted over this frame's walk only

		// node culling and level of detail using breadth-first search
		for (;;) {
			auto level_pot_start = potential_nodes.size();
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
					walk_bulks.push_back(b);
					if (b->dl_state == dl_state_stub) {
						// ranked and issued after the walk, like nodes. the
						// yardstick is the node this bulk hangs under — the
						// subtree's own apparent size — so the bulks under
						// what you are looking at resolve first and the view
						// refines outward from there instead of every subtree
						// in the frustum competing at once
						double pri = 1e300;
						auto head = bulk->nodes.find(rel);
						if (head != bulk->nodes.end()) {
							auto radius = head->second->obb.extents.norm();
							auto dist = fmax(0.0, (head->second->obb.center - eye).norm() - radius);
							if (radius > 0) pri = dist / (2 * radius);
						}
						to_download_bulks.push_back({ pri, cur.size(), b });
					}
					if (b->dl_state != dl_state_downloaded) {
						// the walk cannot see past a bulk it doesn't have: this
						// is a whole 4-level subtree that stays coarse until it
						// lands, which is what a deep zoom is waiting on
						stat_bulk_blocked++;
						all_loaded = false;
						continue;
					}
					bulk = b;
				}
				bulk->last_wanted_ms = now_ms;
				walk_bulks.push_back(bulk);
				potential_bulk_count++;

				for(auto o : octs) {
					cnt_oct++;
					auto nxt = cur + o;
					auto nxt_rel = nxt.substr (floor((nxt.size() - 1) / 4) * 4, 4);
					// EARTH_TRACE=<path>: log every walk decision on the
					// prefixes of one octant path, to see where and why the
					// descent toward it stops
					static const char *trace = getenv("EARTH_TRACE");
					auto traced = trace && strncmp(trace, nxt.c_str(), nxt.size()) == 0;
					auto node_kv = bulk->nodes.find(nxt_rel);
					if (node_kv == bulk->nodes.end()) { // node at "nxt" doesn't exist
						if (traced) printf("trace %s: no node in bulk\n", nxt.c_str());
						continue;
					}
					auto node = node_kv->second.get();

					// cull outside frustum using obb
					// todo: check if it could cull more
					cnt_cull++;
					// ground-column nodes skip culling and the lod cutoff:
					// the ground query wants the finest mesh under the
					// camera regardless of where the view points
					auto in_column = ground_column_on
						&& rayHitsObb(col_origin, col_dir, node->obb, INFINITY);
					Vector3d tube_c = Vector3d::Zero(); // node center in the rect-local frame
					if (tube_on)
						tube_c = (tube_local_from_globe * Vector4d(
							node->obb.center.x(), node->obb.center.y(), node->obb.center.z(), 1.0)).head<3>();
					// tube mode culls against the rect, not the view: the
					// whole slab is drawn wherever the camera looks. body
					// mode keeps the land around the figure too — the rect
					// cut out of it, since that sheet is up on the skin — so
					// there a node is wanted if it touches the rect (drawn
					// wrapped) or is in view (drawn flat); a straddler is
					// drawn both ways
					auto in_rect = tube_on && tubeRectTouches(node->obb.extents.norm(), tube_c);
					auto in_view = false;
					if (in_column) {
					} else if (tube_on && !tube_body) {
						if (!in_rect) continue;
					} else {
						if (obb_frustum_outside == classifyObbFrustum(&node->obb, frustum_planes)) {
							if (traced) printf("trace %s: frustum culled\n", nxt.c_str());
						} else if (obbOccluded(eye, node->obb, planet_radius)) {
							if (traced) printf("trace %s: horizon culled\n", nxt.c_str());
						} else {
							in_view = true;
						}
						if (!in_view && !in_rect) continue;
					}
					cnt_lod++;

					// level of detail: the ground column always descends — it
					// wants the finest mesh there regardless of the camera
					// view. tube mode descends by distance measured through
					// the roll (below), with an unconditional floor one level
					// past tube_lock_mpt so tubeLockGround's strict coverage
					// test can always be satisfied and the roll animation has
					// decent imagery over the whole rect
					// EARTH_LOD_SCALE=n: draw n-times coarser than the
					// standard target (lod experiment knob)
					// draw n-times coarser than the standard target. the env
					// var seeds it for native runs; the web shell sets it live
					// (see setLodScale) because the volume of data a view asks
					// for is the one thing that decides whether a deep zoom can
					// finish at all, and that is worth being able to feel
					if (lod_scale <= 0) lod_scale = getenv("EARTH_LOD_SCALE")
						? atof(getenv("EARTH_LOD_SCALE")) : 1.0;
					// not for the tunnel: it has no lock to cover, and its rect
					// can be hundreds of km² — at 20 km around the floor alone
					// was more tiles than the cap allows, and the fine tiles in
					// front of the camera were what got trimmed. distance
					// through the roll decides all of it there
					auto tube_floor = tube_on && !tunnel_on && in_rect
						&& node->meters_per_texel > tube_lock_mpt * 0.5;
					// the tunnel has a floor of its own, by size: a tile too
					// coarse to roll isn't drawn (see the draw loop), so where
					// distance alone stops at one — down the pipe, a few
					// circumferences out — the walk keeps going until the
					// tiles are small enough. children are about half their
					// parent, so wanting every child over 1.25 R is wanting
					// the children of anything over ~2.5 R, inside the 3 R
					// the draw allows with a margin for uneven splits. sized
					// by the radius, it's a few hundred tiles at any
					// circumference
					if (tunnel_on && in_rect && node->obb.extents.norm() > 1.25 * tube_radius)
						tube_floor = true;
					auto tube_want = 1e30; // floor/column: never shed
					// the download order's default (see sched_pri below)
					node->sched_pri = fmax(0.0, (node->obb.center - eye).norm()
						- node->obb.extents.norm()) / (2 * node->obb.extents.norm());
					if (!in_column && !tube_floor) {
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
							// mode through the warp — both endpoints on the
							// rolled side, so lod tracks proximity in the tube
							// as flown (the wall overhead is near, the far end
							// of the axis is far)
							// (the land around the figure is flat, so a
							// straddler takes the nearer of the two)
							double dist = (eye - node->obb.center).norm();
							if (tube_on && in_rect) {
								Vector3d w = (tube_body ? bodyWarp(tube_c) : tubeWarp(tube_c)) - tube_eye_local;
								auto straight = dist;
								dist = w.norm();
								// can the node's rolled position be on screen? a
								// cone test widened by the node's own angular size
								auto in_cone = true;
								if (dist > 1e-6) {
									auto half_diag = atan(tan(cam.fov / 2.0)
										* sqrt(1.0 + aspect_ratio * aspect_ratio));
									auto node_ang = asin(fmin(1.0,
										node->obb.extents.norm() / dist));
									auto cos_ang = w.dot(tube_dir_local) / dist;
									in_cone = cos_ang >= cos(fmin(M_PI, half_diag + node_ang));
								}
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
								if (in_view) dist = fmin(dist, straight);
								// and the download order sees the same
								// distance: ranked by straight-line distance,
								// the wall in front of the camera — ground a
								// long way off, flat — queued behind tiles
								// under and behind it that weren't on screen,
								// and turning the tunnel on went blurry for
								// seconds. what can't be on screen goes last
								node->sched_pri = dist / (2 * node->obb.extents.norm())
									* (in_cone ? 1.0 : 16.0);
							}
							auto t = Affine3d().Identity();
							t.translate(eye + dist * direction);
							auto m = viewprojection * t;
							auto s = m(3, 3);
							r = (2.0*(1.0/s)) * wh * zoom;
						}
						r /= lod_scale;
						if (traced) printf("trace %s: lod tpm %.6f r %.6f -> %s (state %d)\n",
							nxt.c_str(), texels_per_meter, r,
							texels_per_meter > r ? "stop" : "descend", (int)node->dl_state.load());
						if (texels_per_meter > r) continue;
						// how far past its target this node was requested,
						// scale-free (1 = barely wanted)
						tube_want = r * node->meters_per_texel;
					}

					next_valid.push_back(std::make_pair(nxt, bulk));
					if (tube_on) next_want.push_back(tube_want);

					if (node->can_have_data) {
						if (node->last_wanted_ms != prev_walk_ms) node->first_wanted_ms = now_ms;
						node->last_wanted_ms = now_ms;
						potential_nodes.emplace_back(std::move(nxt), node);
						if (tube_on) potential_want.push_back(tube_want);
					}
				}
			}
			if (next_valid.size() == 0) break;
			// node-budget backstop: distance lod bounds the set on its own for
			// any sane rect (a floor level over the rect plus fine rings near
			// the camera), but a degenerate rect or heavy zoom can overflow
			// it. shed this level's least-wanted additions instead of breaking
			// off the whole walk: a uniform break coarsens exactly the tiles
			// with the highest demand (the zoomed-at building loses its lod as
			// the fov narrows — backwards), while a want-ordered trim degrades
			// everything evenly relative to its own target. reordering within
			// one bfs level is fine: the draw loop only needs whole levels in
			// order (children after parents when read backwards)
			const size_t tube_node_cap = 3000;
			if (tube_on && potential_nodes.size() > tube_node_cap) {
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
				decltype(next_valid) nv;
				std::vector<double> nw;
				for (size_t i = 0; i < next_valid.size(); i++)
					if (next_want[i] >= cutoff) {
						nv.push_back(std::move(next_valid[i]));
						nw.push_back(next_want[i]);
					}
				next_valid = std::move(nv);
				next_want = std::move(nw);
				if (next_valid.size() == 0) break;
			}
			valid = next_valid;
			next_valid.clear();
			next_want.clear();
		}
		walk_bulk_count = potential_bulk_count;
		walk_bulk_blocked = stat_bulk_blocked;
		walk_cache_valid = cacheable;
		walk_key_epoch = epoch;
		walk_key_w = width; walk_key_h = height; walk_key_lod = lod_scale;
		walk_key_column = ground_column_on; walk_key_eye = eye;
		walk_key_vp = viewprojection;
		} // end of the walk
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
			// small on purpose: see bulks_in_flight. eight is enough to keep
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
			stat_bulks_started = 0;
			for (auto &c : to_download_bulks) {
				if (bulks_in_flight >= max_bulks_in_flight) break;
				// (a reused walk's candidates include ones already requested)
				if (c.bulk->dl_state != dl_state_stub) continue;
				if (!retry_ready(c.bulk)) continue;
				bulks_in_flight++;
				stat_bulks_started++;
				c.bulk->dl_next_try_ms = now_ms + retry_delay_ms(c.bulk->dl_fails);
				c.bulk->setStartedDownloading();
				getBulk(c.bulk->request, c.bulk, [this](auto) { bulks_in_flight--; });
			}

			struct candidate_t { double priority; size_t level; rocktree_t::node_t *node; };
			std::vector<candidate_t> to_download;
			stat_nodes_wanted = (int)potential_nodes.size();
			stat_nodes_loaded = stat_nodes_stub = stat_nodes_stuck = 0;
			stat_nodes_failed = stat_max_level = 0;
			pending_nodes.clear();
			for (auto &kv : potential_nodes) {
				auto node = kv.second;
				if ((int)kv.first.size() > stat_max_level) stat_max_level = (int)kv.first.size();
				if (node->dl_state != dl_state_downloaded) {
					all_loaded = false;
					pending_nodes.push_back({ kv.first, (int)node->dl_state.load(), node->dl_fails,
						(node->obb.center - eye).norm(), node->meters_per_texel, node->first_wanted_ms });
				}
				else stat_nodes_loaded++;
				if (node->dl_state == dl_state_stub) stat_nodes_stub++;
				else if (node->dl_state == dl_state_downloading) stat_nodes_stuck++;
				if (node->dl_fails) stat_nodes_failed++;
				if (node->dl_state != dl_state_stub) continue;
				if (!retry_ready(node)) continue; // waiting out a failure
				auto radius = node->obb.extents.norm();
				auto dist = fmax(0.0, (node->obb.center - eye).norm() - radius);
				// (in tube mode the walk measured the distance through the
				// roll, which is what proximity means there)
				to_download.push_back({ tube_on ? node->sched_pri : dist / (2 * radius), kv.first.size(), node });
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
		std::unordered_map<std::string, rocktree_t::node_t *> potential_set;
		if (debug_lod)
			for (auto &kv : potential_nodes) potential_set[kv.first] = kv.second;

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
				(float)r_eff, (float)tube_half_len, (float)tube_half_wid, (float)tube_wid_offset,
				(float)tunnelFade());
		} else {
			renderSetTube(ctx, false, nullptr, 0, 0, 0, 0);
		}
		// the body's geometry image goes up on the gl thread, here, the
		// first draw after the host bakes it
		if (body_dirty && body_w > 1 && body_h > 1) {
			renderBodyUpload(ctx, body_w, body_h, body_pos.data(), body_nrm.data());
			body_dirty = false;
		}
		float bands[4] = {(float)body_bands[0], (float)body_bands[1], (float)body_bands[2], (float)body_bands[3]};
		renderSetBody(ctx, tube_on && tube_body && body_w > 1 && body_h > 1,
			(float)fmax(0.0, fmin(tube_curl, 1.0)), bands);

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
			// the tunnel rolls whatever is resident, and a tile that is
			// coarse against the roll's radius can't be rolled: its
			// triangles span a large arc, the shader's rect clip is
			// interpolated straight across them while the warp is not, and
			// a lake tile came out as a slab across half the view. the rect
			// tube avoids this by staying flat until fine coverage locks;
			// the tunnel re-enters coarse land every time it moves, so it
			// leaves a hole there until the fine tile lands instead. the
			// parent's mask bit is already set above, so no ancestor fills
			// the hole with its own slab
			if (tunnel_on && node->obb.extents.norm() > 3.0 * tube_radius) continue;

			drawn_nodes.push_back({ node, self_mask });

			// octants where a finer tile is wanted but not drawn, and why:
			// three bits per octant saying where that tile's request is
			// (the shader's legend — 1 not yet requested, 2 cache lookup,
			// 3 network, 4 decoding, 5 waiting to retry after a failure).
			// children are drawn before parents here, so mask_map for this
			// node is already complete
			uint32_t stale_mask = 0;
			if (debug_lod) {
				for (auto o = 0; o < 8; o++) {
					if (self_mask & (1 << o)) continue;
					auto it = potential_set.find(full_path + octs[o]);
					if (it == potential_set.end()) continue;
					auto child = it->second;
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
			// body mode: the land first, flat — the rect's own ground painted
			// black in the shader, where the sheet lifted off — then the
			// wrapped pass for whatever touches the rect
			if (tube_on && tube_body) {
				auto margin = node->obb.extents.norm();
				Vector3d c = (tube_local_from_globe * Vector4d(
					node->obb.center.x(), node->obb.center.y(), node->obb.center.z(), 1.0)).head<3>();
				renderSetTubePass(ctx, 2);
				for (auto &mesh : node->meshes) {
					if (!mesh.buffered) bufferMesh(mesh, ctx);
					bindAndDrawMesh(mesh, self_mask, stale_mask, ctx);
				}
				if (!tubeRectTouches(margin, c)) continue;
				renderSetTubePass(ctx, 1);
			}
			for (auto &mesh : node->meshes) {
				if (!mesh.buffered) bufferMesh(mesh, ctx);
				bindAndDrawMesh(mesh, self_mask, stale_mask, ctx);
			}
		}

		// the dropped track, over the terrain it was walked on. after the
		// meshes, so the depth test it loses against is a complete one; not
		// in tube mode, where the ground has been rolled out from under it
		// and a track drawn in globe space would hang in the air where the
		// terrain used to be
		if (!path_up.empty() && !tube_on && path_rgba[3] > 0) {
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
			Matrix4d to_clip = viewprojection;
			to_clip.col(3) = viewprojection * Vector4d(path_origin.x(), path_origin.y(), path_origin.z(), 1.0);
			Matrix4f to_clip_f = to_clip.cast<float>();
			renderPathDraw(ctx, to_clip_f.data(), path_rgba, path_width, path_depth_bias, width, height);
			if (path_cursor_on) {
				// the path's own color, exactly: the disc is the track's
				// position marker, not a second thing sitting on it
				Vector3f c = (path_cursor - path_origin).cast<float>();
				renderPathCursorDraw(ctx, to_clip_f.data(), path_rgba,
					fmaxf(11.0f, path_width * 3.0f), path_depth_bias, width, height, c.data());
			}
		}

		static const bool audit_env = getenv("EARTH_AUDIT") != nullptr;
		if (audit_env && scene_complete && !audited) {
			audited = true;
			auditDrawnNodes(viewprojection, width, height);
		}

		sec_draw += ticksMs() - t3;
		sec_frames++;
		sec_report_ms += dt_ms;
		if (sec_report_ms > 2000 && sec_frames > 0) {
			sec_report_ms = 0;
			avg_bfs = sec_bfs / sec_frames; avg_dl = sec_dl / sec_frames;
			avg_evict = sec_evict / sec_frames; avg_draw = sec_draw / sec_frames;
			avg_oct = cnt_oct / sec_frames; avg_cull = cnt_cull / sec_frames;
			avg_lod = cnt_lod / sec_frames;
			avg_fps = sec_frames / 2;
			printf("sections avg ms: bfs %.2f, dl %.2f, evict %.2f, draw %.2f (%d frames; per frame: oct %ld, cull %ld, lod %ld)\n",
				avg_bfs, avg_dl, avg_evict, avg_draw,
				sec_frames, avg_oct, avg_cull, avg_lod);
			// what the view is still waiting for. the section above times the
			// cpu work; this says whether anything is actually coming. a view
			// that never completes with waiting=0 and inflight=0 is not slow,
			// it is stuck: those nodes asked once and nothing answered
			printf("tiles: want %d loaded %d waiting %d inflight %d downloading %d retrying %d"
				" · deepest level %d · bulks: blocked %d inflight %d started %d%s\n",
				stat_nodes_wanted, stat_nodes_loaded, stat_nodes_stub,
				(int)nodes_in_flight, stat_nodes_stuck, stat_nodes_failed,
				stat_max_level, stat_bulk_blocked, (int)bulks_in_flight,
				stat_bulks_started, scene_complete ? " · complete" : "");
			sec_bfs = sec_dl = sec_evict = sec_draw = 0;
			cnt_oct = cnt_cull = cnt_lod = 0;
			sec_frames = 0;
		}
	}
};
