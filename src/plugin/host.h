// Manifold -- loading plugins.
#pragma once

#include <string>
#include <vector>

#include "plugin/plugin.h"

namespace mf {

struct LoadedPlugin {
    std::string path;
    std::string directory;
    const PluginInfo *info = nullptr;
    void *handle = nullptr;
    bool (*init)(const PluginContext *) = nullptr;
    void (*shutdown)() = nullptr;
    void (*frame)(float) = nullptr;
    bool started = false;
};

class PluginHost {
public:
    ~PluginHost();

    // Every shared library in a directory, in dependency order.
    // Returns how many started. Failures are logged with the reason
    // and skipped; one bad plugin does not stop the others.
    int load_directory(const std::string &dir, const PluginContext &ctx);
    bool load(const std::string &path, const PluginContext &ctx);
    void unload_all();

    void frame(float dt);

    const std::vector<LoadedPlugin> &plugins() const { return plugins_; }
    const LoadedPlugin *find(const std::string &name) const;
    std::string report() const;

    // The extension a plugin has on this platform.
    static const char *library_extension();

private:
    bool start(LoadedPlugin &p, const PluginContext &ctx);
    std::vector<LoadedPlugin> plugins_;
};

}  // namespace mf
