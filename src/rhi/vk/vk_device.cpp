// Warren -- the Vulkan 1.3 backend.
//
// Written to the modern core, not to 1.0 with a decade of extensions
// bolted on: dynamic rendering instead of render pass and framebuffer
// objects, synchronization2 instead of the old barrier zoo. That keeps
// this file close in shape to the OpenGL one, which is what makes the
// two comparable -- and the portal tests do compare them, pixel for
// pixel.
//
// THE Y AXIS. Vulkan's clip space has +Y pointing down. Rather than
// flip it in every shader or bake a flip into every projection, the
// viewport is given a NEGATIVE HEIGHT, which inverts the mapping once,
// in one place, and leaves the engine with one clip space.
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "core/log.h"
#include "rhi/handle_pool.h"
#include "rhi/rhi.h"
#include "rhi/vk/vk_alloc.h"
#include "rhi/vk/vkfn.h"

namespace wr::rhi {
namespace {

using wr::vk::Allocation;
using wr::vk::Allocator;
using wr::vk::MemoryUsage;

#define VK_CHECK(expr, what)                                         \
    do {                                                             \
        VkResult r_ = (expr);                                        \
        if (r_ != VK_SUCCESS) {                                      \
            WR_ERROR("vk: %s failed (%d)", what, int(r_));           \
        }                                                            \
    } while (0)

#define VK_TRY(expr, what)                                           \
    do {                                                             \
        VkResult r_ = (expr);                                        \
        if (r_ != VK_SUCCESS) {                                      \
            WR_FATAL("vk: %s failed (%d)", what, int(r_));           \
            return false;                                            \
        }                                                            \
    } while (0)

// ------------------------------------------------------------ conversion

VkFormat vk_format(Format f) {
    switch (f) {
        case Format::R8: return VK_FORMAT_R8_UNORM;
        case Format::RG8: return VK_FORMAT_R8G8_UNORM;
        case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::R16F: return VK_FORMAT_R16_SFLOAT;
        case Format::RG16F: return VK_FORMAT_R16G16_SFLOAT;
        case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::R32F: return VK_FORMAT_R32_SFLOAT;
        case Format::RG32F: return VK_FORMAT_R32G32_SFLOAT;
        case Format::RGB32F: return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case Format::RG11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case Format::R8UI: return VK_FORMAT_R8_UINT;
        case Format::R16UI: return VK_FORMAT_R16_UINT;
        case Format::R32UI: return VK_FORMAT_R32_UINT;
        case Format::RGBA8UI: return VK_FORMAT_R8G8B8A8_UINT;
        case Format::D32F_S8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case Format::D32F: return VK_FORMAT_D32_SFLOAT;
        case Format::D24_S8: return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Format::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Format::BC5: return VK_FORMAT_BC5_UNORM_BLOCK;
        case Format::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
        case Format::BC7_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
        default: return VK_FORMAT_UNDEFINED;
    }
}

Format from_vk_format(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_SRGB: return Format::BGRA8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8;
        case VK_FORMAT_R8G8B8A8_SRGB: return Format::RGBA8_SRGB;
        case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return Format::RGB10A2;
        default: return Format::RGBA8;
    }
}

VkCompareOp vk_compare(CompareOp c) {
    switch (c) {
        case CompareOp::Never: return VK_COMPARE_OP_NEVER;
        case CompareOp::Less: return VK_COMPARE_OP_LESS;
        case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkStencilOp vk_stencil_op(StencilOp o) {
    switch (o) {
        case StencilOp::Keep: return VK_STENCIL_OP_KEEP;
        case StencilOp::Zero: return VK_STENCIL_OP_ZERO;
        case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
        case StencilOp::IncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case StencilOp::DecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case StencilOp::Invert: return VK_STENCIL_OP_INVERT;
        case StencilOp::IncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case StencilOp::DecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkBlendFactor vk_blend_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcColour: return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::OneMinusSrcColour: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColour: return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::OneMinusDstColour: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::ConstantColour: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case BlendFactor::OneMinusConstantColour:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp vk_blend_op(BlendOp o) {
    switch (o) {
        case BlendOp::Add: return VK_BLEND_OP_ADD;
        case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::Min: return VK_BLEND_OP_MIN;
        case BlendOp::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkPrimitiveTopology vk_topology(Topology t) {
    switch (t) {
        case Topology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case Topology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case Topology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case Topology::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case Topology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkDescriptorType vk_descriptor_type(BindingType t) {
    switch (t) {
        case BindingType::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case BindingType::UniformBufferDynamic:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        case BindingType::StorageBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case BindingType::SampledTexture:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case BindingType::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

VkShaderStageFlags vk_stages(const BindGroupLayoutEntry &e) {
    VkShaderStageFlags f = 0;
    if (e.vertex) f |= VK_SHADER_STAGE_VERTEX_BIT;
    if (e.fragment) f |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (e.compute) f |= VK_SHADER_STAGE_COMPUTE_BIT;
    return f ? f : VkShaderStageFlags(VK_SHADER_STAGE_ALL);
}

VkImageAspectFlags aspect_of(Format f) {
    if (!format_is_depth(f)) return VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageAspectFlags a = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (format_has_stencil(f)) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a;
}

// ------------------------------------------------------------- resources

struct VkBufferRes {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    uint64_t size = 0;
    BufferUsage usage = BufferUsage::None;
    MemoryAccess access = MemoryAccess::GpuOnly;
    std::string name;
};

struct VkTextureRes {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // ATTACHMENT VIEWS FOR ONE MIP AND ONE LAYER.
    //
    // `view` covers the whole image, which is what a shader samples
    // and what a single-layer attachment happens to need too. An
    // attachment naming a specific layer -- a shadow cascade in an
    // array, a cube face -- cannot use it: a render pass writes
    // through a view, so the view has to be the slice. Made on first
    // use and kept, because the set of slices an engine renders to is
    // small and fixed.
    std::unordered_map<uint64_t, VkImageView> slices;
    Allocation memory;
    TextureDesc desc;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool owns_image = true;     // false for swapchain images
    bool is_swapchain = false;
};

struct VkSamplerRes {
    VkSampler sampler = VK_NULL_HANDLE;
};

struct VkShaderRes {
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry = "main";
    std::string name;
};

struct VkLayoutRes {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    BindGroupLayoutDesc desc;
};

struct VkGroupRes {
    VkDescriptorSet set = VK_NULL_HANDLE;
    BindGroupDesc desc;
};

struct VkPipelineRes {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    PipelineDesc desc;
    bool compute = false;
};

class VkDeviceImpl;

// ------------------------------------------------------- the command list

class VkCommandListImpl final : public CommandList {
public:
    explicit VkCommandListImpl(VkDeviceImpl *d) : dev_(d) {}

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
    void draw(uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) override;
    void draw_indexed(uint32_t idx, uint32_t inst, uint32_t first,
                      int32_t vofs, uint32_t finst) override;
    void draw_indexed_indirect(BufferH args, uint64_t offset, uint32_t count,
                               uint32_t stride) override;
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override;
    void copy_buffer(BufferH s, uint64_t so, BufferH d, uint64_t dof,
                     uint64_t size) override;
    void copy_buffer_to_texture(BufferH s, uint64_t so, TextureH d, uint32_t mip,
                                uint32_t layer, uint32_t w, uint32_t h) override;
    void generate_mips(TextureH t) override;
    void texture_barrier(TextureH t, TextureUsage from, TextureUsage to) override;
    void buffer_barrier(BufferH b) override;
    void push_debug_group(const char *name, const Color &c) override;
    void pop_debug_group() override;
    void insert_debug_marker(const char *name) override;

    VkCommandBuffer cb = VK_NULL_HANDLE;

private:
    VkDeviceImpl *dev_;
    PipelineH bound_;
    VkPipelineLayout bound_layout_ = VK_NULL_HANDLE;
    bool rendering_ = false;
    friend class VkDeviceImpl;
};

// ------------------------------------------------------------- the device

class VkDeviceImpl final : public Device {
public:
    ~VkDeviceImpl() override;
    bool init(const DeviceDesc &d);

    const DeviceCaps &caps() const override { return caps_; }
    Backend backend() const override { return Backend::Vulkan; }

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

    void request_capture() override {}
    CommandList *begin_frame() override;
    void end_frame() override;
    TextureH swapchain_texture() const override { return swapchain_handle_; }
    Format swapchain_format() const override { return swapchain_format_; }
    uint32_t swapchain_width() const override { return extent_.width; }
    uint32_t swapchain_height() const override { return extent_.height; }
    void resize_swapchain(uint32_t w, uint32_t h) override;
    void set_vsync(bool on) override;
    void wait_idle() override { if (device_) vkDeviceWaitIdle(device_); }
    const FrameStats &stats() const override { return stats_; }
    std::string resource_report() const override;
    size_t pending_deletions() const override;
    uint32_t frame_slot() const override { return frame_index_; }
    uint32_t frames_in_flight() const override { return frames_in_flight_; }

    // --- used by the command list ----------------------------------------
    HandlePool<VkBufferRes, BufferH> buffers;
    HandlePool<VkTextureRes, TextureH> textures;
    HandlePool<VkSamplerRes, SamplerH> samplers;
    HandlePool<VkShaderRes, ShaderH> shaders;
    HandlePool<VkLayoutRes, BindGroupLayoutH> layouts;
    HandlePool<VkGroupRes, BindGroupH> groups;
    HandlePool<VkPipelineRes, PipelineH> pipelines;
    FrameStats stats_;
    VkDevice device_ = VK_NULL_HANDLE;
    bool debug_labels_ = false;

    void transition(VkCommandBuffer cb, VkTextureRes &t, VkImageLayout to);
    void name_object(uint64_t handle, VkObjectType type, const char *name);

private:
    bool create_instance(bool validation);
    bool pick_physical_device();
    bool create_logical_device();
    bool create_swapchain(uint32_t w, uint32_t h);
    void destroy_swapchain();
    void flush_uploads(VkCommandBuffer cb);
    void record_mips(VkCommandBuffer cb, VkTextureRes &t);

    // STAGED WRITES THAT HAVE NOT BEEN RECORDED YET.
    //
    // They carry their own bytes rather than an offset into the ring,
    // because the ring is bump-allocated per frame and reset at
    // begin_frame: anything written into it before the copy is
    // RECORDED can be overwritten by a later write in the same frame,
    // and the copy would then read whatever landed there instead.
    //
    // That is not a theoretical hazard. It is what happens to every
    // resource uploaded before the first frame: the copy is recorded
    // at the top of frame 0, the frame then loads the rest of the
    // scene through the same ring, and the first resource silently
    // receives the last one's bytes. OpenGL never sees it, because it
    // has no staging.
    struct Pending {
        std::vector<uint8_t> bytes;
        BufferH dst_buffer;
        TextureH dst_texture;
        uint64_t dst_offset = 0;
        uint32_t mip = 0, layer = 0, width = 0, height = 0;
    };

    // Reserve `size` bytes of this frame's staging ring, aligned, and
    // return where they went. -1 when the ring is full.
    int64_t stage(const void *data, uint64_t size, uint64_t alignment);
    // Record one staged copy straight into the open command buffer.
    void record_copy(VkCommandBuffer cb, uint64_t src_offset, const Pending &p);
    std::vector<Pending> pending_uploads_;
    // Textures whose chain has to be filled in once their level
    // zero has actually been copied, which on this backend is not
    // until a command buffer is open.
    std::vector<TextureH> pending_mips_;
    bool inside_render_pass_ = false;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkSemaphore rendered = VK_NULL_HANDLE;
        // A ring of host-visible memory for this frame's uploads.
        BufferH staging;
        uint64_t staging_used = 0;
        std::vector<std::function<void()>> deletions;
    };

    // The frame a deferred deletion should ride on; see the comment
    // on the definition, which is where the bug was.
    Frame &retiring_frame();

    SDL_Window *window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    Allocator alloc_;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{0, 0};
    Format swapchain_format_ = Format::BGRA8_SRGB;
    VkFormat swapchain_vk_format_ = VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<TextureH> swapchain_images_;
    TextureH swapchain_handle_;      // aliases the current image
    uint32_t image_index_ = 0;

    std::vector<TextureH> pending_initial_layout_;
    std::vector<Frame> frames_;
    uint32_t frame_index_ = 0;
    uint32_t frames_in_flight_ = 2;
    bool vsync_ = true;
    bool frame_open_ = false;
    VkCommandListImpl *cmd_ = nullptr;
    DeviceCaps caps_;
    // The view a render pass writes through for one mip and one
    // layer; the whole-image view where that is the same thing.
    VkImageView attachment_view(VkTextureRes &t, int32_t layer, uint32_t mip);
    friend class VkCommandListImpl;
};

// ------------------------------------------------------------ validation

VKAPI_ATTR VkBool32 VKAPI_CALL
debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
               VkDebugUtilsMessageTypeFlagsEXT types,
               const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types;
    (void)user;
    if (!data || !data->pMessage) return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        WR_ERROR("vk validation: %s", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        WR_WARN("vk validation: %s", data->pMessage);
    else
        WR_DEBUG("vk: %s", data->pMessage);
    return VK_FALSE;
}


// ==================================================== instance and device

bool VkDeviceImpl::create_instance(bool validation) {
    if (!wr::vk::load_global()) {
        WR_WARN("vk: no Vulkan library on this system");
        return false;
    }

    uint32_t api = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_3) {
        WR_WARN("vk: loader reports %u.%u; this backend needs 1.3",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
        return false;
    }

    unsigned ext_count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window_, &ext_count, nullptr)) {
        WR_WARN("vk: SDL cannot list the surface extensions: %s", SDL_GetError());
        return false;
    }
    std::vector<const char *> extensions(ext_count);
    SDL_Vulkan_GetInstanceExtensions(window_, &ext_count, extensions.data());

    std::vector<const char *> layers;
    if (validation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> have(n);
        vkEnumerateInstanceLayerProperties(&n, have.data());
        for (const auto &l : have)
            if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                debug_labels_ = true;
                break;
            }
        if (layers.empty())
            WR_WARN("vk: validation asked for but the layer is not installed");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Warren";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Warren";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = uint32_t(layers.size());
    ci.ppEnabledLayerNames = layers.data();

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        WR_WARN("vk: vkCreateInstance failed (%d)", int(r));
        return false;
    }
    wr::vk::load_instance(instance_);

    if (debug_labels_ && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT di{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug_callback;
        vkCreateDebugUtilsMessengerEXT(instance_, &di, nullptr, &messenger_);
    }
    return true;
}

bool VkDeviceImpl::pick_physical_device() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    if (!n) {
        WR_WARN("vk: no physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance_, &n, devices.data());

    int best_score = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;

        // It must have a queue that can both render and present.
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        int family = -1;
        for (uint32_t i = 0; i < qn; i++) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if (present) { family = int(i); break; }
        }
        if (family < 0) continue;

        // And the depth-stencil format the engine is built around. An
        // adapter without D32_SFLOAT_S8_UINT cannot run reverse-Z with
        // portals, which is the whole engine, so it is not a candidate.
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures &
              VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            WR_WARN("vk: %s has no D32_SFLOAT_S8_UINT; skipping", props.deviceName);
            continue;
        }

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 500;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) score += 1;
        score += int(props.limits.maxImageDimension2D / 1024);
        if (score > best_score) {
            best_score = score;
            physical_ = pd;
            queue_family_ = uint32_t(family);
        }
    }
    if (!physical_) {
        WR_WARN("vk: no adapter meets the requirements (1.3, present, D32F_S8)");
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    caps_.backend = Backend::Vulkan;
    caps_.device_name = props.deviceName;
    caps_.discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    char ver[96];
    std::snprintf(ver, sizeof(ver), "Vulkan %u.%u.%u",
                  VK_API_VERSION_MAJOR(props.apiVersion),
                  VK_API_VERSION_MINOR(props.apiVersion),
                  VK_API_VERSION_PATCH(props.apiVersion));
    caps_.api_version = ver;
    std::snprintf(ver, sizeof(ver), "driver %u.%u.%u",
                  VK_VERSION_MAJOR(props.driverVersion),
                  VK_VERSION_MINOR(props.driverVersion),
                  VK_VERSION_PATCH(props.driverVersion));
    caps_.driver_info = ver;
    caps_.max_texture_2d = props.limits.maxImageDimension2D;
    caps_.max_texture_layers = props.limits.maxImageArrayLayers;
    caps_.max_colour_attachments = props.limits.maxColorAttachments;
    caps_.max_push_constant_size = props.limits.maxPushConstantsSize;
    caps_.max_anisotropy = uint32_t(props.limits.maxSamplerAnisotropy);
    caps_.uniform_buffer_alignment =
        uint32_t(props.limits.minUniformBufferOffsetAlignment);
    caps_.storage_buffer_alignment =
        uint32_t(props.limits.minStorageBufferOffsetAlignment);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                props.limits.framebufferDepthSampleCounts;
    caps_.max_samples = counts & VK_SAMPLE_COUNT_8_BIT   ? 8
                        : counts & VK_SAMPLE_COUNT_4_BIT ? 4
                        : counts & VK_SAMPLE_COUNT_2_BIT ? 2
                                                         : 1;
    caps_.stencil_bits = 8;  // guaranteed by the format check above
    caps_.supports_compute = true;
    caps_.supports_indirect = true;
    return true;
}

bool VkDeviceImpl::create_logical_device() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = queue_family_;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;

    const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // The two 1.3 features the whole backend is built on. Asking for
    // them here means a driver that lacks them fails at start-up with a
    // clear message rather than at the first frame with a crash.
    VkPhysicalDeviceVulkan13Features f13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features f12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    // Wide support, and the engine uses all three.
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.features.fillModeNonSolid = VK_TRUE;
    f2.features.depthClamp = VK_TRUE;
    f2.features.multiDrawIndirect = VK_TRUE;
    f2.features.independentBlend = VK_TRUE;

    // Check what is actually offered before asking for it.
    VkPhysicalDeviceVulkan13Features have13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    have.pNext = &have13;
    vkGetPhysicalDeviceFeatures2(physical_, &have);
    if (!have13.dynamicRendering || !have13.synchronization2) {
        WR_WARN("vk: %s lacks dynamic rendering or synchronization2",
                caps_.device_name.c_str());
        return false;
    }
    if (!have.features.samplerAnisotropy) f2.features.samplerAnisotropy = VK_FALSE;
    if (!have.features.fillModeNonSolid) f2.features.fillModeNonSolid = VK_FALSE;
    if (!have.features.depthClamp) f2.features.depthClamp = VK_FALSE;
    if (!have.features.multiDrawIndirect) {
        f2.features.multiDrawIndirect = VK_FALSE;
        caps_.supports_indirect = false;
    }
    if (!have.features.independentBlend) f2.features.independentBlend = VK_FALSE;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = extensions;

    VK_TRY(vkCreateDevice(physical_, &ci, nullptr, &device_), "vkCreateDevice");
    wr::vk::load_device(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    alloc_.init(physical_, device_);
    caps_.vram_bytes = alloc_.device_local_bytes();

    // One pool, generously sized, grown by recreation if it ever runs
    // out. Bind groups are persistent objects in this API, so the churn
    // a per-frame pool exists to absorb does not happen.
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1024},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512},
    };
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 4096;
    dp.poolSizeCount = uint32_t(sizeof(sizes) / sizeof(sizes[0]));
    dp.pPoolSizes = sizes;
    VK_TRY(vkCreateDescriptorPool(device_, &dp, nullptr, &descriptor_pool_),
           "vkCreateDescriptorPool");

    VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(device_, &pc, nullptr, &pipeline_cache_);
    return true;
}

// ---------------------------------------------------------- the swapchain

bool VkDeviceImpl::create_swapchain(uint32_t w, uint32_t h) {
    VkSurfaceCapabilitiesKHR sc{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &sc);

    extent_ = sc.currentExtent;
    if (extent_.width == UINT32_MAX) {
        extent_.width = std::clamp(w, sc.minImageExtent.width, sc.maxImageExtent.width);
        extent_.height = std::clamp(h, sc.minImageExtent.height, sc.maxImageExtent.height);
    }
    if (!extent_.width || !extent_.height) return false;  // minimised

    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fn, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty()
                                    ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB,
                                                         VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                    : formats[0];
    // An sRGB swapchain means the hardware does the encode on write,
    // which is the same thing GL_FRAMEBUFFER_SRGB does on the other
    // backend -- so both end up with one linear pipeline and one
    // conversion at the very end.
    for (const auto &f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    swapchain_vk_format_ = chosen.format;
    swapchain_format_ = from_vk_format(chosen.format);

    uint32_t pn = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pn, nullptr);
    std::vector<VkPresentModeKHR> modes(pn);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pn, modes.data());
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;  // always available
    if (!vsync_) {
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) { present = m; break; }
    } else {
        // Mailbox is vsync without the latency of a full queued frame.
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) { present = m; break; }
    }

