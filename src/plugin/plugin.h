// Warren -- the plugin contract.
//
// A plugin is a shared library that the engine loads at start-up and
// that can register node classes, resource loaders, render passes and
// services exactly as built-in code does. The voxel terrain that ships
// with the engine is one, on purpose: if the plugin interface is not
// good enough to build terrain with, it is not good enough.
//
// THE ENTRY POINTS ARE C, AND THE REST IS C++.
//
// C, because a C++ symbol's name and a C++ object's layout depend on
// the compiler, its version and its flags, and a plugin that cannot
// say "I was built for engine version X" before anything else is
// dereferenced will crash instead of complaining.
//
// C++ after that, because a plugin that could only speak C would have
// to reimplement every type the engine already has. The cost of this
// is stated plainly rather than hidden: A PLUGIN MUST BE BUILT FROM
// THE SAME HEADERS, WITH A COMPATIBLE COMPILER, AS THE ENGINE IT LOADS
// INTO. The ABI number below is checked first and refuses the load
// when it does not match, which turns a mystery into a message.
#pragma once

#include <cstdint>

#if defined(_WIN32)
#define WR_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define WR_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Bumped whenever anything a plugin can see changes shape. A plugin
// built against a different number is refused.
#define WARREN_PLUGIN_ABI 1

namespace wr {

class Engine;
class SceneTree;
class Renderer;
class PhysicsWorld;
namespace rhi {
class Device;
}

// What a plugin is told about itself. Static; read before anything
// else is called, so it must not depend on initialisation.
struct PluginInfo {
    uint32_t abi = WARREN_PLUGIN_ABI;
    const char *name = "unnamed";
    const char *version = "0.0.0";
    const char *author = "";
    const char *description = "";
    // Other plugins this one needs, loaded first. Null-terminated.
    const char *const *requires_plugins = nullptr;
};

// What a plugin is given. Everything a built-in subsystem has.
struct PluginContext {
    uint32_t abi = WARREN_PLUGIN_ABI;
    Engine *engine = nullptr;
    rhi::Device *device = nullptr;
    SceneTree *tree = nullptr;
    Renderer *renderer = nullptr;
    PhysicsWorld *physics = nullptr;
    // Where the plugin was loaded from, so it can find its own data.
    const char *directory = nullptr;
};

}  // namespace wr

// --- what a plugin must export -----------------------------------------
//
//   const wr::PluginInfo *mf_plugin_info(void);
//   bool mf_plugin_init(const wr::PluginContext *ctx);
//   void mf_plugin_shutdown(void);
//
// and may export:
//
//   void mf_plugin_frame(float dt);      // once per frame, before the tree
//
// WR_PLUGIN_DECLARE writes the first two for the common case.

#define WR_PLUGIN_DECLARE(NAME, VERSION, AUTHOR, DESCRIPTION)              \
    static const ::wr::PluginInfo k_mf_plugin_info = {                     \
        WARREN_PLUGIN_ABI, NAME, VERSION, AUTHOR, DESCRIPTION, nullptr}; \
    WR_PLUGIN_EXPORT const ::wr::PluginInfo *mf_plugin_info(void) {        \
        return &k_mf_plugin_info;                                          \
    }
