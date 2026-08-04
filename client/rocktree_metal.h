// Metal renderer backend (macOS native). Compiled as Objective-C++ from
// main.cpp; see rocktree_gl.h for the OpenGL backend used by the wasm build.
// Same per-mesh draw structure as the GL backend — no batching — but Metal's
// per-draw encoder cost is a fraction of Apple's GL-on-Metal shim, which is
// the whole point of this backend.

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL_metal.h>

struct render_ctx_t {
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	CAMetalLayer *layer;
	id<MTLRenderPipelineState> pso;
	id<MTLDepthStencilState> depth_state;
	id<MTLSamplerState> sampler;
	id<MTLTexture> depth_tex;
	int depth_w, depth_h;

	// per-frame state
	NSAutoreleasePool *pool;
	id<CAMetalDrawable> drawable;
	id<MTLCommandBuffer> cmdbuf;
	id<MTLRenderCommandEncoder> encoder;
	bool committed; // cmdbuf already committed (by a readback)

	float transform[16];
	bool debug_lod;
};

// must match the MSL Uniforms struct below (float4x4 is column-major, same
// layout as the eigen/gl matrices used everywhere else)
struct metal_uniforms_t {
	float transform[16];
	float uv_offset[2];
	float uv_scale[2];
	uint32_t octant_mask;
	uint32_t stale_mask;
	uint32_t flags; // bit 0: debug lod tint
	uint32_t pad;
};

// the one device everything is created from (set in renderInit; bufferMesh
// runs later on the render thread)
static id<MTLDevice> metal_device = nil;

static const char *metal_shader_src = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
	float4x4 transform;
	float2 uv_offset;
	float2 uv_scale;
	uint octant_mask;
	uint stale_mask;
	uint flags;
	uint pad;
};

struct VSIn {
	uchar3 position [[attribute(0)]];
	uchar octant [[attribute(1)]];
	ushort2 texcoords [[attribute(2)]];
};

struct VSOut {
	float4 pos [[position]];
	float2 uv;
	float stale;
	float maskv;
};

vertex VSOut vmain(VSIn in [[stage_in]], constant Uniforms &u [[buffer(1)]]) {
	VSOut o;
	uint oct = uint(in.octant);
	// masking: a triangle is dropped only when ALL its vertices are in
	// masked octants (maskv interpolates to 0 -> fragment discard);
	// straddling triangles draw, pushed slightly away in depth so the
	// finer tile wins wherever they overlap it (see the GL shader)
	float mask = ((u.octant_mask >> oct) & 1u) ? 0.0 : 1.0;
	o.maskv = mask;
	o.stale = ((u.stale_mask >> oct) & 1u) ? 1.0 : 0.0;
	o.uv = (float2(in.texcoords) + u.uv_offset) * u.uv_scale;
	float4 p = u.transform * float4(float3(in.position), 1.0);
	p.z += (1.0 - mask) * 0.002 * p.w;
	// the matrices produce gl clip z in [-w,w]; metal wants [0,w]
	p.z = (p.z + p.w) * 0.5;
	o.pos = p;
	return o;
}

fragment float4 fmain(VSOut v [[stage_in]], constant Uniforms &u [[buffer(0)]],
		texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
	if (v.maskv < 0.004) discard_fragment();
	float3 c = tex.sample(smp, v.uv).rgb;
	if (u.flags & 1u) c = mix(c, float3(1.0, 0.0, 0.0), v.stale * 0.5);
	return float4(c, 1.0);
}
)MSL";

