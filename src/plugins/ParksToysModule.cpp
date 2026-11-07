#include "ParksSceneApi.h"
#include "Utils.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <vector>
using namespace sf;
namespace {
SfTrackVec3 pod(Vec3 p) { return {p.x, p.y, p.z}; }
Vec3 vec(SfTrackVec3 p) { return {p.x, p.y, p.z}; }
float rnd01(uint32_t s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; s *= 2654435761u; s ^= s >> 15; return (s & 0xFFFFFF) / float(0x1000000); }
void require(int ok) { if (!ok) throw std::runtime_error("toys host callback failed"); }
struct Toys {
    SfParksToysV1 api;
    const SfParksToysHostV1* host = nullptr;
    struct Shot { Vec3 p, v; int kind = 0; float age = 0, boomT = 0; bool boom = false; };
    std::vector<Shot> shots_;
    int held_ = -1; Vec3 look_{0, 0, -1}, blast_{}; bool blastPending_ = false; double wall_ = 0;
    void drawItem(int, const Mat4&, float);
    void throwHeld(Vec3);
    bool takeBlast(Vec3&);
    void stepShots(double, Vec3);
    void renderToys(Vec3);
    void renderBoxM(const Mat4& m, Vec3 color) { require(host->draw_box(host->context, m.m, pod(color))); }
    void resolve(Vec3& p, float radius, float eye) { auto v = pod(p); host->resolve(host->context, &v, radius, eye); p = vec(v); }
    float floorAt(Vec3 p, float radius, float eye) { return host->floor_at(host->context, pod(p), radius, eye); }
    void popDucks(Vec3 p) { require(host->pop_ducks(host->context, pod(p))); }
    void sound(int id, Vec3 p) { require(host->sound(host->context, id, pod(p))); }
};
void Toys::drawItem(int kind, const Mat4& base, float glow) {
    auto part = [&](Vec3 at, Vec3 sz, Vec3 col) { renderBoxM(base * Mat4::translate(at) * Mat4::scale(sz), col * glow); };
    if (kind == 0) {
        part({0, 0, 0}, {0.5f, 0.15f, 0.2f}, {1.0f, 0.62f, 0.2f});         // bun
        part({0, 0.06f, 0}, {0.58f, 0.1f, 0.1f}, {1.0f, 0.25f, 0.25f});    // sausage
    } else {
        part({0, -0.2f, 0}, {0.12f, 0.1f, 0.12f}, {1.0f, 0.8f, 0.45f});    // cone
        part({0, -0.1f, 0}, {0.18f, 0.1f, 0.18f}, {1.0f, 0.8f, 0.45f});
        part({0, 0.04f, 0}, {0.26f, 0.2f, 0.26f}, {1.0f, 0.45f, 0.8f});    // scoop
        part({0, 0.22f, 0}, {0.16f, 0.15f, 0.16f}, {1.0f, 0.7f, 0.9f});
    }
}
void Toys::throwHeld(Vec3 eye) {
    if (held_ < 0) return;
    const Vec3 up{0, 1, 0};
    const Vec3 right = normalize(cross(look_, up));
    Shot s;
    s.p = eye + look_ * 1.0f + right * 0.5f + up * -0.35f;
    s.v = look_ * 15.0f + up * 3.5f;
    s.kind = held_;
    shots_.push_back(s);
    if (shots_.size() > 8) shots_.erase(shots_.begin());
    held_ = -1;
    sound(1, s.p);
}
bool Toys::takeBlast(Vec3& impulse) {
    if (!blastPending_) return false;
    blastPending_ = false;
    impulse = blast_;
    return true;
}
void Toys::stepShots(double dt, Vec3 eye) {
    const float fdt = float(dt);
    for (auto& s : shots_) {
        if (s.boom) {
            s.boomT += fdt;
            continue;
        }
        s.age += fdt;
        s.v.y -= 20.0f * fdt;
        Vec3 np = s.p + s.v * fdt;
        Vec3 q = np;
        resolve(q, 0.2f, 0.0f);
        if (std::fabs(q.x - np.x) > 1e-4f) s.v.x *= -0.35f;
        if (std::fabs(q.z - np.z) > 1e-4f) s.v.z *= -0.35f;
        np.x = q.x;
        np.z = q.z;
        const float fl = floorAt(np, 0.2f, 0.0f) + 0.15f;
        if (np.y < fl) {
            np.y = fl;
            if (s.v.y < 0.0f) s.v.y = std::fabs(s.v.y) > 2.0f ? -s.v.y * 0.35f : 0.0f;
            s.v.x *= 0.8f;
            s.v.z *= 0.8f;
        }
        s.p = np;
        if (s.age >= 5.0f) {
            s.boom = true;
            sound(2, s.p);
            popDucks(s.p);
            const Vec3 away = eye - s.p;
            const float d = length(away);
            const float kRange = 18.0f;
            if (d < kRange) {
                const float f = 1.0f - d / kRange;
                Vec3 h{away.x, 0, away.z};
                h = length(h) > 0.01f ? normalize(h) : Vec3{0, 0, 1};
                blast_ = {h.x * 30.0f * f, 5.0f + 8.0f * f, h.z * 30.0f * f};
                blastPending_ = true;
            }
        }
    }
    shots_.erase(std::remove_if(shots_.begin(), shots_.end(), [](const Shot& s) { return s.boom && s.boomT > 1.3f; }), shots_.end());
}

void Toys::renderToys(Vec3 eye) {
    const Vec3 up{0, 1, 0};
    if (held_ >= 0) {
        const Vec3 right = normalize(cross(look_, up));
        const float bob = 0.02f * std::sin(float(wall_) * 2.0f);
        const Vec3 at = eye + look_ * 1.2f + right * 0.55f + up * (-0.42f + bob);
        drawItem(held_, Mat4::translate(at) * Mat4::rotateY(std::atan2(look_.x, look_.z) + 1.5708f) * Mat4::scale({1.6f, 1.6f, 1.6f}), 0.8f);
    }
    for (const auto& s : shots_) {
        const Vec3 warm = s.kind == 0 ? Vec3{1.0f, 0.5f, 0.15f} : Vec3{1.0f, 0.3f, 0.8f};
        if (!s.boom) {
            // Tumbles in flight and blinks faster as the fuse runs down.
            const float blink = s.age > 3.0f ? 0.5f + 0.5f * std::sin(s.age * (6.0f + (s.age - 3.0f) * 8.0f)) : 1.0f;
            drawItem(s.kind, Mat4::translate(s.p) * Mat4::rotateY(s.age * 6.0f) * Mat4::rotateX(s.age * 8.0f), 0.5f + 0.7f * blink);
            continue;
        }
        const float t = std::min(1.0f, s.boomT / 1.2f), e = 1.0f - (1.0f - t) * (1.0f - t), fade = (1.0f - t);
        const Vec3 cyan{0.3f, 0.9f, 1.0f};
        for (int i = 0; i < 3; ++i) {
            const float sz = (4.0f + 4.0f * i) * e + 0.3f;
            renderBoxM(Mat4::translate(s.p) * Mat4::rotateY(t * (1.0f + i)) * Mat4::scale({sz, sz, sz}), (i == 1 ? cyan : warm) * (1.3f * fade));
        }
        renderBoxM(Mat4::translate(s.p) * Mat4::scale({1.5f * fade + 0.2f, 1.5f * fade + 0.2f, 1.5f * fade + 0.2f}), Vec3{1.6f, 1.6f, 1.6f} * fade);
        for (int i = 0; i < 12; ++i) {
            const float a = rnd01(uint32_t(i) * 77u + 1u) * 6.2832f, el = rnd01(uint32_t(i) * 131u + 5u) * 1.2f - 0.1f;
            const float sp = 5.0f + 6.0f * rnd01(uint32_t(i) * 17u + 9u);
            const Vec3 dir{std::cos(a) * std::cos(el), std::sin(el), std::sin(a) * std::cos(el)};
            const Vec3 at = s.p + dir * (sp * s.boomT) + up * (-6.0f * s.boomT * s.boomT);
            const float sz = 0.45f * fade + 0.05f;
            renderBoxM(Mat4::translate(at) * Mat4::rotateY(s.boomT * 7.0f + i) * Mat4::rotateX(s.boomT * 5.0f) * Mat4::scale({sz, sz, sz}), warm * (1.4f * fade));
        }
    }
}

// Street lights round the block, and a park (lawn, trees, benches, a pond) in every cell with no building on it.

bool valid(const SfParksToysHostV1* host) {
    return host && host->size == sizeof(*host) && host->abi == SF_PARKS_SCENE_ABI && host->resolve &&
           host->floor_at && host->pop_ducks && host->sound && host->draw_box;
}
struct HostLease {
    Toys* toys;
    HostLease(Toys* value, const SfParksToysHostV1* host) : toys(value) { toys->host = host; }
    ~HostLease() { toys->host = nullptr; }
};
}
void* createParksToys() {
    auto* toys = new (std::nothrow) Toys{};
    if (!toys) return nullptr;
    toys->api = {sizeof(SfParksToysV1), SF_PARKS_SCENE_ABI, toys,
        [](void* ctx, int kind) { if (kind == 0 || kind == 1) static_cast<Toys*>(ctx)->held_ = kind; },
        [](void* ctx) -> int { return static_cast<Toys*>(ctx)->held_ >= 0; },
        [](void* ctx, SfTrackVec3 look) { static_cast<Toys*>(ctx)->look_ = vec(look); },
        [](void* ctx, const SfParksToysHostV1* host, SfTrackVec3 eye) -> int {
            if (!valid(host)) return 0; auto* t = static_cast<Toys*>(ctx); HostLease lease(t, host);
            try { t->throwHeld(vec(eye)); return 1; } catch (const std::exception& e) { std::fprintf(stderr, "Throw failed: %s\n", e.what()); return 0; }
        },
        [](void* ctx, const SfParksToysHostV1* host, double dt, SfTrackVec3 eye) -> int {
            if (!valid(host)) return 0; auto* t = static_cast<Toys*>(ctx); HostLease lease(t, host);
            try { t->stepShots(dt, vec(eye)); return 1; } catch (const std::exception& e) { std::fprintf(stderr, "Toy update failed: %s\n", e.what()); return 0; }
        },
        [](void* ctx, const SfParksToysHostV1* host, double wall, SfTrackVec3 eye) -> int {
            if (!valid(host)) return 0; auto* t = static_cast<Toys*>(ctx); HostLease lease(t, host); t->wall_ = wall;
            try { t->renderToys(vec(eye)); return 1; } catch (const std::exception& e) { std::fprintf(stderr, "Toy rendering failed: %s\n", e.what()); return 0; }
        },
        [](void* ctx, SfTrackVec3* out) -> int {
            if (!out) return 0; Vec3 v; if (!static_cast<Toys*>(ctx)->takeBlast(v)) return 0; *out = pod(v); return 1;
        }};
    return &toys->api;
}
void destroyParksToys(void* context) {
    auto* api = static_cast<SfParksToysV1*>(context);
    if (api) delete static_cast<Toys*>(api->context);
}
