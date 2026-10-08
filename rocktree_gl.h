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

// the sky pass: a fullscreen triangle drawn after the terrain at the far
// plane, a gradient by elevation above the true horizon with the sun in it
struct sky_gl_t {
	GLuint program = 0;
	GLint right_loc, up_loc, fwd_loc, local_up_loc, sun_loc, params_loc, zenith_loc, haze_loc, alt_loc;
};

// glamour mode's accumulator: frames rendered with sub-pixel and lens
// offsets are summed into a float texture and shown as their average, which
// converges while the camera sits still — supersampling and thin-lens depth
// of field for the price of waiting a second (see EarthView::frame)
struct accum_gl_t {
	bool on = false;
	bool hold = false; // enough samples: present the average, add nothing
	bool failed = false; // no float render target here; stays off
	GLuint fbo = 0, tex = 0, present_program = 0, blit_program = 0;
	int w = 0, h = 0;
	int samples = 0; // frames summed so far
	GLint present_tex_loc, present_scale_loc, blit_tex_loc;
	// ambient occlusion, applied to each sample on its way in (rotated
	// differently each time, so the noise averages out): on, strength,
	// this sample's rotation; and the camera it needs to read depth back
	// into positions, set by the engine every frame
	bool ao_on = true;
	float ao_strength = 1.0f, ao_rotation = 0;
	float near = 1, far = 1e6, tan_half = 0.414f, aspect = 1.6f;
	bool ortho = false;
	GLuint ao_program = 0;
	GLint ao_color_loc, ao_depth_loc, ao_viewport_loc, ao_planes_loc, ao_proj_loc, ao_params_loc;
};

// sun shadows: a depth map rendered from the sun (orthographic, fitted by
// the engine to the near part of the view) over the drawn nodes, sampled by
// the terrain shader with hardware pcf. lives on texture unit 3, always
// bound — a 1x1 placeholder until the first real pass — so the terrain
// program's shadow sampler never points at nothing
struct shadow_gl_t {
	GLuint fbo = 0, tex = 0, program = 0;
	int size = 0; // the depth texture's allocated size (1 = placeholder)
	GLint mv_loc, box_loc, mask_loc;
};
static const int shadow_unit = 3;
static int shadow_debug = 1; // 1 normal; 2 facing term only, 3 cast shadows only
static bool ao_debug = false; // glamour shows the occlusion alone

struct gl_ctx_t {
	int width, height; // weblib: canvas drawable size, set by the shell
	dof_gl_t dof;
	sky_gl_t sky;
	accum_gl_t accum;
	shadow_gl_t shadow;
	// the framebuffer the scene is being drawn into this frame, and its
	// size: an offscreen pass (the shadow map) puts it back afterwards
	GLuint scene_fbo = 0;
	int scene_w = 0, scene_h = 0;
	GLint shadow_mv_loc, shadow_box_loc, shadow_params_loc, shadow_map_loc, shadow_up_loc;
	// aerial perspective: see renderSetAtmosphere
	GLint fog_loc, haze_loc, tint_loc, fog_planes_loc, cam_loc, eye_loc, cam_params_loc, viewport_loc;
	GLuint program;
	GLint transform_loc;
	GLint tube_on_loc;
	GLint tube_mesh_to_local_loc;
	GLint tube_local_to_clip_loc;
	GLint tube_params_loc;
	GLint tube_pass_loc;
	GLint tube_fade_loc;
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

void bufferMesh(rocktree_t::node_t::mesh_t &mesh, const gl_ctx_t &ctx) {
	if (mesh.buffered) fprintf(stderr, "mesh already buffered\n"), abort();

	// this runs mid-draw, with the last drawn mesh's vao still bound, and
	// the element buffer binding is vao state: uploading the index buffers
	// below with that vao bound rewrote *its* binding, so that tile drew one
	// frame with this tile's indices — a flash of sky through the terrain
	// every time a tile arrived
	glBindVertexArray(0);

	glGenBuffers(1, &mesh.vertex_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(unsigned char), mesh.vertices.data(), GL_STATIC_DRAW);
	glGenBuffers(1, &mesh.index_buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(unsigned short), mesh.indices.data(), GL_STATIC_DRAW);
	glGenBuffers(1, &mesh.boundary_index_buffer);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.boundary_index_buffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.boundary_indices.size() * sizeof(unsigned short), mesh.boundary_indices.data(), GL_STATIC_DRAW);

