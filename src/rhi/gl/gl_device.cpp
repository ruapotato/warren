// Warren -- the OpenGL 4.5 backend.
//
// OpenGL is not the fallback here. It is the backend that runs
// everywhere, on drivers a decade old, on machines where Vulkan's
// loader is missing or its driver is a liability, and it is expected to
// produce a picture identical to Vulkan's -- not similar, identical,
// because the portal tests compare them.
//
// The work of this file is pretending to be Vulkan-shaped: baked
// pipelines over a state machine, descriptor sets over binding points,
// dynamic rendering over framebuffer objects. Each of those is a cache
// and a diff, and all three are cheap.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "rhi/gl/glfn.h"
#include "rhi/handle_pool.h"
#include "rhi/rhi.h"

namespace wr::rhi {
namespace {

// ------------------------------------------------------------ conversion

GLenum gl_compare(CompareOp c) {
    switch (c) {
        case CompareOp::Never: return GL_NEVER;
        case CompareOp::Less: return GL_LESS;
        case CompareOp::Equal: return GL_EQUAL;
        case CompareOp::LessEqual: return GL_LEQUAL;
        case CompareOp::Greater: return GL_GREATER;
        case CompareOp::NotEqual: return GL_NOTEQUAL;
        case CompareOp::GreaterEqual: return GL_GEQUAL;
        case CompareOp::Always: return GL_ALWAYS;
    }
    return GL_ALWAYS;
}

GLenum gl_stencil_op(StencilOp o) {
    switch (o) {
        case StencilOp::Keep: return GL_KEEP;
        case StencilOp::Zero: return GL_ZERO;
        case StencilOp::Replace: return GL_REPLACE;
        case StencilOp::IncrementClamp: return GL_INCR;
        case StencilOp::DecrementClamp: return GL_DECR;
        case StencilOp::Invert: return GL_INVERT;
        case StencilOp::IncrementWrap: return GL_INCR_WRAP;
        case StencilOp::DecrementWrap: return GL_DECR_WRAP;
    }
    return GL_KEEP;
}

GLenum gl_blend_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero: return GL_ZERO;
        case BlendFactor::One: return GL_ONE;
        case BlendFactor::SrcColour: return GL_SRC_COLOR;
        case BlendFactor::OneMinusSrcColour: return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColour: return GL_DST_COLOR;
        case BlendFactor::OneMinusDstColour: return GL_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha: return GL_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
        case BlendFactor::ConstantColour: return GL_CONSTANT_COLOR;
        case BlendFactor::OneMinusConstantColour: return GL_ONE_MINUS_CONSTANT_COLOR;
    }
    return GL_ONE;
}

GLenum gl_blend_op(BlendOp o) {
    switch (o) {
        case BlendOp::Add: return GL_FUNC_ADD;
        case BlendOp::Subtract: return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOp::Min: return GL_MIN;
        case BlendOp::Max: return GL_MAX;
    }
    return GL_FUNC_ADD;
}

GLenum gl_topology(Topology t) {
    switch (t) {
        case Topology::TriangleList: return GL_TRIANGLES;
        case Topology::TriangleStrip: return GL_TRIANGLE_STRIP;
        case Topology::LineList: return GL_LINES;
        case Topology::LineStrip: return GL_LINE_STRIP;
        case Topology::PointList: return GL_POINTS;
    }
    return GL_TRIANGLES;
}

struct GlFormat {
    GLenum internal, format, type;
    bool compressed;
};

GlFormat gl_format(Format f) {
    switch (f) {
        case Format::R8: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE, false};
        case Format::RG8: return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, false};
        case Format::RGBA8: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false};
        case Format::RGBA8_SRGB: return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, false};
        case Format::BGRA8: return {GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, false};
        case Format::BGRA8_SRGB: return {GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE, false};
        case Format::R16F: return {GL_R16F, GL_RED, GL_HALF_FLOAT, false};
        case Format::RG16F: return {GL_RG16F, GL_RG, GL_HALF_FLOAT, false};
        case Format::RGBA16F: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, false};
        case Format::R32F: return {GL_R32F, GL_RED, GL_FLOAT, false};
        case Format::RG32F: return {GL_RG32F, GL_RG, GL_FLOAT, false};
        case Format::RGB32F: return {GL_RGB32F, GL_RGB, GL_FLOAT, false};
        case Format::RGBA32F: return {GL_RGBA32F, GL_RGBA, GL_FLOAT, false};
        case Format::RGB10A2: return {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, false};
        case Format::RG11B10F: return {GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, false};
        case Format::R8UI: return {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, false};
        case Format::R16UI: return {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, false};
        case Format::R32UI: return {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, false};
        case Format::RGBA8UI: return {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, false};
        case Format::D32F_S8:
            return {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, false};
        case Format::D32F: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, false};
        case Format::D24_S8: return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, false};
        case Format::BC1: return {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_RGBA, GL_UNSIGNED_BYTE, true};
        case Format::BC3: return {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_RGBA, GL_UNSIGNED_BYTE, true};
        case Format::BC5: return {GL_COMPRESSED_RG_RGTC2, GL_RG, GL_UNSIGNED_BYTE, true};
        case Format::BC7: return {GL_COMPRESSED_RGBA_BPTC_UNORM, GL_RGBA, GL_UNSIGNED_BYTE, true};
        case Format::BC7_SRGB: return {GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, GL_RGBA, GL_UNSIGNED_BYTE, true};
        default: return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false};
    }
}

// The vertex attribute formats the engine actually uses.
void gl_attrib_format(Format f, GLint *size, GLenum *type, GLboolean *norm,
                      bool *integer) {
    *norm = GL_FALSE;
    *integer = false;
    switch (f) {
        case Format::R32F: *size = 1; *type = GL_FLOAT; return;
        case Format::RG32F: *size = 2; *type = GL_FLOAT; return;
        case Format::RGB32F: *size = 3; *type = GL_FLOAT; return;
        case Format::RGBA32F: *size = 4; *type = GL_FLOAT; return;
        case Format::RGBA8: *size = 4; *type = GL_UNSIGNED_BYTE; *norm = GL_TRUE; return;
        case Format::RG16F: *size = 2; *type = GL_HALF_FLOAT; return;
        case Format::RGBA16F: *size = 4; *type = GL_HALF_FLOAT; return;
        case Format::R32UI: *size = 1; *type = GL_UNSIGNED_INT; *integer = true; return;
        case Format::RGBA8UI: *size = 4; *type = GL_UNSIGNED_BYTE; *integer = true; return;
        default: *size = 3; *type = GL_FLOAT; return;
    }
}

// ------------------------------------------------------------- resources

struct GlBuffer {
    GLuint id = 0;
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryAccess access = MemoryAccess::GpuOnly;
    void *mapped = nullptr;
    std::string name;
};

struct GlTexture {
    GLuint id = 0;
    TextureDesc desc;
    GLenum target = GL_TEXTURE_2D;
    // THE SWAPCHAIN IS A REAL TEXTURE HERE.
    //
    // OpenGL's default framebuffer is not one, and standing a handle
    // in for it means a special case in every path that touches a
    // render target: it cannot be read back like a texture, it cannot
    // be sampled, and -- since glClipControl puts the origin at the
    // upper left while the window system still presents bottom-up --
    // anything drawn into it comes out inverted.
    //
    // So the backend renders to a texture of its own and blits it to
    // framebuffer zero, flipped, at present. One blit a frame buys a
    // swapchain that behaves exactly like Vulkan's.
    bool is_swapchain = false;
};

struct GlSampler {
    GLuint id = 0;
    SamplerDesc desc;
};

struct GlShader {
    ShaderStage stage = ShaderStage::Vertex;
    std::string glsl;
    std::string name;
};

struct GlBindGroupLayout {
    BindGroupLayoutDesc desc;
};

struct GlBindGroup {
    BindGroupDesc desc;
};

struct GlPipeline {
    GLuint program = 0;
    GLuint vao = 0;                 // format only; buffers bound per draw
    PipelineDesc desc;
    bool compute = false;
    // Where the cross-compiler put the push-constant block, if any.
    GLuint push_block_index = GL_INVALID_INDEX;
    uint32_t push_size = 0;
};

