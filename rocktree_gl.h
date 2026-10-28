#pragma once

#include <GLES3/gl3.h>
// gl3.h has no extension enums; the s3tc/dxt formats live here (gl2ext is the
// shared extension header — it applies to gles3 contexts too)
#include <GLES2/gl2ext.h>
#include <iostream>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "earth_shaders.h"
#include "rocktree_types.h"

// what the engine works out about a frame, once (earth_core's setupFrame
// and setAtmosphere), for its own phases and for the passes here: the
// camera as drawn (the eye and direction moved by the lens offset, and its
// right and up), the clip range and the matrices, the atmosphere and the
// sun. the defaults are what the passes read before the first frame
struct frame_t {
	int width = 0, height = 0;
	Vector3d eye, direction, right, cam_up;
	double near = 1, far = 1e6;
	bool ortho = false;
	double ortho_half_extent = 0; // vertical half-extent in meters (ortho only)
	double tan_half = 0.414; // tan(fov / 2), in ortho too
	float aspect_ratio = 1.6f;
	Matrix4d viewprojection;
	Matrix4d key_vp; // the unmoved camera, before the jitter: the walk's cache key
	Vector3d key_eye, key_direction, key_right, key_up; // the unmoved camera's frame (no lens offset)
	bool tube = false; // tube mode (see earth_tube.h)
	bool debug_lod = false; // tint octants still waiting for a finer tile
	// the miniature's sharp plane (meters) and the half-width of its sharp
	// band, scaled to the fov (see dof_gl_t); set only with it on
	float focus = 1000.0f, focus_range = 0.35f;
	// the atmosphere: the haze, the eye's local frame and the sun in it,
	// the sky's colours and the ground's tint
	double planet_radius = 0, scale_height = 0;
	bool haze_on = false;
	double visibility_m = 0;
	Vector3d local_up, east, sun_dir;
	double sun_el = 0;
	Vector3f zenith, haze, tint;
	// the sky pass's: the horizon's dip below level, how much sky is left
	// at this height, and the limb's angular width
	double horizon_dip = 0, sky_amount = 1, limb = 1e-3;
};

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
	// set by the host; the engine works out the focus, and scales the range
	// to the current fov, per frame (frame_t)
	float strength = 0.015f; // max blur radius as a fraction of the height
	float range_host = 0.35f; // half-width of the sharp band, as a fraction of focus, at a 45-degree fov
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
	// this sample's rotation. it reads depth back into positions with the
	// frame's camera
	bool ao_on = true;
	float ao_strength = 1.0f, ao_rotation = 0;
	bool ao_debug = false; // shows the occlusion alone
	// the occlusion is two passes: computed into a one-channel texture,
	// then blurred (in real time; a single tap in glamour mode, whose
	// average does the smoothing) and multiplied onto the scene
	// computed into ao_tex, blurred into ao_tex2 (both rg: occlusion, and
	// the depth it was computed at in km, so the passes after don't read
	// the full-size depth buffer again)
	GLuint ao_fbo = 0, ao_tex = 0, ao_fbo2 = 0, ao_tex2 = 0, ao_program = 0, ao_blur_program = 0, ao_comp_program = 0;
	GLuint ao_result = 0; // whichever of the two the composite reads
	GLint blur_src_loc;
	int ao_w = 0, ao_h = 0;
	// the occlusion texture's size as a divisor of the scene's (1 full, 2
	// half), and the taps per pixel
	int ao_scale = 2, ao_taps = 16;
	GLint ao_depth_loc, ao_viewport_loc, ao_planes_loc, ao_proj_loc, ao_params_loc, ao_taps_loc;
	GLint comp_color_loc, comp_ao_loc, comp_depth_loc, comp_planes_loc, comp_params_loc;
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
	int debug = 1; // 1 normal; 2 facing term only, 3 cast shadows only
};
static const int shadow_unit = 3;

// gpu timing (EXT_disjoint_timer_query_webgl2): the frame is cut into
// sections, each a run of time-elapsed queries — they can't nest, so
// switching section ends one query and starts the next — read back a few
// frames later, when the gpu has got round to them, and averaged per frame
// over about a second. cpu timers can't see any of this: webgl only queues
// the work, and the gpu does it afterwards
#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif
enum gpu_section : int { gpu_shadow = 0, gpu_scene = 1, gpu_occlusion = 2, gpu_post = 3, gpu_sections = 4 };
struct gpu_timer_t {
	bool ok = false; // the extension is there (set by the shell)
	struct pending_t { GLuint q; int section; int frame; };
	std::vector<GLuint> spare;
	std::vector<pending_t> pending; // oldest first
	GLuint active = 0;
	int active_section = -1, frame = 0, last_done = -1, frames_done = 0;
	double sum[gpu_sections] = {};
	double avg[gpu_sections] = {}; // ms per frame, the last second's
	double window_ms = 0;
};

