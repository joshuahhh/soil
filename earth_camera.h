// camera state and flying controls, independent of any windowing or input
// backend. included after eigen (see earth_web.cpp); part of the platform-free
// core so embedding shells (native window, web canvas) can own and drive the
// camera directly

struct camera_t {
	Vector3d eye;       // ecef meters
	Vector3d direction; // unit
	double fov;         // vertical, radians
	// orthographic projection: the view extent follows what the fov would
	// show at the terrain distance, so mode switches keep the apparent
	// scale and the fov zoom controls keep working
	bool ortho = false;
	// ortho vertical half-extent in meters — explicit state, initialized on
	// entering ortho and scaled by zooms. deriving it per frame from the
	// terrain distance under the boresight made movement pump the zoom
	double ortho_extent = 0;
	// meters of mesh in front of the eye to ignore, both drawn and picked
	// (perspective only). a photo solved from inside a building puts the
	// camera in a room whose walls hide the view the picture actually has
	// through a window; this is the window. 0 = off
	double near_cut = 0;
	// airplane mode (tube flying): the camera owns its up vector instead of
	// deriving it from the planet center, so roll is a real degree of
	// freedom and yaw/pitch happen about the body axes with no horizon
	// clamps. body_up is only meaningful while airplane is set
	bool airplane = false;
	Vector3d body_up = Vector3d::UnitZ();
	// radians about the boresight, for the ordinary ground-frame camera: the
	// image rotates, the controls don't. airplane mode expresses the same
	// freedom through body_up instead (it owns its whole frame), so this is
	// only read when airplane is off. a photo taken by a hand-held camera is
	// almost never level, and matching one is what this is for
	double roll = 0;
};

// (re)level the airplane frame: body up from the planet's up at the current
// position. used on mode entry and after teleports
void alignAirplaneUp(camera_t &cam) {
	auto up = cam.eye.normalized();
	Vector3d right = cam.direction.cross(up);
	if (right.norm() < 1e-9) right = Vector3d::UnitX().cross(cam.direction); // looking straight down
	right.normalize();
	cam.body_up = right.cross(cam.direction).normalized();
}

// geodetic pose on the spherical planetoid — the vocabulary a 2d map speaks.
// angles in radians; alt in meters above the sphere; heading 0 = north,
// clockwise positive; tilt 0 = horizon, -pi/2 = straight down
struct geo_pose_t {
	double lat, lon, alt;
	double heading, tilt;
};

static inline Vector3d geoUp(double lat, double lon) {
	return { cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat) };
}

static inline Vector3d geoNorth(double lat, double lon) {
	return { -sin(lat) * cos(lon), -sin(lat) * sin(lon), cos(lat) };
}

camera_t poseToCamera(const geo_pose_t &p, double planet_radius, double fov) {
	auto up = geoUp(p.lat, p.lon);
	auto north = geoNorth(p.lat, p.lon);
	auto east = north.cross(up);
	auto horizontal = north * cos(p.heading) + east * sin(p.heading);
	camera_t cam;
	cam.eye = up * (planet_radius + p.alt);
	cam.direction = (horizontal * cos(p.tilt) + up * sin(p.tilt)).normalized();
	cam.fov = fov;
	return cam;
}

geo_pose_t cameraToPose(const camera_t &cam, double planet_radius) {
	geo_pose_t p;
	auto r = cam.eye.norm();
	p.lat = asin(cam.eye.z() / r);
	p.lon = atan2(cam.eye.y(), cam.eye.x());
	p.alt = r - planet_radius;
	auto up = geoUp(p.lat, p.lon);
	auto north = geoNorth(p.lat, p.lon);
	auto east = north.cross(up);
	auto vertical = cam.direction.dot(up);
	p.tilt = asin(fmax(-1.0, fmin(1.0, vertical)));
	auto horizontal = cam.direction - up * vertical;
	p.heading = horizontal.norm() > 1e-9 ? atan2(horizontal.dot(east), horizontal.dot(north)) : 0;
	return p;
}

// distance along the boresight to the reference sphere, with the altitude as
// a fallback when the view misses it (looking at the sky). the anchor both
// for the ortho view extent and for ortho zooming, so the two stay in step
double centerDistance(const camera_t &cam, double planet_radius) {
	auto od = cam.eye.dot(cam.direction);
	auto disc = od * od - (cam.eye.dot(cam.eye) - planet_radius * planet_radius);
	auto altitude = cam.eye.norm() - planet_radius;
	auto dist = disc > 0 ? -od - sqrt(disc) : altitude;
	if (!(dist > 0)) dist = fmax(1.0, altitude);
	return dist;
}

