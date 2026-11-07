#include "AppShell.h"

#include <algorithm>
#include <cmath>

namespace sf {

Plugin AppShell::makeDrivingPlugin() {
    return {"org.scifishell.driving", "CAR AND TRACK", "Driving, ramps and race track", "1.0.0",
            [this] { drivingPluginEnabled_ = true; city_->setDrivingEnabled(true); },
            [this] { stopDrivingPlugin(); }};
}

void AppShell::leaveTrack() {
    if (!onTrack_ || !city_) return;
    onTrack_ = false;
    City::TrackFrame f0;
    if (city_->trackSample(0.0f, f0)) {
        carPos_ = {f0.p.x, 0.0f, f0.p.z};
        carYaw_ = std::atan2(-f0.flat.x, -f0.flat.z);
        carPos_.y = city_->floorAt({carPos_.x, 3.0f, carPos_.z}, 1.7f, 1.0f);
    }
    carVel_ = {};
    carSpeed_ = carVy_ = 0.0f;
    trackCool_ = 3.0f;
}

// Summon the car just ahead of the player and climb in, or climb out again leaving it parked.
void AppShell::toggleCar() {
    if (!drivingPluginEnabled_ || !cityMode_ || !city_ || vrOn_) return;
    if (driving_) {
        driving_ = false;
        leaveTrack();
        const Vec3 rt{std::cos(carYaw_), 0, -std::sin(carYaw_)};
        camera_.position = {carPos_.x + rt.x * 3.0f, carPos_.y + 1.7f, carPos_.z + rt.z * 3.0f};
        camera_.yaw = carYaw_;
        camera_.pitch = 0.0f;
        flying_ = false;
        vy_ = 0.0f;
        carVel_ = {};
        return;
    }
    city_->ensureTrack();
    onTrack_ = false;
    trackCool_ = 0.0f;
    const Vec3 f{-std::sin(camera_.yaw), 0, -std::cos(camera_.yaw)};
    Vec3 p{camera_.position.x + f.x * 7.0f, 0, camera_.position.z + f.z * 7.0f};
    p.y = city_->floorAt({p.x, camera_.position.y, p.z}, 1.7f, 1.0f) + 1.0f;
    city_->resolve(p, 1.7f, 1.0f);
    carPos_ = {p.x, city_->floorAt({p.x, p.y + 1.0f, p.z}, 1.7f, 1.0f), p.z};
    carYaw_ = chaseYaw_ = camera_.yaw;
    carVel_ = {};
    carVy_ = carPitch_ = carSteer_ = carSpeed_ = lookOffX_ = lookOffY_ = 0.0f;
    carSpawn_ = 0.0f;
    carOn_ = driving_ = true;
    sfx_.play(SfxId::CarSummon);
}

void AppShell::stopDrivingPlugin() {
    if (driving_ && city_) {
        leaveTrack();
        const Vec3 rt{std::cos(carYaw_), 0, -std::sin(carYaw_)};
        if (cityMode_ || cityCamSet_) {
            Camera& cam = cityMode_ ? camera_ : savedCam_;
            cam.position = {carPos_.x + rt.x * 3.0f, carPos_.y + 1.7f, carPos_.z + rt.z * 3.0f};
            cam.yaw = carYaw_;
            cam.pitch = 0.0f;
        }
        if (cityMode_) {
            flying_ = false;
            vy_ = 0.0f;
        }
    }
    driving_ = carOn_ = onTrack_ = carBoostOn_ = false;
    carVel_ = {};
    carSpeed_ = carVy_ = trackFade_ = scriptThrottle_ = scriptSteer_ = 0.0f;
    sfx_.setCar(false, 0, false);
    if (city_) city_->setDrivingEnabled(false);
    drivingPluginEnabled_ = false;
}

// Riding the race track: speed follows the throttle and the slope, steering slides across the deck, the camera follows the track's own frame.
void AppShell::updateRail(float dt, float throttle, float steer, bool boost, float lookX, float lookY) {
    const float L = city_->trackLength();
    City::TrackFrame f;
    city_->trackSample(trackS_, f);
    if (throttle > 0.05f) trackV_ += throttle * (boost ? 60.0f : 40.0f) * dt;
    else if (throttle < -0.05f) trackV_ += throttle * 55.0f * dt;
    trackV_ -= 9.0f * f.t.y * dt;
    trackV_ = clampf(trackV_, 24.0f, boost ? 100.0f : 78.0f);
    trackS_ += trackV_ * dt;
    const float lim = City::kTrackHalf - 1.4f;
    trackD_ = clampf(trackD_ + steer * 16.0f * dt, -lim, lim);
    carSteer_ += (steer - carSteer_) * std::min(1.0f, 8.0f * dt);
    if (trackS_ >= L - 1.0f) {
        city_->trackSample(L, f);
        onTrack_ = false;
        trackCool_ = 3.0f;
        carVel_ = f.flat * trackV_;
        carVy_ = 0.0f;
        carPos_ = {f.p.x, f.p.y, f.p.z};
        carYaw_ = std::atan2(-f.flat.x, -f.flat.z);
        carPitch_ = 0.0f;
        return;
    }
    city_->trackSample(trackS_, f);
    const Vec3 R = cross(f.t, f.u);
    carPos_ = f.p + R * trackD_;
    railT_ = f.t;
    railU_ = f.u;
    carYaw_ = std::atan2(-f.flat.x, -f.flat.z);
    carPitch_ = 0.0f;
    carSpeed_ = trackV_;
    carVel_ = f.t * trackV_;
    carSpin_ = std::fmod(carSpin_ + trackV_ * dt / 0.34f, 6.2831853f);

    lookOffX_ -= lookX;
    lookOffY_ -= lookY;
    if (lookX == 0.0f && lookY == 0.0f) {
        lookOffX_ *= std::exp(-2.5f * dt);
        lookOffY_ *= std::exp(-2.5f * dt);
    }
    lookOffX_ = clampf(lookOffX_, -3.0f, 3.0f);
    lookOffY_ = clampf(lookOffY_, -0.8f, 0.8f);
    if (carFirst_) {
        camera_.position = carPos_ + f.u * 1.15f - R * 0.38f;
        if (std::hypot(f.t.x, f.t.z) > 0.25f) railYaw_ = std::atan2(-f.t.x, -f.t.z);
        camera_.yaw = railYaw_ + lookOffX_;
        camera_.pitch = clampf(std::asin(clampf(f.t.y, -1.0f, 1.0f)) + lookOffY_, -1.3f, 1.3f);
    } else {
        const Vec3 target = carPos_ - f.flat * 8.5f + Vec3{0, 3.4f, 0};
        const float k = 1.0f - std::exp(-9.0f * dt);
        camera_.position = camera_.position + (target - camera_.position) * k;
        const Vec3 look = (carPos_ + f.u * 1.0f) - camera_.position;
        camera_.yaw = std::atan2(-look.x, -look.z) + lookOffX_;
        camera_.pitch = clampf(std::atan2(look.y, std::hypot(look.x, look.z)) + lookOffY_, -1.3f, 1.3f);
    }
}
// Arcade driving: strong lateral grip (cut by the handbrake for drifts), speed-scaled steering, gravity over ramps and slabs.
void AppShell::updateCar(double dtd, float throttle, float steer, bool handbrake, bool boost, float lookX, float lookY) {
    const float dt = std::min(float(dtd), 0.05f);
    trackCool_ = std::max(0.0f, trackCool_ - dt);
    carBoostOn_ = boost;
    if (onTrack_) {
        updateRail(dt, throttle, steer, boost, lookX, lookY);
        return;
    }
    const Vec3 fwd{-std::sin(carYaw_), 0, -std::cos(carYaw_)}, rt{std::cos(carYaw_), 0, -std::sin(carYaw_)};
    float vf = carVel_.x * fwd.x + carVel_.z * fwd.z, vl = carVel_.x * rt.x + carVel_.z * rt.z;
    if (trackCool_ <= 0.0f && vf > 4.0f && city_->trackReady()) {
        City::TrackFrame f0;
        for (float s = 0.0f; s <= 14.0f; s += 1.0f) {
            city_->trackSample(s, f0);
            const float dx = carPos_.x - f0.p.x, dz = carPos_.z - f0.p.z;
            const Vec3 R0 = cross(f0.t, f0.u);
            const float lat = dx * R0.x + dz * R0.z, lon = dx * f0.flat.x + dz * f0.flat.z;
            if (std::fabs(lat) < 3.2f && std::fabs(lon) < 1.6f && std::fabs(carPos_.y - f0.p.y) < 1.6f && fwd.x * f0.flat.x + fwd.z * f0.flat.z > 0.3f) {
                onTrack_ = true;
                sfx_.play(SfxId::RailOn);
                trackS_ = s;
                trackD_ = clampf(lat, -2.3f, 2.3f);
                trackV_ = std::max(vf, 22.0f);
                railYaw_ = carYaw_;
                updateRail(dt, throttle, steer, boost, lookX, lookY);
                return;
            }
        }
    }
    const float top = boost ? 100.0f : 62.0f;
    if (throttle > 0.05f) vf += throttle * (vf < 0 ? 70.0f : (boost ? 64.0f : 42.0f)) * dt;
    else if (throttle < -0.05f) vf += throttle * (vf > 0 ? 75.0f : 26.0f) * dt;
    else vf *= std::exp(-0.7f * dt);
    vf *= std::exp(-0.06f * dt);
    if (vf > top) vf = std::max(top, vf - 40.0f * dt);
    vf = std::max(vf, -18.0f);
    vl *= std::exp(-(handbrake ? 1.1f : 9.0f) * dt);
    if (handbrake) vf *= std::exp(-0.5f * dt);
    const float turn = steer * 2.3f * clampf(std::fabs(vf) / 7.0f, 0.0f, 1.0f) / (1.0f + std::fabs(vf) / 55.0f) * (handbrake ? 1.5f : 1.0f);
    carYaw_ -= turn * (vf < 0 ? -1.0f : 1.0f) * dt;
    carSteer_ += (steer - carSteer_) * std::min(1.0f, 8.0f * dt);
    const Vec3 nf{-std::sin(carYaw_), 0, -std::cos(carYaw_)}, nr{std::cos(carYaw_), 0, -std::sin(carYaw_)};
    carVel_ = {nf.x * vf + nr.x * vl, 0, nf.z * vf + nr.z * vl};

    Vec3 p{carPos_.x + carVel_.x * dt, carPos_.y + 1.0f, carPos_.z + carVel_.z * dt};
    const float want = std::hypot(carVel_.x, carVel_.z);
    Vec3 q = p;
    city_->resolve(q, 1.8f, 1.0f);
    const float px = q.x - p.x, pz = q.z - p.z, pl = std::hypot(px, pz);
    if (pl > 0.002f) {
        const float nx = px / pl, nz = pz / pl, into = carVel_.x * nx + carVel_.z * nz;
        if (into < 0) {
            carVel_.x -= nx * into * 1.15f;
            carVel_.z -= nz * into * 1.15f;
            carVel_.x *= 0.985f;
            carVel_.z *= 0.985f;
        }
    }
    carPos_.x = q.x;
    carPos_.z = q.z;
    const float maxR = city_->extent() * 1.3f, r = std::hypot(carPos_.x, carPos_.z);
    if (r > maxR) {
        carPos_.x *= maxR / r;
        carPos_.z *= maxR / r;
    }
    (void)want;
    // Vertical: gravity, snapping to the ground over small dips so ramps feel smooth.
    auto groundAt = [&](float x, float z) { return city_->floorAt({x, carPos_.y + 1.0f, z}, 0.6f, 1.0f); };
    const float fl = groundAt(carPos_.x, carPos_.z);
    carVy_ -= 32.0f * dt;
    carPos_.y += carVy_ * dt;
    if (carPos_.y <= fl || (carPos_.y - fl < 0.25f && carVy_ <= 0.0f)) {
        if (carPos_.y < fl - 0.001f || carVy_ <= 0) carPos_.y = fl;
        carVy_ = 0.0f;
    }
    const float hf = groundAt(carPos_.x + nf.x * 2.0f, carPos_.z + nf.z * 2.0f), hb = groundAt(carPos_.x - nf.x * 2.0f, carPos_.z - nf.z * 2.0f);
    const bool grounded = carPos_.y - fl < 0.3f;
    const float tp = grounded ? std::atan2(hf - hb, 4.0f) : clampf(carVy_ * 0.02f, -0.3f, 0.3f);
    carPitch_ += (tp - carPitch_) * std::min(1.0f, 10.0f * dt);
    carSpeed_ = vf;
    carSpin_ = std::fmod(carSpin_ + vf * static_cast<float>(dtd) / 0.34f, 6.2831853f);

    // Camera: chase with a lagging heading, or the driver's seat; the mouse looks around and springs back.
    lookOffX_ -= lookX;
    lookOffY_ -= lookY;
    if (lookX == 0.0f && lookY == 0.0f) {
        lookOffX_ *= std::exp(-2.5f * dt);
        lookOffY_ *= std::exp(-2.5f * dt);
    }
    lookOffX_ = clampf(lookOffX_, -3.0f, 3.0f);
    lookOffY_ = clampf(lookOffY_, -0.8f, 0.8f);
    float dy = carYaw_ - chaseYaw_;
    while (dy > kPi) dy -= 2.0f * kPi;
    while (dy < -kPi) dy += 2.0f * kPi;
    chaseYaw_ += dy * std::min(1.0f, 4.5f * dt);
    if (carFirst_) {
        const float y = carYaw_ + lookOffX_;
        const Vec3 f{-std::sin(carYaw_), 0, -std::cos(carYaw_)}, r2{std::cos(carYaw_), 0, -std::sin(carYaw_)};
        camera_.position = {carPos_.x + f.x * 0.1f - r2.x * 0.38f, carPos_.y + 1.12f, carPos_.z + f.z * 0.1f - r2.z * 0.38f};
        camera_.yaw = y;
        camera_.pitch = clampf(carPitch_ * 0.6f + lookOffY_, -1.2f, 1.2f);
    } else {
        const float y = chaseYaw_ + lookOffX_;
        const Vec3 f{-std::sin(y), 0, -std::cos(y)};
        const float back = 7.4f + std::min(carSpeed_, 100.0f) * 0.03f;
        Vec3 c{carPos_.x - f.x * back, carPos_.y + 2.7f, carPos_.z - f.z * back};
        c.y = std::max(c.y, city_->floorAt({c.x, c.y, c.z}, 0.5f, 0.0f) + 0.8f);
        camera_.position = c;
        camera_.yaw = y;
        camera_.pitch = clampf(-0.2f + lookOffY_, -1.2f, 1.0f);
    }
}

}  // namespace sf
