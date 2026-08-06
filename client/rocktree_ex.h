#include <unordered_map>
#include <algorithm>
#include "crn/crn.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

void populateBulk(rocktree_t::bulk_t *bulk, std::unique_ptr<BulkMetadata> bulk_metadata) {

	bulk->_metadata = std::move(bulk_metadata);

	for (int i = 0; i < 3; i++) bulk->head_node_center[i] = bulk->_metadata->head_node_center()[i];			

	for (auto node_meta : bulk->_metadata->node_metadata()) {						
		
		auto aux = unpackPathAndFlags(node_meta);	   
		auto has_data = !(aux.flags & NodeMetadata_Flags_NODATA);
		auto has_bulk = strlen(aux.path) == 4 && !(aux.flags & NodeMetadata_Flags_LEAF);

		if (has_bulk) {													
			auto epoch = node_meta.has_bulk_metadata_epoch()
				? node_meta.bulk_metadata_epoch()
				: bulk->_metadata->head_node_key().epoch();
			
			auto b = std::make_unique<rocktree_t::bulk_t>();
			b->setNotDownloadedYet();
			b->parent = bulk;
			b->request = createBulkMetadataRequest(bulk->request.node_key().path(), aux.path, epoch);			
			b->busy_ctr = 0;
			assert(bulk->bulks.insert(std::make_pair(aux.path,std::move(b))).second);
		}

		if ((has_data || !(aux.flags & NodeMetadata_Flags_LEAF)) && !node_meta.has_oriented_bounding_box()) {
#ifndef EMSCRIPTEN
			printf("skip unknown node\n");
#endif
		}

		if ((has_data || !(aux.flags & NodeMetadata_Flags_LEAF)) && node_meta.has_oriented_bounding_box()) {
			auto meters_per_texel = node_meta.has_meters_per_texel()
				? node_meta.meters_per_texel()
				: bulk->_metadata->meters_per_texel(aux.level - 1);		
			
			auto n = std::make_unique<rocktree_t::node_t>();
			n->setNotDownloadedYet();
			n->parent = bulk;
			n->can_have_data = has_data;
			if (has_data) {
				n->request = createNodeDataRequest(bulk->request.node_key().path(), *(bulk->_metadata), node_meta);
			}
			n->meters_per_texel = meters_per_texel;
			n->obb = unpackObb(node_meta.oriented_bounding_box(), bulk->head_node_center, meters_per_texel);
			assert(bulk->nodes.insert(std::make_pair(aux.path,std::move(n))).second);
		}
	}	
	bulk->_metadata = nullptr;
	bulk->setFinishedDownloading();
}

void populatePlanetoid(rocktree_t *planetoid, std::unique_ptr<PlanetoidMetadata> planetoid_metadata) {
	auto bulk = new rocktree_t::bulk_t();
	bulk->parent = nullptr;
	bulk->request = createBulkMetadataRequest("", "", planetoid_metadata->root_node_metadata().epoch());
	bulk->busy_ctr = 0;
	bulk->setNotDownloadedYet();

	planetoid->radius = planetoid_metadata->radius();	
	planetoid->root_bulk = bulk;	
	planetoid->downloaded = true;
}

