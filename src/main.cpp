// Warren -- the runtime.
//
//   warren --demo portals          two rooms, a mismatched pair
//   warren --backend gl            force a backend
//   warren --shot out.png --frames 60
#include <cstring>
#include <string>

#include "agent/agent.h"
#include "anim/clip.h"
#include "anim/skeleton.h"
#include "scene/animated.h"
#include "nav/debug.h"
#include "scene/nav_nodes.h"
#include "app/engine.h"
#if WARREN_PYTHON
#include "script/python.h"
#endif
#include "core/log.h"
#include "scene/audio_nodes.h"
#include "scene/nodes.h"
#include "plugin/host.h"
#include "script/stubs.h"
#include "scene/bodies.h"
#include "resource/resource.h"
#include "scene/controls.h"
#include "scene/portal.h"

using namespace wr;

namespace {

// WALKING, AND CHANGING SIZE BY WALKING.
//
// The whole point of the demo. Speeds, jump and the mouse are all in
// the character's OWN units -- `body->scaled(x)` -- so that at four
// times the size everything feels identical and only the world has
// changed proportion.
class PlayerController : public ScriptInstance {
public:
    PlayerController(CharacterBody3D *body, Camera3D *cam, Window *win,
                     PhysicsWorld *world)
        : body_(body), cam_(cam), win_(win), world_(world) {}

    const char *script_name() const override { return "PlayerController"; }

    void on_ready() override {
        if (body_) body_->set_world(world_);
    }

    void on_process(float dt) override {
        if (!body_ || !cam_ || !win_) return;
        if (Input::mouse_just_pressed(MouseButton::Right))
            win_->set_mouse_captured(true);
        if (Input::key_just_pressed(Key::Escape)) win_->set_mouse_captured(false);
        if (win_->mouse_captured()) {
            Vec2 m = Input::mouse_motion();
            yaw_ -= m.x * 0.0022f;
            pitch_ = clampf(pitch_ - m.y * 0.0022f, -1.5f, 1.5f);
        }
        // Yaw turns the body so that movement and the collider agree;
        // pitch is the camera's alone, so looking up does not tip the
        // capsule over.
        body_->set_rotation(Quat::from_axis_angle(Vec3::up(), yaw_));
        cam_->set_rotation(Quat::from_axis_angle(Vec3::right(), pitch_));
        cam_->set_position({0.0f, body_->height + body_->radius, 0.0f});
    }

    void on_physics(float dt) override {
        if (!body_ || !world_) return;
        Vec3 forward(-std::sin(yaw_), 0.0f, -std::cos(yaw_));
        Vec3 right(std::cos(yaw_), 0.0f, -std::sin(yaw_));
        Vec3 wish;
        if (Input::key_down(Key::W)) wish += forward;
        if (Input::key_down(Key::S)) wish -= forward;
        if (Input::key_down(Key::D)) wish += right;
        if (Input::key_down(Key::A)) wish -= right;
        if (wish.length_sq() > 0.0f) wish = wish.normalized();

        const bool sprint = Input::key_down(Key::LeftShift);
        // In the character's own units, so the feel survives a change
        // of scale.
        const float speed = body_->scaled(sprint ? 9.0f : 4.6f);
        const float accel = body_->scaled(body_->on_floor() ? 60.0f : 12.0f);

        Vec3 flat(body_->velocity.x, 0.0f, body_->velocity.z);
        flat = move_toward(flat, wish * speed, accel * dt);
        body_->velocity.x = flat.x;
        body_->velocity.z = flat.z;

        if (body_->on_floor() && Input::key_down(Key::Space))
            body_->velocity.y = body_->scaled(7.0f);
        if (Input::key_just_pressed(Key::R)) {
            body_->set_global_position({0.0f, 1.0f, 3.0f});
            body_->set_size(1.0f);
            body_->velocity = Vec3();
        }

        int before = body_->portals_traversed();
        body_->move_and_slide(world_, dt);
        if (body_->portals_traversed() != before)
            WR_INFO("through a portal: x%.2f, now %.2fx size (%.2fm tall)",
                    double(body_->last_portal_scale()), double(body_->get_size()),
                    double(body_->eye_height()));
    }

