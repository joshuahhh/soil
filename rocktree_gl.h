#include <GLES3/gl3.h>
// gl3.h has no extension enums; the s3tc/dxt formats live here (gl2ext is the
// shared extension header — it applies to gles3 contexts too)
#include <GLES2/gl2ext.h>

// the dropped-track overlay. gl line width is clamped to 1 in webgl (and the
// crack-fill lines are the only thing that ever wanted more), so a polyline
// you can actually see and style has to be triangles: each segment is a quad
// whose corners the vertex shader pushes sideways in *screen* pixels, which
// is what makes the width constant with distance. its own vao keeps the
// terrain program's attribute arrays — enabled once in renderInit, in the
// default vao — untouched
struct path_gl_t {
	GLuint program = 0, vbo = 0, vao = 0;
	GLint transform_loc, viewport_loc, width_loc, color_loc, bias_loc;
	GLint pos_a_loc, pos_b_loc, end_loc, side_loc;
	int vert_count = 0;
};

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
	// the path overlay's own program and geometry (see renderPathInit): a
	// second pass with nothing in common with the terrain one but the frame
	path_gl_t path;
};

// largest anisotropy the gpu offers, 1 if the extension is missing
static float texture_max_aniso = 1.0f;
// live A/B switch (M key). only the uncompressed path has a mip chain to
// switch to, so this does nothing on dxt tiles
static bool texture_mipmaps_on = true;

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
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	meshTexImage2d(mesh);

	// lod selection stops at the first level finer than one texel per pixel
	// (see earth_core's texels_per_meter test), so minification is bounded —
	// but that test uses distance to the node centre, which ignores
	// foreshortening. at a grazing angle one axis is compressed several times
	// over, and that's where an unmipmapped texture aliases.
	//
	// only the uncompressed path can build its own chain: glGenerateMipmap
	// rejects compressed textures, and the crn files carry a single level
	// (crn_get_levels == 1 on every tile sampled), so the dxt path would need
	// its levels generated and re-encoded cpu-side.
	//
	// that covers more than it sounds like: jpeg is not merely the no-s3tc
	// fallback this file's other comments imply — google serves a lot of
	// nodes as jpeg regardless. measured over settled views: grand canyon
	// 200/200 tiles jpeg, rainier 137/250, seattle 111/250, manhattan 24/100
	if (mesh.texture_format == rocktree_t::texture_format_rgb) {
		glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		// trilinear alone picks its level from the *worst* axis, so it fixes
		// the aliasing by blurring the other one; anisotropy is what keeps a
		// grazing-angle tile sharp along its uncompressed axis
		if (texture_max_aniso > 1.0f)
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, texture_max_aniso);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}

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
	// per-draw rather than at upload so the M key can A/B it on a live scene:
	// the temporal difference (shimmer as the camera moves) is the whole point
	// and a still frame barely shows it
	if (mesh.texture_format == rocktree_t::texture_format_rgb)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			texture_mipmaps_on ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
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

void renderSetMipmaps(render_ctx_t &ctx, bool on) { texture_mipmaps_on = on; }

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

