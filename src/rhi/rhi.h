// Warren -- the render hardware interface.
//
// TWO BACKENDS, ONE CONTRACT. Vulkan and OpenGL are both first class:
// neither is a fallback for the other, both are expected to run every
// feature the engine has, and this file is the thing they must agree
// about. It is written to Vulkan's shape -- baked pipelines, explicit
// render passes, descriptor sets, command lists -- because that shape
// can be emulated on OpenGL cheaply while the reverse cannot be done at
// all.
//
// THE FOUR DECISIONS THAT MAKE ONE CONTRACT POSSIBLE.
//
// 1. ONE CLIP SPACE. Depth in [0, 1] reversed, +Y up. Vulkan gets a
//    negative-height viewport, OpenGL gets glClipControl. No shader and
//    no matrix anywhere in the engine knows which backend it is on.
//
// 2. DYNAMIC RENDERING ONLY. No VkRenderPass, no VkFramebuffer objects:
//    `begin_rendering` takes its attachments inline. Vulkan 1.3 has
//    this in core and OpenGL has never had anything else, so the two
//    converge instead of one pretending to be the other.
//
// 3. ONE SHADER SOURCE. GLSL 4.50 with Vulkan bindings, compiled to
//    SPIR-V for Vulkan and cross-compiled to GLSL 460 for OpenGL, both
//    at build time. Binding numbers are globally unique -- set*8 + slot
//    -- so the flattening that OpenGL needs cannot collide.
//
// 4. STENCIL REFERENCE IS DYNAMIC STATE. Recursive portals change it
//    per portal per level; baking it into pipelines would mean one
//    pipeline per recursion depth.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math/projection.h"

namespace wr::rhi {

// ---------------------------------------------------------------- handles

// Generational: a handle to a destroyed resource is detected rather
// than followed into freed memory, which is the difference between a
// clear error message and a driver crash three frames later.
template <class Tag>
struct Handle {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    explicit operator bool() const { return valid(); }
    bool operator==(const Handle &o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const Handle &o) const { return !(*this == o); }
};

using BufferH = Handle<struct BufferTag>;
using TextureH = Handle<struct TextureTag>;
using SamplerH = Handle<struct SamplerTag>;
using ShaderH = Handle<struct ShaderTag>;
using PipelineH = Handle<struct PipelineTag>;
using BindGroupH = Handle<struct BindGroupTag>;
using BindGroupLayoutH = Handle<struct BindGroupLayoutTag>;

// ------------------------------------------------------------ enumerations

enum class Backend : uint8_t { Vulkan, OpenGL };
const char *backend_name(Backend b);

enum class Format : uint16_t {
    Undefined = 0,
    // colour
    R8, RG8, RGBA8, RGBA8_SRGB, BGRA8, BGRA8_SRGB,
    R16F, RG16F, RGBA16F,
    R32F, RG32F, RGB32F, RGBA32F,
    RGB10A2, RG11B10F,
    R8UI, R16UI, R32UI,
    // FOUR BYTES AS INTEGERS, NOT NORMALISED. RGBA8 is the same four
    // bytes read as 0..1, which is right for a colour and wrong for a
    // bone index: joint 3 of 19 must arrive as 3 and not as 0.0118.
    RGBA8UI,
    // THE depth-stencil format. Float depth because the engine is
    // reverse-Z, stencil because it does portals, and this is the only
    // format that is both.
    D32F_S8,
    D32F,
    D24_S8,
    // block compressed
    BC1, BC3, BC5, BC7, BC7_SRGB,
    Count
};
bool format_is_depth(Format f);
bool format_has_stencil(Format f);
bool format_is_srgb(Format f);
uint32_t format_block_size(Format f);
const char *format_name(Format f);

enum class BufferUsage : uint32_t {
    None = 0,
    Vertex = 1 << 0,
    Index = 1 << 1,
    Uniform = 1 << 2,
    Storage = 1 << 3,
    Indirect = 1 << 4,
    TransferSrc = 1 << 5,
    TransferDst = 1 << 6,
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return BufferUsage(uint32_t(a) | uint32_t(b));
}
inline bool operator&(BufferUsage a, BufferUsage b) {
    return (uint32_t(a) & uint32_t(b)) != 0;
}

enum class TextureUsage : uint32_t {
    None = 0,
    Sampled = 1 << 0,
    ColourTarget = 1 << 1,
    DepthTarget = 1 << 2,
    Storage = 1 << 3,
    TransferSrc = 1 << 4,
    TransferDst = 1 << 5,
};
inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return TextureUsage(uint32_t(a) | uint32_t(b));
}
inline bool operator&(TextureUsage a, TextureUsage b) {
    return (uint32_t(a) & uint32_t(b)) != 0;
}