// ----------------------------------------------------------- the device

class GlDevice;

class GlCommandList final : public CommandList {
public:
    explicit GlCommandList(GlDevice *d) : dev_(d) {}

    void begin_rendering(const RenderingInfo &info) override;
    void end_rendering() override;
    void set_viewport(const Viewport &vp) override;
    void set_scissor(const Rect &r) override;
    void bind_pipeline(PipelineH p) override;
    void bind_group(uint32_t set, BindGroupH g, const uint32_t *offsets,
                    uint32_t count) override;
    void push_constants(const void *data, uint32_t size, uint32_t offset) override;
    void set_stencil_reference(uint32_t ref) override;
    void set_blend_constant(const Color &c) override;
    void bind_vertex_buffer(uint32_t slot, BufferH b, uint64_t offset) override;
    void bind_index_buffer(BufferH b, IndexType type, uint64_t offset) override;
    void draw(uint32_t vertices, uint32_t instances, uint32_t first_vertex,
              uint32_t first_instance) override;
    void draw_indexed(uint32_t indices, uint32_t instances, uint32_t first_index,
                      int32_t vertex_offset, uint32_t first_instance) override;
    void draw_indexed_indirect(BufferH args, uint64_t offset, uint32_t draw_count,
                               uint32_t stride) override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override;
    void copy_buffer(BufferH src, uint64_t so, BufferH dst, uint64_t dof,
                     uint64_t size) override;
    void copy_buffer_to_texture(BufferH src, uint64_t so, TextureH dst,
                                uint32_t mip, uint32_t layer, uint32_t w,
                                uint32_t h) override;
    void generate_mips(TextureH t) override;
    void texture_barrier(TextureH t, TextureUsage from, TextureUsage to) override;
    void buffer_barrier(BufferH b) override;
    void push_debug_group(const char *name, const Color &c) override;
    void pop_debug_group() override;
    void insert_debug_marker(const char *name) override;

private:
    GlDevice *dev_;
    PipelineH bound_pipeline_;
    GLuint bound_vao_ = 0;
    IndexType index_type_ = IndexType::U32;
    uint64_t index_offset_ = 0;
    uint32_t stencil_ref_ = 0;
    int debug_depth_ = 0;
    // Kept so end_rendering knows what to resolve. Vulkan does this
    // itself through the attachment's resolve mode; OpenGL has to be
    // told, with a blit.
    RenderingInfo current_pass_;
    bool pass_open_ = false;
    // The height of whatever is being rendered into, so a y-down
    // viewport or scissor can be turned into OpenGL's y-up one.
    uint32_t target_height_ = 0;
    friend class GlDevice;
};

class GlDevice final : public Device {
public:
    GlDevice() = default;
    ~GlDevice() override;

    bool init(const DeviceDesc &d);

    const DeviceCaps &caps() const override { return caps_; }
    Backend backend() const override { return Backend::OpenGL; }

    BufferH create_buffer(const BufferDesc &d, const void *initial) override;
    void destroy(BufferH h) override;
    void *map(BufferH h) override;
    void unmap(BufferH h) override;
    void write_buffer(BufferH h, const void *data, uint64_t size,
                      uint64_t offset) override;

    TextureH create_texture(const TextureDesc &d, const void *initial) override;
    void destroy(TextureH h) override;
    void generate_mips(TextureH h) override;
    void write_texture(TextureH h, const void *data, uint64_t size, uint32_t mip,
                       uint32_t layer) override;
    TextureDesc texture_desc(TextureH h) const override;
    size_t read_texture(TextureH h, void *out, size_t capacity, uint32_t mip,
                        uint32_t layer) override;

    SamplerH create_sampler(const SamplerDesc &d) override;
    void destroy(SamplerH h) override;

    ShaderH create_shader(const ShaderDesc &d) override;
    void destroy(ShaderH h) override;

    BindGroupLayoutH create_bind_group_layout(const BindGroupLayoutDesc &d) override;
    void destroy(BindGroupLayoutH h) override;
    BindGroupH create_bind_group(const BindGroupDesc &d) override;
    void destroy(BindGroupH h) override;
    void update_bind_group(BindGroupH h, const BindGroupDesc &d) override;

    PipelineH create_pipeline(const PipelineDesc &d) override;
    PipelineH create_compute_pipeline(const ComputePipelineDesc &d) override;
    void destroy(PipelineH h) override;

    void request_capture() override { capture_requested_ = true; }
    CommandList *begin_frame() override;
    void end_frame() override;
    TextureH swapchain_texture() const override { return swapchain_; }
    Format swapchain_format() const override { return swapchain_format_; }
    uint32_t swapchain_width() const override { return width_; }
    uint32_t swapchain_height() const override { return height_; }
    void resize_swapchain(uint32_t w, uint32_t h) override;
    void set_vsync(bool on) override;
    void wait_idle() override { glFinish(); }
    const FrameStats &stats() const override { return stats_; }
    std::string resource_report() const override;
    // Immediate: a GL call is ordered against everything before it,
    // so a delete needs no deferral and nothing is ever outstanding.
    size_t pending_deletions() const override { return 0; }

    // --- used by the command list -----------------------------------------
    HandlePool<GlBuffer, BufferH> buffers;
    HandlePool<GlTexture, TextureH> textures;
    HandlePool<GlSampler, SamplerH> samplers;
    HandlePool<GlShader, ShaderH> shaders;
    HandlePool<GlBindGroupLayout, BindGroupLayoutH> layouts;
    HandlePool<GlBindGroup, BindGroupH> groups;
    HandlePool<GlPipeline, PipelineH> pipelines;
    FrameStats stats_;

    GLuint framebuffer_for(const RenderingInfo &info);
    bool make_swapchain_texture();
    void apply_pipeline_state(const PipelineDesc &d, uint32_t stencil_ref);
    // The uniform buffer standing in for Vulkan's push constants.
    GLuint push_buffer() const { return push_ubo_; }

    static constexpr uint32_t kPushBinding = 31;

private:
    SDL_Window *window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    DeviceCaps caps_;
    TextureH swapchain_;
    Format swapchain_format_ = Format::RGBA8_SRGB;
    uint32_t width_ = 0, height_ = 0;
    GlCommandList *cmd_ = nullptr;
    GLuint push_ubo_ = 0;
    // Framebuffer objects, cached by the attachments they wrap, because
    // dynamic rendering has no framebuffer object and creating one per
    // pass per frame is a measurable cost.
    std::map<std::vector<uint64_t>, GLuint> fbo_cache_;
    // The last state applied, so a pipeline bind is a diff.
    PipelineDesc applied_;
    bool applied_valid_ = false;
    bool capture_requested_ = false;
    uint32_t applied_stencil_ref_ = 0xFFFFFFFF;
    friend class GlCommandList;
};

// ------------------------------------------------------------------ init

