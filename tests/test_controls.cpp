// Warren -- user interface as part of the scene.
//
// The three things a node UI has to get right, none of which a
// screenshot shows:
//
//   ANCHORS. A control's rectangle is derived from four fractions of
//   its parent and four pixel offsets, and the whole point is that
//   it is still right when the window changes size.
//
//   CONTAINERS. A box arranges its children and OVERRIDES their
//   anchors, which is what putting something in a box means. Getting
//   the leftover space wrong is invisible until two children expand.
//
//   INPUT. The topmost control under the pointer gets the click,
//   "topmost" means the last sibling because that is what draws on
//   top, and a click is the release over the thing that was pressed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "resource/packed_scene.h"
#include "scene/controls.h"
#include "scene/scene_tree.h"
#include "scene/ui_system.h"

using namespace wr;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}
bool near(float a, float b, float tol = 0.01f) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();
    std::printf("controls\n");

    // ------------------------------------------------------- anchors
    {
        SceneTree tree;
        Control *root = new Control();
        root->set_name("Root");
        root->set_anchors_preset(Control::Preset::FullRect);
        tree.set_scene(root);

        // Pinned to the top left at a fixed size.
        Control *corner = new Control();
        corner->set_name("Corner");
        corner->offset_left = 10;
        corner->offset_top = 10;
        corner->offset_right = 110;
        corner->offset_bottom = 40;
        root->add_child(corner);

        // Stretched across the bottom. THROUGH A PRESET, because
        // setting two anchors and leaving the other offsets at
        // their defaults is the mistake anchors invite: a right
        // anchor of 1 with the default right offset of 100 gives a
        // control a hundred pixels wider than its parent, and it
        // looks like the anchor is broken. The preset zeroes them.
        Control *strip = new Control();
        strip->set_name("Strip");
        strip->set_anchors_preset(Control::Preset::BottomWide);
        strip->offset_top = -50;
        root->add_child(strip);

        UiSystem ui;
        UiSystem::Frame f;
        f.width = 800;
        f.height = 600;
        ui.update(&tree, f, 1.0f / 60.0f);

        check(near(root->rect().w, 800) && near(root->rect().h, 600),
              "a full-rect control fills its parent");
        check(near(corner->rect().x, 10) && near(corner->rect().w, 100),
              "a pinned control is where its offsets put it");
        std::printf("       strip rect (%.1f %.1f) %.1f x %.1f\n",
                    strip->rect().x, strip->rect().y, strip->rect().w,
                    strip->rect().h);
        check(near(strip->rect().y, 550) && near(strip->rect().w, 800),
              "and a stretched one spans the parent");

        // THE TEST ANCHORS EXIST FOR. Resize and everything has to
        // still be right, with no relayout call of its own.
        f.width = 1280;
        f.height = 720;
        ui.update(&tree, f, 1.0f / 60.0f);
        check(near(corner->rect().x, 10) && near(corner->rect().w, 100),
              "after a resize the pinned control has not moved");
        check(near(strip->rect().y, 670) && near(strip->rect().w, 1280),
              "and the stretched one followed the edge");
        tree.set_scene(nullptr);
    }

    // ---------------------------------------------------- containers
    {
        SceneTree tree;
        VBoxContainer *box = new VBoxContainer();
        box->set_name("Box");
        box->set_anchors_preset(Control::Preset::FullRect);
        box->separation = 10.0f;
        tree.set_scene(box);

        // Two fixed, one that takes what is left.
        Control *top = new Control();
        top->set_name("Top");
        top->custom_minimum_size = {0, 40};
        top->size_flags_vertical = Control::SizeFill;
        box->add_child(top);

        Control *middle = new Control();
        middle->set_name("Middle");
        middle->custom_minimum_size = {0, 20};
        middle->size_flags_vertical = Control::SizeFill | Control::SizeExpand;
        box->add_child(middle);

        Control *bottom = new Control();
        bottom->set_name("Bottom");
        bottom->custom_minimum_size = {0, 30};
        box->add_child(bottom);

        UiSystem ui;
        UiSystem::Frame f;
        f.width = 400;
        f.height = 300;
        ui.update(&tree, f, 1.0f / 60.0f);

        char what[200];
        std::snprintf(what, sizeof(what),
                      "a vbox stacks its children (%.0f, %.0f, %.0f)",
                      top->rect().y, middle->rect().y, bottom->rect().y);
        check(near(top->rect().y, 0) && near(middle->rect().y, 50) &&
                  near(bottom->rect().y, 270),
              what);
        check(near(top->rect().h, 40) && near(bottom->rect().h, 30),
              "the fixed ones keep their heights");
        // 300 total, 90 of minimums, 20 of separation -> 190 spare,
        // all of it to the one that asked.
        std::snprintf(what, sizeof(what),
                      "and the expanding one takes what is left (%.0f)",
                      middle->rect().h);
        check(near(middle->rect().h, 210), what);
        check(near(top->rect().w, 400), "with the full width each");

        // TWO EXPANDING CHILDREN SHARE IT, in proportion. This is
        // the case a single-expander implementation gets wrong and
        // nobody notices until the second one appears.
        top->size_flags_vertical = Control::SizeFill | Control::SizeExpand;
        top->stretch_ratio = 3.0f;
        middle->stretch_ratio = 1.0f;
        ui.update(&tree, f, 1.0f / 60.0f);
        const float extra_top = top->rect().h - 40.0f;
        const float extra_mid = middle->rect().h - 20.0f;
        std::snprintf(what, sizeof(what),
                      "two expanders split the spare space 3:1 (%.0f and %.0f)",
                      extra_top, extra_mid);
        check(near(extra_top / std::max(extra_mid, 1.0f), 3.0f, 0.05f), what);

        // A container's minimum is what it needs to hold its
        // children, which is what stops a window shrinking past its
        // own content.
        const Vec2 m = box->minimum_size();
        std::snprintf(what, sizeof(what),
                      "and the box knows its own minimum (%.0f)", m.y);
        check(near(m.y, 40 + 20 + 30 + 20), what);
        tree.set_scene(nullptr);
    }

    // --------------------------------------------- input and signals
    {
        SceneTree tree;
        Control *root = new Control();
        root->set_name("Root");
        root->set_anchors_preset(Control::Preset::FullRect);
        root->mouse_filter = Control::MouseFilter::Ignore;
        tree.set_scene(root);

        Button *a = new Button();
        a->set_name("A");
        a->text = "A";
        a->offset_left = 10;
        a->offset_top = 10;
        a->offset_right = 110;
        a->offset_bottom = 50;
        root->add_child(a);

        // Overlapping, and declared second -- so it is on top.
        Button *b = new Button();
        b->set_name("B");
        b->text = "B";
        b->offset_left = 60;
        b->offset_top = 10;
        b->offset_right = 160;
        b->offset_bottom = 50;
        root->add_child(b);

        int a_clicks = 0, b_clicks = 0;
        a->connect("pressed", a, [&](const Variant *, int) { a_clicks++; });
        b->connect("pressed", b, [&](const Variant *, int) { b_clicks++; });

        UiSystem ui;
        UiSystem::Frame f;
        f.width = 400;
        f.height = 300;

        // Over the part of A that B does not cover.
        f.mouse = {30, 30};
        ui.update(&tree, f, 1.0f / 60.0f);
        check(ui.hovered() == a, "the pointer finds the control under it");
        check(a->hovered() && !b->hovered(), "and only that one is hovered");

        f.mouse_down = true;
        ui.update(&tree, f, 1.0f / 60.0f);
        check(a_clicks == 0, "pressing is not clicking");
        f.mouse_down = false;
        ui.update(&tree, f, 1.0f / 60.0f);
        check(a_clicks == 1, "releasing over it is");
        check(a->has_focus(), "and it took the focus");

        // OVER THE OVERLAP, B WINS. Later siblings draw on top, so
        // the hit test walks backwards -- forwards and A would take
        // every click in the overlap while B drew over it, which
        // looks like the button not working.
        f.mouse = {80, 30};
        ui.update(&tree, f, 1.0f / 60.0f);
        check(ui.hovered() == b, "the topmost overlapping control is hit");
        f.mouse_down = true;
        ui.update(&tree, f, 1.0f / 60.0f);
        f.mouse_down = false;
        ui.update(&tree, f, 1.0f / 60.0f);
        check(b_clicks == 1 && a_clicks == 1,
              "and it is the one that gets the click");

        // DRAG OFF AND IT IS NOT A CLICK, which is what makes a
        // button cancellable.
        f.mouse = {80, 30};
        f.mouse_down = true;
        ui.update(&tree, f, 1.0f / 60.0f);
        f.mouse = {300, 200};
        ui.update(&tree, f, 1.0f / 60.0f);
        f.mouse_down = false;
        ui.update(&tree, f, 1.0f / 60.0f);
        check(b_clicks == 1, "pressing and dragging off does not click");

        tree.set_scene(nullptr);
    }

    // ------------------------------------- a slider and a text field
    {
        SceneTree tree;
        Control *root = new Control();
        root->set_anchors_preset(Control::Preset::FullRect);
        root->mouse_filter = Control::MouseFilter::Ignore;
        tree.set_scene(root);

        Slider *s = new Slider();
        s->set_name("S");
        s->offset_left = 0;
        s->offset_top = 0;
        s->offset_right = 200;
        s->offset_bottom = 20;
        s->min_value = 0;
        s->max_value = 100;
        root->add_child(s);

        float reported = -1.0f;
        s->connect("value_changed", s, [&](const Variant *v, int n) {
            if (n > 0) reported = float(v[0].to_float());
        });

        UiSystem ui;
        UiSystem::Frame f;
        f.width = 400;
        f.height = 300;
        f.mouse = {150, 10};
        f.mouse_down = true;
        ui.update(&tree, f, 1.0f / 60.0f);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "dragging a slider to three quarters gives 75 (%.1f)",
                      double(s->value));
        check(near(s->value, 75.0f, 0.5f), what);
        check(near(reported, s->value), "and it says so through its signal");
        f.mouse_down = false;
        ui.update(&tree, f, 1.0f / 60.0f);

        LineEdit *e = new LineEdit();
        e->set_name("E");
        e->offset_left = 0;
        e->offset_top = 40;
        e->offset_right = 200;
        e->offset_bottom = 70;
        root->add_child(e);

        f.mouse = {100, 55};
        f.mouse_down = true;
        ui.update(&tree, f, 1.0f / 60.0f);
        f.mouse_down = false;
        ui.update(&tree, f, 1.0f / 60.0f);
        check(e->has_focus(), "clicking a text field focuses it");
        check(!s->has_focus(), "and unfocuses whatever had it");

        f.text = "hello";
        ui.update(&tree, f, 1.0f / 60.0f);
        f.text.clear();
        check(e->text == "hello", "typing reaches the focused field");

        std::string submitted;
        e->connect("text_submitted", e, [&](const Variant *v, int n) {
            if (n > 0) submitted = v[0].to_string();
        });
        f.keys_pressed.push_back(40);   // return
        ui.update(&tree, f, 1.0f / 60.0f);
        f.keys_pressed.clear();
        check(submitted == "hello", "and enter submits it");
        tree.set_scene(nullptr);
    }

    // ---------------------------- and a UI is a scene like any other
    {
        // THE WHOLE REASON THE UI IS MADE OF NODES. A health bar is
        // a scene; a party of four is four instances of it.
        VBoxContainer *bar = new VBoxContainer();
        bar->set_name("HealthBar");
        Label *name = new Label();
        name->set_name("Name");
        name->text = "Unit";
        bar->add_child(name);
        ProgressBar *hp = new ProgressBar();
        hp->set_name("HP");
        hp->value = 0.8f;
        bar->add_child(hp);
        bar->set_owner_recursive(bar);
        bar->set_owner(nullptr);

        Ref<PackedScene> scene(new PackedScene());
        check(scene->pack(bar), "a UI can be packed like any other scene");

        Node *one = scene->instantiate();
        Node *two = scene->instantiate();
        check(one && two && one != two, "and instanced more than once");
        Node *hp1 = one ? one->find_child("HP") : nullptr;
        ProgressBar *p1 = hp1 ? hp1->cast_to<ProgressBar>() : nullptr;
        check(p1 && near(p1->value, 0.8f),
              "with its widgets and their values");
        Node *n1 = one ? one->find_child("Name") : nullptr;
        Label *l1 = n1 ? n1->cast_to<Label>() : nullptr;
        check(l1 && l1->text == "Unit", "and its text");

        if (one) { one->queue_free(); delete one; }
        if (two) { two->queue_free(); delete two; }
        bar->queue_free();
        delete bar;
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