void renderInit(render_ctx_t &ctx, SDL_Window *window) {
	ctx.device = MTLCreateSystemDefaultDevice();
	if (!ctx.device) fprintf(stderr, "no metal device\n"), abort();
	metal_device = ctx.device;

	auto view = SDL_Metal_CreateView(window);
	if (!view) fprintf(stderr, "SDL_Metal_CreateView failed: %s\n", SDL_GetError()), abort();
	ctx.layer = (CAMetalLayer *)SDL_Metal_GetLayer(view);
	ctx.layer.device = ctx.device;
	ctx.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	ctx.layer.framebufferOnly = NO; // the bench rig blits the drawable for readback

	ctx.queue = [ctx.device newCommandQueue];

	NSError *err = nil;
	id<MTLLibrary> lib = [ctx.device newLibraryWithSource:[NSString stringWithUTF8String:metal_shader_src]
		options:nil error:&err];
	if (!lib) fprintf(stderr, "metal shader compile failed: %s\n", err.localizedDescription.UTF8String), abort();

	// vertex layout matches the packed 8-byte vertex: xyz u8, octant u8, uv u16x2
	MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
	vd.attributes[0].format = MTLVertexFormatUChar3;
	vd.attributes[0].offset = 0;
	vd.attributes[0].bufferIndex = 0;
	vd.attributes[1].format = MTLVertexFormatUChar;
	vd.attributes[1].offset = 3;
	vd.attributes[1].bufferIndex = 0;
	vd.attributes[2].format = MTLVertexFormatUShort2;
	vd.attributes[2].offset = 4;
	vd.attributes[2].bufferIndex = 0;
	vd.layouts[0].stride = 8;

	MTLRenderPipelineDescriptor *pd = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
	pd.vertexFunction = [[lib newFunctionWithName:@"vmain"] autorelease];
	pd.fragmentFunction = [[lib newFunctionWithName:@"fmain"] autorelease];
	pd.vertexDescriptor = vd;
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	ctx.pso = [ctx.device newRenderPipelineStateWithDescriptor:pd error:&err];
	if (!ctx.pso) fprintf(stderr, "metal pipeline failed: %s\n", err.localizedDescription.UTF8String), abort();
	[lib release];

	MTLDepthStencilDescriptor *dd = [[[MTLDepthStencilDescriptor alloc] init] autorelease];
	dd.depthCompareFunction = MTLCompareFunctionLess;
	dd.depthWriteEnabled = YES;
	ctx.depth_state = [ctx.device newDepthStencilStateWithDescriptor:dd];

	MTLSamplerDescriptor *sd = [[[MTLSamplerDescriptor alloc] init] autorelease];
	sd.minFilter = MTLSamplerMinMagFilterLinear;
	sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	ctx.sampler = [ctx.device newSamplerStateWithDescriptor:sd];
}

void renderDrawableSize(render_ctx_t &ctx, SDL_Window *window, int *w, int *h) {
	SDL_Metal_GetDrawableSize(window, w, h);
}

void renderFrameBegin(render_ctx_t &ctx, SDL_Window *window, int width, int height, int sky) {
	ctx.pool = [[NSAutoreleasePool alloc] init];
	ctx.committed = false;

	if (ctx.layer.drawableSize.width != width || ctx.layer.drawableSize.height != height)
		ctx.layer.drawableSize = CGSizeMake(width, height);
	if (!ctx.depth_tex || ctx.depth_w != width || ctx.depth_h != height) {
		[ctx.depth_tex release];
		MTLTextureDescriptor *td = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
			width:width height:height mipmapped:NO];
		td.storageMode = MTLStorageModePrivate;
		td.usage = MTLTextureUsageRenderTarget;
		ctx.depth_tex = [ctx.device newTextureWithDescriptor:td];
		ctx.depth_w = width;
		ctx.depth_h = height;
	}

	ctx.drawable = [[ctx.layer nextDrawable] retain];
	if (!ctx.drawable) return; // window occluded etc; draws become no-ops

	ctx.cmdbuf = [[ctx.queue commandBuffer] retain];
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	rp.colorAttachments[0].texture = ctx.drawable.texture;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = MTLClearColorMake(
		(sky>>16 & 0xff) / 255.0, (sky>>8 & 0xff) / 255.0, (sky & 0xff) / 255.0, 1.0);
	rp.depthAttachment.texture = ctx.depth_tex;
	rp.depthAttachment.loadAction = MTLLoadActionClear;
	rp.depthAttachment.storeAction = MTLStoreActionDontCare;
	rp.depthAttachment.clearDepth = 1.0;

	ctx.encoder = [[ctx.cmdbuf renderCommandEncoderWithDescriptor:rp] retain];
	[ctx.encoder setRenderPipelineState:ctx.pso];
	[ctx.encoder setDepthStencilState:ctx.depth_state];
	// same screen-space orientation as gl (ndc +y is up in both), so the
	// mesh data's ccw front faces stay ccw
	[ctx.encoder setCullMode:MTLCullModeBack];
	[ctx.encoder setFrontFacingWinding:MTLWindingCounterClockwise];
	[ctx.encoder setFragmentSamplerState:ctx.sampler atIndex:0];
}