	// the vertex setup, recorded once here rather than re-issued per draw:
	// a settled horizon view draws well over a thousand nodes a frame, and
	// at that count the per-node bind sequence is what the frame costs on
	// the main thread (webgl calls are not cheap, and a busy main thread is
	// what starves the tile fetches). a vao holds the attribute pointers
	// and the element buffer binding, so a draw is one bind and one call.
	// (a second vao for the crack-fill lines, over the same vertices, drew
	// garbage — streaks across the sky — so the lines swap the element
	// buffer in this one instead; see bindAndDrawMesh)
	glGenVertexArrays(1, &mesh.vao);
	glBindVertexArray(mesh.vao);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.vertex_buffer);
	glEnableVertexAttribArray(ctx.position_loc);
	glEnableVertexAttribArray(ctx.octant_loc);
	glEnableVertexAttribArray(ctx.texcoords_loc);
	glVertexAttribPointer(ctx.position_loc, 3, GL_UNSIGNED_BYTE, GL_FALSE, 8, (void*)0);
	glVertexAttribPointer(ctx.octant_loc, 1, GL_UNSIGNED_BYTE, GL_FALSE, 8, (void*)3);
	glVertexAttribPointer(ctx.texcoords_loc, 2, GL_UNSIGNED_SHORT, GL_FALSE, 8, (void*)4);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
	glBindVertexArray(0);

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
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			texture_mipmaps_on ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		mesh.mips_on = texture_mipmaps_on;
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

void bindAndDrawMesh(rocktree_t::node_t::mesh_t &mesh, uint8_t octant_mask, uint32_t stale_mask, const gl_ctx_t &ctx) {
	glUniform2fv(ctx.uv_offset_loc, 1, mesh.uv_offset.data());
	glUniform2fv(ctx.uv_scale_loc, 1, mesh.uv_scale.data());
	// the masks go up as the bytes they are; the shader picks the bit
	glUniform1i(ctx.octant_mask_loc, octant_mask);
	glUniform1i(ctx.stale_mask_loc, (GLint)stale_mask);
	glBindTexture(GL_TEXTURE_2D, mesh.texture_buffer);
	// the M key flips texture_mipmaps_on live so it can be A/B'd on a scene
	// (the temporal difference, shimmer as the camera moves, is the whole
	// point and a still frame barely shows it); the parameter follows it on
	// the next draw of each tile, and is otherwise left alone
	if (mesh.texture_format == rocktree_t::texture_format_rgb && mesh.mips_on != texture_mipmaps_on) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			texture_mipmaps_on ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		mesh.mips_on = texture_mipmaps_on;
	}
	glBindVertexArray(mesh.vao);
	glDrawElements(GL_TRIANGLE_STRIP, mesh.indices.size(), GL_UNSIGNED_SHORT, NULL);

	// crack fill: re-draw the mesh's boundary edges as lines. in the slit
	// pixels along tile seams nothing has been drawn, so the lines win the
	// depth test there and nowhere else
	static const bool no_lines_debug = getenv("EARTH_NO_LINES") != nullptr;
	if (!no_lines_debug && mesh.boundary_indices.size()) {
		// the vao's element binding, swapped for the lines and put back
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.boundary_index_buffer);
		glDrawElements(GL_LINES, mesh.boundary_indices.size(), GL_UNSIGNED_SHORT, NULL);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
	}
}

