#include "NativePluginApi.h"
#include "TrackSamplingApi.h"

#include <cmath>
#include <cstring>

int buildTrackModule(const SfTrackObstacle*, uint64_t, float, uint32_t, void*,
                     int (*)(void*, const SfTrackFrame*), int (*)(void*, const SfTrackStrut*),
                     int (*)(void*, const SfTrackLoop*));
int renderTrackModule(const SfTrackRenderV1*, SfTrackVec3, float);
int driveModule(const SfDrivingHostV1*, SfDrivingStateV1*, double, float, float, int, int, float, float);
int renderCarModule(const SfCarRenderV1*, const SfCarViewV1*, int*);

#ifdef _WIN32
#define SF_EXPORT __declspec(dllexport)
#else
#define SF_EXPORT __attribute__((visibility("default")))
#endif

namespace {
SfTrackVec3 interpolate(SfTrackVec3 a, SfTrackVec3 b, float u, bool normalize) {
    SfTrackVec3 v{a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u};
    if (normalize) {
        const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (length > 1e-8f) { v.x /= length; v.y /= length; v.z /= length; }
        else v = {0, 0, -1};
    }
    return v;
}
int sample(const SfTrackSourceV1* source, float s, SfTrackFrame* out) {
    if (!source || source->size != sizeof(*source) || source->abi != SF_TRACK_SAMPLING_ABI ||
        !source->count || !source->frame || !out) return 0;
    const uint64_t count = source->count(source->context);
    if (count < 2) return 0;
    SfTrackFrame last{};
    if (!source->frame(source->context, count - 1, &last)) return 0;
    s = s < 0 ? 0 : (s > last.s ? last.s : s);
    uint64_t lo = 0, hi = count - 1;
    while (hi - lo > 1) {
        const uint64_t mid = lo + (hi - lo) / 2;
        SfTrackFrame frame{};
        if (!source->frame(source->context, mid, &frame)) return 0;
        if (frame.s <= s) lo = mid; else hi = mid;
    }
    SfTrackFrame a{}, b{};
    if (!source->frame(source->context, lo, &a) || !source->frame(source->context, hi, &b)) return 0;
    float u = b.s > a.s ? (s - a.s) / (b.s - a.s) : 0;
    u = u < 0 ? 0 : (u > 1 ? 1 : u);
    *out = {interpolate(a.p, b.p, u, false), interpolate(a.t, b.t, u, true),
            interpolate(a.u, b.u, u, true), interpolate(a.flat, b.flat, u, true), s};
    return 1;
}
void* create() { static int instance; return &instance; }
int start(void*) { return 1; }
void stop(void*) {}
void destroy(void*) {}
const void* getInterface(void*, const char* id) {
    static const SfTrackSamplingV1 api{sizeof(SfTrackSamplingV1), SF_TRACK_SAMPLING_ABI,
                                      sample, buildTrackModule, renderTrackModule, driveModule, renderCarModule};
    return id && std::strcmp(id, SF_TRACK_SAMPLING_INTERFACE) == 0 ? &api : nullptr;
}
}

extern "C" SF_EXPORT const SfNativePluginV1* sf_plugin_query_v1() {
    static const SfNativePluginV1 plugin{sizeof(SfNativePluginV1), SF_NATIVE_PLUGIN_ABI,
        "org.scifishell.track-sampling", create, start, stop, getInterface, destroy};
    return &plugin;
}