// near the horizon an orthographic view stops being meaningful: the parallel
// beam grazes the planet and the derived view extent blows up. keep the tilt
// at least this far below the horizon while in ortho mode
static const double ortho_min_tilt = -10.0 * M_PI / 180.0;

void clampOrthoTilt(camera_t &cam) {
	if (!cam.ortho) return;
	auto up = cam.eye.normalized();
	auto vertical = cam.direction.dot(up);
	if (vertical <= sin(ortho_min_tilt)) return; // already steep enough
	Vector3d horizontal = cam.direction - up * vertical;
	if (horizontal.norm() < 1e-9) return;
	horizontal.normalize();
	cam.direction = horizontal * cos(ortho_min_tilt) + up * sin(ortho_min_tilt);
}

// revolve the camera around a pivot point (typically what's at the center of
// the screen), turning about the pivot's local up. that axis passes through
// the planet center, so the orbit preserves altitude exactly, and the pivot
// itself is invariant: whatever was at the screen center stays there
void orbitCamera(camera_t &cam, const Vector3d &pivot, double angle) {
	AngleAxisd rot(angle, pivot.normalized());
	cam.eye = rot * cam.eye;
	cam.direction = (rot * cam.direction).normalized();
}

// revolve the camera around the pivot in the tilt direction: rotation about
// the horizontal screen-right axis through the pivot, so whatever is at the
// screen center stays there while the view swings between overhead and
// oblique. the angle is clamped against the tilt limits (straight down, and
// the ortho horizon floor) before applying, so hitting a limit stops the
// orbit instead of un-pinning the pivot
void orbitCameraTilt(camera_t &cam, const Vector3d &pivot, double angle) {
	auto up = cam.eye.normalized();
	Vector3d right = cam.direction.cross(up);
	if (right.norm() < 1e-9) return; // looking straight down: right is undefined
	right.normalize();
	auto tilt = asin(fmax(-1.0, fmin(1.0, cam.direction.dot(up))));
	auto shallowest = cam.ortho ? ortho_min_tilt : 0.0;
	const auto steepest = -0.495 * M_PI; // matches the overhead pitch guard
	angle = fmax(steepest, fmin(tilt + angle, shallowest)) - tilt;
	AngleAxisd rot(angle, right);
	cam.eye = pivot + rot * (cam.eye - pivot);
	cam.direction = (rot * cam.direction).normalized();
}

// one frame of the built-in flying controls; shells translate their native
// input events into this and hosts that drive the camera themselves (e.g. a
// synced 2d map) skip it entirely.
//
// two movement frames share these keys (see applyCameraInput): the ground
// frame (cruise holds altitude, pedestal is along gravity-up) and, with
// view_frame set, the view frame (dolly along the boresight, boom along
// view-up). truck (left/right) is identical in both because roll is never
// introduced, so camera-right is always horizontal
struct camera_input_t {
	double yaw = 0, pitch = 0; // pan/tilt radians this frame
	double roll = 0;           // radians this frame; airplane mode only
	bool forward = false, back = false, left = false, right = false;
	bool raise = false, lower = false;
	bool view_frame = false;
	bool slow = false;        // momentary precision (x0.1)
	double speed_gain = 1.0;  // sticky multiplier owned by the shell
	double dt_ms = 0;
	// when set (not nan), used for the altitude-proportional speed instead
	// of the sphere-datum altitude — e.g. true height above the mesh, which
	// the sphere altitude misses by kilometers where the datums diverge
	double altitude_override = NAN;
};

