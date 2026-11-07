#include "AppShell.h"

#include <algorithm>
#include <cmath>
#include <SDL.h>

namespace sf {

Plugin AppShell::makeDrivingPlugin() {
    Plugin plugin{"org.scifishell.driving", "CAR AND TRACK", "Driving and track; trusted native code, not sandboxed", "1.0.0",
            [this] { drivingPluginEnabled_ = true; city_->setDrivingEnabled(true); },
            [this] { stopDrivingPlugin(); }};
    plugin.prepare = [this](std::string& error) {
        char* base = SDL_GetBasePath();
        if (!base) { error = std::string("Cannot locate driving module: ") + SDL_GetError(); return false; }
        const auto path = std::filesystem::u8path(base) / SCIFI_TRACK_MODULE_FILENAME;
        SDL_free(base);
        return city_->prepareDrivingModule(path, error);
    };
    plugin.cancelPrepare = [this] { city_->releaseDrivingModule(); };
    return plugin;
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
void AppShell::updateCar(double dt, float throttle, float steer, bool handbrake, bool boost, float lookX, float lookY) {
    const auto* api = city_ ? city_->drivingModule() : nullptr;
    if (!api) { std::fprintf(stderr, "Driving module is unavailable\n"); return; }
    const SfDrivingHostV1 host{
        sizeof(SfDrivingHostV1), SF_TRACK_SAMPLING_ABI, this,
        [](void* ctx) -> int { return static_cast<AppShell*>(ctx)->city_->trackReady(); },
        [](void* ctx) -> float { return static_cast<AppShell*>(ctx)->city_->trackLength(); },
        [](void* ctx, float distance, SfTrackFrame* out) -> int {
            City::TrackFrame f{};
            if (!out || !static_cast<AppShell*>(ctx)->city_->trackSample(distance, f)) return 0;
            *out = {{f.p.x, f.p.y, f.p.z}, {f.t.x, f.t.y, f.t.z},
                    {f.u.x, f.u.y, f.u.z}, {f.flat.x, f.flat.y, f.flat.z}, f.s};
            return 1;
        },
        [](void* ctx, SfTrackVec3* p, float radius, float eye) {
            Vec3 v{p->x, p->y, p->z};
            static_cast<AppShell*>(ctx)->city_->resolve(v, radius, eye);
            *p = {v.x, v.y, v.z};
        },
        [](void* ctx, SfTrackVec3 p, float radius, float eye) -> float {
            return static_cast<AppShell*>(ctx)->city_->floorAt({p.x, p.y, p.z}, radius, eye);
        },
        [](void* ctx) -> float { return static_cast<AppShell*>(ctx)->city_->extent(); },
        [](void* ctx) { static_cast<AppShell*>(ctx)->sfx_.play(SfxId::RailOn); }
    };
    SfDrivingStateV1 state{
        {carPos_.x, carPos_.y, carPos_.z}, {carVel_.x, carVel_.y, carVel_.z},
        {railT_.x, railT_.y, railT_.z}, {railU_.x, railU_.y, railU_.z},
        {camera_.position.x, camera_.position.y, camera_.position.z},
        carYaw_, carPitch_, carVy_, carSteer_, carSpeed_, carSpin_, chaseYaw_, lookOffX_, lookOffY_,
        trackS_, trackD_, trackV_, trackCool_, railYaw_, camera_.yaw, camera_.pitch,
        onTrack_, carBoostOn_, carFirst_
    };
    if (!api->drive(&host, &state, dt, throttle, steer, handbrake, boost, lookX, lookY)) {
        std::fprintf(stderr, "Driving simulation failed in module\n");
        return;
    }
    carPos_ = {state.car_pos.x, state.car_pos.y, state.car_pos.z};
    carVel_ = {state.car_vel.x, state.car_vel.y, state.car_vel.z};
    railT_ = {state.rail_t.x, state.rail_t.y, state.rail_t.z};
    railU_ = {state.rail_u.x, state.rail_u.y, state.rail_u.z};
    camera_.position = {state.camera_pos.x, state.camera_pos.y, state.camera_pos.z};
    carYaw_ = state.car_yaw; carPitch_ = state.car_pitch; carVy_ = state.car_vy;
    carSteer_ = state.car_steer; carSpeed_ = state.car_speed; carSpin_ = state.car_spin;
    chaseYaw_ = state.chase_yaw; lookOffX_ = state.look_x; lookOffY_ = state.look_y;
    trackS_ = state.track_s; trackD_ = state.track_d; trackV_ = state.track_v;
    trackCool_ = state.track_cool; railYaw_ = state.rail_yaw;
    camera_.yaw = state.camera_yaw; camera_.pitch = state.camera_pitch;
    onTrack_ = state.on_track != 0; carBoostOn_ = state.boost_on != 0;
}

}  // namespace sf
