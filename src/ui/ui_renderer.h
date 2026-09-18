// Manifold -- putting the UI's draw list on the screen.
#pragma once

#include "rhi/rhi.h"
#include "ui/ui.h"

namespace mf::ui {

class Renderer {
public:
    bool init(rhi::Device *dev, rhi::Format target_format, uint32_t samples = 1);
    void shutdown();
    // Into whatever pass is currently open. The caller owns the
    // render pass, so the UI can be drawn over the scene, into an
    // offscreen target, or into a test's texture.
    void draw(rhi::CommandList *cmd, const DrawData &data, uint32_t width,
              uint32_t height);

private:
    bool ensure_capacity(size_t vertices, size_t indices);

    rhi::Device *device_ = nullptr;
    rhi::PipelineH pipeline_;
    rhi::BufferH vertex_buffer_, index_buffer_;
    size_t vertex_capacity_ = 0, index_capacity_ = 0;
    rhi::TextureH atlas_;
    rhi::BindGroupLayoutH layout_;
    rhi::BindGroupH group_;
    rhi::BufferH dummy_ubo_;
};

}  // namespace mf::ui