void unbufferMesh(rocktree_t::node_t::mesh_t &mesh) {
	if (!mesh.buffered) fprintf(stderr, "mesh isn't buffered\n"), abort();

	mesh.buffered = false;
	
	glDeleteVertexArrays(1, &mesh.vao);
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
	// the scene goes to the offscreen target whenever a pass reads it back
	auto offscreen = ctx.dof.on || ctx.accum.on;
	if (offscreen) renderDofEnsure(ctx, width, height);
	ctx.scene_fbo = offscreen ? ctx.dof.fbo : 0;
	ctx.scene_w = width;
	ctx.scene_h = height;
	glBindFramebuffer(GL_FRAMEBUFFER, ctx.scene_fbo);
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
// parameters (effective roll radius, rect half length/width in meters).
// fade, in rect units along the axis: where the walls start darkening, to
// black at the rect's ends; 0 is no fade
void renderSetTube(render_ctx_t &ctx, bool on, const float *local_to_clip16,
		float r_eff, float half_len, float half_wid, float wid_offset, float fade = 0) {
	glUniform1i(ctx.tube_on_loc, on);
	glDisable(GL_POLYGON_OFFSET_FILL); // body mode's flat pass turns it on per node
	// draw both faces in tube mode: the camera legitimately sees walls from
	// either side (approaching the tube from outside, or geometry the roll
	// has folded over), and culling them reads as holes in the mesh
	if (on) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
	if (!on) return;
	glUniformMatrix4fv(ctx.tube_local_to_clip_loc, 1, GL_FALSE, local_to_clip16);
	glUniform4f(ctx.tube_params_loc, r_eff, half_len, half_wid, wid_offset);
	glUniform1f(ctx.tube_fade_loc, fade);
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
// the accumulator's target, (re)made at the canvas size. a float texture
// holding the running average (not the sum: a half float summing 128 frames
// rounds every addition to a sixteenth once past 64, and the error, varying
// with the value, drew contour bands across the sky)
void renderAccumEnsure(render_ctx_t &ctx, int w, int h) {
	auto &a = ctx.accum;
	if (a.fbo && a.w == w && a.h == h) return;
	if (!a.fbo) glGenFramebuffers(1, &a.fbo);
	if (!a.tex) glGenTextures(1, &a.tex);
	glBindTexture(GL_TEXTURE_2D, a.tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.tex, 0);
	auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		printf("glamour: no float render target (0x%x); accumulation off\n", status);
		a.failed = true;
		a.on = false;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	a.w = w;
	a.h = h;
	a.samples = 0;
}

GLuint makeShader(const char* vert_src, const char* frag_src);

void renderAccumInit(render_ctx_t &ctx) {
	auto &a = ctx.accum;
	static const char *fullscreen_vs =
		"#version 300 es\n"
		"out vec2 v_uv;"
		"void main() {"
		"	vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);"
		"	v_uv = p * 0.5 + 0.5;"
		"	gl_Position = vec4(p, 0.0, 1.0);"
		"}";
	a.present_program = makeShader(fullscreen_vs,
		"#version 300 es\n"
		"precision highp float;"
		"uniform sampler2D tex;"
		"uniform float scale;" // 1 / samples
		"in vec2 v_uv;"
		"out vec4 frag_color;"
		"void main() { frag_color = vec4(texture(tex, v_uv).rgb * scale, 1.0); }");
	a.present_tex_loc = glGetUniformLocation(a.present_program, "tex");
	a.present_scale_loc = glGetUniformLocation(a.present_program, "scale");
	a.blit_program = makeShader(fullscreen_vs,
		"#version 300 es\n"
		"precision highp float;"
		"uniform sampler2D tex;"
		"in vec2 v_uv;"
		"out vec4 frag_color;"
		"void main() { frag_color = vec4(texture(tex, v_uv).rgb, 1.0); }");
	a.blit_tex_loc = glGetUniformLocation(a.blit_program, "tex");
	// ambient occlusion from the depth buffer: each pixel's view-space
	// position and face normal, and a spiral of taps around it out to a
	// radius in metres (a fraction of the distance, held between half a
	// metre and thirty); a tap occludes by how far above the surface's
	// plane it sits, and not at all past the radius, so a building's
	// silhouette doesn't darken the distant ground behind it. the cap is
	// high because street canyons are: a 30 m one left every wall of a
	// manhattan street outside the radius
	a.ao_program = makeShader(fullscreen_vs,
		"#version 300 es\n"
		"precision highp float;"
		"uniform sampler2D color_tex;"
		"uniform sampler2D depth_tex;"
		"uniform vec2 viewport;"
		"uniform vec3 planes;" // near, far, ortho flag
		"uniform vec2 proj;" // tan(fov/2) (ortho: half height, metres), aspect
		"uniform vec3 params;" // strength, rotation, radius as a fraction of distance
		"in vec2 v_uv;"
		"out vec4 frag_color;"
		"float lin(float d) {"
		"	if (planes.z > 0.5) return planes.x + d * (planes.y - planes.x);"
		"	float z = d * 2.0 - 1.0;"
		"	return 2.0 * planes.x * planes.y / (planes.y + planes.x - z * (planes.y - planes.x));"
		"}"
		"vec3 pos(vec2 uv) {"
		"	float z = lin(texture(depth_tex, uv).r);"
		"	vec2 n = uv * 2.0 - 1.0;"
		"	if (planes.z > 0.5) return vec3(n.x * proj.x * proj.y, n.y * proj.x, -z);"
		"	return vec3(n.x * proj.x * proj.y * z, n.y * proj.x * z, -z);"
		"}"
		"void main() {"
		"	vec3 c = texture(color_tex, v_uv).rgb;"
		"	if (texture(depth_tex, v_uv).r >= 1.0 || params.x == 0.0) { frag_color = vec4(params.x < 0.0 ? vec3(1.0) : c, 1.0); return; }"
		"	vec3 p = pos(v_uv);"
		"	vec3 n = normalize(cross(dFdx(p), dFdy(p)));"
		"	if (dot(n, p) > 0.0) n = -n;" // toward the camera
		"	float z = -p.z;"
		"	float R = clamp(params.z * z, 0.5, 150.0);"
		"	float rpx = planes.z > 0.5 ? R / (2.0 * proj.x) * viewport.y : R / (z * proj.x) * 0.5 * viewport.y;"
		"	rpx = min(rpx, 0.08 * viewport.y);"
		"	float occ = 0.0;"
		"	const int N = 16;"
		"	for (int i = 0; i < N; i++) {"
		"		float r = sqrt((float(i) + 0.5) / float(N)) * rpx;"
		"		float a = float(i) * 2.39996 + params.y;"
		"		vec2 uv = v_uv + vec2(cos(a), sin(a)) * r / viewport;"
		"		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) continue;"
		"		vec3 v = pos(uv) - p;"
		"		float l = length(v);"
		"		if (l < 1e-4) continue;"
		"		occ += max(0.0, dot(v / l, n) - 0.1) * (1.0 - smoothstep(0.7 * R, R, l));"
		"	}"
		"	float ao = clamp(1.0 - abs(params.x) * 1.6 * occ / float(N), 0.0, 1.0);"
		// a negative strength shows the occlusion alone (?aodebug)
		"	frag_color = vec4(params.x < 0.0 ? vec3(ao) : c * ao, 1.0);"
		"}");
	a.ao_color_loc = glGetUniformLocation(a.ao_program, "color_tex");
	a.ao_depth_loc = glGetUniformLocation(a.ao_program, "depth_tex");
	a.ao_viewport_loc = glGetUniformLocation(a.ao_program, "viewport");
	a.ao_planes_loc = glGetUniformLocation(a.ao_program, "planes");
	a.ao_proj_loc = glGetUniformLocation(a.ao_program, "proj");
	a.ao_params_loc = glGetUniformLocation(a.ao_program, "params");
}

void dofDraw(render_ctx_t &ctx);

// the scene is drawn; put it on the screen. plain frames go straight there.
// with the miniature on, the scene was drawn offscreen and the blur pass
// writes it out. in glamour mode the pass (or a copy) is summed into the
// accumulator instead, and the average of the samples so far is presented
void renderFrameEnd(render_ctx_t &ctx) {
	auto &d = ctx.dof;
	auto &a = ctx.accum;
	if (!d.on && !a.on) return;
	glDisable(GL_DEPTH_TEST);
	if (a.on) {
		renderAccumEnsure(ctx, d.w, d.h);
		if (a.on) { // (ensure can turn it off)
			glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
			glViewport(0, 0, a.w, a.h);
			if (!a.hold || a.samples == 0) {
				// the n-th sample goes in with weight 1/n: the first replaces
				// whatever was there, and the texture is always the average
				glEnable(GL_BLEND);
				glBlendColor(0, 0, 0, 1.0f / (float)(a.samples + 1));
				glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
				if (d.on) dofDraw(ctx);
				else if (a.ao_on && a.ao_strength > 0) {
					glUseProgram(a.ao_program);
					glBindVertexArray(d.vao);
					glActiveTexture(GL_TEXTURE1);
					glBindTexture(GL_TEXTURE_2D, d.depth);
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, d.color);
					glUniform1i(a.ao_color_loc, 0);
					glUniform1i(a.ao_depth_loc, 1);
					glUniform2f(a.ao_viewport_loc, (float)a.w, (float)a.h);
					glUniform3f(a.ao_planes_loc, a.near, a.far, a.ortho ? 1.0f : 0.0f);
					glUniform2f(a.ao_proj_loc, a.tan_half, a.aspect);
					glUniform3f(a.ao_params_loc, ao_debug ? -a.ao_strength : a.ao_strength, a.ao_rotation, 0.04f);
					glDrawArrays(GL_TRIANGLES, 0, 3);
				} else {
					glUseProgram(a.blit_program);
					glBindVertexArray(d.vao);
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, d.color);
					glUniform1i(a.blit_tex_loc, 0);
					glDrawArrays(GL_TRIANGLES, 0, 3);
				}
				glDisable(GL_BLEND);
				a.samples++;
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glViewport(0, 0, a.w, a.h);
			glUseProgram(a.present_program);
			glBindVertexArray(d.vao);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, a.tex);
			glUniform1i(a.present_tex_loc, 0);
			glUniform1f(a.present_scale_loc, 1.0f);
			glDrawArrays(GL_TRIANGLES, 0, 3);
			glBindVertexArray(0);
		}
	}
	if (!a.on) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, d.w, d.h);
		dofDraw(ctx);
	}
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0); // the terrain textures live on unit 0
	glEnable(GL_DEPTH_TEST);
	glUseProgram(ctx.program);
}