struct gl_ctx_t {
	gpu_timer_t gpu;
	dof_gl_t dof;
	sky_gl_t sky;
	accum_gl_t accum;
	shadow_gl_t shadow;
	// multisampled stand-ins for the offscreen target's colour and depth,
	// resolved into its textures at the end of the frame: drawing straight
	// into textures loses the context's msaa, and every pass that reads the
	// scene back (occlusion, the miniature, glamour) went jagged with it.
	// samples set by the shell from the context's antialias attribute
	int msaa_samples = 0;
	GLuint msaa_fbo = 0, msaa_color = 0, msaa_depth = 0;
	int msaa_w = 0, msaa_h = 0;
	// the framebuffer the scene is being drawn into this frame, and its
	// size: each pass that draws into the scene binds it (renderBindScene)
	GLuint scene_fbo = 0;
	int scene_w = 0, scene_h = 0;
	GLint shadow_mv_loc, shadow_box_loc, shadow_params_loc, shadow_map_loc, shadow_up_loc, sun_rot_loc, eye_sun_loc;
	// aerial perspective: see renderTerrainBegin
	GLint fog_loc, haze_loc, tint_loc, fog_planes_loc, cam_loc, eye_loc, cam_params_loc, viewport_loc;
	GLint clay_loc, sun_dir_loc, ortho_half_loc;
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
	// largest anisotropy the gpu offers, 1 if the extension is missing
	float max_aniso = 1.0f;
	// live A/B switch (M key). only the uncompressed path has a mip chain to
	// switch to, so this does nothing on dxt tiles
	bool mipmaps_on = true;
	// clay (U key): every mesh one colour, lit by the sun and the sky
	bool clay_on = false;
	float clay_rgb[3] = {0.8f, 0.78f, 0.74f};
};

inline void meshTexImage2d(const rocktree_t::node_t::mesh_t &mesh) {
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

inline void bufferMesh(rocktree_t::node_t::mesh_t &mesh, const gl_ctx_t &ctx) {
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
	glGenBuffers(1, &mesh.normal_buffer);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.normal_buffer);
	glBufferData(GL_ARRAY_BUFFER, mesh.normals.size(), mesh.normals.data(), GL_STATIC_DRAW);

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
	glBindBuffer(GL_ARRAY_BUFFER, mesh.normal_buffer);
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 3, GL_BYTE, GL_TRUE, 4, (void*)0);
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
			ctx.mipmaps_on ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		mesh.mips_on = ctx.mipmaps_on;
		// trilinear alone picks its level from the *worst* axis, so it fixes
		// the aliasing by blurring the other one; anisotropy is what keeps a
		// grazing-angle tile sharp along its uncompressed axis
		if (ctx.max_aniso > 1.0f)
			glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, ctx.max_aniso);
	} else {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}

	mesh.buffered = true;
}

inline void bindAndDrawMesh(rocktree_t::node_t::mesh_t &mesh, uint8_t octant_mask, uint32_t stale_mask, const gl_ctx_t &ctx) {
	glUniform2fv(ctx.uv_offset_loc, 1, mesh.uv_offset.data());
	glUniform2fv(ctx.uv_scale_loc, 1, mesh.uv_scale.data());
	// the masks go up as the bytes they are; the shader picks the bit
	glUniform1i(ctx.octant_mask_loc, octant_mask);
	glUniform1i(ctx.stale_mask_loc, (GLint)stale_mask);
	glBindTexture(GL_TEXTURE_2D, mesh.texture_buffer);
	// the M key flips ctx.mipmaps_on live so it can be A/B'd on a scene
	// (the temporal difference, shimmer as the camera moves, is the whole
	// point and a still frame barely shows it); the parameter follows it on
	// the next draw of each tile, and is otherwise left alone
	if (mesh.texture_format == rocktree_t::texture_format_rgb && mesh.mips_on != ctx.mipmaps_on) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
			ctx.mipmaps_on ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		mesh.mips_on = ctx.mipmaps_on;
	}
	glBindVertexArray(mesh.vao);
	glDrawElements(GL_TRIANGLE_STRIP, mesh.indices.size(), GL_UNSIGNED_SHORT, NULL);

	// crack fill: re-draw the mesh's boundary edges as lines. in the slit
	// pixels along tile seams nothing has been drawn, so the lines win the
	// depth test there and nowhere else
	if (mesh.boundary_indices.size()) {
		// the vao's element binding, swapped for the lines and put back
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.boundary_index_buffer);
		glDrawElements(GL_LINES, mesh.boundary_indices.size(), GL_UNSIGNED_SHORT, NULL);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.index_buffer);
	}
}

