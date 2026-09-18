// Warren -- the sky, the sun and the air between things.
//
// These live on the Renderer, which is not an Object and has no
// business becoming one: it owns pipelines, targets and a device,
// and none of that belongs in a property list. But the handful of
// numbers that decide what a scene FEELS like -- how far you can
// see, what colour the haze is, where the sun is and how hard --
// are exactly what a game wants to set, and until now a game
// written in a script could not touch any of them.
//
// So this is a face, not a thing: it holds no state of its own and
// every property reads and writes the renderer behind it. One
// exists per engine and `warren.environment()` hands it over.
#pragma once

#include "core/object.h"
#include "core/math/vector.h"

namespace wr {

class Renderer;

class Environment : public Object {
    WR_CLASS(Environment, Object)

public:
    Environment() = default;
    explicit Environment(Renderer *r) : renderer_(r) {}
    void bind_renderer(Renderer *r) { renderer_ = r; }

    // The sun. `direction` is the way the light TRAVELS, so a sun
    // overhead points down -- which catches people out often enough
    // to be worth saying twice.
    Vec3 get_sun_direction() const;
    void set_sun_direction(const Vec3 &v);
    Color get_sun_colour() const;
    void set_sun_colour(const Color &c);
    float get_sun_energy() const;
    void set_sun_energy(float v);

    // The light with no direction: what a surface facing away from
    // everything still receives.
    Color get_ambient() const;
    void set_ambient(const Color &c);
    float get_ambient_energy() const;
    void set_ambient_energy(float v);

    // Distance fog. Density is per metre, so 0.004 is visible at
    // fifty metres and gone at five. `height_falloff` above zero
    // pools it in the low ground, which is what makes a valley read
    // as a valley.
    Color get_fog_colour() const;
    void set_fog_colour(const Color &c);
    float get_fog_density() const;
    void set_fog_density(float v);
    float get_fog_height_falloff() const;
    void set_fog_height_falloff(float v);

    float get_exposure() const;
    void set_exposure(float v);
    bool get_draw_sky() const;
    void set_draw_sky(bool v);
    Color get_clear_colour() const;
    void set_clear_colour(const Color &c);
    float get_env_intensity() const;
    void set_env_intensity(float v);
    bool get_shadows() const;
    void set_shadows(bool v);
    float get_shadow_distance() const;
    void set_shadow_distance(float v);

private:
    Renderer *renderer_ = nullptr;
};

}  // namespace wr
