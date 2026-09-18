// Warren -- images on the GPU.
#pragma once

#include <string>
#include <vector>

#include "core/object.h"
#include "resource/resource.h"
#include "rhi/rhi.h"

namespace wr {

class Texture : public Resource {
    WR_CLASS(Texture, Resource)

public:
    Texture() = default;
    ~Texture() override;

    // --- making one -----------------------------------------------------
    // From a file. PNG, JPEG, TGA, BMP, HDR and a few others, via stb.
    // `srgb` should be true for anything a human picked a colour for
    // (albedo, emissive) and false for data (normals, roughness,
    // masks) -- getting it wrong on a normal map is a subtle,
    // persistent lighting error.
    static Ref<Texture> load(rhi::Device *dev, const std::string &path,
                             bool srgb = true, bool mips = true);
    // From memory, same rules.
    static Ref<Texture> from_memory(rhi::Device *dev, const void *data, size_t size,
                                    bool srgb = true, bool mips = true,
                                    const char *name = nullptr);
    // From raw pixels the caller already has.
    static Ref<Texture> from_pixels(rhi::Device *dev, const void *pixels,
                                    uint32_t w, uint32_t h, rhi::Format format,
                                    bool mips = true, const char *name = nullptr);
    // A single colour, one pixel. The defaults the material system
    // falls back to, so a shader never samples an unbound texture.
    static Ref<Texture> solid(rhi::Device *dev, const Color &c, bool srgb = true,
                              const char *name = nullptr);

    // The four every material needs when it has no map of its own.
    // Created once per device, kept alive by the device's cache.
    static Texture *white(rhi::Device *dev);
    static Texture *black(rhi::Device *dev);
    static Texture *flat_normal(rhi::Device *dev);   // (0.5, 0.5, 1)
    static Texture *default_orm(rhi::Device *dev);   // white: no AO, full rough
    static void release_defaults(rhi::Device *dev);

    // --- using one --------------------------------------------------------
    rhi::TextureH handle() const { return texture_; }
    rhi::SamplerH sampler() const { return sampler_; }
    void set_sampler(rhi::SamplerH s) { sampler_ = s; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    rhi::Format format() const { return format_; }
    const std::string &path() const { return path_; }
    bool valid() const { return texture_.valid(); }

    // WHAT IT WAS DECODED FROM, kept, for textures that came from
    // memory rather than from a file.
    //
    // A texture embedded in a .glb has no path of its own, so a
    // scene that refers to it cannot refer to it BY path -- and
    // before this the scene format silently dropped it, which
    // turned every imported model white the moment it was packed.
    // Holding the encoded bytes costs a couple of hundred kilobytes
    // per texture and makes a texture a resource that can be
    // written out like any other.
    //
    // Empty for a texture loaded from a file: that one has a path,
    // which is smaller and better.
    const std::vector<uint8_t> &source() const { return source_; }
    bool source_srgb() const { return source_srgb_; }
    void keep_source(const void *data, size_t size, bool srgb) {
        source_.assign((const uint8_t *)data, (const uint8_t *)data + size);
        source_srgb_ = srgb;
    }

    void release();

private:
    rhi::Device *device_ = nullptr;
    rhi::TextureH texture_;
    rhi::SamplerH sampler_;
    uint32_t width_ = 0, height_ = 0;
    rhi::Format format_ = rhi::Format::RGBA8;
    std::string path_;
    std::vector<uint8_t> source_;
    bool source_srgb_ = true;
    bool owns_sampler_ = false;
};

// The samplers the engine reuses, so a thousand materials do not make a
// thousand identical sampler objects.
struct SamplerCache {
    static rhi::SamplerH linear_repeat(rhi::Device *dev);
    static rhi::SamplerH linear_clamp(rhi::Device *dev);
    static rhi::SamplerH nearest_clamp(rhi::Device *dev);
    static rhi::SamplerH aniso_repeat(rhi::Device *dev);
    // Comparison sampler for shadow maps. GREATER_EQUAL, because the
    // whole engine is reverse-Z.
    static rhi::SamplerH shadow(rhi::Device *dev);
    static void release(rhi::Device *dev);
};

}  // namespace wr
