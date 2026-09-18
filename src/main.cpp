// Manifold -- the runtime.
//
//   manifold --demo portals          two rooms, a mismatched pair
//   manifold --backend gl            force a backend
//   manifold --shot out.png --frames 60
#include <cstring>
#include <string>

#include "app/engine.h"
#include "core/log.h"
#include "scene/nodes.h"
#include "scene/portal.h"

using namespace mf;

namespace {

// A free camera, so there is something to fly around with before the
// character controller lands. Written as a C++ ScriptInstance, which
// is the same interface the Python bindings will implement.
class FlyCamera : public ScriptInstance {
public:
    explicit FlyCamera(Camera3D *cam, Window *win) : cam_(cam), win_(win) {}

    const char *script_name() const override { return "FlyCamera"; }

    void on_process(float dt) override {
        if (!cam_ || !win_) return;
        if (Input::mouse_just_pressed(MouseButton::Right))
            win_->set_mouse_captured(true);
        if (Input::key_just_pressed(Key::Escape)) win_->set_mouse_captured(false);

        if (win_->mouse_captured()) {
            Vec2 m = Input::mouse_motion();
            yaw_ -= m.x * 0.0025f;
            pitch_ = clampf(pitch_ - m.y * 0.0025f, -1.5f, 1.5f);
        }
        cam_->set_rotation(Quat::from_euler_yxz(yaw_, pitch_, 0.0f));

        Vec3 wish;
        if (Input::key_down(Key::W)) wish += cam_->forward();
        if (Input::key_down(Key::S)) wish -= cam_->forward();
        if (Input::key_down(Key::D)) wish += cam_->right();
        if (Input::key_down(Key::A)) wish -= cam_->right();
        if (Input::key_down(Key::E) || Input::key_down(Key::Space))
            wish += Vec3::up();
        if (Input::key_down(Key::Q) || Input::key_down(Key::LeftCtrl))
            wish -= Vec3::up();
        if (wish.length_sq() > 0.0f) wish = wish.normalized();
        float speed = Input::key_down(Key::LeftShift) ? 14.0f : 4.5f;
        velocity_ = lerp(velocity_, wish * speed, clampf(dt * 12.0f, 0.0f, 1.0f));
        cam_->translate(velocity_ * dt);
    }

private:
    Camera3D *cam_;
    Window *win_;
    float yaw_ = 0.0f, pitch_ = 0.0f;
    Vec3 velocity_;
};

MeshInstance3D *add_mesh(Node *parent, const char *name, Ref<Mesh> mesh,
                         Ref<Material> mat, const Vec3 &position,
                         const Quat &rotation = Quat()) {
    MeshInstance3D *mi = new MeshInstance3D();
    mi->set_name(name);
    mi->mesh = mesh;
    mi->set_material(0, mat.get());
    mi->set_position(position);
    mi->set_rotation(rotation);
    parent->add_child(mi);
    return mi;
}

// A room: floor, ceiling and four walls, built from boxes so that
// every surface is solid from both sides.
void build_room(Node *parent, const Vec3 &centre, const Vec3 &size,
                Ref<Material> floor_mat, Ref<Material> wall_mat,
                bool open_north = false) {
    const float t = 0.25f;
    Vec3 h = size * 0.5f;
    add_mesh(parent, "floor", Mesh::box({size.x, t, size.z}), floor_mat,
             centre + Vec3(0, -h.y, 0));
    add_mesh(parent, "ceiling", Mesh::box({size.x, t, size.z}), wall_mat,
             centre + Vec3(0, h.y, 0));
    add_mesh(parent, "wall_w", Mesh::box({t, size.y, size.z}), wall_mat,
             centre + Vec3(-h.x, 0, 0));
    add_mesh(parent, "wall_e", Mesh::box({t, size.y, size.z}), wall_mat,
             centre + Vec3(h.x, 0, 0));
    add_mesh(parent, "wall_s", Mesh::box({size.x, size.y, t}), wall_mat,
             centre + Vec3(0, 0, h.z));
    if (!open_north)
        add_mesh(parent, "wall_n", Mesh::box({size.x, size.y, t}), wall_mat,
                 centre + Vec3(0, 0, -h.z));
}

// TWO ROOMS, ONE SMALL AND ONE LARGE, AND A PORTAL PAIR BETWEEN THEM
// WHOSE ENDS ARE DIFFERENT SIZES.
//
// This is the demo because it is the thing the engine is for. The
// small room is furnished at half scale and the large one at double,
// and the portal between them is sized to match each end -- so looking
// through the small portal into the large room shows a world that is
// correctly, consistently four times the size, with no seam, at full
// resolution, anti-aliased with everything around it.
void build_portal_demo(Engine &e) {
    SceneTree *tree = e.tree();
    rhi::Device *dev = e.device();
    (void)dev;

    Node *scene = new Node();
    scene->set_name("PortalDemo");
    tree->set_scene(scene);

    auto mat = [](const Color &c, float rough, float metal = 0.0f) {
        return Material::make(c, rough, metal);
    };
    Ref<Material> grey = mat(Color::hex(0x9AA0A6), 0.85f);
    Ref<Material> warm = mat(Color::hex(0xC9A227), 0.6f);
    Ref<Material> cool = mat(Color::hex(0x3F7CAC), 0.5f);
    Ref<Material> red = mat(Color::hex(0xC8503C), 0.45f);
    Ref<Material> pale = mat(Color::hex(0xE8E3D9), 0.9f);
    Ref<Material> dark = mat(Color::hex(0x35393F), 0.7f);
    Ref<Material> chrome = mat(Color::hex(0xD8DEE9), 0.15f, 1.0f);

    // --- the small room, at 1x -------------------------------------
    Node *small = new Node();
    small->set_name("SmallRoom");
    scene->add_child(small);
    build_room(small, {0, 2, 0}, {10, 4, 10}, pale, grey);
    add_mesh(small, "crate", Mesh::box({1, 1, 1}), warm, {-2.5f, 0.5f, -2.0f});
    add_mesh(small, "crate2", Mesh::box({0.7f, 0.7f, 0.7f}), warm,
             {-2.6f, 1.35f, -2.3f}, Quat::from_axis_angle(Vec3::up(), 0.6f));
    add_mesh(small, "ball", Mesh::sphere(0.45f), chrome, {2.2f, 0.45f, -1.6f});
    add_mesh(small, "pillar", Mesh::cylinder(0.3f, 3.0f), dark, {3.2f, 1.5f, 3.0f});
    add_mesh(small, "ring", Mesh::torus(0.6f, 0.12f), red, {3.1f, 1.0f, 1.4f},
             Quat::from_axis_angle(Vec3::right(), PI * 0.5f));

    // --- the large room, forty metres away and at 4x ---------------
    const Vec3 far_centre(60, 8, 0);
    Node *large = new Node();
    large->set_name("LargeRoom");
    scene->add_child(large);
    build_room(large, far_centre, {40, 16, 40}, dark, cool);
    add_mesh(large, "crate", Mesh::box({4, 4, 4}), warm,
             far_centre + Vec3(-10, -6, -8));
    add_mesh(large, "crate2", Mesh::box({2.8f, 2.8f, 2.8f}), warm,
             far_centre + Vec3(-10.4f, -2.6f, -9.2f),
             Quat::from_axis_angle(Vec3::up(), 0.6f));
    add_mesh(large, "ball", Mesh::sphere(1.8f), chrome,
             far_centre + Vec3(8, -6.2f, -4));
    add_mesh(large, "pillar", Mesh::cylinder(1.2f, 12.0f), grey,
             far_centre + Vec3(12.8f, -2, 12));
    add_mesh(large, "ring", Mesh::torus(2.4f, 0.48f), red,
             far_centre + Vec3(4.8f, -3.2f, 10.4f),
             Quat::from_axis_angle(Vec3::right(), PI * 0.5f));

    // --- the pair --------------------------------------------------
    //
    // A 1.6m doorway in the small room, wired to a 6.4m arch in the
    // large one: a ratio of exactly four. Walk through and you come
    // out four times the size, which is why the furniture on the far
    // side is built at four times the scale -- it should look
    // identical from the other end.
    Portal3D *a = new Portal3D();
    a->set_name("PortalA");
    a->width = 1.6f;
    a->height = 2.6f;
    a->edge_colour = Color::hex(0xFF8C1A);
    a->set_position({0, 1.3f, -4.85f});
    // Facing +Z, back into the room, so the player walking north sees
    // its front.
    a->set_rotation(Quat());
    small->add_child(a);

    Portal3D *b = new Portal3D();
    b->set_name("PortalB");
    b->width = 6.4f;
    b->height = 10.4f;
    b->edge_colour = Color::hex(0x2FA8FF);
    b->set_position(far_centre + Vec3(0, -2.8f, 19.4f));
    b->set_rotation(Quat::from_axis_angle(Vec3::up(), PI));
    large->add_child(b);
    a->link_to(b);

    MF_INFO("portal pair: %.2fm and %.2fm -- walking through scales by %.2fx",
            double(a->world_width()), double(b->world_width()),
            double(Portal3D::scale_ratio(a, b)));

    // --- a second, equal pair, so recursion has something to chew on
    Portal3D *c = new Portal3D();
    c->set_name("PortalC");
    c->width = 1.6f;
    c->height = 2.6f;
    c->edge_colour = Color::hex(0x6BE36B);
    c->set_position({-4.85f, 1.3f, 0});
    c->set_rotation(Quat::from_axis_angle(Vec3::up(), PI * 0.5f));
    small->add_child(c);

    Portal3D *d = new Portal3D();
    d->set_name("PortalD");
    d->width = 1.6f;
    d->height = 2.6f;
    d->edge_colour = Color::hex(0x6BE36B);
    d->set_position({4.85f, 1.3f, 0});
    d->set_rotation(Quat::from_axis_angle(Vec3::up(), -PI * 0.5f));
    small->add_child(d);
    c->link_to(d);

    // --- the camera -------------------------------------------------
    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(70.0f);
    cam->set_near(0.05f);
    cam->set_mode(Camera3D::Mode::PerspectiveInfinite);
    cam->set_position({-0.2f, 1.65f, 3.4f});
    cam->set_rotation(Quat::from_euler_yxz(0.0f, -0.02f, 0.0f));
    scene->add_child(cam);
    cam->make_current();
    cam->set_script(new FlyCamera(cam, e.window()));

    DirectionalLight3D *sun = new DirectionalLight3D();
    sun->set_name("Sun");
    sun->set_rotation(Quat::from_euler_yxz(0.7f, -0.9f, 0.0f));
    scene->add_child(sun);

    Renderer *r = e.renderer();
    r->sun_direction = sun->direction();
    r->ambient = Color::hex(0x4F5B6B);
    r->ambient_energy = 0.7f;
    r->fog_colour = Color::hex(0x7C8A9A);
    r->fog_density = 0.008f;
}

}  // namespace

