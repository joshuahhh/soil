// the renderer's glsl, compiled in rocktree_gl.h. raw strings, so each
// shader keeps its lines and a compile error's line number means something;
// a piece several of them share is a macro, spliced in by literal
// concatenation (its lines count toward the line numbers like any others)

// main() is the shader's own, and says at what depth the triangle goes
#define GLSL_FULLSCREEN R"glsl(#version 300 es
// one triangle covering the screen, no buffers
out vec2 v_uv;
vec2 fullscreen() {
	vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
	v_uv = p * 0.5 + 0.5;
	return p;
}
)glsl"

// the shader declares planes (near, far, ortho flag) before splicing it in
#define GLSL_LIN R"glsl(
// depth buffer value -> distance along the view axis, meters
float lin(float d) {
	if (planes.z > 0.5) return planes.x + d * (planes.y - planes.x);
	float z = d * 2.0 - 1.0;
	return 2.0 * planes.x * planes.y / (planes.y + planes.x - z * (planes.y - planes.x));
}
)glsl"

static const char *fullscreen_vs = GLSL_FULLSCREEN R"glsl(
void main() { gl_Position = vec4(fullscreen(), 0.0, 1.0); }
)glsl";

static const char *sky_vs = GLSL_FULLSCREEN R"glsl(
void main() {
	// at the far plane exactly: it shows wherever nothing was drawn
	gl_Position = vec4(fullscreen(), 1.0, 1.0);
}
)glsl";

// glamour mode (see accum_gl_t): the accumulator onto the canvas, a plain
// copy of the scene into it, and the ambient occlusion's three passes
static const char *present_fs = R"glsl(#version 300 es
precision highp float;
uniform sampler2D tex;
uniform float scale; // 1 / samples
in vec2 v_uv;
out vec4 frag_color;
void main() { frag_color = vec4(texture(tex, v_uv).rgb * scale, 1.0); }
)glsl";

static const char *blit_fs = R"glsl(#version 300 es
precision highp float;
uniform sampler2D tex;
in vec2 v_uv;
out vec4 frag_color;
void main() { frag_color = vec4(texture(tex, v_uv).rgb, 1.0); }
)glsl";

// ambient occlusion from the depth buffer: each pixel's view-space
// position and face normal, and a spiral of taps around it out to a
// radius in metres (a fraction of the distance, held between half a
// metre and 150); a tap occludes by how far above the surface's plane
// it sits, and not at all past the radius, so a building's silhouette
// doesn't darken the distant ground behind it. the cap is high because
// street canyons are: a 30 m one left every wall of a manhattan street
// outside the radius. the spiral turns per pixel (interleaved gradient
// noise) and per glamour sample, so its pattern breaks up into noise
// the blur, or the average, removes.
//
// and it fades out twice with distance: where the haze has taken over
// (it is drawn after the haze, and darkening what the haze has washed
// out drew dark lace over distant mountains), and where the depth
// buffer can no longer resolve the radius — 24 bits over a near plane
// of a metre resolve hundreds of metres at 100 km, and occlusion
// measured from that is quantization, not geometry
static const char *ao_fs = R"glsl(#version 300 es
precision highp float;
uniform sampler2D depth_tex;
uniform vec2 viewport;
uniform vec3 planes; // near, far, ortho flag
uniform vec2 proj; // tan(fov/2) (ortho: half height, metres), aspect
uniform vec3 params; // visibility (m, 0 = no haze), rotation, radius as a fraction of distance
uniform int taps;
in vec2 v_uv;
out vec4 frag_color;
)glsl" GLSL_LIN R"glsl(
vec3 pos(vec2 uv) {
	float z = lin(texture(depth_tex, uv).r);
	vec2 n = uv * 2.0 - 1.0;
	if (planes.z > 0.5) return vec3(n.x * proj.x * proj.y, n.y * proj.x, -z);
	return vec3(n.x * proj.x * proj.y * z, n.y * proj.x * z, -z);
}
void main() {
	if (texture(depth_tex, v_uv).r >= 1.0) { frag_color = vec4(0.0, 60000.0, 0.0, 1.0); return; }
	vec3 p = pos(v_uv);
	vec3 n = normalize(cross(dFdx(p), dFdy(p)));
	if (dot(n, p) > 0.0) n = -n; // toward the camera
	float z = -p.z;
	float R = clamp(params.z * z, 0.5, 150.0);
	float dz = planes.z > 0.5 ? (planes.y - planes.x) / 16777216.0 : z * z / (planes.x * 16777216.0);
	float fade = 1.0 - smoothstep(0.03 * R, 0.15 * R, dz);
	if (params.x > 0.0) fade *= exp(-z / params.x);
	if (fade < 0.01) { frag_color = vec4(0.0, z * 0.001, 0.0, 1.0); return; }
	float rpx = planes.z > 0.5 ? R / (2.0 * proj.x) * viewport.y : R / (z * proj.x) * 0.5 * viewport.y;
	rpx = min(rpx, 0.08 * viewport.y);
	float spin = params.y + 6.2832 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	float occ = 0.0;
	int N = taps;
	for (int i = 0; i < N; i++) {
		float r = sqrt((float(i) + 0.5) / float(N)) * rpx;
		float a = float(i) * 2.39996 + spin;
		vec2 uv = v_uv + vec2(cos(a), sin(a)) * r / viewport;
		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) continue;
		vec3 v = pos(uv) - p;
		float l = length(v);
		if (l < 1e-4) continue;
		// only what rises more than ~15 degrees above the surface's plane:
		// a street canyon's walls are far steeper, the photogrammetry's
		// bumpy water mostly isn't, and at 6 degrees it smudged every bay
		occ += max(0.0, dot(v / l, n) - 0.25) * 1.33 * (1.0 - smoothstep(0.7 * R, R, l));
	}
	frag_color = vec4(occ / float(N) * fade, z * 0.001, 0.0, 1.0);
}
)glsl";