// Where a buffer lives and who writes it.
enum class MemoryAccess : uint8_t {
    GpuOnly,      // fastest; written through a staging copy
    CpuToGpu,     // host visible, written every frame (uniforms, dynamic geometry)
    GpuToCpu,     // readback
};

enum class TextureDim : uint8_t { Tex2D, Tex2DArray, TexCube, Tex3D };

enum class Filter : uint8_t { Nearest, Linear };
enum class MipFilter : uint8_t { None, Nearest, Linear };
enum class AddressMode : uint8_t { Repeat, MirrorRepeat, ClampEdge, ClampBorder };
enum class BorderColour : uint8_t { TransparentBlack, OpaqueBlack, OpaqueWhite };

enum class CompareOp : uint8_t {
    Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always
};

enum class StencilOp : uint8_t {
    Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert,
    IncrementWrap, DecrementWrap
};

enum class BlendFactor : uint8_t {
    Zero, One, SrcColour, OneMinusSrcColour, DstColour, OneMinusDstColour,
    SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, ConstantColour,
    OneMinusConstantColour
};
enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

enum class CullMode : uint8_t { None, Front, Back };
enum class FrontFace : uint8_t { CounterClockwise, Clockwise };
enum class PolygonMode : uint8_t { Fill, Line, Point };

enum class Topology : uint8_t {
    TriangleList, TriangleStrip, LineList, LineStrip, PointList
};

enum class IndexType : uint8_t { U16, U32 };

enum class ShaderStage : uint8_t { Vertex, Fragment, Compute };

enum class LoadOp : uint8_t { Load, Clear, DontCare };
enum class StoreOp : uint8_t { Store, DontCare };

enum class BindingType : uint8_t {
    UniformBuffer,
    UniformBufferDynamic,   // offset supplied per bind, for per-draw data
    StorageBuffer,
    SampledTexture,         // texture + sampler, combined
    StorageTexture,
};

// ------------------------------------------------------------- descriptions

struct BufferDesc {
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryAccess access = MemoryAccess::GpuOnly;
    const char *name = nullptr;
};

struct TextureDesc {
    uint32_t width = 1, height = 1, depth = 1;
    uint32_t layers = 1;
    uint32_t mips = 1;           // 0 means "all the way down"
    uint32_t samples = 1;
    TextureDim dim = TextureDim::Tex2D;
    Format format = Format::RGBA8;
    TextureUsage usage = TextureUsage::Sampled;
    const char *name = nullptr;
};

struct SamplerDesc {
    Filter min = Filter::Linear;
    Filter mag = Filter::Linear;
    MipFilter mip = MipFilter::Linear;
    AddressMode address_u = AddressMode::Repeat;
    AddressMode address_v = AddressMode::Repeat;
    AddressMode address_w = AddressMode::Repeat;
    float anisotropy = 1.0f;
    float lod_bias = 0.0f;
    float min_lod = 0.0f;
    float max_lod = 1000.0f;
    // A shadow sampler compares rather than fetches.
    bool compare_enable = false;
    CompareOp compare = CompareOp::GreaterEqual;  // reverse-Z
    BorderColour border = BorderColour::OpaqueWhite;
    const char *name = nullptr;
};

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    // SPIR-V, for the Vulkan backend.
    const uint32_t *spirv = nullptr;
    size_t spirv_words = 0;
    // GLSL 460, cross-compiled from the same SPIR-V, for the OpenGL
    // backend. Both are produced by the build and shipped together, so
    // a shader is never missing the half the running backend needs.
    const char *glsl = nullptr;
    const char *entry = "main";
    const char *name = nullptr;
};

