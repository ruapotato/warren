#include "host.h"
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "core/log.h"

namespace mf {
namespace {

void *open_library(const std::string &path, std::string *error) {
#if defined(_WIN32)
    HMODULE h = LoadLibraryA(path.c_str());
    if (!h && error) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "LoadLibrary failed (%lu)", GetLastError());
        *error = buf;
    }
    return (void *)h;
#else
    // RTLD_LOCAL so two plugins cannot see each other's symbols by
    // accident; RTLD_NOW so a missing symbol is reported here rather
    // than at the first call to it.
    void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h && error) {
        const char *e = dlerror();
        *error = e ? e : "dlopen failed";
    }
    return h;
#endif
}

void *symbol(void *handle, const char *name) {
#if defined(_WIN32)
    return (void *)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

void close_library(void *handle) {
#if defined(_WIN32)
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

}  // namespace

const char *PluginHost::library_extension() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

PluginHost::~PluginHost() { unload_all(); }

bool PluginHost::load(const std::string &path, const PluginContext &ctx) {
    std::string error;
    void *handle = open_library(path, &error);
    if (!handle) {
        MF_ERROR("plugin '%s' could not be opened: %s", path.c_str(), error.c_str());
        return false;
    }

    auto info_fn = (const PluginInfo *(*)())symbol(handle, "mf_plugin_info");
    if (!info_fn) {
        MF_ERROR("plugin '%s' has no mf_plugin_info; it is not a Manifold plugin",
                 path.c_str());
        close_library(handle);
        return false;
    }
    const PluginInfo *info = info_fn();
    if (!info) {
        MF_ERROR("plugin '%s' returned no info", path.c_str());
        close_library(handle);
        return false;
    }
    // CHECKED BEFORE ANYTHING ELSE IS TOUCHED. A plugin built against
    // a different engine may have a different idea of what every
    // pointer below means, and calling into it would be a crash with
    // no explanation.
    if (info->abi != MANIFOLD_PLUGIN_ABI) {
        MF_ERROR("plugin '%s' was built for ABI %u; this engine is ABI %u",
                 info->name ? info->name : path.c_str(), info->abi,
                 uint32_t(MANIFOLD_PLUGIN_ABI));
        close_library(handle);
        return false;
    }

    LoadedPlugin p;
    p.path = path;
    p.directory = std::filesystem::path(path).parent_path().string();
    p.handle = handle;
    p.info = info;
    p.init = (bool (*)(const PluginContext *))symbol(handle, "mf_plugin_init");
    p.shutdown = (void (*)())symbol(handle, "mf_plugin_shutdown");
    p.frame = (void (*)(float))symbol(handle, "mf_plugin_frame");
    if (!p.init) {
        MF_ERROR("plugin '%s' has no mf_plugin_init", info->name);
        close_library(handle);
        return false;
    }
    plugins_.push_back(p);
    return start(plugins_.back(), ctx);
}

bool PluginHost::start(LoadedPlugin &p, const PluginContext &ctx) {
    // Its own requirements first, and only ones already loaded: a
    // cycle would otherwise be an infinite recursion rather than a
    // message.
    if (p.info->requires_plugins) {
        for (const char *const *r = p.info->requires_plugins; *r; r++) {
            const LoadedPlugin *dep = find(*r);
            if (!dep || !dep->started) {
                MF_ERROR("plugin '%s' needs '%s', which is not loaded",
                         p.info->name, *r);
                return false;
            }
        }
    }
    PluginContext local = ctx;
    local.directory = p.directory.c_str();
    if (!p.init(&local)) {
        MF_ERROR("plugin '%s' refused to start", p.info->name);
        return false;
    }
    p.started = true;
    MF_INFO("plugin: %s %s -- %s", p.info->name, p.info->version,
            p.info->description ? p.info->description : "");
    return true;
}

int PluginHost::load_directory(const std::string &dir, const PluginContext &ctx) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;

    std::vector<std::string> candidates;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != library_extension()) continue;
        candidates.push_back(entry.path().string());
    }
    // Alphabetical, so a load order is at least reproducible; real
    // ordering comes from `requires_plugins`, and a plugin whose
    // dependency has not loaded yet is retried below.
    std::sort(candidates.begin(), candidates.end());

    int started = 0;
    std::vector<std::string> deferred;
    for (const std::string &path : candidates) {
        size_t before = plugins_.size();
        if (load(path, ctx)) {
            started++;
        } else if (plugins_.size() > before && !plugins_.back().started) {
            // Failed on a dependency rather than on loading; try again
            // once everything else is up.
            deferred.push_back(path);
            if (plugins_.back().handle) close_library(plugins_.back().handle);
            plugins_.pop_back();
        }
    }
    for (const std::string &path : deferred)
        if (load(path, ctx)) started++;
    return started;
}

void PluginHost::frame(float dt) {
    for (LoadedPlugin &p : plugins_)
        if (p.started && p.frame) p.frame(dt);
}

void PluginHost::unload_all() {
    // Reverse order, so a plugin is never shut down while something
    // that depends on it is still running.
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        if (it->started && it->shutdown) it->shutdown();
        it->started = false;
    }
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it)
        if (it->handle) close_library(it->handle);
    plugins_.clear();
}

const LoadedPlugin *PluginHost::find(const std::string &name) const {
    for (const LoadedPlugin &p : plugins_)
        if (p.info && name == p.info->name) return &p;
    return nullptr;
}

std::string PluginHost::report() const {
    std::string s = "plugins:";
    if (plugins_.empty()) return s + " none";
    for (const LoadedPlugin &p : plugins_) {
        s += "\n  ";
        s += p.info->name;
        s += " ";
        s += p.info->version;
        s += p.started ? "" : "  (failed)";
    }
    return s;
}

std::string PluginHost::default_directory() {
    char *base = SDL_GetBasePath();
    std::filesystem::path root = base ? base : ".";
    if (base) SDL_free(base);
    return (root / "plugins").string();
}

}  // namespace mf