    uint32_t count = sc.minImageCount + 1;
    if (sc.maxImageCount && count > sc.maxImageCount) count = sc.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent_;
    ci.imageArrayLayers = 1;
    // TRANSFER_SRC as well, so a screenshot can copy the presented
    // image straight out of it.
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = sc.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = present;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;

    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VK_TRY(vkCreateSwapchainKHR(device_, &ci, nullptr, &fresh), "vkCreateSwapchainKHR");
    if (swapchain_) destroy_swapchain();
    swapchain_ = fresh;

    uint32_t in = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &in, nullptr);
    std::vector<VkImage> images(in);
    vkGetSwapchainImagesKHR(device_, swapchain_, &in, images.data());

    swapchain_images_.clear();
    for (VkImage img : images) {
        TextureH h = textures.create();
        VkTextureRes *t = textures.get(h);
        t->image = img;
        t->owns_image = false;
        t->is_swapchain = true;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->desc.width = extent_.width;
        t->desc.height = extent_.height;
        t->desc.format = swapchain_format_;
        t->desc.usage = TextureUsage::ColourTarget;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = chosen.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &t->view),
                 "swapchain image view");
        swapchain_images_.push_back(h);
    }
    WR_INFO("vk: swapchain %ux%u, %zu images, %s, %s", extent_.width, extent_.height,
            swapchain_images_.size(), format_name(swapchain_format_),
            present == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
            : present == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
                                                       : "fifo");
    return true;
}

