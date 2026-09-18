#include "rhi.h"

#include <cstring>

#include "core/log.h"

namespace mf::rhi {

const char *backend_name(Backend b) {
    switch (b) {
        case Backend::Vulkan: return "Vulkan";
        case Backend::OpenGL: return "OpenGL";
    }
    return "?";
}

struct FormatEntry {
    Format f;
    const char *name;
    uint32_t block;  // bytes per pixel, or per 4x4 block for compressed
    bool depth, stencil, srgb;
};

static const FormatEntry k_formats[] = {
    {Format::Undefined, "undefined", 0, false, false, false},
    {Format::R8, "R8", 1, false, false, false},
    {Format::RG8, "RG8", 2, false, false, false},
    {Format::RGBA8, "RGBA8", 4, false, false, false},
    {Format::RGBA8_SRGB, "RGBA8_SRGB", 4, false, false, true},
    {Format::BGRA8, "BGRA8", 4, false, false, false},
    {Format::BGRA8_SRGB, "BGRA8_SRGB", 4, false, false, true},
    {Format::R16F, "R16F", 2, false, false, false},
    {Format::RG16F, "RG16F", 4, false, false, false},
    {Format::RGBA16F, "RGBA16F", 8, false, false, false},
    {Format::R32F, "R32F", 4, false, false, false},
    {Format::RG32F, "RG32F", 8, false, false, false},
    {Format::RGB32F, "RGB32F", 12, false, false, false},
    {Format::RGBA32F, "RGBA32F", 16, false, false, false},
    {Format::RGB10A2, "RGB10A2", 4, false, false, false},
    {Format::RG11B10F, "RG11B10F", 4, false, false, false},
    {Format::R8UI, "R8UI", 1, false, false, false},
    {Format::R16UI, "R16UI", 2, false, false, false},
    {Format::R32UI, "R32UI", 4, false, false, false},
    {Format::D32F_S8, "D32F_S8", 8, true, true, false},
    {Format::D32F, "D32F", 4, true, false, false},
    {Format::D24_S8, "D24_S8", 4, true, true, false},
    {Format::BC1, "BC1", 8, false, false, false},
    {Format::BC3, "BC3", 16, false, false, false},
    {Format::BC5, "BC5", 16, false, false, false},
    {Format::BC7, "BC7", 16, false, false, false},
    {Format::BC7_SRGB, "BC7_SRGB", 16, false, false, true},
};

static const FormatEntry &entry(Format f) {
    for (const FormatEntry &e : k_formats)
        if (e.f == f) return e;
    return k_formats[0];
}

bool format_is_depth(Format f) { return entry(f).depth; }
bool format_has_stencil(Format f) { return entry(f).stencil; }
bool format_is_srgb(Format f) { return entry(f).srgb; }
uint32_t format_block_size(Format f) { return entry(f).block; }
const char *format_name(Format f) { return entry(f).name; }

// Defined by each backend's translation unit; null when that backend
// was compiled out.
Device *create_gl_device(const DeviceDesc &d);
Device *create_vulkan_device(const DeviceDesc &d);

std::vector<Backend> available_backends() {
    std::vector<Backend> v;
#if MANIFOLD_VULKAN
    v.push_back(Backend::Vulkan);
#endif
#if MANIFOLD_OPENGL
    v.push_back(Backend::OpenGL);
#endif
    return v;
}

Device *create_device(const DeviceDesc &desc) {
    Device *d = nullptr;
    switch (desc.backend) {
        case Backend::Vulkan:
#if MANIFOLD_VULKAN
            d = create_vulkan_device(desc);
#else
            MF_ERROR("rhi: this build has no Vulkan backend");
#endif
            break;
        case Backend::OpenGL:
#if MANIFOLD_OPENGL
            d = create_gl_device(desc);
#else
            MF_ERROR("rhi: this build has no OpenGL backend");
#endif
            break;
    }
    if (!d) {
        // Deliberately NOT falling back to the other backend. An engine
        // that silently changes renderer produces bug reports that
        // cannot be reproduced, and a performance mystery that takes a
        // week to trace to a driver that failed to initialise.
        MF_ERROR("rhi: could not create a %s device", backend_name(desc.backend));
        return nullptr;
    }
    const DeviceCaps &c = d->caps();
    MF_INFO("rhi: %s on %s", backend_name(c.backend), c.device_name.c_str());
    MF_INFO("     %s, %s", c.api_version.c_str(), c.driver_info.c_str());
    if (c.stencil_bits < 8) {
        MF_FATAL("rhi: %u stencil bits; portals need 8", c.stencil_bits);
        destroy_device(d);
        return nullptr;
    }
    return d;
}

void destroy_device(Device *d) {
    if (!d) return;
    d->wait_idle();
    delete d;
}

}  // namespace mf::rhi
