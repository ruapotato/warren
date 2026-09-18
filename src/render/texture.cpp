#include "texture.h"

#include <cstring>
#include <map>

#define STB_IMAGE_IMPLEMENTATION
// stdio stays on: Texture::load takes a path.
#include "stb/stb_image.h"

#include "core/log.h"

namespace wr {
namespace {

// Per-device, because two devices would otherwise share handles from
// one and free them into the other.
struct Defaults {
    Ref<Texture> white, black, normal, orm;
    rhi::SamplerH linear_repeat, linear_clamp, nearest_clamp, aniso_repeat, shadow;
};
std::map<rhi::Device *, Defaults> g_defaults;

}  // namespace

Texture::~Texture() { release(); }

void Texture::release() {
    if (!device_) return;
    if (texture_.valid()) device_->destroy(texture_);
    if (owns_sampler_ && sampler_.valid()) device_->destroy(sampler_);
    texture_ = {};
    sampler_ = {};
    device_ = nullptr;
}

Ref<Texture> Texture::from_pixels(rhi::Device *dev, const void *pixels, uint32_t w,
                                  uint32_t h, rhi::Format format, bool mips,
                                  const char *name) {
    using namespace rhi;
    if (!dev || !w || !h) return {};
    Ref<Texture> t(new Texture());
    t->device_ = dev;
    t->width_ = w;
    t->height_ = h;
    t->format_ = format;

    TextureDesc td;
    td.width = w;
    td.height = h;
    td.format = format;
    td.mips = mips ? 0 : 1;  // 0 means all the way down
    td.usage = TextureUsage::Sampled | TextureUsage::TransferDst;
    if (mips) td.usage = td.usage | TextureUsage::TransferSrc;
    td.name = name;
    t->texture_ = dev->create_texture(td, pixels);
    if (!t->texture_.valid()) return {};
    t->sampler_ = mips ? SamplerCache::aniso_repeat(dev)
                       : SamplerCache::linear_repeat(dev);
    return t;
}

Ref<Texture> Texture::from_memory(rhi::Device *dev, const void *data, size_t size,
                                  bool srgb, bool mips, const char *name) {
    int w = 0, h = 0, channels = 0;
    // Always four channels: three-channel uploads need row padding on
    // some drivers and a separate format on others, and the memory
    // saved is not worth either.
    stbi_uc *pixels = stbi_load_from_memory((const stbi_uc *)data, int(size), &w, &h,
                                            &channels, 4);
    if (!pixels) {
        WR_ERROR("texture '%s': %s", name ? name : "?", stbi_failure_reason());
        return {};
    }
    Ref<Texture> t = from_pixels(dev, pixels,
                                 uint32_t(w), uint32_t(h),
                                 srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8,
                                 mips, name);
    stbi_image_free(pixels);
    return t;
}

Ref<Texture> Texture::load(rhi::Device *dev, const std::string &path, bool srgb,
                           bool mips) {
    int w = 0, h = 0, channels = 0;
    stbi_uc *pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        WR_ERROR("could not load '%s': %s", path.c_str(), stbi_failure_reason());
        return {};
    }
    Ref<Texture> t = from_pixels(dev, pixels, uint32_t(w), uint32_t(h),
                                 srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8,
                                 mips, path.c_str());
    stbi_image_free(pixels);
    if (t) t->path_ = path;
    return t;
}