struct VertexAttribute {
    uint32_t location = 0;
    uint32_t binding = 0;
    Format format = Format::RGBA32F;
    uint32_t offset = 0;
};

struct VertexBinding {
    uint32_t binding = 0;
    uint32_t stride = 0;
    bool per_instance = false;
};

struct VertexLayout {
    std::vector<VertexBinding> bindings;
    std::vector<VertexAttribute> attributes;
};

struct StencilFace {
    StencilOp fail = StencilOp::Keep;
    StencilOp depth_fail = StencilOp::Keep;
    StencilOp pass = StencilOp::Keep;
    CompareOp compare = CompareOp::Always;
    uint8_t compare_mask = 0xFF;
    uint8_t write_mask = 0xFF;
    // The reference value is NOT here: it is dynamic state, set per
    // draw. See the note at the top of this file.
};

struct DepthStencilState {
    bool depth_test = true;
    bool depth_write = true;
    // Reverse-Z: nearer is GREATER. This default is the engine's whole
    // depth convention in one line.
    CompareOp depth_compare = CompareOp::GreaterEqual;
    bool stencil_test = false;
    StencilFace front;
    StencilFace back;
    bool depth_bias_enable = false;
    float depth_bias_constant = 0.0f;
    float depth_bias_slope = 0.0f;
    float depth_bias_clamp = 0.0f;
};

struct BlendState {
    bool enable = false;
    BlendFactor src_colour = BlendFactor::SrcAlpha;
    BlendFactor dst_colour = BlendFactor::OneMinusSrcAlpha;
    BlendOp colour_op = BlendOp::Add;
    BlendFactor src_alpha = BlendFactor::One;
    BlendFactor dst_alpha = BlendFactor::OneMinusSrcAlpha;
    BlendOp alpha_op = BlendOp::Add;
    // Per-channel writes. Turning colour off entirely is how the portal
    // renderer writes depth and stencil without touching the picture.
    bool write_r = true, write_g = true, write_b = true, write_a = true;

    static BlendState opaque() { return {}; }
    static BlendState alpha() {
        BlendState b;
        b.enable = true;
        return b;
    }
    static BlendState additive() {
        BlendState b;
        b.enable = true;
        b.src_colour = BlendFactor::SrcAlpha;
        b.dst_colour = BlendFactor::One;
        b.src_alpha = BlendFactor::Zero;
        b.dst_alpha = BlendFactor::One;
        return b;
    }
    static BlendState no_colour() {
        BlendState b;
        b.write_r = b.write_g = b.write_b = b.write_a = false;
        return b;
    }
};

struct RasterState {
    CullMode cull = CullMode::Back;
    FrontFace front_face = FrontFace::CounterClockwise;
    PolygonMode polygon = PolygonMode::Fill;
    float line_width = 1.0f;
    // Clamp instead of clip at the near plane. A shadow caster behind
    // the light's near plane should still cast, not vanish.
    bool depth_clamp = false;
};

struct BindGroupLayoutEntry {
    uint32_t binding = 0;         // globally unique: set * 8 + slot
    BindingType type = BindingType::UniformBuffer;
    // Which stages may see it. Vulkan needs this; OpenGL ignores it.
    bool vertex = true;
    bool fragment = true;
    bool compute = false;
    uint32_t count = 1;           // > 1 for an array of textures
};

struct BindGroupLayoutDesc {
    std::vector<BindGroupLayoutEntry> entries;
    const char *name = nullptr;
};