// --- path overlay ---------------------------------------------------------
// vertex layout, 8 floats: both ends of the segment (pos_a, pos_b), which end
// this corner belongs to (0/1), and which side of the line it is on (-1/+1).
// both ends are on every vertex because the sideways push is perpendicular to
// the *projected* segment, which neither endpoint knows on its own.
// a degenerate quad (pos_a == pos_b) is a join: the square patch that fills
// the wedge two segments leave open at a bend.
void renderPathInit(render_ctx_t &ctx) {
	auto &p = ctx.path;
	p.program = makeShader(
		"#version 300 es\n"
		"uniform mat4 transform;"   // path-local -> clip
		"uniform vec2 viewport;"    // drawable size in pixels
		"uniform float width;"      // line width in pixels
		// depth bias in ndc units, toward the camera: a track draped onto the
		// terrain is coplanar with it, and a constant ndc nudge is roughly
		// the shape of the depth buffer's own precision (fine near, coarse far)
		"uniform float bias;"
		"in vec3 pos_a;"
		"in vec3 pos_b;"
		"in float end;"
		"in float side;"
		"void main() {"
		"	vec4 a = transform * vec4(pos_a, 1.0);"
		"	vec4 b = transform * vec4(pos_b, 1.0);"
		// near-plane clip in clip space: projecting a vertex with w <= 0 flings
		// it to the wrong side of the screen and the segment draws as a streak
		// across the view. shorten the segment to the plane instead
		"	const float wmin = 1e-4;"
		"	if (a.w < wmin && b.w < wmin) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }"
		"	if (a.w < wmin) a = mix(a, b, (wmin - a.w) / (b.w - a.w));"
		"	if (b.w < wmin) b = mix(b, a, (wmin - b.w) / (a.w - b.w));"
		"	vec2 half_vp = 0.5 * viewport;"
		"	vec2 sa = a.xy / a.w * half_vp;"
		"	vec2 sb = b.xy / b.w * half_vp;"
		"	vec2 d = sb - sa;"
		"	vec4 p = end < 0.5 ? a : b;"
		"	vec2 off;"
		// a degenerate segment is a join patch: an axis-aligned square, using
		// the two corner bits as its two signs
		"	if (dot(d, d) < 1e-8) off = vec2(end * 2.0 - 1.0, side) * (0.5 * width);"
		"	else off = normalize(vec2(-d.y, d.x)) * (side * 0.5 * width);"
		"	p.xy += off / half_vp * p.w;"
		"	p.z -= bias * p.w;"
		"	gl_Position = p;"
		"}",

		"#version 300 es\n"
		"precision highp float;\n"
		"uniform vec4 color;"
		"out vec4 frag_color;"
		"void main() { frag_color = color; }"
	);
	p.transform_loc = glGetUniformLocation(p.program, "transform");
	p.viewport_loc = glGetUniformLocation(p.program, "viewport");
	p.width_loc = glGetUniformLocation(p.program, "width");
	p.bias_loc = glGetUniformLocation(p.program, "bias");
	p.color_loc = glGetUniformLocation(p.program, "color");
	p.pos_a_loc = glGetAttribLocation(p.program, "pos_a");
	p.pos_b_loc = glGetAttribLocation(p.program, "pos_b");
	p.end_loc = glGetAttribLocation(p.program, "end");
	p.side_loc = glGetAttribLocation(p.program, "side");
	glGenVertexArrays(1, &p.vao);
	glGenBuffers(1, &p.vbo);
	glBindVertexArray(p.vao);
	glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
	const GLsizei stride = 8 * sizeof(float);
	glEnableVertexAttribArray(p.pos_a_loc);
	glVertexAttribPointer(p.pos_a_loc, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
	glEnableVertexAttribArray(p.pos_b_loc);
	glVertexAttribPointer(p.pos_b_loc, 3, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray(p.end_loc);
	glVertexAttribPointer(p.end_loc, 1, GL_FLOAT, GL_FALSE, stride, (void *)(6 * sizeof(float)));
	glEnableVertexAttribArray(p.side_loc);
	glVertexAttribPointer(p.side_loc, 1, GL_FLOAT, GL_FALSE, stride, (void *)(7 * sizeof(float)));
	glBindVertexArray(0);
}

// replace the ribbon's geometry (floats, 8 per vertex)
void renderPathUpload(render_ctx_t &ctx, const float *data, int vert_count) {
	auto &p = ctx.path;
	p.vert_count = vert_count;
	glBindBuffer(GL_ARRAY_BUFFER, p.vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vert_count * 8 * sizeof(float),
		vert_count ? data : nullptr, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// draw the uploaded ribbon. depth *test* on so terrain hides the parts of the
// track that are over the hill, but depth *writes* off: the ribbon overlaps
// itself at every join, and a translucent line that occludes itself comes out
// blotchy. culling is off because a quad's winding follows which way the
// segment happens to run on screen — renderSetTube re-establishes it at the
// top of the next frame, and nothing else draws after this one
void renderPathDraw(render_ctx_t &ctx, const float *transform16, const float *rgba,
		float width_px, float bias, int w, int h) {
	auto &p = ctx.path;
	if (!p.vert_count) return;
	glUseProgram(p.program);
	glBindVertexArray(p.vao);
	glUniformMatrix4fv(p.transform_loc, 1, GL_FALSE, transform16);
	glUniform2f(p.viewport_loc, (float)w, (float)h);
	glUniform1f(p.width_loc, width_px);
	glUniform1f(p.bias_loc, bias);
	glUniform4fv(p.color_loc, 1, rgba);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLES, 0, p.vert_count);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glBindVertexArray(0);
	// the terrain path sets its uniforms without a glUseProgram of its own
	glUseProgram(ctx.program);
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
	// anisotropic filtering is an extension in both webgl 1 and 2
	for (GLint i = 0; i < ext_count; i++) {
		auto e = (const char *)glGetStringi(GL_EXTENSIONS, i);
		if (e && strstr(e, "texture_filter_anisotropic")) {
			glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &texture_max_aniso);
			break;
		}
	}
	printf("max anisotropy: %.0f\n", texture_max_aniso);
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

	renderPathInit(ctx);
	glUseProgram(ctx.program); // path init left its own program current
}
