// Manifold -- the thing that owns the loop.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "platform/window.h"
#include "physics/world.h"
#include "plugin/host.h"
#include "render/renderer.h"
#include "scene/scene_tree.h"

namespace mf {

struct EngineConfig {
    WindowConfig window;
    RenderSettings render;
    // Try this backend; on failure, say so and stop. Set
    // `allow_fallback` to try the other one instead -- off by default,
    // because an engine that silently changes renderer produces bug
    // reports nobody can reproduce.
    bool allow_fallback = false;
    // Fixed timestep for physics and for anything that must be
    // deterministic. 0 runs physics at the frame rate, which is a
    // choice and not a default.
    float physics_hz = 60.0f;
    // Where to look for plugins. Empty uses `plugins` beside the
    // executable. "-" disables them.
    std::string plugin_directory;
    // Worker threads. 0 leaves one core for the main thread.
    int worker_threads = 0;
    // Stop after this many frames. For tests and for screenshots.
    uint64_t max_frames = 0;
    // A FIXED FRAME TIME, so a capture is reproducible.
    //
    // With real timing, the same command twice produces two different
    // pictures -- the simulation has taken a different number of
    // steps -- and comparing two backends' output compares the clock
    // as much as the renderer. Set when `max_frames` is, unless the
    // caller asks otherwise.
    float fixed_delta = 0.0f;
    std::string screenshot_path;
    uint64_t screenshot_frame = 0;
};

class Engine {
public:
    Engine() = default;
    ~Engine();

    bool init(const EngineConfig &cfg);
    void shutdown();

    // Runs until the window closes or max_frames is reached.
    int run();
    // Or drive it yourself: returns false when it is time to stop.
    bool step();

    Window *window() { return &window_; }
    rhi::Device *device() { return device_; }
    SceneTree *tree() { return tree_.get(); }
    Renderer *renderer() { return &renderer_; }
    PhysicsWorld *physics() { return physics_.get(); }
    PluginHost *plugins() { return &plugins_; }
    const Clock &clock() const { return clock_; }
    uint64_t frames() const { return frames_; }

    // Called once the device and the tree exist, before the first
    // frame. Build the scene here.
    std::function<void(Engine &)> on_ready;
    // Every frame, before the tree is processed.
    std::function<void(Engine &, float)> on_frame;

    bool save_screenshot(const std::string &path);
    std::string status_line() const;

private:
    EngineConfig config_;
    Window window_;
    rhi::Device *device_ = nullptr;
    std::unique_ptr<SceneTree> tree_;
    Renderer renderer_;
    Ref<PhysicsWorld> physics_;
    PluginHost plugins_;
    Clock clock_;
    double physics_accumulator_ = 0.0;
    uint64_t frames_ = 0;
    bool running_ = false;
};

}  // namespace mf