bool GlDevice::init(const DeviceDesc &d) {
    window_ = (SDL_Window *)d.window;
    if (!window_) {
        WR_FATAL("gl: no window");
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    context_ = SDL_GL_CreateContext(window_);
    if (!context_) {
        WR_FATAL("gl: no 4.5 core context: %s", SDL_GetError());
        return false;
    }
    int missing = gl::load((void *(*)(const char *))SDL_GL_GetProcAddress);
    if (missing) {
        int n = 0;
        const char *const *names = gl::missing(&n);
        WR_FATAL("gl: driver is missing %d entry points, first %s", n,
                 n ? names[0] : "?");
        return false;
    }

    // THE CLIP SPACE, AND THE FRAMEBUFFER ORIGIN, IN ONE CALL.
    //
    // ZERO_TO_ONE gives Vulkan's depth range. UPPER_LEFT gives Vulkan's
    // framebuffer origin, and that second half matters more than it
    // looks: without it, a render target's texel (0, 0) is the
    // BOTTOM-left corner of the image on OpenGL and the TOP-left on
    // Vulkan, so every pass that samples a previous pass -- tonemap,
    // bloom, screen-space anything -- comes out upside down on one of
    // them. It is a difference that hides until the first full-screen
    // effect and then looks like a bug in that effect.
    //
    // With both halves set, the two backends agree about depth, about
    // which way is up, about gl_FragCoord, about texel order in a
    // render target, and about the row order of a readback. Winding
    // needs no compensation: ARB_clip_control negates the polygon-area
    // sign for an upper-left origin, so facing is unchanged.
    glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);

    glEnable(GL_FRAMEBUFFER_SRGB);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glFrontFace(GL_CCW);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    caps_.backend = Backend::OpenGL;
    const char *rend = (const char *)glGetString(GL_RENDERER);
    const char *ver = (const char *)glGetString(GL_VERSION);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    caps_.device_name = rend ? rend : "unknown";
    caps_.api_version = ver ? ver : "unknown";
    caps_.driver_info = vendor ? vendor : "unknown";
    GLint v = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v); caps_.max_texture_2d = uint32_t(v);
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &v); caps_.max_texture_layers = uint32_t(v);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &v); caps_.max_colour_attachments = uint32_t(v);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &v);
    caps_.uniform_buffer_alignment = uint32_t(v > 0 ? v : 256);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &v);
    caps_.storage_buffer_alignment = uint32_t(v > 0 ? v : 256);
    glGetIntegerv(GL_MAX_SAMPLES, &v); caps_.max_samples = uint32_t(v);
    GLfloat af = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &af);
    caps_.max_anisotropy = uint32_t(af);
    // OpenGL has no push constants; the engine's 128-byte budget is
    // emulated with a uniform buffer, so report the same limit and the
    // renderer needs no special case.
    caps_.max_push_constant_size = 128;
    caps_.supports_compute = true;
    caps_.supports_indirect = true;
    caps_.discrete = false;
    int sb = 0;
    SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &sb);
    caps_.stencil_bits = uint32_t(sb);

    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window_, &w, &h);
    width_ = uint32_t(w);
    height_ = uint32_t(h);
    set_vsync(d.vsync);

    if (!make_swapchain_texture()) return false;

    glCreateBuffers(1, &push_ubo_);
    glNamedBufferData(push_ubo_, 128, nullptr, GL_DYNAMIC_DRAW);

    cmd_ = new GlCommandList(this);
    return true;
}

GlDevice::~GlDevice() {
    delete cmd_;
    for (auto &kv : fbo_cache_) glDeleteFramebuffers(1, &kv.second);
    if (push_ubo_) glDeleteBuffers(1, &push_ubo_);
    buffers.for_each([](GlBuffer &b) { if (b.id) glDeleteBuffers(1, &b.id); });
    textures.for_each([](GlTexture &t) { if (t.id) glDeleteTextures(1, &t.id); });
    samplers.for_each([](GlSampler &s) { if (s.id) glDeleteSamplers(1, &s.id); });
    pipelines.for_each([](GlPipeline &p) {
        if (p.program) glDeleteProgram(p.program);
        if (p.vao) glDeleteVertexArrays(1, &p.vao);
    });
    if (context_) SDL_GL_DeleteContext(context_);
}

// -------------------------------------------------------------- buffers

BufferH GlDevice::create_buffer(const BufferDesc &d, const void *initial) {
    BufferH h = buffers.create();
    GlBuffer *b = buffers.get(h);
    b->size = d.size;
    b->usage = d.usage;
    b->access = d.access;
    b->name = d.name ? d.name : "";
    glCreateBuffers(1, &b->id);
    GLbitfield flags = GL_DYNAMIC_STORAGE_BIT;
    if (d.access == MemoryAccess::CpuToGpu)
        flags |= GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    else if (d.access == MemoryAccess::GpuToCpu)
        flags |= GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glNamedBufferStorage(b->id, GLsizeiptr(d.size ? d.size : 4), initial, flags);
    if (d.name && glObjectLabel) glObjectLabel(GL_BUFFER, b->id, -1, d.name);
    return h;
}

void GlDevice::destroy(BufferH h) {
    if (GlBuffer *b = buffers.get(h)) {
        if (b->mapped) glUnmapNamedBuffer(b->id);
        if (b->id) glDeleteBuffers(1, &b->id);
    }
    buffers.destroy(h);
}

void *GlDevice::map(BufferH h) {
    GlBuffer *b = buffers.get_checked(h, "buffer");
    if (!b) return nullptr;
    if (b->access == MemoryAccess::GpuOnly) {
        WR_ERROR("gl: buffer '%s' is GpuOnly and cannot be mapped", b->name.c_str());
        return nullptr;
    }
    if (!b->mapped) {
        GLbitfield f = GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        f |= (b->access == MemoryAccess::CpuToGpu) ? GL_MAP_WRITE_BIT : GL_MAP_READ_BIT;
        b->mapped = glMapNamedBufferRange(b->id, 0, GLsizeiptr(b->size), f);
    }
    return b->mapped;
}

void GlDevice::unmap(BufferH h) {
    // Persistent mappings stay; coherent means the writes are already
    // visible. Keeping the pointer avoids a map/unmap pair per frame.
    (void)h;
}

void GlDevice::write_buffer(BufferH h, const void *data, uint64_t size,
                            uint64_t offset) {
    GlBuffer *b = buffers.get_checked(h, "buffer");
    if (!b || !data || !size) return;
    if (offset + size > b->size) {
        WR_ERROR("gl: write of %llu at %llu overflows buffer '%s' (%llu bytes)",
                 (unsigned long long)size, (unsigned long long)offset,
                 b->name.c_str(), (unsigned long long)b->size);
        return;
    }
    glNamedBufferSubData(b->id, GLintptr(offset), GLsizeiptr(size), data);
}

// -------------------------------------------------------------- textures

