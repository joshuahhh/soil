#include <GLES3/gl3.h>
// gl3.h has no extension enums; the s3tc/dxt formats live here (gl2ext is the
// shared extension header — it applies to gles3 contexts too)
#include <GLES2/gl2ext.h>

struct gl_ctx_t {
	int width, height; // weblib: canvas drawable size, set by the shell
	GLuint program;
	GLint transform_loc;
	GLint tube_on_loc;
	GLint tube_mesh_to_local_loc;
	GLint tube_local_to_clip_loc;
	GLint tube_params_loc;
	GLint uv_offset_loc;
	GLint uv_scale_loc;
	GLint octant_mask_loc;
	GLint stale_mask_loc;
	GLint debug_lod_loc;
	GLint texture_loc;
	GLint position_loc;
	GLint octant_loc;
	GLint texcoords_loc;
};

void meshTexImage2d(const rocktree_t::node_t::mesh_t &mesh) {
	switch (mesh.texture_format) {
	case rocktree_t::texture_format_rgb:
		// tightly packed rgb rows; without this, widths whose row size isn't
		// a multiple of 4 upload skewed (the default unpack alignment is 4)
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, mesh.texture_width, mesh.texture_height, 0, GL_RGB, GL_UNSIGNED_BYTE, mesh.texture.data());
		break;
	case rocktree_t::texture_format_dxt1:
		glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT, mesh.texture_width, mesh.texture_height, 0, mesh.texture.size(), mesh.texture.data());
		break;
	default:
		fprintf(stderr, "unsupported texture format: %d\n", mesh.texture_format);
		abort();
	}	
}

void bufferMesh(rocktree_t::node_t::mesh_t &mesh) {
	if (mesh.buffered) fprintf(stderr, "mesh already buffered\n"), abort();

	glGenBuffers(1, &mesh.vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(unsigned char), mesh.vertices.data(), GL_STATIC_DRAW);
	glGenBuffers(1, &mesh.index_buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(unsigned short), mesh.indices.data(), GL_STATIC_DRAW);
	glGenBuffers(1, &mesh.boundary_index_buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.boundary_index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.boundary_indices.size() * sizeof(unsigned short), mesh.boundary_indices.data(), GL_STATIC_DRAW);

	glGenTextures(1, &mesh.texture_buffer);
	glBindTexture(GL_TEXTURE_2D, mesh.texture_buffer);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); // GL_NEAREST
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	meshTexImage2d(mesh);

	mesh.buffered = true;
}

void bindAndDrawMesh(const rocktree_t::node_t::mesh_t &mesh, uint8_t octant_mask, uint8_t stale_mask, const gl_ctx_t &ctx) {
	glUniform2fv(ctx.uv_offset_loc, 1, mesh.uv_offset.data());
	glUniform2fv(ctx.uv_scale_loc, 1, mesh.uv_scale.data());
	int v[8] = {
		(octant_mask >> 0) & 1, (octant_mask >> 1) & 1, (octant_mask >> 2) & 1, (octant_mask >> 3) & 1,
		(octant_mask >> 4) & 1, (octant_mask >> 5) & 1, (octant_mask >> 6) & 1, (octant_mask >> 7) & 1
	};
	glUniform1iv(ctx.octant_mask_loc, 8, v);
	int s[8] = {
		(stale_mask >> 0) & 1, (stale_mask >> 1) & 1, (stale_mask >> 2) & 1, (stale_mask >> 3) & 1,
		(stale_mask >> 4) & 1, (stale_mask >> 5) & 1, (stale_mask >> 6) & 1, (stale_mask >> 7) & 1
	};
	glUniform1iv(ctx.stale_mask_loc, 8, s);
	glUniform1i(ctx.texture_loc, 0);
	glBindTexture(GL_TEXTURE_2D, mesh.texture_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.vertex_buffer);
	
	glVertexAttribPointer(ctx.position_loc, 3, GL_UNSIGNED_BYTE, GL_FALSE, 8, (void*)0);
	glVertexAttribPointer(ctx.octant_loc, 1, GL_UNSIGNED_BYTE, GL_FALSE, 8, (void*)3);
	glVertexAttribPointer(ctx.texcoords_loc, 2, GL_UNSIGNED_SHORT, GL_FALSE, 8, (void*)4);
	
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
	glDrawElements(GL_TRIANGLE_STRIP, mesh.indices.size(), GL_UNSIGNED_SHORT, NULL);

	// crack fill: re-draw the mesh's boundary edges as lines. in the slit
	// pixels along tile seams nothing has been drawn, so the lines win the
	// depth test there and nowhere else
	static const bool no_lines_debug = getenv("EARTH_NO_LINES") != nullptr;
	if (!no_lines_debug && mesh.boundary_indices.size()) {
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.boundary_index_buffer);
		glDrawElements(GL_LINES, mesh.boundary_indices.size(), GL_UNSIGNED_SHORT, NULL);
	}
}

