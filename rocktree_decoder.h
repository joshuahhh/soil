#include <math.h>
const int MAX_LEVEL = 20;

// the unpack* functions that can meet bytes that don't add up return false
// rather than asserting: a malformed tile fails its decode (and is asked for
// again later), it doesn't take the page down

// unpackVarInt unpacks variable length integer from proto (like coded_stream.h)
bool unpackVarInt(const std::string &packed, int* index, int* value) {
	auto data = (uint8_t*)packed.data();
	auto size = packed.size();
	int c = 0, d = 1, e;
	do {
		if (*index >= size) return false;
		e = data[(*index)++];
		c += (e & 0x7F) * d;
		d <<= 7;
	} while (e & 0x80);
	*value = c;
	return true;
}

// vertex is a packed struct for an 8-byte-per-vertex array
#pragma pack(push, 1) 
struct vertex_t {
	uint8_t x, y, z; // position
	uint8_t w;       // octant mask
	uint16_t u, v;   // texture coordinates
};
#pragma pack(pop)
static_assert((sizeof(vertex_t) == 8), "vertex_t size must be 8");

// unpackVertices unpacks vertices XYZ to new 8-byte-per-vertex array
std::vector<uint8_t> unpackVertices(const std::string &packed) {
	auto data = (uint8_t*)packed.data();
	auto count = packed.size() / 3;
	auto vertices = std::vector<uint8_t>(count * sizeof(vertex_t));
	auto vtx = (vertex_t*)vertices.data();
	uint8_t x = 0, y = 0, z = 0; // 8 bit for % 0x100
	for (auto i = 0; i < count; i++) {
		vtx[i].x = x += data[count * 0 + i];
		vtx[i].y = y += data[count * 1 + i];
		vtx[i].z = z += data[count * 2 + i];
	}
	return vertices;
}

// unpackTexCoords unpacks texture coordinates UV to 8-byte-per-vertex-array
bool unpackTexCoords(const std::string &packed, uint8_t* vertices, int vertices_len, Vector2f &uv_offset, Vector2f &uv_scale) {	
	auto data = (uint8_t*)packed.data();
	auto count = vertices_len / sizeof(vertex_t);
	if (!(count * 4 == (packed.size() - 4) && packed.size() >= 4)) return false;
	auto u_mod = 1 + *(uint16_t *)(data + 0);
	auto v_mod = 1 + *(uint16_t *)(data + 2);
	data += 4;
	auto vtx = (vertex_t*)vertices;
	auto u = 0, v = 0;
	for (auto i = 0; i < count; i++) {
		vtx[i].u = u = (u + data[count * 0 + i] + (data[count * 2 + i] << 8)) % u_mod;
		vtx[i].v = v = (v + data[count * 1 + i] + (data[count * 3 + i] << 8)) % v_mod;
	}
	
	uv_offset[0] = 0.5;
	uv_offset[1] = 0.5;
	uv_scale[0] = 1.0 / u_mod;
	uv_scale[1] = 1.0 / v_mod;
	return true;
}

// unpackIndices unpacks indices to triangle strip
bool unpackIndices(const std::string &packed, std::vector<uint16_t> &triangle_strip) {
	auto offset = 0;	

	int triangle_strip_len;
	if (!unpackVarInt(packed, &offset, &triangle_strip_len)) return false;
	// every index takes at least a byte: a length past that is garbage, and
	// sizing the vector by it could ask for gigabytes
	if (triangle_strip_len < 0 || triangle_strip_len > packed.size()) return false;
	triangle_strip.resize(triangle_strip_len);
	for (int zeros = 0, i = 0; i < triangle_strip_len; i++) {
		int val;
		if (!unpackVarInt(packed, &offset, &val)) return false;
		triangle_strip[i] = zeros - val;
		if (0 == val) zeros++;
	}	
	
	return true;
}