TextureH GlDevice::create_texture(const TextureDesc &d, const void *initial) {
    TextureH h = textures.create();
    GlTexture *t = textures.get(h);
    t->desc = d;
    GlFormat gf = gl_format(d.format);

    uint32_t mips = d.mips;
    if (mips == 0) {
        mips = 1;
        uint32_t w = d.width, ht = d.height;
        while (w > 1 || ht > 1) { w = w > 1 ? w / 2 : 1; ht = ht > 1 ? ht / 2 : 1; mips++; }
    }
    t->desc.mips = mips;

    switch (d.dim) {
        case TextureDim::Tex2D:
            t->target = d.samples > 1 ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
            break;
        case TextureDim::Tex2DArray: t->target = GL_TEXTURE_2D_ARRAY; break;
        case TextureDim::TexCube: t->target = GL_TEXTURE_CUBE_MAP; break;
        case TextureDim::Tex3D: t->target = GL_TEXTURE_3D; break;
    }
    glCreateTextures(t->target, 1, &t->id);

    if (d.samples > 1) {
        glTextureStorage2DMultisample(t->id, GLsizei(d.samples), gf.internal,
                                      GLsizei(d.width), GLsizei(d.height), GL_TRUE);
    } else if (d.dim == TextureDim::Tex2DArray) {
        glTextureStorage3D(t->id, GLsizei(mips), gf.internal, GLsizei(d.width),
                           GLsizei(d.height), GLsizei(d.layers));
    } else if (d.dim == TextureDim::Tex3D) {
        glTextureStorage3D(t->id, GLsizei(mips), gf.internal, GLsizei(d.width),
                           GLsizei(d.height), GLsizei(d.depth));
    } else if (d.dim == TextureDim::TexCube) {
        glTextureStorage2D(t->id, GLsizei(mips), gf.internal, GLsizei(d.width),
                           GLsizei(d.height));
    } else {
        glTextureStorage2D(t->id, GLsizei(mips), gf.internal, GLsizei(d.width),
                           GLsizei(d.height));
    }

    if (d.samples == 1) {
        // Filtering lives on the sampler object, but a texture bound
        // without one must still be legal, so give it sane defaults.
        glTextureParameteri(t->id, GL_TEXTURE_MIN_FILTER,
                            mips > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(t->id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(t->id, GL_TEXTURE_MAX_LEVEL, GLint(mips - 1));
    }
    if (d.name && glObjectLabel) glObjectLabel(GL_TEXTURE, t->id, -1, d.name);
    if (initial) {
        write_texture(h, initial, 0, 0, 0);
        // A texture given its pixels up front and asked for mips
        // wants the chain filled; nothing else is going to do it.
        if (mips > 1) generate_mips(h);
    }
    return h;
}

void GlDevice::generate_mips(TextureH h) {
    const GlTexture *t = textures.get(h);
    if (t && t->id && t->desc.mips > 1) glGenerateTextureMipmap(t->id);
}

void GlDevice::destroy(TextureH h) {
    if (GlTexture *t = textures.get(h)) {
        if (t->is_swapchain) return;  // not ours to delete
        if (t->id) glDeleteTextures(1, &t->id);
    }
    textures.destroy(h);
}

void GlDevice::write_texture(TextureH h, const void *data, uint64_t size,
                             uint32_t mip, uint32_t layer) {
    GlTexture *t = textures.get_checked(h, "texture");
    if (!t || !data) return;
    GlFormat gf = gl_format(t->desc.format);
    uint32_t w = std::max(1u, t->desc.width >> mip);
    uint32_t ht = std::max(1u, t->desc.height >> mip);
    if (gf.compressed) {
        glCompressedTextureSubImage2D(t->id, GLint(mip), 0, 0, GLsizei(w),
                                      GLsizei(ht), gf.internal, GLsizei(size),
                                      data);
        return;
    }
    if (t->desc.dim == TextureDim::Tex2DArray || t->desc.dim == TextureDim::Tex3D) {
        glTextureSubImage3D(t->id, GLint(mip), 0, 0, GLint(layer), GLsizei(w),
                            GLsizei(ht), 1, gf.format, gf.type, data);
    } else {
        glTextureSubImage2D(t->id, GLint(mip), 0, 0, GLsizei(w), GLsizei(ht),
                            gf.format, gf.type, data);
    }
}

TextureDesc GlDevice::texture_desc(TextureH h) const {
    const GlTexture *t = textures.get(h);
    return t ? t->desc : TextureDesc();
}

size_t GlDevice::read_texture(TextureH h, void *out, size_t capacity, uint32_t mip,
                              uint32_t layer) {
    GlTexture *t = textures.get_checked(h, "texture");
    if (!t || !out) return 0;
    (void)layer;

    uint32_t w = std::max(1u, t->desc.width >> mip);
    uint32_t hh = std::max(1u, t->desc.height >> mip);
    size_t need = size_t(w) * hh * format_block_size(t->desc.format);
    if (capacity < need) {
        WR_ERROR("gl: read_texture needs %zu bytes, given %zu", need, capacity);
        return 0;
    }
    glFinish();
    GlFormat gf = gl_format(t->desc.format);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // No flip: with GL_UPPER_LEFT the image is already stored the way
    // Vulkan stores it, which is the way the contract says to return it.
    glGetTextureImage(t->id, GLint(mip), gf.format, gf.type, GLsizei(need), out);
    return need;
}

// -------------------------------------------------------------- samplers

SamplerH GlDevice::create_sampler(const SamplerDesc &d) {
    SamplerH h = samplers.create();
    GlSampler *s = samplers.get(h);
    s->desc = d;
    glCreateSamplers(1, &s->id);
    GLenum minf = GL_LINEAR;
    if (d.min == Filter::Nearest)
        minf = d.mip == MipFilter::None      ? GL_NEAREST
               : d.mip == MipFilter::Nearest ? GL_NEAREST_MIPMAP_NEAREST
                                             : GL_NEAREST_MIPMAP_LINEAR;
    else
        minf = d.mip == MipFilter::None      ? GL_LINEAR
               : d.mip == MipFilter::Nearest ? GL_LINEAR_MIPMAP_NEAREST
                                             : GL_LINEAR_MIPMAP_LINEAR;
    glSamplerParameteri(s->id, GL_TEXTURE_MIN_FILTER, GLint(minf));
    glSamplerParameteri(s->id, GL_TEXTURE_MAG_FILTER,
                        d.mag == Filter::Nearest ? GL_NEAREST : GL_LINEAR);
    auto wrap = [](AddressMode m) -> GLint {
        switch (m) {
            case AddressMode::Repeat: return GL_REPEAT;
            case AddressMode::MirrorRepeat: return GL_MIRRORED_REPEAT;
            case AddressMode::ClampEdge: return GL_CLAMP_TO_EDGE;
            case AddressMode::ClampBorder: return GL_CLAMP_TO_BORDER;
        }
        return GL_REPEAT;
    };
    glSamplerParameteri(s->id, GL_TEXTURE_WRAP_S, wrap(d.address_u));
    glSamplerParameteri(s->id, GL_TEXTURE_WRAP_T, wrap(d.address_v));
    glSamplerParameteri(s->id, GL_TEXTURE_WRAP_R, wrap(d.address_w));
    if (d.anisotropy > 1.0f)
        glSamplerParameterf(s->id, GL_TEXTURE_MAX_ANISOTROPY, d.anisotropy);
    glSamplerParameterf(s->id, GL_TEXTURE_LOD_BIAS, d.lod_bias);
    glSamplerParameterf(s->id, GL_TEXTURE_MIN_LOD, d.min_lod);
    glSamplerParameterf(s->id, GL_TEXTURE_MAX_LOD, d.max_lod);
    if (d.compare_enable) {
        glSamplerParameteri(s->id, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s->id, GL_TEXTURE_COMPARE_FUNC, GLint(gl_compare(d.compare)));
    }
    // A shadow lookup outside the map must read "lit". White border,
    // and with reverse-Z lit is the maximum, which is 1.
    float border[4] = {1, 1, 1, 1};
    if (d.border == BorderColour::TransparentBlack) { border[0] = border[1] = border[2] = border[3] = 0; }
    else if (d.border == BorderColour::OpaqueBlack) { border[0] = border[1] = border[2] = 0; border[3] = 1; }
    glSamplerParameterfv(s->id, GL_TEXTURE_BORDER_COLOR, border);
    return h;
}

void GlDevice::destroy(SamplerH h) {
    if (GlSampler *s = samplers.get(h))
        if (s->id) glDeleteSamplers(1, &s->id);
    samplers.destroy(h);
}

// ---------------------------------------------------------------- shaders

ShaderH GlDevice::create_shader(const ShaderDesc &d) {
    if (!d.glsl) {
        WR_ERROR("gl: shader '%s' has no GLSL variant -- was it built with "
                 "spirv-cross?", d.name ? d.name : "?");
        return {};
    }
    ShaderH h = shaders.create();
    GlShader *s = shaders.get(h);
    s->stage = d.stage;
    s->glsl = d.glsl;
    s->name = d.name ? d.name : "";
    return h;
}

void GlDevice::destroy(ShaderH h) { shaders.destroy(h); }

// ------------------------------------------------------------ bind groups

BindGroupLayoutH GlDevice::create_bind_group_layout(const BindGroupLayoutDesc &d) {
    BindGroupLayoutH h = layouts.create();
    layouts.get(h)->desc = d;
    return h;
}
void GlDevice::destroy(BindGroupLayoutH h) { layouts.destroy(h); }

BindGroupH GlDevice::create_bind_group(const BindGroupDesc &d) {
    BindGroupH h = groups.create();
    groups.get(h)->desc = d;
    return h;
}
void GlDevice::destroy(BindGroupH h) { groups.destroy(h); }

void GlDevice::update_bind_group(BindGroupH h, const BindGroupDesc &d) {
    if (GlBindGroup *g = groups.get_checked(h, "bind group")) g->desc = d;
}

// ------------------------------------------------------------- pipelines

static GLuint compile_program(const std::vector<std::pair<GLenum, const GlShader *>> &stages,
                              const char *name) {
    std::vector<GLuint> objs;
    for (auto &st : stages) {
        GLuint s = glCreateShader(st.first);
        const char *src = st.second->glsl.c_str();
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            GLint len = 0;
            glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
            std::string log(size_t(len > 1 ? len : 1), '\0');
            glGetShaderInfoLog(s, len, nullptr, log.data());
            WR_ERROR("gl: '%s' stage failed to compile:\n%s", name, log.c_str());
            glDeleteShader(s);
            for (GLuint o : objs) glDeleteShader(o);
            return 0;
        }
        objs.push_back(s);
    }
    GLuint p = glCreateProgram();
    for (GLuint o : objs) glAttachShader(p, o);
    glLinkProgram(p);
    for (GLuint o : objs) { glDetachShader(p, o); glDeleteShader(o); }
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::string log(size_t(len > 1 ? len : 1), '\0');
        glGetProgramInfoLog(p, len, nullptr, log.data());
        WR_ERROR("gl: '%s' failed to link:\n%s", name, log.c_str());
        glDeleteProgram(p);
        return 0;
    }
    if (name && glObjectLabel) glObjectLabel(GL_PROGRAM, p, -1, name);
    return p;
}

