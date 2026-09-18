// Warren voxel -- the plugin entry points.
#include "plugin/plugin.h"

#include "app/engine.h"
#include "core/log.h"
#include "terrain.h"

namespace wr::voxel {
void voxel_set_services(PhysicsWorld *physics, rhi::Device *device);
}

WR_PLUGIN_DECLARE("voxel", "0.1.0", "Warren",
                  "Signed-distance voxel terrain: dual contouring, threaded "
                  "streaming, runtime digging")

WR_PLUGIN_EXPORT bool mf_plugin_init(const wr::PluginContext *ctx) {
    if (!ctx) return false;
    wr::voxel::voxel_set_services(ctx->physics, ctx->device);
    // Registering here rather than through a static initialiser: the
    // engine's ClassDB has already run its registrars by the time a
    // plugin loads, so a late one has to say so itself.
    wr::ClassDB::register_all();
    WR_INFO("voxel: VoxelTerrain3D registered");
    return true;
}

WR_PLUGIN_EXPORT void mf_plugin_shutdown() {
    wr::voxel::voxel_set_services(nullptr, nullptr);
}
