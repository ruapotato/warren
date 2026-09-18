#include "engine.h"

#include <cstdio>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <filesystem>

#include "core/jobs.h"
#include "core/log.h"
#include "scene/nodes.h"
#if MANIFOLD_PYTHON
#include "script/python.h"
#endif

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

    Jobs::init(cfg.worker_threads);
    ClassDB::register_all();

    // PLUGINS BEFORE THE SCENE. A plugin registers node classes, and
    // a scene built before it loaded could not name them.
    if (cfg.plugin_directory != "-") {
        std::string dir = PluginHost::resolve_directory(cfg.plugin_directory);
        PluginContext pc;
        pc.engine = this;
        pc.device = device_;
        pc.tree = tree_.get();
        pc.renderer = &renderer_;
        pc.physics = physics_.get();
        int n = plugins_.load_directory(dir, pc);
        if (n) MF_INFO("%s", plugins_.report().c_str());
    }

    if (cfg.enable_audio) {
        audio_.init(cfg.audio);
        audio_system_.set_server(&audio_);
        // The system installs itself as the one the nodes talk to on
        // its first update; doing it here too means a node that plays
        // something during on_ready, before any frame has run, finds
        // a server rather than silence.
        audio_system_install(&audio_system_);
    }

#if MANIFOLD_PYTHON
    if (cfg.python) {
        std::vector<std::string> paths = cfg.script_paths;
        char *base = SDL_GetBasePath();
        if (base) {
            paths.push_back((std::filesystem::path(base) / "scripts").string());
            SDL_free(base);
        }
        // AFTER THE PLUGINS. A class registered by a plugin has to
        // exist before the module is built, or a script cannot name
        // it -- and refresh_classes covers anything registered later.
        if (Python::init(this, paths)) Python::refresh_classes();
    }
#endif

    if (on_ready) on_ready(*this);

#if MANIFOLD_PYTHON
    // AFTER on_ready, so the script finds a scene to work on.
    if (cfg.python && !cfg.startup_script.empty()) {
        Python::refresh_classes();
        Python::run_file(cfg.startup_script);
    }
#endif
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

    // THE ORDER HERE IS LOAD-BEARING.
    //
    // 1. The tree, first. Destroying a node runs its script's
    //    destructor, which releases a Python reference -- so Python
    //    must still be alive. It also runs the voxel terrain's
    //    destructor, which waits for its meshing jobs -- so the
    //    workers must still be alive.
    // 2. Python, once nothing holds a reference into it.
    // 3. Plugins, once the nodes they registered are gone.
    // 4. Jobs, once nothing is waiting on one.
    //
    // AUDIO GOES FIRST OF ALL, before any of it. The device calls
    // back on its own thread and holds a reference to whatever clip
    // each voice is playing; a node destroyed while that callback is
    // mid-mix would free the samples under it. Closing the device
    // joins that thread, and after that nothing else is racing.
    audio_.shutdown();
    audio_system_.set_server(nullptr);
    audio_system_install(nullptr);

    tree_.reset();
#if MANIFOLD_PYTHON
    Python::shutdown();
#endif
    plugins_.unload_all();
    Jobs::shutdown();
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
    const double frame_start = Clock::now();
    float real_dt = clock_.tick();
    const float dt = config_.fixed_delta > 0.0f ? config_.fixed_delta : real_dt;

    if (window_.was_resized()) {
        Vec2i d = window_.drawable_size();
        device_->wait_idle();
        device_->resize_swapchain(uint32_t(d.x), uint32_t(d.y));
        renderer_.resize(uint32_t(d.x), uint32_t(d.y));
    }

    plugins_.frame(dt);
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

    // AFTER THE TREE, BEFORE THE FRAME. Every player's place in the
    // world is settled by now, and a sound placed from last frame's
    // transforms lags the picture by exactly the amount that makes a
    // footstep sound like it came from behind you.
    if (audio_.running()) audio_system_.update(tree_.get(), dt);

    const bool want_shot = !config_.screenshot_path.empty() &&
                           frames_ + 1 == config_.screenshot_frame;
    rhi::CommandList *cmd = device_->begin_frame();
    if (cmd) {
        Camera3D *cam = tree_->active_camera();
        if (cam) renderer_.render(cmd, tree_.get(), cam, device_->swapchain_texture());
        if (want_shot) device_->request_capture();
        device_->end_frame();
    }

    if (timing_) {
        // WALL CLOCK AROUND THE WHOLE STEP, submission included. The
        // renderer's own cpu_ms covers recording only, and a frame
        // that spends its time waiting on a fence spends it outside
        // that. Both are kept, because the gap between them is where
        // "the GPU is the bottleneck" lives.
        frame_ms_.push_back((Clock::now() - frame_start) * 1000.0);
        render_cpu_ms_.push_back(renderer_.stats().cpu_ms);
    }
    frames_++;
    if (want_shot) save_screenshot(config_.screenshot_path);
    if (config_.max_frames && frames_ >= config_.max_frames) return false;
    return true;
}

Engine::FrameTimes Engine::frame_times(uint32_t skip_first) const {
    FrameTimes t;
    if (frame_ms_.size() <= skip_first) return t;
    // The first frames are pipeline warm-up, shader upload and the
    // first chunk of streaming; including them measures the load, not
    // the loop.
    std::vector<double> v(frame_ms_.begin() + skip_first, frame_ms_.end());
    t.frames = uint32_t(v.size());
    double sum = 0;
    for (double d : v) sum += d;
    t.mean_ms = sum / double(v.size());
    double cpu = 0;
    for (size_t i = skip_first; i < render_cpu_ms_.size(); i++)
        cpu += render_cpu_ms_[i];
    t.mean_cpu_ms = render_cpu_ms_.size() > skip_first
                        ? cpu / double(render_cpu_ms_.size() - skip_first)
                        : 0.0;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) {
        size_t i = size_t(q * double(v.size() - 1) + 0.5);
        return v[std::min(i, v.size() - 1)];
    };
    t.min_ms = v.front();
    t.p50_ms = at(0.50);
    t.p95_ms = at(0.95);
    t.p99_ms = at(0.99);
    t.max_ms = v.back();
    return t;
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
