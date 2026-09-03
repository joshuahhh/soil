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
	// the follow cursor is one quad, rewritten every frame, so it gets its
	// own buffer rather than disturbing the ribbon's
	GLuint cur_vbo = 0, cur_vao = 0;
	GLint transform_loc, viewport_loc, width_loc, color_loc, bias_loc, round_loc;
	GLint pos_a_loc, pos_b_loc, end_loc, side_loc;
	int vert_count = 0;
};

// the miniature ("tilt-shift") post-process: the frame is drawn into an
// offscreen color+depth pair instead of the canvas, then a fullscreen pass
// blurs each pixel by how far its depth is from a focus distance — the
// shallow depth of field of a macro lens, which is what makes a city read
// as a model of one — and pushes the colors toward toy paint
struct dof_gl_t {
	bool on = false;
	GLuint fbo = 0, color = 0, depth = 0, program = 0, vao = 0;
	int w = 0, h = 0; // the attachments' allocated size
	GLint color_loc, depth_loc, viewport_loc, planes_loc, params_loc;
	// set per frame by the engine (it knows the clip planes and where the
	// terrain under the crosshair is); strength/range by the host
	float focus = 1000.0f;   // meters, the sharp plane
	float strength = 0.015f; // max blur radius as a fraction of the height
	float range_host = 0.35f; // half-width of the sharp band, as a fraction of focus, at a 45-degree fov
	float range = 0.35f;      // the same, scaled by the engine to the current fov
	float near = 1.0f, far = 1.0e6f;
	bool ortho = false;
};

struct gl_ctx_t {
	int width, height; // weblib: canvas drawable size, set by the shell
	dof_gl_t dof;
	GLuint program;
	GLint transform_loc;
	GLint tube_on_loc;
	GLint tube_mesh_to_local_loc;
	GLint tube_local_to_clip_loc;
	GLint tube_params_loc;
	GLint tube_pass_loc;
	GLint body_on_loc;
	GLint body_curl_loc;
	GLint body_bands_loc;
	GLint body_size_loc;
	GLint body_pos_loc;
	GLint body_nrm_loc;
	GLuint body_tex[2] = {0, 0}; // the figure's geometry image: positions, normals
	int body_w = 0, body_h = 0;
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

void renderDofEnsure(render_ctx_t &ctx, int w, int h) {
	auto &d = ctx.dof;
	if (d.fbo && d.w == w && d.h == h) return;
	if (!d.fbo) glGenFramebuffers(1, &d.fbo);
	if (!d.color) glGenTextures(1, &d.color);
	if (!d.depth) glGenTextures(1, &d.depth);
	glBindTexture(GL_TEXTURE_2D, d.color);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, d.depth);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, d.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d.color, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, d.depth, 0);
	auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		printf("miniature framebuffer incomplete: 0x%x\n", status);
		d.on = false;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	d.w = w;
	d.h = h;
}

void renderFrameBegin(render_ctx_t &ctx, void *window, int width, int height, int sky) {
	if (ctx.dof.on) renderDofEnsure(ctx, width, height);
	glBindFramebuffer(GL_FRAMEBUFFER, ctx.dof.on ? ctx.dof.fbo : 0);
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
		float r_eff, float half_len, float half_wid, float wid_offset) {
	glUniform1i(ctx.tube_on_loc, on);
	glDisable(GL_POLYGON_OFFSET_FILL); // body mode's flat pass turns it on per node
	// draw both faces in tube mode: the camera legitimately sees walls from
	// either side (approaching the tube from outside, or geometry the roll
	// has folded over), and culling them reads as holes in the mesh
	if (on) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
	if (!on) return;
	glUniformMatrix4fv(ctx.tube_local_to_clip_loc, 1, GL_FALSE, local_to_clip16);
	glUniform4f(ctx.tube_params_loc, r_eff, half_len, half_wid, wid_offset);
	glUniform1i(ctx.tube_pass_loc, 1);
}