// the blur: a 5x5 block of the occlusion texture's own texels — at its
// resolution, which is where the per-pixel spin's noise lives — those
// whose depth is near this one's, so it doesn't bleed across a
// silhouette. (at half size a 3x3 kernel applied at full size left the
// noise in as a stipple)
static const char *ao_blur_fs = R"glsl(#version 300 es
precision highp float;
uniform sampler2D src;
out vec4 frag_color;
void main() {
	ivec2 p = ivec2(gl_FragCoord.xy);
	ivec2 hi = textureSize(src, 0) - 1;
	vec2 c = texelFetch(src, p, 0).rg;
	float s = 0.0, w = 0.0;
	for (int i = -2; i <= 2; i++)
		for (int j = -2; j <= 2; j++) {
			vec2 t = texelFetch(src, clamp(p + ivec2(i, j), ivec2(0), hi), 0).rg;
			float k = abs(t.g - c.g) < 0.05 * c.g ? 1.0 : 0.0;
			s += t.r * k;
			w += k;
		}
	frag_color = vec4(w > 0.0 ? s / w : c.r, c.g, 0.0, 1.0);
}
)glsl";

// the occlusion onto the scene, upscaled: the 2x2 texels around this
// pixel, bilinear weights times a depth test — each texel carries the
// depth it was computed at — so a texel straddling a roofline gives
// the roof its occlusion and the ground behind it nothing (a plain
// bilinear upscale draws a dark fringe along every edge). none
// matching, it takes the texel nearest in depth: falling back to no
// occlusion left white gaps along edges
static const char *ao_comp_fs = R"glsl(#version 300 es
precision highp float;
uniform sampler2D color_tex;
uniform sampler2D ao_tex;
uniform sampler2D depth_tex;
uniform vec3 planes;
uniform vec3 params; // strength (negative: the occlusion alone), -, -
in vec2 v_uv;
out vec4 frag_color;
)glsl" GLSL_LIN R"glsl(
void main() {
	vec3 c = texture(color_tex, v_uv).rgb;
	float d = texture(depth_tex, v_uv).r;
	float occ = 0.0;
	if (d < 1.0) {
		float z0 = lin(d) * 0.001;
		vec2 asz = vec2(textureSize(ao_tex, 0));
		vec2 hp = v_uv * asz - 0.5;
		ivec2 b = ivec2(floor(hp));
		vec2 f = hp - floor(hp);
		ivec2 hi = ivec2(asz) - 1;
		float s = 0.0, w = 0.0, best = 1e30, bestv = 0.0;
		for (int i = 0; i <= 1; i++)
			for (int j = 0; j <= 1; j++) {
				vec2 t = texelFetch(ao_tex, clamp(b + ivec2(i, j), ivec2(0), hi), 0).rg;
				float bw = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
				float dz = abs(t.g - z0);
				float k = bw * (dz < 0.05 * z0 ? 1.0 : 0.0);
				s += t.r * k;
				w += k;
				if (dz < best) { best = dz; bestv = t.r; }
			}
		occ = w > 1e-4 ? s / w : bestv;
	}
	float ao = clamp(1.0 - abs(params.x) * 1.6 * occ, 0.0, 1.0);
	frag_color = vec4(params.x < 0.0 ? vec3(ao) : c * ao, 1.0);
}
)glsl";