void VkDeviceImpl::destroy_swapchain() {
    for (TextureH h : swapchain_images_) {
        if (VkTextureRes *t = textures.get(h)) {
            if (t->view) vkDestroyImageView(device_, t->view, nullptr);
        }
        textures.destroy(h);
    }
    swapchain_images_.clear();
    if (swapchain_) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VkDeviceImpl::resize_swapchain(uint32_t w, uint32_t h) {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    create_swapchain(w, h);
}

void VkDeviceImpl::set_vsync(bool on) {
    if (vsync_ == on) return;
    vsync_ = on;
    if (device_) {
        vkDeviceWaitIdle(device_);
        create_swapchain(extent_.width, extent_.height);
    }
}

// ------------------------------------------------------------------ init

bool VkDeviceImpl::init(const DeviceDesc &d) {
    window_ = (SDL_Window *)d.window;
    if (!window_) {
        WR_ERROR("vk: no window");
        return false;
    }
    frames_in_flight_ = std::max(1u, std::min(3u, d.frames_in_flight));
    vsync_ = d.vsync;

    if (!create_instance(d.validation)) return false;
    if (!SDL_Vulkan_CreateSurface(window_, instance_, &surface_)) {
        WR_WARN("vk: SDL could not create a surface: %s", SDL_GetError());
        return false;
    }
    if (!pick_physical_device()) return false;
    if (!create_logical_device()) return false;

    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(window_, &w, &h);
    if (!create_swapchain(uint32_t(w), uint32_t(h))) return false;

    frames_.resize(frames_in_flight_);
    for (Frame &f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = queue_family_;
        VK_TRY(vkCreateCommandPool(device_, &pi, nullptr, &f.pool), "command pool");
        VkCommandBufferAllocateInfo ai{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_TRY(vkAllocateCommandBuffers(device_, &ai, &f.cb), "command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_TRY(vkCreateFence(device_, &fi, nullptr, &f.fence), "fence");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_TRY(vkCreateSemaphore(device_, &si, nullptr, &f.acquired), "semaphore");
        VK_TRY(vkCreateSemaphore(device_, &si, nullptr, &f.rendered), "semaphore");

        BufferDesc sd;
        sd.size = 8ull * 1024 * 1024;
        sd.usage = BufferUsage::TransferSrc;
        sd.access = MemoryAccess::CpuToGpu;
        sd.name = "frame staging";
        f.staging = create_buffer(sd, nullptr);
    }

    cmd_ = new VkCommandListImpl(this);
    WR_INFO("vk: %s", alloc_.report().c_str());
    return true;
}

VkDeviceImpl::~VkDeviceImpl() {
    if (device_) vkDeviceWaitIdle(device_);
    delete cmd_;
    // Run every deferred deletion FIRST, then free the staging buffers
    // directly. Routing them through destroy() here would queue them
    // onto a frame whose deletions have already been drained, and they
    // would leak -- which the validation layer duly reported.
    for (Frame &f : frames_)
        for (auto &fn : f.deletions) fn();
    for (Frame &f : frames_) {
        f.deletions.clear();
        if (VkBufferRes *sb = buffers.get(f.staging)) {
            if (sb->buffer) vkDestroyBuffer(device_, sb->buffer, nullptr);
            if (sb->memory.valid()) alloc_.free(sb->memory);
            buffers.destroy(f.staging);
        }
        if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
        if (f.acquired) vkDestroySemaphore(device_, f.acquired, nullptr);
        if (f.rendered) vkDestroySemaphore(device_, f.rendered, nullptr);
        if (f.pool) vkDestroyCommandPool(device_, f.pool, nullptr);
    }
    frames_.clear();
    destroy_swapchain();
    pipelines.for_each([this](VkPipelineRes &p) {
        if (p.pipeline) vkDestroyPipeline(device_, p.pipeline, nullptr);
        if (p.layout) vkDestroyPipelineLayout(device_, p.layout, nullptr);
    });
    layouts.for_each([this](VkLayoutRes &l) {
        if (l.layout) vkDestroyDescriptorSetLayout(device_, l.layout, nullptr);
    });
    shaders.for_each([this](VkShaderRes &s) {
        if (s.module) vkDestroyShaderModule(device_, s.module, nullptr);
    });
    samplers.for_each([this](VkSamplerRes &s) {
        if (s.sampler) vkDestroySampler(device_, s.sampler, nullptr);
    });
    textures.for_each([this](VkTextureRes &t) {
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.owns_image && t.image) vkDestroyImage(device_, t.image, nullptr);
        if (t.memory.valid()) alloc_.free(t.memory);
    });
    buffers.for_each([this](VkBufferRes &b) {
        if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
        if (b.memory.valid()) alloc_.free(b.memory);
    });
    if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    alloc_.shutdown();
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_ && vkDestroyDebugUtilsMessengerEXT)
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VkDeviceImpl::name_object(uint64_t handle, VkObjectType type,
                               const char *name) {
    if (!debug_labels_ || !name || !vkSetDebugUtilsObjectNameEXT) return;
    VkDebugUtilsObjectNameInfoEXT ni{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device_, &ni);
}


// ======================================================== resources

BufferH VkDeviceImpl::create_buffer(const BufferDesc &d, const void *initial) {
    BufferH h = buffers.create();
    VkBufferRes *b = buffers.get(h);
    b->size = d.size ? d.size : 4;
    b->usage = d.usage;
    b->access = d.access;
    b->name = d.name ? d.name : "";

    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = b->size;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (d.usage & BufferUsage::Vertex) ci.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (d.usage & BufferUsage::Index) ci.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (d.usage & BufferUsage::Uniform) ci.usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (d.usage & BufferUsage::Storage) ci.usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (d.usage & BufferUsage::Indirect) ci.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &ci, nullptr, &b->buffer) != VK_SUCCESS) {
        WR_ERROR("vk: could not create buffer '%s'", b->name.c_str());
        buffers.destroy(h);
        return {};
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, b->buffer, &req);
    MemoryUsage mu = d.access == MemoryAccess::CpuToGpu   ? MemoryUsage::CpuToGpu
                     : d.access == MemoryAccess::GpuToCpu ? MemoryUsage::GpuToCpu
                                                          : MemoryUsage::GpuOnly;
    b->memory = alloc_.allocate(req, mu);
    if (!b->memory.valid()) {
        vkDestroyBuffer(device_, b->buffer, nullptr);
        buffers.destroy(h);
        return {};
    }
    vkBindBufferMemory(device_, b->buffer, b->memory.memory, b->memory.offset);
    name_object(uint64_t(b->buffer), VK_OBJECT_TYPE_BUFFER, d.name);
    if (initial) write_buffer(h, initial, d.size, 0);
    return h;
}

// WHERE A DEFERRED DELETION GOES.
//
// Not `frames_[frame_index_]`, which is what this used to do and is
// wrong half the time. end_frame ADVANCES frame_index_, so between
// frames it names the slot that will be recorded NEXT -- and that
// slot's fence is from frames_in_flight ago and is already signalled.
// A resource destroyed between frames was therefore freed at the very
// next begin_frame, while the frame that had just been submitted was
// still reading it.
//
// That is not a subtle corruption. A game frees things in its update
// -- a corpse is removed, a weapon is swapped -- and an update runs
// between frames, because that is when a script runs. So buffers and
// descriptor sets were destroyed out from under the GPU, the
// validation layer said so in a flood, and some seconds later the
// frame loop came apart: fences in use, command buffers in use, and
// vkQueueSubmit2 failing outright.
//
// The correct slot is the one whose fence is waited LAST among
// everything that could still be running: the frame being recorded if
// there is one, otherwise the frame just submitted.
VkDeviceImpl::Frame &VkDeviceImpl::retiring_frame() {
    if (frame_open_) return frames_[frame_index_];
    return frames_[(frame_index_ + frames_in_flight_ - 1) % frames_in_flight_];
}

void VkDeviceImpl::destroy(BufferH h) {
    VkBufferRes *b = buffers.get(h);
    if (!b) return;
    // Deferred: the GPU may still be reading it this frame and for
    // frames_in_flight - 1 more.
    VkBuffer buf = b->buffer;
    Allocation mem = b->memory;
    retiring_frame().deletions.push_back([this, buf, mem]() {
        if (buf) vkDestroyBuffer(device_, buf, nullptr);
        if (mem.valid()) alloc_.free(mem);
    });
    buffers.destroy(h);
}