// which side of the rect a tube-mode draw keeps: 1 warps the mesh and
// discards fragments outside the rect (the tube; the figure); 2 leaves it
// flat and discards the inside — the land around the figure, with the
// hole where the sheet lifted off
// the flat pass sits a hair further away: at curl 0 the sheet lies
// exactly on this ground and the two would fight for the depth buffer.
// polygon offset, in window depth units, not a clip-space nudge — with a
// perspective projection nearly all of the ground is already within a
// fraction of a thousandth of the far plane in ndc, and a constant added
// there pushes it past the plane and clips it away entirely
void renderSetTubePass(render_ctx_t &ctx, int pass) {
	glUniform1i(ctx.tube_pass_loc, pass);
	if (pass == 2) {
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(1.0f, 4.0f);
	} else {
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
}

// per-node mesh-to-local-frame matrix (tube mode only)
void renderSetTubeNode(render_ctx_t &ctx, const float *m16) {
	glUniformMatrix4fv(ctx.tube_mesh_to_local_loc, 1, GL_FALSE, m16);
}

// body mode (see earth_core.h): the figure's geometry image, two w×h float
// textures of positions and unit normals in body units. unfiltered float
// textures are core webgl2; linear filtering of them is not, so the shader
// fetches texels and interpolates by hand. units 5 and 6, clear of the
// terrain texture on 0 and the dof pass's pair on 0/1
void renderBodyUpload(render_ctx_t &ctx, int w, int h, const float *pos, const float *nrm) {
	if (!ctx.body_tex[0]) glGenTextures(2, ctx.body_tex);
	const float *data[2] = {pos, nrm};
	for (auto k = 0; k < 2; k++) {
		glActiveTexture(GL_TEXTURE5 + k);
		glBindTexture(GL_TEXTURE_2D, ctx.body_tex[k]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, data[k]);
	}
	glActiveTexture(GL_TEXTURE0);
	ctx.body_w = w;
	ctx.body_h = h;
	glUniform1i(ctx.body_pos_loc, 5);
	glUniform1i(ctx.body_nrm_loc, 6);
	glUniform2f(ctx.body_size_loc, (float)w, (float)h);
}

// per frame: whether the tube warp is the figure rather than the cylinder,
// and how far along the sheet is from flat (0) to wrapped (1)
void renderSetBody(render_ctx_t &ctx, bool on, float curl, const float *bands4) {
	glUniform1i(ctx.body_on_loc, on && ctx.body_tex[0]);
	glUniform1f(ctx.body_curl_loc, curl);
	glUniform4fv(ctx.body_bands_loc, 1, bands4);
}

// resolve the miniature pass onto the canvas (see dof_gl_t). the terrain
// program is left current again, as the rest of the frame code assumes
void renderFrameEnd(render_ctx_t &ctx) {
	auto &d = ctx.dof;
	if (!d.on) return;
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, d.w, d.h);
	glDisable(GL_DEPTH_TEST);
	glUseProgram(d.program);
	glBindVertexArray(d.vao);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, d.color);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, d.depth);
	glUniform1i(d.color_loc, 0);
	glUniform1i(d.depth_loc, 1);
	glUniform2f(d.viewport_loc, (float)d.w, (float)d.h);
	glUniform3f(d.planes_loc, d.near, d.far, d.ortho ? 1.0f : 0.0f);
	glUniform3f(d.params_loc, d.focus, d.strength * d.h, d.range);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0); // the terrain textures live on unit 0
	glEnable(GL_DEPTH_TEST);
	glUseProgram(ctx.program);
}


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

