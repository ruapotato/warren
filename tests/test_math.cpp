// Manifold -- does the maths do what it says.
//
// The oblique near plane is the engine's reason to exist, so it is
// tested first and hardest: the claim is that after `with_oblique_near`
// a point lying exactly on the given plane lands exactly on the near
// plane of clip space, at every angle, and that nothing else about the
// projection moves.
#include "../src/core/math/projection.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using namespace mf;

static int g_fail = 0;
static int g_checks = 0;

static void check(bool ok, const char *what, double detail = 0.0) {
    g_checks++;
    if (!ok) {
        g_fail++;
        std::printf("  FAIL  %s   (%g)\n", what, detail);
    }
}
static void near_check(float got, float want, float tol, const char *what) {
    g_checks++;
    if (!(std::fabs(got - want) <= tol)) {
        g_fail++;
        std::printf("  FAIL  %s: got %.9g want %.9g (tol %g)\n", what, got, want,
                    tol);
    }
}

static void section(const char *name) { std::printf("%s\n", name); }

// ---------------------------------------------------------------- basics

static void test_vectors() {
    section("vectors");
    Vec3 a(1, 2, 3), b(-4, 5, 6);
    near_check(dot(a, b), -4 + 10 + 18, 1e-5f, "dot");
    Vec3 c = cross(a, b);
    near_check(dot(c, a), 0.0f, 1e-4f, "cross perpendicular to a");
    near_check(dot(c, b), 0.0f, 1e-4f, "cross perpendicular to b");
    near_check(a.normalized().length(), 1.0f, 1e-6f, "normalized is unit");
    Vec3 p = any_perpendicular(Vec3(0, 1, 0));
    near_check(dot(p, Vec3(0, 1, 0)), 0.0f, 1e-5f, "any_perpendicular of up");
    near_check(p.length(), 1.0f, 1e-5f, "any_perpendicular is unit");
    for (int i = 0; i < 64; i++) {
        Vec3 v(float(rand() % 200 - 100), float(rand() % 200 - 100),
               float(rand() % 200 - 100));
        if (v.length_sq() < 1.0f) continue;
        Vec3 q = any_perpendicular(v);
        near_check(dot(q, v.normalized()), 0.0f, 1e-4f, "any_perpendicular");
    }
}

static void test_quat_basis() {
    section("rotations");
    for (int i = 0; i < 200; i++) {
        Vec3 axis(float(rand() % 200 - 100), float(rand() % 200 - 100),
                  float(rand() % 200 - 100));
        if (axis.length_sq() < 1.0f) continue;
        float ang = float(rand() % 6283) / 1000.0f - PI;
        Quat q = Quat::from_axis_angle(axis, ang);
        Basis b = q.to_basis();
        // A rotation basis is orthonormal with determinant +1.
        near_check(b.determinant(), 1.0f, 1e-4f, "rotation determinant");
        near_check(b.col[0].length(), 1.0f, 1e-4f, "rotation column unit");
        // Quaternion and basis must move a vector identically.
        Vec3 v(0.3f, -1.7f, 2.2f);
        Vec3 vq = q.xform(v), vb = b.xform(v);
        near_check((vq - vb).length(), 0.0f, 1e-4f, "quat == basis xform");
        // And the round trip through to_quat must come back.
        Quat q2 = b.to_quat();
        Vec3 v2 = q2.xform(v);
        near_check((v2 - vq).length(), 0.0f, 1e-3f, "basis->quat round trip");
    }
    // looking_at must put -Z on the direction asked for.
    for (int i = 0; i < 64; i++) {
        Vec3 dir(float(rand() % 200 - 100), float(rand() % 200 - 100),
                 float(rand() % 200 - 100));
        if (dir.length_sq() < 1.0f) continue;
        dir = dir.normalized();
        Basis b = Basis::looking_at(dir);
        near_check((b.forward() - dir).length(), 0.0f, 1e-4f, "looking_at -Z");
        near_check(b.determinant(), 1.0f, 1e-4f, "looking_at handedness");
    }
    // Euler round trip in the camera's own order.
    for (int i = 0; i < 200; i++) {
        float yaw = float(rand() % 6283) / 1000.0f - PI;
        float pitch = (float(rand() % 3000) / 1000.0f - 1.5f) * 0.99f;
        float roll = float(rand() % 2000) / 1000.0f - 1.0f;
        Basis b = Basis::from_euler_yxz(yaw, pitch, roll);
        Vec3 e = b.to_euler_yxz();
        Basis b2 = Basis::from_euler_yxz(e.x, e.y, e.z);
        Vec3 v(1.3f, 0.7f, -2.1f);
        near_check((b.xform(v) - b2.xform(v)).length(), 0.0f, 2e-3f,
                   "euler yxz round trip");
    }
}