struct BindGroupEntry {
    uint32_t binding = 0;
    BufferH buffer;
    uint64_t offset = 0;
    uint64_t range = 0;           // 0 means to the end
    TextureH texture;
    SamplerH sampler;
    uint32_t mip = 0;             // for a storage image
    int32_t layer = -1;           // -1 means the whole texture
};

struct BindGroupDesc {
    BindGroupLayoutH layout;
    std::vector<BindGroupEntry> entries;
    const char *name = nullptr;
};

// Everything the rasteriser needs, baked. Creating one may compile
// shaders, so it happens at load time and never inside a frame.
struct PipelineDesc {
    ShaderH vertex;
    ShaderH fragment;
    VertexLayout vertex_layout;
    Topology topology = Topology::TriangleList;
    RasterState raster;
    DepthStencilState depth_stencil;
    // One per colour attachment.
    std::vector<BlendState> blend;
    // The attachment formats this pipeline will be used with. Vulkan
    // needs them at creation; OpenGL ignores them but the validation
    // layer in the RHI checks them against begin_rendering, which
    // catches a whole class of "works on GL, black on Vulkan" bug.
    std::vector<Format> colour_formats;
    Format depth_format = Format::Undefined;
    uint32_t samples = 1;
    std::vector<BindGroupLayoutH> bind_group_layouts;
    // At most 128 bytes: what Vulkan guarantees, so what the engine
    // allows. create_pipeline refuses more and says so.
    uint32_t push_constant_size = 0;
    const char *name = nullptr;
};

struct ComputePipelineDesc {
    ShaderH compute;
    std::vector<BindGroupLayoutH> bind_group_layouts;
    uint32_t push_constant_size = 0;
    const char *name = nullptr;
};

// ------------------------------------------------------------- rendering

union ClearValue {
    struct { float r, g, b, a; } colour;
    struct { float depth; uint32_t stencil; } depth_stencil;
};

struct ColourAttachment {
    TextureH texture;
    uint32_t mip = 0;
    int32_t layer = -1;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    Color clear{0, 0, 0, 1};
    // Multisample resolve target, if any.
    TextureH resolve;
};

struct DepthAttachment {
    TextureH texture;
    uint32_t mip = 0;
    int32_t layer = -1;
    LoadOp depth_load = LoadOp::Clear;
    StoreOp depth_store = StoreOp::Store;
    LoadOp stencil_load = LoadOp::Clear;
    StoreOp stencil_store = StoreOp::Store;
    // Reverse-Z clears to ZERO, not one. Every engine that switches to
    // reverse-Z gets this wrong once and sees a blank screen.
    float clear_depth = 0.0f;
    uint32_t clear_stencil = 0;
};

struct RenderingInfo {
    std::vector<ColourAttachment> colour;
    DepthAttachment depth;
    bool has_depth = false;
    uint32_t width = 0, height = 0;
    const char *name = nullptr;
};

// VIEWPORTS AND SCISSORS ARE IN ONE CONVENTION: origin at the TOP-LEFT
// of the target, y increasing downwards, height positive. It matches
// the screen rectangles the renderer computes and the row order
// read_texture returns.
//
// Neither backend takes it in that form natively -- Vulkan needs a
// negative height to put +Y up, OpenGL measures y from the bottom --
// and both convert here rather than at the call site. A caller that
// has to remember which backend it is on has no abstraction.
struct Viewport {
    float x = 0, y = 0, width = 0, height = 0;
    float min_depth = 0.0f, max_depth = 1.0f;
};

struct Rect {
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

// ---------------------------------------------------------- command list

class CommandList {
public:
    virtual ~CommandList() = default;

    // --- passes ---------------------------------------------------------
    virtual void begin_rendering(const RenderingInfo &info) = 0;
    virtual void end_rendering() = 0;

