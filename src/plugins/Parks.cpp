#include "City.h"
#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace sf {

namespace {
constexpr float kSlabH = 1.0f, kPlateH = 0.3f;
}

void City::setParksEnabled(bool enabled) {
    parksEnabled_ = enabled;
    if (!enabled) {
        deadDucks_.clear();
        sfxQ_.clear();
        scorePending_ = 0;
        releaseParksModule();
    }
}

// A wireframe snack built from a few outlined boxes, in the local frame of `base` (origin = its centre).
SfParksToysHostV1 City::toysHost(ToysContext* context) {
    return {sizeof(SfParksToysHostV1), SF_PARKS_SCENE_ABI, context,
        [](void* ctx, SfTrackVec3* p, float radius, float eye) {
            auto* c = static_cast<ToysContext*>(ctx); Vec3 v{p->x, p->y, p->z};
            c->city->resolve(v, radius, eye); *p = {v.x, v.y, v.z};
        },
        [](void* ctx, SfTrackVec3 p, float radius, float eye) -> float {
            return static_cast<ToysContext*>(ctx)->city->floorAt({p.x, p.y, p.z}, radius, eye);
        },
        [](void* ctx, SfTrackVec3 p) -> int {
            try { static_cast<ToysContext*>(ctx)->city->popDucks({p.x, p.y, p.z}); return 1; }
            catch (const std::exception& e) { std::fprintf(stderr, "Duck update failed: %s\n", e.what()); return 0; }
        },
        [](void* ctx, int id, SfTrackVec3 p) -> int {
            try { static_cast<ToysContext*>(ctx)->city->sfxQ_.push_back({id, {p.x, p.y, p.z}}); return 1; }
            catch (const std::exception& e) { std::fprintf(stderr, "Toy sound failed: %s\n", e.what()); return 0; }
        },
        [](void* ctx, const float* matrix, SfTrackVec3 color) -> int {
            auto* c = static_cast<ToysContext*>(ctx); if (!c->renderer || !matrix) return 0;
            Mat4 m; std::copy(matrix, matrix + 16, m.m);
            c->city->renderBoxM(*c->renderer, m, {color.x, color.y, color.z}); return 1;
        }};
}

void City::giveItem(int kind) { if (parksEnabled_ && parksToys_) parksToys_->give(parksToys_->context, kind); }
bool City::holding() const { return parksToys_ && parksToys_->holding(parksToys_->context); }
void City::setLook(Vec3 look) { if (parksToys_) parksToys_->set_look(parksToys_->context, {look.x, look.y, look.z}); }
void City::throwHeld(Vec3 eye) {
    if (!parksEnabled_ || !parksToys_) return;
    ToysContext context{this, nullptr}; const auto host = toysHost(&context);
    if (!parksToys_->throw_held(parksToys_->context, &host, {eye.x, eye.y, eye.z}))
        std::fprintf(stderr, "Toy throw failed in parks module\n");
}
bool City::takeBlast(Vec3& impulse) {
    if (!parksToys_) return false; SfTrackVec3 v{};
    if (!parksToys_->take_blast(parksToys_->context, &v)) return false;
    impulse = {v.x, v.y, v.z}; return true;
}
void City::stepShots(double dt, Vec3 eye) {
    if (!parksToys_) return; ToysContext context{this, nullptr}; const auto host = toysHost(&context);
    if (!parksToys_->step(parksToys_->context, &host, dt, {eye.x, eye.y, eye.z}))
        std::fprintf(stderr, "Toy simulation failed in parks module\n");
}
void City::renderToys(Renderer& r, Vec3 eye) {
    if (!parksToys_) return; ToysContext context{this, &r}; const auto host = toysHost(&context);
    if (!parksToys_->render(parksToys_->context, &host, wall_, {eye.x, eye.y, eye.z}))
        std::fprintf(stderr, "Toy rendering failed in parks module\n");
}

bool City::debugPond(Vec3& pos) {
    if (!parksEnabled_ || !built_) return false;
    std::vector<Vec3> ponds;
    pondSink_ = &ponds;
    dry_ = true;
    int draws = 0;
    for (size_t i = 0; i < platforms_.size(); ++i)
        for (size_t bi = 0; bi < platforms_[i].blocks.size(); ++bi)
            renderBlockScenery(renderer_, platforms_[i], platforms_[i].blocks[bi], uint32_t(i * 7919 + bi * 104729), tintColor(platforms_[i].tint), true, draws);
    dry_ = false;
    pondSink_ = nullptr;
    if (ponds.empty()) return false;
    pos = ponds[0];
    for (const auto& q : ponds) if (q.x * q.x + q.z * q.z < pos.x * pos.x + pos.z * pos.z) pos = q;
    return true;
}