static void test_transform() {
    section("transforms");
    for (int i = 0; i < 200; i++) {
        Vec3 axis(float(rand() % 200 - 100), float(rand() % 200 - 100),
                  float(rand() % 200 - 100));
        if (axis.length_sq() < 1.0f) continue;
        float ang = float(rand() % 6283) / 1000.0f - PI;
        float scale = 0.2f + float(rand() % 400) / 100.0f;
        Transform3D t(Basis::from_axis_angle(axis, ang) * scale,
                      Vec3(float(rand() % 40 - 20), float(rand() % 40 - 20),
                           float(rand() % 40 - 20)));
        Vec3 v(0.4f, -2.2f, 5.1f);
        Transform3D ti = t.inverse();
        near_check((ti.xform(t.xform(v)) - v).length(), 0.0f, 1e-2f,
                   "transform inverse");
        // Composition must agree with applying one after the other.
        Transform3D u(Basis::from_axis_angle(Vec3(1, 2, 3), 0.7f), Vec3(1, -2, 3));
        near_check(((t * u).xform(v) - t.xform(u.xform(v))).length(), 0.0f, 1e-3f,
                   "transform composition");
        // Uniform scale must be readable back out.
        near_check(t.basis.uniform_scale(), scale, 1e-3f, "uniform_scale");
        check(t.basis.is_uniform(), "is_uniform on a uniformly scaled basis");
    }
}

static void test_plane() {
    section("planes");
    // A plane moved by a transform must still contain the moved points.
    for (int i = 0; i < 100; i++) {
        Vec3 n(float(rand() % 200 - 100), float(rand() % 200 - 100),
               float(rand() % 200 - 100));
        if (n.length_sq() < 1.0f) continue;
        n = n.normalized();
        Vec3 origin(float(rand() % 40 - 20), float(rand() % 40 - 20),
                    float(rand() % 40 - 20));
        Plane p(n, origin);
        near_check(p.distance_to(origin), 0.0f, 1e-3f, "point on plane");

        Transform3D t(Basis::from_axis_angle(Vec3(0.3f, 1.0f, -0.5f), 1.1f) * 2.5f,
                      Vec3(3, -4, 5));
        Plane q = p.transformed(t);
        // Two more points on the original plane, moved.
        Vec3 u = any_perpendicular(n), w = cross(n, u);
        for (float s : {-3.0f, 0.0f, 7.5f})
            for (float r : {-2.0f, 4.0f}) {
                Vec3 on = origin + u * s + w * r;
                near_check(q.distance_to(t.xform(on)), 0.0f, 1e-2f,
                           "transformed plane contains transformed point");
            }
    }
}

// ------------------------------------------------- the one that matters