    float yaw() const { return yaw_; }

private:
    CharacterBody3D *body_;
    Camera3D *cam_;
    Window *win_;
    PhysicsWorld *world_;
    float yaw_ = 0.0f, pitch_ = 0.0f;
};

// A free camera, for looking at the scene from outside it.
class FlyCamera : public ScriptInstance {
public:
    // The initial angles are a parameter because this script
    // overwrites the camera's rotation every frame: a transform set
    // before attaching it survives exactly until the first tick, and
    // a demo that framed its scene carefully then finds the camera
    // staring at the horizon.
    FlyCamera(Camera3D *cam, Window *win, float yaw = 0.0f, float pitch = 0.0f)
        : cam_(cam), win_(win), yaw_(yaw), pitch_(pitch) {}

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

PhysicsWorld *g_world = nullptr;

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
    // Everything in the demo is level geometry, so everything gets a
    // collider. A real game would mark which.
    if (g_world) {
        StaticBody3D *sb = new StaticBody3D();
        sb->set_name("Collider");
        mi->add_child(sb);
        sb->build_from_mesh(g_world, mesh.get(), 1);
    }
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
    // THE CEILING DOES NOT CAST. A sealed box lit by a directional
    // sun is, correctly, pitch dark inside -- which is a true
    // rendering of a room with no windows and a useless demo of a
    // shadow system. Taking the ceiling out of the shadow pass is
    // what a level designer does here: the sun becomes the interior's
    // key light, and everything in the room still casts onto the
    // floor. Nothing about the shadow system is special-cased; one
    // material says it does not cast.
    Ref<Material> ceiling_mat = wall_mat->duplicate();
    ceiling_mat->cast_shadows = false;
    add_mesh(parent, "ceiling", Mesh::box({size.x, t, size.z}), ceiling_mat,
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
    g_world = e.physics();

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
    // Bottom edge exactly on the floor, whose surface is at 0.125.
    a->set_position({0, 0.125f + a->height * 0.5f, -4.85f});
    // Facing +Z, back into the room, so the player walking north sees
    // its front.
    a->set_rotation(Quat());
    small->add_child(a);

    Portal3D *b = new Portal3D();
    b->set_name("PortalB");
    b->width = 6.4f;
    b->height = 10.4f;
    b->edge_colour = Color::hex(0x2FA8FF);
    b->set_position(far_centre + Vec3(0, -8.0f + 0.125f + b->height * 0.5f, 19.4f));
    b->set_rotation(Quat::from_axis_angle(Vec3::up(), PI));
    large->add_child(b);
    a->link_to(b);

    WR_INFO("portal pair: %.2fm and %.2fm -- walking through scales by %.2fx",
            double(a->world_width()), double(b->world_width()),
            double(Portal3D::scale_ratio(a, b)));

    // --- a second, equal pair, so recursion has something to chew on
    Portal3D *c = new Portal3D();
    c->set_name("PortalC");
    c->width = 1.6f;
    c->height = 2.6f;
    c->edge_colour = Color::hex(0x6BE36B);
    c->set_position({-4.85f, 0.125f + c->height * 0.5f, 0});
    c->set_rotation(Quat::from_axis_angle(Vec3::up(), PI * 0.5f));
    small->add_child(c);

    Portal3D *d = new Portal3D();
    d->set_name("PortalD");
    d->width = 1.6f;
    d->height = 2.6f;
    d->edge_colour = Color::hex(0x6BE36B);
    d->set_position({4.85f, 0.125f + d->height * 0.5f, 0});
    d->set_rotation(Quat::from_axis_angle(Vec3::up(), -PI * 0.5f));
    small->add_child(d);
    c->link_to(d);

    // The physics needs to know where space is connected, or a body
    // walks into the wall the portal is cut into.
    for (Portal3D *p : {a, b, c, d}) g_world->add_portal(p);

    // --- the player ---------------------------------------------------
    CharacterBody3D *player = new CharacterBody3D();
    player->set_name("Player");
    player->radius = 0.32f;
    player->height = 1.1f;
    player->step_height = 0.45f;
    player->set_global_position({0.0f, 1.0f, 3.0f});
    scene->add_child(player);

    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(74.0f);
    cam->set_near(0.04f);
    cam->set_mode(Camera3D::Mode::PerspectiveInfinite);
    cam->set_position({0.0f, player->height + player->radius, 0.0f});
    player->add_child(cam);
    cam->make_current();
    player->set_script(new PlayerController(player, cam, e.window(), g_world));

    DirectionalLight3D *sun = new DirectionalLight3D();
    sun->set_name("Sun");
    sun->set_rotation(Quat::from_euler_yxz(0.7f, -0.9f, 0.0f));
    sun->colour = Color::hex(0xFFF3E0);
    sun->energy = 3.0f;
    scene->add_child(sun);

    // PUNCTUAL LIGHTS, ON BOTH SIDES OF THE PORTAL.
    //
    // Two in the small room and one over the arch in the large one,
    // which is the case that matters: the light in the far room is
    // culled against the PORTAL VIEW's froxel grid, not the camera's,
    // so it lights what is seen through the hole even though it is
    // forty metres behind the player.
    auto lamp = [&](Node *parent, const char *name, const Vec3 &at,
                    const Color &c, float energy, float range) {
        OmniLight3D *o = new OmniLight3D();
        o->set_name(name);
        o->set_position(at);
        o->colour = c;
        o->energy = energy;
        o->range = range;
        parent->add_child(o);
        return o;
    };
    lamp(small, "lamp_w", {-3.2f, 3.0f, 2.0f}, Color::hex(0xFFB259), 14.0f, 9.0f);
    lamp(small, "lamp_e", {3.2f, 3.0f, -2.0f}, Color::hex(0x6FA8FF), 10.0f, 8.0f);
    lamp(large, "lamp_far", far_centre + Vec3(0, 9.0f, -6.0f),
         Color::hex(0xFFD9A0), 260.0f, 34.0f);

    // And a spot, because a cone is the thing a cluster grid is worst
    // at and therefore the thing worth having in the demo.
    SpotLight3D *spot = new SpotLight3D();
    spot->set_name("spot");
    spot->set_position({0.0f, 3.6f, 3.4f});
    spot->look_at({0.0f, 0.6f, -3.0f});
    spot->colour = Color::hex(0xE8F4FF);
    spot->energy = 90.0f;
    spot->range = 18.0f;
    spot->angle = deg2rad(34.0f);
    spot->angle_softness = 0.35f;
    small->add_child(spot);

    // A HUM IN THE FAR ROOM, WHICH YOU HEAR THROUGH THE HOLE.
    //
    // Forty metres away through the wall and a few metres through
    // the arch. Walk up to the portal and it gets louder and stays
    // in front of you; walk away along the wall and it fades,
    // because the path to it goes through the aperture and the
    // aperture is behind you.
    {
        AudioPlayer3D *hum = new AudioPlayer3D();
        hum->set_name("FarHum");
        hum->clip = AudioClip::tone(110.0f, 2.0f);
        hum->loop = true;
        hum->autoplay = true;
        hum->volume = 0.6f;
        hum->max_distance = 30.0f;
        hum->reference_distance = 2.0f;
        hum->set_position(far_centre + Vec3(-12.0f, -5.0f, -6.0f));
        large->add_child(hum);
    }

    // A GAME UI, MADE OF NODES. Anchored to the screen, arranged by
    // containers, and no different from any other part of the scene
    // -- it could be saved as its own file and instanced, which is
    // the whole reason it is built this way.
    {
        Control *hud = new Control();
        hud->set_name("HUD");
        hud->set_anchors_preset(Control::Preset::FullRect);
        hud->mouse_filter = Control::MouseFilter::Ignore;
        scene->add_child(hud);

        PanelContainer *card = new PanelContainer();
        card->set_name("Card");
        card->set_anchors_preset(Control::Preset::BottomLeft);
        card->offset_left = 16;
        card->offset_top = -132;
        card->offset_right = 276;
        card->offset_bottom = -16;
        hud->add_child(card);

        VBoxContainer *rows = new VBoxContainer();
        rows->set_name("Rows");
        card->add_child(rows);

        Label *title = new Label();
        title->set_name("Title");
        title->text = "WARREN";
        title->size_flags_vertical = Control::SizeShrinkCentre;
        rows->add_child(title);

        Label *hint = new Label();
        hint->set_name("Hint");
        hint->text = "F1 editor  .  right drag looks";
        hint->use_theme_colour = false;
        hint->colour = Color::hex(0x8C929C);
        hint->size_flags_vertical = Control::SizeShrinkCentre;
        rows->add_child(hint);

        ProgressBar *bar = new ProgressBar();
        bar->set_name("Size");
        bar->min_value = 0.16f;
        bar->max_value = 6.0f;
        bar->value = 1.0f;
        bar->show_percentage = false;
        bar->size_flags_vertical = Control::SizeShrinkCentre;
        rows->add_child(bar);

        HBoxContainer *buttons = new HBoxContainer();
        buttons->set_name("Buttons");
        buttons->size_flags_vertical = Control::SizeShrinkEnd;
        rows->add_child(buttons);

        Button *shrink = new Button();
        shrink->set_name("Shrink");
        shrink->text = "smaller";
        shrink->size_flags_horizontal = Control::SizeFill | Control::SizeExpand;
        buttons->add_child(shrink);

        Button *grow = new Button();
        grow->set_name("Grow");
        grow->text = "bigger";
        grow->size_flags_horizontal = Control::SizeFill | Control::SizeExpand;
        buttons->add_child(grow);

        // Signals, wired the way a game would: the button knows
        // nothing about the player.
        CharacterBody3D *body = player;
        shrink->connect("pressed", shrink, [body](const Variant *, int) {
            if (body) body->set_size(body->get_size() * 0.8f);
        });
        grow->connect("pressed", grow, [body](const Variant *, int) {
            if (body) body->set_size(body->get_size() * 1.25f);
        });
    }

    Renderer *r = e.renderer();
    r->ambient = Color::hex(0x4F5B6B);
    r->ambient_energy = 0.7f;
    r->fog_colour = Color::hex(0x7C8A9A);
    r->fog_density = 0.008f;
    WR_INFO("%s", g_world->report().c_str());
}

// TERRAIN, BUILT ENTIRELY THROUGH REFLECTION.
//
// The voxel terrain is a plugin, so this file cannot include its
// headers or name its types -- the class does not exist until a
// shared library is loaded. Everything below goes through ClassDB by
// name, which is exactly what a script would do, and is the honest
// test of whether the plugin interface is any good.
// A BODY WITH BONES IN IT, BENDING.
//
// The smallest thing that proves the whole skinning chain end to
// end and can be looked at: a four-bone tentacle built in code,
// skinned by hand, with a clip that waves it. If the bones are
// wrong it is a straight bar; if the weights are wrong it is a
// fan of triangles; if the upload is wrong it is not there.
// Moves the quarry round the level and keeps the pack pointed at it.
// The point of the demo is that this is ALL the game has to do: set a
// target. Where the route goes, which ladder it uses, and how thirty
// bodies get through one gap without stacking are the engine's
// business.
class NavDemoDriver : public ScriptInstance {
public:
    NavDemoDriver(NavRegion3D *region, Node3D *quarry,
                  std::vector<NavAgent3D *> pack)
        : region_(region), quarry_(quarry), pack_(std::move(pack)) {}

