#pragma once

#include <GLES3/gl3.h>
#include <atomic>
#include <map>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>
#include "rocktree_proto.h"
#include "rocktree_math.h"

// whether the gpu can take dxt1 uploads. set by the render backend at init;
// without it (android gpus, generally) node requests ask for jpg textures
// instead — an unsupported compressed upload samples as solid black
static bool texture_s3tc_supported = true;

enum dl_state : int {
	dl_state_stub = 1,
	dl_state_downloading = 2,
	dl_state_downloaded = 4,	
};

// where a downloading node's request is, as far as the fetch backend says
// (the web page reports cache vs network per request); the lod overlay
// (L) tints a missing tile by it
enum fetch_phase : int {
	fetch_phase_none = 0,   // requested, no word yet
	fetch_phase_cache = 1,  // being looked up in the tile cache
	fetch_phase_net = 2,    // on the network
	fetch_phase_decode = 3, // bytes in hand, in the decode pool
};

// bumped whenever the octree's shape changes under the walk — a bulk
// arrives, fails back to a stub, or is purged — so a cached walk result
// (earth_core.h) knows it is stale
static std::atomic<int> g_tree_epoch{0};

struct rocktree_t {
	enum texture_format : int {
		texture_format_rgb = 1,
		texture_format_dxt1 = 2,
	};
	struct bulk_t;
	// the download state machine nodes and bulks share: a stub is asked for
	// (downloading), then lands (downloaded) or fails back to a stub, and a
	// downloaded one is deleted back to a stub. from asked-for until it is a
	// stub again it counts in its parent's busy_ctr. bulks shape the octree,
	// so their transitions also bump g_tree_epoch; nodes' don't. the
	// transitions are defined below rocktree_t, where bulk_t is complete
	template <bool shapes_tree>
	struct download_t {
		std::atomic<dl_state> dl_state;
		bulk_t* parent;

		void setNotDownloadedYet();
		void setStartedDownloading();
		void setFinishedDownloading();
		void setFailedDownloading();
		void setDeleted();

		double last_wanted_ms = 0; // last time this was in the potential set (render thread only)
		// a request that failed comes back to stub so it can be asked for
		// again, but a url that is failing for a reason won't start working
		// this frame: the scheduler holds off until dl_next_try_ms, which it
		// pushes further out with every attempt
		int dl_fails = 0;
		double dl_next_try_ms = 0;
	};
	struct node_t : download_t<false> {
		NodeDataRequest request;
		bool can_have_data;
		std::atomic<int> fetch_phase{fetch_phase_none};
		// tube mode: the download order's measure of this node, set by the
		// walk — its distance through the roll over its size, pushed back
		// when its rolled position can't be on screen (render thread only)
		double sched_pri = 0;

		// when the current unbroken run of being wanted began (profiling)
		double first_wanted_ms = 0;

		float meters_per_texel;
		OrientedBoundingBox obb;

		Matrix4d matrix_globe_from_mesh;
		struct mesh_t {
			std::vector<uint8_t> vertices;
			std::vector<uint16_t> indices;
			// edges belonging to exactly one triangle, drawn as GL_LINES to
			// plug the hairline cracks that open along seams between tiles
			std::vector<uint16_t> boundary_indices;
			// per-vertex normals in the globe frame, area-weighted over the
			// triangles each vertex is in, as signed bytes (xyz, pad): the
			// shading's normal where a primitive has no face of its own —
			// the crack-fill lines
			std::vector<int8_t> normals;
			Vector2f uv_offset;
			Vector2f uv_scale;

			std::vector<uint8_t> texture;
			texture_format texture_format;
			int texture_width;
			int texture_height;

			GLuint vertex_buffer;
			GLuint index_buffer;
			GLuint boundary_index_buffer;
			GLuint texture_buffer;
			GLuint normal_buffer;
			// the vertex setup for a draw, in one bind (see bufferMesh)
			GLuint vao;
			// the min filter the texture was last given (the M key flips
			// ctx.mipmaps_on live; the parameter is re-sent only when it
			// no longer matches, not on every draw)
			bool mips_on;
			bool buffered;
		};
		std::vector<mesh_t> meshes;
	};
	
	struct bulk_t : download_t<true> {
		BulkMetadataRequest request;

		Vector3f head_node_center;

		std::atomic<int> busy_ctr;

		std::map<std::string, std::unique_ptr<node_t>> nodes;
		std::map<std::string, std::unique_ptr<bulk_t>> bulks;
	};
	float radius;
	bulk_t *root_bulk;
	std::atomic<bool> downloaded;
};

template <bool shapes_tree>
void rocktree_t::download_t<shapes_tree>::setNotDownloadedYet() {
	dl_state = dl_state_stub;
}

template <bool shapes_tree>
void rocktree_t::download_t<shapes_tree>::setStartedDownloading() {
	if (parent) parent->busy_ctr++;
	dl_state = dl_state_downloading;
}

template <bool shapes_tree>
void rocktree_t::download_t<shapes_tree>::setFinishedDownloading() {
	dl_state = dl_state_downloaded;
	if (shapes_tree) g_tree_epoch++;
}

template <bool shapes_tree>
void rocktree_t::download_t<shapes_tree>::setFailedDownloading() {
	dl_fails++;
	dl_state = dl_state_stub;
	if (parent) parent->busy_ctr--;
	if (shapes_tree) g_tree_epoch++;
}

template <bool shapes_tree>
void rocktree_t::download_t<shapes_tree>::setDeleted() {
	dl_state = dl_state_stub;
	if (parent) parent->busy_ctr--;
	if (shapes_tree) g_tree_epoch++;
}