static void test_projection_basics() {
    section("projection: reverse-Z, one clip space for both backends");
    Projection p = Projection::perspective(deg2rad(60.0f), 16.0f / 9.0f, 0.1f, 500.0f);
    near_check(p.get_fov_y(), deg2rad(60.0f), 1e-4f, "fov round trip");
    near_check(p.get_aspect(), 16.0f / 9.0f, 1e-4f, "aspect round trip");
    near_check(p.get_z_near(), 0.1f, 1e-4f, "znear round trip");
    near_check(p.get_z_far(), 500.0f, 1e-1f, "zfar round trip");
    check(!p.is_orthographic(), "perspective is not orthographic");
    check(!p.is_infinite_far(), "finite far is finite");

    // REVERSED: near is 1, far is 0.
    near_check(p.project(Vec3(0, 0, -0.1f)).z, 1.0f, 1e-4f, "near -> 1");
    near_check(p.project(Vec3(0, 0, -500.0f)).z, 0.0f, 1e-4f, "far -> 0");
    // And monotonically decreasing in between, which is what makes the
    // depth test GREATER rather than LESS.
    float prev = 2.0f;
    for (float d = 0.1f; d < 500.0f; d *= 1.5f) {
        float z = p.project(Vec3(0, 0, -d)).z;
        check(z <= prev + 1e-6f, "reverse-Z depth decreases with distance",
              double(z));
        prev = z;
    }
    Vec3 c = p.project(Vec3(0, 0, -10.0f));
    near_check(c.x, 0.0f, 1e-5f, "centre x");
    near_check(c.y, 0.0f, 1e-5f, "centre y");
    // +Y up: a point above the axis must land in the upper half.
    check(p.project(Vec3(0, 1, -10.0f)).y > 0.0f, "+Y is up in NDC");
    check(p.project(Vec3(1, 0, -10.0f)).x > 0.0f, "+X is right in NDC");

    for (int i = 0; i < 400; i++) {
        Vec3 v(float(rand() % 200 - 100) * 0.05f, float(rand() % 200 - 100) * 0.05f,
               -0.2f - float(rand() % 20000) * 0.01f);
        Vec3 back = p.unproject(p.project(v));
        near_check((back - v).length() / v.length(), 0.0f, 1e-3f,
                   "projection inverse round trip");
    }

    // THE PRECISION CLAIM, PROVEN RATHER THAN ASSERTED.
    //
    // Reverse-Z is only a win paired with a FLOATING-POINT depth
    // buffer: it works by moving distant geometry to near zero, where
    // float has its resolution. With a fixed-point buffer it is no
    // better and slightly worse. This is why the engine's depth-stencil
    // format is D32_SFLOAT_S8_UINT and not the cheaper D24_UNORM_S8 --
    // portals need the stencil and terrain needs the float, and that
    // format is the only one that gives both.
    //
    // The comparison below is against the same projection un-reversed,
    // which is the thing reverse-Z is claimed to beat.
    {
        const float n = 0.05f, f = 4000.0f;
        Projection rev = Projection::perspective(deg2rad(70.0f), 1.7f, n, f);
        // The same frustum with the depth row NOT reversed: the
        // standard [0, 1] mapping, near -> 0 and far -> 1.
        Projection fwd = rev;
        fwd.m[2][2] = f / (n - f);
        fwd.m[3][2] = n * f / (n - f);
        near_check(fwd.project(Vec3(0, 0, -n)).z, 0.0f, 1e-5f, "control: near -> 0");
        near_check(fwd.project(Vec3(0, 0, -f)).z, 1.0f, 1e-4f, "control: far -> 1");

        // A centimetre apart, a kilometre away. Count how many
        // representable floats separate the two depths in each scheme:
        // that is exactly the number of distinct values the depth
        // buffer can use to tell them apart.
        auto ulps_between = [](float a, float b) {
            if (a == b) return 0.0;
            float mid = (std::fabs(a) + std::fabs(b)) * 0.5f;
            if (mid <= 0.0f) return 0.0;
            // Spacing of float32 at this magnitude.
            float ulp = std::nextafter(mid, INF) - mid;
            return double(std::fabs(a - b)) / double(ulp);
        };
        double rev_ulps = ulps_between(rev.project(Vec3(0, 0, -1000.0f)).z,
                                       rev.project(Vec3(0, 0, -1000.01f)).z);
        double fwd_ulps = ulps_between(fwd.project(Vec3(0, 0, -1000.0f)).z,
                                       fwd.project(Vec3(0, 0, -1000.01f)).z);
        std::printf("  1cm at 1km:  reverse-Z %.1f float ulps,  standard %.1f\n",
                    rev_ulps, fwd_ulps);
        check(rev_ulps >= 1.0, "reverse-Z resolves 1cm at 1km in float depth",
              rev_ulps);
        check(rev_ulps > fwd_ulps * 8.0,
              "reverse-Z beats standard depth by a wide margin",
              rev_ulps / (fwd_ulps + 1e-12));
    }

    Projection inf = Projection::perspective_infinite(deg2rad(60.0f), 1.6f, 0.05f);
    check(inf.is_infinite_far(), "infinite far is infinite");
    near_check(inf.get_z_near(), 0.05f, 1e-5f, "infinite: znear round trip");
    near_check(inf.project(Vec3(0, 0, -0.05f)).z, 1.0f, 1e-5f, "infinite: near -> 1");
    near_check(inf.project(Vec3(0, 0, -1e7f)).z, 0.0f, 1e-5f, "infinite: far -> 0");
    // It must still invert, because that is how a fragment's world
    // position is reconstructed from the depth buffer.
    for (int i = 0; i < 200; i++) {
        Vec3 v(float(rand() % 200 - 100) * 0.05f, float(rand() % 200 - 100) * 0.05f,
               -0.2f - float(rand() % 20000) * 0.01f);
        Vec3 back = inf.unproject(inf.project(v));
        near_check((back - v).length() / v.length(), 0.0f, 1e-3f,
                   "infinite projection inverts");
    }

    Projection o = Projection::orthographic(-4, 4, -3, 3, 0.5f, 100.0f);
    check(o.is_orthographic(), "orthographic is orthographic");
    near_check(o.project(Vec3(4, 3, -0.5f)).x, 1.0f, 1e-5f, "ortho right edge");
    near_check(o.project(Vec3(0, 0, -0.5f)).z, 1.0f, 1e-5f, "ortho near -> 1");
    near_check(o.project(Vec3(0, 0, -100.0f)).z, 0.0f, 1e-5f, "ortho far -> 0");
    near_check(o.get_z_near(), 0.5f, 1e-4f, "ortho znear round trip");
    near_check(o.get_z_far(), 100.0f, 1e-3f, "ortho zfar round trip");

    float t = 0.1f * std::tan(deg2rad(60.0f) * 0.5f);
    Projection f = Projection::frustum(-t * 16.0f / 9.0f, t * 16.0f / 9.0f, -t, t,
                                       0.1f, 500.0f);
    for (int c2 = 0; c2 < 4; c2++)
        for (int r = 0; r < 4; r++)
            near_check(f.m[c2][r], p.m[c2][r], 1e-6f, "frustum == perspective");
}

