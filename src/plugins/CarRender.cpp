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
        releaseDrivingModule();
    }
}

// The sports car: the glTF mesh drawn as a neon hologram, with an underglow pool, headlight beams and tail lights.
void City::renderCar(Renderer& r) {
    if (!trackSampling_) return;
    struct Context { City* city; Renderer* renderer; } context{this, &r};
    const SfCarRenderV1 host{
        sizeof(SfCarRenderV1), SF_TRACK_SAMPLING_ABI, &context,
        [](void* ctx, const char* name) -> int { return static_cast<Context*>(ctx)->renderer->model(name); },
        [](void* ctx, int id) -> SfTrackVec3 {
            const Vec3 v = static_cast<Context*>(ctx)->renderer->modelSize(id);
            return {v.x, v.y, v.z};
        },
        [](void* ctx) -> int { return static_cast<Context*>(ctx)->renderer->mirror(); },
        [](void* ctx, int id, const float* matrix, SfTrackVec3 color, float alpha) {
            const auto* c = static_cast<Context*>(ctx); Mat4 m; std::copy(matrix, matrix + 16, m.m);
            c->renderer->drawModel(id, *c->city->dummy_, m, {color.x, color.y, color.z}, alpha, false);
        },
        [](void* ctx, const float* matrix, SfTrackVec3 color) {
            auto* c = static_cast<Context*>(ctx); Mat4 m; std::copy(matrix, matrix + 16, m.m);
            c->city->renderBoxM(*c->renderer, m, {color.x, color.y, color.z});
        }
    };
    const SfCarViewV1 view{{car_.pos.x, car_.pos.y, car_.pos.z},
        {car_.railT.x, car_.railT.y, car_.railT.z}, {car_.railU.x, car_.railU.y, car_.railU.z},
        car_.yaw, car_.pitch, car_.steer, car_.speed, car_.spawn, car_.spin,
        car_.show, car_.hideBody, car_.rail};
    if (!trackSampling_->render_car(&host, &view, &carModel_))
        std::fprintf(stderr, "Car rendering failed in driving module\n");
}

}  // namespace sf