// the miniature's blur pass over the offscreen scene, into whatever
// framebuffer is bound
void dofDraw(render_ctx_t &ctx) {
	auto &d = ctx.dof;
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
// the terrain program's aerial-perspective uniforms, once per frame. the
// terrain program must be current
void renderSetAtmosphere(render_ctx_t &ctx, bool on, float visibility_m, const float *haze3,
		const float *tint3, float near, float far, bool ortho,
		const float *cam9, const float *eye3, float tan_half_fov, float aspect, float planet_r, float scale_h,
		int width, int height) {
	glUniform4f(ctx.fog_loc, visibility_m, on ? 1.0f : 0.0f, 0.0f, 0.0f);
	glUniform3fv(ctx.haze_loc, 1, haze3);
	glUniform3fv(ctx.tint_loc, 1, tint3);
	glUniform3f(ctx.fog_planes_loc, near, far, ortho ? 1.0f : 0.0f);
	glUniformMatrix3fv(ctx.cam_loc, 1, GL_FALSE, cam9);
	glUniform3fv(ctx.eye_loc, 1, eye3);
	glUniform4f(ctx.cam_params_loc, tan_half_fov, aspect, planet_r, scale_h);
	glUniform2f(ctx.viewport_loc, (float)width, (float)height);
}

// (re)allocate the shadow map at size x size. the placeholder is 1x1
void renderShadowEnsure(render_ctx_t &ctx, int size) {
	auto &s = ctx.shadow;
	if (s.tex && s.size == size) return;
	if (!s.tex) glGenTextures(1, &s.tex);
	glActiveTexture(GL_TEXTURE0 + shadow_unit);
	glBindTexture(GL_TEXTURE_2D, s.tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glActiveTexture(GL_TEXTURE0);
	if (!s.fbo) glGenFramebuffers(1, &s.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, s.tex, 0);
	GLenum none = GL_NONE;
	glDrawBuffers(1, &none);
	glReadBuffer(GL_NONE);
	auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) printf("shadow framebuffer incomplete: 0x%x\n", status);
	glBindFramebuffer(GL_FRAMEBUFFER, ctx.scene_fbo);
	s.size = size;
}

void renderShadowInit(render_ctx_t &ctx) {
	auto &s = ctx.shadow;
	s.program = makeShader(
		"#version 300 es\n"
		"uniform mat4 shadow_mv;"
		"uniform vec4 shadow_box;" // half x, half y, near, far (metres along the sun)
		"uniform int octant_mask;"
		"layout(location = 0) in vec3 position;"
		"layout(location = 1) in float octant;"
		"out float v_mask;"
		"void main() {"
		"	vec3 v = (shadow_mv * vec4(position, 1.0)).xyz;"
		// the octants a finer drawn tile covers cast through that tile,
		// not through this coarser copy of the same ground — and unlike the
		// colour pass, which pushes a triangle straddling the boundary back
		// in depth, here any part-masked one goes: a coarse triangle sitting
		// above the fine surface would shade it
		"	v_mask = ((octant_mask >> int(octant)) & 1) != 0 ? 0.0 : 1.0;"
		"	gl_Position = vec4(v.xy / shadow_box.xy,"
		"		(-v.z - shadow_box.z) / (shadow_box.w - shadow_box.z) * 2.0 - 1.0, 1.0);"
		"}",
		"#version 300 es\n"
		"precision highp float;"
		"in float v_mask;"
		"void main() { if (v_mask < 0.999) discard; }"
	);
	s.mv_loc = glGetUniformLocation(s.program, "shadow_mv");
	s.box_loc = glGetUniformLocation(s.program, "shadow_box");
	s.mask_loc = glGetUniformLocation(s.program, "octant_mask");
	renderShadowEnsure(ctx, 1);
}

// the shadow pass: into the map, depth only, both faces (terrain is a
// sheet, not a solid), with a slope-scaled depth offset against acne
static bool shadow_cull_was = true;
void renderShadowBegin(render_ctx_t &ctx, int size, const float *box4) {
	auto &s = ctx.shadow;
	renderShadowEnsure(ctx, size);
	glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
	glViewport(0, 0, s.size, s.size);
	glClear(GL_DEPTH_BUFFER_BIT);
	glUseProgram(s.program);
	glUniform4fv(s.box_loc, 1, box4);
	shadow_cull_was = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_CULL_FACE);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(2.0f, 2.0f);
}