void applyCameraInput(camera_t &cam, const camera_input_t &in, double planet_radius) {
	// up is the vec from the planetoid's center towards the sky
	auto up = cam.eye.normalized();

	// zooming magnifies apparent motion, so rotation and the
	// altitude-proportional speed scale down with it, keeping screen-space
	// rates constant. 1 at the default 45-degree fov
	auto zoom_scale = tan(cam.fov / 2.0) / tan(0.125 * M_PI);

	// airplane mode: yaw/pitch/roll about the camera's own axes and all
	// movement in the body frame. no horizon or overhead clamps — loops and
	// rolls are the point. axis signs chosen to match the ground-frame
	// controls when flying level
	if (cam.airplane) {
		Vector3d right = cam.direction.cross(cam.body_up);
		if (right.norm() < 1e-9) right = cam.direction.cross(up);
		if (right.norm() < 1e-9) right = Vector3d::UnitX().cross(cam.direction);
		right.normalize();
		cam.body_up = right.cross(cam.direction).normalized();
		auto quat = AngleAxisd(in.roll * zoom_scale, cam.direction)
			* AngleAxisd(in.yaw * zoom_scale, cam.direction.cross(right)) // = -body_up
			* AngleAxisd(in.pitch * zoom_scale, right);
		cam.direction = (quat * cam.direction).normalized();
		// re-orthonormalize so drift never creeps in
		cam.body_up = (quat * cam.body_up).normalized();
		right = cam.direction.cross(cam.body_up).normalized();
		cam.body_up = right.cross(cam.direction).normalized();

		const auto altitude_per_second = 1.0;
		const auto min_speed = 5.0;
		auto altitude = cam.eye.norm() - planet_radius;
		auto speed = fmax(min_speed, altitude * altitude_per_second * zoom_scale);
		auto mag = speed * (in.dt_ms / 1000.0) * (in.slow ? 0.1 : 1.0) * in.speed_gain;
		auto fwd = (in.forward ? 1.0 : 0.0) - (in.back ? 1.0 : 0.0);
		auto vert = (in.raise ? 1.0 : 0.0) - (in.lower ? 1.0 : 0.0);
		auto lat = (in.right ? 1.0 : 0.0) - (in.left ? 1.0 : 0.0);
		Vector3d new_eye = cam.eye
			+ (fwd * cam.direction + lat * right + vert * cam.body_up) * mag;
		if (new_eye.norm() - planet_radius < 1000 * 1000 * 10) cam.eye = new_eye;
		return;
	}

	// rotation
	auto yaw = in.yaw * zoom_scale;
	auto pitch = in.pitch * zoom_scale;
	auto overhead = cam.direction.dot(-up);
	if ((overhead > 0.99 && pitch < 0) || (overhead < -0.99 && pitch > 0))
		pitch = 0;
	auto pitch_axis = cam.direction.cross(up);
	auto yaw_axis = cam.direction.cross(pitch_axis);
	pitch_axis.normalize();
	AngleAxisd roll_angle(0, Vector3d::UnitZ());
	AngleAxisd yaw_angle(yaw, yaw_axis);
	AngleAxisd pitch_angle(pitch, pitch_axis);
	auto quat = roll_angle * yaw_angle * pitch_angle;
	auto rotation = quat.matrix();
	cam.direction = (rotation * cam.direction).normalized();

	// movement: speed proportional to altitude, so apparent (screen-space)
	// motion is constant and approaching the ground eases in exponentially.
	// altitude is a proxy for distance to the terrain being looked at; the
	// shell can override it with real height above the mesh (terrain hug)
	const auto altitude_per_second = 1.0;
	const auto min_speed = 5.0; // m/s floor so we don't freeze at ground level
	auto altitude = isnan(in.altitude_override)
		? cam.eye.norm() - planet_radius : in.altitude_override;
	// the floor stays absolute so zoomed-in ground-level flight can still move
	auto speed = fmax(min_speed, altitude * altitude_per_second * zoom_scale);
	auto mag = speed * (in.dt_ms / 1000.0) * (in.slow ? 0.1 : 1.0) * in.speed_gain;
	auto sideways = cam.direction.cross(up).normalized();
	auto horizontal = up.cross(sideways).normalized(); // view direction projected onto the horizontal plane

	auto fwd = (in.forward ? 1.0 : 0.0) - (in.back ? 1.0 : 0.0);
	auto vert = (in.raise ? 1.0 : 0.0) - (in.lower ? 1.0 : 0.0);
	auto lat = (in.right ? 1.0 : 0.0) - (in.left ? 1.0 : 0.0);

	// truck: shared between frames, holds altitude
	Vector3d new_eye = cam.eye + lat * sideways * mag;
	if (!in.view_frame) {
		// ground frame: cruise holds altitude, pedestal climbs along up
		new_eye += fwd * horizontal * mag;
		new_eye = new_eye.normalized() * cam.eye.norm();
		new_eye += vert * up * mag;
	} else {
		// view frame: dolly along the boresight, boom along view-up
		new_eye = new_eye.normalized() * cam.eye.norm();
		auto view_up = sideways.cross(cam.direction).normalized();
		new_eye += fwd * cam.direction * mag + vert * view_up * mag;
	}
	auto pot_altitude = new_eye.norm() - planet_radius;
	if (pot_altitude < 1000 * 1000 * 10) {
		cam.eye = new_eye;
	}

	clampOrthoTilt(cam);
}
