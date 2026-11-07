#include "TrackSamplingApi.h"
#include "Utils.h"

using namespace sf;
namespace {
Vec3 vec(SfTrackVec3 v) { return {v.x, v.y, v.z}; }
}

int renderTrackModule(const SfTrackRenderV1* host, SfTrackVec3 eyeValue, float fade) {
    if (!host || host->size != sizeof(*host) || host->abi != SF_TRACK_SAMPLING_ABI ||
        !host->frame_count || !host->frame || !host->strut_count || !host->strut ||
        !host->loop_count || !host->loop || !host->visible || !host->draw) return 0;
    const uint64_t count = host->frame_count(host->context);
    if (fade <= 0.01f || count < 2) return 1;
    SfTrackFrame last{};
    if (!host->frame(host->context, count - 1, &last) || last.s <= 0) return 0;
    const Vec3 eye = vec(eyeValue);
    const SfTrackVec3 cyan{0.1f, 0.85f, 1.0f}, mag{1.0f, 0.3f, 0.9f};
    const uint64_t loopCount = host->loop_count(host->context);
    uint64_t li = 0;
    SfTrackLoop loop{};
    if (loopCount && !host->loop(host->context, 0, &loop)) return 0;
    for (uint64_t i = 0; i + 1 < count; ++i) {
        SfTrackFrame f{}, g{};
        if (!host->frame(host->context, i, &f) || !host->frame(host->context, i + 1, &g)) return 0;
        while (li < loopCount && f.s > loop.end) {
            ++li;
            if (li < loopCount && !host->loop(host->context, li, &loop)) return 0;
        }
        const Vec3 p = vec(f.p), d = p - eye;
        if (dot(d, d) > 400.0f * 400.0f) continue;
        const float e = clampf(fade * 2.2f - (f.s / last.s) * 1.2f, 0.0f, 1.0f);
        if (e < 0.01f || !host->visible(host->context, f.p, 5.0f)) continue;
        const Vec3 seg = vec(g.p) - p;
        const Vec3 t = vec(f.t), u = vec(f.u), rt = cross(t, u);
        const Vec3 ax[3] = {rt * 7.4f, u * 0.35f, t * (length(seg) + 0.2f)};
        const Vec3 c = p + seg * 0.5f - u * 0.175f;
        float m[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, c.x, c.y, c.z, 1};
        for (int k = 0; k < 3; ++k) { m[k * 4] = ax[k].x; m[k * 4 + 1] = ax[k].y; m[k * 4 + 2] = ax[k].z; }
        if (!host->draw(host->context, m, li < loopCount && f.s >= loop.begin ? mag : cyan, f.s, e, 0)) return 0;
    }
    const float e = clampf(fade * 1.6f, 0.0f, 1.0f);
    for (uint64_t i = 0; i < host->strut_count(host->context); ++i) {
        SfTrackStrut st{};
        if (!host->strut(host->context, i, &st)) return 0;
        const Vec3 d = vec(st.p) - eye;
        if (dot(d, d) > 400.0f * 400.0f ||
            !host->visible(host->context, {st.p.x, st.top * 0.5f, st.p.z}, st.top * 0.5f + 1.0f)) continue;
        const float m[16] = {0.7f, 0, 0, 0, 0, st.top, 0, 0, 0, 0, 0.7f, 0, st.p.x, st.top * 0.5f, st.p.z, 1};
        if (!host->draw(host->context, m, cyan, 0, e, 1)) return 0;
    }
    return 1;
}
