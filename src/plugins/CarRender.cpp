#include "City.h"
#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace sf {

void City::setDrivingEnabled(bool enabled) {
    drivingEnabled_ = enabled;
    if (!enabled) {
        car_ = {};
        trackFade_ = 0;
    }
}

// The sports car: the glTF mesh drawn as a neon hologram, with an underglow pool, headlight beams and tail lights.
void City::renderCar(Renderer& r) {
    if (!car_.show || car_.spawn <= 0.01f) return;
    if (carModel_ == -2) carModel_ = r.model("CarConcept");
    const Vec3 neon{0.1f, 0.85f, 1.0f};
    const float s = car_.spawn * car_.spawn * (3.0f - 2.0f * car_.spawn);
    const Vec3 fwd{-std::sin(car_.yaw), 0, -std::cos(car_.yaw)}, rt{std::cos(car_.yaw), 0, -std::sin(car_.yaw)};
    const Vec3 railR = cross(car_.railT, car_.railU);
    Mat4 orient = Mat4::rotateY(car_.yaw);
    if (car_.rail) {
        orient = Mat4::identity();
        const Vec3 ax[3] = {railR, car_.railU, car_.railT * -1.0f};
        for (int c = 0; c < 3; ++c) { orient.m[c * 4] = ax[c].x; orient.m[c * 4 + 1] = ax[c].y; orient.m[c * 4 + 2] = ax[c].z; }
    }
    const Mat4 base = Mat4::translate(car_.pos) * orient;
    constexpr float kLen = 4.6f;
    if (carModel_ >= 0 && !(car_.hideBody && !r.mirror())) {
        const Vec3 sz = r.modelSize(carModel_);
        const bool alongX = sz.x > sz.z;
        const float k = kLen / std::max(sz.x, sz.z) * s;
        const Mat4 M = base * Mat4::rotateX(car_.pitch) * Mat4::rotateY(alongX ? -1.5708f : 3.14159265f) * Mat4::scale({k, k, k});
        r.drawModel(carModel_, *dummy_, M, neon * 0.22f, 0.3f, false);
        // Each wheel is its own mesh: fronts also steer (up to ~32 degrees), all roll with the car's speed.
        const float ang = -car_.steer * 0.55f;
        const char* names[4] = {"CarConcept#FL", "CarConcept#FR", "CarConcept#RL", "CarConcept#RR"};
        for (int w = 0; w < 4; ++w) {
            const int wid = r.model(names[w]);
            if (wid < 0) continue;
            const Vec3 pv = r.modelSize(wid);
            const Mat4 steer = w < 2 ? Mat4::rotateY(ang) : Mat4::identity();
            r.drawModel(wid, *dummy_, M * Mat4::translate(pv) * steer * Mat4::rotateX(car_.spin) * Mat4::translate({-pv.x, -pv.y, -pv.z}), neon * 0.22f, 0.3f, false);
        }
    }
    const float pulse = 0.85f + 0.15f * std::sin(float(wall_) * 6.0f);
    const Vec3 red{1.0f, 0.1f, 0.2f};
    const float brake = car_.speed < 0 ? 1.6f : 1.0f;
    renderBoxM(r, base * Mat4::translate({0, 0.62f, 2.3f}) * Mat4::scale({1.5f * s, 0.12f, 0.1f}), red * (0.7f * brake));
    (void)fwd; (void)rt;
}

}  // namespace sf