void *VkDeviceImpl::map(BufferH h) {
    VkBufferRes *b = buffers.get_checked(h, "buffer");
    if (!b) return nullptr;
    if (!b->memory.mapped)
        WR_ERROR("vk: buffer '%s' is not host visible", b->name.c_str());
    return b->memory.mapped;
}

void VkDeviceImpl::unmap(BufferH h) { (void)h; }

void VkDeviceImpl::write_buffer(BufferH h, const void *data, uint64_t size,
                                uint64_t offset) {
    VkBufferRes *b = buffers.get_checked(h, "buffer");
    if (!b || !data || !size) return;
    if (offset + size > b->size) {
        WR_ERROR("vk: write of %llu at %llu overflows buffer '%s' (%llu bytes)",
                 (unsigned long long)size, (unsigned long long)offset,
                 b->name.c_str(), (unsigned long long)b->size);
        return;
    }
    // Host visible: write straight through the persistent mapping.
    if (b->memory.mapped) {
        std::memcpy((char *)b->memory.mapped + offset, data, size);
        return;
    }
    // Device local: it has to go through a staging copy.
    Pending p;
    p.bytes.assign((const uint8_t *)data, (const uint8_t *)data + size);
    p.dst_buffer = h;
    p.dst_offset = offset;
    // If a command buffer is open and we are not inside a render pass,
    // the copy can be recorded now and the bytes consumed in order.
    // Otherwise it waits for the next one.
    if (frame_open_ && !inside_render_pass_) {
        int64_t at = stage(p.bytes.data(), p.bytes.size(), 16);
        if (at >= 0) {
            record_copy(frames_[frame_index_].cb, uint64_t(at), p);
            return;
        }
        WR_WARN("vk: staging ring full mid-frame; deferring an upload of %llu "
                "bytes to the next frame",
                (unsigned long long)size);
    }
    pending_uploads_.push_back(std::move(p));
}

TextureH VkDeviceImpl::create_texture(const TextureDesc &d, const void *initial) {
    TextureH h = textures.create();
    VkTextureRes *t = textures.get(h);
    t->desc = d;
    uint32_t mips = d.mips;
    if (mips == 0) {
        mips = 1;
        uint32_t w = d.width, ht = d.height;
        while (w > 1 || ht > 1) { w = w > 1 ? w / 2 : 1; ht = ht > 1 ? ht / 2 : 1; mips++; }
    }
    t->desc.mips = mips;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = d.dim == TextureDim::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = vk_format(d.format);
    ci.extent = {d.width, d.height, d.dim == TextureDim::Tex3D ? d.depth : 1};
    ci.mipLevels = mips;
    ci.arrayLayers = d.dim == TextureDim::TexCube ? 6
                     : d.dim == TextureDim::Tex2DArray ? d.layers : 1;
    ci.samples = VkSampleCountFlagBits(d.samples ? d.samples : 1);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (d.usage & TextureUsage::Sampled) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (d.usage & TextureUsage::ColourTarget) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (d.usage & TextureUsage::DepthTarget)
        ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (d.usage & TextureUsage::Storage) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (d.usage & TextureUsage::TransferSrc) ci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // Mip generation blits from each level to the next, so a texture
    // that wants mips must also be a transfer source.
    if (mips > 1) ci.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d.dim == TextureDim::TexCube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    if (vkCreateImage(device_, &ci, nullptr, &t->image) != VK_SUCCESS) {
        WR_ERROR("vk: could not create texture '%s' (%ux%u %s)",
                 d.name ? d.name : "?", d.width, d.height, format_name(d.format));
        textures.destroy(h);
        return {};
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, t->image, &req);
    t->memory = alloc_.allocate(req, MemoryUsage::GpuOnly);
    if (!t->memory.valid()) {
        vkDestroyImage(device_, t->image, nullptr);
        textures.destroy(h);
        return {};
    }
    vkBindImageMemory(device_, t->image, t->memory.memory, t->memory.offset);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->image;
    vi.viewType = d.dim == TextureDim::TexCube      ? VK_IMAGE_VIEW_TYPE_CUBE
                  : d.dim == TextureDim::Tex2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                  : d.dim == TextureDim::Tex3D      ? VK_IMAGE_VIEW_TYPE_3D
                                                    : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {aspect_of(d.format), 0, mips, 0, ci.arrayLayers};
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &t->view), "image view");
    name_object(uint64_t(t->image), VK_OBJECT_TYPE_IMAGE, d.name);
    // Sampled textures that are never written still have to leave
    // UNDEFINED before anything reads them.
    if (d.usage & TextureUsage::Sampled) pending_initial_layout_.push_back(h);
    if (initial) {
        uint64_t bytes = uint64_t(d.width) * d.height * format_block_size(d.format);
        write_texture(h, initial, bytes, 0, 0);
        // Given its pixels and asked for a chain: fill the rest in,
        // because nothing else is going to.
        if (mips > 1) generate_mips(h);
    }
    return h;
}

void VkDeviceImpl::destroy(TextureH h) {
    VkTextureRes *t = textures.get(h);
    if (!t || t->is_swapchain) return;
    VkImage img = t->owns_image ? t->image : VK_NULL_HANDLE;
    VkImageView view = t->view;
    std::vector<VkImageView> slices;
    for (const auto &kv : t->slices) slices.push_back(kv.second);
    Allocation mem = t->memory;
    retiring_frame().deletions.push_back([this, img, view, slices, mem]() {
        for (VkImageView v : slices) vkDestroyImageView(device_, v, nullptr);
        if (view) vkDestroyImageView(device_, view, nullptr);
        if (img) vkDestroyImage(device_, img, nullptr);
        if (mem.valid()) alloc_.free(mem);
    });
    textures.destroy(h);
}

VkImageView VkDeviceImpl::attachment_view(VkTextureRes &t, int32_t layer,
                                          uint32_t mip) {
    // The whole-image view already is the right one for a plain 2D
    // texture attached at mip zero, which is nearly every attachment.
    if (layer < 0 && mip == 0 && t.desc.layers <= 1) return t.view;
    const uint64_t key = (uint64_t(uint32_t(layer)) << 32) | mip;
    auto it = t.slices.find(key);
    if (it != t.slices.end()) return it->second;

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    // A single layer is attached as a plain 2D view even when the
    // image is an array; a whole array is attached as an array view,
    // which is what a layered render pass would want.
    vi.viewType = layer >= 0 ? VK_IMAGE_VIEW_TYPE_2D
                  : t.desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                      : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = vk_format(t.desc.format);
    vi.subresourceRange = {aspect_of(t.desc.format), mip, 1,
                           layer >= 0 ? uint32_t(layer) : 0,
                           layer >= 0 ? 1u : t.desc.layers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(device_, &vi, nullptr, &v) != VK_SUCCESS) {
        WR_ERROR("vk: could not make an attachment view for layer %d mip %u",
                 layer, mip);
        return t.view;
    }
    t.slices[key] = v;
    return v;
}

void VkDeviceImpl::write_texture(TextureH h, const void *data, uint64_t size,
                                 uint32_t mip, uint32_t layer) {
    VkTextureRes *t = textures.get_checked(h, "texture");
    if (!t || !data || !size) return;
    Pending p;
    p.bytes.assign((const uint8_t *)data, (const uint8_t *)data + size);
    p.dst_texture = h;
    p.mip = mip;
    p.layer = layer;
    p.width = std::max(1u, t->desc.width >> mip);
    p.height = std::max(1u, t->desc.height >> mip);
    if (frame_open_ && !inside_render_pass_) {
        int64_t at = stage(p.bytes.data(), p.bytes.size(), 256);
        if (at >= 0) {
            record_copy(frames_[frame_index_].cb, uint64_t(at), p);
            return;
        }
    }
    pending_uploads_.push_back(std::move(p));
}

TextureDesc VkDeviceImpl::texture_desc(TextureH h) const {
    const VkTextureRes *t = textures.get(h);
    return t ? t->desc : TextureDesc();
}