// The claim, restated for this clip space: after with_oblique_near, a
// point on the plane lands on z_ndc = 1 (the near plane under
// reverse-Z), the kept half survives, the other half is clipped, and
// the x/y image is bit-identical to the unmodified projection.
static void check_oblique_case(const Projection &base, const Plane &clip,
                               const char *label) {
    Projection ob = base.with_oblique_near(clip.as_vec4());
    const Vec3 n = clip.normal;
    const Vec3 point = clip.normal * clip.d;

    // 1. Every point on the plane lands on the near plane.
    Vec3 u = any_perpendicular(n), w = cross(n, u);
    float span = std::fabs(point.z) * 0.4f + 0.2f;
    for (float su = -2.0f; su <= 2.01f; su += 0.8f)
        for (float sw = -1.5f; sw <= 1.51f; sw += 0.75f) {
            Vec3 on = point + u * (su * span) + w * (sw * span);
            if (on.z > -1e-3f) continue;
            Vec4 h = ob.xform(Vec4(on, 1.0f));
            if (h.w <= 1e-6f) continue;
            near_check(h.z / h.w, 1.0f, 3e-3f, label);
        }

    // 2. The kept half survives; the other half is clipped. Under
    //    reverse-Z "nearer than near" means z_ndc > 1.
    for (float off : {0.02f, 0.2f, 1.0f, 5.0f}) {
        Vec3 keep = point + n * off;
        Vec3 drop = point - n * off;
        if (keep.z < -1e-3f) {
            Vec4 h = ob.xform(Vec4(keep, 1.0f));
            if (h.w > 1e-6f)
                check(h.z / h.w < 1.0f + 2e-3f, "kept half survives",
                      double(h.z / h.w));
        }
        if (drop.z < -1e-3f) {
            Vec4 h = ob.xform(Vec4(drop, 1.0f));
            if (h.w > 1e-6f)
                check(h.z / h.w > 1.0f - 2e-3f, "clipped half is clipped",
                      double(h.z / h.w));
        }
    }

    // 3. The picture does not move.
    for (int k = 0; k < 24; k++) {
        Vec3 v(float(rand() % 200 - 100) * 0.06f, float(rand() % 200 - 100) * 0.06f,
               -0.3f - float(rand() % 6000) * 0.01f);
        Vec3 a = base.project(v);
        Vec3 b = ob.project(v);
        near_check(b.x, a.x, 1e-5f, "oblique leaves x alone");
        near_check(b.y, a.y, 1e-5f, "oblique leaves y alone");
    }

    // 4. Depth still sorts: farther must be a SMALLER z under reverse-Z.
    float prev = 2.0f;
    bool monotone = true;
    for (float t2 = 0.05f; t2 < 400.0f; t2 *= 1.35f) {
        Vec3 v = point + n * t2;
        if (v.z > -1e-3f) continue;
        Vec4 h = ob.xform(Vec4(v, 1.0f));
        if (h.w <= 1e-6f) continue;
        float z = h.z / h.w;
        if (z > prev + 1e-4f) monotone = false;
        prev = z;
    }
    check(monotone, "oblique depth still sorts");
}