inline void unbufferMesh(rocktree_t::node_t::mesh_t &mesh) {
	if (!mesh.buffered) fprintf(stderr, "mesh isn't buffered\n"), abort();

	mesh.buffered = false;
	
	glDeleteVertexArrays(1, &mesh.vao);
	glDeleteTextures(1, &mesh.texture_buffer); // auto: glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteBuffers(1, &mesh.index_buffer); // auto: glBindBuffer(GL_ARRAY_BUFFER, 0);
	glDeleteBuffers(1, &mesh.boundary_index_buffer);
	glDeleteBuffers(1, &mesh.normal_buffer);
	glDeleteBuffers(1, &mesh.vertex_buffer); // auto: glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

// the engine's way into gl: earth_core.h and earth_tube.h make no gl calls
// of their own, only these render* ones (the context's name is from a metal
// backend that once provided the same interface). a pass sets up the state
// it draws with as it starts — its target, program and culling — rather
// than counting on the pass before; what it changes from the resting state
// (depth test, writes and func, blending, texture unit 0 active) it puts back
typedef gl_ctx_t render_ctx_t;

// the offscreen targets' storage. a texture (generated the first time) at
// w x h, one level, clamped at the edges, filtered by filter both ways; left
// bound on the active unit
inline void makeTex2D(GLuint &tex, GLenum internal, GLenum format, GLenum type, int w, int h, GLenum filter) {
	if (!tex) glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// the same, as a multisampled renderbuffer; left bound
inline void makeRenderbuffer(GLuint &rb, int samples, GLenum internal, int w, int h) {
	if (!rb) glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internal, w, h);
}

// a framebuffer (generated the first time) with color and depth attached, 0
// for none: textures, or renderbuffers if target says so. depth only, it
// draws and reads no colour. left bound; returns its completeness
inline GLenum makeFbo(GLuint &fbo, GLuint color, GLuint depth, GLenum target = GL_TEXTURE_2D) {
	if (!fbo) glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	auto attach = [&](GLenum point, GLuint name) {
		if (!name) return;
		if (target == GL_RENDERBUFFER) glFramebufferRenderbuffer(GL_FRAMEBUFFER, point, GL_RENDERBUFFER, name);
		else glFramebufferTexture2D(GL_FRAMEBUFFER, point, GL_TEXTURE_2D, name, 0);
	};
	attach(GL_COLOR_ATTACHMENT0, color);
	attach(GL_DEPTH_ATTACHMENT, depth);
	if (!color) {
		GLenum none = GL_NONE;
		glDrawBuffers(1, &none);
		glReadBuffer(GL_NONE);
	}
	return glCheckFramebufferStatus(GL_FRAMEBUFFER);
}

inline void renderDofEnsure(render_ctx_t &ctx, int w, int h) {
	auto &d = ctx.dof;
	if (d.fbo && d.w == w && d.h == h) return;
	makeTex2D(d.color, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h, GL_LINEAR);
	makeTex2D(d.depth, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, w, h, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);
	auto status = makeFbo(d.fbo, d.color, d.depth);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		printf("miniature framebuffer incomplete: 0x%x\n", status);
		d.on = false;
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	d.w = w;
	d.h = h;
}

// end whatever section is running and start s (-1: just end it)
inline void gpuSection(render_ctx_t &ctx, int s) {
	auto &g = ctx.gpu;
	if (!g.ok || s == g.active_section) return;
	if (g.active) {
		glEndQuery(GL_TIME_ELAPSED_EXT);
		g.pending.push_back({ g.active, g.active_section, g.frame });
		g.active = 0;
	}
	g.active_section = s;
	if (s < 0) return;
	GLuint q;
	if (g.spare.empty()) glGenQueries(1, &q); else { q = g.spare.back(); g.spare.pop_back(); }
	glBeginQuery(GL_TIME_ELAPSED_EXT, q);
	g.active = q;
}

// once a frame, before its first section: collect what the gpu has
// finished, and fold a second's worth into the averages
inline void gpuFrameStart(render_ctx_t &ctx, double dt_ms) {
	auto &g = ctx.gpu;
	if (!g.ok) return;
	g.frame++;
	GLint disjoint = 0;
	glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
	if (disjoint) { // a clock change or the like: these are meaningless
		for (auto &p : g.pending) g.spare.push_back(p.q);
		g.pending.clear();
	}
	size_t done = 0;
	for (; done < g.pending.size(); done++) {
		auto &p = g.pending[done];
		GLuint avail = 0;
		glGetQueryObjectuiv(p.q, GL_QUERY_RESULT_AVAILABLE, &avail);
		if (!avail) break; // in order: nothing after this is ready either
		GLuint ns = 0;
		glGetQueryObjectuiv(p.q, GL_QUERY_RESULT, &ns);
		g.sum[p.section] += ns / 1e6;
		if (p.frame != g.last_done) { g.last_done = p.frame; g.frames_done++; }
		g.spare.push_back(p.q);
	}
	g.pending.erase(g.pending.begin(), g.pending.begin() + done);
	g.window_ms += dt_ms;
	if (g.window_ms >= 1000 && g.frames_done > 0) {
		for (int s = 0; s < gpu_sections; s++) { g.avg[s] = g.sum[s] / g.frames_done; g.sum[s] = 0; }
		g.frames_done = 0;
		g.window_ms = 0;
	}
}

// the occlusion runs in real time (outside glamour mode, which has its own
// way in, and not under the miniature, whose blur reads the scene directly)
inline bool renderAoLive(render_ctx_t &ctx) {
	auto &a = ctx.accum;
	return a.ao_on && a.ao_strength > 0 && !a.on && !ctx.dof.on;
}

inline void renderMsaaEnsure(render_ctx_t &ctx, int w, int h) {
	if (ctx.msaa_fbo && ctx.msaa_w == w && ctx.msaa_h == h) return;
	GLint max_samples = 0;
	glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
	auto n = ctx.msaa_samples < max_samples ? ctx.msaa_samples : max_samples;
	makeRenderbuffer(ctx.msaa_color, n, GL_RGBA8, w, h);
	makeRenderbuffer(ctx.msaa_depth, n, GL_DEPTH_COMPONENT24, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	auto status = makeFbo(ctx.msaa_fbo, ctx.msaa_color, ctx.msaa_depth, GL_RENDERBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		printf("msaa framebuffer incomplete (0x%x); offscreen passes without it\n", status);
		glDeleteFramebuffers(1, &ctx.msaa_fbo);
		ctx.msaa_fbo = 0;
		ctx.msaa_samples = 0;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	ctx.msaa_w = w;
	ctx.msaa_h = h;
}

inline void renderAoEnsure(render_ctx_t &ctx, int w, int h) {
	auto &a = ctx.accum;
	if (a.ao_fbo && a.ao_w == w && a.ao_h == h) return;
	auto make = [&](GLuint &fbo, GLuint &tex) {
		makeTex2D(tex, GL_RG16F, GL_RG, GL_HALF_FLOAT, w, h, GL_NEAREST);
		auto status = makeFbo(fbo, tex, 0);
		if (status != GL_FRAMEBUFFER_COMPLETE) printf("occlusion target incomplete: 0x%x\n", status);
	};
	make(a.ao_fbo, a.ao_tex);
	make(a.ao_fbo2, a.ao_tex2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	a.ao_w = w;
	a.ao_h = h;
}

// where each pass that draws into the scene starts
inline void renderBindScene(render_ctx_t &ctx) {
	glBindFramebuffer(GL_FRAMEBUFFER, ctx.scene_fbo);
	glViewport(0, 0, ctx.scene_w, ctx.scene_h);
}

inline void renderFrameBegin(render_ctx_t &ctx, int width, int height, int sky) {
	gpuSection(ctx, gpu_scene);
	// the scene goes to the offscreen target whenever a pass reads it back
	auto offscreen = ctx.dof.on || ctx.accum.on || renderAoLive(ctx);
	if (offscreen) renderDofEnsure(ctx, width, height);
	if (offscreen && ctx.msaa_samples > 1) renderMsaaEnsure(ctx, width, height);
	ctx.scene_fbo = !offscreen ? 0 : ctx.msaa_samples > 1 && ctx.msaa_fbo ? ctx.msaa_fbo : ctx.dof.fbo;
	ctx.scene_w = width;
	ctx.scene_h = height;
	renderBindScene(ctx);
	glClearColor((sky>>16 & 0xff) / 255.0f, (sky>>8 & 0xff) / 255.0f, (sky & 0xff) / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// the terrain pass: its program and state, and the program's per-frame
// uniforms — the camera and the aerial perspective, and the lod tint. the
// tube's, the body's and the shadow's follow, then the nodes'
inline void renderTerrainBegin(render_ctx_t &ctx, const frame_t &f) {
	renderBindScene(ctx);
	glUseProgram(ctx.program);
	glDisable(GL_POLYGON_OFFSET_FILL); // body mode's flat pass turns it on per node
	// draw both faces in tube mode: the camera legitimately sees walls from
	// either side (approaching the tube from outside, or geometry the roll
	// has folded over), and culling them reads as holes in the mesh
	if (f.tube) glDisable(GL_CULL_FACE); else glEnable(GL_CULL_FACE);
	Matrix3f cam;
	cam.col(0) = f.right.cast<float>(); cam.col(1) = f.cam_up.cast<float>(); cam.col(2) = f.direction.cast<float>();
	Vector3f eye = f.eye.cast<float>();
	glUniform4f(ctx.fog_loc, (float)f.visibility_m, f.haze_on ? 1.0f : 0.0f, 0.0f, 0.0f);
	glUniform3fv(ctx.haze_loc, 1, f.haze.data());
	glUniform3fv(ctx.tint_loc, 1, f.tint.data());
	glUniform3f(ctx.fog_planes_loc, (float)f.near, (float)f.far, f.ortho ? 1.0f : 0.0f);
	glUniformMatrix3fv(ctx.cam_loc, 1, GL_FALSE, cam.data());
	glUniform3fv(ctx.eye_loc, 1, eye.data());
	glUniform4f(ctx.cam_params_loc, (float)f.tan_half, f.aspect_ratio, (float)f.planet_radius, (float)f.scale_height);
	glUniform2f(ctx.viewport_loc, (float)f.width, (float)f.height);
	glUniform1i(ctx.debug_lod_loc, f.debug_lod);
	glUniform4f(ctx.clay_loc, ctx.clay_rgb[0], ctx.clay_rgb[1], ctx.clay_rgb[2], ctx.clay_on ? 1.0f : 0.0f);
	Vector3f sun = f.sun_dir.cast<float>();
	glUniform3fv(ctx.sun_dir_loc, 1, sun.data());
	glUniform1f(ctx.ortho_half_loc, (float)f.ortho_half_extent);
}

inline void renderSetTransform(render_ctx_t &ctx, const float *m16) {
	glUniformMatrix4fv(ctx.transform_loc, 1, GL_FALSE, m16);
}

// tube mode (see earth_tube.h): per-frame local-frame-to-clip matrix and warp
// parameters (effective roll radius, rect half length/width in meters).
// fade, in rect units along the axis: where the walls start darkening, to
// black at the rect's ends; 0 is no fade
inline void renderSetTube(render_ctx_t &ctx, bool on, const float *local_to_clip16,
		float r_eff, float half_len, float half_wid, float wid_offset, float fade = 0) {
	glUniform1i(ctx.tube_on_loc, on);
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
inline void renderSetTubePass(render_ctx_t &ctx, int pass) {
	glUniform1i(ctx.tube_pass_loc, pass);
	if (pass == 2) {
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(1.0f, 4.0f);
	} else {
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
}

// per-node mesh-to-local-frame matrix (tube mode only)
inline void renderSetTubeNode(render_ctx_t &ctx, const float *m16) {
	glUniformMatrix4fv(ctx.tube_mesh_to_local_loc, 1, GL_FALSE, m16);
}

// body mode (see earth_tube.h): the figure's geometry image, two w×h float
// textures of positions and unit normals in body units. unfiltered float
// textures are core webgl2; linear filtering of them is not, so the shader
// fetches texels and interpolates by hand. units 5 and 6, clear of the
// terrain texture on 0 and the dof pass's pair on 0/1
inline void renderBodyUpload(render_ctx_t &ctx, int w, int h, const float *pos, const float *nrm) {
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
inline void renderSetBody(render_ctx_t &ctx, bool on, float curl, const float *bands4) {
	glUniform1i(ctx.body_on_loc, on && ctx.body_tex[0]);
	glUniform1f(ctx.body_curl_loc, curl);
	glUniform4fv(ctx.body_bands_loc, 1, bands4);
}

// the accumulator's target, (re)made at the canvas size. a float texture
// holding the running average (not the sum: a half float summing 128 frames
// rounds every addition to a sixteenth once past 64, and the error, varying
// with the value, drew contour bands across the sky)
inline void renderAccumEnsure(render_ctx_t &ctx, int w, int h) {
	auto &a = ctx.accum;
	if (a.fbo && a.w == w && a.h == h) return;
	makeTex2D(a.tex, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, w, h, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);
	auto status = makeFbo(a.fbo, a.tex, 0);
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

inline GLuint makeShader(const char* vert_src, const char* frag_src);

inline void renderAccumInit(render_ctx_t &ctx) {
	auto &a = ctx.accum;
	a.present_program = makeShader(fullscreen_vs, present_fs);
	a.present_tex_loc = glGetUniformLocation(a.present_program, "tex");
	a.present_scale_loc = glGetUniformLocation(a.present_program, "scale");
	a.blit_program = makeShader(fullscreen_vs, blit_fs);
	a.blit_tex_loc = glGetUniformLocation(a.blit_program, "tex");
	a.ao_program = makeShader(fullscreen_vs, ao_fs);
	a.ao_depth_loc = glGetUniformLocation(a.ao_program, "depth_tex");
	a.ao_viewport_loc = glGetUniformLocation(a.ao_program, "viewport");
	a.ao_planes_loc = glGetUniformLocation(a.ao_program, "planes");
	a.ao_proj_loc = glGetUniformLocation(a.ao_program, "proj");
	a.ao_params_loc = glGetUniformLocation(a.ao_program, "params");
	a.ao_taps_loc = glGetUniformLocation(a.ao_program, "taps");
	a.ao_blur_program = makeShader(fullscreen_vs, ao_blur_fs);
	a.blur_src_loc = glGetUniformLocation(a.ao_blur_program, "src");
	a.ao_comp_program = makeShader(fullscreen_vs, ao_comp_fs);
	a.comp_color_loc = glGetUniformLocation(a.ao_comp_program, "color_tex");
	a.comp_ao_loc = glGetUniformLocation(a.ao_comp_program, "ao_tex");
	a.comp_depth_loc = glGetUniformLocation(a.ao_comp_program, "depth_tex");
	a.comp_planes_loc = glGetUniformLocation(a.ao_comp_program, "planes");
	a.comp_params_loc = glGetUniformLocation(a.ao_comp_program, "params");
}

// the occlusion's passes before the composite: computed, then (blur)
// smoothed. leaves an occlusion fbo bound
inline void aoCompute(render_ctx_t &ctx, const frame_t &f, bool blur) {
	auto &a = ctx.accum;
	auto &d = ctx.dof;
	gpuSection(ctx, gpu_occlusion);
	renderAoEnsure(ctx, (d.w + a.ao_scale - 1) / a.ao_scale, (d.h + a.ao_scale - 1) / a.ao_scale);
	glBindFramebuffer(GL_FRAMEBUFFER, a.ao_fbo);
	glViewport(0, 0, a.ao_w, a.ao_h);
	glUseProgram(a.ao_program);
	glBindVertexArray(d.vao);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, d.depth);
	glUniform1i(a.ao_depth_loc, 1);
	glUniform2f(a.ao_viewport_loc, (float)a.ao_w, (float)a.ao_h);
	glUniform3f(a.ao_planes_loc, (float)f.near, (float)f.far, f.ortho ? 1.0f : 0.0f);
	glUniform2f(a.ao_proj_loc, (float)(f.ortho ? f.ortho_half_extent : f.tan_half), f.aspect_ratio);
	// the haze's visibility (m), 0 with the haze off
	glUniform3f(a.ao_params_loc, f.haze_on && !f.tube ? (float)f.visibility_m : 0.0f, a.ao_rotation, 0.04f);
	glUniform1i(a.ao_taps_loc, a.ao_taps);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	a.ao_result = a.ao_tex;
	if (blur) {
		glBindFramebuffer(GL_FRAMEBUFFER, a.ao_fbo2);
		glUseProgram(a.ao_blur_program);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, a.ao_tex);
		glUniform1i(a.blur_src_loc, 2);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glActiveTexture(GL_TEXTURE0);
		a.ao_result = a.ao_tex2;
	}
}

// the second: the scene times the occlusion, into whatever is bound
inline void aoComposite(render_ctx_t &ctx, const frame_t &f) {
	auto &a = ctx.accum;
	auto &d = ctx.dof;
	glUseProgram(a.ao_comp_program);
	glBindVertexArray(d.vao);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, a.ao_result);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, d.depth);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, d.color);
	glUniform1i(a.comp_color_loc, 0);
	glUniform1i(a.comp_depth_loc, 1);
	glUniform1i(a.comp_ao_loc, 2);
	glUniform3f(a.comp_planes_loc, (float)f.near, (float)f.far, f.ortho ? 1.0f : 0.0f);
	glUniform3f(a.comp_params_loc, a.ao_debug ? -a.ao_strength : a.ao_strength, 0.0f, 0.0f);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	gpuSection(ctx, gpu_post);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
}

inline void dofDraw(render_ctx_t &ctx, const frame_t &f);

// the scene is drawn; put it on the screen. plain frames go straight there.
// with the miniature on, the scene was drawn offscreen and the blur pass
// writes it out. in glamour mode the pass (or a copy) is summed into the
// accumulator instead, and the average of the samples so far is presented
inline void renderFrameEnd(render_ctx_t &ctx, const frame_t &f) {
	auto &d = ctx.dof;
	auto &a = ctx.accum;
	auto ao_live = renderAoLive(ctx);
	if (!d.on && !a.on && !ao_live) { gpuSection(ctx, -1); return; }
	gpuSection(ctx, gpu_post);
	glDisable(GL_DEPTH_TEST);
	// the multisampled scene, resolved into the textures the passes read
	if (ctx.scene_fbo == ctx.msaa_fbo && ctx.msaa_fbo) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx.msaa_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d.fbo);
		glBlitFramebuffer(0, 0, d.w, d.h, 0, 0, d.w, d.h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
		// the samples are spent: a tile gpu needn't keep them in memory
		GLenum spent[2] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT };
		glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 2, spent);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	}
	if (ao_live) {
		aoCompute(ctx, f, true);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, d.w, d.h);
		aoComposite(ctx, f);
	} else if (a.on) {
		renderAccumEnsure(ctx, d.w, d.h);
		if (a.on) { // (ensure can turn it off)
			glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
			glViewport(0, 0, a.w, a.h);
			if (!a.hold || a.samples == 0) {
				auto ao = !d.on && a.ao_on && a.ao_strength > 0;
				if (ao) {
					// no blur: the average over samples, each spun differently,
					// smooths it, and blurring each would only soften it
					aoCompute(ctx, f, false);
					glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
					glViewport(0, 0, a.w, a.h);
				}
				// the n-th sample goes in with weight 1/n: the first replaces
				// whatever was there, and the texture is always the average
				glEnable(GL_BLEND);
				glBlendColor(0, 0, 0, 1.0f / (float)(a.samples + 1));
				glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
				if (d.on) dofDraw(ctx, f);
				else if (ao) aoComposite(ctx, f);
				else {
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
	if (!a.on && !ao_live) {
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, d.w, d.h);
		dofDraw(ctx, f);
	}
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0); // the terrain textures live on unit 0
	glEnable(GL_DEPTH_TEST);
	gpuSection(ctx, -1);
}

// the miniature's blur pass over the offscreen scene, into whatever
// framebuffer is bound
inline void dofDraw(render_ctx_t &ctx, const frame_t &f) {
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
	glUniform3f(d.planes_loc, (float)f.near, (float)f.far, f.ortho ? 1.0f : 0.0f);
	glUniform3f(d.params_loc, f.focus, d.strength * d.h, f.focus_range);
	glDrawArrays(GL_TRIANGLES, 0, 3);
}


inline void checkCompileShaderError(GLuint shader) {
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

// both stages compiled can still fail to link (a varying or a shared
// uniform the two disagree on); the program is useless then too
inline void checkLinkProgramError(GLuint program) {
	GLint is_linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &is_linked);
	if(is_linked == GL_TRUE) return;
	GLint max_len = 0;
	glGetProgramiv(program, GL_INFO_LOG_LENGTH, &max_len);
	std::vector<GLchar> error_log(max_len + 1);
	glGetProgramInfoLog(program, max_len, &max_len, &error_log[0]);
	std::cout << "link failed: " << &error_log[0] << std::endl;
	glDeleteProgram(program);
	abort();
}

inline GLuint makeShader(const char* vert_src, const char* frag_src) {
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
	checkLinkProgramError(program);
	glDetachShader(program, vert_shader);
	glDetachShader(program, frag_shader);
	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);
	return program;
}

inline void renderDofInit(render_ctx_t &ctx) {
	auto &d = ctx.dof;
	d.program = makeShader(fullscreen_vs, dof_fs);
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
inline void renderPathInit(render_ctx_t &ctx) {
	auto &p = ctx.path;
	p.program = makeShader(path_vs, path_fs);
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
inline void renderPathUpload(render_ctx_t &ctx, const float *data, int vert_count) {
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
inline void renderPathCursorDraw(render_ctx_t &ctx, const float *transform16, const float *rgba,
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
	renderBindScene(ctx);
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
}

// draw the uploaded ribbon. depth *test* on so terrain hides the parts of the
// track that are over the hill, but depth *writes* off: the ribbon overlaps
// itself at every join, and a translucent line that occludes itself comes out
// blotchy. culling is off because a quad's winding follows which way the
// segment happens to run on screen
inline void renderPathDraw(render_ctx_t &ctx, const float *transform16, const float *rgba,
		float width_px, float bias, int w, int h) {
	auto &p = ctx.path;
	if (!p.vert_count) return;
	renderBindScene(ctx);
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
}

// (re)allocate the shadow map at size x size. the placeholder is 1x1
inline void renderShadowEnsure(render_ctx_t &ctx, int size) {
	auto &s = ctx.shadow;
	if (s.tex && s.size == size) return;
	glActiveTexture(GL_TEXTURE0 + shadow_unit);
	makeTex2D(s.tex, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, size, size, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
	glActiveTexture(GL_TEXTURE0);
	auto status = makeFbo(s.fbo, 0, s.tex);
	if (status != GL_FRAMEBUFFER_COMPLETE) printf("shadow framebuffer incomplete: 0x%x\n", status);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	s.size = size;
}

inline void renderShadowInit(render_ctx_t &ctx) {
	auto &s = ctx.shadow;
	s.program = makeShader(shadow_vs, shadow_fs);
	s.mv_loc = glGetUniformLocation(s.program, "shadow_mv");
	s.box_loc = glGetUniformLocation(s.program, "shadow_box");
	s.mask_loc = glGetUniformLocation(s.program, "octant_mask");
	renderShadowEnsure(ctx, 1);
}

// the shadow pass: into the map, depth only, both faces (terrain is a
// sheet, not a solid), with a slope-scaled depth offset against acne
inline void renderShadowBegin(render_ctx_t &ctx, int size, const float *box4) {
	auto &s = ctx.shadow;
	gpuSection(ctx, gpu_shadow);
	renderShadowEnsure(ctx, size);
	glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
	glViewport(0, 0, s.size, s.size);
	glClear(GL_DEPTH_BUFFER_BIT);
	glUseProgram(s.program);
	glUniform4fv(s.box_loc, 1, box4);
	glDisable(GL_CULL_FACE);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(2.0f, 2.0f);
}

inline void renderShadowNode(render_ctx_t &ctx, const float *mv16, uint8_t octant_mask) {
	glUniformMatrix4fv(ctx.shadow.mv_loc, 1, GL_FALSE, mv16);
	glUniform1i(ctx.shadow.mask_loc, octant_mask);
}

inline void renderShadowMesh(const rocktree_t::node_t::mesh_t &mesh) {
	glBindVertexArray(mesh.vao);
	glDrawElements(GL_TRIANGLE_STRIP, mesh.indices.size(), GL_UNSIGNED_SHORT, NULL);
}

inline void renderShadowEnd(render_ctx_t &ctx) {
	glBindVertexArray(0);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(0.0f, 0.0f);
	gpuSection(ctx, gpu_scene);
}

// the terrain program's shadow uniforms: per frame, then per node
inline void renderSetShadow(render_ctx_t &ctx, bool on, const float *box4, float strength, float texel_uv, float bias,
		const float *up3, const float *rot9, const float *eye3) {
	glUniform4f(ctx.shadow_params_loc, on ? (float)ctx.shadow.debug : 0.0f, strength, texel_uv, bias);
	if (box4) glUniform4fv(ctx.shadow_box_loc, 1, box4);
	if (up3) glUniform3fv(ctx.shadow_up_loc, 1, up3);
	if (rot9) glUniformMatrix3fv(ctx.sun_rot_loc, 1, GL_FALSE, rot9);
	if (eye3) glUniform3fv(ctx.eye_sun_loc, 1, eye3);
}

inline void renderSetShadowNode(render_ctx_t &ctx, const float *mv16) {
	glUniformMatrix4fv(ctx.shadow_mv_loc, 1, GL_FALSE, mv16);
}

inline void renderSkyInit(render_ctx_t &ctx) {
	auto &s = ctx.sky;
	s.program = makeShader(sky_vs, sky_fs);
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
// the far plane
inline void renderSkyDraw(render_ctx_t &ctx, const frame_t &f) {
	auto &s = ctx.sky;
	Vector3f r = f.right.cast<float>(), u = f.cam_up.cast<float>(), fwd = f.direction.cast<float>();
	Vector3f lu = f.local_up.cast<float>(), sd = f.sun_dir.cast<float>();
	renderBindScene(ctx);
	glUseProgram(s.program);
	glUniform3fv(s.right_loc, 1, r.data());
	glUniform3fv(s.up_loc, 1, u.data());
	glUniform3fv(s.fwd_loc, 1, fwd.data());
	glUniform3fv(s.local_up_loc, 1, lu.data());
	glUniform3fv(s.sun_loc, 1, sd.data());
	glUniform4f(s.params_loc, (float)f.tan_half, f.aspect_ratio, (float)f.horizon_dip, (float)f.sun_el);
	glUniform3fv(s.zenith_loc, 1, f.zenith.data());
	glUniform3fv(s.haze_loc, 1, f.haze.data());
	glUniform2f(s.alt_loc, (float)f.sky_amount, (float)f.limb);
	glDepthFunc(GL_LEQUAL); // equal to the cleared depth, where nothing was drawn
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(ctx.dof.vao); // the empty one
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
}

// backend init
inline void renderInit(render_ctx_t &ctx) {
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
			glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &ctx.max_aniso);
			break;
		}
	}
	printf("max anisotropy: %.0f\n", ctx.max_aniso);
	if (!texture_s3tc_supported)
		printf("no s3tc on this gpu; using jpg textures\n");
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	// crack-fill boundary lines (see bindAndDrawMesh) can only land on
	// background pixels, so width just sets how wide a slit they can plug
	// (wider also costs more fill rate). webgl typically clamps this to 1
	glLineWidth(2.0f);
	ctx.program = makeShader(terrain_vs, terrain_fs);
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
	ctx.clay_loc = glGetUniformLocation(ctx.program, "clay");
	ctx.sun_dir_loc = glGetUniformLocation(ctx.program, "sun_dir");
	ctx.ortho_half_loc = glGetUniformLocation(ctx.program, "ortho_half");
	ctx.viewport_loc = glGetUniformLocation(ctx.program, "viewport");
	ctx.shadow_mv_loc = glGetUniformLocation(ctx.program, "shadow_mv");
	ctx.shadow_box_loc = glGetUniformLocation(ctx.program, "shadow_box");
	ctx.shadow_params_loc = glGetUniformLocation(ctx.program, "shadow_params");
	ctx.shadow_map_loc = glGetUniformLocation(ctx.program, "shadow_map");
	ctx.shadow_up_loc = glGetUniformLocation(ctx.program, "shadow_up");
	ctx.sun_rot_loc = glGetUniformLocation(ctx.program, "sun_rot");
	ctx.eye_sun_loc = glGetUniformLocation(ctx.program, "eye_sun");
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
