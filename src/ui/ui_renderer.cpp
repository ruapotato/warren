#include "ui/ui_renderer.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "render/shaders/generated/shaders.h"
#include "render/texture.h"
#include "ui/font.h"

namespace mf::ui {
namespace {

// THE ATLAS IS ALMOST ENTIRELY WHITE, and that is the trick that
// makes the whole UI one draw call. The glyphs are drawn as solid
// rectangles by the draw list -- one per run of set dots -- so they
// need no texture at all; the texture exists only so that the
// pipeline has one to sample, and every vertex points at a white
// texel. A real atlas would be the optimisation, and at a few
// thousand quads a frame it is not needed yet.
constexpr uint32_t kAtlasSize = 4;

}  // namespace

bool Renderer::init(rhi::Device *dev, rhi::Format target_format,
                    uint32_t samples) {
    using namespace rhi;
    device_ = dev;
    if (!dev) return false;

    const shaders::Blob *vs_blob = shaders::find("ui", ShaderStage::Vertex);
    const shaders::Blob *fs_blob = shaders::find("ui", ShaderStage::Fragment);
    if (!vs_blob || !fs_blob) {
        MF_ERROR("ui: the ui shader is missing from the build");
        return false;
    }
    ShaderH vs = dev->create_shader(shaders::desc(*vs_blob));
    ShaderH fs = dev->create_shader(shaders::desc(*fs_blob));

    TextureDesc td;
    td.width = td.height = kAtlasSize;
    td.format = Format::RGBA8;
    td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    td.name = "ui atlas";
    atlas_ = dev->create_texture(td);
    std::vector<uint8_t> white(size_t(kAtlasSize) * kAtlasSize * 4, 255);
    dev->write_texture(atlas_, white.data(), white.size());

    BindGroupLayoutDesc bl;
    bl.entries.push_back({0, BindingType::SampledTexture, false, true, false, 1});
    bl.name = "ui";
    layout_ = dev->create_bind_group_layout(bl);

    BindGroupDesc bg;
    bg.layout = layout_;
    bg.name = "ui";
    BindGroupEntry e;
    e.binding = 0;
    e.texture = atlas_;
    e.sampler = SamplerCache::linear_clamp(dev);
    bg.entries.push_back(e);
    group_ = dev->create_bind_group(bg);

    VertexLayout vl;
    vl.bindings.push_back({0, uint32_t(sizeof(Vertex)), false});
    vl.attributes.push_back(
        {0, 0, Format::RG32F, uint32_t(offsetof(Vertex, position))});
    vl.attributes.push_back(
        {1, 0, Format::RG32F, uint32_t(offsetof(Vertex, uv))});
    // The colour is four bytes normalised, which is what the theme
    // stores and a quarter of what four floats would cost per vertex.
    vl.attributes.push_back(
        {2, 0, Format::RGBA8, uint32_t(offsetof(Vertex, colour))});

    PipelineDesc pd;
    pd.vertex = vs;
    pd.fragment = fs;
    pd.vertex_layout = vl;
    pd.colour_formats = {target_format};
    pd.depth_format = Format::Undefined;
    pd.samples = samples;
    pd.raster.cull = CullMode::None;
    pd.depth_stencil.depth_test = false;
    pd.depth_stencil.depth_write = false;
    pd.depth_stencil.stencil_test = false;
    pd.blend = {BlendState::alpha()};
    pd.bind_group_layouts = {layout_};
    pd.push_constant_size = 96;
    pd.name = "ui";
    pipeline_ = dev->create_pipeline(pd);
    return pipeline_.valid();
}

void Renderer::shutdown() {
    if (!device_) return;
    if (pipeline_.valid()) device_->destroy(pipeline_);
    if (vertex_buffer_.valid()) device_->destroy(vertex_buffer_);
    if (index_buffer_.valid()) device_->destroy(index_buffer_);
    if (atlas_.valid()) device_->destroy(atlas_);
    device_ = nullptr;
}

bool Renderer::ensure_capacity(size_t vertices, size_t indices) {
    using namespace rhi;
    // GROWN, NEVER SHRUNK, AND ONLY IN STEPS. A UI's vertex count
    // jumps around as panels open and lists scroll, and reallocating
    // a GPU buffer every time it does would stall the frame that
    // noticed.
    if (vertices > vertex_capacity_) {
        const size_t want = std::max<size_t>(4096, vertices * 2);
        if (vertex_buffer_.valid()) device_->destroy(vertex_buffer_);
        BufferDesc bd;
        bd.size = want * sizeof(Vertex);
        bd.usage = BufferUsage::Vertex;
        bd.access = MemoryAccess::CpuToGpu;
        bd.name = "ui vertices";
        vertex_buffer_ = device_->create_buffer(bd);
        vertex_capacity_ = want;
    }
    if (indices > index_capacity_) {
        const size_t want = std::max<size_t>(8192, indices * 2);
        if (index_buffer_.valid()) device_->destroy(index_buffer_);
        BufferDesc bd;
        bd.size = want * sizeof(uint32_t);
        bd.usage = BufferUsage::Index;
        bd.access = MemoryAccess::CpuToGpu;
        bd.name = "ui indices";
        index_buffer_ = device_->create_buffer(bd);
        index_capacity_ = want;
    }
    return vertex_buffer_.valid() && index_buffer_.valid();
}

void Renderer::draw(rhi::CommandList *cmd, const DrawData &data,
                    uint32_t width, uint32_t height) {
    using namespace rhi;
    if (!device_ || !cmd || data.empty() || !pipeline_.valid()) return;
    if (!ensure_capacity(data.vertices.size(), data.indices.size())) return;

    device_->write_buffer(vertex_buffer_, data.vertices.data(),
                          data.vertices.size() * sizeof(Vertex));
    device_->write_buffer(index_buffer_, data.indices.data(),
                          data.indices.size() * sizeof(uint32_t));

    cmd->bind_pipeline(pipeline_);
    cmd->bind_group(0, group_);
    cmd->bind_vertex_buffer(0, vertex_buffer_);
    cmd->bind_index_buffer(index_buffer_, IndexType::U32);

    struct Push {
        float model[16];
        float tint[4];
        float params[4];
    } push{};
    push.params[0] = float(width);
    push.params[1] = float(height);
    push.tint[0] = push.tint[1] = push.tint[2] = push.tint[3] = 1.0f;
    cmd->push_constants(&push, sizeof(push));

    for (const DrawCommand &c : data.commands) {
        if (!c.index_count) continue;
        const int x0 = std::max(0, int(c.clip.x));
        const int y0 = std::max(0, int(c.clip.y));
        const int x1 = std::min(int(width), int(c.clip.right() + 0.5f));
        const int y1 = std::min(int(height), int(c.clip.bottom() + 0.5f));
        if (x1 <= x0 || y1 <= y0) continue;
        cmd->set_scissor({x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0)});
        cmd->draw_indexed(c.index_count, 1, c.first_index);
    }
    cmd->set_scissor({0, 0, width, height});
}

}  // namespace mf::ui