    // --- state -----------------------------------------------------------
    virtual void set_viewport(const Viewport &vp) = 0;
    virtual void set_scissor(const Rect &r) = 0;
    virtual void bind_pipeline(PipelineH p) = 0;
    virtual void bind_group(uint32_t set, BindGroupH g,
                            const uint32_t *dynamic_offsets = nullptr,
                            uint32_t dynamic_count = 0) = 0;
    virtual void push_constants(const void *data, uint32_t size,
                                uint32_t offset = 0) = 0;
    // Dynamic, because recursive portals change it per portal.
    virtual void set_stencil_reference(uint32_t ref) = 0;
    virtual void set_blend_constant(const Color &c) = 0;

    // --- geometry -----------------------------------------------------------
    virtual void bind_vertex_buffer(uint32_t slot, BufferH b,
                                    uint64_t offset = 0) = 0;
    virtual void bind_index_buffer(BufferH b, IndexType type,
                                   uint64_t offset = 0) = 0;
    virtual void draw(uint32_t vertices, uint32_t instances = 1,
                      uint32_t first_vertex = 0, uint32_t first_instance = 0) = 0;
    virtual void draw_indexed(uint32_t indices, uint32_t instances = 1,
                              uint32_t first_index = 0, int32_t vertex_offset = 0,
                              uint32_t first_instance = 0) = 0;
    virtual void draw_indexed_indirect(BufferH args, uint64_t offset,
                                       uint32_t draw_count, uint32_t stride) = 0;

    // --- compute --------------------------------------------------------------
    virtual void dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;

    // --- transfers and synchronisation ------------------------------------------
    virtual void copy_buffer(BufferH src, uint64_t src_offset, BufferH dst,
                             uint64_t dst_offset, uint64_t size) = 0;
    virtual void copy_buffer_to_texture(BufferH src, uint64_t src_offset,
                                        TextureH dst, uint32_t mip, uint32_t layer,
                                        uint32_t width, uint32_t height) = 0;
    virtual void generate_mips(TextureH t) = 0;
    // Make previous writes to `t` visible to later reads. The OpenGL
    // backend mostly ignores these; the Vulkan one needs every one.
    virtual void texture_barrier(TextureH t, TextureUsage from,
                                 TextureUsage to) = 0;
    virtual void buffer_barrier(BufferH b) = 0;

    // --- debug --------------------------------------------------------------------
    // Shows up in RenderDoc and in the Vulkan validation output. The
    // portal renderer labels every recursion level, which is the only
    // practical way to read a capture of one.
    virtual void push_debug_group(const char *name, const Color &c = Color(0.4f, 0.6f, 1.0f, 1)) = 0;
    virtual void pop_debug_group() = 0;
    virtual void insert_debug_marker(const char *name) = 0;
};

// A scope guard for the above, so an early return cannot unbalance it.
struct DebugScope {
    CommandList *cmd;
    DebugScope(CommandList *c, const char *name) : cmd(c) {
        if (cmd) cmd->push_debug_group(name);
    }
    ~DebugScope() {
        if (cmd) cmd->pop_debug_group();
    }
    DebugScope(const DebugScope &) = delete;
};
#define WR_GPU_SCOPE(cmd, name) ::wr::rhi::DebugScope mf_gpu_scope_##__LINE__(cmd, name)

// ----------------------------------------------------------------- device

struct DeviceCaps {
    Backend backend = Backend::Vulkan;
    std::string device_name;
    std::string driver_info;
    std::string api_version;
    bool discrete = false;
    uint32_t max_texture_2d = 0;
    uint32_t max_texture_layers = 0;
    uint32_t max_colour_attachments = 0;
    uint32_t max_push_constant_size = 0;
    uint32_t max_anisotropy = 1;
    uint32_t uniform_buffer_alignment = 256;
    uint32_t storage_buffer_alignment = 256;
    uint32_t max_samples = 1;
    bool supports_compute = true;
    bool supports_indirect = true;
    bool supports_geometry_shaders = false;
    // STENCIL IS NOT OPTIONAL. The engine refuses to start without it,
    // because without it there are no portals.
    uint32_t stencil_bits = 0;
    uint64_t vram_bytes = 0;
};

struct FrameStats {
    uint32_t draw_calls = 0;
    uint32_t dispatches = 0;
    uint64_t triangles = 0;
    uint32_t pipeline_binds = 0;
    uint32_t bind_group_binds = 0;
    uint32_t render_passes = 0;
    // What this engine is for, so it gets its own counters.
    uint32_t portal_views = 0;
    uint32_t portal_max_depth = 0;
    double cpu_frame_ms = 0.0;
    double gpu_frame_ms = 0.0;
};

struct DeviceDesc {
    Backend backend = Backend::Vulkan;
    // The SDL_Window the swapchain presents to.
    void *window = nullptr;
    bool validation = false;
    bool vsync = true;
    // 2 is enough to hide a frame of latency; 3 smooths a jittery one.
    uint32_t frames_in_flight = 2;
    // Multisampling on the main colour target.
    uint32_t samples = 4;
};

class Device {
public:
    virtual ~Device() = default;