    const char *script_name() const override { return "NavDemoDriver"; }

    void on_process(float dt) override {
        t_ += dt;
        // A circuit that goes round the buildings and up onto the
        // walkway, so the pack has to use the ladder to follow.
        // ROUND THE OUTSIDE, not through the blocks. The circuit
        // has to stay on ground the pack can reach, and snapping to
        // the nearest navmesh point is not enough to ensure that: a
        // point over a building is nearest to that building's ROOF,
        // which nothing can climb, and the demo then shows eighteen
        // bodies milling about underneath a quarry they cannot get
        // to. Correct, and not what it is here to show.
        const float lap = 30.0f;
        float u = std::fmod(t_, lap) / lap;
        Vec3 want;
        if (u < 0.6f) {
            float a = u / 0.6f * TAU;
            want = Vec3(std::cos(a) * 16.5f, 0.0f, std::sin(a) * 16.5f);
        } else {
            // Up onto the walkway and along it, so the pack has to
            // find the ladder or the stair to follow.
            float a = (u - 0.6f) / 0.4f;
            want = Vec3(12.0f - a * 24.0f, 3.1f, 15.0f);
        }
        Vec3 on = want;
        if (region_) region_->nearest_point(want, &on);
        quarry_->set_position(on);

        // Re-target a few per frame. Thirty paths a frame is a spike
        // for no benefit: the quarry has not moved far enough in a
        // sixtieth of a second to change anyone's route.
        if (pack_.empty()) return;
        for (int i = 0; i < 3; ++i) {
            cursor_ = (cursor_ + 1) % pack_.size();
            pack_[cursor_]->set_target(on);
        }

        // Say what the pack is doing, because a screenshot cannot.
        // Bodies standing still at the edge of the frame look the
        // same whether they are stuck, unable to find a route, or
        // simply the far end of a queue.
        report_ += dt;
        if (report_ < 3.0f) return;
        report_ = 0.0f;
        int close = 0, climbing = 0, stuck = 0, no_route = 0;
        for (NavAgent3D *a : pack_) {
            Vec3 d = a->global_position() - on;
            d.y = 0.0f;
            if (d.length() < 4.0f) ++close;
            if (a->on_link()) ++climbing;
            if (a->path_partial()) ++no_route;
            if (a->stuck_time() > 1.0f) ++stuck;
        }
        WR_INFO("nav demo: %zu chasing -- %d within 4 m, %d on a link, "
                "%d with no route, %d wedged",
                pack_.size(), close, climbing, no_route, stuck);
    }

private:
    NavRegion3D *region_;
    Node3D *quarry_;
    std::vector<NavAgent3D *> pack_;
    size_t cursor_ = 0;
    float t_ = 0.0f, report_ = 0.0f;
};

// A town, a walkway, a ladder, and a pack that wants you.
//
// Everything a game would do here is three lines: put a NavRegion3D
// over the level, put NavAgent3Ds in it, and call set_target. The
// bake, the routes, the ladder and the shoving are the engine's.
void build_nav_demo(Engine &e) {
    Node3D *root = new Node3D();
    root->set_name("NavDemo");
    e.tree()->root()->add_child(root);

    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_position(Vec3(-2.0f, 16.0f, 27.0f));
    root->add_child(cam);
    cam->make_current();
    cam->set_script(new FlyCamera(cam, e.window(), 0.0f, -0.46f));

    DirectionalLight3D *sun = new DirectionalLight3D();
    sun->set_name("Sun");
    sun->set_transform(Transform3D::looking_at(Vec3(8, 14, 6), Vec3::zero()));
    sun->energy = 3.0f;
    root->add_child(sun);
    e.renderer()->fog_density = 0.0009f;

    NavRegion3D *region = new NavRegion3D();
    region->set_name("Nav");
    region->settings.agent.radius = 0.4f;
    region->settings.agent.height = 1.8f;
    region->settings.agent.max_climb = 0.45f;
    region->settings.cell_size = 0.2f;
    // Keep only what can be walked to from the street. Without this
    // the bake also covers the floor inside each sealed block --
    // real walkable ground with a roof over it and no way in -- and
    // anything that spawns there stands in a building for ever.
    // On the street, not the middle of the square -- the middle of
    // the square is the fountain, and the only thing reachable from
    // the top of a fountain is the top of that fountain.
    region->settings.reachable_from = {Vec3(16.5f, 0.0f, 0.0f)};
    root->add_child(region);

    Ref<Material> stone(new Material());
    stone->albedo = Color::hex(0x4a4e55);
    stone->roughness = 0.9f;
    Ref<Material> ground_mat(new Material());
    ground_mat->albedo = Color::hex(0x2e3135);
    ground_mat->roughness = 0.95f;

    auto slab = [&](const char *name, const Vec3 &centre, const Vec3 &size,
                    const Ref<Material> &mat) {
        MeshInstance3D *mi = new MeshInstance3D();
        mi->set_name(name);
        mi->mesh = Mesh::box(size);
        mi->set_position(centre);
        mi->set_material(0, mat.get());
        region->add_child(mi);
        return mi;
    };

    slab("Ground", Vec3(0, -0.25f, 0), Vec3(44, 0.5f, 44), ground_mat);
    // Four blocks around a square, with the gaps between them as
    // streets. The watershed cuts a level like this into a region per
    // open space, which is what the region colours in the overlay
    // show.
    slab("BlockA", Vec3(-8.5f, 2.0f, -8.5f), Vec3(9, 4, 9), stone);
    slab("BlockB", Vec3(8.5f, 2.0f, -8.5f), Vec3(9, 4, 9), stone);
    slab("BlockC", Vec3(-8.5f, 2.0f, 8.5f), Vec3(9, 4, 9), stone);
    slab("BlockD", Vec3(8.5f, 3.0f, 8.5f), Vec3(9, 6, 9), stone);
    // Something in the middle of the square, so there is an obstacle
    // with open ground all round it -- the case that makes a region
    // a ring and needs its hole bridged.
    slab("Fountain", Vec3(0, 0.5f, 0), Vec3(3.2f, 1.0f, 3.2f), stone);

    // A walkway at three metres, off the ground and reachable only by
    // the ladder. Without a link this is an island: a body on it
    // could not plan a route to the street, and a body on the street
    // could not plan one up.
    slab("Walkway", Vec3(-1.5f, 2.9f, 15.0f), Vec3(31, 0.4f, 3.4f), stone);
    // A stair up at one end, so there is a way that is not the
    // ladder and the routes have something to choose between.
    //
    // Each tread rises 0.35 m, which is under the body's 0.45 m
    // climb -- go over that and the bake is right to call every
    // tread a ledge, and the stair becomes scenery.
    for (int i = 0; i < 9; ++i) {
        const float top = 0.35f * float(i + 1);
        slab("Step", Vec3(-15.5f, top * 0.5f, 9.25f + float(i) * 0.5f),
             Vec3(3.0f, top, 0.5f), stone);
    }

    NavLink3D *ladder = new NavLink3D();
    ladder->set_name("ladder");
    // Clear of the block behind it: a link whose foot is inside a
    // building has no polygon to attach to, and the only sign is
    // that nothing ever uses it.
    ladder->set_position(Vec3(9.0f, 0.0f, 13.9f));
    ladder->start = Vec3();
    ladder->end = Vec3(0.0f, 3.15f, 0.9f);
    ladder->radius = 1.2f;
    // Climbing costs more than the distance, or every route in the
    // level would rather go up a ladder than walk round.
    ladder->cost = 6.0f;
    region->add_child(ladder);

    // The quarry.
    Node3D *quarry = new Node3D();
    quarry->set_name("Quarry");
    root->add_child(quarry);
    MeshInstance3D *quarry_body = new MeshInstance3D();
    quarry_body->mesh = Mesh::box(Vec3(0.7f, 1.8f, 0.7f));
    quarry_body->set_position(Vec3(0, 0.9f, 0));
    Ref<Material> quarry_mat(new Material());
    quarry_mat->albedo = Color::hex(0xf0d060);
    quarry_mat->emissive = Color::hex(0x403000);
    quarry_body->set_material(0, quarry_mat.get());
    quarry->add_child(quarry_body);

    // The pack.
    Ref<Material> pack_mat(new Material());
    pack_mat->albedo = Color::hex(0x8a5a4a);
    pack_mat->roughness = 0.7f;
    std::vector<NavAgent3D *> pack;
    const int kPack = 18;
    for (int i = 0; i < kPack; ++i) {
        // Round the outside of the blocks, which end at 13 metres.
        // Spawning on a circle that crosses them puts bodies inside
        // buildings -- which, before the pruning above, was ground
        // they could stand on and never leave.
        float a = float(i) / float(kPack) * TAU;
        float r = 18.0f + float(i % 3) * 1.1f;
        NavAgent3D *agent = new NavAgent3D();
        agent->set_name("Shambler" + std::to_string(i));
        agent->radius = 0.4f;
        agent->height = 1.8f;
        agent->max_speed = 2.2f + float(i % 5) * 0.12f;
        agent->max_accel = 10.0f;
        // Near enough, because eighteen bodies cannot stand on one
        // point and trying is what makes a mob orbit its target.
        agent->goal_radius = 2.2f;
        agent->set_position(Vec3(std::cos(a) * r, 0.0f, std::sin(a) * r));
        region->add_child(agent);

        MeshInstance3D *body = new MeshInstance3D();
        body->mesh = Mesh::box(Vec3(0.66f, 1.8f, 0.5f));
        body->set_position(Vec3(0, 0.9f, 0));
        body->set_material(0, pack_mat.get());
        agent->add_child(body);
        pack.push_back(agent);
    }

    // The overlay. Built after the first bake, below.
    MeshInstance3D *surface = new MeshInstance3D();
    surface->set_name("NavSurface");
    surface->cast_shadows = false;
    Ref<Material> overlay(new Material());
    overlay->roughness = 1.0f;
    overlay->metallic = 0.0f;
    overlay->unlit = true;
    surface->set_material(0, overlay.get());
    root->add_child(surface);

    MeshInstance3D *edges = new MeshInstance3D();
    edges->set_name("NavEdges");
    edges->cast_shadows = false;
    edges->set_material(0, overlay.get());
    root->add_child(edges);

    MeshInstance3D *link_marks = new MeshInstance3D();
    link_marks->set_name("NavLinks");
    link_marks->cast_shadows = false;
    link_marks->set_material(0, overlay.get());
    root->add_child(link_marks);

    // Bake now rather than waiting for the first tick, so the overlay
    // has something to show and the agents have somewhere to stand on
    // frame one.
    region->bake();
    region->collect_links();
    const nav::BakeStats &st = region->stats();
    WR_INFO("nav demo: %d polys, %d verts, %d regions, %d holes bridged, "
            "%d unreachable pruned, %.0f ms",
            st.polys, st.verts, st.regions, st.merged_holes, st.pruned,
            double(st.seconds * 1000.0f));

    if (const nav::NavMesh *mesh = region->mesh()) {
        surface->mesh = nav::debug_surface(*mesh, 0.06f);
        edges->mesh = nav::debug_edges(*mesh, 0.08f);
        link_marks->mesh = nav::debug_links(*mesh);
    }

    quarry->set_script(new NavDemoDriver(region, quarry, pack));
}

void build_skin_demo(Engine &e) {
    Node3D *root = new Node3D();
    root->set_name("SkinDemo");
    e.tree()->root()->add_child(root);

    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_transform(Transform3D::looking_at(Vec3(0, 3.0f, 7.5f),
                                               Vec3(0, 2.2f, 0)));
    root->add_child(cam);
    cam->make_current();

    DirectionalLight3D *sun = new DirectionalLight3D();
    sun->set_name("Sun");
    sun->set_transform(Transform3D::looking_at(Vec3(4, 8, 6), Vec3::zero()));
    sun->energy = 3.0f;
    root->add_child(sun);
    // The default fog is tuned for a town seen down a street and
    // turns a close-up of one object into a white card.
    e.renderer()->fog_density = 0.0006f;

    // The floor, so the thing has somewhere to stand and something
    // to cast a shadow on.
    MeshInstance3D *ground = new MeshInstance3D();
    ground->set_name("Ground");
    ground->mesh = Mesh::plane(Vec2(30, 30), 1);
    Ref<Material> gm(new Material());
    gm->albedo = Color::hex(0x35383c);
    gm->roughness = 0.95f;
    ground->set_material(0, gm.get());
    root->add_child(ground);

    const int kSegments = 4;
    const float kLength = 1.1f;

    // The rig: a chain straight up, each bone a segment above the
    // last.
    Ref<Skeleton> skel(new Skeleton());
    skel->set_resource_name("tentacle");
    for (int i = 0; i < kSegments; i++)
        skel->add("seg" + std::to_string(i), i - 1,
                  Transform3D(Basis(), Vec3(0, i == 0 ? 0.35f : kLength, 0)));
    skel->compute_inverse_binds();

    // The mesh: a box per segment, every vertex of it bound rigidly
    // to that segment. Rigid rather than smooth on purpose -- a
    // smooth weight hides a wrong bone index behind a plausible
    // bulge, and this is here to show wrong bones.
    Ref<Mesh> mesh(new Mesh());
    for (int i = 0; i < kSegments; i++) {
        const float w = 0.42f - 0.06f * float(i);
        Ref<Mesh> part = Mesh::box(Vec3(w, kLength, w));
        const uint32_t base = uint32_t(mesh->vertices.size());
        for (Vertex v : part->vertices) {
            // Into the bone's own space: the box sits astride the
            // segment above its joint.
            v.position.y += kLength * 0.5f;
            v.position = skel->global_rest(i).xform(v.position);
            mesh->vertices.push_back(v);
            SkinVertex sv;
            sv.joints[0] = uint8_t(i);
            sv.weights[0] = 255;
            mesh->skin.push_back(sv);
        }
        for (uint32_t idx : part->indices) mesh->indices.push_back(base + idx);
    }
    SubMesh sm;
    sm.first_index = 0;
    sm.index_count = uint32_t(mesh->indices.size());
    sm.name = "tentacle";
    mesh->submeshes.push_back(sm);
    mesh->compute_normals();
    mesh->compute_tangents();
    mesh->compute_bounds();
    // A CHARACTER'S BOUNDS ARE ITS REST BOUNDS, and it moves outside
    // them: grown so a waving limb is not culled when the camera
    // turns away from where it was standing still.
    mesh->set_bounds(AABB(Vec3(-3, -1, -3), Vec3(3, 7, 3)));

    Skinned3D *body = new Skinned3D();
    body->set_name("Tentacle");
    body->mesh = mesh;
    body->set_skeleton(skel);
    Ref<Material> bm(new Material());
    bm->albedo = Color::hex(0x7ac0a0);
    bm->roughness = 0.45f;
    body->set_material(0, bm.get());
    root->add_child(body);

    // The clip: each segment leans a little more than the one below
    // and a beat later, which is what makes a chain read as a wave
    // rather than as a hinge.
    Ref<AnimationClip> wave(new AnimationClip());
    wave->name = "wave";
    wave->loops = true;
    for (int i = 0; i < kSegments; i++) {
        AnimationClip::BoneTrack tr;
        tr.bone_name = "seg" + std::to_string(i);
        const float lag = float(i) * 0.28f;
        const float amp = deg2rad(14.0f + 5.0f * float(i));
        for (int k = 0; k <= 24; k++) {
            const float t = float(k) / 24.0f * 2.4f;
            tr.rotation.times.push_back(t);
            const float a = std::sin((t / 2.4f) * 6.2831853f - lag) * amp;
            tr.rotation.values.push_back(
                    Quat::from_axis_angle(Vec3(0, 0, 1), a));
        }
        wave->tracks.push_back(tr);
    }
    wave->compute_duration();

    AnimationPlayer *player = new AnimationPlayer();
    player->set_name("Animation");
    player->add_clip(wave);
    body->add_child(player);
    player->play("wave", 0.0f);

    WR_INFO("skin demo: %d bones, %u vertices, one clip",
            skel->count(), mesh->vertex_count());
}

void build_terrain_demo(Engine &e) {
    SceneTree *tree = e.tree();
    g_world = e.physics();

    Node *scene = new Node();
    scene->set_name("TerrainDemo");
    tree->set_scene(scene);

    Object *obj = ClassDB::instantiate("VoxelTerrain3D");
    if (!obj) {
        WR_ERROR("the voxel plugin is not loaded; run with --plugins DIR");
        return;
    }
    Node3D *terrain = static_cast<Node3D *>(obj);
    terrain->set_name("Terrain");
    terrain->set_member("cell_size", Variant(1.0));
    terrain->set_member("chunk_resolution", Variant(int64_t(32)));
    terrain->set_member("view_distance", Variant(176.0));
    terrain->set_member("collision_distance", Variant(90.0));
    terrain->set_member("queue_per_frame", Variant(int64_t(12)));
    terrain->set_member("upload_per_frame", Variant(int64_t(6)));
    scene->add_child(terrain);
    Array seed{Variant(int64_t(20260918))};
    terrain->callv("set_seed", seed);

    // FIND SOMEWHERE FLAT TO STAND.
    //
    // Dropping the player at the origin drops them wherever the noise
    // happened to put a mountainside, and they spend the demo sliding
    // down it. A spiral outwards, sampling the ground at each step
    // and at four points around it, finds a spot level enough to walk
    // on -- which is what a real game does when it places a spawn.
    auto height_at = [&](float x, float z) {
        Array a{Variant(double(x)), Variant(double(z))};
        return float(terrain->callv("height_at", a).to_float());
    };
    Vec2 spawn(0.0f, 0.0f);
    float ground = height_at(0.0f, 0.0f);
    float best_score = 1e30f;
    for (int i = 0; i < 160; i++) {
        const float angle = float(i) * 2.39996f;   // the golden angle
        const float radius = 9.0f * std::sqrt(float(i));
        const float x = std::cos(angle) * radius;
        const float z = std::sin(angle) * radius;
        const float h = height_at(x, z);
        const float d = 2.0f;
        const float slope = std::fabs(height_at(x + d, z) - h) +
                            std::fabs(height_at(x - d, z) - h) +
                            std::fabs(height_at(x, z + d) - h) +
                            std::fabs(height_at(x, z - d) - h);
        // Flat, and not up a mountain where everything is snow.
        const float score = slope * 4.0f + std::fabs(h - 40.0f) * 0.05f;
        if (score < best_score) {
            best_score = score;
            spawn = {x, z};
            ground = h;
        }
        if (slope < 0.6f && h < 80.0f) break;
    }
    WR_INFO("terrain: spawning at (%.0f, %.1f, %.0f), flatness %.2f",
            double(spawn.x), double(ground), double(spawn.y), double(best_score));

    CharacterBody3D *player = new CharacterBody3D();
    player->set_name("Player");
    player->radius = 0.35f;
    player->height = 1.1f;
    player->step_height = 0.6f;
    // Natural ground is steeper than a corridor's; 55 degrees is
    // still a scramble but not a cliff.
    player->max_slope = deg2rad(55.0f);
    player->set_global_position({spawn.x, ground + 1.5f, spawn.y});
    scene->add_child(player);

    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(78.0f);
    cam->set_near(0.08f);
    cam->set_mode(Camera3D::Mode::PerspectiveInfinite);
    cam->set_position({0.0f, player->height + player->radius, 0.0f});
    player->add_child(cam);
    cam->make_current();
    player->set_script(new PlayerController(player, cam, e.window(), g_world));

    Array viewer{Variant(static_cast<Object *>(player))};
    terrain->callv("set_viewer", viewer);

    // Build the ground under the player's feet before the first
    // frame, so they do not spend it falling through a world that
    // has not arrived yet.
    for (int i = 0; i < 6; i++) {
        terrain->on_process(0.0f);
        terrain->callv("wait_for_chunks", {});
    }

    DirectionalLight3D *sun = new DirectionalLight3D();
    sun->set_name("Sun");
    sun->set_rotation(Quat::from_euler_yxz(0.6f, -0.62f, 0.0f));
    scene->add_child(sun);

    Renderer *r = e.renderer();
    r->sun_direction = sun->direction();
    r->sun_colour = Color::hex(0xFFF2D9);
    r->sun_energy = 3.4f;
    r->ambient = Color::hex(0x8FA8C4);
    r->ambient_energy = 0.75f;
    r->fog_colour = Color::hex(0xAFC2D6);
    r->fog_density = 0.0022f;
    r->settings().max_portal_depth = 2;
}

// A spectator camera, for the screenshot harness and for looking at
// the scene from outside it.
void build_flythrough(Engine &e) {
    build_portal_demo(e);
    if (Node *p = e.tree()->root()->find_by_class("CharacterBody3D")) {
        p->set_script(nullptr);
        if (Node *c = p->find_by_class("Camera3D")) {
            Camera3D *cam = static_cast<Camera3D *>(c);
            cam->set_script(new FlyCamera(cam, e.window()));
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
    EngineConfig cfg;
    cfg.window.title = "Warren";
    cfg.window.width = 1600;
    cfg.window.height = 900;
    cfg.window.backend = rhi::Backend::Vulkan;
    std::string demo = "portals";
    std::string stub_path;
    std::string schema_path;
    bool agent_mode = false;
    std::string shadow_dump;
    std::string save_scene_path, load_scene_path, data_directory;
    bool bench = false;

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
        } else if (a == "--fixed-step") {
            cfg.fixed_delta = std::stof(next("0.0166667"));
        } else if (a == "--real-time") {
            cfg.fixed_delta = -1.0f;
        } else if (a == "--no-vsync") {
            cfg.window.vsync = false;
        } else if (a == "--msaa") {
            cfg.render.msaa = std::stoi(next("4"));
        } else if (a == "--portal-depth") {
            cfg.render.max_portal_depth = std::stoi(next("4"));
        } else if (a == "--no-scissor") {
            cfg.render.portal_scissor = false;
        } else if (a == "--script") {
            cfg.startup_script = next("");
        } else if (a == "--script-path") {
            cfg.script_paths.push_back(next("."));
        } else if (a == "--no-sky") {
            cfg.render.draw_sky = false;
        } else if (a == "--clear") {
            cfg.render.clear_colour = Color::hex(uint32_t(
                std::stoul(next("000000"), nullptr, 16)));
        } else if (a == "--bench") {
            bench = true;
            if (!cfg.max_frames) cfg.max_frames = 600;
            // REAL TIME, NOT A FIXED STEP. A benchmark that tells the
            // engine each frame took exactly 1/60s measures nothing.
            cfg.fixed_delta = -1.0f;
            cfg.window.vsync = false;
        } else if (a == "--save-scene") {
            save_scene_path = next("scene.mfs");
        } else if (a == "--load-scene") {
            load_scene_path = next("scene.mfs");
        } else if (a == "--data") {
            data_directory = next(".");
        } else if (a == "--editor") {
            cfg.editor_visible = true;
        } else if (a == "--no-editor") {
            cfg.enable_editor = false;
        } else if (a == "--no-ibl") {
            cfg.render.image_based_lighting = false;
        } else if (a == "--no-punctual-shadows") {
            cfg.render.punctual_shadows = false;
        } else if (a == "--no-lights") {
            cfg.render.punctual_lights = false;
        } else if (a == "--clustered-views") {
            cfg.render.max_clustered_views = std::stoi(next("16"));
        } else if (a == "--no-shadows") {
            cfg.render.shadows = false;
        } else if (a == "--shadow-size") {
            cfg.render.shadow_map_size = uint32_t(std::stoul(next("2048")));
        } else if (a == "--shadow-distance") {
            cfg.render.shadow_distance = std::stof(next("120"));
        } else if (a == "--shadow-cascades") {
            cfg.render.shadow_cascades = std::stoi(next("4"));
        } else if (a == "--shadow-dump") {
            shadow_dump = next("shadows.png");
        } else if (a == "--stubs") {
            stub_path = next("warren.pyi");
        } else if (a == "--schema") {
            schema_path = next("-");
        } else if (a == "--agent") {
            agent_mode = true;
            // An agent drives the frames itself, and a window that
            // steals focus while a script is running is a nuisance;
            // but a screenshot needs a real surface, so the window
            // stays -- just out of the way, and never vsynced, since
            // vsync would cap a batch of steps at 60 a second.
            cfg.window.vsync = false;
            if (!cfg.fixed_delta) cfg.fixed_delta = 1.0f / 60.0f;
        } else if (a == "--no-python") {
            cfg.python = false;
        } else if (a == "--plugins") {
            cfg.plugin_directory = next("plugins");
        } else if (a == "--no-plugins") {
            cfg.plugin_directory = "-";
        } else if (a == "--threads") {
            cfg.worker_threads = std::stoi(next("0"));
        } else if (a == "--fallback") {
            cfg.allow_fallback = true;
        } else if (a == "--quiet") {
            log_set_level(LogLevel::Warn);
        } else if (a == "--verbose") {
            log_set_level(LogLevel::Debug);
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "warren [options]\n"
                "  --backend vulkan|gl   which renderer (default vulkan)\n"
                "  --demo portals        which scene (portals, terrain, fly, skin,\n"
                "                        nav)\n"
                "  --shot FILE           save a png and carry on\n"
                "  --shot-frame N        which frame to save (default 8)\n"
                "  --frames N            stop after N frames (fixed 1/60s step)\n"
                "  --fixed-step S        force a fixed frame time\n"
                "  --real-time           real timing even with --frames\n"
                "  --width N --height N  window size\n"
                "  --msaa N              1, 2, 4 or 8\n"
                "  --portal-depth N      recursion limit (default 4)\n"
                "  --no-scissor          disable the portal scissor (slow)\n"
                "  --no-vsync            uncapped\n"
                "  --script FILE         run a Python script once the scene exists\n"
                "  --script-path DIR     add a directory to sys.path\n"
                "  --no-python           do not start the interpreter\n"
                "  --stubs FILE          write warren.pyi and exit\n"
                "  --schema [FILE]       write the API as JSON and exit (- for stdout)\n"
                "  --agent               JSON commands on stdin, replies on stdout\n"
                "  --no-sky              flat clear instead of the sky\n"
                "  --clear RRGGBB        the clear colour, for spotting holes\n"
                "  --bench               time 600 frames and print percentiles\n"
                "  --no-lights           no punctual lights, sun only\n"
                "  --editor              start with the editor open (F1 toggles)\n"
                "  --save-scene FILE     write the scene out and carry on\n"
                "  --load-scene FILE     replace the scene: .mfs, .glb or .gltf\n"
                "  --data DIR            where relative asset paths resolve\n"
                "  --no-editor           do not build it at all\n"
                "  --no-ibl              hemisphere ambient, no environment\n"
                "  --no-punctual-shadows lights, but nothing blocks them\n"
                "  --clustered-views N   how many views get a froxel grid\n"
                "  --no-shadows          turn the shadow pass off\n"
                "  --shadow-size N       shadow map resolution (default 2048)\n"
                "  --shadow-distance M   how far shadows reach (default 120)\n"
                "  --shadow-cascades N   1 to 4 (default 4)\n"
                "  --shadow-dump FILE    save the shadow cascades as a png\n"
                "  --plugins DIR         where to look for plugins\n"
                "  --no-plugins          do not load any\n"
                "  --threads N           worker threads (0 = cores - 1)\n"
                "  --fallback            try the other backend if this one fails\n"
                "\nRight mouse captures the cursor, escape releases it.\n"
                "WASD to move, QE or space/ctrl for up and down, shift to hurry.\n");
            return 0;
        }
    }

