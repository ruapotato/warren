// Manifold voxel -- the plugin entry points.
#include "plugin/plugin.h"

#include "app/engine.h"
#include "core/log.h"
#include "terrain.h"

namespace mf::voxel {
void voxel_set_services(PhysicsWorld *physics, rhi::Device *device);
}

MF_PLUGIN_DECLARE("voxel", "0.1.0", "Manifold",
                  "Signed-distance voxel terrain: dual contouring, threaded "
                  "streaming, runtime digging")

MF_PLUGIN_EXPORT bool mf_plugin_init(const mf::PluginContext *ctx) {
    if (!ctx) return false;
    mf::voxel::voxel_set_services(ctx->physics, ctx->device);
    // Registering here rather than through a static initialiser: the
    // engine's ClassDB has already run its registrars by the time a
    // plugin loads, so a late one has to say so itself.
    mf::ClassDB::register_all();
    MF_INFO("voxel: VoxelTerrain3D registered");
    return true;
}

MF_PLUGIN_EXPORT void mf_plugin_shutdown() {
    mf::voxel::voxel_set_services(nullptr, nullptr);
}
