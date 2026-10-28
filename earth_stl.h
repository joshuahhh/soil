// the stl export's solid (see earth_core's export rect): the rect's terrain
// as a triangle soup, shrink-wrapped by cgal's 3d alpha wrap into a closed,
// manifold surface, then cut to the rect and a flat base. the wrap starts
// from the mesh as it is — open, cracked, two levels of detail overlapping at
// octant edges, trees and roofs that hang over what's under them — and seals
// any gap narrower than alpha, keeping whatever overhang is wider, and
// thickens anything thinner than twice the offset, so a lamp post or a
// railing comes out printable instead of vanishing.
//
// the terrain is a sheet, not a solid: to make the wrap see a solid the soup
// is clipped to the rect plus a margin, curtains drop from the clipped edge
// to a floor below the model, and the floor closes it. the margin and the
// floor are then cut away (the cut faces are flat), so the model's sides are
// the wrap's true cross-section and its bottom is one plane.
//
// it runs on a thread of its own: tens of seconds for a big model. it is in
// its own translation unit because cgal is heavy to compile and needs wasm
// exceptions (its filtered predicates throw to fall back to exact numbers),
// which the rest of the engine does without
#pragma once

#include <atomic>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

enum { stl_running, stl_done, stl_failed, stl_cancelled };
enum { stl_phase_prep, stl_phase_wrap, stl_phase_cut, stl_phase_write };

struct stl_job_t {
	// in, mm, in the model's frame: x east, y north, z up, the rect
	// centred on the origin
	std::vector<float> pos;     // xyz per vertex
	std::vector<uint32_t> tris; // three vertices per triangle
	double half_x = 0, half_y = 0;
	double base = 3;            // the solid under the lowest point
	double alpha = 0.5, offset = 0.1;
	std::string header;         // the stl's 80-byte header text

	// out. state and the progress counters are written by the job's thread
	// and polled from the page; the rest is only read once state leaves
	// stl_running
	std::atomic<int> state{stl_running};
	std::atomic<int> phase{stl_phase_prep};
	std::atomic<bool> cancel{false};
	std::atomic<long> steiner{0}; // points the wrap has added so far
	long steiner_estimate = 0;    // and roughly how many it will (set in prep)
	std::vector<uint8_t> stl;
	size_t faces = 0;
	size_t dropped = 0; // loose pieces the cut left, thrown away
	bool closed = false;
	std::string error;
	double ms_prep = 0, ms_wrap = 0, ms_cut = 0;
	double ms_flood = 0; // the part of ms_wrap that is the wrap proper, not its cleanup
};

// starts the job on its own thread; false if the thread couldn't be made
bool stlStart(std::shared_ptr<stl_job_t> job);

// the margin round the rect the soup has to cover, mm: the wrap rounds the
// corner where the terrain meets a curtain by about alpha, and the cut at
// the rect's edge has to miss that
inline double stlPad(double alpha, double offset) { return 3 * alpha + 2 * offset; }