size_t VkDeviceImpl::read_texture(TextureH h, void *out, size_t capacity,
                                  uint32_t mip, uint32_t layer) {
    VkTextureRes *t = textures.get_checked(h, "texture");
    if (!t || !out) return 0;
    uint32_t w = std::max(1u, t->desc.width >> mip);
    uint32_t hh = std::max(1u, t->desc.height >> mip);
    size_t need = size_t(w) * hh * format_block_size(t->desc.format);
    if (capacity < need) {
        WR_ERROR("vk: read_texture needs %zu bytes, given %zu", need, capacity);
        return 0;
    }

    BufferDesc bd;
    bd.size = need;
    bd.usage = BufferUsage::TransferDst;
    bd.access = MemoryAccess::GpuToCpu;
    bd.name = "readback";
    BufferH staging = create_buffer(bd, nullptr);
    VkBufferRes *sb = buffers.get(staging);
    if (!sb || !sb->memory.mapped) {
        destroy(staging);
        return 0;
    }

    // A one-shot command buffer of its own: this is a debug and
    // screenshot path, and borrowing the frame's would mean flushing
    // work that has not been recorded yet.
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = frames_[frame_index_].pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkDeviceWaitIdle(device_);
    if (vkAllocateCommandBuffers(device_, &ai, &cb) != VK_SUCCESS) {
        destroy(staging);
        return 0;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkImageLayout was = t->layout;
    transition(cb, *t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    region.imageSubresource = {aspect_of(t->desc.format), mip, layer, 1};
    region.imageExtent = {w, hh, 1};
    VkCopyImageToBufferInfo2 ci{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
    ci.srcImage = t->image;
    ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ci.dstBuffer = sb->buffer;
    ci.regionCount = 1;
    ci.pRegions = &region;
    vkCmdCopyImageToBuffer2(cb, &ci);
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, *t, was);
    vkEndCommandBuffer(cb);

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cb;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, frames_[frame_index_].pool, 1, &cb);

    std::memcpy(out, sb->memory.mapped, need);
    destroy(staging);
    // The deferred delete would otherwise wait a frame that may never
    // come in a headless test. Safe here: the queue was just waited
    // idle, so nothing is still reading anything.
    for (auto &fn : retiring_frame().deletions) fn();
    retiring_frame().deletions.clear();
    return need;
}

SamplerH VkDeviceImpl::create_sampler(const SamplerDesc &d) {
    SamplerH h = samplers.create();
    VkSamplerRes *s = samplers.get(h);
    auto mode = [](AddressMode m) {
        switch (m) {
            case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case AddressMode::MirrorRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case AddressMode::ClampEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case AddressMode::ClampBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = d.mag == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = d.min == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = d.mip == MipFilter::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = mode(d.address_u);
    ci.addressModeV = mode(d.address_v);
    ci.addressModeW = mode(d.address_w);
    ci.mipLodBias = d.lod_bias;
    ci.anisotropyEnable = d.anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = std::min(d.anisotropy, float(caps_.max_anisotropy));
    ci.compareEnable = d.compare_enable ? VK_TRUE : VK_FALSE;
    ci.compareOp = vk_compare(d.compare);
    ci.minLod = d.min_lod;
    ci.maxLod = d.mip == MipFilter::None ? 0.25f : d.max_lod;
    // A shadow lookup outside the map must come back "lit", which under
    // reverse-Z is the maximum. White border, both backends.
    ci.borderColor = d.border == BorderColour::TransparentBlack
                         ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                     : d.border == BorderColour::OpaqueBlack
                         ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                         : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VK_CHECK(vkCreateSampler(device_, &ci, nullptr, &s->sampler), "sampler");
    name_object(uint64_t(s->sampler), VK_OBJECT_TYPE_SAMPLER, d.name);
    return h;
}

void VkDeviceImpl::destroy(SamplerH h) {
    if (VkSamplerRes *s = samplers.get(h)) {
        VkSampler sm = s->sampler;
        retiring_frame().deletions.push_back(
            [this, sm]() { if (sm) vkDestroySampler(device_, sm, nullptr); });
    }
    samplers.destroy(h);
}

ShaderH VkDeviceImpl::create_shader(const ShaderDesc &d) {
    if (!d.spirv || !d.spirv_words) {
        WR_ERROR("vk: shader '%s' has no SPIR-V", d.name ? d.name : "?");
        return {};
    }
    ShaderH h = shaders.create();
    VkShaderRes *s = shaders.get(h);
    s->stage = d.stage;
    s->entry = d.entry ? d.entry : "main";
    s->name = d.name ? d.name : "";
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = d.spirv_words * 4;
    ci.pCode = d.spirv;
    if (vkCreateShaderModule(device_, &ci, nullptr, &s->module) != VK_SUCCESS) {
        WR_ERROR("vk: shader module '%s' rejected", s->name.c_str());
        shaders.destroy(h);
        return {};
    }
    name_object(uint64_t(s->module), VK_OBJECT_TYPE_SHADER_MODULE, d.name);
    return h;
}

void VkDeviceImpl::destroy(ShaderH h) {
    if (VkShaderRes *s = shaders.get(h))
        if (s->module) vkDestroyShaderModule(device_, s->module, nullptr);
    shaders.destroy(h);
}

BindGroupLayoutH VkDeviceImpl::create_bind_group_layout(
    const BindGroupLayoutDesc &d) {
    BindGroupLayoutH h = layouts.create();
    VkLayoutRes *l = layouts.get(h);
    l->desc = d;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(d.entries.size());
    for (const BindGroupLayoutEntry &e : d.entries) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = e.binding;
        b.descriptorType = vk_descriptor_type(e.type);
        b.descriptorCount = e.count;
        b.stageFlags = vk_stages(e);
        bindings.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo ci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = uint32_t(bindings.size());
    ci.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &ci, nullptr, &l->layout),
             "descriptor set layout");
    return h;
}

void VkDeviceImpl::destroy(BindGroupLayoutH h) {
    if (VkLayoutRes *l = layouts.get(h))
        if (l->layout) vkDestroyDescriptorSetLayout(device_, l->layout, nullptr);
    layouts.destroy(h);
}

BindGroupH VkDeviceImpl::create_bind_group(const BindGroupDesc &d) {
    VkLayoutRes *l = layouts.get_checked(d.layout, "bind group layout");
    if (!l) return {};
    BindGroupH h = groups.create();
    VkGroupRes *g = groups.get(h);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &l->layout;
    if (vkAllocateDescriptorSets(device_, &ai, &g->set) != VK_SUCCESS) {
        WR_ERROR("vk: descriptor pool exhausted allocating '%s'",
                 d.name ? d.name : "?");
        groups.destroy(h);
        return {};
    }
    name_object(uint64_t(g->set), VK_OBJECT_TYPE_DESCRIPTOR_SET, d.name);
    update_bind_group(h, d);
    return h;
}

void VkDeviceImpl::destroy(BindGroupH h) {
    if (VkGroupRes *g = groups.get(h)) {
        VkDescriptorSet set = g->set;
        retiring_frame().deletions.push_back([this, set]() {
            if (set) vkFreeDescriptorSets(device_, descriptor_pool_, 1, &set);
        });
    }
    groups.destroy(h);
}

void VkDeviceImpl::update_bind_group(BindGroupH h, const BindGroupDesc &d) {
    VkGroupRes *g = groups.get_checked(h, "bind group");
    if (!g) return;
    g->desc = d;
    const VkLayoutRes *l = layouts.get(d.layout);
    if (!l) return;

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffer_infos;
    std::vector<VkDescriptorImageInfo> image_infos;
    // Reserved up front: these vectors must not reallocate, because the
    // writes point into them.
    buffer_infos.reserve(d.entries.size());
    image_infos.reserve(d.entries.size());

    for (const BindGroupEntry &e : d.entries) {
        BindingType type = BindingType::UniformBuffer;
        uint32_t count = 1;
        for (const BindGroupLayoutEntry &le : l->desc.entries)
            if (le.binding == e.binding) { type = le.type; count = le.count; break; }

        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = g->set;
        w.dstBinding = e.binding;
        w.descriptorCount = 1;
        w.descriptorType = vk_descriptor_type(type);
        (void)count;

        if (type == BindingType::SampledTexture || type == BindingType::StorageTexture) {
            const VkTextureRes *t = textures.get(e.texture);
            const VkSamplerRes *s = samplers.get(e.sampler);
            if (!t) continue;
            VkDescriptorImageInfo ii{};
            ii.imageView = t->view;
            ii.sampler = s ? s->sampler : VK_NULL_HANDLE;
            ii.imageLayout = type == BindingType::StorageTexture
                                 ? VK_IMAGE_LAYOUT_GENERAL
                             : format_is_depth(t->desc.format)
                                 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image_infos.push_back(ii);
            w.pImageInfo = &image_infos.back();
        } else {
            const VkBufferRes *b = buffers.get(e.buffer);
            if (!b) continue;
            VkDescriptorBufferInfo bi{};
            bi.buffer = b->buffer;
            bi.offset = e.offset;
            bi.range = e.range ? e.range : (b->size - e.offset);
            buffer_infos.push_back(bi);
            w.pBufferInfo = &buffer_infos.back();
        }
        writes.push_back(w);
    }
    if (!writes.empty())
        vkUpdateDescriptorSets(device_, uint32_t(writes.size()), writes.data(), 0,
                               nullptr);
}

// ------------------------------------------------------------- pipelines

PipelineH VkDeviceImpl::create_pipeline(const PipelineDesc &d) {
    if (d.push_constant_size > 128) {
        WR_ERROR("pipeline '%s' wants %u bytes of push constants; the engine's "
                 "budget is 128, which is what Vulkan guarantees",
                 d.name ? d.name : "?", d.push_constant_size);
        return {};
    }
    const VkShaderRes *vs = shaders.get(d.vertex);
    const VkShaderRes *fs = shaders.get(d.fragment);
    if (!vs || !fs) {
        WR_ERROR("vk: pipeline '%s' is missing a stage", d.name ? d.name : "?");
        return {};
    }

    std::vector<VkDescriptorSetLayout> set_layouts;
    for (BindGroupLayoutH h : d.bind_group_layouts) {
        const VkLayoutRes *l = layouts.get(h);
        set_layouts.push_back(l ? l->layout : VK_NULL_HANDLE);
    }
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = d.push_constant_size;

    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = uint32_t(set_layouts.size());
    pl.pSetLayouts = set_layouts.data();
    pl.pushConstantRangeCount = d.push_constant_size ? 1 : 0;
    pl.pPushConstantRanges = &pcr;

    PipelineH h = pipelines.create();
    VkPipelineRes *p = pipelines.get(h);
    p->desc = d;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &p->layout) != VK_SUCCESS) {
        pipelines.destroy(h);
        return {};
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs->module;
    stages[0].pName = vs->entry.c_str();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs->module;
    stages[1].pName = fs->entry.c_str();

    std::vector<VkVertexInputBindingDescription> vb;
    for (const VertexBinding &b : d.vertex_layout.bindings)
        vb.push_back({b.binding, b.stride,
                      b.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                     : VK_VERTEX_INPUT_RATE_VERTEX});
    std::vector<VkVertexInputAttributeDescription> va;
    for (const VertexAttribute &a : d.vertex_layout.attributes)
        va.push_back({a.location, a.binding, vk_format(a.format), a.offset});

    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(vb.size());
    vi.pVertexBindingDescriptions = vb.data();
    vi.vertexAttributeDescriptionCount = uint32_t(va.size());
    vi.pVertexAttributeDescriptions = va.data();

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = vk_topology(d.topology);

    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = d.raster.polygon == PolygonMode::Fill   ? VK_POLYGON_MODE_FILL
                     : d.raster.polygon == PolygonMode::Line ? VK_POLYGON_MODE_LINE
                                                             : VK_POLYGON_MODE_POINT;
    rs.cullMode = d.raster.cull == CullMode::None   ? VK_CULL_MODE_NONE
                  : d.raster.cull == CullMode::Back ? VK_CULL_MODE_BACK_BIT
                                                    : VK_CULL_MODE_FRONT_BIT;
    // THE WINDING, AND WHY IT IS *NOT* FLIPPED.
    //
    // Vulkan decides facing from the signed area in FRAMEBUFFER
    // coordinates, whose y already points down -- so a triangle that
    // is counter-clockwise in NDC arrives clockwise, and a naive port
    // has to flip. But this backend also gives every viewport a
    // NEGATIVE HEIGHT to put +Y up, and that is a second flip. The two
    // cancel: counter-clockwise in NDC is counter-clockwise in the
    // framebuffer, and the mapping here is the identity.
    //
    // Flipping anyway is not subtle but it is very confusing: every
    // front face is culled and every back face is drawn, so the screen
    // is not empty, it is inside out.
    rs.frontFace = d.raster.front_face == FrontFace::CounterClockwise
                       ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                       : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = d.raster.line_width;
    rs.depthClampEnable = d.raster.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.depthBiasEnable = d.depth_stencil.depth_bias_enable ? VK_TRUE : VK_FALSE;
    rs.depthBiasConstantFactor = d.depth_stencil.depth_bias_constant;
    rs.depthBiasSlopeFactor = d.depth_stencil.depth_bias_slope;
    rs.depthBiasClamp = d.depth_stencil.depth_bias_clamp;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(d.samples ? d.samples : 1);

    auto face = [](const StencilFace &f) {
        VkStencilOpState s{};
        s.failOp = vk_stencil_op(f.fail);
        s.passOp = vk_stencil_op(f.pass);
        s.depthFailOp = vk_stencil_op(f.depth_fail);
        s.compareOp = vk_compare(f.compare);
        s.compareMask = f.compare_mask;
        s.writeMask = f.write_mask;
        s.reference = 0;  // dynamic
        return s;
    };
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth_stencil.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = d.depth_stencil.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = vk_compare(d.depth_stencil.depth_compare);
    ds.stencilTestEnable = d.depth_stencil.stencil_test ? VK_TRUE : VK_FALSE;
    ds.front = face(d.depth_stencil.front);
    ds.back = face(d.depth_stencil.back);
    ds.minDepthBounds = 0.0f;
    ds.maxDepthBounds = 1.0f;

    std::vector<VkPipelineColorBlendAttachmentState> blends;
    for (size_t i = 0; i < std::max<size_t>(d.colour_formats.size(), 1); i++) {
        BlendState b = i < d.blend.size() ? d.blend[i]
                       : d.blend.empty()  ? BlendState()
                                          : d.blend.back();
        VkPipelineColorBlendAttachmentState a{};
        a.blendEnable = b.enable ? VK_TRUE : VK_FALSE;
        a.srcColorBlendFactor = vk_blend_factor(b.src_colour);
        a.dstColorBlendFactor = vk_blend_factor(b.dst_colour);
        a.colorBlendOp = vk_blend_op(b.colour_op);
        a.srcAlphaBlendFactor = vk_blend_factor(b.src_alpha);
        a.dstAlphaBlendFactor = vk_blend_factor(b.dst_alpha);
        a.alphaBlendOp = vk_blend_op(b.alpha_op);
        a.colorWriteMask = (b.write_r ? VK_COLOR_COMPONENT_R_BIT : 0) |
                           (b.write_g ? VK_COLOR_COMPONENT_G_BIT : 0) |
                           (b.write_b ? VK_COLOR_COMPONENT_B_BIT : 0) |
                           (b.write_a ? VK_COLOR_COMPONENT_A_BIT : 0);
        blends.push_back(a);
    }
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = uint32_t(d.colour_formats.empty() ? 0 : blends.size());
    cb.pAttachments = blends.data();

    // Stencil reference is dynamic so recursive portals do not need one
    // pipeline per level. Viewport and scissor are dynamic so a
    // pipeline survives a window resize.
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;

    std::vector<VkFormat> colour_formats;
    for (Format f : d.colour_formats) colour_formats.push_back(vk_format(f));
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = uint32_t(colour_formats.size());
    ri.pColorAttachmentFormats = colour_formats.data();
    if (d.depth_format != Format::Undefined) {
        ri.depthAttachmentFormat = vk_format(d.depth_format);
        if (format_has_stencil(d.depth_format))
            ri.stencilAttachmentFormat = vk_format(d.depth_format);
    }

    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri;  // dynamic rendering: no VkRenderPass at all
    gi.stageCount = 2;
    gi.pStages = stages;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = p->layout;
    gi.renderPass = VK_NULL_HANDLE;

    // WARREN_DUMP_PIPELINES=1 prints the depth, stencil and blend
    // state of every pipeline as it is created. Two backends that
    // disagree about a picture almost always disagree about one of
    // these first, and reading them back is faster than bisecting.
    if (getenv("WARREN_DUMP_PIPELINES"))
        WR_INFO("vk pipeline '%s': stencil=%d front(cmp=%d ref-dyn fail=%d "
                "dfail=%d pass=%d cmask=0x%02x wmask=0x%02x) depth(test=%d "
                "write=%d cmp=%d) cull=%d colourmask=0x%x fmt(depth=%d stencil=%d)",
                d.name ? d.name : "?", int(ds.stencilTestEnable),
                int(ds.front.compareOp), int(ds.front.failOp),
                int(ds.front.depthFailOp), int(ds.front.passOp),
                ds.front.compareMask, ds.front.writeMask,
                int(ds.depthTestEnable), int(ds.depthWriteEnable),
                int(ds.depthCompareOp), int(rs.cullMode),
                blends.empty() ? 0u : unsigned(blends[0].colorWriteMask),
                int(ri.depthAttachmentFormat), int(ri.stencilAttachmentFormat));
    if (vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &gi, nullptr,
                                  &p->pipeline) != VK_SUCCESS) {
        WR_ERROR("vk: pipeline '%s' could not be created", d.name ? d.name : "?");
        vkDestroyPipelineLayout(device_, p->layout, nullptr);
        pipelines.destroy(h);
        return {};
    }
    name_object(uint64_t(p->pipeline), VK_OBJECT_TYPE_PIPELINE, d.name);
    return h;
}