Ref<Texture> Texture::solid(rhi::Device *dev, const Color &c, bool srgb,
                            const char *name) {
    // The stored bytes are what the shader will read after the sRGB
    // decode, so a solid colour meant to arrive linear must be encoded
    // on the way in.
    auto encode = [srgb](float v) {
        if (!srgb) return uint8_t(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        float s = v <= 0.0031308f ? v * 12.92f
                                  : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return uint8_t(clampf(s, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    uint8_t px[4] = {encode(c.r), encode(c.g), encode(c.b),
                     uint8_t(clampf(c.a, 0.0f, 1.0f) * 255.0f + 0.5f)};
    Ref<Texture> t = from_pixels(dev, px, 1, 1,
                                 srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8,
                                 false, name);
    if (t) t->sampler_ = SamplerCache::linear_clamp(dev);
    return t;
}

Texture *Texture::white(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    if (!d.white) d.white = solid(dev, Color::white(), true, "white");
    return d.white.get();
}
Texture *Texture::black(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    if (!d.black) d.black = solid(dev, Color(0, 0, 0, 1), true, "black");
    return d.black.get();
}
Texture *Texture::flat_normal(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    // NOT sRGB: a normal map is data. Encoding it would bend every
    // normal towards the surface and look like a global roughness
    // change, which is a miserable thing to track down.
    if (!d.normal) d.normal = solid(dev, Color(0.5f, 0.5f, 1.0f, 1.0f), false,
                                    "flat normal");
    return d.normal.get();
}
Texture *Texture::default_orm(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    if (!d.orm) d.orm = solid(dev, Color(1, 1, 1, 1), false, "default orm");
    return d.orm.get();
}

void Texture::release_defaults(rhi::Device *dev) {
    g_defaults.erase(dev);
    SamplerCache::release(dev);
}

// -------------------------------------------------------- sampler cache

rhi::SamplerH SamplerCache::linear_repeat(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    if (!d.linear_repeat.valid()) {
        rhi::SamplerDesc s;
        s.name = "linear repeat";
        d.linear_repeat = dev->create_sampler(s);
    }
    return d.linear_repeat;
}

rhi::SamplerH SamplerCache::linear_clamp(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    if (!d.linear_clamp.valid()) {
        rhi::SamplerDesc s;
        s.address_u = s.address_v = s.address_w = rhi::AddressMode::ClampEdge;
        s.mip = rhi::MipFilter::None;
        s.name = "linear clamp";
        d.linear_clamp = dev->create_sampler(s);
    }
    return d.linear_clamp;
}

rhi::SamplerH SamplerCache::nearest_clamp(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    if (!d.nearest_clamp.valid()) {
        rhi::SamplerDesc s;
        s.min = s.mag = rhi::Filter::Nearest;
        s.mip = rhi::MipFilter::None;
        s.address_u = s.address_v = s.address_w = rhi::AddressMode::ClampEdge;
        s.name = "nearest clamp";
        d.nearest_clamp = dev->create_sampler(s);
    }
    return d.nearest_clamp;
}

rhi::SamplerH SamplerCache::aniso_repeat(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    if (!d.aniso_repeat.valid()) {
        rhi::SamplerDesc s;
        s.anisotropy = float(dev->caps().max_anisotropy > 8 ? 8
                                                            : dev->caps().max_anisotropy);
        s.name = "aniso repeat";
        d.aniso_repeat = dev->create_sampler(s);
    }
    return d.aniso_repeat;
}

rhi::SamplerH SamplerCache::shadow(rhi::Device *dev) {
    Defaults &d = g_defaults[dev];
    if (!d.shadow.valid()) {
        rhi::SamplerDesc s;
        s.compare_enable = true;
        // Reverse-Z: a fragment is lit when its depth is GREATER than
        // what the shadow map stored.
        s.compare = rhi::CompareOp::GreaterEqual;
        s.address_u = s.address_v = s.address_w = rhi::AddressMode::ClampBorder;
        // Outside the cascade must read "lit", which under reverse-Z
        // is the maximum. White border.
        s.border = rhi::BorderColour::OpaqueWhite;
        s.mip = rhi::MipFilter::None;
        s.name = "shadow";
        d.shadow = dev->create_sampler(s);
    }
    return d.shadow;
}

void SamplerCache::release(rhi::Device *dev) {
    auto it = g_defaults.find(dev);
    if (it == g_defaults.end()) return;
    Defaults &d = it->second;
    for (rhi::SamplerH *s : {&d.linear_repeat, &d.linear_clamp, &d.nearest_clamp,
                             &d.aniso_repeat, &d.shadow})
        if (s->valid()) dev->destroy(*s);
    d = Defaults();
}

static void register_texture_class() {
    ClassBuilder<Texture>(false)
        .prop_ro("width", &Texture::width)
        .prop_ro("height", &Texture::height)
        .prop_ro("path", &Texture::path)
        .prop_ro("valid", &Texture::valid);
}
WR_REGISTER(register_texture_class)

}  // namespace wr