// the miniature's blur (see dof_gl_t)
static const char *dof_fs = R"glsl(#version 300 es
precision highp float;
uniform sampler2D color_tex;
uniform sampler2D depth_tex;
uniform vec2 viewport;
uniform vec3 planes; // near, far, ortho flag
uniform vec3 params; // focus (m), max radius (px), sharp band (fraction of focus)
in vec2 v_uv;
out vec4 frag_color;
)glsl" GLSL_LIN R"glsl(
// circle of confusion in pixels: zero inside the sharp band around
// the focus distance, ramping to the max over another band's width.
// relative to the focus, so the look is the same at any altitude
float coc(vec2 uv) {
	float t = abs(lin(texture(depth_tex, uv).r) - params.x) / (params.x * params.z) - 1.0;
	return clamp(t, 0.0, 1.0) * params.y;
}
void main() {
	float c0 = coc(v_uv);
	vec3 sum = texture(color_tex, v_uv).rgb;
	float wsum = 1.0;
	if (c0 > 0.5) {
		// a golden-angle spiral disc of taps out to the pixel's own radius.
		// a tap only counts if its own blur disc reaches back here, which
		// keeps a sharp foreground from smearing into a soft background
		const int N = 32;
		for (int i = 0; i < N; i++) {
			float r = sqrt((float(i) + 0.5) / float(N)) * c0;
			float a = float(i) * 2.39996;
			vec2 uv = v_uv + vec2(cos(a), sin(a)) * r / viewport;
			float w = clamp(coc(uv) - r + 1.0, 0.0, 1.0);
			sum += texture(color_tex, uv).rgb * w;
			wsum += w;
		}
	}
	vec3 c = sum / wsum;
	// toy paint: a little more saturation and contrast
	float l = dot(c, vec3(0.299, 0.587, 0.114));
	c = mix(vec3(l), c, 1.35);
	c = (c - 0.5) * 1.12 + 0.5;
	frag_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)glsl";

// the path overlay (see path_gl_t and renderPathInit)
static const char *path_vs = R"glsl(#version 300 es
uniform mat4 transform;   // path-local -> clip
uniform vec2 viewport;    // drawable size in pixels
uniform float width;      // line width in pixels
// depth bias in ndc units, toward the camera: a track draped onto the
// terrain is coplanar with it, and a constant ndc nudge is roughly
// the shape of the depth buffer's own precision (fine near, coarse far)
uniform float bias;
in vec3 pos_a;
in vec3 pos_b;
in float end;
in float side;
// which corner of the quad this is, for the round cursor; zero along
// a ribbon, where there is nothing to round off
out vec2 v_corner;
void main() {
	vec4 a = transform * vec4(pos_a, 1.0);
	vec4 b = transform * vec4(pos_b, 1.0);
	// near-plane clip in clip space: projecting a vertex with w <= 0 flings
	// it to the wrong side of the screen and the segment draws as a streak
	// across the view. shorten the segment to the plane instead
	const float wmin = 1e-4;
	if (a.w < wmin && b.w < wmin) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }
	if (a.w < wmin) a = mix(a, b, (wmin - a.w) / (b.w - a.w));
	if (b.w < wmin) b = mix(b, a, (wmin - b.w) / (a.w - b.w));
	vec2 half_vp = 0.5 * viewport;
	vec2 sa = a.xy / a.w * half_vp;
	vec2 sb = b.xy / b.w * half_vp;
	vec2 d = sb - sa;
	vec4 p = end < 0.5 ? a : b;
	vec2 off;
	// a degenerate segment is a join patch: an axis-aligned square, using
	// the two corner bits as its two signs
	if (dot(d, d) < 1e-8) { v_corner = vec2(end * 2.0 - 1.0, side); off = v_corner * (0.5 * width); }
	else { v_corner = vec2(0.0); off = normalize(vec2(-d.y, d.x)) * (side * 0.5 * width); }
	p.xy += off / half_vp * p.w;
	p.z -= bias * p.w;
	gl_Position = p;
}
)glsl";

static const char *path_fs = R"glsl(#version 300 es
precision highp float;
uniform vec4 color;
// the cursor quad is drawn as a disc: a white core inside a ring of
// the path's color, the same marker the map draws. msaa can't help
// with either edge -- a hole punched by the fragment shader isn't a
// geometry edge -- so the rim is faded over the last fifth of the
// radius and the core's edge over a sliver of it
uniform bool round_dot;
in vec2 v_corner;
out vec4 frag_color;
void main() {
	float a = color.a;
	vec3 rgb = color.rgb;
	if (round_dot) {
		float d = length(v_corner);
		a *= 1.0 - smoothstep(0.8, 1.0, d);
		rgb = mix(vec3(1.0), rgb, smoothstep(0.46, 0.56, d));
	}
	if (a <= 0.0) discard;
	frag_color = vec4(rgb, a);
}
)glsl";

