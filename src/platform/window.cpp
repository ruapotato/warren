#include "window.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include "core/log.h"

namespace mf {

Window::~Window() { close(); }

bool Window::open(const WindowConfig &cfg) {
    if (window_) close();

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0 && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        MF_FATAL("SDL video init failed: %s", SDL_GetError());
        return false;
    }
    backend_ = cfg.backend;

    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.resizable) flags |= SDL_WINDOW_RESIZABLE;
    if (cfg.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    if (backend_ == rhi::Backend::OpenGL) {
        flags |= SDL_WINDOW_OPENGL;
        // The context itself is created by the OpenGL device; these
        // attributes only have to be set before the window is made.
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        // EIGHT BITS OF STENCIL, OR THERE ARE NO PORTALS. The engine
        // refuses to start without them rather than rendering a
        // portal-shaped hole.
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        if (cfg.debug)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    } else {
        flags |= SDL_WINDOW_VULKAN;
    }

    window_ = SDL_CreateWindow(cfg.title.c_str(), SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, cfg.width, cfg.height,
                               flags);
    if (!window_) {
        MF_ERROR("could not create a %s window: %s",
                 rhi::backend_name(backend_), SDL_GetError());
        return false;
    }

    fullscreen_ = cfg.fullscreen;
    SDL_GetWindowSize(window_, &size_.x, &size_.y);
    if (backend_ == rhi::Backend::OpenGL)
        SDL_GL_GetDrawableSize(window_, &drawable_.x, &drawable_.y);
    else
        SDL_Vulkan_GetDrawableSize(window_, &drawable_.x, &drawable_.y);
    SDL_StartTextInput();
    return true;
}

void Window::close() {
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
}

bool Window::poll() {
    Input::_begin_frame();
    resized_ = false;
    bool keep_running = true;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                keep_running = false;
                break;
            case SDL_KEYDOWN:
                if (!e.key.repeat) Input::_feed_key(e.key.keysym.scancode, true);
                break;
            case SDL_KEYUP:
                Input::_feed_key(e.key.keysym.scancode, false);
                break;
            case SDL_MOUSEBUTTONDOWN:
                Input::_feed_mouse_button(e.button.button, true);
                break;
            case SDL_MOUSEBUTTONUP:
                Input::_feed_mouse_button(e.button.button, false);
                break;
            case SDL_MOUSEMOTION:
                Input::_feed_mouse_motion(float(e.motion.xrel), float(e.motion.yrel),
                                          float(e.motion.x), float(e.motion.y));
                break;
            case SDL_MOUSEWHEEL:
                Input::_feed_wheel(e.wheel.preciseY);
                break;
            case SDL_TEXTINPUT:
                Input::_feed_text(e.text.text);
                break;
            case SDL_WINDOWEVENT:
                switch (e.window.event) {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    case SDL_WINDOWEVENT_RESIZED:
                        SDL_GetWindowSize(window_, &size_.x, &size_.y);
                        if (backend_ == rhi::Backend::OpenGL)
                            SDL_GL_GetDrawableSize(window_, &drawable_.x,
                                                   &drawable_.y);
                        else
                            SDL_Vulkan_GetDrawableSize(window_, &drawable_.x,
                                                       &drawable_.y);
                        resized_ = true;
                        break;
                    case SDL_WINDOWEVENT_FOCUS_GAINED:
                        focus_ = true;
                        break;
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        focus_ = false;
                        Input::_focus_lost();
                        break;
                    case SDL_WINDOWEVENT_CLOSE:
                        keep_running = false;
                        break;
                    default:
                        break;
                }
                break;
            default:
                break;
        }
    }
    return keep_running;
}

float Window::dpi_scale() const {
    if (size_.x <= 0) return 1.0f;
    return float(drawable_.x) / float(size_.x);
}

void Window::set_title(const std::string &t) {
    if (window_) SDL_SetWindowTitle(window_, t.c_str());
}

void Window::set_fullscreen(bool on) {
    if (!window_) return;
    SDL_SetWindowFullscreen(window_, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    fullscreen_ = on;
    SDL_GetWindowSize(window_, &size_.x, &size_.y);
    if (backend_ == rhi::Backend::OpenGL)
        SDL_GL_GetDrawableSize(window_, &drawable_.x, &drawable_.y);
    else
        SDL_Vulkan_GetDrawableSize(window_, &drawable_.x, &drawable_.y);
    resized_ = true;
}

void Window::set_mouse_captured(bool on) {
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    mouse_captured_ = on;
}

// ----------------------------------------------------------------- Clock

Clock::Clock() { last_ = now(); }

double Clock::now() {
    static Uint64 freq = SDL_GetPerformanceFrequency();
    return double(SDL_GetPerformanceCounter()) / double(freq);
}

float Clock::tick() {
    double t = now();
    double d = t - last_;
    last_ = t;
    if (d < 0.0) d = 0.0;
    // A LONG FRAME IS NOT A LONG TICK. Dragging the window, hitting a
    // breakpoint or waking from sleep produce a delta of seconds, and a
    // physics step of seconds puts everything through a wall.
    if (d > max_delta_) d = max_delta_;
    delta_ = float(d);
    elapsed_ += d;
    frame_++;
    float inst = delta_ > 1e-6f ? 1.0f / delta_ : 0.0f;
    fps_ = fps_ <= 0.0f ? inst : fps_ + (inst - fps_) * 0.08f;
    return delta_;
}

}  // namespace mf
