// Warren -- loading plugins.
#pragma once

#include <string>
#include <vector>

#include "plugin/plugin.h"

namespace wr {

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

    // BESIDE THE EXECUTABLE, not beside the shell. A game is launched
    // from a shortcut, a debugger or a package manager, and none of
    // them set the working directory to where the binary lives. An
    // empty configured directory means "here", and this is where
    // "here" is -- for everyone who asks, so they cannot disagree.
    static std::string default_directory();
    // The configured directory, resolved: empty becomes the default.
    static std::string resolve_directory(const std::string &configured) {
        return configured.empty() ? default_directory() : configured;
    }
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

}  // namespace wr
