#include "render/environment.h"

#include "core/bind.h"
#include "render/renderer.h"

namespace wr {

// Every accessor is the same two lines, and writing them out beats a
// macro that would have to be read before any one of them could be.
#define WR_ENV_GET(field, def) \
    return renderer_ ? renderer_->field : (def)
#define WR_ENV_SET(field, value) \
    if (renderer_) renderer_->field = (value)

Vec3 Environment::get_sun_direction() const {
    WR_ENV_GET(sun_direction, Vec3(0, -1, 0));
}
void Environment::set_sun_direction(const Vec3 &v) {
    // Normalised on the way in: a zero-length or unnormalised sun
    // direction is not an error anyone would spot, it is a scene
    // that is subtly too bright or lit from nowhere.
    Vec3 d = v.length() > 1e-5f ? v.normalized() : Vec3(0, -1, 0);
    WR_ENV_SET(sun_direction, d);
}
Color Environment::get_sun_colour() const {
    WR_ENV_GET(sun_colour, Color::white());
}
void Environment::set_sun_colour(const Color &c) { WR_ENV_SET(sun_colour, c); }
float Environment::get_sun_energy() const { WR_ENV_GET(sun_energy, 0.0f); }
void Environment::set_sun_energy(float v) { WR_ENV_SET(sun_energy, v); }

Color Environment::get_ambient() const { WR_ENV_GET(ambient, Color()); }
void Environment::set_ambient(const Color &c) { WR_ENV_SET(ambient, c); }
float Environment::get_ambient_energy() const {
    WR_ENV_GET(ambient_energy, 0.0f);
}
void Environment::set_ambient_energy(float v) { WR_ENV_SET(ambient_energy, v); }

Color Environment::get_fog_colour() const { WR_ENV_GET(fog_colour, Color()); }
void Environment::set_fog_colour(const Color &c) { WR_ENV_SET(fog_colour, c); }
float Environment::get_fog_density() const { WR_ENV_GET(fog_density, 0.0f); }
void Environment::set_fog_density(float v) { WR_ENV_SET(fog_density, v); }
float Environment::get_fog_height_falloff() const {
    WR_ENV_GET(fog_height_falloff, 0.0f);
}
void Environment::set_fog_height_falloff(float v) {
    WR_ENV_SET(fog_height_falloff, v);
}

float Environment::get_exposure() const {
    return renderer_ ? renderer_->settings().exposure : 1.0f;
}
void Environment::set_exposure(float v) {
    if (renderer_) renderer_->settings().exposure = v;
}
bool Environment::get_draw_sky() const {
    return renderer_ ? renderer_->settings().draw_sky : true;
}
void Environment::set_draw_sky(bool v) {
    if (renderer_) renderer_->settings().draw_sky = v;
}
Color Environment::get_clear_colour() const {
    return renderer_ ? renderer_->settings().clear_colour : Color();
}
void Environment::set_clear_colour(const Color &c) {
    if (renderer_) renderer_->settings().clear_colour = c;
}
float Environment::get_env_intensity() const {
    return renderer_ ? renderer_->settings().env_intensity : 1.0f;
}
void Environment::set_env_intensity(float v) {
    if (renderer_) renderer_->settings().env_intensity = v;
}
bool Environment::get_shadows() const {
    return renderer_ ? renderer_->settings().shadows : true;
}
void Environment::set_shadows(bool v) {
    if (renderer_) renderer_->settings().shadows = v;
}
float Environment::get_shadow_distance() const {
    return renderer_ ? renderer_->settings().shadow_distance : 0.0f;
}
void Environment::set_shadow_distance(float v) {
    if (renderer_) renderer_->settings().shadow_distance = v;
}

#undef WR_ENV_GET
#undef WR_ENV_SET

static void register_environment_class() {
    ClassBuilder<Environment>()
        .prop("sun_direction", &Environment::get_sun_direction,
              &Environment::set_sun_direction)
        .prop("sun_colour", &Environment::get_sun_colour,
              &Environment::set_sun_colour)
        .prop("sun_energy", &Environment::get_sun_energy,
              &Environment::set_sun_energy, "range:0,64")
        .prop("ambient", &Environment::get_ambient, &Environment::set_ambient)
        .prop("ambient_energy", &Environment::get_ambient_energy,
              &Environment::set_ambient_energy, "range:0,8")
        .prop("fog_colour", &Environment::get_fog_colour,
              &Environment::set_fog_colour)
        .prop("fog_density", &Environment::get_fog_density,
              &Environment::set_fog_density, "range:0,0.2")
        .prop("fog_height_falloff", &Environment::get_fog_height_falloff,
              &Environment::set_fog_height_falloff, "range:0,1")
        .prop("exposure", &Environment::get_exposure,
              &Environment::set_exposure, "range:0,8")
        .prop("draw_sky", &Environment::get_draw_sky,
              &Environment::set_draw_sky)
        .prop("clear_colour", &Environment::get_clear_colour,
              &Environment::set_clear_colour)
        .prop("env_intensity", &Environment::get_env_intensity,
              &Environment::set_env_intensity, "range:0,8")
        .prop("shadows", &Environment::get_shadows, &Environment::set_shadows)
        .prop("shadow_distance", &Environment::get_shadow_distance,
              &Environment::set_shadow_distance, "range:1,1024");
}
WR_REGISTER(register_environment_class)

}  // namespace wr