static void test_oblique() {
    section("projection: THE OBLIQUE NEAR PLANE (reverse-Z)");

    const float fov = deg2rad(70.0f);
    const float aspect = 16.0f / 9.0f;
    Projection finite = Projection::perspective(fov, aspect, 0.05f, 1000.0f);
    Projection infinite = Projection::perspective_infinite(fov, aspect, 0.05f);
    // An off-axis frustum, as a portal sub-view or a stereo eye uses.
    Projection offaxis = Projection::frustum(-0.04f, 0.07f, -0.03f, 0.05f, 0.05f, 800.0f);

    int cases = 0;
    for (int ai = 0; ai < 17; ai++) {
        float tilt = float(ai) * (80.0f / 16.0f);
        for (int axis = 0; axis < 2; axis++) {
            for (float dist : {0.5f, 2.0f, 9.0f, 40.0f}) {
                Vec3 n = axis == 0
                             ? Vec3(std::sin(deg2rad(tilt)), 0, std::cos(deg2rad(tilt)))
                             : Vec3(0, std::sin(deg2rad(tilt)), std::cos(deg2rad(tilt)));
                n = -n;  // +n points away from the eye: the half we keep
                Plane clip(n, Vec3(0, 0, -dist));
                check_oblique_case(finite, clip, "finite: on the plane -> z_ndc 1");
                check_oblique_case(infinite, clip, "infinite: on the plane -> z_ndc 1");
                check_oblique_case(offaxis, clip, "off-axis: on the plane -> z_ndc 1");
                cases += 3;
            }
        }
    }
    std::printf("  swept %d projection/plane combinations\n", cases);

    // Obliquing an ALREADY oblique projection must land on the second
    // plane -- recursive portals do exactly this, a portal seen through
    // a portal.
    Plane first(Vec3(0.35f, 0.1f, -0.93f).normalized(), Vec3(0, 0, -2.0f));
    Plane second(Vec3(-0.2f, 0.3f, -0.93f).normalized(), Vec3(0.3f, 0, -5.0f));
    Projection once = finite.with_oblique_near(first.as_vec4());
    check_oblique_case(once, second, "oblique of an oblique");

    // Orthographic, which is how a shadow cascade gets clipped to a
    // portal. The derivation reads row 3 from the matrix, so this works
    // with no special case.
    Projection ortho = Projection::orthographic(-20, 20, -20, 20, 0.1f, 200.0f);
    Plane ocl(Vec3(0.3f, 0.0f, -0.954f).normalized(), Vec3(0, 0, -30.0f));
    Projection oob = ortho.with_oblique_near(ocl.as_vec4());
    for (float su = -8.0f; su <= 8.01f; su += 4.0f) {
        Vec3 u = any_perpendicular(ocl.normal);
        Vec3 on = ocl.normal * ocl.d + u * su;
        Vec4 h = oob.xform(Vec4(on, 1.0f));
        near_check(h.z / h.w, 1.0f, 3e-3f, "orthographic oblique lands on near");
    }

    // The inverse must survive, since that is how a fragment's view
    // position is reconstructed inside a portal.
    Plane clip(Vec3(0.5f, 0.3f, -0.81f).normalized(), Vec3(0, 0, -3.0f));
    Projection ob = finite.with_oblique_near(clip.as_vec4());
    for (int i = 0; i < 200; i++) {
        Vec3 v(float(rand() % 200 - 100) * 0.05f, float(rand() % 200 - 100) * 0.05f,
               -3.5f - float(rand() % 4000) * 0.01f);
        Vec3 back = ob.unproject(ob.project(v));
        near_check((back - v).length() / v.length(), 0.0f, 2e-3f,
                   "oblique projection inverts");
    }

    check(finite.with_oblique_near(Vec4(0, 0, 0, 0)) == finite,
          "degenerate clip plane is a no-op");

    // THE REFUSAL CASE. A plane whose kept half-space does not contain
    // the far corner of the frustum -- one facing back at the camera,
    // so that "keep" means "keep what is CLOSER" -- has no solution as
    // a near plane, and the derivation's k comes out non-negative.
    // Refusing leaves the picture intact; going ahead would invert the
    // depth row and turn the world inside out.
    Plane facing_back(Vec3(0, 0, 1), Vec3(0, 0, -5.0f));
    check(finite.with_oblique_near(facing_back.as_vec4()) == finite,
          "a plane that cannot be a near plane is refused");

    // And whatever is handed to it, the result must be finite: a NaN
    // in a projection matrix is a black screen with nothing in the log.
    for (int i = 0; i < 300; i++) {
        Vec4 c(float(rand() % 200 - 100) * 0.03f, float(rand() % 200 - 100) * 0.03f,
               float(rand() % 200 - 100) * 0.03f, float(rand() % 200 - 100) * 0.4f);
        Projection r = finite.with_oblique_near(c);
        bool ok = true;
        for (int cc = 0; cc < 4; cc++)
            for (int rr = 0; rr < 4; rr++)
                if (!std::isfinite(r.m[cc][rr])) ok = false;
        check(ok, "oblique never produces a non-finite matrix");
    }
}

