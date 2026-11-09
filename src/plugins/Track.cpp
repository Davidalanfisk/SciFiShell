// The race track: a glass deck on a grid that spirals up from the hub's CPU region, tours over the city with a couple of loops,
// and spirals back down. It is laid out once per city layout, riding above every building, and only drawn while driving.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "City.h"
#include "Renderer.h"

namespace sf {


void City::ensureTrack() {
    if (!trackDirty_ && !track_.empty()) return;
    trackDirty_ = false;
    buildTrack();
}

void City::buildTrack() {
    if (!trackSampling_) return;
    std::vector<SfTrackObstacle> obstacles;
    std::vector<Box> monuments;
    monumentBoxes(monuments);
    for (const Box& b : monuments) obstacles.push_back({b.x0, b.x1, b.z0, b.z1, b.y1});
    // Reserve all future plots too, so installing a package cannot invalidate an occupied track.
    obstacles.push_back({kDowntownWest, kDowntownEast, kDowntownNorth, kDowntownSouth, 16});
    for (const Platform& pl : platforms_) {
        if (pl.gone && pl.grow <= 0.0f) continue;
        obstacles.push_back({pl.cx - pl.side * 0.5f, pl.cx + pl.side * 0.5f,
                             pl.cz - pl.side * 0.5f, pl.cz + pl.side * 0.5f, 1.3f});
        for (const Block& b : pl.blocks)
            for (const Tower& t : b.towers) {
                const float w = std::max(t.curW, t.tgtW) * 0.5f + 0.5f;
                const float d = std::max(t.curD, t.tgtD) * 0.5f + 0.5f;
                obstacles.push_back({t.x - w, t.x + w, t.z - d, t.z + d,
                                     1.3f + std::max(t.curH, t.tgtH) + 2.0f});
            }
    }
    struct Output {
        std::vector<TrackFrame> frames;
        std::vector<TrackStrut> struts;
        std::vector<std::pair<float, float>> loops;
    } output;
    const int ok = trackSampling_->build(obstacles.data(), obstacles.size(), extent_,
        static_cast<uint32_t>(platforms_.size()), &output,
        [](void* context, const SfTrackFrame* f) -> int {
            try { static_cast<Output*>(context)->frames.push_back({{f->p.x, f->p.y, f->p.z},
                {f->t.x, f->t.y, f->t.z}, {f->u.x, f->u.y, f->u.z},
                {f->flat.x, f->flat.y, f->flat.z}, f->s}); return 1; }
            catch (const std::exception& e) { std::fprintf(stderr, "Track frame allocation failed: %s\n", e.what()); return 0; }
        },
        [](void* context, const SfTrackStrut* st) -> int {
            try { static_cast<Output*>(context)->struts.push_back({{st->p.x, st->p.y, st->p.z}, st->top}); return 1; }
            catch (const std::exception& e) { std::fprintf(stderr, "Track strut allocation failed: %s\n", e.what()); return 0; }
        },
        [](void* context, const SfTrackLoop* loop) -> int {
            try { static_cast<Output*>(context)->loops.emplace_back(loop->begin, loop->end); return 1; }
            catch (const std::exception& e) { std::fprintf(stderr, "Track loop allocation failed: %s\n", e.what()); return 0; }
        });
    if (!ok) {
        track_.clear(); struts_.clear(); loops_.clear();
        trackDirty_ = true;
        std::fprintf(stderr, "Track generation failed in driving module\n");
        return;
    }
    track_ = std::move(output.frames);
    struts_ = std::move(output.struts);
    loops_ = std::move(output.loops);
}

void City::renderTrack(Renderer& r, Vec3 eye) {
    if (!trackSampling_ || dry_) return;
    struct Context { const City* city; Renderer* renderer; } context{this, &r};
    const SfTrackRenderV1 host{
        sizeof(SfTrackRenderV1), SF_TRACK_SAMPLING_ABI, &context,
        [](void* ctx) -> uint64_t { return static_cast<const Context*>(ctx)->city->track_.size(); },
        [](void* ctx, uint64_t i, SfTrackFrame* out) -> int {
            const auto& frames = static_cast<const Context*>(ctx)->city->track_;
            if (!out || i >= frames.size()) return 0;
            const auto& f = frames[static_cast<size_t>(i)];
            *out = {{f.p.x, f.p.y, f.p.z}, {f.t.x, f.t.y, f.t.z},
                    {f.u.x, f.u.y, f.u.z}, {f.flat.x, f.flat.y, f.flat.z}, f.s};
            return 1;
        },
        [](void* ctx) -> uint64_t { return static_cast<const Context*>(ctx)->city->struts_.size(); },
        [](void* ctx, uint64_t i, SfTrackStrut* out) -> int {
            const auto& struts = static_cast<const Context*>(ctx)->city->struts_;
            if (!out || i >= struts.size()) return 0;
            const auto& st = struts[static_cast<size_t>(i)];
            *out = {{st.p.x, st.p.y, st.p.z}, st.top};
            return 1;
        },
        [](void* ctx) -> uint64_t { return static_cast<const Context*>(ctx)->city->loops_.size(); },
        [](void* ctx, uint64_t i, SfTrackLoop* out) -> int {
            const auto& loops = static_cast<const Context*>(ctx)->city->loops_;
            if (!out || i >= loops.size()) return 0;
            *out = {loops[static_cast<size_t>(i)].first, loops[static_cast<size_t>(i)].second};
            return 1;
        },
        [](void* ctx, SfTrackVec3 p, float radius) -> int {
            return static_cast<const Context*>(ctx)->renderer->visible({p.x, p.y, p.z}, radius);
        },
        [](void* ctx, const float* matrix, SfTrackVec3 color, float seed, float fade, float fill) -> int {
            auto* c = static_cast<const Context*>(ctx);
            Mat4 m;
            std::copy(matrix, matrix + 16, m.m);
            c->renderer->drawBuilding(*c->city->dummy_, m, {color.x, color.y, color.z}, seed, 10, fade, fill, 0.0f);
            return 1;
        }
    };
    if (!trackSampling_->render(&host, {eye.x, eye.y, eye.z}, trackFade_))
        std::fprintf(stderr, "Track rendering failed in driving module\n");
}

}  // namespace sf