PipelineH GlDevice::create_pipeline(const PipelineDesc &d) {
    if (d.push_constant_size > 128) {
        WR_ERROR("pipeline '%s' wants %u bytes of push constants; the engine's "
                 "budget is 128, which is what Vulkan guarantees",
                 d.name ? d.name : "?", d.push_constant_size);
        return {};
    }
    const GlShader *vs = shaders.get(d.vertex);
    const GlShader *fs = shaders.get(d.fragment);
    if (!vs || !fs) {
        WR_ERROR("gl: pipeline '%s' is missing a stage", d.name ? d.name : "?");
        return {};
    }
    GLuint prog = compile_program({{GL_VERTEX_SHADER, vs}, {GL_FRAGMENT_SHADER, fs}},
                                  d.name ? d.name : "pipeline");
    if (!prog) return {};

    PipelineH h = pipelines.create();
    GlPipeline *p = pipelines.get(h);
    p->program = prog;
    p->desc = d;
    p->push_size = d.push_constant_size;

    // Uniform blocks keep the binding number the cross-compiler gave
    // them, which is already globally unique. The push-constant block
    // is the exception: it arrives with no binding, so it is found by
    // name and pinned to a reserved slot.
    GLuint pb = glGetUniformBlockIndex(prog, "Push");
    if (pb != GL_INVALID_INDEX) {
        glUniformBlockBinding(prog, pb, kPushBinding);
        p->push_block_index = pb;
    }

    // The vertex format, as a VAO with no buffers attached. Buffers are
    // bound to it per draw, which is what separates format from data
    // the way Vulkan does.
    glCreateVertexArrays(1, &p->vao);
    for (const VertexAttribute &a : d.vertex_layout.attributes) {
        glEnableVertexArrayAttrib(p->vao, a.location);
        GLint size; GLenum type; GLboolean norm; bool integer;
        gl_attrib_format(a.format, &size, &type, &norm, &integer);
        if (integer)
            glVertexArrayAttribIFormat(p->vao, a.location, size, type, a.offset);
        else
            glVertexArrayAttribFormat(p->vao, a.location, size, type, norm, a.offset);
        glVertexArrayAttribBinding(p->vao, a.location, a.binding);
    }
    for (const VertexBinding &b : d.vertex_layout.bindings)
        glVertexArrayBindingDivisor(p->vao, b.binding, b.per_instance ? 1 : 0);
    return h;
}

PipelineH GlDevice::create_compute_pipeline(const ComputePipelineDesc &d) {
    const GlShader *cs = shaders.get(d.compute);
    if (!cs) return {};
    GLuint prog = compile_program({{GL_COMPUTE_SHADER, cs}},
                                  d.name ? d.name : "compute");
    if (!prog) return {};
    PipelineH h = pipelines.create();
    GlPipeline *p = pipelines.get(h);
    p->program = prog;
    p->compute = true;
    p->push_size = d.push_constant_size;
    GLuint pb = glGetUniformBlockIndex(prog, "Push");
    if (pb != GL_INVALID_INDEX) {
        glUniformBlockBinding(prog, pb, kPushBinding);
        p->push_block_index = pb;
    }
    return h;
}

void GlDevice::destroy(PipelineH h) {
    if (GlPipeline *p = pipelines.get(h)) {
        if (p->program) glDeleteProgram(p->program);
        if (p->vao) glDeleteVertexArrays(1, &p->vao);
    }
    pipelines.destroy(h);
}

// ------------------------------------------------------------ the frame

CommandList *GlDevice::begin_frame() {
    stats_ = FrameStats();
    applied_valid_ = false;
    return cmd_;
}

void GlDevice::end_frame() {
    // PRESENT: blit the swapchain texture to framebuffer zero, turned
    // over. glClipControl put the drawing origin at the upper left to
    // match Vulkan, but the window system still scans the default
    // framebuffer from the bottom, so the flip happens once, here,
    // rather than in every shader that touches a render target.
    if (const GlTexture *sc = textures.get(swapchain_)) {
        RenderingInfo si;
        ColourAttachment sa;
        sa.texture = swapchain_;
        sa.load = LoadOp::Load;
        si.colour.push_back(sa);
        GLuint fbo = framebuffer_for(si);
        glNamedFramebufferReadBuffer(fbo, GL_COLOR_ATTACHMENT0);
        // The source is sRGB-encoded already; a conversion here would
        // encode it twice and wash the picture out.
        glDisable(GL_FRAMEBUFFER_SRGB);
        glBlitNamedFramebuffer(fbo, 0, 0, 0, GLint(width_), GLint(height_), 0,
                               GLint(height_), GLint(width_), 0,
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glEnable(GL_FRAMEBUFFER_SRGB);
        (void)sc;
    }
    capture_requested_ = false;
    SDL_GL_SwapWindow(window_);
}

bool GlDevice::make_swapchain_texture() {
    if (swapchain_.valid()) {
        if (GlTexture *old = textures.get(swapchain_)) {
            if (old->id) glDeleteTextures(1, &old->id);
            old->is_swapchain = false;
        }
        textures.destroy(swapchain_);
        // The cached framebuffers referred to the old texture.
        for (auto &kv : fbo_cache_) glDeleteFramebuffers(1, &kv.second);
        fbo_cache_.clear();
    }
    TextureDesc d;
    d.width = width_ ? width_ : 1;
    d.height = height_ ? height_ : 1;
    // sRGB, so the hardware encodes on write exactly as a Vulkan sRGB
    // swapchain does. The blit at present is then a raw byte copy.
    d.format = swapchain_format_;
    d.usage = TextureUsage::ColourTarget | TextureUsage::Sampled |
              TextureUsage::TransferSrc;
    d.name = "swapchain";
    swapchain_ = create_texture(d, nullptr);
    if (GlTexture *t = textures.get(swapchain_)) t->is_swapchain = true;
    return swapchain_.valid();
}

void GlDevice::resize_swapchain(uint32_t w, uint32_t h) {
    if (w == width_ && h == height_) return;
    width_ = w;
    height_ = h;
    make_swapchain_texture();
}

void GlDevice::set_vsync(bool on) {
    if (on && SDL_GL_SetSwapInterval(-1) == 0) return;
    SDL_GL_SetSwapInterval(on ? 1 : 0);
}

std::string GlDevice::resource_report() const {
    char b[512];
    std::snprintf(b, sizeof(b),
                  "OpenGL: %zu buffers, %zu textures, %zu samplers, %zu shaders, "
                  "%zu pipelines, %zu bind groups, %zu cached framebuffers",
                  buffers.live_count(), textures.live_count(), samplers.live_count(),
                  shaders.live_count(), pipelines.live_count(), groups.live_count(),
                  fbo_cache_.size());
    return b;
}

// --------------------------------------------------- framebuffer caching

GLuint GlDevice::framebuffer_for(const RenderingInfo &info) {
    // No special case for the swapchain: it is an ordinary texture in
    // this backend, and framebuffer zero is only ever the destination
    // of the present blit in end_frame.
    std::vector<uint64_t> key;
    key.reserve(info.colour.size() * 3 + 3);
    for (const ColourAttachment &c : info.colour) {
        key.push_back(c.texture.index);
        key.push_back(c.texture.generation);
        key.push_back(uint64_t(c.mip) | (uint64_t(uint32_t(c.layer)) << 32));
    }
    key.push_back(0xD0D0);
    if (info.has_depth) {
        key.push_back(info.depth.texture.index);
        key.push_back(info.depth.texture.generation);
        key.push_back(uint64_t(info.depth.mip) |
                      (uint64_t(uint32_t(info.depth.layer)) << 32));
    }
    auto it = fbo_cache_.find(key);
    if (it != fbo_cache_.end()) return it->second;

    GLuint fbo = 0;
    glCreateFramebuffers(1, &fbo);
    GLenum bufs[8];
    int nbufs = 0;
    for (size_t i = 0; i < info.colour.size() && i < 8; i++) {
        const GlTexture *t = textures.get(info.colour[i].texture);
        if (!t) continue;
        if (info.colour[i].layer >= 0)
            glNamedFramebufferTextureLayer(fbo, GL_COLOR_ATTACHMENT0 + GLenum(i),
                                           t->id, GLint(info.colour[i].mip),
                                           info.colour[i].layer);
        else
            glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + GLenum(i), t->id,
                                      GLint(info.colour[i].mip));
        bufs[nbufs++] = GL_COLOR_ATTACHMENT0 + GLenum(i);
    }
    if (nbufs)
        glNamedFramebufferDrawBuffers(fbo, nbufs, bufs);
    else
        glNamedFramebufferDrawBuffer(fbo, GL_NONE);

    if (info.has_depth) {
        const GlTexture *t = textures.get(info.depth.texture);
        if (t) {
            GLenum slot = format_has_stencil(t->desc.format)
                              ? GL_DEPTH_STENCIL_ATTACHMENT
                              : GL_DEPTH_ATTACHMENT;
            if (info.depth.layer >= 0)
                glNamedFramebufferTextureLayer(fbo, slot, t->id,
                                               GLint(info.depth.mip),
                                               info.depth.layer);
            else
                glNamedFramebufferTexture(fbo, slot, t->id, GLint(info.depth.mip));
        }
    }
    GLenum status = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        WR_ERROR("gl: framebuffer for pass '%s' is incomplete (0x%x)",
                 info.name ? info.name : "?", status);
    fbo_cache_[key] = fbo;
    return fbo;
}