void renderSetDebugLod(render_ctx_t &ctx, bool debug_lod) {
	ctx.debug_lod = debug_lod;
}

void renderSetTransform(render_ctx_t &ctx, const float *m16) {
	memcpy(ctx.transform, m16, sizeof(ctx.transform));
}

void bufferMesh(rocktree_t::node_t::mesh_t &mesh) {
	if (mesh.buffered) fprintf(stderr, "mesh already buffered\n"), abort();
	auto device = metal_device;

	// zero-length metal buffers are nil; some meshes have empty index/vertex
	// data (e.g. all indices truncated away by the layer bound), so guard
	// creation here and the draws below
	mesh.vertex_buffer = mesh.vertices.size()
		? [device newBufferWithBytes:mesh.vertices.data()
			length:mesh.vertices.size() options:MTLResourceStorageModeShared]
		: nullptr;
	mesh.index_buffer = mesh.indices.size()
		? [device newBufferWithBytes:mesh.indices.data()
			length:mesh.indices.size() * sizeof(uint16_t) options:MTLResourceStorageModeShared]
		: nullptr;
	mesh.boundary_index_buffer = mesh.boundary_indices.size()
		? [device newBufferWithBytes:mesh.boundary_indices.data()
			length:mesh.boundary_indices.size() * sizeof(uint16_t) options:MTLResourceStorageModeShared]
		: nullptr;

	id<MTLTexture> tex;
	if (mesh.texture_format == rocktree_t::texture_format_dxt1) {
		MTLTextureDescriptor *td = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatBC1_RGBA
			width:mesh.texture_width height:mesh.texture_height mipmapped:NO];
		td.storageMode = MTLStorageModeShared;
		tex = [device newTextureWithDescriptor:td];
		auto blocks_wide = (mesh.texture_width + 3) / 4;
		[tex replaceRegion:MTLRegionMake2D(0, 0, mesh.texture_width, mesh.texture_height)
			mipmapLevel:0 withBytes:mesh.texture.data() bytesPerRow:(NSUInteger)blocks_wide * 8];
	} else {
		// metal has no rgb8 format; expand to rgba
		MTLTextureDescriptor *td = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
			width:mesh.texture_width height:mesh.texture_height mipmapped:NO];
		td.storageMode = MTLStorageModeShared;
		tex = [device newTextureWithDescriptor:td];
		std::vector<uint8_t> rgba((size_t)mesh.texture_width * mesh.texture_height * 4);
		auto src = mesh.texture.data();
		for (size_t i = 0; i < (size_t)mesh.texture_width * mesh.texture_height; i++) {
			rgba[i*4+0] = src[i*3+0];
			rgba[i*4+1] = src[i*3+1];
			rgba[i*4+2] = src[i*3+2];
			rgba[i*4+3] = 255;
		}
		[tex replaceRegion:MTLRegionMake2D(0, 0, mesh.texture_width, mesh.texture_height)
			mipmapLevel:0 withBytes:rgba.data() bytesPerRow:(NSUInteger)mesh.texture_width * 4];
	}
	mesh.texture_buffer = tex;

	mesh.buffered = true;
}

void unbufferMesh(rocktree_t::node_t::mesh_t &mesh) {
	if (!mesh.buffered) fprintf(stderr, "mesh isn't buffered\n"), abort();
	mesh.buffered = false;
	[(id<MTLBuffer>)mesh.vertex_buffer release];
	[(id<MTLBuffer>)mesh.index_buffer release];
	[(id<MTLBuffer>)mesh.boundary_index_buffer release];
	[(id<MTLTexture>)mesh.texture_buffer release];
	mesh.vertex_buffer = mesh.index_buffer = mesh.boundary_index_buffer = mesh.texture_buffer = nullptr;
}