static void test_frustum_planes() {
    section("frustum extraction");
    Projection p = Projection::perspective(deg2rad(60.0f), 1.6f, 0.1f, 100.0f);
    Transform3D cam = Transform3D::looking_at(Vec3(3, 2, 8), Vec3(0, 0, 0));
    Plane planes[6];
    p.frustum_planes(cam, planes);

    // A point at the centre of the view, ten metres out, is inside.
    Vec3 inside = cam.origin + cam.forward() * 10.0f;
    for (int i = 0; i < 6; i++)
        check(planes[i].distance_to(inside) > 0.0f, "centre point inside frustum",
              double(planes[i].distance_to(inside)));

    // A point behind the camera is outside at least one.
    Vec3 behind = cam.origin - cam.forward() * 10.0f;
    bool out = false;
    for (int i = 0; i < 6; i++)
        if (planes[i].distance_to(behind) < 0.0f) out = true;
    check(out, "point behind the camera is culled");

    // Far away along the view axis is outside too.
    Vec3 far_away = cam.origin + cam.forward() * 500.0f;
    out = false;
    for (int i = 0; i < 6; i++)
        if (planes[i].distance_to(far_away) < 0.0f) out = true;
    check(out, "point past the far plane is culled");

    // Agreement with the projection itself: anything the planes accept,
    // clip space must accept too.
    Transform3D view = cam.inverse_orthonormal();
    for (int i = 0; i < 4000; i++) {
        Vec3 w(float(rand() % 400 - 200) * 0.25f, float(rand() % 400 - 200) * 0.25f,
               float(rand() % 400 - 200) * 0.25f);
        bool by_planes = true;
        for (int k = 0; k < 6; k++)
            if (planes[k].distance_to(w) < 0.0f) by_planes = false;
        Vec4 h = p.xform(Vec4(view.xform(w), 1.0f));
        bool by_clip = h.w > 0.0f && std::fabs(h.x) <= h.w &&
                       std::fabs(h.y) <= h.w && std::fabs(h.z) <= h.w;
        check(by_planes == by_clip, "frustum planes agree with clip space");
    }
}

