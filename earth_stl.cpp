// the stl export's alpha wrap: see earth_stl.h. compiled on its own by
// build.sh (cgal, wasm exceptions) and linked into the engine
#include "earth_stl.h"

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/alpha_wrap_3.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/clip.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>

#include <array>
#include <chrono>
#include <exception>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

namespace {

using K = CGAL::Exact_predicates_inexact_constructions_kernel;
using Point = K::Point_3;
using Mesh = CGAL::Surface_mesh<Point>;

double nowMs() {
	using namespace std::chrono;
	return (double)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() / 1000.0;
}

// thrown out of the wrap to stop it
struct cancelled_t {};

// counts the wrap's progress and lets the page stop it. cgal calls these
// through the visitor's own type, so hiding the default's is enough. a
// stop by returning false from go_further isn't one: cgal goes on to make
// the half-done wrap manifold, and that ran 95 s on a downtown block
// stopped at a third of the way. a throw unwinds at once
struct visitor_t : CGAL::Alpha_wraps_3::internal::Wrapping_default_visitor {
	stl_job_t *job;
	explicit visitor_t(stl_job_t *j) : job(j) {}
	template <typename Wrapper>
	bool go_further(const Wrapper &) {
		if (job->cancel.load(std::memory_order_relaxed)) throw cancelled_t();
		return true;
	}
	template <typename Wrapper, typename VertexHandle>
	void after_Steiner_point_insertion(const Wrapper &, VertexHandle) {
		job->steiner.fetch_add(1, std::memory_order_relaxed);
	}
	double t0 = 0;
	template <typename Wrapper>
	void on_flood_fill_begin(const Wrapper &) { t0 = nowMs(); }
	template <typename Wrapper>
	void on_flood_fill_end(const Wrapper &) { job->ms_flood = nowMs() - t0; }
};

struct v3 { double x, y, z; };

// the soup clipped to the margin box |x| <= mx, |y| <= my (sutherland-hodgman
// in xy, z carried along), and closed underneath: a curtain from every
// clipped edge down to the floor, and the floor. a point the clip makes on a
// side has that side's coordinate exactly, and so does any point later
// interpolated between two of them, so an edge along a side is found by
// equality. the floor goes under the lowest point, buried ones included;
// the base is measured on the wrap (run)
void prepSoup(const stl_job_t &job, double mx, double my, double &z_floor, double &zmax,
		std::vector<Point> &pts, std::vector<std::array<size_t, 3>> &faces, double &area) {
	auto n = job.pos.size() / 3;
	pts.clear();
	pts.reserve(n + n / 8);
	for (size_t i = 0; i < n; i++) pts.emplace_back(job.pos[3 * i], job.pos[3 * i + 1], job.pos[3 * i + 2]);
	faces.clear();
	faces.reserve(job.tris.size() / 3);
	auto zmin = INFINITY;
	zmax = -INFINITY;
	area = 0;
	std::vector<std::pair<v3, v3>> edges; // the clipped boundary, for the curtains
	auto triArea = [](const v3 &a, const v3 &b, const v3 &c) {
		auto ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
		auto cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
		return 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
	};
	v3 poly[16], next[16];
	for (size_t t = 0; t + 2 < job.tris.size(); t += 3) {
		size_t id[3] = {job.tris[t], job.tris[t + 1], job.tris[t + 2]};
		if (id[0] >= n || id[1] >= n || id[2] >= n) continue;
		v3 p[3];
		auto inside = true, outside = false;
		auto lox = INFINITY, hix = -INFINITY, loy = INFINITY, hiy = -INFINITY;
		for (auto k = 0; k < 3; k++) {
			p[k] = {job.pos[3 * id[k]], job.pos[3 * id[k] + 1], job.pos[3 * id[k] + 2]};
			lox = fmin(lox, p[k].x); hix = fmax(hix, p[k].x);
			loy = fmin(loy, p[k].y); hiy = fmax(hiy, p[k].y);
		}
		if (hix < -mx || lox > mx || hiy < -my || loy > my) outside = true;
		if (lox < -mx || hix > mx || loy < -my || hiy > my) inside = false;
		if (outside) continue;
		if (inside) {
			faces.push_back({id[0], id[1], id[2]});
			for (auto k = 0; k < 3; k++) { zmin = fmin(zmin, p[k].z); zmax = fmax(zmax, p[k].z); }
			area += triArea(p[0], p[1], p[2]);
			continue;
		}
		auto m = 3;
		for (auto k = 0; k < 3; k++) poly[k] = p[k];
		// the four sides as (axis, sign): keep axis * sign <= limit
		for (auto side = 0; side < 4 && m > 0; side++) {
			auto ax = side < 2 ? 0 : 1;
			auto sg = side % 2 ? -1.0 : 1.0;
			auto lim = ax ? my : mx;
			auto c = [&](const v3 &q) { return (ax ? q.y : q.x) * sg; };
			auto k2 = 0;
			for (auto k = 0; k < m; k++) {
				auto &a = poly[k], &b = poly[(k + 1) % m];
				auto ca = c(a), cb = c(b);
				if (ca <= lim) next[k2++] = a;
				if ((ca <= lim) != (cb <= lim)) {
					auto w = (lim - ca) / (cb - ca);
					v3 q = {a.x + (b.x - a.x) * w, a.y + (b.y - a.y) * w, a.z + (b.z - a.z) * w};
					if (ax) q.y = lim * sg; else q.x = lim * sg;
					next[k2++] = q;
				}
			}
			m = k2;
			for (auto k = 0; k < m; k++) poly[k] = next[k];
		}
		if (m < 3) continue;
		auto base = pts.size();
		for (auto k = 0; k < m; k++) {
			pts.emplace_back(poly[k].x, poly[k].y, poly[k].z);
			zmin = fmin(zmin, poly[k].z);
			zmax = fmax(zmax, poly[k].z);
		}
		for (auto k = 1; k + 1 < m; k++) {
			faces.push_back({base, base + k, base + k + 1});
			area += triArea(poly[0], poly[k], poly[k + 1]);
		}
		for (auto k = 0; k < m; k++) {
			auto &a = poly[k], &b = poly[(k + 1) % m];
			if ((fabs(a.x) == mx && a.x == b.x) || (fabs(a.y) == my && a.y == b.y)) edges.push_back({a, b});
		}
	}
	if (faces.empty()) return;
	z_floor = zmin - job.base - job.alpha;
	auto quad = [&](const v3 &a, const v3 &b, const v3 &c, const v3 &d) {
		auto o = pts.size();
		pts.emplace_back(a.x, a.y, a.z);
		pts.emplace_back(b.x, b.y, b.z);
		pts.emplace_back(c.x, c.y, c.z);
		pts.emplace_back(d.x, d.y, d.z);
		faces.push_back({o, o + 1, o + 2});
		faces.push_back({o, o + 2, o + 3});
		area += triArea(a, b, c) + triArea(a, c, d);
	};
	for (auto &e : edges) {
		quad(e.first, e.second, {e.second.x, e.second.y, z_floor}, {e.first.x, e.first.y, z_floor});
	}
	quad({-mx, -my, z_floor}, {mx, -my, z_floor}, {mx, my, z_floor}, {-mx, my, z_floor});
}

void writeStl(stl_job_t &job, const Mesh &mesh, double dx, double dy, double dz) {
	job.faces = mesh.number_of_faces();
	job.stl.assign(84 + 50 * job.faces, 0);
	auto out = job.stl.data();
	// (not starting with 'solid', which some readers take to mean ascii)
	memcpy(out, job.header.data(), std::min<size_t>(80, job.header.size()));
	auto nf = (uint32_t)job.faces;
	memcpy(out + 80, &nf, 4); // wasm is little-endian, as stl is
	auto o = out + 84;
	for (auto f : mesh.faces()) {
		float v[12];
		auto k = 3;
		for (auto vd : CGAL::vertices_around_face(mesh.halfedge(f), mesh)) {
			if (k >= 12) break;
			auto &p = mesh.point(vd);
			v[k++] = (float)(p.x() + dx);
			v[k++] = (float)(p.y() + dy);
			v[k++] = (float)(p.z() + dz);
		}
		auto ux = v[6] - v[3], uy = v[7] - v[4], uz = v[8] - v[5];
		auto wx = v[9] - v[3], wy = v[10] - v[4], wz = v[11] - v[5];
		auto nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
		auto l = sqrtf(nx * nx + ny * ny + nz * nz);
		if (l <= 0) l = 1;
		v[0] = nx / l; v[1] = ny / l; v[2] = nz / l;
		memcpy(o, v, 48);
		o += 50;
	}
}

void run(stl_job_t &job) {
	auto t0 = nowMs();
	auto pad = stlPad(job.alpha, job.offset); // the margin the curtains stand in
	auto mx = job.half_x + pad, my = job.half_y + pad;
	double z_floor, zmax, area;
	std::vector<Point> pts;
	std::vector<std::array<size_t, 3>> faces;
	prepSoup(job, mx, my, z_floor, zmax, pts, faces, area);
	job.pos.clear(); job.pos.shrink_to_fit();
	job.tris.clear(); job.tris.shrink_to_fit();
	if (faces.empty()) {
		job.error = "no terrain in the rect";
		job.state = stl_failed;
		return;
	}
	// what the wrap will add, for the progress bar: some fraction of a
	// point per alpha^2 of surface. on a downtown seattle block it was 0.9
	// at alpha 0.7 / offset 0.1 mm, 0.5 at 0.5 / 0.1 (alpha finer than the
	// tiles' triangles, so nothing more to follow) and 0.45 at 0.6 / 0.2
	job.steiner_estimate = (long)(0.6 * area / (job.alpha * job.alpha));
	job.ms_prep = nowMs() - t0;

	job.phase = stl_phase_wrap;
	t0 = nowMs();
	Mesh wrap;
	visitor_t vis(&job);
	CGAL::alpha_wrap_3(pts, faces, job.alpha, job.offset, wrap, CGAL::parameters::visitor(vis));
	job.ms_wrap = nowMs() - t0;
	pts.clear(); pts.shrink_to_fit();
	faces.clear(); faces.shrink_to_fit();
	if (job.cancel) { job.state = stl_cancelled; return; }

	// the base goes under the wrap's lowest point over the rect, the floor
	// aside (its top is inside the solid, its bottom an offset under it).
	// what the tiles bury is inside the solid too, so it can't sink the base
	auto z_ground = INFINITY;
	for (auto v : wrap.vertices()) {
		auto &p = wrap.point(v);
		auto z = CGAL::to_double(p.z());
		if (fabs(CGAL::to_double(p.x())) <= job.half_x && fabs(CGAL::to_double(p.y())) <= job.half_y
			&& z > z_floor + job.alpha) z_ground = fmin(z_ground, z);
	}
	if (!std::isfinite(z_ground)) {
		job.error = "the wrap came out empty";
		job.state = stl_failed;
		return;
	}
	auto z_base = z_ground - job.base;

	// the rect, from the base up past everything: off come the margin, the
	// curtains and the floor. the wrap is closed and doesn't cross itself,
	// which is what the clip needs to close the cut
	job.phase = stl_phase_cut;
	t0 = nowMs();
	namespace PMP = CGAL::Polygon_mesh_processing;
	K::Iso_cuboid_3 box(Point(-job.half_x, -job.half_y, z_base),
		Point(job.half_x, job.half_y, zmax + 4 * job.offset + 1));
	PMP::clip(wrap, box, CGAL::parameters::clip_volume(true));
	// the cut leaves loose pieces: a canopy whose trunk was outside the
	// rect, a crane's arm. everything that belongs stands on the base, in
	// one piece with it
	job.dropped = PMP::keep_largest_connected_components(wrap, 1);
	wrap.collect_garbage();
	job.closed = CGAL::is_closed(wrap);
	job.ms_cut = nowMs() - t0;
	if (job.cancel) { job.state = stl_cancelled; return; }

	// the stl's frame: the rect's south-west corner at the origin, the base
	// on z = 0
	job.phase = stl_phase_write;
	writeStl(job, wrap, job.half_x, job.half_y, -z_base);
	job.state = job.faces ? stl_done : stl_failed;
	if (!job.faces) job.error = "the wrap came out empty";
}

void *threadMain(void *arg) {
	auto job = (std::shared_ptr<stl_job_t> *)arg;
	try {
		run(**job);
	} catch (cancelled_t &) {
		(*job)->state = stl_cancelled;
	} catch (std::exception &e) {
		(*job)->error = e.what();
		(*job)->state = stl_failed;
	} catch (...) {
		(*job)->error = "the wrap failed";
		(*job)->state = stl_failed;
	}
	delete job;
	return nullptr;
}

} // namespace

bool stlStart(std::shared_ptr<stl_job_t> job) {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	// headroom: a test wrap ran in emscripten's default 64 KB, but a big
	// model's is untried, and in wasm an overflow corrupts the heap quietly
	// instead of faulting
	pthread_attr_setstacksize(&attr, 16 << 20);
	auto arg = new std::shared_ptr<stl_job_t>(job);
	pthread_t th;
	auto ok = pthread_create(&th, &attr, threadMain, arg) == 0;
	pthread_attr_destroy(&attr);
	if (!ok) delete arg;
	return ok;
}