void renderShadowNode(render_ctx_t &ctx, const float *mv16, uint8_t octant_mask) {
	glUniformMatrix4fv(ctx.shadow.mv_loc, 1, GL_FALSE, mv16);
	glUniform1i(ctx.shadow.mask_loc, octant_mask);
}

void renderShadowMesh(const rocktree_t::node_t::mesh_t &mesh) {
	glBindVertexArray(mesh.vao);
	glDrawElements(GL_TRIANGLE_STRIP, mesh.indices.size(), GL_UNSIGNED_SHORT, NULL);
}

void renderShadowEnd(render_ctx_t &ctx) {
	glBindVertexArray(0);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(0.0f, 0.0f);
	if (shadow_cull_was) glEnable(GL_CULL_FACE);
	glBindFramebuffer(GL_FRAMEBUFFER, ctx.scene_fbo);
	glViewport(0, 0, ctx.scene_w, ctx.scene_h);
	glUseProgram(ctx.program);
}

// the terrain program's shadow uniforms: per frame, then per node
void renderSetShadow(render_ctx_t &ctx, bool on, const float *box4, float strength, float texel_uv, float bias,
		const float *up3) {
	glUniform4f(ctx.shadow_params_loc, on ? (float)shadow_debug : 0.0f, strength, texel_uv, bias);
	if (box4) glUniform4fv(ctx.shadow_box_loc, 1, box4);
	if (up3) glUniform3fv(ctx.shadow_up_loc, 1, up3);
}

void renderSetShadowNode(render_ctx_t &ctx, const float *mv16) {
	glUniformMatrix4fv(ctx.shadow_mv_loc, 1, GL_FALSE, mv16);
}