void populateNode(rocktree_t::node_t *node, std::unique_ptr<NodeData> node_data) {
	assert(node->can_have_data);

	for (int i = 0; i < 16; i++) node->matrix_globe_from_mesh.data()[i] = node_data->matrix_globe_from_mesh(i);					

	for (auto mesh : node_data->meshes()) {
		rocktree_t::node_t::mesh_t m;		

		m.indices = unpackIndices(mesh.indices());
		m.vertices = unpackVertices(mesh.vertices());

		unpackTexCoords(mesh.texture_coordinates(), m.vertices.data(), m.vertices.size(), m.uv_offset, m.uv_scale);
		if (mesh.uv_offset_and_scale_size() == 4) {
			m.uv_offset[0] = mesh.uv_offset_and_scale(0);
			m.uv_offset[1] = mesh.uv_offset_and_scale(1);
			m.uv_scale[0] = mesh.uv_offset_and_scale(2);
			m.uv_scale[1] = mesh.uv_offset_and_scale(3);
		}
		// the imagery's v axis runs opposite to the texture t axis in both
		// cases; the flip used to be applied only when uv_offset_and_scale
		// was absent (fine tiles), leaving every mesh that ships the field
		// (the coarse levels) v-mirrored within its tile — landforms diced
		// and rearranged at planet zooms while street level looked perfect
		m.uv_offset[1] -= 1 / m.uv_scale[1];
		m.uv_scale[1] *= -1;

		int layer_bounds[10];
		unpackOctantMaskAndOctantCountsAndLayerBounds(mesh.layer_and_octant_counts(), m.indices.data(), m.indices.size(), m.vertices.data(), m.vertices.size(), layer_bounds);
		assert(0 <= layer_bounds[3] && layer_bounds[3] <= m.indices.size());
		//m.indices_len = layer_bounds[3]; // enable
		if (!getenv("EARTH_FULL_LAYERS")) m.indices.resize(layer_bounds[3]);

		// collect the mesh's boundary: edges used by exactly one
		// non-degenerate triangle of the strip. neighboring tiles quantize
		// their vertices onto different grids, so hairline cracks open along
		// tile seams; the renderer re-draws these edges as lines to fill
		// whatever sub-pixel slits the triangles leave
		{
			std::unordered_map<uint32_t, int> edge_count;
			auto idx = m.indices.data();
			for (size_t i = 0; i + 2 < m.indices.size(); i++) {
				auto a = idx[i], b = idx[i+1], c = idx[i+2];
				if (a == b || b == c || a == c) continue;
				uint32_t edges[3] = {
					(uint32_t)std::min(a,b) << 16 | std::max(a,b),
					(uint32_t)std::min(b,c) << 16 | std::max(b,c),
					(uint32_t)std::min(a,c) << 16 | std::max(a,c),
				};
				for (auto e : edges) edge_count[e]++;
			}
			for (auto &kv : edge_count) {
				if (kv.second != 1) continue;
				m.boundary_indices.push_back(kv.first >> 16);
				m.boundary_indices.push_back(kv.first & 0xffff);
			}
		}

		auto textures = mesh.texture();
		assert(textures.size() == 1);
		auto texture = textures[0];
		assert(texture.data().size() == 1);
		auto tex = texture.data()[0];

		// maybe: keep compressed in memory?
		if (texture.format() == Texture_Format_JPG) {
			auto data = (uint8_t*)tex.data();
			int width, height, comp;
			unsigned char* pixels = stbi_load_from_memory(&data[0], tex.size(), &width, &height, &comp, 0);
			assert(pixels != NULL);
			assert (width == texture.width() && height == texture.height() && comp == 3);
			// the jpg variant of the atlas is stored flipped vertically
			// relative to the crn/dxt variant the uv transform expects
			m.texture.resize((size_t)width * height * comp);
			for (int y = 0; y < height; y++)
				memcpy(m.texture.data() + (size_t)y * width * comp,
					pixels + (size_t)(height - 1 - y) * width * comp,
					(size_t)width * comp);
			stbi_image_free(pixels);
			m.texture_format = rocktree_t::texture_format_rgb;
			{
				// debug: catch tiles whose decoded jpg is (near-)black
				long sum = 0;
				for (size_t i = 0; i < m.texture.size(); i += 97) sum += m.texture[i];
				auto avg = (int)(sum / (m.texture.size() / 97 + 1));
				if (avg < 8) printf("jpgtex dark: %s %dx%d avg=%d jpgbytes=%zu\n",
					node->request.node_key().path().c_str(), width, height, avg, tex.size());
			}
		} else if (texture.format() == Texture_Format_CRN_DXT1) {
			auto src_size = tex.size();
			auto src = (uint8_t*)tex.data();
			auto dst_size = crn_get_decompressed_size(src, src_size, 0);
			assert(dst_size == ((texture.width() + 3) / 4) * ((texture.height() + 3) / 4) * 8);
			m.texture = std::vector<uint8_t>(dst_size);
			crn_decompress(src, src_size, m.texture.data(), dst_size, 0);
			m.texture_format = rocktree_t::texture_format_dxt1;
		} else {
			fprintf(stderr, "unsupported texture format: %d\n", texture.format());
			abort();
		}

		m.texture_width = texture.width();
		m.texture_height = texture.height();		

		m.buffered = false;
		node->meshes.push_back(m);
	}
	node->setFinishedDownloading();
}