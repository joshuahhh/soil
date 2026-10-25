// the tube, tunnel and body modes: the rect's frame, the warps and their cpu
// twins, the ground lock, and what the lod walk and draw loop ask of them.
// the engine is reached only through tube_engine_t.

#pragma once

#include <algorithm>
#include <functional>
#include <math.h>
#include <vector>
#include "rocktree_util.h"
#include "earth_camera.h"

// filled in by earth_core_t. mesh_hit: true on a mesh hit, with its meters
// per texel; ground_radius_under: -1 when it doesn't know
struct tube_engine_t {
	std::function<double()> planet_radius;
	std::function<bool(const Vector3d &origin, const Vector3d &dir, Vector3d &hit, double &mpt)> mesh_hit;
	std::function<double(const Vector3d &p)> ground_radius_under;
};

struct tube_t {
	tube_engine_t engine;

	// tube mode: a geodetic rectangle of terrain is rolled into a cylinder in
	// the vertex shader (the inception effect). selection switches from
	// frustum culling + camera-driven lod to "every node whose obb touches
	// the rect, finest resolution available" — the rect is fixed, so there is
	// no point chasing the camera. curl animates flat slab (0) to fully
	// closed tube (1); the roll radius is chosen so the rect's width is
	// exactly the closed tube's circumference
	bool on = false;
	double curl = 1.0;
	double radius = 0; // fully-curled tube radius (m)
	double half_len = 0, half_wid = 0; // rect half extents (m)
	// where the wrapped range sits across the axis: the rect spans y in
	// [offset - half_wid, offset + half_wid]. zero for a drawn rect (the
	// seam straight up over its centre line); the tunnel's across mode
	// sets a quarter circumference, which puts the seam level behind you
	double wid_offset = 0;
	double ground_radius = 0; // ecef radius of the ground at the rect center
	bool ground_locked = false; // true once measured from the loaded mesh
	bool ew_axis = true; // tube axis east-west (else north-south)
	Vector3d up = Vector3d::UnitZ(), axis = Vector3d::UnitX();
	Matrix4d globe_from_local = Matrix4d::Identity();
	Matrix4d local_from_globe = Matrix4d::Identity();

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
	bool body = false;
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
		auto R_p = engine.planet_radius();
		Vector3d eye_up = cam.eye.normalized();
		Vector3d h = cam.direction - eye_up * cam.direction.dot(eye_up);
		if (h.norm() < 1e-6 && tunnel_heading_set) h = tunnel_heading - eye_up * tunnel_heading.dot(eye_up); // straight down: keep the old heading
		if (h.norm() < 1e-6) h = eye_up.cross(Vector3d::UnitZ());
		h.normalize();
		// the low-pass: ease the followed heading toward the camera's,
		// keeping it in the tangent plane. a 180° turn passes through
		// zero length, so a vanished vector snaps to the new heading
		if (tunnel_heading_set && tunnel_lag_ms > 0) {
			Vector3d hd = tunnel_heading - eye_up * tunnel_heading.dot(eye_up);
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
		auto g = engine.ground_radius_under(cam.eye);
		if (g > 0) {
			if (!tunnel_ground_measured) { tunnel_ground_radius = g; tunnel_ground_measured = true; }
			else tunnel_ground_radius += (g - tunnel_ground_radius) * (1.0 - exp(-dt_ms / 250.0));
		}
		up = eye_up;
		axis = tunnel_across ? h.cross(eye_up).normalized() : h;
		ew_axis = fabs(axis.z()) < 0.7; // roughly east-west, for the map's arrows
		half_wid = C / 2;                 // across the axis: what wraps
		// along it. looking down the pipe its far end is in view, so it
		// runs a long way (tunnel_length) and fades to black (tunnelFade) —
		// at 1.5 C the open end was a wide disc of sky a quarter of the way
		// across the view; at 10 C it is a small dark spot
		half_len = fmax(500.0, (tunnel_across ? 1.5 : fmax(1.0, tunnel_length)) * C);
		// across mode: the seam level behind you (theta = -90°), so the
		// wrapped range runs from a quarter circumference behind to three
		// quarters ahead. along the axis it stays overhead
		wid_offset = tunnel_across ? C / 4 : 0;
		radius = fmax(1.0, half_wid / M_PI);
		ground_radius = tunnel_ground_radius;
		ground_locked = true;
		tubeRebuild();
	}