void renderSkyInit(render_ctx_t &ctx) {
	auto &s = ctx.sky;
	s.program = makeShader(
		"#version 300 es\n"
		"out vec2 v_uv;"
		"void main() {"
		"	vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);"
		"	v_uv = p * 0.5 + 0.5;"
		// at the far plane exactly: it shows wherever nothing was drawn
		"	gl_Position = vec4(p, 1.0, 1.0);"
		"}",
		"#version 300 es\n"
		"precision highp float;"
		"uniform vec3 cam_right, cam_up, cam_fwd;" // the camera's frame, world
		"uniform vec3 local_up;" // straight up at the eye
		"uniform vec3 sun;" // toward the sun, unit
		"uniform vec4 params;" // tan(fov/2), aspect, horizon dip below level (rad), sun elevation (rad)
		"uniform vec3 zenith, haze;"
		// how much sky there is above the eye (1 on the ground, 0 in
		// space) and the angular thickness of the limb seen from there
		"uniform vec2 alt_params;"
		"in vec2 v_uv;"
		"out vec4 frag_color;"
		"void main() {"
		"	vec2 n = v_uv * 2.0 - 1.0;"
		"	vec3 dir = normalize(cam_fwd + cam_right * (n.x * params.x * params.y) + cam_up * (n.y * params.x));"
		"	float el = asin(clamp(dot(dir, local_up), -1.0, 1.0));"
		// height above the horizon line, which sits below level by the dip
		// (more the higher you are)
		"	float h = el + params.z;"
		"	vec3 c;"
		"	float d = max(dot(dir, sun), 0.0);"
		"	vec3 sunc = vec3(1.0, 0.96, 0.88);"
		"	if (h >= 0.0) {"
		// haze at the horizon, the zenith colour high up; the hazy band is
		// thick, as it is, and the gradient slow. the whole of it thins
		// away with altitude (space is black), leaving the limb: the
		// atmosphere seen edge-on just above the horizon line
		"		c = mix(haze, zenith, pow(clamp(h / 1.3, 0.0, 1.0), 0.5)) * alt_params.x;"
		"		c += haze * (1.0 - alt_params.x) * exp(-h / alt_params.y);"
		// the disc (half a degree across), its glow, and a wide forward
		// scatter that brightens the sky around a low sun
		"		c += sunc * (0.5 * pow(d, 1400.0) + (0.14 * pow(d, 24.0) + 0.05 * pow(d, 3.0)) * alt_params.x);"
		"		if (d > 0.99996) c = sunc * 1.4;"
		"	} else {"
		// below the horizon line nothing is drawn only past the far plane:
		// the planet's limb from high up. darken toward it
		"		c = mix(haze, haze * 0.3, clamp(-h * 2.5, 0.0, 1.0)) * alt_params.x;"
		"	}"
		"	frag_color = vec4(clamp(c, 0.0, 1.0), 1.0);"
		"}"
	);
	s.right_loc = glGetUniformLocation(s.program, "cam_right");
	s.up_loc = glGetUniformLocation(s.program, "cam_up");
	s.fwd_loc = glGetUniformLocation(s.program, "cam_fwd");
	s.local_up_loc = glGetUniformLocation(s.program, "local_up");
	s.sun_loc = glGetUniformLocation(s.program, "sun");
	s.params_loc = glGetUniformLocation(s.program, "params");
	s.zenith_loc = glGetUniformLocation(s.program, "zenith");
	s.haze_loc = glGetUniformLocation(s.program, "haze");
	s.alt_loc = glGetUniformLocation(s.program, "alt_params");
}