void renderDofInit(render_ctx_t &ctx) {
	auto &d = ctx.dof;
	d.program = makeShader(
		"#version 300 es\n"
		// one triangle covering the screen, no buffers
		"out vec2 v_uv;"
		"void main() {"
		"	vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);"
		"	v_uv = p * 0.5 + 0.5;"
		"	gl_Position = vec4(p, 0.0, 1.0);"
		"}",
		"#version 300 es\n"
		"precision highp float;"
		"uniform sampler2D color_tex;"
		"uniform sampler2D depth_tex;"
		"uniform vec2 viewport;"
		"uniform vec3 planes;" // near, far, ortho flag
		"uniform vec3 params;" // focus (m), max radius (px), sharp band (fraction of focus)
		"in vec2 v_uv;"
		"out vec4 frag_color;"
		// depth buffer value -> distance along the view axis, meters
		"float lin(float d) {"
		"	if (planes.z > 0.5) return planes.x + d * (planes.y - planes.x);"
		"	float z = d * 2.0 - 1.0;"
		"	return 2.0 * planes.x * planes.y / (planes.y + planes.x - z * (planes.y - planes.x));"
		"}"
		// circle of confusion in pixels: zero inside the sharp band around
		// the focus distance, ramping to the max over another band's width.
		// relative to the focus, so the look is the same at any altitude
		"float coc(vec2 uv) {"
		"	float t = abs(lin(texture(depth_tex, uv).r) - params.x) / (params.x * params.z) - 1.0;"
		"	return clamp(t, 0.0, 1.0) * params.y;"
		"}"
		"void main() {"
		"	float c0 = coc(v_uv);"
		"	vec3 sum = texture(color_tex, v_uv).rgb;"
		"	float wsum = 1.0;"
		"	if (c0 > 0.5) {"
		// a golden-angle spiral disc of taps out to the pixel's own radius.
		// a tap only counts if its own blur disc reaches back here, which
		// keeps a sharp foreground from smearing into a soft background
		"		const int N = 32;"
		"		for (int i = 0; i < N; i++) {"
		"			float r = sqrt((float(i) + 0.5) / float(N)) * c0;"
		"			float a = float(i) * 2.39996;"
		"			vec2 uv = v_uv + vec2(cos(a), sin(a)) * r / viewport;"
		"			float w = clamp(coc(uv) - r + 1.0, 0.0, 1.0);"
		"			sum += texture(color_tex, uv).rgb * w;"
		"			wsum += w;"
		"		}"
		"	}"
		"	vec3 c = sum / wsum;"
		// toy paint: a little more saturation and contrast
		"	float l = dot(c, vec3(0.299, 0.587, 0.114));"
		"	c = mix(vec3(l), c, 1.35);"
		"	c = (c - 0.5) * 1.12 + 0.5;"
		"	frag_color = vec4(clamp(c, 0.0, 1.0), 1.0);"
		"}"
	);
	d.color_loc = glGetUniformLocation(d.program, "color_tex");
	d.depth_loc = glGetUniformLocation(d.program, "depth_tex");
	d.viewport_loc = glGetUniformLocation(d.program, "viewport");
	d.planes_loc = glGetUniformLocation(d.program, "planes");
	d.params_loc = glGetUniformLocation(d.program, "params");
	// an empty vao: the terrain program's attribute arrays are enabled on
	// the default one, and drawing with those enabled but unbacked is an error
	glGenVertexArrays(1, &d.vao);
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
		// which corner of the quad this is, for the round cursor; zero along
		// a ribbon, where there is nothing to round off
		"out vec2 v_corner;"
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
		"	if (dot(d, d) < 1e-8) { v_corner = vec2(end * 2.0 - 1.0, side); off = v_corner * (0.5 * width); }"
		"	else { v_corner = vec2(0.0); off = normalize(vec2(-d.y, d.x)) * (side * 0.5 * width); }"
		"	p.xy += off / half_vp * p.w;"
		"	p.z -= bias * p.w;"
		"	gl_Position = p;"
		"}",

		"#version 300 es\n"
		"precision highp float;\n"
		"uniform vec4 color;"
		// the cursor quad is drawn as a disc: a white core inside a ring of
		// the path's color, the same marker the map draws. msaa can't help
		// with either edge — a hole punched by the fragment shader isn't a
		// geometry edge — so the rim is faded over the last fifth of the
		// radius and the core's edge over a sliver of it
		"uniform bool round_dot;"
		"in vec2 v_corner;"
		"out vec4 frag_color;"
		"void main() {"
		"	float a = color.a;"
		"	vec3 rgb = color.rgb;"
		"	if (round_dot) {"
		"		float d = length(v_corner);"
		"		a *= 1.0 - smoothstep(0.8, 1.0, d);"
		"		rgb = mix(vec3(1.0), rgb, smoothstep(0.46, 0.56, d));"
		"	}"
		"	if (a <= 0.0) discard;"
		"	frag_color = vec4(rgb, a);"
		"}"
	);
	p.transform_loc = glGetUniformLocation(p.program, "transform");
	p.viewport_loc = glGetUniformLocation(p.program, "viewport");
	p.width_loc = glGetUniformLocation(p.program, "width");
	p.bias_loc = glGetUniformLocation(p.program, "bias");
	p.color_loc = glGetUniformLocation(p.program, "color");
	p.round_loc = glGetUniformLocation(p.program, "round_dot");
	p.pos_a_loc = glGetAttribLocation(p.program, "pos_a");
	p.pos_b_loc = glGetAttribLocation(p.program, "pos_b");
	p.end_loc = glGetAttribLocation(p.program, "end");
	p.side_loc = glGetAttribLocation(p.program, "side");
	glGenVertexArrays(1, &p.vao);
	glGenBuffers(1, &p.vbo);
	glGenVertexArrays(1, &p.cur_vao);
	glGenBuffers(1, &p.cur_vbo);
	const GLsizei stride = 8 * sizeof(float);
	for (int pass = 0; pass < 2; pass++) {
		glBindVertexArray(pass ? p.cur_vao : p.vao);
		glBindBuffer(GL_ARRAY_BUFFER, pass ? p.cur_vbo : p.vbo);
		glEnableVertexAttribArray(p.pos_a_loc);
		glVertexAttribPointer(p.pos_a_loc, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
		glEnableVertexAttribArray(p.pos_b_loc);
		glVertexAttribPointer(p.pos_b_loc, 3, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(float)));
		glEnableVertexAttribArray(p.end_loc);
		glVertexAttribPointer(p.end_loc, 1, GL_FLOAT, GL_FALSE, stride, (void *)(6 * sizeof(float)));
		glEnableVertexAttribArray(p.side_loc);
		glVertexAttribPointer(p.side_loc, 1, GL_FLOAT, GL_FALSE, stride, (void *)(7 * sizeof(float)));
	}
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