// ------------------------------------------------ frustum slice corners
//
// What a shadow cascade is fitted to. Two properties matter and both
// are easy to get subtly wrong: the corners must actually be the
// frustum at those two distances, and -- the one that matters for
// this engine -- they must NOT move when an oblique near plane is cut
// into the projection. A portal view is the same cone of directions
// as the camera it was warped from; only the near clip changed, and a
// cascade fitted to it must not lurch every time the player's angle
// to a portal changes.
static void test_slice_corners() {
    section("frustum slices");

    const Projection p = Projection::perspective(deg2rad(60.0f), 16.0f / 9.0f,
                                                 0.1f, 500.0f);
    Vec3 c[8];
    p.slice_corners(10.0f, 40.0f, c);

    // The near face first, then the far one, each at -z.
    for (int i = 0; i < 4; i++) near_check(c[i].z, -10.0f, 1e-4f, "near face at -z_near");
    for (int i = 4; i < 8; i++) near_check(c[i].z, -40.0f, 1e-4f, "far face at -z_far");

    // Ordered (-x,-y), (+x,-y), (+x,+y), (-x,+y) on each face.
    for (int f = 0; f < 2; f++) {
        const Vec3 *q = c + f * 4;
        check(q[0].x < 0 && q[0].y < 0, "corner 0 is lower left");
        check(q[1].x > 0 && q[1].y < 0, "corner 1 is lower right");
        check(q[2].x > 0 && q[2].y > 0, "corner 2 is upper right");
        check(q[3].x < 0 && q[3].y > 0, "corner 3 is upper left");
    }

    // A perspective frustum widens in proportion to distance.
    near_check((c[5].x - c[4].x) / (c[1].x - c[0].x), 4.0f, 1e-3f,
               "the far face is four times as wide at four times the distance");

    // Every corner is on the frustum's boundary: projecting it lands
    // on an NDC edge.
    for (int i = 0; i < 8; i++) {
        const Vec3 ndc = p.project(c[i]);
        near_check(std::fabs(ndc.x), 1.0f, 2e-3f, "corner is on the x edge");
        near_check(std::fabs(ndc.y), 1.0f, 2e-3f, "corner is on the y edge");
    }

    // THE ONE THAT MATTERS. Cut an oblique near plane in and the
    // slice is unchanged, because with_oblique_near only replaces the
    // third row.
    const Plane clip(Vec3(0.3f, 0.2f, -0.9f).normalized(), -6.0f);
    const Projection oblique = p.with_oblique_near(clip.as_vec4());
    Vec3 o[8];
    oblique.slice_corners(10.0f, 40.0f, o);
    for (int i = 0; i < 8; i++) {
        near_check(o[i].x, c[i].x, 1e-3f, "oblique near does not move x");
        near_check(o[i].y, c[i].y, 1e-3f, "oblique near does not move y");
        near_check(o[i].z, c[i].z, 1e-3f, "oblique near does not move z");
    }

    // An orthographic projection has the same box at every distance.
    const Projection ortho = Projection::orthographic(-3, 5, -2, 7, 0.0f, 100.0f);
    Vec3 b[8];
    ortho.slice_corners(5.0f, 50.0f, b);
    near_check(b[0].x, -3.0f, 1e-3f, "ortho left");
    near_check(b[1].x, 5.0f, 1e-3f, "ortho right");
    near_check(b[0].y, -2.0f, 1e-3f, "ortho bottom");
    near_check(b[3].y, 7.0f, 1e-3f, "ortho top");
    for (int i = 0; i < 4; i++) {
        near_check(b[i + 4].x, b[i].x, 1e-3f, "ortho does not widen with distance");
        near_check(b[i + 4].y, b[i].y, 1e-3f, "ortho does not heighten either");
    }

    // A shadow cascade's projection is reverse-Z like every other:
    // the near plane is 1 and the far plane is 0.
    near_check(ortho.project(Vec3(0, 0, 0.0f)).z, 1.0f, 1e-5f,
               "ortho near maps to 1 (reverse-Z)");
    near_check(ortho.project(Vec3(0, 0, -100.0f)).z, 0.0f, 1e-5f,
               "ortho far maps to 0 (reverse-Z)");

    // And an off-axis frustum's slice is off-axis too: the extents do
    // not have to straddle the axis.
    const Projection off = Projection::frustum(-0.1f, 0.4f, -0.2f, 0.1f, 0.1f, 100.0f);
    Vec3 d[8];
    off.slice_corners(1.0f, 2.0f, d);
    check(std::fabs(d[1].x) > std::fabs(d[0].x) * 3.0f,
          "an off-axis frustum keeps its offset");
}

int main() {
    std::srand(20260918);
    test_vectors();
    test_quat_basis();
    test_transform();
    test_plane();
    test_projection_basics();
    test_oblique();
    test_frustum_planes();
    test_slice_corners();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