// the sun's depth map (see shadow_gl_t)
static const char *shadow_vs = R"glsl(#version 300 es
uniform mat4 shadow_mv;
uniform vec4 shadow_box; // half x, half y, near, far (metres along the sun)
uniform int octant_mask;
layout(location = 0) in vec3 position;
layout(location = 1) in float octant;
out float v_mask;
void main() {
	vec3 v = (shadow_mv * vec4(position, 1.0)).xyz;
	// the octants a finer drawn tile covers cast through that tile,
	// not through this coarser copy of the same ground -- and unlike the
	// colour pass, which pushes a triangle straddling the boundary back
	// in depth, here any part-masked one goes: a coarse triangle sitting
	// above the fine surface would shade it
	v_mask = ((octant_mask >> int(octant)) & 1) != 0 ? 0.0 : 1.0;
	gl_Position = vec4(v.xy / shadow_box.xy,
		(-v.z - shadow_box.z) / (shadow_box.w - shadow_box.z) * 2.0 - 1.0, 1.0);
}
)glsl";

static const char *shadow_fs = R"glsl(#version 300 es
precision highp float;
in float v_mask;
void main() { if (v_mask < 0.999) discard; }
)glsl";

// the sky, drawn at the far plane by sky_vs (see sky_gl_t)
static const char *sky_fs = R"glsl(#version 300 es
precision highp float;
uniform vec3 cam_right, cam_up, cam_fwd; // the camera's frame, world
uniform vec3 local_up; // straight up at the eye
uniform vec3 sun; // toward the sun, unit
uniform vec4 params; // tan(fov/2), aspect, horizon dip below level (rad), sun elevation (rad)
uniform vec3 zenith, haze;
// how much sky there is above the eye (1 on the ground, 0 in
// space) and the angular thickness of the limb seen from there
uniform vec2 alt_params;
in vec2 v_uv;
out vec4 frag_color;
void main() {
	vec2 n = v_uv * 2.0 - 1.0;
	vec3 dir = normalize(cam_fwd + cam_right * (n.x * params.x * params.y) + cam_up * (n.y * params.x));
	float el = asin(clamp(dot(dir, local_up), -1.0, 1.0));
	// height above the horizon line, which sits below level by the dip
	// (more the higher you are)
	float h = el + params.z;
	vec3 c;
	float d = max(dot(dir, sun), 0.0);
	float th = acos(clamp(dot(dir, sun), -1.0, 1.0)); // angle from the sun's centre
	// the sun reddens toward the horizon, through more air
	vec3 sunc = mix(vec3(1.0, 0.55, 0.25), vec3(1.0, 0.96, 0.88), smoothstep(0.0, 0.3, params.w));
	if (h >= 0.0) {
		// haze at the horizon, the zenith colour high up; the hazy band is
		// thick, as it is, and the gradient slow. the whole of it thins
		// away with altitude (space is black), leaving the limb: the
		// atmosphere seen edge-on just above the horizon line
		c = mix(haze, zenith, pow(clamp(h / 1.3, 0.0, 1.0), 0.5)) * alt_params.x;
		c += haze * (1.0 - alt_params.x) * exp(-h / alt_params.y);
		// the glow: a tight core a fraction of a degree wide, an aureole a
		// few degrees, and a wide forward scatter that brightens the sky
		// around a low sun. then the disc itself, the real 0.53 degrees,
		// its edge smoothed over a pixel (a hard per-pixel test aliases,
		// and msaa doesn't reach inside a shader). the core used to be
		// pow(d, 1400), still at 40% two degrees out: a fuzzy moon ten
		// times the sun's size
		c += sunc * (0.35 * exp(-th / 0.006) + (0.12 * exp(-th / 0.07) + 0.05 * pow(d, 3.0)) * alt_params.x);
		float r = 0.00466, w = max(fwidth(th), 1e-5);
		c = mix(c, sunc * 1.4, 1.0 - smoothstep(r - w, r + w, th));
	} else {
		// below the horizon line nothing is drawn only past the far plane:
		// the planet's limb from high up. darken toward it
		c = mix(haze, haze * 0.3, clamp(-h * 2.5, 0.0, 1.0)) * alt_params.x;
	}
	frag_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)glsl";

