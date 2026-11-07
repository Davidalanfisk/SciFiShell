#include "TrackSamplingApi.h"
#include "Utils.h"
#include <algorithm>
#include <cmath>

using namespace sf;
namespace {
Vec3 fromPod(SfTrackVec3 v) { return {v.x, v.y, v.z}; }
SfTrackVec3 toPod(Vec3 v) { return {v.x, v.y, v.z}; }
struct City {
    struct TrackFrame { Vec3 p, t, u, flat; float s; };
    static constexpr float kTrackHalf = 3.7f;
    const SfDrivingHostV1* host;
    float trackLength() const { return host->track_length(host->context); }
    bool trackReady() const { return host->track_ready(host->context) != 0; }
    bool trackSample(float s, TrackFrame& out) const {
        SfTrackFrame f{};
        if (!host->track_sample(host->context, s, &f)) return false;
        out = {fromPod(f.p), fromPod(f.t), fromPod(f.u), fromPod(f.flat), f.s};
        return true;
    }
    void resolve(Vec3& p, float radius, float eye) const {
        auto value = toPod(p); host->resolve(host->context, &value, radius, eye); p = fromPod(value);
    }
    float floorAt(Vec3 p, float radius, float eye) const { return host->floor_at(host->context, toPod(p), radius, eye); }
    float extent() const { return host->extent(host->context); }
};
struct DrivingSimulation {
    explicit DrivingSimulation(const SfDrivingHostV1* host) : city{host}, city_(&city), host_(host) {}
    City city; City* city_; const SfDrivingHostV1* host_;
    struct Camera { Vec3 position; float yaw, pitch; } camera_;
    Vec3 carPos_, carVel_, railT_, railU_;
    float carYaw_, carPitch_, carVy_, carSteer_, carSpeed_, carSpin_, chaseYaw_, lookOffX_, lookOffY_;
    float trackS_, trackD_, trackV_, trackCool_, railYaw_;
    bool onTrack_, carBoostOn_, carFirst_;
    void updateRail(float, float, float, bool, float, float);
    void updateCar(double, float, float, bool, bool, float, float);
};
void DrivingSimulation::updateRail(float dt, float throttle, float steer, bool boost, float lookX, float lookY) {
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
void DrivingSimulation::updateCar(double dtd, float throttle, float steer, bool handbrake, bool boost, float lookX, float lookY) {
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
                host_->rail_on(host_->context);
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

} // namespace

int driveModule(const SfDrivingHostV1* host, SfDrivingStateV1* state, double dt, float throttle, float steer, int handbrake, int boost, float lookX, float lookY) {
    if (!host || host->size != sizeof(*host) || host->abi != SF_TRACK_SAMPLING_ABI || !state ||
        !host->track_ready || !host->track_length || !host->track_sample || !host->resolve ||
        !host->floor_at || !host->extent || !host->rail_on) return 0;
    DrivingSimulation d(host);
    d.carPos_ = fromPod(state->car_pos); d.carVel_ = fromPod(state->car_vel);
    d.railT_ = fromPod(state->rail_t); d.railU_ = fromPod(state->rail_u);
    d.camera_ = {fromPod(state->camera_pos), state->camera_yaw, state->camera_pitch};
    d.carYaw_ = state->car_yaw; d.carPitch_ = state->car_pitch; d.carVy_ = state->car_vy;
    d.carSteer_ = state->car_steer; d.carSpeed_ = state->car_speed; d.carSpin_ = state->car_spin;
    d.chaseYaw_ = state->chase_yaw; d.lookOffX_ = state->look_x; d.lookOffY_ = state->look_y;
    d.trackS_ = state->track_s; d.trackD_ = state->track_d; d.trackV_ = state->track_v;
    d.trackCool_ = state->track_cool; d.railYaw_ = state->rail_yaw;
    d.onTrack_ = state->on_track != 0; d.carBoostOn_ = state->boost_on != 0; d.carFirst_ = state->first_person != 0;
    d.updateCar(dt, throttle, steer, handbrake != 0, boost != 0, lookX, lookY);
    state->car_pos = toPod(d.carPos_); state->car_vel = toPod(d.carVel_);
    state->rail_t = toPod(d.railT_); state->rail_u = toPod(d.railU_);
    state->camera_pos = toPod(d.camera_.position); state->camera_yaw = d.camera_.yaw; state->camera_pitch = d.camera_.pitch;
    state->car_yaw = d.carYaw_; state->car_pitch = d.carPitch_; state->car_vy = d.carVy_;
    state->car_steer = d.carSteer_; state->car_speed = d.carSpeed_; state->car_spin = d.carSpin_;
    state->chase_yaw = d.chaseYaw_; state->look_x = d.lookOffX_; state->look_y = d.lookOffY_;
    state->track_s = d.trackS_; state->track_d = d.trackD_; state->track_v = d.trackV_;
    state->track_cool = d.trackCool_; state->rail_yaw = d.railYaw_;
    state->on_track = d.onTrack_; state->boost_on = d.carBoostOn_; state->first_person = d.carFirst_;
    return 1;
}
