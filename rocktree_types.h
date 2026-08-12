#include <SDL_opengl.h>

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
		}

		void setFailedDownloading() {
			dl_state = dl_state_stub;
			if (parent) parent->busy_ctr--;
		}

		void setDeleted() {
			dl_state = dl_state_stub;
			if (parent) parent->busy_ctr--;
		}

		double last_wanted_ms = 0; // last time this node was in the potential set (render thread only)

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
		}

		void setFailedDownloading() {
			dl_state = dl_state_stub;
			if (parent) parent->busy_ctr--;
		}

		void setDeleted() {
			dl_state = dl_state_stub;
			if (parent) parent->busy_ctr--;
		}

		double last_wanted_ms = 0; // last time this bulk was in the potential set (render thread only)

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