// the terrain (see renderInit and bindAndDrawMesh)
static const char *terrain_vs = R"glsl(#version 300 es
uniform mat4 transform;
// tube mode: vertices go mesh -> rect-local frame (x along the tube
// axis, y across, z up), roll across-offset y into an angle around a
// horizontal axis at height R, then local -> clip. R is the effective
// roll radius (the true tube radius over the curl amount, so curl->0
// flattens back out). v_rect is the position in rect units for the
// fragment shader to clip the slab's edges
uniform bool tube_on;
uniform mat4 tube_mesh_to_local;
uniform mat4 tube_local_to_clip;
uniform vec4 tube_params; // (r_eff, half_len, half_wid, wid_offset)
// mediump in both stages: a uniform shared by the two must agree on
// precision, and int defaults differ (highp here, mediump there)
uniform mediump int tube_pass; // 1 warped, 2 flat with the rect cut out
// body mode: the rect goes onto a figure instead of a cylinder. its
// skin is a geometry image -- positions and unit normals over
// (u around, v feet to head), body units with height 1, x across
// the figure, y up, z out of its front -- sampled bilinearly by hand
// (u wraps, v clamps; float textures don't filter in webgl2) and
// scaled so the rect's length is the figure's height. the terrain
// height rides out along the normal, and body_curl eases the sheet
// from flat to wrapped. the cpu twin is earth_core's bodyWarp
uniform bool body_on;
uniform float body_curl;
uniform vec4 body_bands; // (legs below v, arm rows v lo, v hi, arms' share of u)
uniform vec2 body_size;
// highp: a vertex-shader sampler defaults to lowp, and the
// precision qualifier applies to what the fetch returns
uniform highp sampler2D body_pos;
uniform highp sampler2D body_nrm;
uniform vec2 uv_offset;
uniform vec2 uv_scale;
// octant_mask: one bit per octant. stale_mask: three bits per
// octant, the fetch phase of the finer tile wanted there (0 none;
// see earth_core's draw loop for the legend)
uniform int octant_mask;
uniform int stale_mask;
// near, far, ortho flag: the fragment's distance from the eye for
// the aerial perspective comes out of the clip position
uniform vec3 fog_planes;
out float v_dist;
// the sun's view of this vertex: metres, in a frame centred on the
// shadow map's box with +z toward the sun (see renderShadowBegin)
uniform mat4 shadow_mv;
uniform mat3 sun_rot; // the globe frame's axes in the sun's frame
out vec3 v_sunv;
out vec3 v_nsun;
// fixed locations: the shadow pass's program reads the same vaos
layout(location = 0) in vec3 position;
layout(location = 1) in float octant;
layout(location = 2) in vec2 texcoords;
layout(location = 3) in vec3 normal;
out vec2 v_texcoords;
flat out float v_stale;
out float v_mask;
out vec2 v_rect;
out float v_loop;
out vec3 v_local;
// the image is several loops (legs, arms, torso -- see body_bands):
// the loop a texel belongs to, as its (u, v) box and an id. samples
// are clamped to the box, so the interpolation never crosses from
// one part to another, and a terrain triangle whose vertices land
// in different loops is dropped by the fragment shader: it would
// stretch across the gap between them
vec4 bodyLoop(vec2 uv, out float id) {
	if (uv.y < body_bands.x) {
		if (uv.x < 0.5) { id = 1.0; return vec4(0.0, 0.0, 0.5, body_bands.x); }
		id = 2.0; return vec4(0.5, 0.0, 1.0, body_bands.x);
	}
	if (uv.y >= body_bands.y && uv.y < body_bands.z) {
		float a = body_bands.w;
		if (uv.x < a) { id = 4.0; return vec4(0.0, body_bands.y, a, body_bands.z); }
		if (uv.x < 1.0 - a) { id = 3.0; return vec4(a, body_bands.y, 1.0 - a, body_bands.z); }
		id = 5.0; return vec4(1.0 - a, body_bands.y, 1.0, body_bands.z);
	}
	if (uv.y < body_bands.y) { id = 0.0; return vec4(0.0, body_bands.x, 1.0, body_bands.y); }
	id = 6.0; return vec4(0.0, max(body_bands.x, body_bands.z), 1.0, 1.0);
}
vec3 bodyFetch(sampler2D t, vec2 uv, vec4 box) {
	vec2 f = uv * body_size - 0.5;
	vec2 fl = floor(f);
	vec2 fr = f - fl;
	ivec2 lo = ivec2(box.xy * body_size + 0.5);
	ivec2 hi = ivec2(box.zw * body_size + 0.5) - 1;
	int x0 = clamp(int(fl.x), lo.x, hi.x);
	int x1 = clamp(int(fl.x) + 1, lo.x, hi.x);
	int y0 = clamp(int(fl.y), lo.y, hi.y);
	int y1 = clamp(int(fl.y) + 1, lo.y, hi.y);
	vec3 a = mix(texelFetch(t, ivec2(x0, y0), 0).xyz, texelFetch(t, ivec2(x1, y0), 0).xyz, fr.x);
	vec3 b = mix(texelFetch(t, ivec2(x0, y1), 0).xyz, texelFetch(t, ivec2(x1, y1), 0).xyz, fr.x);
	return mix(a, b, fr.y);
}
void main() {
	// masking: a triangle is dropped only when ALL its vertices are in
	// masked octants (v_mask interpolates to 0 -> fragment discard).
	// triangles straddling an octant boundary used to be collapsed
	// entirely, retreating the surface a triangle-row from the boundary
	// and opening hairline cracks the finer tile never covers; instead
	// they are drawn, with masked vertices pushed slightly away in depth
	// so the finer tile wins wherever they overlap it
	float mask = ((octant_mask >> int(octant)) & 1) != 0 ? 0.0 : 1.0;
	v_mask = mask;
	v_stale = float((stale_mask >> (3 * int(octant))) & 7);
	v_texcoords = (texcoords + uv_offset) * uv_scale;
	v_loop = 0.0;
	v_local = vec3(0.0);
	vec4 p;
	if (tube_on) {
		vec3 l = (tube_mesh_to_local * vec4(position, 1.0)).xyz;
		v_rect = vec2(l.x / tube_params.y, (l.y - tube_params.w) / tube_params.z);
		v_local = l;
		if (tube_pass == 2) {
			p = transform * vec4(position, 1.0);
		} else if (body_on) {
			vec2 uv = vec2(v_rect.y, v_rect.x) * 0.5 + 0.5;
			float S = 2.0 * tube_params.y;
			vec4 box = bodyLoop(uv, v_loop);
			// ids a thousand apart: the fragment test is an absolute tolerance,
			// and the sliver of a spanning triangle that passes it next to a
			// vertex shrinks with the gap between ids -- at 1 it was a visible
			// string of dots along the band edges
			v_loop *= 1000.0;
			vec3 b = bodyFetch(body_pos, uv, box);
			vec3 n = normalize(bodyFetch(body_nrm, uv, box));
			vec3 w = vec3(S * b.x, S * b.z, S * b.y) + vec3(n.x, n.z, n.y) * l.z;
			p = tube_local_to_clip * vec4(mix(l, w, body_curl), 1.0);
		} else {
		float R = tube_params.x;
		float th = l.y / R;
		// radial distance from the tube axis; the floor keeps terrain taller
		// than the tube radius squashed near the axis instead of piercing
		// through and coming out inverted on the far side
		float rho = max(R - l.z, 0.05 * R);
		// height = R - rho*cos(th) = R*(1-cos th) + ze*cos th where ze is the
		// height after the clamp; the 2sin^2 form keeps precision when R is
		// huge (curl near zero, i.e. nearly flat)
		float ze = R - rho;
		float hs = sin(0.5 * th);
		vec3 w = vec3(l.x, rho * sin(th), 2.0 * R * hs * hs + ze * cos(th));
		p = tube_local_to_clip * vec4(w, 1.0);
		}
	} else {
		v_rect = vec2(0.0);
		p = transform * vec4(position, 1.0);
	}
	p.z += (1.0 - mask) * 0.002 * p.w;
	gl_Position = p;
	v_sunv = (shadow_mv * vec4(position, 1.0)).xyz;
	v_nsun = sun_rot * normal;
	// view depth: w in perspective; in ortho w is 1 and ndc z is linear
	v_dist = fog_planes.z > 0.5
		? fog_planes.x + (p.z / p.w * 0.5 + 0.5) * (fog_planes.y - fog_planes.x) : p.w;
}
)glsl";