    virtual const DeviceCaps &caps() const = 0;
    virtual Backend backend() const = 0;

    // --- resources ---------------------------------------------------------
    virtual BufferH create_buffer(const BufferDesc &d,
                                  const void *initial = nullptr) = 0;
    virtual void destroy(BufferH h) = 0;
    // For CpuToGpu buffers: a pointer valid until the frame is
    // submitted. Null for GpuOnly, which must go through `write_buffer`.
    virtual void *map(BufferH h) = 0;
    virtual void unmap(BufferH h) = 0;
    // Stages and copies. Safe to call at any time; the copy happens
    // before the next frame's first draw.
    virtual void write_buffer(BufferH h, const void *data, uint64_t size,
                              uint64_t offset = 0) = 0;

    virtual TextureH create_texture(const TextureDesc &d,
                                    const void *initial = nullptr) = 0;
    virtual void destroy(TextureH h) = 0;
    // FILL IN THE MIP CHAIN of a texture that was created with one
    // and written at level zero.
    //
    // Creating a texture with mips allocates the whole chain and
    // uploads only the top of it; the rest is whatever the driver
    // left there, which is black. Every minified sample then reads
    // black, so a texture looks right on screen at full size and
    // goes dark at a distance -- a bug that hides completely until
    // something in the scene has detail in it.
    //
    // On the immediate backend this happens at once. On Vulkan it is
    // recorded at the start of the next frame, after the uploads it
    // depends on and before anything that could sample it.
    virtual void generate_mips(TextureH h) = 0;
    virtual void write_texture(TextureH h, const void *data, uint64_t size,
                               uint32_t mip = 0, uint32_t layer = 0) = 0;
    virtual TextureDesc texture_desc(TextureH h) const = 0;
    // BLOCKING. Waits for the GPU, copies the texture back to host
    // memory and returns the bytes actually written. For screenshots
    // and for the backend-parity tests -- never in a frame.
    //
    // ROW 0 IS THE TOP OF THE PICTURE, on both backends. They do not
    // agree natively: an OpenGL texture is stored from the bottom up
    // and a Vulkan image from the top down, so the OpenGL backend
    // flips on the way out. Without one stated order, a screenshot is
    // upside down on one renderer and a parity test compares an image
    // with its own reflection.
    virtual size_t read_texture(TextureH h, void *out, size_t capacity,
                                uint32_t mip = 0, uint32_t layer = 0) = 0;

    virtual SamplerH create_sampler(const SamplerDesc &d) = 0;
    virtual void destroy(SamplerH h) = 0;

    virtual ShaderH create_shader(const ShaderDesc &d) = 0;
    virtual void destroy(ShaderH h) = 0;