// ------------------------------------------------------ pipeline state

void GlDevice::apply_pipeline_state(const PipelineDesc &d, uint32_t stencil_ref) {
    const bool all = !applied_valid_;
    const PipelineDesc &o = applied_;

    if (all || d.raster.cull != o.raster.cull) {
        if (d.raster.cull == CullMode::None) glDisable(GL_CULL_FACE);
        else {
            glEnable(GL_CULL_FACE);
            glCullFace(d.raster.cull == CullMode::Back ? GL_BACK : GL_FRONT);
        }
    }
    // NOT flipped for GL_UPPER_LEFT. ARB_clip_control negates the
    // sign of the computed polygon area when the origin is upper-left,
    // so facing comes out the same as it always did -- unlike Vulkan,
    // where the negative viewport height is the engine's own doing and
    // has to be reasoned about. Compensating here as well would cull
    // every front face and draw every back one.
    if (all || d.raster.front_face != o.raster.front_face)
        glFrontFace(d.raster.front_face == FrontFace::CounterClockwise ? GL_CCW : GL_CW);
    if (all || d.raster.polygon != o.raster.polygon)
        glPolygonMode(GL_FRONT_AND_BACK,
                      d.raster.polygon == PolygonMode::Fill   ? GL_FILL
                      : d.raster.polygon == PolygonMode::Line ? GL_LINE
                                                              : GL_POINT);
    if (all || d.raster.line_width != o.raster.line_width)
        glLineWidth(d.raster.line_width);
    if (all || d.raster.depth_clamp != o.raster.depth_clamp)
        d.raster.depth_clamp ? glEnable(GL_DEPTH_CLAMP) : glDisable(GL_DEPTH_CLAMP);

    const DepthStencilState &ds = d.depth_stencil;
    const DepthStencilState &os = o.depth_stencil;
    if (all || ds.depth_test != os.depth_test)
        ds.depth_test ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    if (all || ds.depth_write != os.depth_write)
        glDepthMask(ds.depth_write ? GL_TRUE : GL_FALSE);
    if (all || ds.depth_compare != os.depth_compare)
        glDepthFunc(gl_compare(ds.depth_compare));
    if (all || ds.depth_bias_enable != os.depth_bias_enable ||
        ds.depth_bias_constant != os.depth_bias_constant ||
        ds.depth_bias_slope != os.depth_bias_slope) {
        if (ds.depth_bias_enable) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(ds.depth_bias_slope, ds.depth_bias_constant);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

    // STENCIL. The reference value is dynamic, so it is compared
    // separately from the rest and a portal changing depth costs one
    // glStencilFunc rather than a pipeline rebind.
    bool face_changed = all || std::memcmp(&ds.front, &os.front, sizeof(StencilFace)) != 0 ||
                        std::memcmp(&ds.back, &os.back, sizeof(StencilFace)) != 0;
    if (all || ds.stencil_test != os.stencil_test)
        ds.stencil_test ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    if (ds.stencil_test && (face_changed || stencil_ref != applied_stencil_ref_)) {
        glStencilFuncSeparate(GL_FRONT, gl_compare(ds.front.compare), GLint(stencil_ref),
                              ds.front.compare_mask);
        glStencilFuncSeparate(GL_BACK, gl_compare(ds.back.compare), GLint(stencil_ref),
                              ds.back.compare_mask);
        applied_stencil_ref_ = stencil_ref;
    }
    if (ds.stencil_test && face_changed) {
        glStencilOpSeparate(GL_FRONT, gl_stencil_op(ds.front.fail),
                            gl_stencil_op(ds.front.depth_fail),
                            gl_stencil_op(ds.front.pass));
        glStencilOpSeparate(GL_BACK, gl_stencil_op(ds.back.fail),
                            gl_stencil_op(ds.back.depth_fail),
                            gl_stencil_op(ds.back.pass));
        glStencilMaskSeparate(GL_FRONT, ds.front.write_mask);
        glStencilMaskSeparate(GL_BACK, ds.back.write_mask);
    }
    if (!ds.stencil_test && (all || os.stencil_test)) glStencilMask(0xFF);

    // Blending. Per-attachment, because a pass that writes colour and a
    // mask needs different blends on each.
    const size_t n = std::max<size_t>(1, d.blend.size());
    for (size_t i = 0; i < n && i < 8; i++) {
        BlendState b = i < d.blend.size() ? d.blend[i] : BlendState();
        BlendState ob = i < o.blend.size() ? o.blend[i] : BlendState();
        if (!all && std::memcmp(&b, &ob, sizeof(BlendState)) == 0) continue;
        if (b.enable) {
            glEnablei(GL_BLEND, GLuint(i));
            glBlendFuncSeparate(gl_blend_factor(b.src_colour),
                                gl_blend_factor(b.dst_colour),
                                gl_blend_factor(b.src_alpha),
                                gl_blend_factor(b.dst_alpha));
            glBlendEquationSeparate(gl_blend_op(b.colour_op), gl_blend_op(b.alpha_op));
        } else {
            glDisablei(GL_BLEND, GLuint(i));
        }
        glColorMaski(GLuint(i), b.write_r ? GL_TRUE : GL_FALSE,
                     b.write_g ? GL_TRUE : GL_FALSE, b.write_b ? GL_TRUE : GL_FALSE,
                     b.write_a ? GL_TRUE : GL_FALSE);
    }

    applied_ = d;
    applied_valid_ = true;
}

// ------------------------------------------------------- the command list

void GlCommandList::begin_rendering(const RenderingInfo &info) {
    GLuint fbo = dev_->framebuffer_for(info);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    dev_->stats_.render_passes++;

    uint32_t w = info.width, h = info.height;
    if (!w || !h) {
        if (!info.colour.empty()) {
            TextureDesc td = dev_->texture_desc(info.colour[0].texture);
            w = td.width; h = td.height;
        } else if (info.has_depth) {
            TextureDesc td = dev_->texture_desc(info.depth.texture);
            w = td.width; h = td.height;
        }
    }
    target_height_ = h;
    glViewport(0, 0, GLsizei(w), GLsizei(h));
    glDisable(GL_SCISSOR_TEST);

    // Clears must ignore whatever masks the last pipeline left behind,
    // which is a classic source of "the clear did nothing".
    for (size_t i = 0; i < info.colour.size(); i++) {
        if (info.colour[i].load != LoadOp::Clear) continue;
        glColorMaski(GLuint(i), GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        const Color &c = info.colour[i].clear;
        const float v[4] = {c.r, c.g, c.b, c.a};
        glClearNamedFramebufferfv(fbo, GL_COLOR, GLint(i), v);
    }
    if (info.has_depth) {
        const bool cd = info.depth.depth_load == LoadOp::Clear;
        const bool cs = info.depth.stencil_load == LoadOp::Clear;
        if (cd || cs) {
            glDepthMask(GL_TRUE);
            glStencilMask(0xFF);
            TextureDesc td = dev_->texture_desc(info.depth.texture);
            if (cd && cs && format_has_stencil(td.format))
                glClearNamedFramebufferfi(fbo, GL_DEPTH_STENCIL, 0,
                                          info.depth.clear_depth,
                                          GLint(info.depth.clear_stencil));
            else {
                if (cd) {
                    float d = info.depth.clear_depth;
                    glClearNamedFramebufferfv(fbo, GL_DEPTH, 0, &d);
                }
                if (cs) {
                    GLint s = GLint(info.depth.clear_stencil);
                    glClearNamedFramebufferiv(fbo, GL_STENCIL, 0, &s);
                }
            }
        }
    }
    // The state cache no longer knows what the masks are.
    dev_->applied_valid_ = false;
    current_pass_ = info;
    pass_open_ = true;
}

// RESOLVE. A multisample colour target cannot be sampled by an
// ordinary sampler2D, so a pass that asked for one has to be blitted
// down before anything reads it. Vulkan does this inside the render
// pass through the attachment's resolve mode and OpenGL has no such
// thing, so it happens here -- and forgetting it is a black screen on
// one backend and a correct picture on the other, which is exactly the
// sort of divergence this RHI exists to prevent.
void GlCommandList::end_rendering() {
    if (!pass_open_) return;
    pass_open_ = false;
    for (size_t i = 0; i < current_pass_.colour.size(); i++) {
        const ColourAttachment &c = current_pass_.colour[i];
        if (!c.resolve.valid()) continue;
        const GlTexture *src = dev_->textures.get(c.texture);
        const GlTexture *dst = dev_->textures.get(c.resolve);
        if (!src || !dst) continue;

        RenderingInfo si;
        ColourAttachment sa;
        sa.texture = c.texture;
        sa.load = LoadOp::Load;
        si.colour.push_back(sa);
        GLuint src_fbo = dev_->framebuffer_for(si);

        RenderingInfo di;
        ColourAttachment da;
        da.texture = c.resolve;
        da.load = LoadOp::Load;
        di.colour.push_back(da);
        GLuint dst_fbo = dev_->framebuffer_for(di);

        glNamedFramebufferReadBuffer(src_fbo, GL_COLOR_ATTACHMENT0);
        GLenum draw0 = GL_COLOR_ATTACHMENT0;
        glNamedFramebufferDrawBuffers(dst_fbo, 1, &draw0);
        // NEAREST, not LINEAR: source and destination are the same
        // size, so a linear filter would only blur the resolve.
        glBlitNamedFramebuffer(src_fbo, dst_fbo, 0, 0, GLint(src->desc.width),
                               GLint(src->desc.height), 0, 0,
                               GLint(dst->desc.width), GLint(dst->desc.height),
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
}

// glViewport and glScissor take y from the BOTTOM of the framebuffer,
// which glClipControl does not change. The engine's rectangles are
// y-down from the top, so both are turned over here.
void GlCommandList::set_viewport(const Viewport &vp) {
    // No y flip -- for the reason spelled out on set_scissor below.
    // glClipControl(GL_UPPER_LEFT) has already moved the window
    // origin to the top, so a rectangle stated from the top-left is
    // already in the right coordinates.
    glViewport(GLint(vp.x), GLint(vp.y), GLsizei(vp.width), GLsizei(vp.height));
    glDepthRange(double(vp.min_depth), double(vp.max_depth));
}

void GlCommandList::set_scissor(const Rect &r) {
    glEnable(GL_SCISSOR_TEST);
    // NO Y FLIP, AND THAT IS BECAUSE OF glClipControl.
    //
    // The instinct is to flip: OpenGL measures window coordinates from
    // the bottom and the RHI states its rectangles from the top. But
    // this backend runs with glClipControl(GL_UPPER_LEFT), which moves
    // the origin of the window coordinate system itself to the upper
    // left -- so a rectangle already given from the top-left needs no
    // conversion, and flipping it puts the scissor at (height - y -
    // h), which is the reflection of where it belongs.
    //
    // Nothing catches this until a scissor is ASYMMETRIC IN Y. A
    // full-screen scissor is its own reflection, and so is a viewport
    // covering the whole target, so the bug sat behind every test the
    // engine had until one scissored a portal to part of the screen
    // -- and then sliced its contents off. tests/test_backend_parity
    // now scissors an off-centre rectangle and reads back where the
    // pixels landed.
    glScissor(r.x, r.y, GLsizei(r.width), GLsizei(r.height));
}

void GlCommandList::bind_pipeline(PipelineH h) {
    GlPipeline *p = dev_->pipelines.get_checked(h, "pipeline");
    if (!p) return;
    bound_pipeline_ = h;
    glUseProgram(p->program);
    if (p->vao && p->vao != bound_vao_) {
        glBindVertexArray(p->vao);
        bound_vao_ = p->vao;
    }
    if (!p->compute) dev_->apply_pipeline_state(p->desc, stencil_ref_);
    dev_->stats_.pipeline_binds++;
}

void GlCommandList::bind_group(uint32_t set, BindGroupH h, const uint32_t *offsets,
                               uint32_t count) {
    (void)set;
    GlBindGroup *g = dev_->groups.get_checked(h, "bind group");
    if (!g) return;
    const GlBindGroupLayout *l = dev_->layouts.get(g->desc.layout);
    uint32_t dyn = 0;
    for (const BindGroupEntry &e : g->desc.entries) {
        BindingType type = BindingType::UniformBuffer;
        if (l) {
            for (const BindGroupLayoutEntry &le : l->desc.entries)
                if (le.binding == e.binding) { type = le.type; break; }
        }
        switch (type) {
            case BindingType::UniformBuffer:
            case BindingType::UniformBufferDynamic: {
                const GlBuffer *b = dev_->buffers.get(e.buffer);
                if (!b) break;
                uint64_t off = e.offset;
                if (type == BindingType::UniformBufferDynamic && offsets && dyn < count)
                    off += offsets[dyn++];
                uint64_t range = e.range ? e.range : (b->size - off);
                glBindBufferRange(GL_UNIFORM_BUFFER, e.binding, b->id, GLintptr(off),
                                  GLsizeiptr(range));
                break;
            }
            case BindingType::StorageBuffer: {
                const GlBuffer *b = dev_->buffers.get(e.buffer);
                if (!b) break;
                uint64_t range = e.range ? e.range : (b->size - e.offset);
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER, e.binding, b->id,
                                  GLintptr(e.offset), GLsizeiptr(range));
                break;
            }
            case BindingType::SampledTexture: {
                const GlTexture *t = dev_->textures.get(e.texture);
                const GlSampler *s = dev_->samplers.get(e.sampler);
                glBindTextureUnit(e.binding, t ? t->id : 0);
                glBindSampler(e.binding, s ? s->id : 0);
                break;
            }
            case BindingType::StorageTexture: {
                const GlTexture *t = dev_->textures.get(e.texture);
                if (!t) break;
                GlFormat gf = gl_format(t->desc.format);
                glBindImageTexture(e.binding, t->id, GLint(e.mip),
                                   e.layer < 0 ? GL_TRUE : GL_FALSE,
                                   e.layer < 0 ? 0 : e.layer, GL_READ_WRITE,
                                   gf.internal);
                break;
            }
        }
    }
    dev_->stats_.bind_group_binds++;
}

void GlCommandList::push_constants(const void *data, uint32_t size, uint32_t offset) {
    if (!data || !size) return;
    if (offset + size > 128) {
        WR_ERROR("gl: push constants exceed the 128-byte budget");
        return;
    }
    glNamedBufferSubData(dev_->push_buffer(), GLintptr(offset), GLsizeiptr(size), data);
    glBindBufferBase(GL_UNIFORM_BUFFER, GlDevice::kPushBinding, dev_->push_buffer());
}

void GlCommandList::set_stencil_reference(uint32_t ref) {
    if (ref == stencil_ref_) return;
    stencil_ref_ = ref;
    const GlPipeline *p = dev_->pipelines.get(bound_pipeline_);
    if (p && p->desc.depth_stencil.stencil_test) {
        const DepthStencilState &ds = p->desc.depth_stencil;
        glStencilFuncSeparate(GL_FRONT, gl_compare(ds.front.compare), GLint(ref),
                              ds.front.compare_mask);
        glStencilFuncSeparate(GL_BACK, gl_compare(ds.back.compare), GLint(ref),
                              ds.back.compare_mask);
        dev_->applied_stencil_ref_ = ref;
    }
}

void GlCommandList::set_blend_constant(const Color &c) {
    glBlendColor(c.r, c.g, c.b, c.a);
}

void GlCommandList::bind_vertex_buffer(uint32_t slot, BufferH h, uint64_t offset) {
    const GlBuffer *b = dev_->buffers.get(h);
    const GlPipeline *p = dev_->pipelines.get(bound_pipeline_);
    if (!b || !p) return;
    uint32_t stride = 0;
    for (const VertexBinding &vb : p->desc.vertex_layout.bindings)
        if (vb.binding == slot) stride = vb.stride;
    glVertexArrayVertexBuffer(p->vao, slot, b->id, GLintptr(offset), GLsizei(stride));
}

void GlCommandList::bind_index_buffer(BufferH h, IndexType type, uint64_t offset) {
    const GlBuffer *b = dev_->buffers.get(h);
    const GlPipeline *p = dev_->pipelines.get(bound_pipeline_);
    if (!b || !p) return;
    glVertexArrayElementBuffer(p->vao, b->id);
    index_type_ = type;
    index_offset_ = offset;
}

void GlCommandList::draw(uint32_t vertices, uint32_t instances, uint32_t first,
                         uint32_t first_instance) {
    const GlPipeline *p = dev_->pipelines.get(bound_pipeline_);
    if (!p) return;
    (void)first_instance;
    GLenum prim = gl_topology(p->desc.topology);
    if (instances > 1)
        glDrawArraysInstanced(prim, GLint(first), GLsizei(vertices), GLsizei(instances));
    else
        glDrawArrays(prim, GLint(first), GLsizei(vertices));
    dev_->stats_.draw_calls++;
    if (p->desc.topology == Topology::TriangleList)
        dev_->stats_.triangles += uint64_t(vertices / 3) * instances;
}

void GlCommandList::draw_indexed(uint32_t indices, uint32_t instances,
                                 uint32_t first_index, int32_t vertex_offset,
                                 uint32_t first_instance) {
    const GlPipeline *p = dev_->pipelines.get(bound_pipeline_);
    if (!p) return;
    (void)first_instance;
    GLenum prim = gl_topology(p->desc.topology);
    GLenum itype = index_type_ == IndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    size_t isize = index_type_ == IndexType::U16 ? 2 : 4;
    const void *off = (const void *)(uintptr_t(index_offset_ + first_index * isize));
    if (instances > 1)
        glDrawElementsInstanced(prim, GLsizei(indices), itype, off, GLsizei(instances));
    else if (vertex_offset)
        glDrawElementsBaseVertex(prim, GLsizei(indices), itype, (void *)off,
                                 vertex_offset);
    else
        glDrawElements(prim, GLsizei(indices), itype, off);
    dev_->stats_.draw_calls++;
    if (p->desc.topology == Topology::TriangleList)
        dev_->stats_.triangles += uint64_t(indices / 3) * instances;
}

void GlCommandList::draw_indexed_indirect(BufferH args, uint64_t offset,
                                          uint32_t draw_count, uint32_t stride) {
    const GlBuffer *b = dev_->buffers.get(args);
    const GlPipeline *p = dev_->pipelines.get(bound_pipeline_);
    if (!b || !p) return;
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, b->id);
    GLenum itype = index_type_ == IndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    glMultiDrawElementsIndirect(gl_topology(p->desc.topology), itype,
                                (const void *)uintptr_t(offset), GLsizei(draw_count),
                                GLsizei(stride));
    dev_->stats_.draw_calls += draw_count;
}

void GlCommandList::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    glDispatchCompute(x, y, z);
    dev_->stats_.dispatches++;
}

void GlCommandList::copy_buffer(BufferH src, uint64_t so, BufferH dst, uint64_t dof,
                                uint64_t size) {
    const GlBuffer *s = dev_->buffers.get(src);
    const GlBuffer *d = dev_->buffers.get(dst);
    if (!s || !d || !size) return;
    glCopyNamedBufferSubData(s->id, d->id, GLintptr(so), GLintptr(dof),
                             GLsizeiptr(size));
}

void GlCommandList::copy_buffer_to_texture(BufferH src, uint64_t so, TextureH dst,
                                           uint32_t mip, uint32_t layer, uint32_t w,
                                           uint32_t h) {
    const GlBuffer *s = dev_->buffers.get(src);
    const GlTexture *t = dev_->textures.get(dst);
    if (!s || !t) return;
    // A pixel unpack buffer makes the texture upload read from GPU
    // memory, which is the same thing Vulkan's staging copy does.
    GlFormat gf = gl_format(t->desc.format);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, s->id);
    if (t->desc.dim == TextureDim::Tex2DArray || t->desc.dim == TextureDim::Tex3D)
        glTextureSubImage3D(t->id, GLint(mip), 0, 0, GLint(layer), GLsizei(w),
                            GLsizei(h), 1, gf.format, gf.type,
                            (const void *)uintptr_t(so));
    else
        glTextureSubImage2D(t->id, GLint(mip), 0, 0, GLsizei(w), GLsizei(h),
                            gf.format, gf.type, (const void *)uintptr_t(so));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void GlCommandList::generate_mips(TextureH h) {
    if (const GlTexture *t = dev_->textures.get(h)) glGenerateTextureMipmap(t->id);
}

void GlCommandList::texture_barrier(TextureH t, TextureUsage from, TextureUsage to) {
    (void)t;
    // OpenGL orders most of this itself. The exception is an image
    // written by compute and then sampled, which needs a real barrier.
    if ((from & TextureUsage::Storage) || (to & TextureUsage::Storage))
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    else if ((from & TextureUsage::ColourTarget) || (from & TextureUsage::DepthTarget))
        glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

void GlCommandList::buffer_barrier(BufferH b) {
    (void)b;
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_UNIFORM_BARRIER_BIT |
                    GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
}

// These are what make a RenderDoc capture of a recursive portal
// readable: without them a frame is four hundred draws in a flat list
// and there is no way to tell which recursion level any of them is.
void GlCommandList::push_debug_group(const char *name, const Color &c) {
    (void)c;
    debug_depth_++;
    if (glPushDebugGroup && name)
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, GLuint(debug_depth_), -1, name);
}

void GlCommandList::pop_debug_group() {
    if (debug_depth_ <= 0) return;
    debug_depth_--;
    if (glPopDebugGroup) glPopDebugGroup();
}

void GlCommandList::insert_debug_marker(const char *name) {
    if (glDebugMessageInsert && name)
        glDebugMessageInsert(GL_DEBUG_SOURCE_APPLICATION, GL_DEBUG_TYPE_MARKER, 0,
                             GL_DEBUG_SEVERITY_NOTIFICATION, -1, name);
}

}  // namespace

// Defined out of the anonymous namespace so rhi.cpp can find it.
Device *create_gl_device(const DeviceDesc &d) {
    GlDevice *dev = new GlDevice();
    if (!dev->init(d)) {
        delete dev;
        return nullptr;
    }
    return dev;
}

}  // namespace wr::rhi