static const char *terrain_fs = R"glsl(#version 300 es
// gles 3.0 requires highp in fragment shaders, so the "use highp if the
// hardware has it, else mediump" dance webgl1 needed is gone -- and with
// it the fp16 path that garbled atlas uv interpolation on mobile gpus
precision highp float;
// not "texture": that names a built-in function in gles 3.00, and a
// uniform of the same name would hide it
uniform sampler2D tex;
uniform bool debug_lod;
uniform bool tube_on;
uniform mediump int tube_pass;
// aerial perspective: fog.x the visibility (m) at sea level, fog.y
// on/off; haze what distance fades toward; tint the sun's colour on
// the ground (warm and dim at a low sun, dark at night). the camera
// frame and eye (world, float -- metres of precision are plenty for
// a height) recover the fragment's world height from its depth
uniform vec4 fog;
uniform vec3 haze;
uniform vec3 tint;
uniform mat3 cam; // columns: right, up, forward
uniform vec3 eye;
uniform vec3 fog_planes; // (the vertex stage's; same uniform)
uniform vec4 cam_params; // tan(fov/2), aspect, planet radius, scale height
uniform vec2 viewport;
in float v_dist;
// sun shadows: x on, y strength (how dark the shade is), z one
// shadow texel in uv, w the depth bias; box is the map's half
// widths and its near/far along the sun (metres)
uniform highp sampler2DShadow shadow_map;
uniform vec4 shadow_params;
uniform vec4 shadow_box;
uniform vec3 shadow_up; // straight up at the eye, in the sun's frame
uniform vec3 eye_sun; // the eye, in the sun's frame
in vec3 v_sunv;
in vec3 v_nsun;
uniform float tube_fade;
uniform bool body_on;
in vec2 v_texcoords;
in float v_loop;
in vec3 v_local;
flat in float v_stale;
in float v_mask;
in vec2 v_rect;
out vec4 frag_color;
void main() {
	if (v_mask < 0.004) discard;
	// tube mode: clip the slab to the drawn rectangle, so tiles straddling
	// the edge (and geometry rolled past the seam) end cleanly. the flat
	// pass keeps the other side, the land around; and the rect itself,
	// the ground the sheet lifted off, it paints near-black -- a facet
	// normal from the screen-space slope of the rect-local position,
	// catching a little of a fixed light so the relief still reads
	if (tube_on) {
		bool inside = abs(v_rect.x) <= 1.0 && abs(v_rect.y) <= 1.0;
		if (tube_pass == 2 && inside) {
			vec3 n = normalize(cross(dFdx(v_local), dFdy(v_local)));
			float lit = abs(dot(n, normalize(vec3(0.4, 0.3, 1.0))));
			frag_color = vec4(vec3(0.012) + 0.05 * lit, 1.0);
			return;
		}
		if (tube_pass != 2 && !inside) discard;
		// a triangle spanning two of the figure's loops: its loop id
		// interpolates to a fraction somewhere inside it
		if (body_on && tube_pass == 1 && abs(v_loop - 1000.0 * floor(v_loop / 1000.0 + 0.5)) > 0.5) discard;
	}
	vec3 c = texture(tex, v_texcoords).rgb * tint;
	// the sun: faces turned away from it are in their own shade (the
	// face normal from the screen-space slope of the sun-frame
	// position, which points at the camera, so +z is toward the sun),
	// and inside the map's box so is whatever something nearer the sun
	// hides. the shade is lit by the sky instead, so it goes a little
	// blue. 3x3 taps over the hardware's 2x2 comparison filter
	if (shadow_params.x > 0.5 && !tube_on) {
		// the face's normal; a crack-fill line has no face, so it takes
		// the vertices' instead, turned toward the camera like a face's.
		// a one-pixel line's two screen derivatives both run along it, so
		// the test is relative -- nearly parallel, not merely nonzero -- or
		// the cross of two almost-equal vectors passes for a normal, and
		// every seam across a shaded wall came out lit
		vec3 ddx = dFdx(v_sunv), ddy = dFdy(v_sunv);
		vec3 cr = cross(ddx, ddy);
		vec3 n;
		if (dot(cr, cr) > 0.0025 * dot(ddx, ddx) * dot(ddy, ddy)) n = normalize(cr);
		else {
			n = dot(v_nsun, v_nsun) > 1e-6 ? normalize(v_nsun) : shadow_up;
			if (dot(n, eye_sun - v_sunv) < 0.0) n = -n;
		}
		// only on steep faces -- walls, cliffs, slopes past 25 degrees or
		// so: the photogrammetry's water is bumpy (reflections defeat the
		// stereo), and at a low sun every bump turned into a hard facet;
		// gentler ground gets its shade from the cast shadows, which a
		// ridge throws across the slope behind it anyway. a plain lambert
		// ramp, half lit edge-on: measured against flat ground (dividing
		// by the sun's height) it amplified the tiny differences between
		// a facade's triangles five times at a low sun, and an edge-on
		// wall came out streaked along every triangle edge
		float steep = 1.0 - smoothstep(0.75, 0.92, dot(n, shadow_up));
		float lit = mix(1.0, smoothstep(-0.25, 0.3, n.z), steep);
		// debug (?shadowdebug=): 2 the facing term alone, 3 cast shadows alone
		if (shadow_params.x > 2.5) lit = 1.0;
		// normal offset: look the map up off the surface, so it doesn't
		// shadow itself -- a texel and a half facing the sun, up to four
		// times that where the sun only skims it. raising the lookup by h
		// drops shadows from anything less than h taller than the
		// receiver, which at a grazing sun is the point: the
		// photogrammetry's water is bumpy, and a low sun stretched every
		// bump into a streak, while a building keeps its shadow (shorter
		// by h / tan(sun) at the far end, still attached at the base)
		float texel_m = shadow_params.z * 2.0 * shadow_box.x;
		float graze = 1.0 - clamp(n.z, 0.0, 1.0);
		vec3 q = v_sunv + n * (1.5 * texel_m * (1.0 + 3.0 * graze * graze));
		vec2 uv = q.xy / (2.0 * shadow_box.xy) + 0.5;
		float d = (-q.z - shadow_box.z) / (shadow_box.w - shadow_box.z) - shadow_params.w;
		if (shadow_params.x < 1.5 || shadow_params.x > 2.5)
		if (uv.x > 0.0 && uv.y > 0.0 && uv.x < 1.0 && uv.y < 1.0 && d < 1.0) {
			float s = 0.0;
			for (int i = -1; i <= 1; i++)
				for (int j = -1; j <= 1; j++)
					s += texture(shadow_map, vec3(uv + vec2(float(i), float(j)) * shadow_params.z, d));
			vec2 e = min(uv, 1.0 - uv);
			float edge = smoothstep(0.0, 0.04, min(e.x, e.y));
			lit = min(lit, mix(1.0, s / 9.0, edge));
		}
		vec3 shade = (1.0 - shadow_params.y) * vec3(0.88, 0.95, 1.12);
		c *= mix(shade, vec3(1.0), lit);
	}
	// aerial perspective: light from a distance is scattered out on the
	// way and the sky's light scattered in takes its place, so what is
	// far fades toward the haze -- per channel, the red surviving the
	// trip best, which is why distance goes blue-grey. not in the tube,
	// whose distances are through the roll and whose ends fade to
	// black on their own
	if (fog.y > 0.5 && !tube_on) {
		// the air thins with height, so the optical depth of the way
		// here is the distance times the mean density along it -- taken
		// between the eye's height and this fragment's, exponential in
		// height with the scale height H. from orbit that leaves the
		// planet clear with a hazy limb instead of a pale disc
		vec2 ndc = gl_FragCoord.xy / viewport * 2.0 - 1.0;
		vec3 off = vec3(ndc.x * cam_params.x * cam_params.y, ndc.y * cam_params.x, 1.0);
		if (fog_planes.z > 0.5) off = vec3(0.0, 0.0, 1.0);
		vec3 w = eye + cam * (off * v_dist);
		float H = cam_params.w;
		float ha = max(length(eye) - cam_params.z, 0.0) / H;
		float hf = max(length(w) - cam_params.z, 0.0) / H;
		float dh = ha - hf;
		float mean = abs(dh) < 1e-3 ? exp(-hf) : (exp(-hf) - exp(-ha)) / dh;
		float d = v_dist * length(off) * mean;
		vec3 e = exp(-d / (fog.x * vec3(1.3, 1.0, 0.75)));
		c = mix(haze, c, e);
	}
	// the lod overlay: a finer tile is wanted here and isn't drawn yet;
	// the colour says where its request is. yellow: not yet requested
	// (queued behind the download slots). blue: being looked up in
	// the tile cache. red: on the network. green: decoding. magenta:
	// failed, waiting to retry
	if (debug_lod && v_stale > 0.5) {
		int ph = int(v_stale + 0.5);
		vec3 t = ph == 1 ? vec3(1.0, 0.9, 0.0) : ph == 2 ? vec3(0.1, 0.5, 1.0)
			: ph == 3 ? vec3(1.0, 0.0, 0.0) : ph == 4 ? vec3(0.0, 1.0, 0.2) : vec3(1.0, 0.0, 1.0);
		c = mix(c, t, 0.5);
	}
	// the tunnel down the heading: the walls go dark toward both ends,
	// so the pipe runs off into black rather than stopping at a hole
	if (tube_on && tube_fade > 0.0) c *= 1.0 - smoothstep(tube_fade, 1.0, abs(v_rect.x));
	frag_color = vec4(c, 1.0);
}
)glsl";
