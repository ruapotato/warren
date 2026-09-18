#include "resource/resource.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "core/bind.h"
#include "core/log.h"

namespace mf {
namespace {

struct Registry {
    std::vector<std::pair<std::vector<std::string>, ResourceLoader::Loader>>
        loaders;
    std::vector<std::pair<std::vector<std::string>, ResourceSaver::Saver>>
        savers;
    // WEAK, DELIBERATELY. The cache must not be the reason a mesh
    // stays in memory after the last scene using it is gone; it is
    // there so two references to one path are one object, not so
    // that everything ever loaded is kept for ever.
    std::unordered_map<std::string, Object *> cache;
    std::string base = ".";
    std::mutex lock;
};

Registry &registry() {
    static Registry r;
    return r;
}

}  // namespace

Resource::~Resource() { ResourceLoader::_forget_object(this); }

std::string Resource::resource_name() const {
    if (!resource_name_.empty()) return resource_name_;
    const std::string &p = resource_path();
    if (p.empty()) return "<untitled>";
    return std::filesystem::path(p).stem().string();
}

// ------------------------------------------------------------- loading

std::string ResourceLoader::extension_of(const std::string &path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

void ResourceLoader::set_base_directory(const std::string &dir) {
    std::lock_guard<std::mutex> g(registry().lock);
    registry().base = dir.empty() ? "." : dir;
}

const std::string &ResourceLoader::base_directory() {
    return registry().base;
}

std::string ResourceLoader::resolve(const std::string &path) {
    if (path.empty()) return path;
    std::filesystem::path p(path);
    if (p.is_absolute()) return path;
    return (std::filesystem::path(registry().base) / p).string();
}

void ResourceLoader::register_loader(const std::vector<std::string> &exts,
                                     Loader loader) {
    if (!loader) return;
    std::lock_guard<std::mutex> g(registry().lock);
    registry().loaders.emplace_back(exts, std::move(loader));
}

Ref<Resource> ResourceLoader::cached(const std::string &path) {
    std::lock_guard<std::mutex> g(registry().lock);
    auto it = registry().cache.find(path);
    if (it == registry().cache.end()) return {};
    Resource *r = it->second ? it->second->cast_to<Resource>() : nullptr;
    return Ref<Resource>(r);
}

bool ResourceLoader::exists(const std::string &path) {
    std::error_code ec;
    return std::filesystem::exists(resolve(path), ec);
}

Ref<Resource> ResourceLoader::load(const std::string &path,
                                   const std::string &type_hint) {
    (void)type_hint;
    if (path.empty()) return {};
    if (Ref<Resource> hit = cached(path)) return hit;

    const std::string ext = extension_of(path);
    Loader chosen;
    {
        std::lock_guard<std::mutex> g(registry().lock);
        for (const auto &entry : registry().loaders)
            for (const std::string &e : entry.first)
                if (e == ext) {
                    chosen = entry.second;
                    break;
                }
    }
    if (!chosen) {
        MF_ERROR("resource: nothing can read '%s' (extension '%s')",
                 path.c_str(), ext.c_str());
        return {};
    }

    // LOADED OUTSIDE THE LOCK. A loader may itself load -- a scene
    // pulls in meshes, a material pulls in textures -- and holding
    // the registry's lock through that is a deadlock waiting for
    // the first nested reference.
    Ref<Resource> r = chosen(resolve(path));
    if (!r) return {};
    r->set_resource_path(path);
    {
        std::lock_guard<std::mutex> g(registry().lock);
        // Someone else may have finished first; theirs wins, so
        // that "the same path is the same object" survives a race.
        auto it = registry().cache.find(path);
        if (it != registry().cache.end() && it->second) {
            Resource *existing = it->second->cast_to<Resource>();
            if (existing) return Ref<Resource>(existing);
        }
        registry().cache[path] = r.get();
    }
    return r;
}

void ResourceLoader::_forget_object(Object *o) {
    if (!o) return;
    std::lock_guard<std::mutex> g(registry().lock);
    for (auto it = registry().cache.begin(); it != registry().cache.end();) {
        if (it->second == o)
            it = registry().cache.erase(it);
        else
            ++it;
    }
}

void ResourceLoader::forget(const std::string &path) {
    std::lock_guard<std::mutex> g(registry().lock);
    registry().cache.erase(path);
}

void ResourceLoader::forget_all() {
    std::lock_guard<std::mutex> g(registry().lock);
    registry().cache.clear();
}

size_t ResourceLoader::cached_count() {
    std::lock_guard<std::mutex> g(registry().lock);
    return registry().cache.size();
}

// -------------------------------------------------------------- saving

void ResourceSaver::register_saver(const std::vector<std::string> &exts,
                                   Saver saver) {
    if (!saver) return;
    std::lock_guard<std::mutex> g(registry().lock);
    registry().savers.emplace_back(exts, std::move(saver));
}

bool ResourceSaver::save(Resource *r, const std::string &path) {
    if (!r || path.empty()) return false;
    const std::string ext = ResourceLoader::extension_of(path);
    Saver chosen;
    {
        std::lock_guard<std::mutex> g(registry().lock);
        for (const auto &entry : registry().savers)
            for (const std::string &e : entry.first)
                if (e == ext) {
                    chosen = entry.second;
                    break;
                }
    }
    if (!chosen) {
        MF_ERROR("resource: nothing can write '%s'", path.c_str());
        return false;
    }
    if (!chosen(r, ResourceLoader::resolve(path))) return false;
    r->set_resource_path(path);
    return true;
}

static void register_resource() {
    ClassBuilder<Resource>(false)
        .prop("resource_path", &Resource::resource_path,
              &Resource::set_resource_path)
        .prop("resource_name", &Resource::resource_name,
              &Resource::set_resource_name);
}
MF_REGISTER(register_resource)

}  // namespace mf