    // --stubs is not a mode of the engine, it is a question about
    // the class registry: which classes exist, with what methods. No
    // window, no device, no frame loop -- just registration, plugins
    // (which tolerate a context with nothing in it, and register
    // their own classes), and the file.
    // The schema is the stubs' twin: the same table, written for a
    // program instead of for an editor's autocomplete, and like the
    // stubs it needs no device and no window.
    if (!schema_path.empty()) {
        // Before anything registers, because registration logs.
        if (schema_path == "-") log_reserve_stdout();
        ClassDB::register_all();
        PluginHost host;
        if (cfg.plugin_directory != "-") {
            PluginContext ctx;
            const std::string dir = PluginHost::resolve_directory(cfg.plugin_directory);
            ctx.directory = dir.c_str();
            host.load_directory(dir, ctx);
            ClassDB::register_all();
        }
        const std::string text = agent_schema("", false).to_string(2);
        bool wrote = true;
        if (schema_path == "-") {
            std::fwrite(text.data(), 1, text.size(), stdout);
            std::fputc('\n', stdout);
        } else if (FILE *f = std::fopen(schema_path.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fputc('\n', f);
            std::fclose(f);
            WR_INFO("schema: %s (%zu classes, %zu bytes)", schema_path.c_str(),
                    ClassDB::all().size(), text.size());
        } else {
            WR_ERROR("could not write %s", schema_path.c_str());
            wrote = false;
        }
        host.unload_all();
        return wrote ? 0 : 1;
    }

    if (!stub_path.empty()) {
        ClassDB::register_all();
        PluginHost host;
        if (cfg.plugin_directory != "-") {
            PluginContext ctx;
            const std::string dir = PluginHost::resolve_directory(cfg.plugin_directory);
            ctx.directory = dir.c_str();
            host.load_directory(dir, ctx);
            ClassDB::register_all();
        }
        const bool ok = write_python_stubs(stub_path);
        host.unload_all();
        return ok ? 0 : 1;
    }

    Engine engine;
    engine.on_ready = [&](Engine &e) {
        if (demo == "portals") build_portal_demo(e);
        else if (demo == "fly") build_flythrough(e);
        else if (demo == "terrain") build_terrain_demo(e);
        else if (demo == "skin") build_skin_demo(e);
        else if (demo == "nav") build_nav_demo(e);
        else WR_ERROR("unknown demo '%s'", demo.c_str());
    };
    double title_timer = 0.0;
    engine.on_frame = [&](Engine &e, float dt) {
        title_timer += dt;
        if (title_timer > 0.25) {
            title_timer = 0.0;
            std::string extra;
            if (Node *n = e.tree()->root()->find_by_class("CharacterBody3D")) {
                CharacterBody3D *b = static_cast<CharacterBody3D *>(n);
                char buf[96];
                std::snprintf(buf, sizeof(buf), "  |  size %.2fx (%.2fm)",
                              double(b->get_size()), double(b->eye_height()));
                extra = buf;
            }
            e.window()->set_title("Warren -- " + e.status_line() + extra);
        }
    };
    // An agent that asks for stats wants numbers in them; the cost
    // is a ring buffer of doubles.
    engine.record_timings(bench || agent_mode);
    if (!engine.init(cfg)) return 1;
    if (!data_directory.empty())
        ResourceLoader::set_base_directory(data_directory);
    if (!load_scene_path.empty()) engine.editor()->load_scene(load_scene_path);
    if (!save_scene_path.empty()) engine.editor()->save_scene(save_scene_path);
    if (agent_mode) {
        const int rc = agent_serve(&engine);
        engine.shutdown();
        return rc;
    }
    int rc = engine.run();
    if (bench) {
        // The first thirty frames are pipeline warm-up, the first
        // shader uploads and the first wave of terrain streaming.
        const Engine::FrameTimes t = engine.frame_times(30);
        const RenderStats &r = engine.renderer()->stats();
        std::printf(
            "\nbench  %s  %s  %ux%u  msaa %d\n"
            "  %u frames after warm-up\n"
            "  frame   mean %6.2f ms  (%5.1f fps)\n"
            "          min  %6.2f   p50 %6.2f   p95 %6.2f   p99 %6.2f   max %6.2f\n"
            "  record  mean %6.2f ms   (the renderer's own CPU time)\n"
            "  last frame: %u draws, %u tris, %u views, %u portal levels,\n"
            "              %u cascades (%u shadow draws), %u lights in %u "
            "clustered views\n"
            "  environment baked %llu time%s in the whole run\n"
            "  punctual shadow atlas: %u casting lights, %u draws, %s\n",
            rhi::backend_name(engine.device()->backend()), demo.c_str(),
            cfg.window.width, cfg.window.height, cfg.render.msaa, t.frames,
            t.mean_ms, t.mean_ms > 0 ? 1000.0 / t.mean_ms : 0.0, t.min_ms,
            t.p50_ms, t.p95_ms, t.p99_ms, t.max_ms, t.mean_cpu_ms, r.draw_calls,
            r.triangles, r.views, r.max_depth_reached, r.cascades,
            r.shadow_draws, r.lights, r.clustered_views,
            (unsigned long long)r.environment_bakes,
            r.environment_bakes == 1 ? "" : "s", r.shadow_casting_lights,
            r.punctual_shadow_draws,
            r.punctual_shadows_reused ? "reused from an earlier frame"
                                      : "rebuilt this frame");
    }
    if (!shadow_dump.empty()) engine.renderer()->dump_shadow_map(shadow_dump);
    {
        if (Node *t = engine.tree()->root()->find_by_class("VoxelTerrain3D")) {
            WR_INFO("%s", t->callv("report", {}).to_string().c_str());
            if (Node *n = engine.tree()->root()->find_by_class("CharacterBody3D")) {
                Vec3 p = static_cast<Node3D *>(n)->global_position();
                Array where{Variant(p)};
                WR_INFO("field at the player: %.3f (negative is inside rock)",
                        t->callv("distance_at", where).to_float());
            }
        }
        WR_INFO("%s", engine.physics()->report().c_str());
        if (Node *n = engine.tree()->root()->find_by_class("CharacterBody3D")) {
            CharacterBody3D *b = static_cast<CharacterBody3D *>(n);
            Vec3 p = b->global_position();
            WR_INFO("player: (%.2f %.2f %.2f) floor=%d size=%.2f crossings=%d",
                    double(p.x), double(p.y), double(p.z), int(b->on_floor()),
                    double(b->get_size()), b->portals_traversed());
        }
        const RenderStats &st = engine.renderer()->stats();
        WR_INFO("last frame: %u views, %u draws, %u tris, portals: %u seen / %u "
                "culled, deepest %u, %.2f ms cpu",
                st.views, st.draw_calls, st.triangles, st.portals_considered,
                st.portals_culled, st.max_depth_reached, st.cpu_ms);
    }
    WR_INFO("%s", engine.device() ? engine.device()->resource_report().c_str() : "");
    engine.shutdown();
    return rc;
}
