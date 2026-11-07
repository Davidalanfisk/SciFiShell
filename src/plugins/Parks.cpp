#include "City.h"
#include "Renderer.h"

#include <algorithm>
#include <cmath>

namespace sf {

namespace {
constexpr float kSlabH = 1.0f, kPlateH = 0.3f;
float rnd01(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    s *= 2654435761u;
    s ^= s >> 15;
    return (s & 0xFFFFFF) / float(0x1000000);
}
}

void City::setParksEnabled(bool enabled) {
    parksEnabled_ = enabled;
    if (!enabled) {
        held_ = -1;
        shots_.clear();
        deadDucks_.clear();
        sfxQ_.clear();
        scorePending_ = 0;
        blastPending_ = false;
    }
}

// A wireframe snack built from a few outlined boxes, in the local frame of `base` (origin = its centre).
void City::drawItem(Renderer& r, int kind, const Mat4& base, float glow) {
    auto part = [&](Vec3 at, Vec3 sz, Vec3 col) { renderBoxM(r, base * Mat4::translate(at) * Mat4::scale(sz), col * glow); };
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

void City::throwHeld(Vec3 eye) {
    if (!parksEnabled_ || held_ < 0) return;
    const Vec3 up{0, 1, 0};
    const Vec3 right = normalize(cross(look_, up));
    Shot s;
    s.p = eye + look_ * 1.0f + right * 0.5f + up * -0.35f;
    s.v = look_ * 15.0f + up * 3.5f;
    s.kind = held_;
    shots_.push_back(s);
    if (shots_.size() > 8) shots_.erase(shots_.begin());
    held_ = -1;
    sfxQ_.push_back({1, s.p});
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

bool City::takeBlast(Vec3& impulse) {
    if (!blastPending_) return false;
    blastPending_ = false;
    impulse = blast_;
    return true;
}

void City::stepShots(double dt, Vec3 eye) {
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
            sfxQ_.push_back({2, s.p});
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

void City::renderToys(Renderer& r, Vec3 eye) {
    const Vec3 up{0, 1, 0};
    if (held_ >= 0) {
        const Vec3 right = normalize(cross(look_, up));
        const float bob = 0.02f * std::sin(float(wall_) * 2.0f);
        const Vec3 at = eye + look_ * 1.2f + right * 0.55f + up * (-0.42f + bob);
        drawItem(r, held_, Mat4::translate(at) * Mat4::rotateY(std::atan2(look_.x, look_.z) + 1.5708f) * Mat4::scale({1.6f, 1.6f, 1.6f}), 0.8f);
    }
    for (const auto& s : shots_) {
        const Vec3 warm = s.kind == 0 ? Vec3{1.0f, 0.5f, 0.15f} : Vec3{1.0f, 0.3f, 0.8f};
        if (!s.boom) {
            // Tumbles in flight and blinks faster as the fuse runs down.
            const float blink = s.age > 3.0f ? 0.5f + 0.5f * std::sin(s.age * (6.0f + (s.age - 3.0f) * 8.0f)) : 1.0f;
            drawItem(r, s.kind, Mat4::translate(s.p) * Mat4::rotateY(s.age * 6.0f) * Mat4::rotateX(s.age * 8.0f), 0.5f + 0.7f * blink);
            continue;
        }
        const float t = std::min(1.0f, s.boomT / 1.2f), e = 1.0f - (1.0f - t) * (1.0f - t), fade = (1.0f - t);
        const Vec3 cyan{0.3f, 0.9f, 1.0f};
        for (int i = 0; i < 3; ++i) {
            const float sz = (4.0f + 4.0f * i) * e + 0.3f;
            renderBoxM(r, Mat4::translate(s.p) * Mat4::rotateY(t * (1.0f + i)) * Mat4::scale({sz, sz, sz}), (i == 1 ? cyan : warm) * (1.3f * fade));
        }
        renderBoxM(r, Mat4::translate(s.p) * Mat4::scale({1.5f * fade + 0.2f, 1.5f * fade + 0.2f, 1.5f * fade + 0.2f}), Vec3{1.6f, 1.6f, 1.6f} * fade);
        for (int i = 0; i < 12; ++i) {
            const float a = rnd01(uint32_t(i) * 77u + 1u) * 6.2832f, el = rnd01(uint32_t(i) * 131u + 5u) * 1.2f - 0.1f;
            const float sp = 5.0f + 6.0f * rnd01(uint32_t(i) * 17u + 9u);
            const Vec3 dir{std::cos(a) * std::cos(el), std::sin(el), std::sin(a) * std::cos(el)};
            const Vec3 at = s.p + dir * (sp * s.boomT) + up * (-6.0f * s.boomT * s.boomT);
            const float sz = 0.45f * fade + 0.05f;
            renderBoxM(r, Mat4::translate(at) * Mat4::rotateY(s.boomT * 7.0f + i) * Mat4::rotateX(s.boomT * 5.0f) * Mat4::scale({sz, sz, sz}), warm * (1.4f * fade));
        }
    }
}

// Street lights round the block, and a park (lawn, trees, benches, a pond) in every cell with no building on it.
void City::renderBlockScenery(Renderer& r, const Platform& p, const Block& b, uint32_t seed, Vec3 tint, bool detail, int& draws) {
    const float y0 = kSlabH + kPlateH;
    const Vec3 green{0.2f, 0.85f, 0.45f}, water{0.2f, 0.6f, 1.0f};
    std::vector<char> used(size_t(b.g) * b.g, 0);
    for (const auto& t : b.towers) {
        const int ix = (int)std::lround((t.x - b.cx) / p.pitch + (b.g - 1) * 0.5f), iz = (int)std::lround((t.z - b.cz) / p.pitch + (b.g - 1) * 0.5f);
        if (ix >= 0 && ix < b.g && iz >= 0 && iz < b.g) used[size_t(iz) * b.g + ix] = 1;
    }
    auto tree = [&](float x, float z, float k) {
        const float h = 2.2f + 1.6f * k;
        renderBox(r, {x, y0 + h * 0.5f, z}, {0.4f, h, 0.4f}, green * 0.3f, 0.5f, 5, 1, 0, 0);
        renderBox(r, {x, y0 + h + 1.1f, z}, {2.8f + k, 2.4f, 2.8f + k}, green, 0.5f, 6, 1, 0, 0);
        draws += 2;
    };
    auto bench = [&](float x, float z, bool alongX) {
        const Vec3 c = tint * 0.8f;
        renderBox(r, {x, y0 + 0.45f, z}, alongX ? Vec3{1.8f, 0.1f, 0.6f} : Vec3{0.6f, 0.1f, 1.8f}, c, 0.5f, 5, 1, 0, 0);
        renderBox(r, {x + (alongX ? 0.0f : 0.28f), y0 + 0.75f, z + (alongX ? 0.28f : 0.0f)}, alongX ? Vec3{1.8f, 0.5f, 0.08f} : Vec3{0.08f, 0.5f, 1.8f}, c * 0.7f, 0.5f, 5, 1, 0, 0);
        draws += 2;
    };
    const Vec3 warm{1.0f, 0.55f, 0.2f}, pink{1.0f, 0.5f, 0.75f};
    // A hot dog cart: a body on two wheels with a striped awning. `dir` turns the serving side.
    auto hotdogCart = [&](float x, float z, bool alongX) {
        if (standSink_) standSink_->push_back({x, z, 0});
        const Vec3 sz = alongX ? Vec3{2.0f, 1.0f, 1.0f} : Vec3{1.0f, 1.0f, 2.0f};
        renderBox(r, {x, y0 + 0.9f, z}, sz, warm * 0.7f, 0.5f, 5, 1, 0, 0);
        renderBox(r, {x, y0 + 1.6f, z}, alongX ? Vec3{2.4f, 0.12f, 1.4f} : Vec3{1.4f, 0.12f, 2.4f}, warm, 0.5f, 5, 1, 0, 0);
        renderBox(r, {x, y0 + 0.35f, z}, alongX ? Vec3{0.2f, 0.7f, 1.1f} : Vec3{1.1f, 0.7f, 0.2f}, tint * 0.4f, 0.5f, 5, 1, 0, 0);
        draws += 3;
    };
    // An ice cream stand: a small kiosk with a cone on the roof.
    auto iceCream = [&](float x, float z) {
        if (standSink_) standSink_->push_back({x, z, 1});
        renderBox(r, {x, y0 + 0.9f, z}, {1.6f, 1.8f, 1.6f}, pink * 0.6f, 0.5f, 5, 1, 0, 0);
        renderBox(r, {x, y0 + 1.9f, z}, {2.0f, 0.12f, 2.0f}, pink, 0.5f, 5, 1, 0, 0);
        renderBox(r, {x, y0 + 2.45f, z}, {0.5f, 0.9f, 0.5f}, {0.95f, 0.9f, 0.8f}, 0.5f, 5, 1, 0, 0);
        renderBox(r, {x, y0 + 3.0f, z}, {0.8f, 0.5f, 0.8f}, pink, 0.5f, 5, 1, 0, 0);
        draws += 4;
    };
    // Wireframe ducks (one or two) that wander about a park on slow looping paths and never leave its bounds.
    auto ducks = [&](float cx, float cz, float hx, float hz, uint32_t sd) {
        const int n = rnd01(sd + 90) < 0.5f ? 1 : 2;
        const float t = float(wall_);
        for (int k = 0; k < n; ++k) {
            const uint32_t s = sd + 100u * uint32_t(k + 1);
            const float w1 = 0.12f + 0.12f * rnd01(s + 1), w2 = 0.2f + 0.15f * rnd01(s + 2), w3 = 0.12f + 0.12f * rnd01(s + 3), w4 = 0.2f + 0.15f * rnd01(s + 4);
            const float p1 = rnd01(s + 5) * 6.28f, p2 = rnd01(s + 6) * 6.28f, p3 = rnd01(s + 7) * 6.28f, p4 = rnd01(s + 8) * 6.28f;
            const float x = cx + hx * (0.6f * std::sin(w1 * t + p1) + 0.4f * std::sin(w2 * t + p2));
            const float z = cz + hz * (0.6f * std::sin(w3 * t + p3) + 0.4f * std::sin(w4 * t + p4));
            const float dx = hx * (0.6f * w1 * std::cos(w1 * t + p1) + 0.4f * w2 * std::cos(w2 * t + p2));
            const float dz = hz * (0.6f * w3 * std::cos(w3 * t + p3) + 0.4f * w4 * std::cos(w4 * t + p4));
            const Mat4 base = Mat4::translate({x, y0 + 0.2f + 0.03f * std::sin(t * 2.0f + p1), z}) * Mat4::rotateY(std::atan2(dx, dz));
            auto dead = deadDucks_.find(s);
            if (dead != deadDucks_.end() && wall_ - dead->second < 90.0) continue;
            if (duckSink_) duckSink_->push_back({s, {x, y0 + 0.4f, z}});
            const Vec3 body{1.0f, 0.95f, 0.5f}, beak{1.0f, 0.55f, 0.15f};
            renderBoxM(r, base * Mat4::translate({0, 0, 0}) * Mat4::scale({0.5f, 0.4f, 0.8f}), body);
            renderBoxM(r, base * Mat4::translate({0, 0.12f, -0.45f}) * Mat4::scale({0.2f, 0.2f, 0.25f}), body);
            renderBoxM(r, base * Mat4::translate({0, 0.42f, 0.35f}) * Mat4::scale({0.28f, 0.3f, 0.28f}), body);
            renderBoxM(r, base * Mat4::translate({0, 0.38f, 0.58f}) * Mat4::scale({0.16f, 0.07f, 0.2f}), beak);
            draws += 4;
        }
    };
    // Carts and stands among the towers: on the pad edge between street lights, so dense blocks get them too.
    if (detail) {
        const float half = b.side * 0.5f - 4.2f;
        for (int s = 0; s < 4; ++s)
            for (int k = 0; k < b.g; ++k) {
                const uint32_t hs = seed * 31u + uint32_t(s * 64 + k) * 40503u;
                if (rnd01(hs) > 0.16f) continue;
                const float along = (float(k) + 0.5f - b.g * 0.5f) * p.pitch;
                const bool xs = s < 2;
                const float x = b.cx + (xs ? along : (s == 2 ? -half : half)), z = b.cz + (xs ? (s == 0 ? -half : half) : along);
                if (rnd01(hs + 9) < 0.5f) hotdogCart(x, z, xs);
                else iceCream(x, z);
            }
    }
    for (int cz = 0; cz < b.g; ++cz)
        for (int cx = 0; cx < b.g; ++cx) {
            if (used[size_t(cz) * b.g + cx]) continue;
            const float x = b.cx + (float(cx) - (b.g - 1) * 0.5f) * p.pitch, z = b.cz + (float(cz) - (b.g - 1) * 0.5f) * p.pitch;
            const uint32_t sd = seed + uint32_t(cz * b.g + cx) * 2654435u;
            renderBox(r, {x, y0 + 0.03f, z}, {20.0f, 0.06f, 20.0f}, green, 0.5f, 6, 1, 5.0f, 0);
            ++draws;
            const float ex = lodEye_.x - x, ez = lodEye_.z - z;
            if (!detail && ex * ex + ez * ez > 90.0f * 90.0f) {
                // Far away a park is drawn as a few canopies (and the pond, if it has one) instead of a bare lawn.
                const float lv = rnd01(sd);
                if (lv < 0.2f || lv >= 0.85f) renderBox(r, {x, y0 + 0.07f, z}, lv < 0.2f ? Vec3{9.0f, 0.06f, 9.0f} : Vec3{14.0f, 0.06f, 5.0f}, water, 0.5f, 6, 1, 6.0f, 0);
                const int nt = lv < 0.7f && lv >= 0.4f ? 5 : 3;
                for (int q = 0; q < nt; ++q) {
                    const float tx = x + (rnd01(sd + 50 + q) - 0.5f) * 15.0f, tz = z + (rnd01(sd + 60 + q) - 0.5f) * 15.0f;
                    renderBox(r, {tx, y0 + 3.2f, tz}, {3.2f, 3.0f, 3.2f}, green, 0.5f, 6, 1, 0, 0);
                }
                draws += nt + 1;
                continue;
            }
            const float v = rnd01(sd);
            const Vec3 stone{0.75f, 0.85f, 0.9f};
            if (v < 0.2f) {
                // Pond park: a pond ringed with benches and two trees.
                if (pondSink_) pondSink_->push_back({x, y0, z});
                renderBox(r, {x, y0 + 0.07f, z}, {9.0f, 0.06f, 9.0f}, water, 0.5f, 6, 1, 6.0f, 0);
                bench(x, z - 6.4f, true);
                bench(x, z + 6.4f, true);
                tree(x - 7.0f, z - 7.0f, rnd01(sd + 1));
                tree(x + 7.0f, z + 7.0f, rnd01(sd + 2));
                ducks(x, z, 6.5f, 6.5f, sd);
                draws += 1;
            } else if (v < 0.4f) {
                // Cross-path garden with four trees.
                renderBox(r, {x, y0 + 0.07f, z}, {20.0f, 0.05f, 2.2f}, tint, 0.5f, 6, 1, 4.0f, 0);
                renderBox(r, {x, y0 + 0.07f, z}, {2.2f, 0.05f, 20.0f}, tint, 0.5f, 6, 1, 4.0f, 0);
                bench(x + 3.5f, z - 2.2f, true);
                tree(x - 6.5f, z - 6.5f, rnd01(sd + 3));
                tree(x + 6.5f, z - 6.5f, rnd01(sd + 4));
                tree(x - 6.5f, z + 6.5f, rnd01(sd + 5));
                tree(x + 6.5f, z + 6.5f, rnd01(sd + 6));
                draws += 2;
            } else if (v < 0.55f) {
                // Fountain plaza: a paved square with a stepped fountain and benches facing it.
                renderBox(r, {x, y0 + 0.07f, z}, {14.0f, 0.05f, 14.0f}, tint, 0.5f, 6, 1, 4.0f, 0);
                renderBox(r, {x, y0 + 0.3f, z}, {5.0f, 0.5f, 5.0f}, stone * 0.5f, 0.5f, 5, 1, 0, 0);
                renderBox(r, {x, y0 + 0.62f, z}, {4.0f, 0.2f, 4.0f}, water, 0.5f, 6, 1, 6.0f, 0);
                renderBox(r, {x, y0 + 1.5f, z}, {0.7f, 2.0f, 0.7f}, water, 0.5f, 5, 1, 0, 0);
                bench(x - 5.2f, z, false);
                bench(x + 5.2f, z, false);
                tree(x - 7.5f, z - 7.5f, rnd01(sd + 7));
                tree(x + 7.5f, z + 7.5f, rnd01(sd + 8));
                draws += 6;
            } else if (v < 0.7f) {
                // Grove: a dense stand of trees on open lawn.
                for (int q = 0; q < 6; ++q) tree(x + (rnd01(sd + 20 + q) - 0.5f) * 15.0f, z + (rnd01(sd + 30 + q) - 0.5f) * 15.0f, rnd01(sd + 40 + q));
                bench(x, z + 8.0f, true);
                draws += 1;
            } else if (v < 0.85f) {
                // Market square: paved, with a hot dog cart, an ice cream stand and a bench.
                renderBox(r, {x, y0 + 0.07f, z}, {16.0f, 0.05f, 16.0f}, tint, 0.5f, 6, 1, 4.0f, 0);
                hotdogCart(x - 4.0f, z - 3.0f, true);
                iceCream(x + 4.0f, z + 3.0f);
                bench(x, z + 6.5f, true);
                tree(x - 7.0f, z + 7.0f, rnd01(sd + 9));
                tree(x + 7.0f, z - 7.0f, rnd01(sd + 10));
                draws += 2;
            } else {
                // Duck pond: a long narrow pond with a path along it and benches.
                renderBox(r, {x, y0 + 0.07f, z}, {14.0f, 0.06f, 5.0f}, water, 0.5f, 6, 1, 6.0f, 0);
                renderBox(r, {x, y0 + 0.07f, z + 6.0f}, {20.0f, 0.05f, 2.2f}, tint, 0.5f, 6, 1, 4.0f, 0);
                bench(x - 4.0f, z + 4.4f, true);
                bench(x + 4.0f, z + 4.4f, true);
                tree(x - 7.0f, z - 7.0f, rnd01(sd + 11));
                tree(x + 7.0f, z - 7.0f, rnd01(sd + 12));
                hotdogCart(x, z - 7.5f, true);
                ducks(x, z, 8.0f, 5.0f, sd);
                draws += 3;
            }
        }
}


}  // namespace sf