// unpackOctantMaskAndOctantCountsAndLayerBounds unpacks the octant mask for vertices (W) and layer bounds and octant counts
bool unpackOctantMaskAndOctantCountsAndLayerBounds(const std::string &packed, const uint16_t *indices, int indices_len, uint8_t *vertices, int vertices_len, int layer_bounds[10])
{
	// todo: octant counts
	auto offset = 0;
	int len;
	if (!unpackVarInt(packed, &offset, &len)) return false;
	auto idx_i = 0;
	auto k = 0;
	auto m = 0;

	for (auto i = 0; i < len; i++) {
		if (0 == i % 8) {
			if (!(m < 10)) return false;
			layer_bounds[m++] = k;
		}
		int v;
		if (!unpackVarInt(packed, &offset, &v)) return false;
		for (auto j = 0; j < v; j++) {
			if (!(idx_i < indices_len)) return false;
			auto idx = indices[idx_i++];
			if (!(0 <= idx && idx < indices_len)) return false;
			auto vtx_i = idx;
			if (!(0 <= vtx_i && vtx_i < vertices_len / sizeof(vertex_t))) return false;
			((vertex_t *)vertices)[vtx_i].w = i & 7;
		}
		k += v;
	}

	for (; 10 > m; m++) layer_bounds[m] = k;
	return true;
}

struct node_data_path_and_flags_t {
	char path[MAX_LEVEL+1];	
	int flags;
	int level;
};

// unpackPathAndFlags unpacks path, flags and level (strlen(path)) from node metadata
node_data_path_and_flags_t unpackPathAndFlags(const NodeMetadata &node_meta) {

	auto getPathAndFlags = [](int path_id, char path[], int* level, int* flags) {
		*level = 1 + (path_id & 3);
		path_id >>= 2;
		for (int i = 0; i < *level; i++) {
			path[i] = '0' + (path_id & 7);
			path_id >>= 3;
		}
		*flags = path_id;
	};

	node_data_path_and_flags_t result;
	getPathAndFlags(node_meta.path_and_flags(), result.path, &result.level, &result.flags);
	result.path[result.level] = '\0';

	return result;
}

struct OrientedBoundingBox {
	Vector3d center;
	Vector3d extents;
	Matrix3d orientation;
};

bool unpackObb(const std::string &packed, Vector3f head_node_center, float meters_per_texel, OrientedBoundingBox &obb) {
	if (packed.size() != 15) return false;
	auto data = (uint8_t*)packed.data();
	obb.center[0] = *(int16_t *)(data + 0) * meters_per_texel + head_node_center[0];
	obb.center[1] = *(int16_t *)(data + 2) * meters_per_texel + head_node_center[1];
	obb.center[2] = *(int16_t *)(data + 4) * meters_per_texel + head_node_center[2];
	obb.extents[0] = *(uint8_t *)(data + 6) * meters_per_texel;
	obb.extents[1] = *(uint8_t *)(data + 7) * meters_per_texel;
	obb.extents[2] = *(uint8_t *)(data + 8) * meters_per_texel;
	Vector3f euler;
	euler[0] = *(uint16_t *)(data + 9) * M_PI / 32768.0f;
	euler[1] = *(uint16_t *)(data + 11) * M_PI / 65536.0f;
	euler[2] = *(uint16_t *)(data + 13) * M_PI / 32768.0f;
	double c0 = cosf(euler[0]);
	double s0 = sinf(euler[0]);
	double c1 = cosf(euler[1]);
	double s1 = sinf(euler[1]);
	double c2 = cosf(euler[2]);
	double s2 = sinf(euler[2]);
	auto orientation = obb.orientation.data();
	orientation[0] = c0 * c2 - c1 * s0 * s2;
	orientation[1] = c1 * c0 * s2 + c2 * s0;
	orientation[2] = s2 * s1;
	orientation[3] = -c0 * s2 - c2 * c1 * s0;
	orientation[4] = c0 * c1 * c2 - s0 * s2;
	orientation[5] = c2 * s1;
	orientation[6] = s1 * s0;
	orientation[7] = -c0 * s1;
	orientation[8] = c1;

	return true;
}