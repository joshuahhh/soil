// camera state and flying controls, independent of any windowing or input
// backend. included after eigen (see main.cpp); part of the platform-free
// core so embedding shells (native window, web canvas) can own and drive the
// camera directly

struct camera_t {
	Vector3d eye;       // ecef meters
	Vector3d direction; // unit
	double fov;         // vertical, radians
};

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
	bool forward = false, back = false, left = false, right = false;
	bool raise = false, lower = false;
	bool view_frame = false;
	bool slow = false;        // momentary precision (x0.1)
	double speed_gain = 1.0;  // sticky multiplier owned by the shell
	double dt_ms = 0;
};

void applyCameraInput(camera_t &cam, const camera_input_t &in, double planet_radius) {
	// up is the vec from the planetoid's center towards the sky
	auto up = cam.eye.normalized();

	// zooming magnifies apparent motion, so rotation and the
	// altitude-proportional speed scale down with it, keeping screen-space
	// rates constant. 1 at the default 45-degree fov
	auto zoom_scale = tan(cam.fov / 2.0) / tan(0.125 * M_PI);

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
	// altitude is a proxy for distance to the terrain being looked at; good
	// enough until we track real terrain height under the camera
	const auto altitude_per_second = 1.0;
	const auto min_speed = 5.0; // m/s floor so we don't freeze at ground level
	auto altitude = cam.eye.norm() - planet_radius;
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
}