// after the terrain: the sky fills whatever the depth buffer still has at
// the far plane. leaves the terrain program current
void renderSkyDraw(render_ctx_t &ctx, const float *right3, const float *up3, const float *fwd3,
		const float *local_up3, const float *sun3, float tan_half_fov, float aspect,
		float horizon_dip, float sun_el, const float *zenith3, const float *haze3,
		float sky_amount, float limb_angle) {
	auto &s = ctx.sky;
	auto cull = glIsEnabled(GL_CULL_FACE);
	glUseProgram(s.program);
	glUniform3fv(s.right_loc, 1, right3);
	glUniform3fv(s.up_loc, 1, up3);
	glUniform3fv(s.fwd_loc, 1, fwd3);
	glUniform3fv(s.local_up_loc, 1, local_up3);
	glUniform3fv(s.sun_loc, 1, sun3);
	glUniform4f(s.params_loc, tan_half_fov, aspect, horizon_dip, sun_el);
	glUniform3fv(s.zenith_loc, 1, zenith3);
	glUniform3fv(s.haze_loc, 1, haze3);
	glUniform2f(s.alt_loc, sky_amount, limb_angle);
	glDepthFunc(GL_LEQUAL); // equal to the cleared depth, where nothing was drawn
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(ctx.dof.vao); // the empty one
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	if (cull) glEnable(GL_CULL_FACE);
	glUseProgram(ctx.program);
}

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
		// octant_mask: one bit per octant. stale_mask: three bits per
		// octant, the fetch phase of the finer tile wanted there (0 none;
		// see earth_core's draw loop for the legend)
		"uniform int octant_mask;"
		"uniform int stale_mask;"
		// near, far, ortho flag: the fragment's distance from the eye for
		// the aerial perspective comes out of the clip position
		"uniform vec3 fog_planes;"
		"out float v_dist;"
		// the sun's view of this vertex: metres, in a frame centred on the
		// shadow map's box with +z toward the sun (see renderShadowBegin)
		"uniform mat4 shadow_mv;"
		"out vec3 v_sunv;"
		// fixed locations: the shadow pass's program reads the same vaos
		"layout(location = 0) in vec3 position;"
		"layout(location = 1) in float octant;"
		"layout(location = 2) in vec2 texcoords;"
		"out vec2 v_texcoords;"
		"flat out float v_stale;"
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
		"	float mask = ((octant_mask >> int(octant)) & 1) != 0 ? 0.0 : 1.0;"
		"	v_mask = mask;"
		"	v_stale = float((stale_mask >> (3 * int(octant))) & 7);"
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
		"	v_sunv = (shadow_mv * vec4(position, 1.0)).xyz;"
		// view depth: w in perspective; in ortho w is 1 and ndc z is linear
		"	v_dist = fog_planes.z > 0.5"
		"		? fog_planes.x + (p.z / p.w * 0.5 + 0.5) * (fog_planes.y - fog_planes.x) : p.w;"
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
		// aerial perspective: fog.x the visibility (m) at sea level, fog.y
		// on/off; haze what distance fades toward; tint the sun's colour on
		// the ground (warm and dim at a low sun, dark at night). the camera
		// frame and eye (world, float — metres of precision are plenty for
		// a height) recover the fragment's world height from its depth
		"uniform vec4 fog;"
		"uniform vec3 haze;"
		"uniform vec3 tint;"
		"uniform mat3 cam;" // columns: right, up, forward
		"uniform vec3 eye;"
		"uniform vec3 fog_planes;" // (the vertex stage's; same uniform)
		"uniform vec4 cam_params;" // tan(fov/2), aspect, planet radius, scale height
		"uniform vec2 viewport;"
		"in float v_dist;"
		// sun shadows: x on, y strength (how dark the shade is), z one
		// shadow texel in uv, w the depth bias; box is the map's half
		// widths and its near/far along the sun (metres)
		"uniform highp sampler2DShadow shadow_map;"
		"uniform vec4 shadow_params;"
		"uniform vec4 shadow_box;"
		"uniform vec3 shadow_up;" // straight up at the eye, in the sun's frame
		"in vec3 v_sunv;"
		"uniform float tube_fade;"
		"uniform bool body_on;"
		"in vec2 v_texcoords;"
		"in float v_loop;"
		"in vec3 v_local;"
		"flat in float v_stale;"
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
		"	vec3 c = texture(tex, v_texcoords).rgb * tint;"
		// the sun: faces turned away from it are in their own shade (the
		// face normal from the screen-space slope of the sun-frame
		// position, which points at the camera, so +z is toward the sun),
		// and inside the map's box so is whatever something nearer the sun
		// hides. the shade is lit by the sky instead, so it goes a little
		// blue. 3x3 taps over the hardware's 2x2 comparison filter
		"	if (shadow_params.x > 0.5 && !tube_on) {"
		// a crack-fill line has no face: its normal would be nan
		"		vec3 cr = cross(dFdx(v_sunv), dFdy(v_sunv));"
		"		vec3 n = dot(cr, cr) > 1e-20 ? normalize(cr) : shadow_up;"
		// relative to flat ground, so level land is fully lit at any sun
		// height, and only faces actually turned from the sun darken —
		// walls, the far side of a ridge. anything gentler is left alone:
		// the photogrammetry's water is bumpy (reflections defeat the
		// stereo), and a low sun turned each bump into a hard facet
		"		float lit = smoothstep(-0.3, 0.1, n.z / max(shadow_up.z, 0.05));"
		// debug (?shadowdebug=): 2 the facing term alone, 3 cast shadows alone
		"		if (shadow_params.x > 2.5) lit = 1.0;"
		// normal offset: look the map up a texel and a half off the
		// surface, so a surface at a grazing sun doesn't shadow itself
		"		float texel_m = shadow_params.z * 2.0 * shadow_box.x;"
		"		vec3 q = v_sunv + n * (1.5 * texel_m);"
		"		vec2 uv = q.xy / (2.0 * shadow_box.xy) + 0.5;"
		"		float d = (-q.z - shadow_box.z) / (shadow_box.w - shadow_box.z) - shadow_params.w;"
		"		if (shadow_params.x < 1.5 || shadow_params.x > 2.5)"
		"		if (uv.x > 0.0 && uv.y > 0.0 && uv.x < 1.0 && uv.y < 1.0 && d < 1.0) {"
		"			float s = 0.0;"
		"			for (int i = -1; i <= 1; i++)"
		"				for (int j = -1; j <= 1; j++)"
		"					s += texture(shadow_map, vec3(uv + vec2(float(i), float(j)) * shadow_params.z, d));"
		"			vec2 e = min(uv, 1.0 - uv);"
		"			float edge = smoothstep(0.0, 0.04, min(e.x, e.y));"
		"			lit = min(lit, mix(1.0, s / 9.0, edge));"
		"		}"
		"		vec3 shade = (1.0 - shadow_params.y) * vec3(0.88, 0.95, 1.12);"
		"		c *= mix(shade, vec3(1.0), lit);"
		"	}"
		// aerial perspective: light from a distance is scattered out on the
		// way and the sky's light scattered in takes its place, so what is
		// far fades toward the haze — per channel, the red surviving the
		// trip best, which is why distance goes blue-grey. not in the tube,
		// whose distances are through the roll and whose ends fade to
		// black on their own
		"	if (fog.y > 0.5 && !tube_on) {"
		// the air thins with height, so the optical depth of the way
		// here is the distance times the mean density along it — taken
		// between the eye's height and this fragment's, exponential in
		// height with the scale height H. from orbit that leaves the
		// planet clear with a hazy limb instead of a pale disc
		"		vec2 ndc = gl_FragCoord.xy / viewport * 2.0 - 1.0;"
		"		vec3 off = vec3(ndc.x * cam_params.x * cam_params.y, ndc.y * cam_params.x, 1.0);"
		"		if (fog_planes.z > 0.5) off = vec3(0.0, 0.0, 1.0);"
		"		vec3 w = eye + cam * (off * v_dist);"
		"		float H = cam_params.w;"
		"		float ha = max(length(eye) - cam_params.z, 0.0) / H;"
		"		float hf = max(length(w) - cam_params.z, 0.0) / H;"
		"		float dh = ha - hf;"
		"		float mean = abs(dh) < 1e-3 ? exp(-hf) : (exp(-hf) - exp(-ha)) / dh;"
		"		float d = v_dist * length(off) * mean;"
		"		vec3 e = exp(-d / (fog.x * vec3(1.3, 1.0, 0.75)));"
		"		c = mix(haze, c, e);"
		"	}"
		// the lod overlay: a finer tile is wanted here and isn't drawn yet;
		// the colour says where its request is. yellow: not yet requested
		// (queued behind the download slots). blue: being looked up in
		// the tile cache. red: on the network. green: decoding. magenta:
		// failed, waiting to retry
		"	if (debug_lod && v_stale > 0.5) {"
		"		int ph = int(v_stale + 0.5);"
		"		vec3 t = ph == 1 ? vec3(1.0, 0.9, 0.0) : ph == 2 ? vec3(0.1, 0.5, 1.0)"
		"			: ph == 3 ? vec3(1.0, 0.0, 0.0) : ph == 4 ? vec3(0.0, 1.0, 0.2) : vec3(1.0, 0.0, 1.0);"
		"		c = mix(c, t, 0.5);"
		"	}"
		// the tunnel down the heading: the walls go dark toward both ends,
		// so the pipe runs off into black rather than stopping at a hole
		"	if (tube_on && tube_fade > 0.0) c *= 1.0 - smoothstep(tube_fade, 1.0, abs(v_rect.x));"
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
	ctx.tube_fade_loc = glGetUniformLocation(ctx.program, "tube_fade");
	ctx.fog_loc = glGetUniformLocation(ctx.program, "fog");
	ctx.haze_loc = glGetUniformLocation(ctx.program, "haze");
	ctx.tint_loc = glGetUniformLocation(ctx.program, "tint");
	ctx.fog_planes_loc = glGetUniformLocation(ctx.program, "fog_planes");
	ctx.cam_loc = glGetUniformLocation(ctx.program, "cam");
	ctx.eye_loc = glGetUniformLocation(ctx.program, "eye");
	ctx.cam_params_loc = glGetUniformLocation(ctx.program, "cam_params");
	ctx.viewport_loc = glGetUniformLocation(ctx.program, "viewport");
	ctx.shadow_mv_loc = glGetUniformLocation(ctx.program, "shadow_mv");
	ctx.shadow_box_loc = glGetUniformLocation(ctx.program, "shadow_box");
	ctx.shadow_params_loc = glGetUniformLocation(ctx.program, "shadow_params");
	ctx.shadow_map_loc = glGetUniformLocation(ctx.program, "shadow_map");
	ctx.shadow_up_loc = glGetUniformLocation(ctx.program, "shadow_up");
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

	// (the attribute arrays are enabled per mesh, in its vao — bufferMesh)

	renderPathInit(ctx);
	renderDofInit(ctx);
	renderSkyInit(ctx);
	renderAccumInit(ctx);
	renderShadowInit(ctx);
	glUseProgram(ctx.program); // the overlay inits left their own programs current
	glUniform1i(ctx.shadow_map_loc, shadow_unit);
	glUniform4f(ctx.shadow_params_loc, 0.0f, 0.0f, 0.0f, 0.0f);
	glUniform1i(ctx.texture_loc, 0); // the terrain textures live on unit 0, always
	glUniform4f(ctx.fog_loc, 60000.0f, 0.0f, 0.0f, 0.0f);
	glUniform3f(ctx.tint_loc, 1.0f, 1.0f, 1.0f);
}
