#pragma once

#include <unordered_map>
#include <algorithm>
#include <assert.h>
#include <math.h>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "crn/crn.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "rocktree_proto.h"
#include "rocktree_decoder.h"
#include "rocktree_math.h"
#include "rocktree_types.h"

// createBulkMetadataRequest creates a object for requesting bulk data. can be re-used
inline BulkMetadataRequest createBulkMetadataRequest(const std::string &base_path, const std::string &path, int epoch)
{
	BulkMetadataRequest req;
	auto key = new NodeKey();
	key->set_allocated_path(new std::string(base_path + path));
	key->set_epoch(epoch);
	req.set_allocated_node_key(key);
	return req;
}

// createNodeDataRequest creates a object for requesting node data. can be re-used
inline NodeDataRequest createNodeDataRequest(const std::string &base_path, const BulkMetadata &bulk, const NodeMetadata &node_meta) {
	auto aux = unpackPathAndFlags(node_meta);	
	assert(!(aux.flags & NodeMetadata_Flags_NODATA));
	//assert(node_meta.has_epoch());
	NodeDataRequest req;

	// set texture format based on supported formats. crn/dxt1 preferred
	// (smaller, gpu-native) unless the render backend reported no s3tc
	req.set_texture_format(([&]() {
		static Texture_Format with_s3tc[] = { Texture_Format_CRN_DXT1, Texture_Format_JPG };
		static Texture_Format without_s3tc[] = { Texture_Format_JPG, Texture_Format_CRN_DXT1 };
		auto supported = texture_s3tc_supported ? with_s3tc : without_s3tc;

		int available = node_meta.has_available_texture_formats()
			? node_meta.available_texture_formats()
			: bulk.default_available_texture_formats();

		for (auto i = 0; i < 2; i++) {
			if (available & (1 << (supported[i] - 1))) return supported[i];
		}
		return supported[0];
	})());
	
	// set imagery epoch if flags say it should be used
	if (aux.flags & NodeMetadata_Flags_USE_IMAGERY_EPOCH) {
		auto imagery_epoch = node_meta.has_imagery_epoch() ?
			node_meta.imagery_epoch() :
			bulk.default_imagery_epoch();
		req.set_imagery_epoch(imagery_epoch);
	}

	// set path and epoch
	req.set_allocated_node_key(([&](){
		auto key = new NodeKey();
		key->set_allocated_path(new std::string(base_path + std::string(aux.path)));	
		assert(bulk.has_head_node_key() && bulk.head_node_key().has_epoch());
		key->set_epoch(node_meta.has_epoch() ? node_meta.epoch() : bulk.head_node_key().epoch());
		return key;
	})());

	return req;
}

// populateBulk and populateNode fill in a bulk or node from its parsed
// message and publish it (setFinishedDownloading). they run on the decode
// pool. tile data that doesn't add up fails the decode instead: they return
// what was wrong, leave nothing behind, and the caller puts it back to a stub

inline const char *populateBulk(rocktree_t::bulk_t *bulk, const BulkMetadata &bulk_metadata) {

	for (int i = 0; i < 3; i++) bulk->head_node_center[i] = bulk_metadata.head_node_center()[i];			

	for (const auto &node_meta : bulk_metadata.node_metadata()) {						
		
		auto aux = unpackPathAndFlags(node_meta);	   
		auto has_data = !(aux.flags & NodeMetadata_Flags_NODATA);
		auto has_bulk = strlen(aux.path) == 4 && !(aux.flags & NodeMetadata_Flags_LEAF);

		if (has_bulk) {													
			auto epoch = node_meta.has_bulk_metadata_epoch()
				? node_meta.bulk_metadata_epoch()
				: bulk_metadata.head_node_key().epoch();
			
			auto b = std::make_unique<rocktree_t::bulk_t>();
			b->setNotDownloadedYet();
			b->parent = bulk;
			b->request = createBulkMetadataRequest(bulk->request.node_key().path(), aux.path, epoch);			
			b->busy_ctr = 0;
			bulk->bulks.insert(std::make_pair(aux.path,std::move(b)));
		}

		if ((has_data || !(aux.flags & NodeMetadata_Flags_LEAF)) && node_meta.has_oriented_bounding_box()) {
			auto meters_per_texel = node_meta.has_meters_per_texel()
				? node_meta.meters_per_texel()
				: bulk_metadata.meters_per_texel(aux.level - 1);		
			
			auto n = std::make_unique<rocktree_t::node_t>();
			n->setNotDownloadedYet();
			n->parent = bulk;
			n->can_have_data = has_data;
			if (has_data) {
				n->request = createNodeDataRequest(bulk->request.node_key().path(), bulk_metadata, node_meta);
			}
			n->meters_per_texel = meters_per_texel;
			if (!unpackObb(node_meta.oriented_bounding_box(), bulk->head_node_center, meters_per_texel, n->obb)) {
				// (only stubs so far: nothing else has seen them)
				bulk->nodes.clear();
				bulk->bulks.clear();
				return "bad obb";
			}
			bulk->nodes.insert(std::make_pair(aux.path,std::move(n)));
		}
	}	
	bulk->setFinishedDownloading();
	return nullptr;
}

