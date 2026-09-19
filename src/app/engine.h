// Warren -- the thing that owns the loop.
#pragma once

#include <functional>
#include <memory>
#include <vector>
#include <string>

#include "platform/window.h"
#include "physics/world.h"
#include "plugin/host.h"
#include "audio/audio.h"
#include "editor/editor.h"
#include "render/renderer.h"
#include "scene/audio_nodes.h"
#include "scene/ui_system.h"
#include "ui/ui.h"
#include "ui/ui_renderer.h"
#include "scene/scene_tree.h"

namespace wr {

struct EngineConfig {
    WindowConfig window;
    RenderSettings render;
    AudioServer::Config audio;
    bool enable_audio = true;
    // The editor is built but off; F1 shows it. A shipped game
    // passes false and the panels are never constructed.
    bool enable_editor = true;
    bool editor_visible = false;
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
    // Python. Started after the plugins, so a plugin's classes are
    // scriptable too.
    bool python = true;
    std::vector<std::string> script_paths;
    // Run this once the scene exists.
    std::string startup_script;
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

// A SCRIPT CAN END THE GAME.
//
// There was no way at all. A menu's Quit button, a "you win"
// screen, a headless harness that has finished its work -- every
// one of them had to wait for max_frames to run out or for
// somebody to close the window by hand. `request_quit` is what a
// script's wr.quit() sets and what the frame loop honours at the
// end of the frame that asked, so the tick that asked to stop
// still finishes.
void request_quit();
bool quit_requested();

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
    AudioServer *audio() { return &audio_; }
    AudioSystem *audio_system() { return &audio_system_; }
    Editor *editor() { return &editor_; }
    UiSystem *ui_system() { return &ui_system_; }
    ui::Context *ui() { return &ui_; }
    const Clock &clock() const { return clock_; }
    uint64_t frames() const { return frames_; }

    // Called once the device and the tree exist, before the first
    // frame. Build the scene here.
    std::function<void(Engine &)> on_ready;
    // Every frame, before the tree is processed.
    std::function<void(Engine &, float)> on_frame;

    bool save_screenshot(const std::string &path);
    std::string status_line() const;

    // HOW LONG A FRAME ACTUALLY TOOK, and not the average.
    //
    // A mean frame time hides the thing that matters: an engine that
    // averages 4 ms and spikes to 40 every second when a chunk
    // streams in is worse to play than one that sits at 8. So the
    // last N frames are kept and reported as percentiles. `--bench`
    // prints them and exits.
    struct FrameTimes {
        uint32_t frames = 0;
        double mean_ms = 0, min_ms = 0, p50_ms = 0, p95_ms = 0, p99_ms = 0,
               max_ms = 0;
        double mean_cpu_ms = 0;  // the renderer's own, from RenderStats
    };
    FrameTimes frame_times(uint32_t skip_first = 0) const;
    // THE LAST FRAME, IN MILLISECONDS. A game that cannot time
    // itself cannot draw an FPS counter, cannot degrade its own
    // settings, and cannot tell "the portal hitched" from "it
    // felt like the portal hitched". Zero before the first frame
    // has been measured.
    double last_frame_ms() const { return last_frame_ms_; }
    double last_render_cpu_ms() const { return last_render_cpu_ms_; }
    uint64_t frame_count() const { return frames_; }
    void record_timings(bool on) { timing_ = on; }

private:
    EngineConfig config_;
    Window window_;
    rhi::Device *device_ = nullptr;
    std::unique_ptr<SceneTree> tree_;
    Renderer renderer_;
    Ref<PhysicsWorld> physics_;
    PluginHost plugins_;
    AudioServer audio_;
    AudioSystem audio_system_;
    Editor editor_;
    UiSystem ui_system_;
    ui::Context ui_;
    ui::Renderer ui_renderer_;
    bool ui_ready_ = false;
    Clock clock_;
    double physics_accumulator_ = 0.0;
    uint64_t frames_ = 0;
    bool running_ = false;
    bool timing_ = false;
    std::vector<double> frame_ms_, render_cpu_ms_;
    double last_frame_ms_ = 0.0, last_render_cpu_ms_ = 0.0;
};

}  // namespace wr
