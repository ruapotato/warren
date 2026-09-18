// Warren -- the window and the GL context.
#pragma once

#include <string>

#include "core/math/vector.h"
#include "input.h"
#include "rhi/rhi.h"

struct SDL_Window;

namespace wr {

struct WindowConfig {
    // Which backend the window is being opened for. SDL needs to know
    // at creation time -- a Vulkan surface cannot be made on a window
    // created for OpenGL -- so this is the one piece of backend
    // knowledge the platform layer carries.
    rhi::Backend backend = rhi::Backend::Vulkan;
    std::string title = "Warren";
    int width = 1600;
    int height = 900;
    bool fullscreen = false;
    bool resizable = true;
    bool vsync = true;
    // Multisampling on the main colour target. Portals render into the
    // same buffer as everything else -- they are stencilled regions of
    // it, not textures -- so this covers them too, which is one of the
    // quiet wins of doing portals this way.
    int msaa = 4;
    // Validation layers on Vulkan, a debug context on OpenGL. Costs
    // nothing on a release driver that ignores the request.
    bool debug = true;
};

class Window {
public:
    Window() = default;
    ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    bool open(const WindowConfig &cfg);
    void close();
    bool is_open() const { return window_ != nullptr; }

    // Pump the OS queue into Input. False when the user asked to quit.
    bool poll();

    Vec2i size() const { return size_; }
    Vec2i drawable_size() const { return drawable_; }
    float aspect() const { return drawable_.aspect(); }
    // Physical pixels per logical pixel; 2 on a retina display.
    float dpi_scale() const;

    void set_title(const std::string &t);
    void set_fullscreen(bool on);
    bool fullscreen() const { return fullscreen_; }

    // Hide the cursor and feed relative motion, for a mouselook camera.
    void set_mouse_captured(bool on);
    bool mouse_captured() const { return mouse_captured_; }

    bool was_resized() const { return resized_; }
    bool has_focus() const { return focus_; }

    SDL_Window *sdl_window() const { return window_; }
    rhi::Backend backend() const { return backend_; }

private:
    SDL_Window *window_ = nullptr;
    rhi::Backend backend_ = rhi::Backend::Vulkan;
    Vec2i size_{0, 0};
    Vec2i drawable_{0, 0};
    bool fullscreen_ = false;
    bool mouse_captured_ = false;
    bool resized_ = false;
    bool focus_ = true;
};

// Monotonic wall clock and the frame timing built on it.
class Clock {
public:
    Clock();
    // Advance one frame. Returns the delta in seconds, clamped: a
    // breakpoint or a stalled driver must not teleport the simulation.
    float tick();
    float delta() const { return delta_; }
    double elapsed() const { return elapsed_; }
    uint64_t frame() const { return frame_; }
    // Smoothed, for a readout that a human can read.
    float fps() const { return fps_; }
    void set_max_delta(float s) { max_delta_ = s; }
    // Seconds since the process started, independent of frames.
    static double now();

private:
    double last_ = 0.0;
    double elapsed_ = 0.0;
    float delta_ = 0.0f;
    float max_delta_ = 0.1f;
    float fps_ = 0.0f;
    uint64_t frame_ = 0;
};

}  // namespace wr
