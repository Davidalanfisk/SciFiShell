#include "ParksSceneApi.h"
#include "Utils.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace sf;
namespace {
constexpr float kSlabH = 1.0f, kPlateH = 0.3f;
float rnd01(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5; s *= 2654435761u; s ^= s >> 15;
    return (s & 0xFFFFFF) / float(0x1000000);
}
SfTrackVec3 pod(Vec3 v) { return {v.x, v.y, v.z}; }
void require(int ok) { if (!ok) throw std::runtime_error("parks host callback failed"); }
void renderBox(const SfParksSceneHostV1* h, Vec3 c, Vec3 size, Vec3 col, float seed, int kind, float emerge, float fill, float flash) {
    require(h->draw_box(h->context, pod(c), pod(size), pod(col), seed, kind, emerge, fill, flash));
}
void renderBoxM(const SfParksSceneHostV1* h, const Mat4& m, Vec3 col) {
    require(h->draw_box_matrix(h->context, m.m, pod(col)));
}
}
int renderParksScene(const SfParksSceneHostV1* host, const SfParksBlockV1* input, int* outputDraws) {
    if (!host || host->size != sizeof(*host) || host->abi != SF_PARKS_SCENE_ABI || !input || !outputDraws ||
        !host->tower_count || !host->tower || !host->duck_alive || !host->stand || !host->pond || !host->duck ||
        !host->draw_box || !host->draw_box_matrix || input->grid < 0) return 0;
    try {
    struct Tower { float x, z; };
    struct Block { float cx, cz, side; int g; std::vector<Tower> towers; } b{input->cx, input->cz, input->side, input->grid, {}};
    struct Platform { float pitch; } p{input->pitch};
    const auto count = host->tower_count(host->context);
    b.towers.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) { Tower t{}; require(host->tower(host->context, i, &t.x, &t.z)); b.towers.push_back(t); }
    const Vec3 tint{input->tint.x, input->tint.y, input->tint.z};
    const Vec3 lodEye_{input->eye.x, input->eye.y, input->eye.z};
    const double wall_ = input->wall;
    const uint32_t seed = input->seed; const bool detail = input->detail != 0;
    int draws = *outputDraws;

    const float y0 = kSlabH + kPlateH;
    const Vec3 green{0.2f, 0.85f, 0.45f}, water{0.2f, 0.6f, 1.0f};
    std::vector<char> used(size_t(b.g) * b.g, 0);
    for (const auto& t : b.towers) {
        const int ix = (int)std::lround((t.x - b.cx) / p.pitch + (b.g - 1) * 0.5f), iz = (int)std::lround((t.z - b.cz) / p.pitch + (b.g - 1) * 0.5f);
        if (ix >= 0 && ix < b.g && iz >= 0 && iz < b.g) used[size_t(iz) * b.g + ix] = 1;
    }
    auto tree = [&](float x, float z, float k) {
        const float h = 2.2f + 1.6f * k;
        renderBox(host, {x, y0 + h * 0.5f, z}, {0.4f, h, 0.4f}, green * 0.3f, 0.5f, 5, 1, 0, 0);
        renderBox(host, {x, y0 + h + 1.1f, z}, {2.8f + k, 2.4f, 2.8f + k}, green, 0.5f, 6, 1, 0, 0);
        draws += 2;
    };
    auto bench = [&](float x, float z, bool alongX) {
        const Vec3 c = tint * 0.8f;
        renderBox(host, {x, y0 + 0.45f, z}, alongX ? Vec3{1.8f, 0.1f, 0.6f} : Vec3{0.6f, 0.1f, 1.8f}, c, 0.5f, 5, 1, 0, 0);
        renderBox(host, {x + (alongX ? 0.0f : 0.28f), y0 + 0.75f, z + (alongX ? 0.28f : 0.0f)}, alongX ? Vec3{1.8f, 0.5f, 0.08f} : Vec3{0.08f, 0.5f, 1.8f}, c * 0.7f, 0.5f, 5, 1, 0, 0);
        draws += 2;
    };
    const Vec3 warm{1.0f, 0.55f, 0.2f}, pink{1.0f, 0.5f, 0.75f};
    // A hot dog cart: a body on two wheels with a striped awning. `dir` turns the serving side.
    auto hotdogCart = [&](float x, float z, bool alongX) {
        require(host->stand(host->context, x, z, 0));
        const Vec3 sz = alongX ? Vec3{2.0f, 1.0f, 1.0f} : Vec3{1.0f, 1.0f, 2.0f};
        renderBox(host, {x, y0 + 0.9f, z}, sz, warm * 0.7f, 0.5f, 5, 1, 0, 0);
        renderBox(host, {x, y0 + 1.6f, z}, alongX ? Vec3{2.4f, 0.12f, 1.4f} : Vec3{1.4f, 0.12f, 2.4f}, warm, 0.5f, 5, 1, 0, 0);
        renderBox(host, {x, y0 + 0.35f, z}, alongX ? Vec3{0.2f, 0.7f, 1.1f} : Vec3{1.1f, 0.7f, 0.2f}, tint * 0.4f, 0.5f, 5, 1, 0, 0);
        draws += 3;
    };
    // An ice cream stand: a small kiosk with a cone on the roof.
    auto iceCream = [&](float x, float z) {
        require(host->stand(host->context, x, z, 1));
        renderBox(host, {x, y0 + 0.9f, z}, {1.6f, 1.8f, 1.6f}, pink * 0.6f, 0.5f, 5, 1, 0, 0);
        renderBox(host, {x, y0 + 1.9f, z}, {2.0f, 0.12f, 2.0f}, pink, 0.5f, 5, 1, 0, 0);
        renderBox(host, {x, y0 + 2.45f, z}, {0.5f, 0.9f, 0.5f}, {0.95f, 0.9f, 0.8f}, 0.5f, 5, 1, 0, 0);
        renderBox(host, {x, y0 + 3.0f, z}, {0.8f, 0.5f, 0.8f}, pink, 0.5f, 5, 1, 0, 0);
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
            if (!host->duck_alive(host->context, s)) continue;
            require(host->duck(host->context, s, {x, y0 + 0.4f, z}));
            const Vec3 body{1.0f, 0.95f, 0.5f}, beak{1.0f, 0.55f, 0.15f};
            renderBoxM(host, base * Mat4::translate({0, 0, 0}) * Mat4::scale({0.5f, 0.4f, 0.8f}), body);
            renderBoxM(host, base * Mat4::translate({0, 0.12f, -0.45f}) * Mat4::scale({0.2f, 0.2f, 0.25f}), body);
            renderBoxM(host, base * Mat4::translate({0, 0.42f, 0.35f}) * Mat4::scale({0.28f, 0.3f, 0.28f}), body);
            renderBoxM(host, base * Mat4::translate({0, 0.38f, 0.58f}) * Mat4::scale({0.16f, 0.07f, 0.2f}), beak);
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
            renderBox(host, {x, y0 + 0.03f, z}, {20.0f, 0.06f, 20.0f}, green, 0.5f, 6, 1, 5.0f, 0);
            ++draws;
            const float ex = lodEye_.x - x, ez = lodEye_.z - z;
            if (!detail && ex * ex + ez * ez > 90.0f * 90.0f) {
                // Far away a park is drawn as a few canopies (and the pond, if it has one) instead of a bare lawn.
                const float lv = rnd01(sd);
                if (lv < 0.2f || lv >= 0.85f) renderBox(host, {x, y0 + 0.07f, z}, lv < 0.2f ? Vec3{9.0f, 0.06f, 9.0f} : Vec3{14.0f, 0.06f, 5.0f}, water, 0.5f, 6, 1, 6.0f, 0);
                const int nt = lv < 0.7f && lv >= 0.4f ? 5 : 3;
                for (int q = 0; q < nt; ++q) {
                    const float tx = x + (rnd01(sd + 50 + q) - 0.5f) * 15.0f, tz = z + (rnd01(sd + 60 + q) - 0.5f) * 15.0f;
                    renderBox(host, {tx, y0 + 3.2f, tz}, {3.2f, 3.0f, 3.2f}, green, 0.5f, 6, 1, 0, 0);
                }
                draws += nt + 1;
                continue;
            }
            const float v = rnd01(sd);
            const Vec3 stone{0.75f, 0.85f, 0.9f};
            if (v < 0.2f) {
                // Pond park: a pond ringed with benches and two trees.
                require(host->pond(host->context, {x, y0, z}));
                renderBox(host, {x, y0 + 0.07f, z}, {9.0f, 0.06f, 9.0f}, water, 0.5f, 6, 1, 6.0f, 0);
                bench(x, z - 6.4f, true);
                bench(x, z + 6.4f, true);
                tree(x - 7.0f, z - 7.0f, rnd01(sd + 1));
                tree(x + 7.0f, z + 7.0f, rnd01(sd + 2));
                ducks(x, z, 6.5f, 6.5f, sd);
                draws += 1;
            } else if (v < 0.4f) {
                // Cross-path garden with four trees.
                renderBox(host, {x, y0 + 0.07f, z}, {20.0f, 0.05f, 2.2f}, tint, 0.5f, 6, 1, 4.0f, 0);
                renderBox(host, {x, y0 + 0.07f, z}, {2.2f, 0.05f, 20.0f}, tint, 0.5f, 6, 1, 4.0f, 0);
                bench(x + 3.5f, z - 2.2f, true);
                tree(x - 6.5f, z - 6.5f, rnd01(sd + 3));
                tree(x + 6.5f, z - 6.5f, rnd01(sd + 4));
                tree(x - 6.5f, z + 6.5f, rnd01(sd + 5));
                tree(x + 6.5f, z + 6.5f, rnd01(sd + 6));
                draws += 2;
            } else if (v < 0.55f) {
                // Fountain plaza: a paved square with a stepped fountain and benches facing it.
                renderBox(host, {x, y0 + 0.07f, z}, {14.0f, 0.05f, 14.0f}, tint, 0.5f, 6, 1, 4.0f, 0);
                renderBox(host, {x, y0 + 0.3f, z}, {5.0f, 0.5f, 5.0f}, stone * 0.5f, 0.5f, 5, 1, 0, 0);
                renderBox(host, {x, y0 + 0.62f, z}, {4.0f, 0.2f, 4.0f}, water, 0.5f, 6, 1, 6.0f, 0);
                renderBox(host, {x, y0 + 1.5f, z}, {0.7f, 2.0f, 0.7f}, water, 0.5f, 5, 1, 0, 0);
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
                renderBox(host, {x, y0 + 0.07f, z}, {16.0f, 0.05f, 16.0f}, tint, 0.5f, 6, 1, 4.0f, 0);
                hotdogCart(x - 4.0f, z - 3.0f, true);
                iceCream(x + 4.0f, z + 3.0f);
                bench(x, z + 6.5f, true);
                tree(x - 7.0f, z + 7.0f, rnd01(sd + 9));
                tree(x + 7.0f, z - 7.0f, rnd01(sd + 10));
                draws += 2;
            } else {
                // Duck pond: a long narrow pond with a path along it and benches.
                renderBox(host, {x, y0 + 0.07f, z}, {14.0f, 0.06f, 5.0f}, water, 0.5f, 6, 1, 6.0f, 0);
                renderBox(host, {x, y0 + 0.07f, z + 6.0f}, {20.0f, 0.05f, 2.2f}, tint, 0.5f, 6, 1, 4.0f, 0);
                bench(x - 4.0f, z + 4.4f, true);
                bench(x + 4.0f, z + 4.4f, true);
                tree(x - 7.0f, z - 7.0f, rnd01(sd + 11));
                tree(x + 7.0f, z - 7.0f, rnd01(sd + 12));
                hotdogCart(x, z - 7.5f, true);
                ducks(x, z, 8.0f, 5.0f, sd);
                draws += 3;
            }
        }

    *outputDraws = draws; return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Parks scene failed: %s\n", e.what()); return 0;
    } catch (...) {
        std::fprintf(stderr, "Parks scene failed: unknown exception\n"); return 0;
    }
}
