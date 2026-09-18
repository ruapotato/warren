#include "engine.h"

#include <cstdio>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "core/log.h"
#include "scene/nodes.h"

namespace mf {

Engine::~Engine() { shutdown(); }

bool Engine::init(const EngineConfig &cfg) {
    config_ = cfg;

    // The window has to know the backend before it is created: a
    // Vulkan surface cannot be made on a window opened for OpenGL.
    std::vector<rhi::Backend> order{cfg.window.backend};
    if (cfg.allow_fallback)
        for (rhi::Backend b : rhi::available_backends())
            if (b != cfg.window.backend) order.push_back(b);

    for (rhi::Backend backend : order) {
        WindowConfig wc = cfg.window;
        wc.backend = backend;
        if (!window_.open(wc)) continue;

        rhi::DeviceDesc dd;
        dd.backend = backend;
        dd.window = window_.sdl_window();
        dd.validation = cfg.window.debug;
        dd.vsync = cfg.window.vsync;
        dd.samples = uint32_t(cfg.render.msaa);
        device_ = rhi::create_device(dd);
        if (device_) break;
        window_.close();
        if (order.size() > 1)
            MF_WARN("engine: %s did not start; trying the next backend",
                    rhi::backend_name(backend));
    }
    if (!device_) {
        MF_FATAL("engine: no graphics backend could be started");
        return false;
    }

    if (!renderer_.init(device_, cfg.render)) {
        MF_FATAL("engine: the renderer failed to start");
        return false;
    }
    physics_ = Ref<PhysicsWorld>(new PhysicsWorld());
    tree_ = std::make_unique<SceneTree>();
    if (cfg.physics_hz > 0.0f) tree_->set_physics_step(1.0f / cfg.physics_hz);

    ClassDB::register_all();
    if (on_ready) on_ready(*this);
    running_ = true;
    clock_ = Clock();
    if (config_.max_frames && config_.fixed_delta <= 0.0f) {
        config_.fixed_delta = 1.0f / 60.0f;
        MF_INFO("engine: %llu frames at a fixed 1/60s, for a reproducible run",
                (unsigned long long)config_.max_frames);
    }
    return true;
}

void Engine::shutdown() {
    if (device_) device_->wait_idle();
    tree_.reset();
    physics_.reset();
    renderer_.shutdown();
    if (device_) {
        rhi::destroy_device(device_);
        device_ = nullptr;
    }
    window_.close();
    running_ = false;
}

bool Engine::step() {
    if (!running_ || !device_) return false;
    if (!window_.poll()) return false;
    float real_dt = clock_.tick();
    const float dt = config_.fixed_delta > 0.0f ? config_.fixed_delta : real_dt;

    if (window_.was_resized()) {
        Vec2i d = window_.drawable_size();
        device_->wait_idle();
        device_->resize_swapchain(uint32_t(d.x), uint32_t(d.y));
        renderer_.resize(uint32_t(d.x), uint32_t(d.y));
    }

    if (on_frame) on_frame(*this, dt);

    // FIXED STEP PHYSICS, VARIABLE STEP EVERYTHING ELSE. Capped at
    // four sub-steps: a frame that took a second must not try to
    // simulate a second, or the catch-up takes longer than the stall
    // and the game never recovers.
    if (config_.physics_hz > 0.0f) {
        physics_accumulator_ += dt;
        const float step = tree_->physics_step();
        int steps = 0;
        while (physics_accumulator_ >= step && steps < 4) {
            tree_->physics_tick(step);
            physics_accumulator_ -= step;
            steps++;
        }
        if (steps == 4) physics_accumulator_ = 0.0;
    } else {
        tree_->physics_tick(dt);
    }

    tree_->process(dt);
    tree_->flush_frees();

    const bool want_shot = !config_.screenshot_path.empty() &&
                           frames_ + 1 == config_.screenshot_frame;
    rhi::CommandList *cmd = device_->begin_frame();
    if (cmd) {
        Camera3D *cam = tree_->active_camera();
        if (cam) renderer_.render(cmd, tree_.get(), cam, device_->swapchain_texture());
        if (want_shot) device_->request_capture();
        device_->end_frame();
    }

    frames_++;
    if (want_shot) save_screenshot(config_.screenshot_path);
    if (config_.max_frames && frames_ >= config_.max_frames) return false;
    return true;
}

int Engine::run() {
    while (step()) {
    }
    return 0;
}

bool Engine::save_screenshot(const std::string &path) {
    if (!device_) return false;
    device_->wait_idle();
    rhi::TextureH src = device_->swapchain_texture();
    rhi::TextureDesc td = device_->texture_desc(src);
    size_t bytes = size_t(td.width) * td.height * rhi::format_block_size(td.format);
    std::vector<uint8_t> pixels(bytes);
    size_t got = device_->read_texture(src, pixels.data(), pixels.size());
    if (got != bytes) {
        MF_ERROR("screenshot: could not read the swapchain back");
        return false;
    }
    // BGRA on most swapchains; PNG wants RGBA.
    if (td.format == rhi::Format::BGRA8 || td.format == rhi::Format::BGRA8_SRGB)
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            std::swap(pixels[i], pixels[i + 2]);
    // Rows come back top-down on both backends by contract, which is
    // what PNG wants.
    int ok = stbi_write_png(path.c_str(), int(td.width), int(td.height), 4,
                            pixels.data(), int(td.width * 4));
    if (!ok) {
        MF_ERROR("screenshot: could not write '%s'", path.c_str());
        return false;
    }
    MF_INFO("screenshot: %s (%ux%u)", path.c_str(), td.width, td.height);
    return true;
}

std::string Engine::status_line() const {
    const RenderStats &r = renderer_.stats();
    char b[256];
    std::snprintf(b, sizeof(b),
                  "%s | %.1f fps | %u draws, %u tris | %u views, portals deep %u",
                  device_ ? rhi::backend_name(device_->backend()) : "?",
                  double(clock_.fps()), r.draw_calls, r.triangles, r.views,
                  r.max_depth_reached);
    return b;
}

}  // namespace mf