PipelineH VkDeviceImpl::create_compute_pipeline(const ComputePipelineDesc &d) {
    const VkShaderRes *cs = shaders.get(d.compute);
    if (!cs) return {};
    std::vector<VkDescriptorSetLayout> set_layouts;
    for (BindGroupLayoutH h : d.bind_group_layouts) {
        const VkLayoutRes *l = layouts.get(h);
        set_layouts.push_back(l ? l->layout : VK_NULL_HANDLE);
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, d.push_constant_size};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = uint32_t(set_layouts.size());
    pl.pSetLayouts = set_layouts.data();
    pl.pushConstantRangeCount = d.push_constant_size ? 1 : 0;
    pl.pPushConstantRanges = &pcr;

    PipelineH h = pipelines.create();
    VkPipelineRes *p = pipelines.get(h);
    p->compute = true;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &p->layout) != VK_SUCCESS) {
        pipelines.destroy(h);
        return {};
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs->module;
    ci.stage.pName = cs->entry.c_str();
    ci.layout = p->layout;
    if (vkCreateComputePipelines(device_, pipeline_cache_, 1, &ci, nullptr,
                                 &p->pipeline) != VK_SUCCESS) {
        vkDestroyPipelineLayout(device_, p->layout, nullptr);
        pipelines.destroy(h);
        return {};
    }
    name_object(uint64_t(p->pipeline), VK_OBJECT_TYPE_PIPELINE, d.name);
    return h;
}

void VkDeviceImpl::destroy(PipelineH h) {
    if (VkPipelineRes *p = pipelines.get(h)) {
        VkPipeline pipe = p->pipeline;
        VkPipelineLayout lay = p->layout;
        frames_[frame_index_].deletions.push_back([this, pipe, lay]() {
            if (pipe) vkDestroyPipeline(device_, pipe, nullptr);
            if (lay) vkDestroyPipelineLayout(device_, lay, nullptr);
        });
    }
    pipelines.destroy(h);
}

// ------------------------------------------------------------ transitions

void VkDeviceImpl::transition(VkCommandBuffer cb, VkTextureRes &t,
                              VkImageLayout to) {
    if (t.layout == to) return;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.oldLayout = t.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {aspect_of(t.desc.format), 0, VK_REMAINING_MIP_LEVELS, 0,
                          VK_REMAINING_ARRAY_LAYERS};

    // Conservative but correct: ALL_COMMANDS on both sides, with the
    // access masks chosen for the layouts. A frame graph will narrow
    // these; getting them wrong is a corruption that appears on one
    // driver and not another, so they start wide on purpose.
    auto stage_access = [](VkImageLayout l, VkPipelineStageFlags2 *stage,
                           VkAccessFlags2 *access) {
        switch (l) {
            case VK_IMAGE_LAYOUT_UNDEFINED:
                *stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
                *access = 0;
                break;
            case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
                *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                *access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
                break;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
                *stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
                *access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
                break;
            case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                *stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                *access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                break;
            case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
                *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
                *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                *access = VK_ACCESS_2_TRANSFER_READ_BIT;
                break;
            case VK_IMAGE_LAYOUT_GENERAL:
                *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                *access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                break;
            case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
                *stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
                *access = 0;
                break;
            default:
                *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                *access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                break;
        }
    };
    stage_access(t.layout, &b.srcStageMask, &b.srcAccessMask);
    stage_access(to, &b.dstStageMask, &b.dstAccessMask);

    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
    t.layout = to;
}

int64_t VkDeviceImpl::stage(const void *data, uint64_t size, uint64_t alignment) {
    Frame &f = frames_[frame_index_];
    VkBufferRes *sb = buffers.get(f.staging);
    if (!sb || !sb->memory.mapped) return -1;
    uint64_t at = (f.staging_used + alignment - 1) / alignment * alignment;
    if (at + size > sb->size) return -1;
    std::memcpy((char *)sb->memory.mapped + at, data, size);
    f.staging_used = at + size;
    return int64_t(at);
}