    virtual BindGroupLayoutH create_bind_group_layout(
        const BindGroupLayoutDesc &d) = 0;
    virtual void destroy(BindGroupLayoutH h) = 0;
    virtual BindGroupH create_bind_group(const BindGroupDesc &d) = 0;
    virtual void destroy(BindGroupH h) = 0;
    // Rewrite an existing group in place, so a material that changed a
    // texture does not leak a group per frame.
    virtual void update_bind_group(BindGroupH h, const BindGroupDesc &d) = 0;

    virtual PipelineH create_pipeline(const PipelineDesc &d) = 0;
    virtual PipelineH create_compute_pipeline(const ComputePipelineDesc &d) = 0;
    virtual void destroy(PipelineH h) = 0;

    // --- the frame -------------------------------------------------------------
    // Acquires a swapchain image and returns the list to record into.
    // Null when the swapchain needs rebuilding, in which case the caller
    // should skip the frame.
    // Ask for the next presented frame to be readable. It must be
    // called BEFORE end_frame, and read_texture(swapchain_texture())
    // after it.
    //
    // The two backends can only catch the image at different moments:
    // Vulkan's presented image survives until it is reacquired and can
    // be copied afterwards, while OpenGL's back buffer is gone the
    // instant SwapWindow returns, so it has to be grabbed inside
    // end_frame before the swap. One flag hides that difference;
    // without it, screenshots work on one renderer and come out black
    // on the other.
    virtual void request_capture() = 0;

    virtual CommandList *begin_frame() = 0;
    virtual void end_frame() = 0;
    virtual TextureH swapchain_texture() const = 0;
    virtual Format swapchain_format() const = 0;
    virtual uint32_t swapchain_width() const = 0;
    virtual uint32_t swapchain_height() const = 0;
    virtual void resize_swapchain(uint32_t w, uint32_t h) = 0;
    virtual void set_vsync(bool on) = 0;
    // Block until the GPU is idle. Only for shutdown and for resizing.
    virtual void wait_idle() = 0;

    virtual const FrameStats &stats() const = 0;

    // --- diagnostics ---------------------------------------------------------------
    // Human-readable dump of every live resource, for a leak hunt.
    virtual std::string resource_report() const = 0;

    // WHICH FRAME SLOT IS BEING RECORDED, and how many there are.
    //
    // A renderer writing a uniform or storage buffer every frame
    // CANNOT use one buffer. The CPU runs ahead: while the GPU is
    // still drawing frame N-1, the CPU is writing frame N, and a
    // host-visible write is a memcpy straight through a persistent
    // mapping with nothing to stop it landing in the middle of a
    // draw that is reading it. What comes out is some draws using
    // this frame's data and some using last frame's -- on a
    // skinned figure, the shirt a frame behind the body.
    //
    // So anything written per frame needs one copy per slot, and
    // these are how a renderer indexes them. An immediate backend
    // reports one slot and zero, which makes the same code correct
    // and free there.
    virtual uint32_t frame_slot() const = 0;
    virtual uint32_t frames_in_flight() const = 0;

    // HOW MANY DESTROYED RESOURCES ARE STILL WAITING FOR THE GPU.
    //
    // A backend that records command buffers ahead of the GPU cannot
    // free a resource the moment destroy() is called -- a frame that
    // is still executing may be reading it. So destruction is
    // deferred until the fence of the last frame that could reference
    // it has been waited on.
    //
    // This counts what is outstanding. It is a diagnostic, and it is
    // also the only way to TEST the deferral without depending on how
    // fast the GPU happens to be: the rule is that a resource
    // destroyed between frames survives at least one more begin_frame,
    // and that is checkable exactly.
    //
    // An immediate backend has nothing to defer and reports zero.
    virtual size_t pending_deletions() const = 0;
};

// Creates the requested backend, or returns null with the reason
// logged. Falling back from Vulkan to OpenGL is the CALLER's decision,
// not this function's: an engine that silently drops to a different
// renderer is an engine whose bug reports are useless.
Device *create_device(const DeviceDesc &desc);
void destroy_device(Device *d);

// Which backends this build can actually create, most preferred first.
std::vector<Backend> available_backends();

}  // namespace wr::rhi