bool City::pickStand(Vec3 o, Vec3 d, int& kind) {
    if (!parksEnabled_ || !built_) return false;
    std::vector<Stand> st;
    standSink_ = &st;
    dry_ = true;
    int draws = 0;
    for (size_t i = 0; i < platforms_.size(); ++i) {
        auto& p = platforms_[i];
        if (p.grow <= 0.95f) continue;
        for (size_t bi = 0; bi < p.blocks.size(); ++bi) {
            auto& b = p.blocks[bi];
            const float dx = o.x - b.cx, dz = o.z - b.cz, lim = b.side * 0.75f + 18.0f;
            if (dx * dx + dz * dz > lim * lim) continue;
            renderBlockScenery(renderer_, p, b, uint32_t(i * 7919 + bi * 104729), tintColor(p.tint), true, draws);
        }
    }
    dry_ = false;
    standSink_ = nullptr;
    const float y0 = kSlabH + kPlateH;
    float best = 1e9f;
    for (const auto& s : st) {
        const Vec3 c{s.x, y0 + 1.4f, s.z};
        const Vec3 oc = c - o;
        const float t = dot(oc, d);
        if (t < 0.0f || t > 14.0f) continue;
        if (dot(oc, oc) - t * t < 1.7f * 1.7f && t < best) {
            best = t;
            kind = s.kind;
        }
    }
    return best < 1e8f;
}

bool City::takeScore(int& points, int& total) {
    if (!scorePending_) return false;
    points = scorePending_;
    total = scoreTotal_;
    scorePending_ = 0;
    return true;
}

void City::popDucks(Vec3 at) {
    std::vector<DuckPos> ducks;
    duckSink_ = &ducks;
    dry_ = true;
    int draws = 0;
    for (size_t i = 0; i < platforms_.size(); ++i) {
        auto& p = platforms_[i];
        for (size_t bi = 0; bi < p.blocks.size(); ++bi) {
            auto& b = p.blocks[bi];
            const float dx = at.x - b.cx, dz = at.z - b.cz, lim = b.side * 0.75f + 10.0f;
            if (dx * dx + dz * dz > lim * lim) continue;
            renderBlockScenery(renderer_, p, b, uint32_t(i * 7919 + bi * 104729), tintColor(p.tint), true, draws);
        }
    }
    dry_ = false;
    duckSink_ = nullptr;
    for (const auto& d : ducks)
        if (length(d.p - at) < 7.0f) {
            deadDucks_[d.key] = wall_;
            sfxQ_.push_back({3, d.p});
            scorePending_ += 100;
            scoreTotal_ += 100;
        }
}

void City::renderBlockScenery(Renderer& r, const Platform& p, const Block& b, uint32_t seed, Vec3 tint, bool detail, int& draws) {
    if (!parksScene_) return;
    struct Context { City* city; Renderer* renderer; const Block* block; } context{this, &r, &b};
    const SfParksSceneHostV1 host{
        sizeof(SfParksSceneHostV1), SF_PARKS_SCENE_ABI, &context,
        [](void* ctx) -> uint64_t { return static_cast<Context*>(ctx)->block->towers.size(); },
        [](void* ctx, uint64_t i, float* x, float* z) -> int {
            const auto& towers = static_cast<Context*>(ctx)->block->towers;
            if (i >= towers.size() || !x || !z) return 0;
            *x = towers[static_cast<size_t>(i)].x; *z = towers[static_cast<size_t>(i)].z; return 1;
        },
        [](void* ctx, uint32_t key) -> int {
            const auto* city = static_cast<Context*>(ctx)->city; const auto it = city->deadDucks_.find(key);
            return it == city->deadDucks_.end() || city->wall_ - it->second >= 90.0;
        },
        [](void* ctx, float x, float z, int kind) -> int {
            auto* city = static_cast<Context*>(ctx)->city;
            try { if (city->standSink_) city->standSink_->push_back({x, z, kind}); return 1; }
            catch (const std::exception& e) { std::fprintf(stderr, "Stand collection failed: %s\n", e.what()); return 0; }
        },
        [](void* ctx, SfTrackVec3 p) -> int {
            auto* city = static_cast<Context*>(ctx)->city;
            try { if (city->pondSink_) city->pondSink_->push_back({p.x, p.y, p.z}); return 1; }
            catch (const std::exception& e) { std::fprintf(stderr, "Pond collection failed: %s\n", e.what()); return 0; }
        },
        [](void* ctx, uint32_t key, SfTrackVec3 p) -> int {
            auto* city = static_cast<Context*>(ctx)->city;
            try { if (city->duckSink_) city->duckSink_->push_back({key, {p.x, p.y, p.z}}); return 1; }
            catch (const std::exception& e) { std::fprintf(stderr, "Duck collection failed: %s\n", e.what()); return 0; }
        },
        [](void* ctx, SfTrackVec3 pos, SfTrackVec3 size, SfTrackVec3 color, float seed, int kind, float emerge, float fill, float flash) -> int {
            auto* c = static_cast<Context*>(ctx);
            c->city->renderBox(*c->renderer, {pos.x, pos.y, pos.z}, {size.x, size.y, size.z},
                               {color.x, color.y, color.z}, seed, kind, emerge, fill, flash); return 1;
        },
        [](void* ctx, const float* matrix, SfTrackVec3 color) -> int {
            auto* c = static_cast<Context*>(ctx); Mat4 m; std::copy(matrix, matrix + 16, m.m);
            c->city->renderBoxM(*c->renderer, m, {color.x, color.y, color.z}); return 1;
        }
    };
    const SfParksBlockV1 block{p.pitch, b.cx, b.cz, b.side, b.g, {tint.x, tint.y, tint.z},
        {lodEye_.x, lodEye_.y, lodEye_.z}, wall_, seed, detail};
    if (!parksScene_->render(&host, &block, &draws)) std::fprintf(stderr, "Parks scene rendering failed in module\n");
}



}  // namespace sf