int main(int argc, char **argv) {
    EngineConfig cfg;
    cfg.window.title = "Manifold";
    cfg.window.width = 1600;
    cfg.window.height = 900;
    cfg.window.backend = rhi::Backend::Vulkan;
    std::string demo = "portals";

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *def) -> std::string {
            return (i + 1 < argc) ? argv[++i] : def;
        };
        if (a == "--backend") {
            std::string b = next("vulkan");
            cfg.window.backend = (b == "gl" || b == "opengl") ? rhi::Backend::OpenGL
                                                              : rhi::Backend::Vulkan;
        } else if (a == "--demo") {
            demo = next("portals");
        } else if (a == "--shot") {
            cfg.screenshot_path = next("shot.png");
            if (!cfg.screenshot_frame) cfg.screenshot_frame = 8;
        } else if (a == "--shot-frame") {
            cfg.screenshot_frame = uint64_t(std::stoull(next("8")));
        } else if (a == "--frames") {
            cfg.max_frames = uint64_t(std::stoull(next("0")));
        } else if (a == "--width") {
            cfg.window.width = std::stoi(next("1600"));
        } else if (a == "--height") {
            cfg.window.height = std::stoi(next("900"));
        } else if (a == "--no-vsync") {
            cfg.window.vsync = false;
        } else if (a == "--msaa") {
            cfg.render.msaa = std::stoi(next("4"));
        } else if (a == "--portal-depth") {
            cfg.render.max_portal_depth = std::stoi(next("4"));
        } else if (a == "--no-scissor") {
            cfg.render.portal_scissor = false;
        } else if (a == "--fallback") {
            cfg.allow_fallback = true;
        } else if (a == "--quiet") {
            log_set_level(LogLevel::Warn);
        } else if (a == "--verbose") {
            log_set_level(LogLevel::Debug);
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "manifold [options]\n"
                "  --backend vulkan|gl   which renderer (default vulkan)\n"
                "  --demo portals        which scene\n"
                "  --shot FILE           save a png and carry on\n"
                "  --shot-frame N        which frame to save (default 8)\n"
                "  --frames N            stop after N frames\n"
                "  --width N --height N  window size\n"
                "  --msaa N              1, 2, 4 or 8\n"
                "  --portal-depth N      recursion limit (default 4)\n"
                "  --no-scissor          disable the portal scissor (slow)\n"
                "  --no-vsync            uncapped\n"
                "  --fallback            try the other backend if this one fails\n"
                "\nRight mouse captures the cursor, escape releases it.\n"
                "WASD to move, QE or space/ctrl for up and down, shift to hurry.\n");
            return 0;
        }
    }

    Engine engine;
    engine.on_ready = [&](Engine &e) {
        if (demo == "portals") build_portal_demo(e);
        else MF_ERROR("unknown demo '%s'", demo.c_str());
    };
    double title_timer = 0.0;
    engine.on_frame = [&](Engine &e, float dt) {
        title_timer += dt;
        if (title_timer > 0.25) {
            title_timer = 0.0;
            e.window()->set_title("Manifold -- " + e.status_line());
        }
    };
    if (!engine.init(cfg)) return 1;
    int rc = engine.run();
    {
        const RenderStats &st = engine.renderer()->stats();
        MF_INFO("last frame: %u views, %u draws, %u tris, portals: %u seen / %u "
                "culled, deepest %u, %.2f ms cpu",
                st.views, st.draw_calls, st.triangles, st.portals_considered,
                st.portals_culled, st.max_depth_reached, st.cpu_ms);
    }
    MF_INFO("%s", engine.device() ? engine.device()->resource_report().c_str() : "");
    engine.shutdown();
    return rc;
}