	// where the walls start going dark, in rect units from the camera
	// along the axis (the rect is centred on it): only looking down the
	// pipe, where its ends are what you see
	double tunnelFade() const {
		return on && tunnel_on && !tunnel_across ? 0.15 : 0.0;
	}

	// how far the sky is dimmed to black, 0..1: inside the closed tunnel
	// down the heading the only sky in view is through the pipe's two
	// ends, and blue there undoes the fade. eased in with the curl, and
	// only while the camera is inside (below the roof, 2R above the floor)
	double tunnelSkyDark(const Vector3d &eye) const {
		if (tunnelFade() <= 0) return 0;
		if (eye.norm() - ground_radius > 2 * radius) return 0;
		auto c = fmax(0.0, fmin(curl, 1.0));
		return c * c;
	}

	// the roll's radius at the current curl: the closed tube's, growing as
	// curl shrinks (flat at curl 0), floored so it stays finite
	double tubeRollRadius() const { return radius / fmax(curl, 1e-3); }

	void tubeRebuild() {
		Matrix4d g = Matrix4d::Identity();
		g.block<3,1>(0,0) = axis;
		g.block<3,1>(0,1) = up.cross(axis);
		g.block<3,1>(0,2) = up;
		g.block<3,1>(0,3) = up * ground_radius;
		globe_from_local = g;
		local_from_globe = g.inverse();
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
	void setTubeRect(double lat0, double lon0, double lat1, double lon1, bool ew) {
		auto R_p = engine.planet_radius();
		auto latc = (lat0 + lat1) / 2, lonc = (lon0 + lon1) / 2;
		auto ns_half = R_p * fabs(lat1 - lat0) / 2;
		auto ew_half = R_p * cos(latc) * fabs(lon1 - lon0) / 2;
		up = Vector3d(cos(latc) * cos(lonc), cos(latc) * sin(lonc), sin(latc));
		Vector3d east(-sin(lonc), cos(lonc), 0);
		Vector3d north = up.cross(east);
		ew_axis = ew;
		axis = ew ? east : north;
		half_len = ew ? ew_half : ns_half;
		half_wid = ew ? ns_half : ew_half;
		radius = fmax(1.0, half_wid / M_PI); // 2*half_wid = 2*pi*R
		wid_offset = 0;
		ground_locked = false;
		// seed at the sphere radius: measured (straight-down mesh probes at
		// many levels and latitudes) the mesh datum IS the planetoid sphere,
		// not the wgs84 ellipsoid — mesh hits come in at sphere + terrain
		// height. the real zero-point still comes from tubeLockGround()
		ground_radius = R_p;
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
		if (!on || radius <= 0) return false;
		auto R_eff = tubeRollRadius();
		Vector3d l = transformPoint(local_from_globe, eye);
		Vector3d d = (local_from_globe
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
		auto latc = asin(fmax(-1.0, fmin(1.0, up.z())));
		auto lonc = atan2(up.y(), up.x());
		Vector3d east(-sin(lonc), cos(lonc), 0);
		Vector3d north = up.cross(east);
		Vector3d across = up.cross(axis);
		auto dE = x * axis.dot(east) + y * across.dot(east);
		auto dN = x * axis.dot(north) + y * across.dot(north);
		auto hE = dx * axis.dot(east) + dy * across.dot(east);
		auto hN = dx * axis.dot(north) + dy * across.dot(north);
		auto R_p = engine.planet_radius();
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
		auto S = 2.0 * half_len;
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
		auto u = (l.y() - wid_offset) / (2.0 * half_wid) + 0.5;
		auto v = l.x() / (2.0 * half_len) + 0.5;
		Vector3d p, n;
		bodyTexel((int)floor(u * body_w), (int)floor(v * body_h), p, n);
		Vector3d w = bodyToLocal(p) + bodyDirToLocal(n) * l.z();
		return l + (w - l) * fmax(0.0, fmin(curl, 1.0));
	}
	// tubePose for the body: the texel whose skin is nearest the camera,
	// unwrapped to its spot on the rect. a coarse scan of the image and a
	// refinement around the best hit; the heading is the camera's own
	bool bodyPose(const Vector3d &eye, const Vector3d &direction,
			double &lat, double &lon, double &heading) {
		if (!on || !body || body_w < 2 || body_h < 2) return false;
		Vector3d l = transformPoint(local_from_globe, eye);
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
		auto x = (v - 0.5) * 2.0 * half_len;
		auto y = wid_offset + (u - 0.5) * 2.0 * half_wid;
		auto latc = asin(fmax(-1.0, fmin(1.0, up.z())));
		auto lonc = atan2(up.y(), up.x());
		Vector3d east(-sin(lonc), cos(lonc), 0);
		Vector3d north = up.cross(east);
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
		auto R = tubeRollRadius();
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
		return !(fabs(c.x()) > half_len + margin
			|| fabs(c.y() - wid_offset) > half_wid + margin
			|| c.z() > 9000 + margin || c.z() < -1500 - margin);
	}

	// the rolled tube's cylindrical coordinate system, for tube terrain hug
	// (see earth_web fly): axial coordinate x along the tube axis, angle
	// theta around it (0 at the wall's bottom, radial_l(theta) =
	// (0, sin, -cos) in the rect-local frame), radial distance rho from the
	// axis. the frame is right-handed with local +x = axis, so a
	// rotation by dtheta about axis in globe space is exactly the
	// transport that carries the frame at theta to the frame at theta+dtheta
	struct tube_cyl_t {
		double x, theta, rho;
		Vector3d radial; // outward (toward the wall) unit radial, globe frame
	};
	tube_cyl_t tubeCyl(const Vector3d &p) const {
		tube_cyl_t c;
		auto R = tubeRollRadius();
		Vector3d l = transformPoint(local_from_globe, p);
		c.x = l.x();
		c.theta = atan2(l.y(), R - l.z());
		c.rho = Vector2d(l.y(), l.z() - R).norm();
		Vector3d radial_l(0.0, sin(c.theta), -cos(c.theta));
		c.radial = globe_from_local.block<3, 3>(0, 0) * radial_l;
		return c;
	}

	// globe position of cylindrical (x, theta, rho)
	Vector3d tubeCylPoint(double x, double theta, double rho) const {
		auto R = tubeRollRadius();
		return transformPoint(globe_from_local, Vector3d(x, rho * sin(theta), R - rho * cos(theta)));
	}

	// the wall's radial distance at (x, theta): unroll the angle back onto
	// the rect (the meshes are stored unrolled; only the shader rolls them)
	// and raycast straight down from above any terrain, like tubeLockGround's
	// probes. theta wraps so walking around a closed tube's seam continues
	// seamlessly. false while the mesh there is missing or coarse filler
	// (same gate as groundRadiusUnder): better to hold and wait than glide
	// toward a surface that sits tens of meters off
	bool tubeWallRho(double x, double theta, double &rho_wall) {
		if (!on || radius <= 0) return false;
		auto R = tubeRollRadius();
		auto y = wid_offset + remainder(theta - wid_offset / R, 2.0 * M_PI) * R;
		Vector3d origin = transformPoint(globe_from_local, Vector3d(x, y, 10000.0));
		Vector3d hit;
		double mpt;
		if (!engine.mesh_hit(origin, -up, hit, mpt)) return false;
		if (mpt > 10.0) return false;
		auto z_ground = transformPoint(local_from_globe, hit).z();
		rho_wall = fmax(R - z_ground, 0.05 * R); // the warp's own clamp
		return true;
	}

	// the tunnel's roof straight over the camera, as a globe radius: the
	// camera sits at the tube's bottom (x = 0, theta = 0) and overhead is
	// the wall at theta = pi — the ground there and whatever hangs from it.
	// not measurable yet: the roof of flat ground. false until it closes,
	// when there is no roof to be under
	bool tunnelCeilingRadius(double &r) {
		if (!on || !tunnel_on || curl < 0.999) return false;
		auto R = tubeRollRadius();
		double rho;
		if (!tubeWallRho(0, M_PI, rho)) rho = R;
		r = ground_radius + R + rho;
		return true;
	}

	// terrain elevation converges several lod levels before the imagery
	// does: mesh at this resolution already carries the ground to within a
	// few meters, so the roll can start ~two orders of magnitude fewer tiles
	// in, and full res streams into the rolled tube afterwards
	static constexpr double lock_mpt = 4.0;

	// the elevation analysis: raycast a 7x7 grid over the rect (cpu, against
	// the resident meshes) and take the median hit radius as the zero-point
	// — robust to a roof or treetop at any one sample. strict mode (retried
	// every frame while loading) only locks once every sample hits mesh
	// finer than lock_mpt — that coverage test needs no download
	// bookkeeping. non-strict is the host's timeout escape hatch: take the
	// median of whatever is resident. the 0.9 keeps samples off the edges
	void tubeLockGround(bool strict) {
		std::vector<double> rs;
		Vector3d across = up.cross(axis);
		for (auto i = 0; i < 7; i++) {
			for (auto j = 0; j < 7; j++) {
				Vector3d p = up * (ground_radius + 10000.0)
					+ axis * ((i / 3.0 - 1.0) * 0.9 * half_len)
					+ across * (wid_offset + (j / 3.0 - 1.0) * 0.9 * half_wid);
				Vector3d hit;
				double mpt;
				if (!engine.mesh_hit(p, -up, hit, mpt)) {
					if (strict) return; // a sample has no mesh yet
					continue;
				}
				if (strict && mpt > lock_mpt) return; // only coarse mesh here yet
				rs.push_back(hit.norm());
			}
		}
		if (rs.empty()) return; // nothing resident at all; retried next frame
		std::nth_element(rs.begin(), rs.begin() + rs.size() / 2, rs.end());
		ground_radius = rs[rs.size() / 2];
		ground_locked = true;
		tubeRebuild();
	}

	// --- the lod walk's questions -----------------------------------------

	// the camera folded into the rect-local frame, for tube-mode lod
	// distances (the camera flies inside the rolled tube, so its local
	// position and view direction are already on the rolled side of
	// the warp)
	struct walk_view_t {
		Vector3d eye = Vector3d::Zero();
		Vector3d dir = Vector3d::UnitX();
	};
	walk_view_t walkView(const Vector3d &eye, const Vector3d &direction) const {
		walk_view_t v;
		if (on) {
			v.eye = transformPoint(local_from_globe, eye);
			v.dir = local_from_globe.block<3, 3>(0, 0) * direction;
		}
		return v;
	}

	// a node's centre in the rect-local frame, and whether it reaches the rect
	struct walk_node_t {
		Vector3d c = Vector3d::Zero();
		bool in_rect = false;
	};
	walk_node_t walkNode(const OrientedBoundingBox &obb) const {
		walk_node_t n;
		if (on) {
			n.c = transformPoint(local_from_globe, obb.center);
			n.in_rect = tubeRectTouches(obb.extents.norm(), n.c);
		}
		return n;
	}

	// the walk's unconditional floor in the rect, wanted whatever its
	// distance. the rect tube's is one level past lock_mpt, so
	// tubeLockGround's strict coverage test can always be satisfied and the
	// roll animation has decent imagery over the whole rect
	// not for the tunnel: it has no lock to cover, and its rect
	// can be hundreds of km² — at 20 km around the floor alone
	// was more tiles than the cap allows, and the fine tiles in
	// front of the camera were what got trimmed. distance
	// through the roll decides all of it there
	bool walkFloor(const rocktree_t::node_t *node, const walk_node_t &n) const {
		auto tube_floor = on && !tunnel_on && n.in_rect
			&& node->meters_per_texel > lock_mpt * 0.5;
		// the tunnel has a floor of its own, by size: a tile too
		// coarse to roll isn't drawn (see tunnelTooCoarse), so where
		// distance alone stops at one — down the pipe, a few
		// circumferences out — the walk keeps going until the
		// tiles are small enough. children are about half their
		// parent, so wanting every child over 1.25 R is wanting
		// the children of anything over ~2.5 R, inside the 3 R
		// the draw allows with a margin for uneven splits. sized
		// by the radius, it's a few hundred tiles at any
		// circumference
		if (tunnel_on && n.in_rect && node->obb.extents.norm() > 1.25 * radius)
			tube_floor = true;
		return tube_floor;
	}

	// the lod distance to a node in the rect, through the warp — both
	// endpoints on the rolled side, so lod tracks proximity in the tube as
	// flown (the wall overhead is near, the far end of the axis is far).
	// a node also in view takes the straight distance if nearer (the land
	// around the figure is flat). sets the node's download order too
	double walkDistance(const walk_view_t &v, const walk_node_t &n, rocktree_t::node_t *node,
			double straight, bool in_view, double fov, float aspect_ratio, bool &in_cone) const {
		Vector3d w = (body ? bodyWarp(n.c) : tubeWarp(n.c)) - v.eye;
		auto dist = w.norm();
		// can the node's rolled position be on screen? a
		// cone test widened by the node's own angular size
		in_cone = true;
		if (dist > 1e-6) {
			auto half_diag = atan(tan(fov / 2.0)
				* sqrt(1.0 + aspect_ratio * aspect_ratio));
			auto node_ang = asin(fmin(1.0,
				node->obb.extents.norm() / dist));
			auto cos_ang = w.dot(v.dir) / dist;
			in_cone = cos_ang >= cos(fmin(M_PI, half_diag + node_ang));
		}
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
		return dist;
	}

	// --- the draw loop's pieces -------------------------------------------

	// tube warp uniforms: the local->clip matrix folds the camera in, so
	// the shader works in small rect-local coordinates end to end. the
	// effective roll radius grows as curl shrinks (flat at curl 0); the
	// floor keeps it finite for the shader
	void setTubeUniforms(render_ctx_t &ctx, const Matrix4d &viewprojection) {
		if (on) {
			Matrix4d l2c = viewprojection * globe_from_local;
			Matrix4f l2cf = l2c.cast<float>();
			auto r_eff = tubeRollRadius();
			renderSetTube(ctx, true, l2cf.data(),
				(float)r_eff, (float)half_len, (float)half_wid, (float)wid_offset,
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
		renderSetBody(ctx, on && body && body_w > 1 && body_h > 1,
			(float)fmax(0.0, fmin(curl, 1.0)), bands);
	}

	// the tunnel rolls whatever is resident, and a tile that is
	// coarse against the roll's radius can't be rolled: its
	// triangles span a large arc, the shader's rect clip is
	// interpolated straight across them while the warp is not, and
	// a lake tile came out as a slab across half the view. the rect
	// tube avoids this by staying flat until fine coverage locks;
	// the tunnel re-enters coarse land every time it moves, so it
	// leaves a hole there until the fine tile lands instead
	bool tunnelTooCoarse(const rocktree_t::node_t *node) const {
		return tunnel_on && node->obb.extents.norm() > 3.0 * radius;
	}

	// a node's mesh -> rect-local frame, composed in double then narrowed:
	// local coordinates are km-scale, safe in float
	void setTubeNode(render_ctx_t &ctx, const Matrix4d &globe_from_mesh) const {
		if (!on) return;
		Matrix4f m2l = (local_from_globe * globe_from_mesh).cast<float>();
		renderSetTubeNode(ctx, m2l.data());
	}

	// body mode: the land first, flat — the rect's own ground painted
	// black in the shader, where the sheet lifted off — then the
	// wrapped pass if the node touches the rect
	template <typename F>
	void drawBodyNode(render_ctx_t &ctx, const OrientedBoundingBox &obb, F draw) const {
		auto margin = obb.extents.norm();
		Vector3d c = transformPoint(local_from_globe, obb.center);
		renderSetTubePass(ctx, 2);
		draw();
		if (!tubeRectTouches(margin, c)) return;
		renderSetTubePass(ctx, 1);
		draw();
	}
};