void unbufferMesh(rocktree_t::node_t::mesh_t &mesh) {
	if (!mesh.buffered) fprintf(stderr, "mesh isn't buffered\n"), abort();

	mesh.buffered = false;
	
	glDeleteTextures(1, &mesh.texture_buffer); // auto: glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteBuffers(1, &mesh.index_buffer); // auto: glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDeleteBuffers(1, &mesh.boundary_index_buffer);
	glDeleteBuffers(1, &mesh.vertex_buffer); // auto: glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// backend-neutral wrappers (a removed metal backend once provided the same
// interface; the indirection is kept in case another backend returns)
typedef gl_ctx_t render_ctx_t;

#ifdef EARTH_WEBLIB
// no sdl in the web library: the shell tracks the canvas drawable size in
// the ctx and the browser presents implicitly when the frame callback returns
void renderDrawableSize(render_ctx_t &ctx, void *window, int *w, int *h) {
	*w = ctx.width;
	*h = ctx.height;
}
#else
void renderDrawableSize(render_ctx_t &ctx, void *window, int *w, int *h) {
	SDL_GL_GetDrawableSize((SDL_Window *)window, w, h);
}
#endif

void renderFrameBegin(render_ctx_t &ctx, void *window, int width, int height, int sky) {
	glViewport(0, 0, width, height);
	glClearColor((sky>>16 & 0xff) / 255.0f, (sky>>8 & 0xff) / 255.0f, (sky & 0xff) / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void renderSetDebugLod(render_ctx_t &ctx, bool debug_lod) {
	glUniform1i(ctx.debug_lod_loc, debug_lod);
}

void renderSetTransform(render_ctx_t &ctx, const float *m16) {
	glUniformMatrix4fv(ctx.transform_loc, 1, GL_FALSE, m16);
}

// tube mode (see earth_core.h): per-frame local-frame-to-clip matrix and warp
// parameters (effective roll radius, rect half length/width in meters)
void renderSetTube(render_ctx_t &ctx, bool on, const float *local_to_clip16,
		float r_eff, float half_len, float half_wid) {
	glUniform1i(ctx.tube_on_loc, on);
	// draw both faces in tube mode: the camera legitimately sees walls from
	// either side (approaching the tube from outside, or geometry the roll
	// has folded over), and culling them reads as holes in the mesh
	if (on) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
	if (!on) return;
	glUniformMatrix4fv(ctx.tube_local_to_clip_loc, 1, GL_FALSE, local_to_clip16);
	glUniform3f(ctx.tube_params_loc, r_eff, half_len, half_wid);
}

// per-node mesh-to-local-frame matrix (tube mode only)
void renderSetTubeNode(render_ctx_t &ctx, const float *m16) {
	glUniformMatrix4fv(ctx.tube_mesh_to_local_loc, 1, GL_FALSE, m16);
}

void renderFrameEnd(render_ctx_t &ctx) {}

bool renderReadPixels(render_ctx_t &ctx, int w, int h, uint8_t *rgb) {
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
	return true;
}

void renderPresent(render_ctx_t &ctx, void *window) {
#ifndef EARTH_WEBLIB
	SDL_GL_SwapWindow((SDL_Window *)window);
#endif
}

void checkCompileShaderError(GLuint shader) {
	GLint is_compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &is_compiled);
	if(is_compiled == GL_TRUE) return;
	GLint max_len = 0;
	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &max_len);
	std::vector<GLchar> error_log(max_len);
	glGetShaderInfoLog(shader, max_len, &max_len, &error_log[0]);
	std::cout << &error_log[0] << std::endl;
	glDeleteShader(shader);
	abort();
}

GLuint makeShader(const char* vert_src, const char* frag_src) {
	GLuint vert_shader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vert_shader, 1, &vert_src, NULL);
	glCompileShader(vert_shader);
	checkCompileShaderError(vert_shader);
	GLuint frag_shader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(frag_shader, 1, &frag_src, NULL);
	glCompileShader(frag_shader);
	checkCompileShaderError(frag_shader);
	GLuint program = glCreateProgram();
	glAttachShader(program, vert_shader);
	glAttachShader(program, frag_shader);
	glLinkProgram(program);
	glDetachShader(program, vert_shader);
	glDetachShader(program, frag_shader);
	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);
	return program;
}
// backend init
void renderInit(render_ctx_t &ctx, void *) {
	// gles 3.0 dropped the single-string GL_EXTENSIONS query — it returns null
	// here, and taking that as "no s3tc" would silently push every texture
	// down the jpeg path. enumerate with glGetStringi instead
	GLint ext_count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &ext_count);
	bool has_s3tc = false;
	for (GLint i = 0; i < ext_count && !has_s3tc; i++) {
		auto e = (const char *)glGetStringi(GL_EXTENSIONS, i);
		if (e && strstr(e, "s3tc")) has_s3tc = true;
	}
	texture_s3tc_supported = texture_s3tc_supported && has_s3tc;
	if (!texture_s3tc_supported)
		printf("no s3tc on this gpu; using jpg textures\n");
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	// crack-fill boundary lines (see bindAndDrawMesh) can only land on
	// background pixels, so width just sets how wide a slit they can plug
	// (wider also costs more fill rate). webgl typically clamps this to 1
	auto lw = getenv("EARTH_LINE_WIDTH");
	glLineWidth(lw ? (float)atof(lw) : 2.0f);
	ctx.program = makeShader(
		"#version 300 es\n"
		"uniform mat4 transform;"
		// tube mode: vertices go mesh -> rect-local frame (x along the tube
		// axis, y across, z up), roll across-offset y into an angle around a
		// horizontal axis at height R, then local -> clip. R is the effective
		// roll radius (the true tube radius over the curl amount, so curl->0
		// flattens back out). v_rect is the position in rect units for the
		// fragment shader to clip the slab's edges
		"uniform bool tube_on;"
		"uniform mat4 tube_mesh_to_local;"
		"uniform mat4 tube_local_to_clip;"
		"uniform vec3 tube_params;" // (r_eff, half_len, half_wid)
		"uniform vec2 uv_offset;"
		"uniform vec2 uv_scale;"
		"uniform bool octant_mask[8];"
		"uniform bool stale_mask[8];"
		"in vec3 position;"
		"in float octant;"
		"in vec2 texcoords;"
		"out vec2 v_texcoords;"
		"out float v_stale;"
		"out float v_mask;"
		"out vec2 v_rect;"
		"void main() {"
		// masking: a triangle is dropped only when ALL its vertices are in
		// masked octants (v_mask interpolates to 0 -> fragment discard).
		// triangles straddling an octant boundary used to be collapsed
		// entirely, retreating the surface a triangle-row from the boundary
		// and opening hairline cracks the finer tile never covers; instead
		// they are drawn, with masked vertices pushed slightly away in depth
		// so the finer tile wins wherever they overlap it
		"	float mask = octant_mask[int(octant)] ? 0.0 : 1.0;"
		"	v_mask = mask;"
		"	v_stale = stale_mask[int(octant)] ? 1.0 : 0.0;"
		"	v_texcoords = (texcoords + uv_offset) * uv_scale;"
		"	vec4 p;"
		"	if (tube_on) {"
		"		vec3 l = (tube_mesh_to_local * vec4(position, 1.0)).xyz;"
		"		v_rect = vec2(l.x / tube_params.y, l.y / tube_params.z);"
		"		float R = tube_params.x;"
		"		float th = l.y / R;"
		// radial distance from the tube axis; the floor keeps terrain taller
		// than the tube radius squashed near the axis instead of piercing
		// through and coming out inverted on the far side
		"		float rho = max(R - l.z, 0.05 * R);"
		// height = R - rho*cos(th) = R*(1-cos th) + ze*cos th where ze is the
		// height after the clamp; the 2sin^2 form keeps precision when R is
		// huge (curl near zero, i.e. nearly flat)
		"		float ze = R - rho;"
		"		float hs = sin(0.5 * th);"
		"		vec3 w = vec3(l.x, rho * sin(th), 2.0 * R * hs * hs + ze * cos(th));"
		"		p = tube_local_to_clip * vec4(w, 1.0);"
		"	} else {"
		"		v_rect = vec2(0.0);"
		"		p = transform * vec4(position, 1.0);"
		"	}"
		"	p.z += (1.0 - mask) * 0.002 * p.w;"
		"	gl_Position = p;"
		"}",

		"#version 300 es\n"
		// gles 3.0 requires highp in fragment shaders, so the "use highp if the
		// hardware has it, else mediump" dance webgl1 needed is gone — and with
		// it the fp16 path that garbled atlas uv interpolation on mobile gpus
		"precision highp float;\n"
		// not "texture": that names a built-in function in gles 3.00, and a
		// uniform of the same name would hide it
		"uniform sampler2D tex;"
		"uniform bool debug_lod;"
		"uniform bool tube_on;"
		"in vec2 v_texcoords;"
		"in float v_stale;"
		"in float v_mask;"
		"in vec2 v_rect;"
		"out vec4 frag_color;"
		"void main() {"
		"	if (v_mask < 0.004) discard;"
		// tube mode: clip the slab to the drawn rectangle, so tiles straddling
		// the edge (and geometry rolled past the seam) end cleanly
		"	if (tube_on && (abs(v_rect.x) > 1.0 || abs(v_rect.y) > 1.0)) discard;"
		"	vec3 c = texture(tex, v_texcoords).rgb;"
		"	if (debug_lod) c = mix(c, vec3(1.0, 0.0, 0.0), v_stale * 0.5);"
		"	frag_color = vec4(c, 1.0);"
		"}"
	);
	glUseProgram(ctx.program);
	ctx.transform_loc = glGetUniformLocation(ctx.program, "transform");
	ctx.tube_on_loc = glGetUniformLocation(ctx.program, "tube_on");
	ctx.tube_mesh_to_local_loc = glGetUniformLocation(ctx.program, "tube_mesh_to_local");
	ctx.tube_local_to_clip_loc = glGetUniformLocation(ctx.program, "tube_local_to_clip");
	ctx.tube_params_loc = glGetUniformLocation(ctx.program, "tube_params");
	ctx.uv_offset_loc = glGetUniformLocation(ctx.program, "uv_offset");
	ctx.uv_scale_loc = glGetUniformLocation(ctx.program, "uv_scale");
	ctx.octant_mask_loc = glGetUniformLocation(ctx.program, "octant_mask");
	ctx.stale_mask_loc = glGetUniformLocation(ctx.program, "stale_mask");
	ctx.debug_lod_loc = glGetUniformLocation(ctx.program, "debug_lod");
	ctx.texture_loc = glGetUniformLocation(ctx.program, "tex");
	ctx.position_loc = glGetAttribLocation(ctx.program, "position");
	ctx.octant_loc = glGetAttribLocation(ctx.program, "octant");
	ctx.texcoords_loc = glGetAttribLocation(ctx.program, "texcoords");

	glEnableVertexAttribArray(ctx.position_loc);
	glEnableVertexAttribArray(ctx.octant_loc);
	glEnableVertexAttribArray(ctx.texcoords_loc);
}