void bindAndDrawMesh(const rocktree_t::node_t::mesh_t &mesh, uint8_t octant_mask, uint8_t stale_mask, render_ctx_t &ctx) {
	if (!ctx.encoder) return;

	metal_uniforms_t u;
	memcpy(u.transform, ctx.transform, sizeof(u.transform));
	u.uv_offset[0] = mesh.uv_offset[0];
	u.uv_offset[1] = mesh.uv_offset[1];
	u.uv_scale[0] = mesh.uv_scale[0];
	u.uv_scale[1] = mesh.uv_scale[1];
	u.octant_mask = octant_mask;
	u.stale_mask = stale_mask;
	u.flags = ctx.debug_lod ? 1 : 0;
	u.pad = 0;

	if (!mesh.vertex_buffer) return;
	[ctx.encoder setVertexBuffer:(id<MTLBuffer>)mesh.vertex_buffer offset:0 atIndex:0];
	[ctx.encoder setVertexBytes:&u length:sizeof(u) atIndex:1];
	[ctx.encoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
	[ctx.encoder setFragmentTexture:(id<MTLTexture>)mesh.texture_buffer atIndex:0];
	if (mesh.index_buffer)
		[ctx.encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip
			indexCount:mesh.indices.size() indexType:MTLIndexTypeUInt16
			indexBuffer:(id<MTLBuffer>)mesh.index_buffer indexBufferOffset:0];

	// crack fill: re-draw the mesh's boundary edges as lines; the depth test
	// only lets them land on background pixels showing through a slit.
	// metal lines are always 1px (no glLineWidth equivalent)
	static const bool no_lines_debug = getenv("EARTH_NO_LINES") != nullptr;
	if (!no_lines_debug && mesh.boundary_index_buffer) {
		[ctx.encoder drawIndexedPrimitives:MTLPrimitiveTypeLine
			indexCount:mesh.boundary_indices.size() indexType:MTLIndexTypeUInt16
			indexBuffer:(id<MTLBuffer>)mesh.boundary_index_buffer indexBufferOffset:0];
	}
}

void renderFrameEnd(render_ctx_t &ctx) {
	if (!ctx.encoder) return;
	[ctx.encoder endEncoding];
	[ctx.encoder release];
	ctx.encoder = nil;
}

// read the frame back as tightly packed rgb, bottom-up rows (gl convention,
// which the bench rig's ppm reader/writer assume). call between frameEnd and
// present; commits the frame's command buffer and waits for the gpu
bool renderReadPixels(render_ctx_t &ctx, int w, int h, uint8_t *rgb) {
	if (!ctx.drawable || !ctx.cmdbuf) return false;

	id<MTLBuffer> buf = [ctx.device newBufferWithLength:(NSUInteger)w * h * 4
		options:MTLResourceStorageModeShared];
	id<MTLBlitCommandEncoder> blit = [ctx.cmdbuf blitCommandEncoder];
	[blit copyFromTexture:ctx.drawable.texture sourceSlice:0 sourceLevel:0
		sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
		toBuffer:buf destinationOffset:0
		destinationBytesPerRow:(NSUInteger)w * 4
		destinationBytesPerImage:(NSUInteger)w * h * 4];
	[blit endEncoding];
	[ctx.cmdbuf commit];
	ctx.committed = true;
	[ctx.cmdbuf waitUntilCompleted];

	// bgra top-down -> rgb bottom-up
	auto src = (const uint8_t *)buf.contents;
	for (int y = 0; y < h; y++) {
		auto row = src + (size_t)(h - 1 - y) * w * 4;
		auto dst = rgb + (size_t)y * w * 3;
		for (int x = 0; x < w; x++) {
			dst[x*3+0] = row[x*4+2];
			dst[x*3+1] = row[x*4+1];
			dst[x*3+2] = row[x*4+0];
		}
	}
	[buf release];
	return true;
}

void renderPresent(render_ctx_t &ctx, SDL_Window *window) {
	if (ctx.drawable) {
		if (!ctx.committed) {
			[ctx.cmdbuf presentDrawable:ctx.drawable];
			[ctx.cmdbuf commit];
		} else {
			// the frame was committed early by a readback; present separately
			id<MTLCommandBuffer> cb = [ctx.queue commandBuffer];
			[cb presentDrawable:ctx.drawable];
			[cb commit];
		}
	}
	[ctx.cmdbuf release];
	ctx.cmdbuf = nil;
	[ctx.drawable release];
	ctx.drawable = nil;
	[ctx.pool drain];
	ctx.pool = nil;
}
