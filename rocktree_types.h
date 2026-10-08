#include <GLES3/gl3.h>

typedef GLuint render_handle_t;

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
	struct node_t {
		NodeDataRequest request;
		bool can_have_data;
		std::atomic<dl_state> dl_state;
		std::atomic<int> fetch_phase{fetch_phase_none};
		bulk_t* parent;
		// tube mode: the download order's measure of this node, set by the
		// walk — its distance through the roll over its size, pushed back
		// when its rolled position can't be on screen (render thread only)
		double sched_pri = 0;

		void setNotDownloadedYet() {
			dl_state = dl_state_stub;
		}

		void setStartedDownloading() {
			if (parent) parent->busy_ctr++;
			dl_state = dl_state_downloading;
		}

		void setFinishedDownloading() {
			dl_state = dl_state_downloaded;
		}

		void setFailedDownloading() {
			dl_fails++;
			dl_state = dl_state_stub;
			if (parent) parent->busy_ctr--;
		}

		void setDeleted() {
			dl_state = dl_state_stub;
			if (parent) parent->busy_ctr--;
		}

		double last_wanted_ms = 0; // last time this node was in the potential set (render thread only)
		// when the current unbroken run of being wanted began (profiling)
		double first_wanted_ms = 0;
		// a request that failed comes back to stub so it can be asked for
		// again, but a url that is failing for a reason won't start working
		// this frame: the scheduler holds off until dl_next_try_ms, which it
		// pushes further out with every attempt
		int dl_fails = 0;
		double dl_next_try_ms = 0;

		float meters_per_texel;
		OrientedBoundingBox obb;

		std::unique_ptr<NodeData> _data;

		Matrix4d matrix_globe_from_mesh;
		struct mesh_t {
			std::vector<uint8_t> vertices;
			std::vector<uint16_t> indices;
			// edges belonging to exactly one triangle, drawn as GL_LINES to
			// plug the hairline cracks that open along seams between tiles
			std::vector<uint16_t> boundary_indices;
			Vector2f uv_offset;
			Vector2f uv_scale;

			std::vector<uint8_t> texture;
			texture_format texture_format;
			int texture_width;
			int texture_height;

			render_handle_t vertex_buffer;
			render_handle_t index_buffer;
			render_handle_t boundary_index_buffer;
			render_handle_t texture_buffer;
			// the vertex setup for a draw, in one bind (see bufferMesh)
			render_handle_t vao;
			// the min filter the texture was last given (the M key flips
			// texture_mipmaps_on live; the parameter is re-sent only when it
			// no longer matches, not on every draw)
			bool mips_on;
			bool buffered;
		};
		std::vector<mesh_t> meshes;
	};
	
	struct bulk_t {
		BulkMetadataRequest request;
		std::atomic<dl_state> dl_state;
		bulk_t* parent;

		void setNotDownloadedYet() {
			dl_state = dl_state_stub;
		}

		void setStartedDownloading() {
			if (parent) parent->busy_ctr++;
			dl_state = dl_state_downloading;
		}

		void setFinishedDownloading() {
			dl_state = dl_state_downloaded;
			g_tree_epoch++;
		}

		void setFailedDownloading() {
			dl_fails++;
			dl_state = dl_state_stub;
			if (parent) parent->busy_ctr--;
			g_tree_epoch++;
		}

		void setDeleted() {
			dl_state = dl_state_stub;
			if (parent) parent->busy_ctr--;
			g_tree_epoch++;
		}

		double last_wanted_ms = 0; // last time this bulk was in the potential set (render thread only)
		// a request that failed comes back to stub so it can be asked for
		// again, but a url that is failing for a reason won't start working
		// this frame: the scheduler holds off until dl_next_try_ms, which it
		// pushes further out with every attempt
		int dl_fails = 0;
		double dl_next_try_ms = 0;

		Vector3f head_node_center;

		std::unique_ptr<BulkMetadata> _metadata;
		std::atomic<int> busy_ctr;

		std::map<std::string, std::unique_ptr<node_t>> nodes;
		std::map<std::string, std::unique_ptr<bulk_t>> bulks;
	};
	float radius;
	bulk_t *root_bulk;
	std::unique_ptr<PlanetoidMetadata> _metadata;
	std::atomic<bool> downloaded;
};