void VkDeviceImpl::record_copy(VkCommandBuffer cb, uint64_t src_offset,
                               const Pending &p) {
    Frame &f = frames_[frame_index_];
    const VkBufferRes *src = buffers.get(f.staging);
    if (!src) return;
    if (p.dst_buffer.valid()) {
        const VkBufferRes *dst = buffers.get(p.dst_buffer);
        if (!dst) return;
        VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
        region.srcOffset = src_offset;
        region.dstOffset = p.dst_offset;
        region.size = p.bytes.size();
        VkCopyBufferInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
        ci.srcBuffer = src->buffer;
        ci.dstBuffer = dst->buffer;
        ci.regionCount = 1;
        ci.pRegions = &region;
        vkCmdCopyBuffer2(cb, &ci);
    } else if (p.dst_texture.valid()) {
        VkTextureRes *dst = textures.get(p.dst_texture);
        if (!dst) return;
        transition(cb, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        region.bufferOffset = src_offset;
        region.imageSubresource = {aspect_of(dst->desc.format), p.mip, p.layer, 1};
        region.imageExtent = {p.width, p.height, 1};
        VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
        ci.srcBuffer = src->buffer;
        ci.dstImage = dst->image;
        ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ci.regionCount = 1;
        ci.pRegions = &region;
        vkCmdCopyBufferToImage2(cb, &ci);
        transition(cb, *dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

// Everything written while no command buffer was open, staged and
// copied now that one is. Called immediately after vkBeginCommandBuffer
// and before anything else, so the copies land before the first draw
// that could read them.
void VkDeviceImpl::flush_uploads(VkCommandBuffer cb) {
    // Mips can outlive the uploads that caused them -- see the end of
    // this function -- so an empty upload queue is not a reason to
    // leave without looking.
    if (pending_uploads_.empty() && pending_mips_.empty()) return;
    std::vector<Pending> batch;
    batch.swap(pending_uploads_);
    // WHAT WILL NOT FIT WAITS, it is not thrown away.
    //
    // A frame that loads a model asks to upload every one of its
    // textures at once -- six 512x512 maps with mips is eight
    // megabytes against an eight megabyte ring -- and dropping the
    // overflow leaves those textures as whatever was in the image
    // when it was created. Nothing fails, nothing is logged where
    // anyone looks, and the model is silently untextured.
    //
    // Deferring costs a frame or two at a load, which is exactly
    // when a frame or two is free.
    std::vector<Pending> deferred;
    const VkBufferRes *ring = buffers.get(frames_[frame_index_].staging);
    const uint64_t ring_size = ring ? ring->size : 0;
    for (Pending &p : batch) {
        if (!deferred.empty()) {
            // Once one has been held over, everything after it waits
            // too: uploads to the same resource must stay in order.
            deferred.push_back(std::move(p));
            continue;
        }
        int64_t at = stage(p.bytes.data(), p.bytes.size(),
                           p.dst_texture.valid() ? 256 : 16);
        if (at < 0) {
            if (p.bytes.size() > ring_size) {
                // Bigger than the whole ring: waiting will not help.
                WR_ERROR("vk: an upload of %zu bytes exceeds the %llu byte "
                         "staging ring and was dropped",
                         p.bytes.size(), (unsigned long long)ring_size);
                continue;
            }
            deferred.push_back(std::move(p));
            continue;
        }
        record_copy(cb, uint64_t(at), p);
    }
    if (!deferred.empty()) {
        // Ahead of anything queued since, for the same ordering
        // reason.
        deferred.insert(deferred.end(),
                        std::make_move_iterator(pending_uploads_.begin()),
                        std::make_move_iterator(pending_uploads_.end()));
        pending_uploads_.swap(deferred);
    }
    // After the copies, in the same buffer: a chain built from a
    // level zero that has not landed yet would be a chain of
    // whatever was there before.
    //
    // AND THAT INCLUDES A LEVEL ZERO THAT WAS JUST DEFERRED. Holding
    // the overflow back fixed textures coming out blank, and quietly
    // introduced this: the copy waits a frame, the mip chain does
    // not, and the chain is built from an image nothing has written.
    // Level zero lands next frame and the mips are never rebuilt, so
    // the texture is correct close up and garbage at any distance
    // that samples a smaller level -- which reads as dirt on the
    // model, not as a missing upload. It cost a long afternoon.
    //
    // The threshold is sharp and entirely about the ring: five
    // 512-pixel textures on one mesh were fine and seven were not.
    std::vector<TextureH> mips;
    mips.swap(pending_mips_);
    for (TextureH h : mips) {
        bool waiting = false;
        for (const Pending &p : pending_uploads_) {
            if (p.dst_texture == h) {
                waiting = true;
                break;
            }
        }
        if (waiting) {
            // Back in the queue; it will be built the frame after
            // its last copy lands.
            pending_mips_.push_back(h);
            continue;
        }
        if (VkTextureRes *t = textures.get(h)) record_mips(cb, *t);
    }
}

void VkDeviceImpl::generate_mips(TextureH h) {
    const VkTextureRes *t = textures.get(h);
    if (!t || t->desc.mips <= 1) return;
    pending_mips_.push_back(h);
}

// The blit chain, shared by the device-level call above and the
// command-list one a renderer uses on a texture it has just drawn to.
void VkDeviceImpl::record_mips(VkCommandBuffer cb, VkTextureRes &t) {
    if (t.desc.mips <= 1) return;
    int32_t w = int32_t(t.desc.width), ht = int32_t(t.desc.height);
    transition(cb, t, VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t m = 1; m < t.desc.mips; m++) {
        VkImageBlit2 b{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        b.srcOffsets[1] = {w, ht, 1};
        w = std::max(1, w / 2);
        ht = std::max(1, ht / 2);
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        b.dstOffsets[1] = {w, ht, 1};
        VkBlitImageInfo2 bi{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
        bi.srcImage = t.image;
        bi.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        bi.dstImage = t.image;
        bi.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        bi.regionCount = 1;
        bi.pRegions = &b;
        bi.filter = VK_FILTER_LINEAR;
        vkCmdBlitImage2(cb, &bi);
    }
    transition(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ------------------------------------------------------------- the frame

CommandList *VkDeviceImpl::begin_frame() {
    if (frame_open_) {
        WR_ERROR("vk: begin_frame called twice");
        return cmd_;
    }
    Frame &f = frames_[frame_index_];
    vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);

    // Everything queued for deletion when this frame was last recorded
    // has now finished on the GPU.
    for (auto &fn : f.deletions) fn();
    f.deletions.clear();

    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, f.acquired,
                                       VK_NULL_HANDLE, &image_index_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window_, &w, &h);
        vkDeviceWaitIdle(device_);
        create_swapchain(uint32_t(w), uint32_t(h));
        return nullptr;
    }
    if (r != VK_SUCCESS) {
        WR_ERROR("vk: vkAcquireNextImageKHR failed (%d)", int(r));
        return nullptr;
    }

    vkResetFences(device_, 1, &f.fence);
    vkResetCommandPool(device_, f.pool, 0);
    f.staging_used = 0;

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cb, &bi);
    flush_uploads(f.cb);

    // A texture created but never rendered to is still in UNDEFINED,
    // and sampling one is undefined behaviour the validation layer
    // rightly objects to. Anything sampled must be moved to a readable
    // layout once, and the first frame is the place.
    for (TextureH h : pending_initial_layout_)
        if (VkTextureRes *t = textures.get(h))
            transition(f.cb, *t,
                       format_is_depth(t->desc.format)
                           ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    pending_initial_layout_.clear();

    swapchain_handle_ = swapchain_images_[image_index_];
    cmd_->cb = f.cb;
    stats_ = FrameStats();
    frame_open_ = true;
    return cmd_;
}

void VkDeviceImpl::end_frame() {
    if (!frame_open_) return;
    frame_open_ = false;
    Frame &f = frames_[frame_index_];

    // The swapchain image must be in PRESENT_SRC when it is handed
    // back, whatever the renderer left it in.
    if (VkTextureRes *sc = textures.get(swapchain_handle_))
        transition(f.cb, *sc, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(f.cb);

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = f.rendered;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cb;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(queue_, 1, &si, f.fence), "vkQueueSubmit2");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &f.rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image_index_;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(window_, &w, &h);
        vkDeviceWaitIdle(device_);
        create_swapchain(uint32_t(w), uint32_t(h));
    }
    frame_index_ = (frame_index_ + 1) % frames_in_flight_;
}

size_t VkDeviceImpl::pending_deletions() const {
    size_t n = 0;
    for (const Frame &f : frames_) n += f.deletions.size();
    return n;
}

std::string VkDeviceImpl::resource_report() const {
    char b[512];
    std::snprintf(b, sizeof(b),
                  "Vulkan: %zu buffers, %zu textures, %zu samplers, %zu shaders, "
                  "%zu pipelines, %zu bind groups\n  %s",
                  buffers.live_count(), textures.live_count(), samplers.live_count(),
                  shaders.live_count(), pipelines.live_count(), groups.live_count(),
                  alloc_.report().c_str());
    return b;
}

// ==================================================== the command list

void VkCommandListImpl::begin_rendering(const RenderingInfo &info) {
    std::vector<VkRenderingAttachmentInfo> colour;
    for (const ColourAttachment &c : info.colour) {
        VkTextureRes *t = dev_->textures.get(c.texture);
        if (!t) continue;
        dev_->transition(cb, *t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = dev_->attachment_view(*t, c.layer, c.mip);
        a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp = c.load == LoadOp::Clear    ? VK_ATTACHMENT_LOAD_OP_CLEAR
                   : c.load == LoadOp::Load   ? VK_ATTACHMENT_LOAD_OP_LOAD
                                              : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.storeOp = c.store == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE
                                              : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.clearValue.color = {{c.clear.r, c.clear.g, c.clear.b, c.clear.a}};
        if (c.resolve.valid()) {
            VkTextureRes *rt = dev_->textures.get(c.resolve);
            if (rt) {
                dev_->transition(cb, *rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                a.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                a.resolveImageView = dev_->attachment_view(*rt, c.layer, c.mip);
                a.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
        }
        colour.push_back(a);
    }

    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool has_stencil = false;
    if (info.has_depth) {
        VkTextureRes *t = dev_->textures.get(info.depth.texture);
        if (t) {
            dev_->transition(cb, *t,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            depth.imageView = dev_->attachment_view(*t, info.depth.layer,
                                                    info.depth.mip);
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = info.depth.depth_load == LoadOp::Clear
                               ? VK_ATTACHMENT_LOAD_OP_CLEAR
                           : info.depth.depth_load == LoadOp::Load
                               ? VK_ATTACHMENT_LOAD_OP_LOAD
                               : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depth.storeOp = info.depth.depth_store == StoreOp::Store
                                ? VK_ATTACHMENT_STORE_OP_STORE
                                : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // Reverse-Z clears to zero.
            depth.clearValue.depthStencil.depth = info.depth.clear_depth;
            if (format_has_stencil(t->desc.format)) {
                has_stencil = true;
                stencil = depth;
                stencil.loadOp = info.depth.stencil_load == LoadOp::Clear
                                     ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                 : info.depth.stencil_load == LoadOp::Load
                                     ? VK_ATTACHMENT_LOAD_OP_LOAD
                                     : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                stencil.storeOp = info.depth.stencil_store == StoreOp::Store
                                      ? VK_ATTACHMENT_STORE_OP_STORE
                                      : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                stencil.clearValue.depthStencil.stencil = info.depth.clear_stencil;
            }
        }
    }

    uint32_t w = info.width, h = info.height;
    if (!w || !h) {
        TextureDesc td = !info.colour.empty()
                             ? dev_->texture_desc(info.colour[0].texture)
                             : dev_->texture_desc(info.depth.texture);
        w = td.width;
        h = td.height;
    }

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = uint32_t(colour.size());
    ri.pColorAttachments = colour.data();
    ri.pDepthAttachment = info.has_depth ? &depth : nullptr;
    ri.pStencilAttachment = has_stencil ? &stencil : nullptr;
    if (getenv("WARREN_DUMP_PIPELINES"))
        WR_INFO("vk begin_rendering '%s': %ux%u colour=%zu depth=%s stencil=%s "
                "(depth load=%d clear=%.3f, stencil load=%d clear=%u)",
                info.name ? info.name : "?", w, h, colour.size(),
                ri.pDepthAttachment ? "yes" : "NO",
                ri.pStencilAttachment ? "yes" : "NO",
                int(depth.loadOp), double(depth.clearValue.depthStencil.depth),
                int(stencil.loadOp), stencil.clearValue.depthStencil.stencil);
    vkCmdBeginRendering(cb, &ri);
    rendering_ = true;
    dev_->inside_render_pass_ = true;
    dev_->stats_.render_passes++;

    // The whole target, in the engine's convention; set_viewport does
    // the flip.
    Viewport vp;
    vp.width = float(w);
    vp.height = float(h);
    set_viewport(vp);
    set_scissor({0, 0, w, h});
}

void VkCommandListImpl::end_rendering() {
    if (!rendering_) return;
    vkCmdEndRendering(cb);
    rendering_ = false;
    dev_->inside_render_pass_ = false;
}

// THE NEGATIVE HEIGHT LIVES HERE AND NOWHERE ELSE. The caller gives a
// top-left, y-down, positive-height rectangle; this turns it into the
// flipped viewport that makes Vulkan's clip space agree with OpenGL's.
void VkCommandListImpl::set_viewport(const Viewport &v) {
    VkViewport vv;
    vv.x = v.x;
    vv.y = v.y + v.height;
    vv.width = v.width;
    vv.height = -v.height;
    vv.minDepth = v.min_depth;
    vv.maxDepth = v.max_depth;
    vkCmdSetViewport(cb, 0, 1, &vv);
}

void VkCommandListImpl::set_scissor(const Rect &r) {
    VkRect2D s{{r.x, r.y}, {r.width, r.height}};
    vkCmdSetScissor(cb, 0, 1, &s);
}

void VkCommandListImpl::bind_pipeline(PipelineH h) {
    VkPipelineRes *p = dev_->pipelines.get_checked(h, "pipeline");
    if (!p) return;
    bound_ = h;
    bound_layout_ = p->layout;
    vkCmdBindPipeline(cb,
                      p->compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                 : VK_PIPELINE_BIND_POINT_GRAPHICS,
                      p->pipeline);
    dev_->stats_.pipeline_binds++;
}

void VkCommandListImpl::bind_group(uint32_t set, BindGroupH h,
                                   const uint32_t *offsets, uint32_t count) {
    VkGroupRes *g = dev_->groups.get_checked(h, "bind group");
    const VkPipelineRes *p = dev_->pipelines.get(bound_);
    if (!g || !p) return;
    vkCmdBindDescriptorSets(cb,
                            p->compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                       : VK_PIPELINE_BIND_POINT_GRAPHICS,
                            p->layout, set, 1, &g->set, count, offsets);
    dev_->stats_.bind_group_binds++;
}

void VkCommandListImpl::push_constants(const void *data, uint32_t size,
                                       uint32_t offset) {
    if (!bound_layout_ || !data || !size) return;
    const VkPipelineRes *p = dev_->pipelines.get(bound_);
    VkShaderStageFlags stages = (p && p->compute)
                                    ? VK_SHADER_STAGE_COMPUTE_BIT
                                    : (VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT);
    vkCmdPushConstants(cb, bound_layout_, stages, offset, size, data);
}

void VkCommandListImpl::set_stencil_reference(uint32_t ref) {
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ref);
}

void VkCommandListImpl::set_blend_constant(const Color &c) {
    const float v[4] = {c.r, c.g, c.b, c.a};
    vkCmdSetBlendConstants(cb, v);
}

void VkCommandListImpl::bind_vertex_buffer(uint32_t slot, BufferH h,
                                           uint64_t offset) {
    const VkBufferRes *b = dev_->buffers.get(h);
    if (!b) return;
    VkDeviceSize o = offset;
    vkCmdBindVertexBuffers(cb, slot, 1, &b->buffer, &o);
}

void VkCommandListImpl::bind_index_buffer(BufferH h, IndexType t, uint64_t offset) {
    const VkBufferRes *b = dev_->buffers.get(h);
    if (!b) return;
    vkCmdBindIndexBuffer(cb, b->buffer, offset,
                         t == IndexType::U16 ? VK_INDEX_TYPE_UINT16
                                             : VK_INDEX_TYPE_UINT32);
}

void VkCommandListImpl::draw(uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) {
    vkCmdDraw(cb, v, i, fv, fi);
    dev_->stats_.draw_calls++;
    dev_->stats_.triangles += uint64_t(v / 3) * i;
}

void VkCommandListImpl::draw_indexed(uint32_t idx, uint32_t inst, uint32_t first,
                                     int32_t vofs, uint32_t finst) {
    vkCmdDrawIndexed(cb, idx, inst, first, vofs, finst);
    dev_->stats_.draw_calls++;
    dev_->stats_.triangles += uint64_t(idx / 3) * inst;
}

void VkCommandListImpl::draw_indexed_indirect(BufferH args, uint64_t offset,
                                              uint32_t count, uint32_t stride) {
    const VkBufferRes *b = dev_->buffers.get(args);
    if (!b) return;
    vkCmdDrawIndexedIndirect(cb, b->buffer, offset, count, stride);
    dev_->stats_.draw_calls += count;
}

void VkCommandListImpl::dispatch(uint32_t x, uint32_t y, uint32_t z) {
    vkCmdDispatch(cb, x, y, z);
    dev_->stats_.dispatches++;
}

void VkCommandListImpl::copy_buffer(BufferH s, uint64_t so, BufferH d,
                                    uint64_t dof, uint64_t size) {
    const VkBufferRes *src = dev_->buffers.get(s);
    const VkBufferRes *dst = dev_->buffers.get(d);
    if (!src || !dst || !size) return;
    VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
    region.srcOffset = so;
    region.dstOffset = dof;
    region.size = size;
    VkCopyBufferInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
    ci.srcBuffer = src->buffer;
    ci.dstBuffer = dst->buffer;
    ci.regionCount = 1;
    ci.pRegions = &region;
    vkCmdCopyBuffer2(cb, &ci);
}

void VkCommandListImpl::copy_buffer_to_texture(BufferH s, uint64_t so, TextureH d,
                                               uint32_t mip, uint32_t layer,
                                               uint32_t w, uint32_t h) {
    const VkBufferRes *src = dev_->buffers.get(s);
    VkTextureRes *dst = dev_->textures.get(d);
    if (!src || !dst) return;
    dev_->transition(cb, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    region.bufferOffset = so;
    region.imageSubresource = {aspect_of(dst->desc.format), mip, layer, 1};
    region.imageExtent = {w, h, 1};
    VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    ci.srcBuffer = src->buffer;
    ci.dstImage = dst->image;
    ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ci.regionCount = 1;
    ci.pRegions = &region;
    vkCmdCopyBufferToImage2(cb, &ci);
}

void VkCommandListImpl::generate_mips(TextureH h) {
    if (VkTextureRes *t = dev_->textures.get(h)) dev_->record_mips(cb, *t);
}

void VkCommandListImpl::texture_barrier(TextureH h, TextureUsage from,
                                        TextureUsage to) {
    (void)from;
    VkTextureRes *t = dev_->textures.get(h);
    if (!t) return;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (to & TextureUsage::ColourTarget)
        layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    else if (to & TextureUsage::DepthTarget)
        layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    else if (to & TextureUsage::Storage)
        layout = VK_IMAGE_LAYOUT_GENERAL;
    else if (to & TextureUsage::TransferSrc)
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    else if (to & TextureUsage::TransferDst)
        layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    else if ((to & TextureUsage::Sampled) && format_is_depth(t->desc.format))
        layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    dev_->transition(cb, *t, layout);
}

void VkCommandListImpl::buffer_barrier(BufferH h) {
    const VkBufferRes *b = dev_->buffers.get(h);
    if (!b) return;
    VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    bb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    bb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    bb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = b->buffer;
    bb.size = VK_WHOLE_SIZE;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.bufferMemoryBarrierCount = 1;
    di.pBufferMemoryBarriers = &bb;
    vkCmdPipelineBarrier2(cb, &di);
}

void VkCommandListImpl::push_debug_group(const char *name, const Color &c) {
    if (!dev_->debug_labels_ || !vkCmdBeginDebugUtilsLabelEXT || !name) return;
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = name;
    l.color[0] = c.r; l.color[1] = c.g; l.color[2] = c.b; l.color[3] = c.a;
    vkCmdBeginDebugUtilsLabelEXT(cb, &l);
}

void VkCommandListImpl::pop_debug_group() {
    if (!dev_->debug_labels_ || !vkCmdEndDebugUtilsLabelEXT) return;
    vkCmdEndDebugUtilsLabelEXT(cb);
}

void VkCommandListImpl::insert_debug_marker(const char *name) {
    if (!dev_->debug_labels_ || !vkCmdInsertDebugUtilsLabelEXT || !name) return;
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = name;
    vkCmdInsertDebugUtilsLabelEXT(cb, &l);
}

}  // namespace

Device *create_vulkan_device(const DeviceDesc &d) {
    VkDeviceImpl *dev = new VkDeviceImpl();
    if (!dev->init(d)) {
        delete dev;
        return nullptr;
    }
    return dev;
}

}  // namespace wr::rhi
