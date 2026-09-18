// Manifold -- things that live in files.
//
// A Resource is an Object that came from somewhere and can be shared:
// a mesh, a material, a texture, a sound, a scene. The distinction
// that matters is not what it contains but that TWO PLACES CAN REFER
// TO THE SAME ONE, and that a file can name it.
//
// Until now a scene wrote its meshes inline, because a pointer means
// nothing in a file and there was nothing else to write. That is
// correct and it does not scale: a level made of forty rooms, each
// referring to the same crate, is forty copies of a crate -- and
// editing the crate edits none of them. The same argument as scene
// instancing, one layer down, and it wants the same answer: a
// reference to a file, and inline only for what has no file.
//
// THE CACHE IS PART OF THE CONTRACT. Loading a path twice gives the
// same object, not two equal ones, because "the same material" is
// what a renderer batches by and what an editor edits once.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/object.h"

namespace mf {

namespace rhi { class Device; }

// Textures need a device to upload through. The Engine sets this
// once; a game with no window and no device simply cannot load one,
// which is a clearer failure than a black square.
void resource_set_device(rhi::Device *dev);
rhi::Device *resource_device();

class Resource : public Object {
    MF_CLASS(Resource, Object)

public:
    // TAKES ITSELF OUT OF THE CACHE.
    //
    // The cache is weak on purpose: it exists so that two references
    // to one path are one object, not so that everything ever loaded
    // is kept for ever. Weak means somebody has to remove the entry
    // when the object dies, and the only thing that knows is the
    // object -- the same rule the active camera, the audio listener
    // and the focused control each had to learn. A raw pointer to a
    // refcounted thing is cleared by the thing.
    ~Resource() override;

    // Where it came from. Empty means it was made in code and has
    // nowhere to be reloaded from -- which is exactly the case that
    // still has to be written inline.
    const std::string &resource_path() const { return resource_path_; }
    void set_resource_path(const std::string &p) { resource_path_ = p; }
    bool has_path() const { return !resource_path_.empty(); }

    // A name for an editor to show. Falls back to the file's stem.
    std::string resource_name() const;
    void set_resource_name(const std::string &n) { resource_name_ = n; }

private:
    std::string resource_path_;
    std::string resource_name_;
};

// Loading, with a cache and a table of loaders by extension.
class ResourceLoader {
public:
    // Returns null if nothing can read it. `type_hint` lets a caller
    // say what it expects when an extension is ambiguous.
    static Ref<Resource> load(const std::string &path,
                              const std::string &type_hint = "");
    // Already loaded, or null. Never touches the disk.
    static Ref<Resource> cached(const std::string &path);
    static bool exists(const std::string &path);

    // A loader takes a path and returns a resource, or null if it
    // cannot read that particular file even though it claimed the
    // extension.
    using Loader = std::function<Ref<Resource>(const std::string &)>;
    // Extensions without the dot, lower case.
    static void register_loader(const std::vector<std::string> &extensions,
                                Loader loader);

    // Drops the cache. A hot reload calls this for one path; a test
    // calls it for all of them, because a cached resource from an
    // earlier case is a test that passes for the wrong reason.
    static void forget(const std::string &path);
    static void forget_all();
    static size_t cached_count();
    // Called by ~Resource. Not for anything else.
    static void _forget_object(Object *o);

    // Where relative paths are resolved from. A game sets it once to
    // wherever its data lives.
    static void set_base_directory(const std::string &dir);
    static const std::string &base_directory();
    static std::string resolve(const std::string &path);

    static std::string extension_of(const std::string &path);
};

// Saving, for the formats that have a writer.
class ResourceSaver {
public:
    using Saver = std::function<bool(Resource *, const std::string &)>;
    static void register_saver(const std::vector<std::string> &extensions,
                               Saver saver);
    static bool save(Resource *r, const std::string &path);
};

}  // namespace mf