inline void populatePlanetoid(rocktree_t *planetoid, std::unique_ptr<PlanetoidMetadata> planetoid_metadata) {
	auto bulk = new rocktree_t::bulk_t();
	bulk->parent = nullptr;
	bulk->request = createBulkMetadataRequest("", "", planetoid_metadata->root_node_metadata().epoch());
	bulk->busy_ctr = 0;
	bulk->setNotDownloadedYet();

	planetoid->radius = planetoid_metadata->radius();	
	planetoid->root_bulk = bulk;	
	planetoid->downloaded = true;
}

inline const char *populateNode(rocktree_t::node_t *node, const NodeData &node_data) {
	assert(node->can_have_data);

	for (int i = 0; i < 16; i++) node->matrix_globe_from_mesh.data()[i] = node_data.matrix_globe_from_mesh(i);					

	// decoded into a local, so a failure partway leaves node->meshes empty
	std::vector<rocktree_t::node_t::mesh_t> meshes;
	for (const auto &mesh : node_data.meshes()) {
		rocktree_t::node_t::mesh_t m;		

		if (!unpackIndices(mesh.indices(), m.indices)) return "bad indices";
		m.vertices = unpackVertices(mesh.vertices());

		if (!unpackTexCoords(mesh.texture_coordinates(), m.vertices.data(), m.vertices.size(), m.uv_offset, m.uv_scale))
			return "bad texture coordinates";
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
		if (!unpackOctantMaskAndOctantCountsAndLayerBounds(mesh.layer_and_octant_counts(), m.indices.data(), m.indices.size(), m.vertices.data(), m.vertices.size(), layer_bounds))
			return "bad octant counts";
		if (!(0 <= layer_bounds[3] && layer_bounds[3] <= m.indices.size())) return "bad layer bounds";
		//m.indices_len = layer_bounds[3]; // enable
		m.indices.resize(layer_bounds[3]);

		// vertex normals, in the globe frame: each triangle's edges taken
		// through the mesh's linear transform (its axes are scaled
		// unequally, so a normal crossed in mesh space would lean) and
		// crossed there, summed onto its corners. the strip's odd triangles
		// wind the other way; the shader turns the result toward the camera
		// anyway, so only consistency within a mesh matters
		{
			auto nv = m.vertices.size() / 8;
			std::vector<Vector3d> acc(nv, Vector3d::Zero());
			Matrix3d lin = node->matrix_globe_from_mesh.block<3, 3>(0, 0);
			auto vp = [&](uint16_t i) {
				auto p = m.vertices.data() + (size_t)i * 8;
				return Vector3d(p[0], p[1], p[2]);
			};
			for (size_t i = 0; i + 2 < m.indices.size(); i++) {
				auto a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
				if (a == b || b == c || a == c || a >= nv || b >= nv || c >= nv) continue;
				if (i & 1) std::swap(a, b);
				Vector3d f = (lin * (vp(b) - vp(a))).cross(lin * (vp(c) - vp(a)));
				acc[a] += f; acc[b] += f; acc[c] += f;
			}
			m.normals.resize(nv * 4);
			for (size_t i = 0; i < nv; i++) {
				Vector3d n = acc[i].norm() > 0 ? acc[i].normalized() : Vector3d::Zero();
				for (int k = 0; k < 3; k++) m.normals[i * 4 + k] = (int8_t)lround(n[k] * 127.0);
				m.normals[i * 4 + 3] = 0;
			}
		}

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

		const auto &textures = mesh.texture();
		if (textures.size() != 1) return "not one texture";
		const auto &texture = textures[0];
		if (texture.data().size() != 1) return "not one texture data";
		const auto &tex = texture.data()[0];

		// maybe: keep compressed in memory?
		if (texture.format() == Texture_Format_JPG) {
			auto data = (uint8_t*)tex.data();
			int width, height, comp;
			unsigned char* pixels = stbi_load_from_memory(&data[0], tex.size(), &width, &height, &comp, 0);
			if (pixels == NULL) return "jpg decode failed";
			if (!(width == texture.width() && height == texture.height() && comp == 3)) {
				stbi_image_free(pixels);
				return "jpg size mismatch";
			}
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
			if (dst_size != ((texture.width() + 3) / 4) * ((texture.height() + 3) / 4) * 8) return "crn size mismatch";
			m.texture = std::vector<uint8_t>(dst_size);
			crn_decompress(src, src_size, m.texture.data(), dst_size, 0);
			m.texture_format = rocktree_t::texture_format_dxt1;
		} else {
			fprintf(stderr, "unsupported texture format: %d\n", texture.format());
			return "unsupported texture format";
		}

		m.texture_width = texture.width();
		m.texture_height = texture.height();		

		m.buffered = false;
		meshes.push_back(std::move(m));
	}
	node->meshes = std::move(meshes);
	node->setFinishedDownloading();
	return nullptr;
}