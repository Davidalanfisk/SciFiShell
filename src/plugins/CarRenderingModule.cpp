#include "TrackSamplingApi.h"
#include "Utils.h"

#include <algorithm>
#include <cmath>

using namespace sf;
namespace {
Vec3 vec(SfTrackVec3 v) { return {v.x, v.y, v.z}; }
}

int renderCarModule(const SfCarRenderV1* host, const SfCarViewV1* view, int* model) {
    if (!host || host->size != sizeof(*host) || host->abi != SF_TRACK_SAMPLING_ABI ||
        !host->model || !host->model_size || !host->mirror || !host->draw_model ||
        !host->draw_box || !view || !model) return 0;
    if (!view->show || view->spawn <= 0.01f) return 1;
    if (*model == -2) *model = host->model(host->context, "CarConcept");
    const Vec3 neon{0.1f, 0.85f, 1.0f};
    const float s = view->spawn * view->spawn * (3.0f - 2.0f * view->spawn);
    Mat4 orient = Mat4::rotateY(view->yaw);
    if (view->rail) {
        orient = Mat4::identity();
        const Vec3 t = vec(view->rail_t), u = vec(view->rail_u);
        const Vec3 ax[3] = {cross(t, u), u, t * -1.0f};
        for (int c = 0; c < 3; ++c) {
            orient.m[c * 4] = ax[c].x; orient.m[c * 4 + 1] = ax[c].y; orient.m[c * 4 + 2] = ax[c].z;
        }
    }
    const Mat4 base = Mat4::translate(vec(view->pos)) * orient;
    if (*model >= 0 && !(view->hide_body && !host->mirror(host->context))) {
        const Vec3 sz = vec(host->model_size(host->context, *model));
        const bool alongX = sz.x > sz.z;
        const float k = 4.6f / std::max(sz.x, sz.z) * s;
        const Mat4 m = base * Mat4::rotateX(view->pitch) * Mat4::rotateY(alongX ? -1.5708f : 3.14159265f) * Mat4::scale({k, k, k});
        const SfTrackVec3 col{neon.x * 0.22f, neon.y * 0.22f, neon.z * 0.22f};
        host->draw_model(host->context, *model, m.m, col, 0.3f);
        const float ang = -view->steer * 0.55f;
        const char* names[4] = {"CarConcept#FL", "CarConcept#FR", "CarConcept#RL", "CarConcept#RR"};
        for (int w = 0; w < 4; ++w) {
            const int wid = host->model(host->context, names[w]);
            if (wid < 0) continue;
            const Vec3 pv = vec(host->model_size(host->context, wid));
            const Mat4 steer = w < 2 ? Mat4::rotateY(ang) : Mat4::identity();
            const Mat4 wheel = m * Mat4::translate(pv) * steer * Mat4::rotateX(view->spin) * Mat4::translate({-pv.x, -pv.y, -pv.z});
            host->draw_model(host->context, wid, wheel.m, col, 0.3f);
        }
    }
    const float brake = view->speed < 0 ? 1.6f : 1.0f;
    const Mat4 lights = base * Mat4::translate({0, 0.62f, 2.3f}) * Mat4::scale({1.5f * s, 0.12f, 0.1f});
    host->draw_box(host->context, lights.m, {0.7f * brake, 0.07f * brake, 0.14f * brake});
    return 1;
}