// the follow cursor: one degenerate quad at a point, which the shader turns
// into a screen-space disc — the marker for where you are along the track.
// same depth treatment as the ribbon it sits on, so a shoulder of terrain
// hides it exactly when it hides the line
void renderPathCursorDraw(render_ctx_t &ctx, const float *transform16, const float *rgba,
		float size_px, float bias, int w, int h, const float *pos3) {
	auto &p = ctx.path;
	float v[6 * 8];
	static const float corners[6][2] = {
		{ 0, -1 }, { 0, 1 }, { 1, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 } };
	for (int i = 0; i < 6; i++) {
		float *d = v + i * 8;
		d[0] = d[3] = pos3[0];
		d[1] = d[4] = pos3[1];
		d[2] = d[5] = pos3[2];
		d[6] = corners[i][0];
		d[7] = corners[i][1];
	}
	glUseProgram(p.program);
	glBindVertexArray(p.cur_vao);
	glBindBuffer(GL_ARRAY_BUFFER, p.cur_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
	glUniformMatrix4fv(p.transform_loc, 1, GL_FALSE, transform16);
	glUniform2f(p.viewport_loc, (float)w, (float)h);
	glUniform1f(p.width_loc, size_px);
	glUniform1f(p.bias_loc, bias);
	glUniform4fv(p.color_loc, 1, rgba);
	glUniform1i(p.round_loc, 1);
	glDisable(GL_CULL_FACE);
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
	glBindVertexArray(0);
	glUseProgram(ctx.program);
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
	glUniform1i(p.round_loc, 0);
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
		"uniform vec4 tube_params;" // (r_eff, half_len, half_wid, wid_offset)
		// mediump in both stages: a uniform shared by the two must agree on
		// precision, and int defaults differ (highp here, mediump there)
		"uniform mediump int tube_pass;" // 1 warped, 2 flat with the rect cut out
		// body mode: the rect goes onto a figure instead of a cylinder. its
		// skin is a geometry image — positions and unit normals over
		// (u around, v feet to head), body units with height 1, x across
		// the figure, y up, z out of its front — sampled bilinearly by hand
		// (u wraps, v clamps; float textures don't filter in webgl2) and
		// scaled so the rect's length is the figure's height. the terrain
		// height rides out along the normal, and body_curl eases the sheet
		// from flat to wrapped. the cpu twin is earth_core's bodyWarp
		"uniform bool body_on;"
		"uniform float body_curl;"
		"uniform vec4 body_bands;" // (legs below v, arm rows v lo, v hi, arms' share of u)
		"uniform vec2 body_size;"
		// highp: a vertex-shader sampler defaults to lowp, and the
		// precision qualifier applies to what the fetch returns
		"uniform highp sampler2D body_pos;"
		"uniform highp sampler2D body_nrm;"
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
		"out float v_loop;"
		"out vec3 v_local;"
		// the image is several loops (legs, arms, torso — see body_bands):
		// the loop a texel belongs to, as its (u, v) box and an id. samples
		// are clamped to the box, so the interpolation never crosses from
		// one part to another, and a terrain triangle whose vertices land
		// in different loops is dropped by the fragment shader: it would
		// stretch across the gap between them
		"vec4 bodyLoop(vec2 uv, out float id) {"
		"	if (uv.y < body_bands.x) {"
		"		if (uv.x < 0.5) { id = 1.0; return vec4(0.0, 0.0, 0.5, body_bands.x); }"
		"		id = 2.0; return vec4(0.5, 0.0, 1.0, body_bands.x);"
		"	}"
		"	if (uv.y >= body_bands.y && uv.y < body_bands.z) {"
		"		float a = body_bands.w;"
		"		if (uv.x < a) { id = 4.0; return vec4(0.0, body_bands.y, a, body_bands.z); }"
		"		if (uv.x < 1.0 - a) { id = 3.0; return vec4(a, body_bands.y, 1.0 - a, body_bands.z); }"
		"		id = 5.0; return vec4(1.0 - a, body_bands.y, 1.0, body_bands.z);"
		"	}"
		"	if (uv.y < body_bands.y) { id = 0.0; return vec4(0.0, body_bands.x, 1.0, body_bands.y); }"
		"	id = 6.0; return vec4(0.0, max(body_bands.x, body_bands.z), 1.0, 1.0);"
		"}"
		"vec3 bodyFetch(sampler2D t, vec2 uv, vec4 box) {"
		"	vec2 f = uv * body_size - 0.5;"
		"	vec2 fl = floor(f);"
		"	vec2 fr = f - fl;"
		"	ivec2 lo = ivec2(box.xy * body_size + 0.5);"
		"	ivec2 hi = ivec2(box.zw * body_size + 0.5) - 1;"
		"	int x0 = clamp(int(fl.x), lo.x, hi.x);"
		"	int x1 = clamp(int(fl.x) + 1, lo.x, hi.x);"
		"	int y0 = clamp(int(fl.y), lo.y, hi.y);"
		"	int y1 = clamp(int(fl.y) + 1, lo.y, hi.y);"
		"	vec3 a = mix(texelFetch(t, ivec2(x0, y0), 0).xyz, texelFetch(t, ivec2(x1, y0), 0).xyz, fr.x);"
		"	vec3 b = mix(texelFetch(t, ivec2(x0, y1), 0).xyz, texelFetch(t, ivec2(x1, y1), 0).xyz, fr.x);"
		"	return mix(a, b, fr.y);"
		"}"
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
		"	v_loop = 0.0;"
		"	v_local = vec3(0.0);"
		"	vec4 p;"
		"	if (tube_on) {"
		"		vec3 l = (tube_mesh_to_local * vec4(position, 1.0)).xyz;"
		"		v_rect = vec2(l.x / tube_params.y, (l.y - tube_params.w) / tube_params.z);"
		"		v_local = l;"
		"		if (tube_pass == 2) {"
		"			p = transform * vec4(position, 1.0);"
		"		} else if (body_on) {"
		"			vec2 uv = vec2(v_rect.y, v_rect.x) * 0.5 + 0.5;"
		"			float S = 2.0 * tube_params.y;"
		"			vec4 box = bodyLoop(uv, v_loop);"
		// ids a thousand apart: the fragment test is an absolute tolerance,
		// and the sliver of a spanning triangle that passes it next to a
		// vertex shrinks with the gap between ids — at 1 it was a visible
		// string of dots along the band edges
		"			v_loop *= 1000.0;"
		"			vec3 b = bodyFetch(body_pos, uv, box);"
		"			vec3 n = normalize(bodyFetch(body_nrm, uv, box));"
		"			vec3 w = vec3(S * b.x, S * b.z, S * b.y) + vec3(n.x, n.z, n.y) * l.z;"
		"			p = tube_local_to_clip * vec4(mix(l, w, body_curl), 1.0);"
		"		} else {"
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
		"		}"
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
		"uniform mediump int tube_pass;"
		"uniform bool body_on;"
		"in vec2 v_texcoords;"
		"in float v_loop;"
		"in vec3 v_local;"
		"in float v_stale;"
		"in float v_mask;"
		"in vec2 v_rect;"
		"out vec4 frag_color;"
		"void main() {"
		"	if (v_mask < 0.004) discard;"
		// tube mode: clip the slab to the drawn rectangle, so tiles straddling
		// the edge (and geometry rolled past the seam) end cleanly. the flat
		// pass keeps the other side, the land around; and the rect itself,
		// the ground the sheet lifted off, it paints near-black — a facet
		// normal from the screen-space slope of the rect-local position,
		// catching a little of a fixed light so the relief still reads
		"	if (tube_on) {"
		"		bool inside = abs(v_rect.x) <= 1.0 && abs(v_rect.y) <= 1.0;"
		"		if (tube_pass == 2 && inside) {"
		"			vec3 n = normalize(cross(dFdx(v_local), dFdy(v_local)));"
		"			float lit = abs(dot(n, normalize(vec3(0.4, 0.3, 1.0))));"
		"			frag_color = vec4(vec3(0.012) + 0.05 * lit, 1.0);"
		"			return;"
		"		}"
		"		if (tube_pass != 2 && !inside) discard;"
		// a triangle spanning two of the figure's loops: its loop id
		// interpolates to a fraction somewhere inside it
		"		if (body_on && tube_pass == 1 && abs(v_loop - 1000.0 * floor(v_loop / 1000.0 + 0.5)) > 0.5) discard;"
		"	}"
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
	ctx.tube_pass_loc = glGetUniformLocation(ctx.program, "tube_pass");
	ctx.body_on_loc = glGetUniformLocation(ctx.program, "body_on");
	ctx.body_curl_loc = glGetUniformLocation(ctx.program, "body_curl");
	ctx.body_bands_loc = glGetUniformLocation(ctx.program, "body_bands");
	ctx.body_size_loc = glGetUniformLocation(ctx.program, "body_size");
	ctx.body_pos_loc = glGetUniformLocation(ctx.program, "body_pos");
	ctx.body_nrm_loc = glGetUniformLocation(ctx.program, "body_nrm");
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
	renderDofInit(ctx);
	glUseProgram(ctx.program); // the overlay inits left their own programs current
}
