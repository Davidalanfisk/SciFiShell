#include "City.h"

#include <algorithm>
#include <queue>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "TextRender.h"
#include "Theme.h"
#include "Thumbnails.h"

namespace sf {

namespace {
constexpr float kRoadY = 0.08f;
constexpr float kSlabH = 1.0f;
constexpr float kPlateH = 0.3f;
constexpr float kPadSlope = 1.4f;  // width of the sloped rim round each block pad
constexpr float kGrowSeconds = 4.5f;

float rnd01(uint32_t s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    s *= 2654435761u;
    s ^= s >> 15;
    return (s & 0xFFFFFF) / float(0x1000000);
}

std::string fmtCount(uint64_t n) {
    char b[32];
    if (n >= 1000000) std::snprintf(b, sizeof b, "%.1fM", n / 1e6);
    else if (n >= 1000) std::snprintf(b, sizeof b, "%.1fK", n / 1e3);
    else std::snprintf(b, sizeof b, "%llu", (unsigned long long)n);
    return b;
}

std::string fmtBytes(uint64_t n) {
    char b[32];
    double g = n / 1073741824.0;
    if (g >= 1024) std::snprintf(b, sizeof b, "%.1f TB", g / 1024);
    else if (g >= 10) std::snprintf(b, sizeof b, "%.0f GB", g);
    else if (g >= 0.1) std::snprintf(b, sizeof b, "%.1f GB", g);
    else std::snprintf(b, sizeof b, "%.0f MB", n / 1048576.0);
    return b;
}

std::string upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
}  // namespace
namespace {
constexpr int kStepN = 7;  // treads in the ring round each slab: a gentle stair every foot can climb
constexpr float kStepTread = 0.5f, kStepRise = 1.0f / 8.0f;
constexpr float kStRun = 0.5f, kStMaxRise = 0.25f, kStDeck = 4.5f, kStHalfW = 2.8f;  // station stairs and deck
constexpr int kRampSteps = 8;
constexpr float kRampStep = 0.8f, kRampW = 7.0f;
struct RampStep { float cx, cz, sx, sz, top; };
// Step k=0 is the lowest and furthest from the slab edge; the last step is flush with the slab top.
RampStep rampStep(float x, float z, int dir, int k) {
    const float dist = (kRampSteps - 1 - k) * kRampStep + kRampStep * 0.5f, top = kSlabH * float(k + 1) / kRampSteps;
    switch (dir) {
        case 0: return {x + dist, z, kRampStep, kRampW, top};
        case 1: return {x - dist, z, kRampStep, kRampW, top};
        case 2: return {x, z + dist, kRampW, kRampStep, top};
        default: return {x, z - dist, kRampW, kRampStep, top};
    }
}
}  // namespace

City::City(Renderer& renderer) : renderer_(renderer) {
    Bitmap b;
    b.w = b.h = 1;
    b.rgba = {255, 255, 255, 255};
    dummy_ = renderer.createLabel(b);
}

City::~City() { data_.stop(); }

float City::emergeProgress() const { return (float)std::min(1.0, clock_ / 8.0); }

Vec3 City::tintColor(int i) const {
    // Dark blue / cyan / teal range with white-ish steel greys mixed in. Hue steps by the golden ratio and saturation and
    // brightness drift on their own periods, so every index gets a distinct colour however many drives and regions exist.
    constexpr float kDim = 0.68f;  // keeps the regions dark and moody
    if (i == 0) return currentTheme().accent * 0.8f * kDim;
    auto fr = [](float v) { return v - std::floor(v); };
    const float u = fr(i * 0.6180339f), v = fr(i * 0.7548777f), w = fr(i * 0.5698403f);
    Vec3 c;
    if (i % 5 == 3) {
        const float l = 0.42f + 0.50f * v;  // cool whites, each a slightly different temperature
        c = Vec3{l * (0.82f + 0.12f * u), l * (0.90f + 0.08f * w), l};
    } else {
        const float h = 0.44f + 0.22f * u;  // 160 to 240 degrees: sea-green through cyan to blue
        const float sat = 0.62f + 0.36f * w, val = 0.34f + 0.46f * v;
        const float hh = h * 6.0f;
        const float f = hh - std::floor(hh);
        const float p = val * (1.0f - sat), q = val * (1.0f - sat * f), tt = val * (1.0f - sat * (1.0f - f));
        switch ((int)std::floor(hh) % 6) {
            case 2: c = {p, val, tt}; break;
            case 3: c = {p, q, val}; break;
            default: c = {tt, p, val}; break;
        }
    }
    return c * kDim;
}
std::shared_ptr<GpuLabel> City::makeText(const std::string& s) {
    return renderer_.createLabel(makeLabel(s, 54.0f, FontFace::UI, Color8{}, 96, 4));
}

void City::layoutPlatforms(const std::vector<CityDrive>& snap) {
    platforms_.clear();
    const int n = (int)snap.size();
    constexpr float kPitch = 24.0f, kMargin = 14.0f, kGap = 14.0f;
    float maxSide = 0, sum = 0;
    std::vector<float> contentW(n, 0.0f), contentH(n, 0.0f);

    // Pass 1: size every block from its building count, shelf-pack the blocks, and size the platform around them.
    for (int i = 0; i < n; ++i) {
        const auto& d = snap[i];
        Platform p;
        p.name = d.name;
        p.root = d.root;
        p.pitch = kPitch;
        p.blocks.resize(d.folders.size());
        struct Item { int idx; float w; };
        std::vector<Item> items;
        float area = 0, maxW = 0;
        for (size_t fi = 0; fi < d.folders.size(); ++fi) {
            Block& b = p.blocks[fi];
            b.name = d.folders[fi].name;
            const int nb = std::max<int>(1, (int)d.folders[fi].subs.size());
            b.g = std::max(1, (int)std::ceil(std::sqrt(nb * 1.0)));
            b.side = b.g * kPitch + kMargin;
            items.push_back({(int)fi, b.side});
            area += (b.side + kGap) * (b.side + kGap);
            maxW = std::max(maxW, b.side);
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& A, const Item& B) { return A.w > B.w; });
        const float rowW = std::max(maxW, std::sqrt(area * 1.08f));
        float x = 0, y = 0, rowH = 0, usedW = 0;
        for (const Item& it : items) {
            if (x > 0 && x + it.w > rowW) {
                y += rowH + kGap;
                x = 0;
                rowH = 0;
            }
            p.blocks[it.idx].cx = x + it.w * 0.5f;  // local to the content box for now
            p.blocks[it.idx].cz = y + it.w * 0.5f;
            x += it.w + kGap;
            rowH = std::max(rowH, it.w);
            usedW = std::max(usedW, x - kGap);
        }
        contentW[i] = usedW;
        contentH[i] = y + rowH;
        p.side = std::max(std::max(usedW, y + rowH) + 2.0f * kMargin, 45.0f);
        maxSide = std::max(maxSide, p.side);
        sum += p.side + 30.0f;
        platforms_.push_back(std::move(p));
    }
    // Greedy compact packing: biggest platforms first, each at the smallest radius from the hub where it clears the hub and
    // every platform already placed. Target angles follow the golden angle so the regions spread evenly around the centre.
    constexpr float kHubHalf = 75.0f, kClear = 22.0f;
    std::vector<float> px(n, 0.0f), pz(n, 0.0f);
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int A, int B) { return platforms_[A].side > platforms_[B].side; });
    std::vector<int> placed;
    float maxR = 0;
    for (int k = 0; k < n; ++k) {
        const int i = order[k];
        const float half = platforms_[i].side * 0.5f;
        const float target = 0.5f * kPi + 2.399963f * k;
        bool done = false;
        for (float r = kHubHalf + half; !done && r < 6000.0f; r += 3.0f) {
            for (int s = 0; s < 72 && !done; ++s) {
                const float off = ((s + 1) / 2) * (2.0f * kPi / 72.0f) * ((s & 1) ? 1.0f : -1.0f);
                const float a = target + off;
                const float x = r * std::cos(a), z = r * std::sin(a);
                bool ok = std::fabs(x) > kHubHalf + half + kClear || std::fabs(z) > kHubHalf + half + kClear;
                for (size_t q = 0; ok && q < placed.size(); ++q) {
                    const int j = placed[q];
                    const float need = half + platforms_[j].side * 0.5f + kClear;
                    if (std::fabs(x - px[j]) < need && std::fabs(z - pz[j]) < need) ok = false;
                }
                if (ok) {
                    px[i] = x;
                    pz[i] = z;
                    maxR = std::max(maxR, r);
                    done = true;
                }
            }
        }
        placed.push_back(i);
    }
    extent_ = maxR + maxSide * 0.75f + 100.0f;
    // Pass 2: place the platforms on a ring around the monuments and move the blocks into world space.
    for (int i = 0; i < n; ++i) {
        const auto& d = snap[i];
        Platform& p = platforms_[i];
        p.cx = px[i];
        p.cz = pz[i];
        p.tint = i;
        p.kind = d.kind;
        p.total = d.total;
        p.usedFrac = d.total ? clampf(1.0f - float(d.freeBytes) / float(d.total), 0.02f, 1.0f) : 0.3f;
        float dist = std::sqrt(p.cx * p.cx + p.cz * p.cz);
        p.delay = 1.0f + dist / 220.0f;
        // Blocks are packed biggest-first from the (0,0) corner; mirror so that corner faces the hub.
        const bool flipX = p.cx < 0, flipZ = p.cz < 0;
        for (Block& b : p.blocks) {
            if (flipX) b.cx = contentW[i] - b.cx;
            if (flipZ) b.cz = contentH[i] - b.cz;
            b.cx += p.cx - contentW[i] * 0.5f;
            b.cz += p.cz - contentH[i] * 0.5f;
            // Cells nearest the hub (world origin) fill first; the empty (free-space) cells end up on the outskirts.
            const int g = b.g;
            b.cellOrder.resize(size_t(g) * g);
            std::vector<float> key(size_t(g) * g);
            for (int c = 0; c < g * g; ++c) {
                const float x = b.cx + (float(c % g) - (g - 1) * 0.5f) * p.pitch;
                const float z = b.cz + (float(c / g) - (g - 1) * 0.5f) * p.pitch;
                key[c] = x * x + z * z;
                b.cellOrder[c] = c;
            }
            std::stable_sort(b.cellOrder.begin(), b.cellOrder.end(), [&](int A, int B) { return key[A] < key[B]; });
        }
    }
    built_ = true;
    netBuilt_ = false;
    clock_ = std::max(clock_, 0.0);
}

namespace {
// Building silhouettes. Every style is a stack of axis-aligned boxes inside the w x d footprint; part 0 is always the full-footprint
// base that carries the street door on its +z face.
struct TPart { float cx, cz, sx, sz, y0, y1; bool glow; };
constexpr int kStyles = 12;
void towerParts(int style, float w, float d, float h, std::vector<TPart>& out) {
    out.clear();
    const float hb = std::min(h, 4.0f);
    auto P = [&](float cx, float cz, float sx, float sz, float y0, float y1, bool g = false) { if (y1 > y0 + 0.01f) out.push_back({cx, cz, sx, sz, y0, y1, g}); };
    switch (style) {
    default:
    case 0:  // monolith
        P(0, 0, w, d, 0, h);
        break;
    case 1:  // ziggurat
        P(0, 0, w, d, 0, std::max(hb, h * 0.45f));
        P(0, 0, w * 0.72f, d * 0.72f, h * 0.45f, h * 0.75f);
        P(0, 0, w * 0.45f, d * 0.45f, h * 0.75f, h * 0.93f);
        P(0, 0, w * 0.12f, d * 0.12f, h * 0.93f, h, true);
        break;
    case 2:  // podium and tower
        P(0, 0, w, d, 0, std::max(hb, h * 0.16f));
        P(0, -d * 0.05f, w * 0.58f, d * 0.58f, h * 0.16f, h);
        P(0, -d * 0.05f, w * 0.66f, d * 0.66f, h * 0.9f, h * 0.94f, true);
        break;
    case 3:  // twin towers joined by a sky bridge
        P(0, 0, w, d, 0, std::max(hb, h * 0.14f));
        P(-w * 0.27f, 0, w * 0.42f, d * 0.7f, h * 0.14f, h);
        P(w * 0.27f, 0, w * 0.42f, d * 0.7f, h * 0.14f, h * 0.8f);
        P(0, 0, w * 0.2f, d * 0.4f, h * 0.55f, h * 0.62f, true);
        break;
    case 4:  // antenna spire
        P(0, 0, w, d, 0, std::max(hb, h * 0.3f));
        P(0, 0, w * 0.7f, d * 0.7f, h * 0.3f, h * 0.82f);
        P(0, 0, w * 0.1f, d * 0.1f, h * 0.82f, h, true);
        break;
    case 5:  // slab with a low annex
        P(0, 0, w, d, 0, std::max(hb, h * 0.22f));
        P(0, -d * 0.2f, w, d * 0.5f, h * 0.22f, h);
        P(0, -d * 0.2f, w * 0.9f, d * 0.5f + 0.1f, h * 0.7f, h * 0.73f, true);
        break;
    case 6:  // cantilever
        P(0, 0, w, d, 0, std::max(hb, h * 0.2f));
        P(-w * 0.18f, 0, w * 0.55f, d * 0.8f, h * 0.2f, h);
        P(w * 0.12f, 0, w * 0.75f, d * 0.88f, h * 0.62f, h * 0.84f);
        P(w * 0.12f, 0, w * 0.75f, d * 0.88f, h * 0.84f, h * 0.86f, true);
        break;
    case 7:  // terraces stepping down along x
        P(0, 0, w, d, 0, std::max(hb, h * 0.2f));
        for (int i = 0; i < 4; ++i) P(-w * 0.375f + i * w * 0.25f, 0, w * 0.25f, d * 0.9f, h * 0.2f, h * (1.0f - i * 0.2f));
        break;
    case 8:  // tower cluster
        P(0, 0, w, d, 0, std::max(hb, h * 0.1f));
        P(-w * 0.26f, -d * 0.26f, w * 0.44f, d * 0.44f, h * 0.1f, h);
        P(w * 0.26f, -d * 0.26f, w * 0.44f, d * 0.44f, h * 0.1f, h * 0.68f);
        P(-w * 0.26f, d * 0.26f, w * 0.44f, d * 0.44f, h * 0.1f, h * 0.55f);
        P(w * 0.26f, d * 0.26f, w * 0.44f, d * 0.44f, h * 0.1f, h * 0.85f);
        break;
    case 9:  // sail: layers shrinking along x
        P(0, 0, w, d, 0, std::max(hb, h * 0.12f));
        for (int i = 0; i < 5; ++i) {
            const float lw = w * (1.0f - i * 0.16f);
            P(-w * 0.5f + lw * 0.5f, 0, lw, d * 0.5f, h * 0.12f + h * 0.88f * i / 5.0f, h * 0.12f + h * 0.88f * (i + 1) / 5.0f);
        }
        break;
    case 10:  // hammerhead
        P(0, 0, w, d, 0, std::max(hb, h * 0.1f));
        P(0, 0, w * 0.5f, d * 0.5f, h * 0.1f, h * 0.84f);
        P(0, 0, w, d, h * 0.84f, h);
        P(0, 0, w + 0.1f, d + 0.1f, h * 0.88f, h * 0.9f, true);
        break;
    case 11:  // gate: two pylons and a bridge
        P(0, 0, w, d, 0, std::max(hb, h * 0.08f));
        P(-w * 0.36f, 0, w * 0.28f, d * 0.8f, h * 0.08f, h);
        P(w * 0.36f, 0, w * 0.28f, d * 0.8f, h * 0.08f, h);
        P(0, 0, w * 0.5f, d * 0.72f, h * 0.7f, h * 0.9f);
        P(0, 0, w * 0.5f, d * 0.72f + 0.1f, h * 0.9f, h * 0.92f, true);
        break;
    }
    // Upper parts must start on top of the base, not inside it: a part buried in the base shares side planes with it and z-fights.
    if (!out.empty()) {
        const float top = out[0].y1;
        for (size_t i = out.size(); i-- > 1;) {
            out[i].y0 = std::max(out[i].y0, top);
            if (out[i].y1 <= out[i].y0 + 0.01f) out.erase(out.begin() + i);
        }
    }
}
}  // namespace
void City::refreshTargets(const std::vector<CityDrive>& snap) {
    for (size_t i = 0; i < snap.size() && i < platforms_.size(); ++i) {
        const auto& d = snap[i];
        auto& p = platforms_[i];
        p.files = d.files;
        p.bytes = d.bytes;
        p.done = d.done;
        for (size_t fi = 0; fi < d.folders.size() && fi < p.blocks.size(); ++fi) {
            Block& b = p.blocks[fi];
            const auto& subs = d.folders[fi].subs;
            for (size_t si = 0; si < subs.size() && si < size_t(b.g) * size_t(b.g); ++si) {
                if (si >= b.towers.size()) {
                    Tower t;
                    t.name = subs[si].name;
                    t.x = b.cx + (float(b.cellOrder[si] % b.g) - (b.g - 1) * 0.5f) * p.pitch;
                    t.z = b.cz + (float(b.cellOrder[si] / b.g) - (b.g - 1) * 0.5f) * p.pitch;
                    const float jitter = rnd01(uint32_t(si * 7919 + fi * 104729 + i * 31 + 3));
                    const float ring = std::sqrt((t.x - p.cx) * (t.x - p.cx) + (t.z - p.cz) * (t.z - p.cz)) / 40.0f;
                    t.delay = clock_ > p.delay + 4.0 ? float(clock_) + jitter * 0.8f : p.delay + 0.8f + ring * 0.5f + jitter * 1.0f;
                    b.towers.push_back(std::move(t));
                }
                auto& t = b.towers[si];
                t.files = subs[si].files;
                t.bytes = subs[si].bytes;
                // Size and silhouette come from the folder's contents plus a stable per-name hash, so every building is distinct but
                // never changes between runs.
                const uint32_t hs = uint32_t(std::hash<std::string>{}(t.name) & 0x7FFFFFFF);
                const float h1 = rnd01(hs + 11), h2 = rnd01(hs + 23), h3 = rnd01(hs + 37), h4 = rnd01(hs + 51);
                t.tgtH = clampf((3.0f + 12.0f * std::log10(1.0f + float(t.files))) * (0.78f + 0.5f * h3), 3.0f, 110.0f);
                float share = p.total ? clampf(float(t.bytes) / (0.05f * float(p.total)), 0.0f, 1.0f) : 0.3f;
                const float maxF = p.pitch * 0.84f;
                t.tgtW = clampf(p.pitch * (0.5f + 0.28f * std::sqrt(share)) * (0.8f + 0.34f * h1), 3.0f, maxF);
                t.tgtD = clampf(t.tgtW * (0.58f + 0.6f * h2), 3.0f, maxF);
                if (h2 > 0.8f) std::swap(t.tgtW, t.tgtD);
                static const int smallStyles[3] = {0, 5, 7};
                t.style = t.tgtH < 9.0f ? smallStyles[hs % 3] : int(hs % uint32_t(kStyles));
                if (t.prop < 0 && t.tgtH >= 9.0f && h4 < 0.16f) t.prop = int(hs % 7u);
            }
        }
        if (p.done && !p.sorted) {
            p.sorted = true;
            // Seat the tallest towers on the cells nearest the hub.
            for (Block& b : p.blocks) {
                const size_t n = b.towers.size();
                if (n < 2) continue;
                std::vector<size_t> byH(n);
                for (size_t k = 0; k < n; ++k) byH[k] = k;
                std::stable_sort(byH.begin(), byH.end(), [&](size_t A, size_t B) { return b.towers[A].tgtH > b.towers[B].tgtH; });
                std::vector<std::pair<float, float>> pos(n);
                for (size_t k = 0; k < n; ++k) pos[k] = {b.towers[k].x, b.towers[k].z};
                std::stable_sort(pos.begin(), pos.end(), [](const auto& A, const auto& B) { return A.first * A.first + A.second * A.second < B.first * B.first + B.second * B.second; });
                for (size_t k = 0; k < n; ++k) { b.towers[byH[k]].x = pos[k].first; b.towers[byH[k]].z = pos[k].second; }
            }
        }
    }
}

void City::update(double dt, bool active, Vec3 eye) {
    (void)eye;
    wall_ += dt;
    data_.pollStats(dt);
    const SystemStats& s = data_.stats();

    // Live values, smoothed so the bars flow instead of jumping.
    const float k = (float)std::min(1.0, dt * 4.0);
    cpuLoad_ = s.cpuLoad;
    ramUsed_ = s.ramTotal ? float(s.ramUsed) / float(s.ramTotal) : 0.0f;
    gpuLoad_ = s.gpuLoad >= 0 ? s.gpuLoad : 0.25f + 0.15f * std::sin((float)wall_ * 0.7f);
    vramUsed_ = s.vramUsed >= 0 ? s.vramUsed : 0.4f;
    shownCpu_ += (cpuLoad_ - shownCpu_) * k;
    shownRam_ += (ramUsed_ - shownRam_) * k;
    shownGpu_ += (gpuLoad_ - shownGpu_) * k;
    if (shownCore_.size() != s.coreLoad.size()) shownCore_.assign(s.coreLoad.size(), 0.0f);
    for (size_t i = 0; i < shownCore_.size(); ++i) shownCore_[i] += (s.coreLoad[i] - shownCore_[i]) * k;

    bool ready = false;
    if (data_.snapshot(snap_, version_, ready) || (ready && !built_)) {
        if (ready && !built_ && !snap_.empty()) layoutPlatforms(snap_);
        if (built_) refreshTargets(snap_);
    }

    if (built_) {
        std::vector<CityChange> ch;
        data_.takeChanges(ch);
        for (auto& c : ch) {
            if (c.drive >= (int)platforms_.size()) continue;
            auto& p = platforms_[c.drive];
            if (c.folder >= (int)p.blocks.size()) continue;
            auto& blk = p.blocks[c.folder];
            if (c.sub < 0 || c.sub >= (int)blk.towers.size()) continue;
            auto& t = blk.towers[c.sub];
            if (t.grow < 0.5f) continue;
            if (c.added || c.removed) t.flash = c.added >= c.removed ? 1.0f : -1.0f;
            else t.flash = std::max(t.flash, 0.25f);
        }
    }

    if (!active) return;
    clock_ += dt;
    const float growStep = (float)dt / kGrowSeconds;
    for (int i = 0; i < 3; ++i)
        if (clock_ > coreDelay_[i]) coreGrow_[i] = std::min(1.0f, coreGrow_[i] + growStep);
    const float lerpK = (float)std::min(1.0, dt * 2.5);
    for (auto& p : platforms_) {
        if (clock_ > p.delay) p.grow = std::min(1.0f, p.grow + growStep);
        float mh = 10;
        for (auto& b : p.blocks) {
            float bh = 4;
            for (auto& t : b.towers) {
                if (clock_ > t.delay) t.grow = std::min(1.0f, t.grow + growStep);
                t.curH += (t.tgtH - t.curH) * lerpK;
                t.curW += (t.tgtW - t.curW) * lerpK;
                t.curD += (t.tgtD - t.curD) * lerpK;
                if (std::fabs(t.flash) > 0.001f) {
                    float dec = (float)dt * 1.4f;
                    t.flash = t.flash > 0 ? std::max(0.0f, t.flash - dec) : std::min(0.0f, t.flash + dec);
                }
                bh = std::max(bh, t.curH * ease(t.grow));
            }
            b.maxH = bh;
            mh = std::max(mh, bh);
        }
        p.maxH = mh;
    }
}

void City::renderBox(Renderer& r, Vec3 c, Vec3 size, Vec3 neon, float seed, int kind, float emerge, float fill,
                     float flash) {
    r.drawBuilding(*dummy_, Mat4::translate(c) * Mat4::scale(size), neon, seed, kind, emerge, fill, flash);
}

void City::drawLabel(Renderer& r, GpuLabel& l, Vec3 at, float h, Vec3 eye, Vec3 color, float) {
    float yaw = std::atan2(eye.x - at.x, eye.z - at.z);
    float w = h * l.aspect;
    PanelParams pp;
    pp.time = (float)wall_;
    pp.aspect = l.aspect;
    pp.radius = 0.3f;
    pp.selected = 0;
    pp.segments = 0;
    (void)r;
    pendingLabels_.push_back({&l, Mat4::translate(at) * Mat4::rotateY(yaw) * Mat4::scale({w, h, 1.0f}), l.aspect,
                              (eye.x - at.x) * (eye.x - at.x) + (eye.y - at.y) * (eye.y - at.y) + (eye.z - at.z) * (eye.z - at.z)});
}

void City::flushLabels(Renderer& r) {
    std::sort(pendingLabels_.begin(), pendingLabels_.end(), [](const PendingLabel& a, const PendingLabel& b) { return a.dist > b.dist; });
    const float white[3] = {1, 1, 1}, uvAll[4] = {0, 0, 1, 1};
    for (const PendingLabel& p : pendingLabels_) r.drawWebRegion(*p.label, p.world, uvAll, p.aspect, 0.0f, -1.0f, 1.0f, white);
    pendingLabels_.clear();
}

// Same face as the lobby doors: a folder panel above the building name, which shrinks to fit.
std::shared_ptr<GpuLabel> City::makeDoorLabel(const std::string& name, Vec3 tint) {
    const float m = std::max(0.01f, std::max(tint.x, std::max(tint.y, tint.z)));
    DoorFaceSpec spec;
    spec.accent = Color8{static_cast<uint8_t>(std::clamp(tint.x / m * 255.0f, 60.0f, 255.0f)), static_cast<uint8_t>(std::clamp(tint.y / m * 255.0f, 60.0f, 255.0f)),
                         static_cast<uint8_t>(std::clamp(tint.z / m * 255.0f, 60.0f, 255.0f)), 255};
    Bitmap bm = composeDoorBase(spec);
    drawDoorText(bm, name, {}, false, 0.0f, spec.accent);
    return renderer_.createLabel(bm, true);
}

void City::markEntered() {
    hasEntered_ = false;
    if (hoverP_ < 0 || hoverB_ < 0 || hoverT_ < 0) return;
    const auto& t = platforms_[hoverP_].blocks[hoverB_].towers[hoverT_];
    enteredDoor_ = {t.x, kSlabH + kPlateH, t.z + std::max(1.5f, t.curD) * 0.5f};
    hasEntered_ = true;
}

bool City::exitSpawn(Vec3& pos) const {
    if (!hasEntered_) return false;
    pos = {enteredDoor_.x, enteredDoor_.y + 1.7f, enteredDoor_.z + 2.5f};
    return true;
}

bool City::debugStreet(Vec3& pos) const {
    for (const auto& pl : platforms_)
        for (const auto& b : pl.blocks)
            for (const auto& t : b.towers)
                if (t.grow > 0.97f && t.curH > 8.0f) {
                    pos = {t.x, kSlabH + kPlateH + 1.7f, t.z + std::max(1.5f, t.curD) * 0.5f + 5.0f};
                    return true;
                }
    return false;
}

void City::moduleLabel(Renderer& r, Monument& m, const std::string& text, Vec3 at, float h, Vec3 eye, Vec3 col) {
    if (r.mirror()) return;
    if ((!m.label || (text != m.text && wall_ - m.labelTime > 1.5)) && labelBudget_ > 0) {
        m.label = makeText(text);
        m.text = text;
        m.labelTime = (float)wall_;
        --labelBudget_;
    }
    if (m.label) drawLabel(r, *m.label, at, h, eye, col);
}

// A small marker standing in the hub-facing corner of each drive region that shows what kind of storage it is:
// NVMe = slim fast spire, SATA SSD = chip package, hard disk = spinning platter with an actuator arm, USB = plug.
void City::renderDriveBeacon(Renderer& r, const Platform& p, Vec3 tint, float pg) {
    const float sx = p.cx < 0 ? 1.0f : -1.0f, sz = p.cz < 0 ? 1.0f : -1.0f;
    const float x = p.cx + sx * (p.side * 0.5f - 4.0f), z = p.cz + sz * (p.side * 0.5f - 4.0f), b0 = kSlabH, t = float(clock_);
    const float act = clampf(0.3f + (p.done ? 0.0f : 0.7f) + 0.5f * (0.5f + 0.5f * std::sin(t * 2.0f + p.cx)), 0.0f, 1.0f);
    auto blk = [&](float dx, float dz, float y0, float w, float h, float d, Vec3 c, int kind) {
        renderBox(r, {x + dx, b0 + (y0 + h * 0.5f) * pg, z + dz}, {w, h * pg, d}, c, 0.35f, kind, p.grow, 1.0f, 0);
    };
    const Vec3 white{0.95f, 0.98f, 1.0f};
    blk(0, 0, 0, 4.4f, 0.4f, 4.4f, tint * 0.6f, 6);
    switch (p.kind) {
    case 2:  // NVMe
        blk(0, 0, 0.4f, 1.2f, 7.0f, 1.2f, tint * 0.8f, 6);
        for (int i = 0; i < 4; ++i) {
            const float ph = float(std::fmod(t * 1.6f + i * 0.25f, 1.0));
            blk(0, 0, 0.6f + ph * 6.0f, 1.6f, 0.35f, 1.6f, white * (0.4f + 1.4f * std::sin(ph * kPi)), 5);
        }
        break;
    case 1:  // SATA SSD
        blk(0, 0, 0.4f, 3.4f, 1.6f, 3.4f, tint * 0.8f, 6);
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) blk(-0.8f + i * 1.6f, -0.8f + j * 1.6f, 2.0f, 1.1f, 0.2f, 1.1f, white * (0.3f + 0.9f * act * (0.5f + 0.5f * std::sin(t * 3.0f + i * 2 + j * 5))), 5);
        break;
    case 3:  // removable
        blk(0, 0.3f, 0.4f, 2.4f, 1.2f, 2.6f, tint * 0.8f, 6);
        blk(0, -1.6f, 0.6f, 1.4f, 0.6f, 1.0f, white * 0.7f, 5);
        blk(0.7f, 1.0f, 1.65f, 0.4f, 0.15f, 0.4f, white * (0.5f + 1.0f * (std::sin(t * 4.0f) > 0 ? 1.0f : 0.0f)), 5);
        break;
    default: {  // hard disk
        blk(0, 0, 0.4f, 3.8f, 0.8f, 3.8f, tint * 0.7f, 6);
        const float spin = t * (2.0f + 3.0f * act);
        for (int i = 0; i < 8; ++i) {
            const float a = spin + i * kPi * 0.25f, lit = i == 0 ? 1.6f : 0.35f;
            blk(1.3f * std::cos(a), 1.3f * std::sin(a), 1.2f, 0.5f, 0.2f, 0.5f, white * lit, 5);
        }
        const float arm = 0.9f * std::sin(t * 1.3f + p.cz);
        blk(1.6f, arm, 1.25f, 0.2f, 0.12f, 2.2f, tint, 5);
    }
    }
}

// The rest of the motherboard: power delivery, cooling, memory controller, USB and the network adapter.
void City::renderModules(Renderer& r, const Theme& th, Vec3 eye) {
    if (coreGrow_[0] <= 0.3f) return;
    const SystemStats& s = data_.stats();
    const float g = ease(coreGrow_[0]), t = float(clock_), b0 = kSlabH;
    const bool anim = g > 0.99f && !r.mirror();
    const int N = std::clamp<int>(s.cores, 1, 32);
    const float plinth = std::max(14.0f, N * 4.2f / (2.0f * kPi)) * 2.0f + 10.0f;
    const float ex = plinth * 0.5f + 9.0f;
    const Vec3 green{0.12f, 0.85f, 0.42f}, hot{1.0f, 0.45f, 0.1f}, white{0.95f, 0.98f, 1.0f};
    const float cpu = shownCpu_, gpu = shownGpu_, ram = shownRam_;
    auto blk = [&](float cx, float cz, float y0, float sx, float h, float sz, Vec3 col, int kind) {
        renderBox(r, {cx, b0 + (y0 + h * 0.5f) * g, cz}, {sx, h * g, sz}, col, 0.35f, kind, coreGrow_[0], 1.0f, 0);
    };
    auto base = [&](float cx, float cz, float w, float d, Vec3 col) {
        renderBox(r, {cx, kSlabH * 0.5f * g, cz}, {w + 2.0f, kSlabH * g, d + 2.0f}, col * 0.8f, 0.3f, 6, coreGrow_[0], 1.0f, 0);
    };

    // VRM / power delivery east of the CPU: a bank of chokes that light with the per-core load, and capacitors.
    {
        const float cx = ex, cz = -46.0f;
        base(cx, cz, 9, 9, th.accent);
        for (int i = 0; i < 6; ++i) {
            const float ld = shownCore_.empty() ? cpu : shownCore_[i % shownCore_.size()];
            blk(cx - 3.0f + (i % 3) * 3.0f, cz - 1.8f + (i / 3) * 3.6f, 0, 2.0f, 2.2f, 2.2f, th.alt * 0.7f, 6);
            if (anim) blk(cx - 3.0f + (i % 3) * 3.0f, cz - 1.8f + (i / 3) * 3.6f, 2.2f, 1.6f, 0.2f, 1.6f, th.accent * (0.3f + 1.6f * ld), 5);
        }
        for (int i = 0; i < 4; ++i) blk(cx - 3.6f + i * 2.4f, cz + 3.8f, 0, 1.0f, 3.2f, 1.0f, green * 0.5f, 6);
        if (anim) {
            blk(cx - 5.5f, cz, 0, 4.0f, 0.2f, 1.0f, th.accent * 0.4f, 5);
            for (int j = 0; j < 3; ++j) {
                const float ph = float(std::fmod(t * (0.3f + 1.5f * cpu) + j / 3.0f, 1.0));
                renderBox(r, {cx - 4.5f - ph * (ex - 19.0f - 4.5f), b0 + 0.3f, cz}, {0.7f, 0.4f, 0.7f}, green * (0.5f + 1.5f * cpu), 0.3f, 5, 1, 0, 0);
            }
        }
        std::string txt = s.battPct >= 0 && s.onBattery ? "VRM  BATTERY " + std::to_string(s.battPct) + "%" : "VRM  POWER  AC";
        moduleLabel(r, modLabel_[0], txt, {cx, b0 + 6.5f, cz}, 1.8f, eye, th.accent);
    }
    // Cooling: a fan whose blades chase round faster (and glow hotter) as the estimated temperature rises.
    {
        const float cx = ex, cz = -65.0f;
        shownTemp_ += ((34.0f + 52.0f * (0.6f * cpu + 0.4f * std::max(gpu, 0.0f))) - shownTemp_) * 0.02f;
        const float t01 = clampf((shownTemp_ - 34.0f) / 52.0f, 0.0f, 1.0f);
        const Vec3 heat = th.accent * (1.0f - t01) + hot * t01;
        base(cx, cz, 9, 9, heat);
        blk(cx, cz, 0, 8.0f, 0.8f, 8.0f, th.alt * 0.6f, 6);
        blk(cx, cz, 0.8f, 1.6f, 1.2f, 1.6f, heat, 5);
        if (anim) {
            const float spin = t * (1.5f + 6.0f * t01);
            for (int i = 0; i < 8; ++i) {
                const float a = spin + i * kPi * 0.25f, lit = 0.25f + 1.4f * std::max(0.0f, std::cos(a - spin)) * (i == 0 ? 1.0f : 0.5f);
                blk(cx + 2.8f * std::cos(a), cz + 2.8f * std::sin(a), 0.9f, 1.1f, 0.25f, 1.1f, heat * lit, 5);
            }
        }
        char b[48];
        std::snprintf(b, sizeof b, "COOLING  EST %d C", int(shownTemp_ + 0.5f));
        moduleLabel(r, modLabel_[1], b, {cx, b0 + 4.0f, cz}, 1.8f, eye, heat);
    }
    // Memory controller: sits on the end of the cache chain (L3); its memory bus is a gantry bundle (see bundles()).
    {
        const float cx = -(plinth * 0.5f + 10.0f), cz = -29.0f;
        base(cx, cz, 7, 7, th.alt);
        blk(cx, cz, 0, 5.0f, 1.6f, 5.0f, th.alt * 0.8f, 6);
        blk(cx, cz, 1.6f, 3.0f, 0.2f, 3.0f, th.alt * (0.4f + 1.4f * ram), 5);
        // link from the L3 cache
        blk(cx, -35.5f, 0, 1.0f, 0.2f, 4.4f, th.accent * 0.4f, 5);
        if (anim) {
            const float ph = float(std::fmod(t * (0.2f + 1.0f * cpu), 1.0));
            renderBox(r, {cx, b0 + 0.4f, -37.5f + 4.0f * ph}, {0.5f, 0.35f, 0.9f}, th.accent * (0.5f + 1.6f * cpu), 0.3f, 5, 1, 0, 0);
        }
        moduleLabel(r, modLabel_[2], "MEMORY CONTROLLER", {cx, b0 + 5.0f, cz}, 1.8f, eye, th.alt);
    }
    // USB controller: a row of ports that light for each attached device.
    {
        const float cx = -28.0f, cz = -17.0f;
        base(cx, cz, 8, 8, th.accent);
        blk(cx, cz, 0, 7.0f, 1.0f, 7.0f, th.accent * 0.5f, 6);
        for (int i = 0; i < 4; ++i) {
            const bool used = i < s.usbDevices;
            blk(cx - 2.4f + i * 1.6f, cz + 1.0f, 1.0f, 1.2f, 0.8f, 2.4f, used ? white * 0.6f : white * 0.2f, 6);
            blk(cx - 2.4f + i * 1.6f, cz + 1.0f, 1.8f, 0.8f, 0.15f, 1.8f, used ? green * 1.3f : th.accent * 0.15f, 5);
        }
        moduleLabel(r, modLabel_[3], "USB  " + std::to_string(s.usbDevices) + " DEVICES", {cx, b0 + 4.0f, cz}, 1.8f, eye, th.accent);
    }
    // Network adapter: antenna with expanding rings for Wi-Fi, or a socket for ethernet, plus a link-speed meter.
    {
        const float cx = 28.0f, cz = -17.0f;
        const float rx = shownNetRx_, tx = shownNetTx_, act = std::max(rx, tx);
        base(cx, cz, 8, 8, green);
        blk(cx, cz, 0, 7.0f, 1.0f, 7.0f, green * 0.5f, 6);
        if (s.netWifi) {
            blk(cx, cz, 1.0f, 0.4f, 5.0f, 0.4f, green, 6);
        } else {
            blk(cx, cz + 1.0f, 1.0f, 3.0f, 1.6f, 2.6f, white * 0.5f, 6);
            if (anim) blk(cx, cz + 1.0f, 2.6f, 2.4f, 0.15f, 2.0f, green * (0.4f + 1.6f * act), 5);
        }
        const int seg = s.netMbps >= 10000 ? 5 : s.netMbps >= 2500 ? 4 : s.netMbps >= 1000 ? 3 : s.netMbps >= 100 ? 2 : s.netMbps > 0 ? 1 : 0;
        for (int i = 0; i < 5; ++i) blk(cx - 2.8f + i * 1.4f, cz - 2.8f, 1.0f, 1.0f, 0.2f + 0.2f * i, 0.8f, i < seg ? green * 1.2f : green * 0.15f, 5);
        char b[64];
        const char* kind = s.netWifi ? "WI-FI" : "ETHERNET";
        if (s.netMbps >= 1000) std::snprintf(b, sizeof b, "NETWORK  %s  %g GBPS", kind, s.netMbps / 1000.0);
        else if (s.netMbps > 0) std::snprintf(b, sizeof b, "NETWORK  %s  %d MBPS", kind, s.netMbps);
        else std::snprintf(b, sizeof b, "NETWORK  OFFLINE");
        moduleLabel(r, modLabel_[4], b, {cx, b0 + 7.0f, cz}, 1.8f, eye, green);
    }

}
// L1 / L2 / L3 cache modules beside the CPU: each a slab whose height tracks its real size, with a grid of cells that light up
// as the CPU works. Smaller, faster caches flicker quicker than the big L3.
void City::renderCache(Renderer& r, const Theme& th, Vec3 eye) {
    if (coreGrow_[0] <= 0.3f) return;
    const SystemStats& s = data_.stats();
    const float g = ease(coreGrow_[0]), t = float(clock_);
    const int N = std::clamp<int>(s.cores, 1, 32);
    const float plinth = std::max(14.0f, N * 4.2f / (2.0f * kPi)) * 2.0f + 10.0f;
    const float cx = -(plinth * 0.5f + 10.0f);
    const float cz[3] = {-70.0f, -58.0f, -44.0f}, side[3] = {6.0f, 8.0f, 11.0f}, hgt[3] = {2.5f, 3.5f, 5.0f};
    const int kb[3] = {s.l1KB ? s.l1KB : 384, s.l2KB ? s.l2KB : 3072, s.l3KB ? s.l3KB : 16384};
    const char* nm[3] = {"L1", "L2", "L3"};
    const float rate[3] = {5.0f, 2.8f, 1.2f};
    const Vec3 col = th.accent;
    for (int m = 0; m < 3; ++m) {
        const float h = hgt[m] * g;
        renderBox(r, {cx, kSlabH * 0.5f * g, cz[m]}, {side[m] + 2.0f, kSlabH * g, side[m] + 2.0f}, col * 0.8f, 0.2f + m, 6, coreGrow_[0], 1.0f, 0);
        renderBox(r, {cx, kSlabH + h * 0.5f, cz[m]}, {side[m], h, side[m]}, th.alt * 0.8f, 0.4f + m, 6, coreGrow_[0], 1.0f, 0);
        if (g < 0.99f || r.mirror()) continue;
        // cell grid on the top face
        const int n = m + 3;
        const float cell = side[m] / n;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                const float ph = 0.5f + 0.5f * std::sin(t * rate[m] * (0.6f + shownCpu_) + (i * 7 + j * 13 + m * 5) * 1.7f);
                const float lit = 0.15f + 1.6f * shownCpu_ * ph;
                renderBox(r, {cx - side[m] * 0.5f + cell * (i + 0.5f), kSlabH + h + 0.08f, cz[m] - side[m] * 0.5f + cell * (j + 0.5f)},
                          {cell * 0.7f, 0.15f, cell * 0.7f}, col * lit, 0.3f, 5, 1, 0, 0);
            }
        // Wired to the cores: a bar across to the CPU plinth with pulses running between them.
        const float fl = 10.0f - side[m] * 0.5f, fx = cx + side[m] * 0.5f;
        renderBox(r, {fx + fl * 0.5f, kSlabH + 0.2f, cz[m]}, {fl, 0.2f, 0.6f}, col * 0.4f, 0.3f, 5, 1, 0, 0);
        for (int j = 0; j < 2; ++j) {
            const float ph = float(std::fmod(t * (0.25f + 1.2f * shownCpu_) + j * 0.5f + m * 0.2f, 1.0));
            renderBox(r, {fx + fl * (m % 2 ? ph : 1.0f - ph), kSlabH + 0.4f, cz[m]}, {0.9f, 0.35f, 0.5f}, col * (0.5f + 1.6f * shownCpu_), 0.3f, 5, 1, 0, 0);
        }
    }
    // The hierarchy is a chain: cores - L1 - L2 - L3 - memory controller, with pulses running outward toward memory.
    if (g >= 0.99f && !r.mirror())
        for (int m = 0; m < 2; ++m) {
            const float z0 = cz[m] + (side[m] + 2.0f) * 0.5f, z1 = cz[m + 1] - (side[m + 1] + 2.0f) * 0.5f;
            renderBox(r, {cx, kSlabH + 0.2f, (z0 + z1) * 0.5f}, {1.0f, 0.2f, z1 - z0 + 0.4f}, col * 0.4f, 0.3f, 5, 1, 0, 0);
            const float ph = float(std::fmod(t * (0.2f + 1.0f * shownCpu_) + m * 0.4f, 1.0));
            renderBox(r, {cx, kSlabH + 0.4f, z0 + (z1 - z0) * ph}, {0.5f, 0.35f, 0.9f}, col * (0.5f + 1.6f * shownCpu_), 0.3f, 5, 1, 0, 0);
        }
    if (r.mirror()) return;
    for (int m = 0; m < 3; ++m) {
        char b[48];
        if (kb[m] >= 1024) std::snprintf(b, sizeof b, "%s CACHE  %.4g MB", nm[m], kb[m] / 1024.0);
        else std::snprintf(b, sizeof b, "%s CACHE  %d KB", nm[m], kb[m]);
        std::string text = b;
        Monument& mo = cacheLabel_[m];
        if ((!mo.label || (text != mo.text && wall_ - mo.labelTime > 1.5)) && labelBudget_ > 0) {
            mo.label = makeText(text);
            mo.text = text;
            mo.labelTime = (float)wall_;
            --labelBudget_;
        }
        if (mo.label) drawLabel(r, *mo.label, {cx, kSlabH + hgt[m] + 3.0f, cz[m]}, 1.8f, eye, col);
    }
}

// The central hub models the chipset (PCH): a squat die with a heatsink, hanging off the CPU by a DMI uplink. Its four corner
// pylons are the chipset's real traffic: storage read / write and network receive / send, climbing faster the busier they are.
void City::renderHub(Renderer& r, const Theme& th, float g, Vec3 eye) {
    const float t = float(clock_), b0 = kSlabH, gg = coreGrow_[0];
    const SystemStats& s = data_.stats();
    const float k = 0.15f;
    shownDiskR_ += (s.diskRead - shownDiskR_) * k;
    shownDiskW_ += (s.diskWrite - shownDiskW_) * k;
    shownNetRx_ += (s.netRx - shownNetRx_) * k;
    shownNetTx_ += (s.netTx - shownNetTx_) * k;
    const float loads[4] = {shownDiskR_, shownDiskW_, shownNetRx_, shownNetTx_};
    const Vec3 hc[4] = {th.accent, th.alt, Vec3{0.12f, 0.85f, 0.42f}, Vec3{0.95f, 0.95f, 1.0f}};
    const char* names[4] = {"DISK READ", "DISK WRITE", "NET RECEIVE", "NET SEND"};
    const float act = clampf((loads[0] + loads[1] + loads[2] + loads[3]) * 0.25f * 1.6f, 0.0f, 1.0f);
    auto blk = [&](float cx, float cz, float y0, float sx, float h, float sz, Vec3 col, int kind) {
        renderBox(r, {cx, b0 + (y0 + h * 0.5f) * g, cz}, {sx, h * g, sz}, col, 0.35f, kind, gg, 1.0f, 0);
    };
    // The die: a package, a lit die window and a finned heatsink.
    blk(0, 0, 0, 16, 0.8f, 16, th.accent * 0.7f, 6);
    blk(0, 0, 0.8f, 11, 2.2f, 11, th.alt * 0.8f, 6);
    blk(0, 0, 3.0f, 7, 0.5f, 7, th.accent * (0.5f + 0.9f * act), 5);
    for (int i = 0; i < 7; ++i) blk(0, -3.0f + i, 3.5f, 6.4f, 3.0f, 0.35f, th.accent * 0.6f, 6);
    blk(0, 0, 6.5f, 7, 0.3f, 7, th.alt * (0.5f + 0.8f * act), 5);
    if (g < 0.99f) return;
    // Chipset lanes: short glowing slots along the east and west edges of the package.
    for (int i = 0; i < 5; ++i) {
        const float z = -4.0f + i * 2.0f, ph = 0.5f + 0.5f * std::sin(t * 4.0f + i * 1.3f);
        blk(-6.6f, z, 0.8f, 1.6f, 0.3f, 1.0f, hc[i % 4] * (0.4f + 0.9f * act * ph), 5);
        blk(6.6f, z, 0.8f, 1.6f, 0.3f, 1.0f, hc[(i + 2) % 4] * (0.4f + 0.9f * act * ph), 5);
    }
    // Corner pylons, one per traffic stream.
    const float px[4] = {-11, 11, -11, 11}, pz[4] = {-11, -11, 11, 11};
    for (int i = 0; i < 4; ++i) {
        const float ld = loads[i];
        blk(px[i], pz[i], 0, 2.2f, 7.0f, 2.2f, hc[i] * 0.5f, 6);
        blk(px[i], pz[i], 7.0f, 2.8f, 0.5f, 2.8f, hc[i] * (0.5f + 1.6f * ld), 5);
        const bool inbound = i == 0 || i == 2;
        for (int j = 0; j < 4; ++j) {
            float ph = float(std::fmod(t * (0.12f + 1.0f * ld) + j * 0.25f + i * 0.13f, 1.0));
            if (!inbound) ph = 1.0f - ph;
            blk(px[i], pz[i], 0.8f + ph * 5.6f, 2.5f, 0.4f, 2.5f, hc[i] * (0.25f + 1.8f * ld * std::sin(ph * kPi)), 5);
        }
        // a bridge from the pylon to the die, with a packet crossing it
        const float ph = float(std::fmod(t * (0.12f + 1.0f * ld) + i * 0.27f, 1.0)), u = inbound ? 1.0f - ph : ph;
        renderBox(r, {px[i] * (0.4f + 0.6f * u), b0 + 1.6f, pz[i] * (0.4f + 0.6f * u)}, {0.6f, 0.6f, 0.6f}, hc[i] * (0.3f + 1.7f * ld), 0.3f, 5, 1, 0, 0);
    }
    // Slab rings: pulses of light spread out from the die when the machine moves data.
    for (int j = 0; j < 2; ++j) {
        const float ph = float(std::fmod(t * (0.1f + 0.35f * act) + j * 0.5f, 1.0)), e = 8.6f + ph * 3.8f, a = (1.0f - ph) * act;
        if (a < 0.03f) continue;
        const Vec3 c = th.accent * a;
        const float y = b0 + 0.07f;
        renderBox(r, {0, y, -e}, {2 * e, 0.1f, 0.3f}, c, 0.3f, 5, 1, 0, 0);
        renderBox(r, {0, y, e}, {2 * e, 0.1f, 0.3f}, c, 0.3f, 5, 1, 0, 0);
        renderBox(r, {-e, y, 0}, {0.3f, 0.1f, 2 * e}, c, 0.3f, 5, 1, 0, 0);
        renderBox(r, {e, y, 0}, {0.3f, 0.1f, 2 * e}, c, 0.3f, 5, 1, 0, 0);
    }
    // Labels.
    if (r.mirror()) return;
    auto lab = [&](Monument& m, const std::string& text, Vec3 at, float h, Vec3 col) {
        if ((!m.label || (text != m.text && wall_ - m.labelTime > 1.5)) && labelBudget_ > 0) {
            m.label = makeText(text);
            m.text = text;
            m.labelTime = (float)wall_;
            --labelBudget_;
        }
        if (m.label) drawLabel(r, *m.label, at, h, eye, col);
    };
    lab(hubLabel_, "CHIPSET  DMI UPLINK TO CPU", {0, b0 + 12.0f, 0}, 3.2f, th.accent);
    for (int i = 0; i < 4; ++i) {
        char b[48];
        std::snprintf(b, sizeof b, "%s  %d%%", names[i], int(loads[i] * 100 + 0.5f));
        lab(pylonLabel_[i], b, {px[i], b0 + 10.0f, pz[i]}, 1.5f, hc[i]);
    }
}
void City::renderMonuments(Renderer& r, Vec3 eye) {
    const Theme& th = currentTheme();
    const SystemStats& s = data_.stats();
    const bool mirror = r.mirror();
    renderCache(r, th, eye);
    renderModules(r, th, eye);

    auto labelFor = [&](Monument& m, const std::string& text, Vec3 at, float h, Vec3 col) {
        if (mirror) return;
        if ((!m.label || (text != m.text && wall_ - m.labelTime > 1.5)) && labelBudget_ > 0) {
            m.label = makeText(text);
            m.text = text;
            m.labelTime = (float)wall_;
            --labelBudget_;
        }
        if (m.label) drawLabel(r, *m.label, at, h, eye, col);
    };
    auto pct = [](float f) { char b[16]; std::snprintf(b, sizeof b, "%d%%", int(f * 100 + 0.5f)); return std::string(b); };

    // CPU: tall spire for overall load with a ring of per-core pillars.
    {
        float g = ease(coreGrow_[0]);
        if (coreGrow_[0] > 0) {
            const Vec3 c{0, 0, -55};
            int N = std::clamp<int>(s.cores, 1, 32);
            float ringR = std::max(14.0f, N * 4.2f / (2.0f * kPi));
            float plinth = ringR * 2.0f + 10.0f;
            renderBox(r, {c.x, kSlabH * 0.5f * g, c.z}, {plinth, kSlabH * g, plinth}, th.accent, 0.1f, 6, coreGrow_[0], 1.0f, 0);
            renderBox(r, {c.x, kSlabH + 45.0f * g, c.z}, {8, 90.0f * g, 8}, th.accent, 0.2f, 4, coreGrow_[0], std::max(0.03f, shownCpu_), 0);
            for (int i = 0; i < N; ++i) {
                float a = 2.0f * kPi * i / N;
                float load = i < (int)shownCore_.size() ? shownCore_[i] : 0.0f;
                renderBox(r, {c.x + ringR * std::cos(a), kSlabH + 15.0f * g, c.z + ringR * std::sin(a)}, {2.6f, 30.0f * g, 2.6f},
                          th.accent, 0.3f, 4, coreGrow_[0], std::max(0.03f, load), 0);
            }
            // Die package: stepped heat-spreader, corner towers, radial heatsink fins and a glowing socket ring.
            {
                const float top = kSlabH;
                renderBox(r, {c.x, top + 1.0f * g, c.z}, {plinth * 0.86f, 2.0f * g, plinth * 0.86f}, th.accent * 0.5f, 0.2f, 3, coreGrow_[0], 1.0f, 0);
                renderBox(r, {c.x, top + 3.0f * g, c.z}, {plinth * 0.62f, 2.0f * g, plinth * 0.62f}, th.accent, 0.3f, 3, coreGrow_[0], 1.0f, 0);
                renderBox(r, {c.x, top + 5.5f * g, c.z}, {plinth * 0.36f, 3.0f * g, plinth * 0.36f}, th.accent, 0.4f, 4, coreGrow_[0], std::max(0.05f, shownCpu_), 0);
                for (int i = 0; i < 12; ++i) {
                    float a2 = 2.0f * kPi * i / 12 + 0.26f;
                    float fr = 6.5f;
                    renderBox(r, {c.x + fr * std::cos(a2), top + 18.0f * g, c.z + fr * std::sin(a2)}, {0.6f, 36.0f * g, 4.5f},
                              th.accent * 0.7f, 0.25f, 3, coreGrow_[0], 1.0f, 0);
                }
                for (int sx = -1; sx <= 1; sx += 2)
                    for (int sz = -1; sz <= 1; sz += 2) {
                        float o = plinth * 0.5f - 2.5f;
                        renderBox(r, {c.x + sx * o, top + 7.0f * g, c.z + sz * o}, {3.5f, 14.0f * g, 3.5f}, th.accent, 0.4f, 3, coreGrow_[0], 1.0f, 0);
                        renderBox(r, {c.x + sx * o, top + 14.4f * g, c.z + sz * o}, {2.0f, 0.8f * g, 2.0f}, th.accent, 0.9f, 4, coreGrow_[0], 1.0f, 0);
                    }
                // Pin-grid strips along each edge of the plinth.
                for (int e = 0; e < 4; ++e)
                    for (int p = 0; p < 9; ++p) {
                        float u = (p - 4) * (plinth * 0.09f);
                        float ox = (e % 2 == 0) ? u : (e == 1 ? 1 : -1) * (plinth * 0.5f - 0.7f);
                        float oz = (e % 2 == 1) ? u : (e == 0 ? 1 : -1) * (plinth * 0.5f - 0.7f);
                        renderBox(r, {c.x + ox, top + 0.4f * g, c.z + oz}, {1.0f, 0.8f * g, 1.0f}, th.accent, 0.8f, 4, coreGrow_[0], 1.0f, 0);
                    }
                // Link ring joining the core pillars.
                for (int i = 0; i < N; ++i) {
                    float a1 = 2.0f * kPi * (i + 0.5f) / N;
                    float seg = 2.0f * kPi * ringR / N;
                    renderBox(r, {c.x + ringR * std::cos(a1), top + 6.0f * g, c.z + ringR * std::sin(a1)},
                              {std::abs(std::sin(a1)) * seg * 0.9f + 0.4f, 0.5f * g, std::abs(std::cos(a1)) * seg * 0.9f + 0.4f},
                              th.accent, 0.8f, 4, coreGrow_[0], 1.0f, 0);
                }
                renderBox(r, {c.x, kSlabH + 91.0f * g, c.z}, {2.0f, 6.0f * g, 2.0f}, th.accent, 1.0f, 4, coreGrow_[0], 1.0f, 0);
            }
            labelFor(mon_[0], "CPU  " + upper(s.cpuName) + "  " + std::to_string(s.cores) + " THREADS  " + pct(shownCpu_),
                     {c.x, 108, c.z}, 6.0f, th.accent);
        }
    }
    // RAM: a bank of slabs filled in order by used memory.
    {
        float g = ease(coreGrow_[1]);
        if (coreGrow_[1] > 0) {
            const Vec3 c{-52, 0, 30};
            double gb = s.ramTotal / 1073741824.0;
            int K = std::clamp<int>((int)(gb / 4.0 + 0.5), 2, 16);
            float width = K * 3.4f + 6.0f;
            renderBox(r, {c.x, kSlabH * 0.5f * g, c.z}, {width, kSlabH * g, 20}, th.alt, 0.4f, 6, coreGrow_[1], 1.0f, 0);
            for (int i = 0; i < K; ++i) {
                float fill = clampf(shownRam_ * K - i, 0.0f, 1.0f);
                float x = c.x + (i - (K - 1) * 0.5f) * 3.4f;
                renderBox(r, {x, kSlabH + 12.0f * g, c.z}, {2.4f, 24.0f * g, 9}, th.alt, 0.5f, 4, coreGrow_[1], std::max(0.02f, fill), 0);
            }
            // DIMM detail: chip rows, gold contact strips, heat-spreader caps and a connector rail.
            for (int i = 0; i < K; ++i) {
                float x = c.x + (i - (K - 1) * 0.5f) * 3.4f;
                for (int k = 0; k < 4; ++k)
                    renderBox(r, {x, kSlabH + (4.0f + k * 5.0f) * g, c.z + 4.7f}, {2.0f, 3.2f * g, 0.5f}, th.alt * 0.6f, 0.3f, 3, coreGrow_[1], 1.0f, 0);
                renderBox(r, {x, kSlabH + 0.6f * g, c.z}, {2.0f, 1.2f * g, 9.6f}, th.alt, 0.9f, 4, coreGrow_[1], 1.0f, 0);
                renderBox(r, {x, kSlabH + 24.8f * g, c.z}, {2.8f, 1.6f * g, 9.6f}, th.alt, 0.7f, 4, coreGrow_[1], std::max(0.1f, shownRam_), 0);
            }
            renderBox(r, {c.x, kSlabH + 0.5f * g, c.z - 7.5f}, {width - 2.0f, 1.0f * g, 2.0f}, th.alt * 0.6f, 0.5f, 3, coreGrow_[1], 1.0f, 0);
            renderBox(r, {c.x, kSlabH + 0.5f * g, c.z + 7.5f}, {width - 2.0f, 1.0f * g, 2.0f}, th.alt * 0.6f, 0.5f, 3, coreGrow_[1], 1.0f, 0);
            char b[96];
            std::snprintf(b, sizeof b, "RAM  %.0f GB  %s IN USE", gb, pct(shownRam_).c_str());
            labelFor(mon_[1], b, {c.x, 40, c.z}, 4.0f, th.alt);
        }
    }
    // GPU: stepped ziggurat for load and a pylon whose height scales with video memory.
    {
        float g = ease(coreGrow_[2]);
        if (coreGrow_[2] > 0) {
            const Vec3 c{52, 0, 30};
            const Vec3 gpuCol{0.12f, 0.85f, 0.42f};
            float sizes[4] = {40, 30, 20, 11};
            renderBox(r, {c.x, kSlabH * 0.5f * g, c.z}, {46, kSlabH * g, 46}, gpuCol, 0.6f, 6, coreGrow_[2], 1.0f, 0);
            for (int i = 0; i < 4; ++i) {
                float fill = clampf(shownGpu_ * 4.0f - i, 0.0f, 1.0f);
                renderBox(r, {c.x, kSlabH + (i * 6.0f + 3.0f) * g, c.z}, {sizes[i], 6.0f * g, sizes[i]}, gpuCol, 0.7f, 4,
                          coreGrow_[2], std::max(0.03f, fill), 0);
            }
            // Cooler shroud: twin fan rings, fin stack, power connectors and VRAM chip blocks around the base.
            for (int fx = -1; fx <= 1; fx += 2) {
                const float cx = c.x + fx * 11.0f;
                renderBox(r, {cx, kSlabH + 1.5f * g, c.z - 18.0f}, {13.0f, 3.0f * g, 8.0f}, gpuCol * 0.6f, 0.3f, 3, coreGrow_[2], 1.0f, 0);
                for (int k = 0; k < 8; ++k) {
                    float a2 = kPi * k / 8 + (float)wall_ * (fx > 0 ? 1.5f : -1.5f);
                    renderBox(r, {cx + 5.0f * std::cos(a2), kSlabH + 3.3f * g, c.z - 18.0f + 3.2f * std::sin(a2)},
                              {0.5f + 4.0f * std::abs(std::sin(a2)), 0.3f * g, 0.5f + 4.0f * std::abs(std::cos(a2))}, gpuCol, 0.9f, 4,
                              coreGrow_[2], 1.0f, 0);
                }
            }
            for (int k = 0; k < 10; ++k)
                renderBox(r, {c.x - 4.5f + k * 1.0f, kSlabH + 12.0f * g, c.z + 8.0f}, {0.4f, 24.0f * g, 8.0f}, gpuCol * 0.5f, 0.3f, 3, coreGrow_[2], 1.0f, 0);
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sz = -1; sz <= 1; sz += 2) {
                    renderBox(r, {c.x + sx * 19.5f, kSlabH + 3.0f * g, c.z + sz * 19.5f}, {5.0f, 6.0f * g, 5.0f}, gpuCol * 0.7f, 0.4f, 3, coreGrow_[2], 1.0f, 0);
                    renderBox(r, {c.x + sx * 19.5f, kSlabH + 6.4f * g, c.z + sz * 19.5f}, {3.0f, 0.8f * g, 3.0f}, gpuCol, 1.0f, 4, coreGrow_[2], std::max(0.1f, shownGpu_), 0);
                }
            for (int k = 0; k < 3; ++k)
                renderBox(r, {c.x + (k - 1) * 4.0f, kSlabH + 0.5f * g, c.z + 21.0f}, {3.0f, 1.0f * g, 1.6f}, gpuCol, 0.9f, 4, coreGrow_[2], 1.0f, 0);            float vgb = float(s.vramTotal / 1073741824.0);
            float ph = std::clamp(20.0f + vgb * 6.0f, 24.0f, 110.0f);
            renderBox(r, {c.x, kSlabH + 24.0f * g + ph * 0.5f * g, c.z}, {4, ph * g, 4}, gpuCol, 0.8f, 4, coreGrow_[2],
                      std::max(0.03f, vramUsed_), 0);
            char b[160];
            std::snprintf(b, sizeof b, "GPU  %s  %.0f GB VRAM  %s", upper(s.gpuName).c_str(), vgb, s.gpuLoad >= 0 ? pct(shownGpu_).c_str() : "--");
            labelFor(mon_[2], b, {c.x, kSlabH + 24.0f + ph + 8.0f, c.z}, 5.0f, gpuCol);
        }
    }
}

// While riding a data lane the player is a packet: a bright cube on the lane with a short light trail behind it.
void City::renderPacket(Renderer& r, Vec3 pos, const std::vector<Vec3>& path, float dist) {
    const Theme& th = currentTheme();
    renderBox(r, {pos.x, pos.y + 0.3f, pos.z}, {1.1f, 0.6f, 1.1f}, th.accent * 2.2f, 0.5f, 5, 1, 0, 0);
    const float from = std::max(0.0f, dist - 60.0f);
    float acc = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const Vec3 a = path[i], b = path[i + 1];
        const float dx = b.x - a.x, dz = b.z - a.z, sl = std::sqrt(dx * dx + dz * dz);
        const float s0 = acc, s1 = acc + sl;
        acc = s1;
        if (sl < 1e-3f || s1 < from || s0 > dist) continue;
        const float u0 = std::max(0.0f, (from - s0) / sl), u1 = std::min(1.0f, (dist - 0.8f - s0) / sl);
        if (u1 <= u0) continue;
        const Vec3 p0{a.x + dx * u0, a.y + (b.y - a.y) * u0, a.z + dz * u0}, p1{a.x + dx * u1, a.y + (b.y - a.y) * u1, a.z + dz * u1};
        const Vec3 c{(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f + 0.3f, (p0.z + p1.z) * 0.5f};
        const float lx = std::fabs(p1.x - p0.x), lz = std::fabs(p1.z - p0.z);
        renderBox(r, c, lx > lz ? Vec3{lx, 0.25f, 0.35f} : Vec3{0.35f, 0.25f, lz}, th.accent * 1.1f, 0.4f, 5, 1, 0, 0);
    }
}

// Every data link on the board as an overhead bundle of lanes (heights are absolute). The lane counts are the real ones where
// known: DMI x4 to the chipset, PCIe x16 to the graphics card, NVMe x4, a single SATA / USB lane for the rest.
void City::bundles(std::vector<Bundle>& out) const {
    out.clear();
    const SystemStats& s = data_.stats();
    const int N = std::clamp<int>(s.cores, 1, 32);
    const float plinth = std::max(14.0f, N * 4.2f / (2.0f * kPi)) * 2.0f + 10.0f;
    const float cxc = -(plinth * 0.5f + 10.0f);
    if (coreGrow_[0] > 0.3f) {
        out.push_back({0, -1, 4, 0.5f, 0.3f, coreGrow_[0], {{0, 4.0f, -13.0f}, {0, 4.0f, -55.0f + plinth * 0.5f}}});
        out.push_back({4, -1, 1, 0.5f, 0.45f, coreGrow_[0], {{13.0f, 3.0f, 0}, {28.0f, 3.0f, 0}, {28.0f, 3.0f, -12.0f}}});
        out.push_back({5, -1, 2, 0.8f, 0.4f, coreGrow_[0], {{-13.0f, 3.0f, 0}, {-28.0f, 3.0f, 0}, {-28.0f, 3.0f, -13.0f}}});
        if (coreGrow_[1] > 0.3f)
            out.push_back({2, -1, 2, 1.4f, 0.9f, std::min(coreGrow_[0], coreGrow_[1]), {{cxc - 2.5f, 4.5f, -29.0f}, {-52.0f, 4.5f, -29.0f}, {-52.0f, 4.5f, 20.0f}}});
        if (coreGrow_[2] > 0.3f)
            out.push_back({1, -1, 16, 0.5f, 0.28f, std::min(coreGrow_[0], coreGrow_[2]), {{plinth * 0.5f, 6.0f, -55.0f}, {38.0f, 6.0f, -55.0f}, {38.0f, 6.0f, 7.0f}}});
    }
    for (size_t ri = 0; ri < net_.size(); ++ri) {
        const NetRoute& nr = net_[ri];
        const Platform& pl = platforms_[nr.plat];
        if (pl.grow < 0.3f) continue;
        const float level = 5.5f + 0.5f * float(ri % 5);
        Bundle b{3, nr.plat, pl.kind == 2 ? 4 : 1, 0.5f, pl.kind == 2 ? 0.28f : 0.45f, pl.grow, {}};
        for (const Vec3& v : nr.pts) b.pts.push_back({v.x, level, v.z});
        // The lane runs on into the region and ends over its station, which sits just inside the near edge.
        if (b.pts.size() >= 2) {
            Vec3& e = b.pts.back();
            const Vec3 q = b.pts[b.pts.size() - 2];
            if (std::fabs(e.x - q.x) > std::fabs(e.z - q.z)) {
                const float s = e.x > q.x ? 1.0f : -1.0f;
                e.x = pl.cx - s * pl.side * 0.5f + s * kStDeck;
            } else {
                const float s = e.z > q.z ? 1.0f : -1.0f;
                e.z = pl.cz - s * pl.side * 0.5f + s * kStDeck;
            }
        }
        out.push_back(std::move(b));
    }
}

void City::drawBundle(Renderer& r, const Bundle& b, Vec3 col, float lf, float lb, bool anim, Vec3 eye) {
    const size_t n = b.pts.size();
    if (n < 2) return;
    const float t = float(clock_), level = b.pts[0].y, laneY = level;
    const float cull = (r.mirror() ? 250.0f : 450.0f) * drawScale_;
    std::vector<float> ux(n - 1), uz(n - 1);
    for (size_t k = 0; k + 1 < n; ++k) {
        const float dx = b.pts[k + 1].x - b.pts[k].x, dz = b.pts[k + 1].z - b.pts[k].z, l = std::max(1e-3f, std::sqrt(dx * dx + dz * dz));
        ux[k] = dx / l;
        uz[k] = dz / l;
    }
    const float width = (b.lanes - 1) * b.gap + b.laneW;
    const float act = std::max(lf, lb);
    auto farFrom = [&](float x, float z) { return std::fabs(x - eye.x) > cull || std::fabs(z - eye.z) > cull; };
    // Lanes: each is the centre line shifted sideways; corners are mitred so lanes nest round a bend.
    for (int j = 0; j < b.lanes; ++j) {
        const float o = (j - (b.lanes - 1) * 0.5f) * b.gap;
        std::vector<Vec3> v(n);
        for (size_t k = 0; k < n; ++k) {
            float ox = 0, oz = 0;
            const bool hasA = k > 0, hasB = k + 1 < n;
            if (hasA && hasB) {
                const float n1x = uz[k - 1], n1z = -ux[k - 1], n2x = uz[k], n2z = -ux[k];
                if (n1x * n2x + n1z * n2z > 0.99f) { ox = n1x; oz = n1z; }
                else { ox = n1x + n2x; oz = n1z + n2z; }
            } else {
                const size_t s = hasB ? k : k - 1;
                ox = uz[s];
                oz = -ux[s];
            }
            v[k] = {b.pts[k].x + ox * o, laneY, b.pts[k].z + oz * o};
        }
        std::vector<float> cum{0.0f};
        for (size_t k = 0; k + 1 < n; ++k) cum.push_back(cum.back() + std::fabs(v[k + 1].x - v[k].x) + std::fabs(v[k + 1].z - v[k].z));
        for (size_t k = 0; k + 1 < n; ++k) {
            const float mx = (v[k].x + v[k + 1].x) * 0.5f, mz = (v[k].z + v[k + 1].z) * 0.5f;
            if (farFrom(mx, mz) && farFrom(v[k].x, v[k].z) && farFrom(v[k + 1].x, v[k + 1].z)) continue;
            const float lx = std::fabs(v[k + 1].x - v[k].x), lz = std::fabs(v[k + 1].z - v[k].z);
            renderBox(r, {mx, laneY, mz}, lx > lz ? Vec3{lx + b.laneW, 0.18f, b.laneW} : Vec3{b.laneW, 0.18f, lz + b.laneW}, col * 0.45f, 0.3f, 5, 1, 0, 0);
        }
        const float len = cum.back();
        // Lane risers at both ends drop to the board.
        for (int e = 0; e < 2; ++e) {
            const Vec3 q = e ? v[n - 1] : v[0];
            if (farFrom(q.x, q.z) || (e && b.kind == 3)) continue;  // drive lanes end in a station instead
            renderBox(r, {q.x, (level + kSlabH) * 0.5f, q.z}, {0.2f, level - kSlabH, 0.2f}, col * (0.4f + 0.8f * act), 0.3f, 5, 1, 0, 0);
        }
        if (!anim || len < 2.0f) continue;
        // Pulses: lanes in the first half carry data away from the start, the rest bring it back; a lone lane carries both.
        const int dirs = b.lanes == 1 ? 2 : 1;
        for (int dd = 0; dd < dirs; ++dd) {
            const bool outward = b.lanes == 1 ? dd == 0 : j < (b.lanes + 1) / 2;
            const float load = outward ? lf : lb, vel = 5.0f + 55.0f * load, spacing = 20.0f;
            const int cnt = std::max(1, int(len / spacing));
            for (int k = 0; k < cnt; ++k) {
                const float d0 = float(std::fmod(double(t) * vel + k * (len / cnt) + j * 5.3f + dd * 9.0f, double(len)));
                const float d = outward ? d0 : len - d0;
                size_t sgi = 0;
                while (sgi + 2 < cum.size() && cum[sgi + 1] < d) ++sgi;
                const float u = clampf((d - cum[sgi]) / std::max(cum[sgi + 1] - cum[sgi], 1e-3f), 0.0f, 1.0f);
                const float px = v[sgi].x + (v[sgi + 1].x - v[sgi].x) * u, pz = v[sgi].z + (v[sgi + 1].z - v[sgi].z) * u;
                if (farFrom(px, pz)) continue;
                const float fade = 0.35f + 0.65f * std::sin(clampf(d / len, 0.0f, 1.0f) * kPi);
                const bool alongX = std::fabs(v[sgi + 1].x - v[sgi].x) > std::fabs(v[sgi + 1].z - v[sgi].z);
                renderBox(r, {px, laneY + 0.06f, pz}, alongX ? Vec3{1.5f, 0.34f, b.laneW + 0.1f} : Vec3{b.laneW + 0.1f, 0.34f, 1.5f},
                          col * ((0.5f + 1.7f * load) * fade), 0.3f, 5, 1, 0, 0);
            }
        }
    }
    // Gantry: a pair of legs and a crossbeam every so often, and at every corner.
    auto gantry = [&](float x, float z, float ax, float az) {
        if (farFrom(x, z)) return;
        const float sx = std::fabs(az), sz = std::fabs(ax);  // the span runs across the lane direction
        renderBox(r, {x, (level - 0.3f) * 0.5f, z}, {0.5f, level - 0.3f, 0.5f}, col * 0.5f, 0.3f, 6, 1, 0, 0);
        renderBox(r, {x, level - 0.3f, z}, {0.5f + sx * (width + 1.2f), 0.3f, 0.5f + sz * (width + 1.2f)}, col * 0.55f, 0.3f, 6, 1, 0, 0);
    };
    for (size_t k = 0; k + 1 < n; ++k) {
        const float dx = b.pts[k + 1].x - b.pts[k].x, dz = b.pts[k + 1].z - b.pts[k].z, L = std::sqrt(dx * dx + dz * dz);
        const int cnt = int(L / 24.0f);
        for (int i = 0; i < cnt; ++i) {
            const float u = (i + 0.5f) / std::max(1, cnt);
            gantry(b.pts[k].x + dx * u, b.pts[k].z + dz * u, ux[k], uz[k]);
        }
        if (k > 0) gantry(b.pts[k].x, b.pts[k].z, ux[k], uz[k]);
    }
}

// Step ring round every slab, and the stations where drive lanes end: a deck under the lane, a canopy and a wide staircase down.
void City::renderAccess(Renderer& r, const Theme& th, Vec3 eye) {
    const float cull = (r.mirror() ? 150.0f : 300.0f) * drawScale_;
    std::vector<StepRect> steps;
    stepRects(steps);
    for (const StepRect& sr : steps) {
        if (std::fabs(eye.x - sr.cx) > sr.sx * 0.5f + cull || std::fabs(eye.z - sr.cz) > sr.sz * 0.5f + cull) continue;
        const Vec3 col = sr.plat >= 0 ? tintColor(platforms_[sr.plat].tint) : th.accent;
        const float g = ease(sr.grow);
        for (int k = 0; k < kStepN; ++k) {
            const float h = kStepRise * (k + 1) * g, o = (kStepN - k) * kStepTread;
            renderBox(r, {sr.cx, h * 0.5f, sr.cz}, {sr.sx + 2.0f * o, h, sr.sz + 2.0f * o}, col * 0.8f, 0.4f, 6, sr.grow, 1.0f, 0);
        }
    }
    std::vector<Station> sts;
    stations(sts);
    for (const Station& st : sts) {
        if (std::fabs(eye.x - st.x) > cull || std::fabs(eye.z - st.z) > cull) continue;
        const Vec3 col = tintColor(platforms_[st.plat].tint);
        const float deckTop = st.level - 0.3f, H = deckTop - kSlabH;
        const int n = std::max(2, (int)std::ceil(H / kStMaxRise));
        const float h = H / n;        const float nx = st.fz, nz = -st.fx;
        auto at = [&](float a, float w, float y) { return Vec3{st.x + st.fx * a + nx * w, y, st.z + st.fz * a + nz * w}; };
        auto box = [&](float a0, float a1, float y0, float y1, float w, Vec3 c, int kind) {
            const Vec3 m = at((a0 + a1) * 0.5f, 0, (y0 + y1) * 0.5f);
            const bool alongX = std::fabs(st.fx) > 0.5f;
            const float len = std::fabs(a1 - a0);
            renderBox(r, m, alongX ? Vec3{len, y1 - y0, w} : Vec3{w, y1 - y0, len}, c, 0.4f, kind, 1, 1.0f, 0);
        };
        box(-kStDeck, 0.4f, kSlabH, deckTop, 2.0f * kStHalfW, col * 0.5f, 6);
        box(-kStDeck, 0.4f, deckTop, deckTop + 0.12f, 2.0f * kStHalfW + 0.2f, col * 1.1f, 5);
        // stairs run on past the end of the lane, down onto the region platform
        for (int k = 0; k + 1 < n; ++k) {
            box(0.4f + k * kStRun, 0.4f + (k + 1) * kStRun, kSlabH, deckTop - (k + 1) * h, 2.0f * kStHalfW, col * 0.5f, 6);
            box(0.4f + k * kStRun, 0.4f + k * kStRun + 0.1f, deckTop - (k + 1) * h - 0.02f, deckTop - (k + 1) * h + 0.05f, 2.0f * kStHalfW, col * 0.9f, 5);
        }
        // canopy on four posts
        for (int i = 0; i < 4; ++i) {
            const Vec3 q = at(i < 2 ? -kStDeck + 0.5f : -0.4f, (i % 2 ? 1.0f : -1.0f) * (kStHalfW - 0.3f), deckTop + 1.6f);
            renderBox(r, q, {0.3f, 3.2f, 0.3f}, col * 0.6f, 0.4f, 6, 1, 1.0f, 0);
        }
        box(-kStDeck - 0.3f, 0.7f, deckTop + 3.2f, deckTop + 3.5f, 2.0f * kStHalfW + 0.8f, col * 0.9f, 5);
        // handrails down both sides of the stairs
        for (int sd = -1; sd <= 1; sd += 2) {
            const Vec3 q0 = at(0.4f, sd * kStHalfW, deckTop + 0.9f), q1 = at(0.4f + n * kStRun, sd * kStHalfW, kSlabH + 0.9f);
            for (int i = 0; i < 14; ++i) {
                const float u = i / 13.0f;
                renderBox(r, {q0.x + (q1.x - q0.x) * u, q0.y + (q1.y - q0.y) * u, q0.z + (q1.z - q0.z) * u}, {0.16f, 0.16f, 0.16f}, col * 1.2f, 0.3f, 5, 1, 1.0f, 0);
            }
        }
    }
}
void City::renderBundles(Renderer& r, const Theme& th, Vec3 eye) {
    std::vector<Bundle> bs;
    bundles(bs);
    if (bs.empty()) return;
    const bool anim = !r.mirror() && coreGrow_[0] > 0.99f;
    const float R = shownDiskR_, W = shownDiskW_, Rx = shownNetRx_, Tx = shownNetTx_;
    const Vec3 green{0.12f, 0.85f, 0.42f};
    for (const Bundle& b : bs) {
        Vec3 col = th.accent;
        float lf = 0.1f, lb = 0.1f;
        switch (b.kind) {
        case 0: lf = std::min(1.0f, R + Rx); lb = std::min(1.0f, W + Tx); break;  // chipset to CPU: reads and receive; writes and send
        case 1: col = green; lf = shownGpu_; lb = 0.5f * shownGpu_; break;
        case 2: col = th.alt; lf = lb = shownRam_; break;
        case 3: col = tintColor(platforms_[b.plat].tint); lf = W; lb = R; break;  // out to the drive = writes, back = reads
        case 4: col = green; lf = Tx; lb = Rx; break;
        default: lf = lb = 0.15f; break;
        }
        const bool full = b.grow > 0.95f;
        drawBundle(r, b, col, lf, lb, anim && full, eye);
    }
    if (r.mirror() || coreGrow_[0] < 0.99f) return;
    struct L { int kind; int slot; const char* text; };
    const L labels[3] = {{0, 7, "DMI X4  CHIPSET TO CPU"}, {1, 5, "PCIE X16  CPU TO GPU"}, {2, 6, "MEMORY BUS  2 CHANNELS"}};
    for (const L& l : labels)
        for (const Bundle& b : bs) {
            if (b.kind != l.kind) continue;
            // Label the longest straight run.
            size_t best = 0;
            float bl = -1;
            for (size_t k = 0; k + 1 < b.pts.size(); ++k) {
                const float d = std::fabs(b.pts[k + 1].x - b.pts[k].x) + std::fabs(b.pts[k + 1].z - b.pts[k].z);
                if (d > bl) { bl = d; best = k; }
            }
            const Vec3 m{(b.pts[best].x + b.pts[best + 1].x) * 0.5f, b.pts[best].y + 2.5f, (b.pts[best].z + b.pts[best + 1].z) * 0.5f};
            moduleLabel(r, modLabel_[l.slot], l.text, m, 1.8f, eye, b.kind == 1 ? green : (b.kind == 2 ? th.alt : th.accent));
        }
}
void City::render(Renderer& r, Vec3 eye) {
    labelBudget_ = 2;
    pendingLabels_.clear();
    renderMonuments(r, eye);
    // While the file system is still being read, square scan pulses ripple out of the hub across the ground.
    bool scanning = !built_;
    for (const auto& d : snap_) scanning = scanning || !d.done;
    if (false && scanning && coreGrow_[0] > 0.3f) {  // scan pulses disabled
        const Theme& th = currentTheme();
        for (int k = 0; k < 3; ++k) {
            const float ph = float(std::fmod(clock_ / 7.0 + k / 3.0, 1.0));
            const float rad = 30.0f + ph * std::max(300.0f, extent_ * 0.8f), a = (1.0f - ph) * (1.0f - ph) * 0.9f;
            const Vec3 col = th.accent * a;
            const float y = kRoadY + 0.05f, w = 1.4f;
            renderBox(r, {0, y, -rad}, {2 * rad, 0.1f, w}, col, 0.3f, 5, 1, 0, 0);
            renderBox(r, {0, y, rad}, {2 * rad, 0.1f, w}, col, 0.3f, 5, 1, 0, 0);
            renderBox(r, {-rad, y, 0}, {w, 0.1f, 2 * rad}, col, 0.3f, 5, 1, 0, 0);
            renderBox(r, {rad, y, 0}, {w, 0.1f, 2 * rad}, col, 0.3f, 5, 1, 0, 0);
        }
    }
    {
        // Central hub where every road and the CPU / RAM / GPU spokes meet.
        const Theme& th = currentTheme();
        const SystemStats& s = data_.stats();
        float g = ease(coreGrow_[0]);
        if (g > 0) {
            renderBox(r, {0, kSlabH * 0.5f * g, 0}, {26, kSlabH * g, 26}, th.accent, 0.35f, 6, coreGrow_[0], 1.0f, 0);
            renderHub(r, th, g, eye);
        }
    }
    renderBundles(r, currentTheme(), eye);
    renderAccess(r, currentTheme(), eye);
    if (!built_) return;
    const bool mirror = r.mirror();
    const float maxD = 900.0f * drawScale_, maxD2 = maxD * maxD;
    int draws = 0;

    if (!netBuilt_ && built_) buildNetwork();
    const int n = (int)platforms_.size();
    for (int i = 0; i < n; ++i) {
        auto& p = platforms_[i];
        Vec3 tint = tintColor(p.tint);
        float dx = eye.x - p.cx, dz = eye.z - p.cz;
        if (dx * dx + dz * dz > maxD2 + p.side * p.side) continue;

        if (p.grow <= 0) continue;
        float pg = ease(p.grow);
        renderBox(r, {p.cx, kSlabH * 0.5f * pg, p.cz}, {p.side, kSlabH * pg, p.side}, tint, 0.5f, 6, p.grow, 0, 0);

        const float base = kSlabH + kPlateH;
        for (size_t bi = 0; bi < p.blocks.size(); ++bi) {
            auto& b = p.blocks[bi];
            const float bdx = eye.x - b.cx, bdz = eye.z - b.cz;
            const float bd2 = bdx * bdx + bdz * bdz;
            if (bd2 > (mirror ? 450.0f * 450.0f * drawScale_ * drawScale_ : maxD2)) continue;
            if (!r.visible({b.cx, base + b.maxH * 0.5f, b.cz}, b.side * 0.72f + b.maxH * 0.5f + 6.0f)) continue;
            // The block plate: a lighter raised pad the buildings stand on.
            const float plateH = (kSlabH + kPlateH) * pg;
            // The pad's rim is a three-step slope (matching blockPadHeight) so streets run up onto it smoothly.
            for (int st = 0; st < 3; ++st) {
                const float sd = b.side - 2.0f - 2.0f * kPadSlope * st / 3.0f, hh = (kSlabH + kPlateH * (st + 1) / 3.0f) * pg;
                renderBox(r, {b.cx, hh * 0.5f, b.cz}, {sd, hh, sd}, tint, 0.5f, 6, p.grow, 1.0f, 0);
            }
            if (bd2 < 220.0f * 220.0f && p.grow > 0.95f) {
                // Street grid: lane markings down the middle of every street between rows and columns of buildings.
                const float y = kSlabH + kPlateH + 0.04f, len = b.side - 4.0f;
                for (int k = 0; k + 1 < b.g; ++k) {
                    const float off = (float(k) - (b.g - 1) * 0.5f + 0.5f) * p.pitch;
                    renderBox(r, {b.cx, y, b.cz + off}, {len, 0.06f, 0.3f}, tint, 0.5f, 5, 1, 0, 0);
                    renderBox(r, {b.cx + off, y, b.cz}, {0.3f, 0.06f, len}, tint, 0.5f, 5, 1, 0, 0);
                    draws += 2;
                }
            }
            std::vector<TPart> parts;
            for (size_t ti = 0; ti < b.towers.size(); ++ti) {
                auto& t = b.towers[ti];
                if (t.grow <= 0 || draws > 3000) continue;
                float h = t.curH * ease(t.grow);
                float w = std::max(1.5f, t.curW), dd = std::max(1.5f, t.curD);
                float tdx = eye.x - t.x, tdz = eye.z - t.z;
                if (tdx * tdx + tdz * tdz > (mirror ? 350.0f * 350.0f * drawScale_ * drawScale_ : maxD2)) continue;
                if (!r.visible({t.x, base + h * 0.5f, t.z}, 0.5f * std::sqrt(w * w + dd * dd + h * h) + 5.0f)) continue;
                float seed = rnd01(uint32_t(std::hash<std::string>{}(t.name) & 0xFFFFFF));
                const bool hovered = !mirror && hoverP_ == i && hoverB_ == (int)bi && hoverT_ == (int)ti;
                if (!mirror && tdx * tdx + tdz * tdz < 180.0f * 180.0f)  // raised sidewalk around the building
                    renderBox(r, {t.x, base + 0.07f, t.z}, {w + 4.0f, 0.14f, dd + 4.0f}, tint, 0.5f, 6, t.grow, 1.0f, 0);
                towerParts(t.style, w, dd, h, parts);
                // Far away only the base and the tallest section are drawn; the rest are sub-pixel detail.
                const bool farLod = tdx * tdx + tdz * tdz > 300.0f * 300.0f * drawScale_ * drawScale_;
                float topY = 0.0f;
                if (farLod) for (const TPart& tp : parts) topY = std::max(topY, tp.y1);
                for (size_t pi = 0; pi < parts.size(); ++pi) {
                    const TPart& tp = parts[pi];
                    if (farLod && pi > 0 && tp.y1 < topY - 0.01f) continue;
                    renderBox(r, {t.x + tp.cx, base + (tp.y0 + tp.y1) * 0.5f, t.z + tp.cz}, {tp.sx, tp.y1 - tp.y0, tp.sz}, tp.glow ? Vec3{0.9f, 1.0f, 1.0f} : tint,
                              seed, tp.glow ? 4 : 3, t.grow, tp.glow ? 1.0f : 0.0f, hovered ? 0.6f : t.flash);
                    ++draws;
                }
                if (!mirror && h > 3.4f && tdx * tdx + tdz * tdz < 70.0f * 70.0f && t.grow > 0.95f) {  // street door on the +z face
                    if (!t.doorLabel && labelBudget_ > 0) {
                        t.doorLabel = makeDoorLabel(t.name == "(FILES)" || t.name == "(MORE)" ? b.name + " " + t.name : t.name, tint);
                        --labelBudget_;
                    }
                    if (t.doorLabel) {
                        const float dh = std::min(3.4f, h * 0.8f), dw = std::min(dh * 0.8f, w * 0.85f);
                        const float zf = t.z + dd * 0.5f;
                        const Vec3 glowC = tint * 0.8f + Vec3{0.3f, 0.3f, 0.3f};
                        for (float sg : {-1.0f, 1.0f}) renderBox(r, {t.x + sg * (dw * 0.5f + 0.12f), base + dh * 0.5f + 0.1f, zf + 0.1f}, {0.14f, dh + 0.2f, 0.22f}, glowC, 0.5f, 4, 1, 1.0f, 0);
                        renderBox(r, {t.x, base + dh + 0.18f, zf + 0.1f}, {dw + 0.5f, 0.16f, 0.26f}, glowC, 0.5f, 4, 1, 1.0f, 0);
                        renderBox(r, {t.x, base + dh + 0.6f, zf + 0.6f}, {dw + 1.6f, 0.1f, 1.3f}, tint, seed, 3, 1, 0, 0);
                        renderBox(r, {t.x, base + 0.05f, zf + 1.5f}, {dw + 1.2f, 0.05f, 2.4f}, tint, 0.5f, 6, 1, 1.0f, 0);
                        draws += 5;
                        r.drawCube(*t.doorLabel, Mat4::translate({t.x, base + dh * 0.5f, zf + 0.03f}) * Mat4::scale({dw, dh, 0.06f}),
                                   {1.0f, 1.0f, 1.0f}, false, true, 0.0f);
                        ++draws;
                    }
                }
                if (std::fabs(t.flash) > 0.12f) {
                    float bh = 70.0f * std::fabs(t.flash);
                    Vec3 bc = t.flash > 0 ? Vec3{0.6f, 1.0f, 0.8f} : Vec3{1.0f, 0.3f, 0.1f};
                    renderBox(r, {t.x, base + h + bh * 0.5f, t.z}, {0.7f, bh, 0.7f}, bc, 0.5f, 7, 1, 0, t.flash);
                    ++draws;
                }
            }
        }

        if (mirror) continue;
        renderDriveBeacon(r, p, tint, pg);
        // Drive label above the platform.
        {
            char b[160];
            static const char* kKind[4] = {"HARD DISK", "SATA SSD", "NVME SSD", "USB DRIVE"};
            const char* kn = kKind[std::clamp(p.kind, 0, 3)];
            if (p.done)
                std::snprintf(b, sizeof b, "%s  %s  %s / %s  %s FILES", p.name.c_str(), kn, fmtBytes(p.bytes).c_str(),
                              fmtBytes(p.total).c_str(), fmtCount(p.files).c_str());
            else
                std::snprintf(b, sizeof b, "%s  %s  SCANNING  %s FILES  %s", p.name.c_str(), kn, fmtCount(p.files).c_str(),
                              fmtBytes(p.bytes).c_str());
            std::string text = b;
            if ((!p.label || (text != p.labelText && wall_ - p.labelTime > 1.5)) && labelBudget_ > 0) {
                p.label = makeText(text);
                p.labelText = text;
                p.labelTime = (float)wall_;
                --labelBudget_;
            }
            if (p.label && pg > 0.5f)
                drawLabel(r, *p.label, {p.cx, p.maxH + 14.0f, p.cz}, 7.0f, eye, tint);
        }
        // Block names (large, visible from further away) and building names (close up only).
        for (auto& b : p.blocks) {
            float bdx = eye.x - b.cx, bdz = eye.z - b.cz;
            if (bdx * bdx + bdz * bdz < 260.0f * 260.0f && !b.towers.empty()) {
                if (!b.label && labelBudget_ > 0) {
                    b.label = makeText(upper(b.name));
                    --labelBudget_;
                }
                if (b.label) drawLabel(r, *b.label, {b.cx, base + b.maxH + 7.0f, b.cz}, 4.5f, eye, tint);
            }
            for (auto& t : b.towers) {
                if (t.grow < 0.9f) continue;
                float tdx = eye.x - t.x, tdz = eye.z - t.z;
                float d2 = tdx * tdx + tdz * tdz + (eye.y - t.curH) * (eye.y - t.curH);
                if (d2 > 110.0f * 110.0f) continue;
                if (!t.label && labelBudget_ > 0) {
                    t.label = makeText(upper(t.name == "(FILES)" || t.name == "(MORE)" ? b.name + " " + t.name : t.name));
                    --labelBudget_;
                }
                if (t.label) drawLabel(r, *t.label, {t.x, base + t.curH + 2.6f, t.z}, 2.2f, eye, tint);
            }
        }
    }
    flushLabels(r);
}



void City::stepRects(std::vector<StepRect>& out) const {
    out.clear();
    const SystemStats& s = data_.stats();
    for (size_t i = 0; i < platforms_.size(); ++i) {
        const auto& p = platforms_[i];
        if (p.grow >= 0.3f) out.push_back({p.cx, p.cz, p.side, p.side, (int)i, p.grow});
    }
    if (coreGrow_[0] > 0.3f) {
        const float ringR = std::max(14.0f, std::clamp<int>(s.cores, 1, 32) * 4.2f / (2.0f * kPi)), pl = ringR * 2.0f + 10.0f;
        out.push_back({0, 0, 26, 26, -1, coreGrow_[0]});
        out.push_back({0, -55, pl, pl, -1, coreGrow_[0]});
    }
    if (coreGrow_[1] > 0.3f) {
        const int K = std::clamp<int>((int)(s.ramTotal / 1073741824.0 / 4.0 + 0.5), 2, 16);
        out.push_back({-52, 30, K * 3.4f + 6.0f, 20, -1, coreGrow_[1]});
    }
    if (coreGrow_[2] > 0.3f) out.push_back({52, 30, 46, 46, -1, coreGrow_[2]});
}

void City::stations(std::vector<Station>& out) const {
    out.clear();
    std::vector<Bundle> bs;
    bundles(bs);
    for (const Bundle& b : bs) {
        if (b.kind != 3 || b.grow < 0.9f || b.pts.size() < 2) continue;
        const Vec3 e = b.pts.back();
        for (size_t k = b.pts.size() - 1; k-- > 0;) {
            const float dx = e.x - b.pts[k].x, dz = e.z - b.pts[k].z, l = std::sqrt(dx * dx + dz * dz);
            if (l < 0.01f) continue;
            out.push_back({e.x, e.z, dx / l, dz / l, e.y, b.plat});
            break;
        }
    }
}

void City::buildNetwork() const {
    net_.clear();
    pieces_.clear();
    bridgeFloor_.clear();
    netBuilt_ = true;
    const SystemStats& s = data_.stats();
    const int n = (int)platforms_.size();
    const int N = std::clamp<int>(s.cores, 1, 32);
    const float plinth = std::max(14.0f, N * 4.2f / (2.0f * kPi)) * 2.0f + 10.0f;
    const int K = std::clamp<int>((int)(s.ramTotal / 1073741824.0 / 4.0 + 0.5), 2, 16);
    const float rx = -52.0f + (K * 3.4f + 6.0f) * 0.5f;
    struct Rect { float x0, x1, z0, z1; };
    struct Seg { float ax, az, bx, bz; };
    using Poly = std::vector<Vec3>;  // x, z in .x/.z; .y unused until bridged
    const Poly coreRoutes[1] = {{{0, 0, 0}, {0, 0, -55 + plinth * 0.5f}}};  // the chipset to CPU link
    const float cxc = -(plinth * 0.5f + 10.0f), vx = plinth * 0.5f + 9.0f;
    // Everything solid on the board, so drive lanes (and their gantry legs) keep clear of it.
    const std::vector<Rect> solids = {{-plinth * 0.5f, plinth * 0.5f, -55 - plinth * 0.5f, -55 + plinth * 0.5f},
                                      {-52 - (K * 3.4f + 6.0f) * 0.5f, rx, 20, 40}, {29, 75, 7, 53},
                                      {cxc - 6.5f, cxc + 6.5f, -76.0f, -37.0f}, {cxc - 4.5f, cxc + 4.5f, -33.5f, -24.5f},
                                      {-33.0f, -23.0f, -21.0f, -13.0f}, {23.0f, 33.0f, -22.0f, -12.0f},
                                      {vx - 5.5f, vx + 5.5f, -51.5f, -40.5f}, {vx - 5.5f, vx + 5.5f, -70.5f, -59.5f}};
    auto segOf = [](const Poly& p, size_t k) { return Seg{p[k].x, p[k].z, p[k + 1].x, p[k + 1].z}; };
    auto horiz = [](const Seg& a) { return std::fabs(a.az - a.bz) < 1e-3f; };
    auto hitsRect = [](const Seg& a, const Rect& r, float pad) {
        return std::max(a.ax, a.bx) + 1.5f > r.x0 - pad && std::min(a.ax, a.bx) - 1.5f < r.x1 + pad &&
               std::max(a.az, a.bz) + 1.5f > r.z0 - pad && std::min(a.az, a.bz) - 1.5f < r.z1 + pad;
    };
    // Interior crossing of two perpendicular segments (junctions at the ends and inside the hub do not count).
    auto cross = [&](const Seg& a, const Seg& b, float& cx, float& cz) {
        const bool ha = horiz(a), hb = horiz(b);
        if (ha == hb) return false;
        const Seg& h = ha ? a : b;
        const Seg& v = ha ? b : a;
        cx = v.ax;
        cz = h.az;
        const float e = 2.0f;
        if (cx < std::min(h.ax, h.bx) + e || cx > std::max(h.ax, h.bx) - e) return false;
        if (cz < std::min(v.az, v.bz) + e || cz > std::max(v.az, v.bz) - e) return false;
        return !(std::fabs(cx) < 14.0f && std::fabs(cz) < 14.0f);
    };
    auto overlap = [&](const Seg& a, const Seg& b) {
        const bool ha = horiz(a), hb = horiz(b);
        if (ha != hb) return false;
        const float pa = ha ? a.az : a.ax, pb = ha ? b.az : b.ax;
        if (std::fabs(pa - pb) > 3.2f) return false;
        const float a0 = ha ? std::min(a.ax, a.bx) : std::min(a.az, a.bz), a1 = ha ? std::max(a.ax, a.bx) : std::max(a.az, a.bz);
        const float b0 = ha ? std::min(b.ax, b.bx) : std::min(b.az, b.bz), b1 = ha ? std::max(b.ax, b.bx) : std::max(b.az, b.bz);
        const float lo = std::max(a0, b0), hi = std::min(a1, b1);
        return hi - lo > 1.0f && std::max(std::fabs(lo), std::fabs(hi)) > 16.0f;
    };
    auto segLen = [](const Seg& a) { return std::fabs(a.bx - a.ax) + std::fabs(a.bz - a.az); };

    std::vector<Poly> chosen;  // indexed like platforms_ (empty when a platform has no route)
    chosen.resize(n);
    auto others = [&](int self, auto&& fn) {  // visits core routes and routes already planned before self
        for (const Poly& c : coreRoutes) for (size_t k = 0; k + 1 < c.size(); ++k) fn(segOf(c, k));
        for (int j = 0; j < self; ++j) for (size_t k = 0; k + 1 < chosen[j].size(); ++k) fn(segOf(chosen[j], k));
    };
    // Routes are planned on a coarse grid with a Dijkstra search: platforms, monuments and the hub are walls, turns cost a little,
    // and running over an existing lane (crossing it) costs more but is allowed, because the crossing becomes a bridge.
    float minX = -90, maxX = 90, minZ = -130, maxZ = 90;
    for (const auto& pl : platforms_) {
        minX = std::min(minX, pl.cx - pl.side * 0.5f);
        maxX = std::max(maxX, pl.cx + pl.side * 0.5f);
        minZ = std::min(minZ, pl.cz - pl.side * 0.5f);
        maxZ = std::max(maxZ, pl.cz + pl.side * 0.5f);
    }
    const float margin = 30.0f;
    float cs = 4.0f;
    while ((maxX - minX + 2 * margin) / cs > 380.0f || (maxZ - minZ + 2 * margin) / cs > 380.0f) cs += 1.0f;
    const int gx0 = (int)std::floor((minX - margin) / cs), gz0 = (int)std::floor((minZ - margin) / cs);
    const int W = (int)std::ceil((maxX + margin) / cs) - gx0 + 1, D = (int)std::ceil((maxZ + margin) / cs) - gz0 + 1;
    std::vector<int> raw(W * D, -1), inf(W * D, -1);
    std::vector<uint8_t> solid(W * D, 0), occ(W * D, 0);
    auto stamp = [&](float x0, float x1, float z0, float z1, auto&& fn) {
        const int i0 = std::max(0, (int)std::floor(x0 / cs) - gx0), i1 = std::min(W - 1, (int)std::ceil(x1 / cs) - gx0);
        const int j0 = std::max(0, (int)std::floor(z0 / cs) - gz0), j1 = std::min(D - 1, (int)std::ceil(z1 / cs) - gz0);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const float x = (gx0 + i) * cs, z = (gz0 + j) * cs;
                if (x >= x0 && x <= x1 && z >= z0 && z <= z1) fn(j * W + i);
            }
    };
    for (int j = 0; j < n; ++j) {
        const auto& pl = platforms_[j];
        const float h = pl.side * 0.5f;
        stamp(pl.cx - h, pl.cx + h, pl.cz - h, pl.cz + h, [&](int c) { raw[c] = j; });
        stamp(pl.cx - h - 3.5f, pl.cx + h + 3.5f, pl.cz - h - 3.5f, pl.cz + h + 3.5f, [&](int c) { inf[c] = (inf[c] == -1 || inf[c] == j) ? j : -3; });
    }
    for (const Rect& r : solids) stamp(r.x0 - 3.5f, r.x1 + 3.5f, r.z0 - 3.5f, r.z1 + 3.5f, [&](int c) { solid[c] = 1; });
    stamp(-13, 13, -13, 13, [&](int c) { solid[c] = 1; });
    auto markPoly = [&](const Poly& pts) {
        const float hw = 1.5f + cs * 0.5f - 0.01f;
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const Seg sg = segOf(pts, k);
            if (horiz(sg)) stamp(std::min(sg.ax, sg.bx) - 0.01f, std::max(sg.ax, sg.bx) + 0.01f, sg.az - hw, sg.az + hw, [&](int c) { occ[c] |= 1; });
            else stamp(sg.ax - hw, sg.ax + hw, std::min(sg.az, sg.bz) - 0.01f, std::max(sg.az, sg.bz) + 0.01f, [&](int c) { occ[c] |= 2; });
        }
    };
    for (const Poly& c : coreRoutes) markPoly(c);

    const int DX[4] = {1, -1, 0, 0}, DZ[4] = {0, 0, 1, -1};
    const float ringD = cs * std::ceil(16.0f / cs);
    auto planRoute = [&](int own, bool cpu, Poly& out) {
        const auto& pl = platforms_[own];
        const float h = pl.side * 0.5f;
        std::vector<float> dist(size_t(W) * D * 4, 1e30f);
        std::vector<int> prev(dist.size(), -1);
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        auto blocked = [&](int c) { return solid[c] || raw[c] >= 0 || inf[c] == -3 || (inf[c] >= 0 && inf[c] != own); };
        auto cellAt = [&](float x, float z) { return (int)std::lround(z / cs - gz0) * W + (int)std::lround(x / cs - gx0); };
        const int m = (int)std::floor(12.0f / cs);
        if (cpu) {
            // Drives wired straight to the CPU leave from the north face of its package.
            const float half = plinth * 0.5f, rd = cs * std::ceil((half + 4.0f) / cs);
            const int mm = (int)std::floor((half - 2.0f) / cs);
            for (int k = -mm; k <= mm; ++k) {
                const int c = cellAt(k * cs, -55.0f - rd);
                if (c < 0 || c >= W * D || blocked(c) || occ[c]) continue;
                dist[c * 4 + 3] = 0;
                pq.push({0.0f, c * 4 + 3});
            }
        }
        for (int k = -m; k <= m && !cpu; ++k)
            for (int d = 0; d < 4; ++d) {
                const float x = d < 2 ? DX[d] * ringD : k * cs, z = d < 2 ? k * cs : DZ[d] * ringD;
                const int c = cellAt(x, z);
                if (c < 0 || c >= W * D || blocked(c) || occ[c]) continue;
                dist[c * 4 + d] = 0;
                pq.push({0.0f, c * 4 + d});
            }
        int goal = -1;
        while (!pq.empty()) {
            const auto [dd, s] = pq.top();
            pq.pop();
            if (dd > dist[s]) continue;
            const int c = s >> 2, d = s & 3, ix = c % W, iz = c / W;
            const int nx = ix + DX[d], nz = iz + DZ[d];
            const bool inside = nx >= 0 && nx < W && nz >= 0 && nz < D;
            if (inside && raw[nz * W + nx] == own) { goal = s; break; }
            auto relax = [&](int ns, float cost) {
                if (dd + cost < dist[ns]) { dist[ns] = dd + cost; prev[ns] = s; pq.push({dd + cost, ns}); }
            };
            if (inside) {
                const int nc = nz * W + nx;
                const uint8_t bit = d < 2 ? 1 : 2;
                if (!blocked(nc) && !(occ[nc] & bit)) relax(nc * 4 + d, 1.0f + (occ[nc] ? 14.0f : 0.0f));
            }
            if (!occ[c]) {
                const int turn[2] = {d < 2 ? 2 : 0, d < 2 ? 3 : 1};
                for (int d2 : turn) relax(c * 4 + d2, 3.0f);
            }
        }
        if (goal < 0) return false;
        std::vector<int> seq;
        for (int s = goal; s >= 0; s = prev[s]) seq.push_back(s);
        std::reverse(seq.begin(), seq.end());
        auto cx = [&](int c) { return (gx0 + c % W) * cs; };
        auto cz = [&](int c) { return (gz0 + c / W) * cs; };
        const int s0 = seq.front(), c0 = s0 >> 2, d0 = s0 & 3;
        out.clear();
        if (cpu) out.push_back({cx(c0), 0, -55.0f - plinth * 0.5f});
        else out.push_back(d0 < 2 ? Vec3{DX[d0] * 13.0f, 0, cz(c0)} : Vec3{cx(c0), 0, DZ[d0] * 13.0f});
        for (size_t k = 1; k < seq.size(); ++k)
            if ((seq[k] & 3) != (seq[k - 1] & 3)) out.push_back({cx(seq[k] >> 2), 0, cz(seq[k] >> 2)});
        const int cg = goal >> 2, dg = goal & 3;
        if (dg == 0) out.push_back({pl.cx - h, 0, cz(cg)});
        else if (dg == 1) out.push_back({pl.cx + h, 0, cz(cg)});
        else if (dg == 2) out.push_back({cx(cg), 0, pl.cz - h});
        else out.push_back({cx(cg), 0, pl.cz + h});
        return out.size() >= 2;
    };

    // The first NVMe drive hangs directly off the CPU, as on a real board; everything else goes through the chipset.
    int cpuDrive = -1;
    for (int i = 0; i < n && cpuDrive < 0; ++i)
        if (platforms_[i].kind == 2) cpuDrive = i;
    for (int i = 0; i < n; ++i) {
        const auto& p = platforms_[i];
        Poly bp;
        if (!(i == cpuDrive && planRoute(i, true, bp)) && !planRoute(i, false, bp)) {
            const float lane = (i - (n - 1) * 0.5f) * 3.5f, sz = p.cz >= 0 ? 1.0f : -1.0f;
            bp = {{0, 0, lane}, {p.cx, 0, lane}, {p.cx, 0, p.cz - sz * p.side * 0.5f}};
        }
        markPoly(bp);
        chosen[i] = bp;
    }
    // Overhead gantries pass over one another at their own heights, so the planned routes need no bridges.
    for (int i = 0; i < n; ++i) {
        const Poly& src = chosen[i];
        if (src.size() < 2) continue;
        NetRoute nr{i, {}};
        for (const Vec3& v : src)
            if (nr.pts.empty() || std::fabs(v.x - nr.pts.back().x) + std::fabs(v.z - nr.pts.back().z) > 0.01f) nr.pts.push_back({v.x, 0, v.z});
        if (nr.pts.size() >= 2) net_.push_back(std::move(nr));
    }
}
void City::drawSlope(Renderer& r, Vec3 a, Vec3 b, float width, float th, Vec3 neon, float seed, int kind, float emerge) {
    const float dx = b.x - a.x, dz = b.z - a.z, dy = b.y - a.y, hl = std::sqrt(dx * dx + dz * dz);
    const float len = std::sqrt(hl * hl + dy * dy);
    if (len < 1e-3f) return;
    const Vec3 c{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - th * 0.5f, (a.z + b.z) * 0.5f};
    const Mat4 w = Mat4::translate(c) * Mat4::rotateY(std::atan2(dx, dz)) * Mat4::rotateX(-std::atan2(dy, hl)) * Mat4::scale({width, th, len});
    r.drawBuilding(*dummy_, w, neon, seed, kind, emerge, 0.0f, 0.0f);
}
int City::pickMonument(Vec3 o, Vec3 d) const {
    std::vector<Box> boxes;
    monumentBoxes(boxes);
    int best = -1;
    float bestT = 1e30f;
    for (const Box& b : boxes) {
        const float cx = (b.x0 + b.x1) * 0.5f, cz = (b.z0 + b.z1) * 0.5f;
        int id = cz < -20 ? 0 : (cx < -20 ? 1 : (cx > 20 ? 2 : -1));
        if (id < 0) continue;
        const float lo[3] = {b.x0, 0.0f, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
        const float org[3] = {o.x, o.y, o.z}, dr[3] = {d.x, d.y, d.z};
        float t0 = 0, t1 = 1e30f;
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i) {
            if (std::fabs(dr[i]) < 1e-6f) {
                if (org[i] < lo[i] || org[i] > hi[i]) ok = false;
            } else {
                float a = (lo[i] - org[i]) / dr[i], c = (hi[i] - org[i]) / dr[i];
                if (a > c) std::swap(a, c);
                t0 = std::max(t0, a);
                t1 = std::min(t1, c);
                if (t0 > t1) ok = false;
            }
        }
        if (ok && t0 < bestT) {
            bestT = t0;
            best = id;
        }
    }
    return best;
}
void City::monumentBoxes(std::vector<Box>& out) const {
    const SystemStats& s = data_.stats();
    auto add = [&](float cx, float cz, float hx, float hz, float top) { out.push_back({cx - hx, cx + hx, top, cz - hz, cz + hz}); };
    if (coreGrow_[0] > 0.3f) {
        int N = std::clamp<int>(s.cores, 1, 32);
        float ringR = std::max(14.0f, N * 4.2f / (2.0f * kPi));
        float plinth = ringR * 2.0f + 10.0f;
        add(0, -55, plinth * 0.5f, plinth * 0.5f, kSlabH);
        add(0, -55, 4, 4, kSlabH + 90);
        {
            const float ex = plinth * 0.5f + 9.0f;
            add(ex, -46, 5.0f, 5.0f, kSlabH + 3.2f);
            add(ex, -65, 4.5f, 4.5f, kSlabH + 2.0f);
            add(-(plinth * 0.5f + 10.0f), -29, 3.0f, 3.0f, kSlabH + 1.6f);
            add(-28, -17, 4.0f, 2.8f, kSlabH + 1.5f);
            add(28, -17, 3.5f, 3.5f, kSlabH + 1.0f);
        }
        {
            const float ccx = -(plinth * 0.5f + 10.0f), cz3[3] = {-70.0f, -58.0f, -44.0f}, sd[3] = {3.0f, 4.0f, 5.5f}, ht[3] = {2.5f, 3.5f, 5.0f};
            for (int m = 0; m < 3; ++m) add(ccx, cz3[m], sd[m], sd[m], kSlabH + ht[m]);
        }
        for (int i = 0; i < N; ++i) {
            float a = 2.0f * kPi * i / N;
            add(ringR * std::cos(a), -55 + ringR * std::sin(a), 1.3f, 1.3f, kSlabH + 30);
        }
        add(0, 0, 13, 13, kSlabH);
        add(0, 0, 8, 8, kSlabH + 0.8f);
        add(0, 0, 5.5f, 5.5f, kSlabH + 3.0f);
        add(0, 0, 3.5f, 3.5f, kSlabH + 6.8f);
        for (int i = 0; i < 4; ++i) add((i % 2 ? 11.0f : -11.0f), (i / 2 ? 11.0f : -11.0f), 1.1f, 1.1f, kSlabH + 7.5f);
    }
    if (coreGrow_[1] > 0.3f) {
        int K = std::clamp<int>((int)(s.ramTotal / 1073741824.0 / 4.0 + 0.5), 2, 16);
        add(-52, 30, (K * 3.4f + 6.0f) * 0.5f, 10, kSlabH);
        for (int i = 0; i < K; ++i) add(-52 + (i - (K - 1) * 0.5f) * 3.4f, 30, 1.2f, 4.5f, kSlabH + 24);
    }
    if (coreGrow_[2] > 0.3f) {
        float sizes[4] = {40, 30, 20, 11};
        add(52, 30, 23, 23, kSlabH);
        for (int i = 0; i < 4; ++i) add(52, 30, sizes[i] * 0.5f, sizes[i] * 0.5f, kSlabH + 6.0f * (i + 1));
        float ph = std::clamp(20.0f + float(s.vramTotal / 1073741824.0) * 6.0f, 24.0f, 110.0f);
        add(52, 30, 2, 2, kSlabH + 24.0f + ph);
    }
}

void City::gatherBoxes(const Vec3& p, float rad, std::vector<Box>& boxes) const {
    monumentBoxes(boxes);
    std::vector<TPart> tparts;
    std::vector<StepRect> steps;
    stepRects(steps);
    for (const StepRect& sr : steps) {
        const float reach = kStepN * kStepTread;
        if (std::fabs(p.x - sr.cx) > sr.sx * 0.5f + reach + 4.0f || std::fabs(p.z - sr.cz) > sr.sz * 0.5f + reach + 4.0f) continue;
        for (int k = 0; k < kStepN; ++k) {
            const float hx = sr.sx * 0.5f + (kStepN - k) * kStepTread, hz = sr.sz * 0.5f + (kStepN - k) * kStepTread;
            boxes.push_back({sr.cx - hx, sr.cx + hx, kStepRise * (k + 1), sr.cz - hz, sr.cz + hz});
        }
    }
    std::vector<Station> sts;
    stations(sts);
    for (const Station& st : sts) {
        if (std::fabs(p.x - st.x) > 40.0f || std::fabs(p.z - st.z) > 40.0f) continue;
        const float deckTop = st.level - 0.3f, H = deckTop - kSlabH;
        const int n = std::max(2, (int)std::ceil(H / kStMaxRise));
        const float h = H / n;
        auto add = [&](float a0, float a1, float top) {
            const float nx = st.fz, nz = -st.fx;
            const float xa = st.x + st.fx * a0 + nx * kStHalfW, xb = st.x + st.fx * a1 - nx * kStHalfW;
            const float za = st.z + st.fz * a0 + nz * kStHalfW, zb = st.z + st.fz * a1 - nz * kStHalfW;
            boxes.push_back({std::min(xa, xb), std::max(xa, xb), top, std::min(za, zb), std::max(za, zb)});
        };
        add(-kStDeck, 0.4f, deckTop);
        for (int k = 0; k + 1 < n; ++k) add(0.4f + k * kStRun, 0.4f + (k + 1) * kStRun, deckTop - (k + 1) * h);
    }
    // Data lanes are walkable decks. A box here is solid down to the ground, so a lane only counts once the feet are on or above it;
    // that keeps the space under a lane open and lets you drop onto it from above or fly up through it from below.
    {
        std::vector<Bundle> bs;
        bundles(bs);
        for (const Bundle& b : bs) {
            if (b.grow < 0.9f) continue;
            const float top = b.pts[0].y + 0.09f, w = (b.lanes - 1) * b.gap + b.laneW, hw = w * 0.5f;
            if (p.y - 1.7f + 0.6f < top) continue;
            for (size_t k = 0; k + 1 < b.pts.size(); ++k) {
                const Vec3 a = b.pts[k], c = b.pts[k + 1];
                const float x0 = std::min(a.x, c.x) - hw, x1 = std::max(a.x, c.x) + hw, z0 = std::min(a.z, c.z) - hw, z1 = std::max(a.z, c.z) + hw;
                if (p.x < x0 - 4 || p.x > x1 + 4 || p.z < z0 - 4 || p.z > z1 + 4) continue;
                boxes.push_back({x0, x1, top, z0, z1});
            }
        }
    }
    for (const auto& pl : platforms_) {
        if (pl.grow < 0.3f) continue;
        float dx = p.x - pl.cx, dz = p.z - pl.cz;
        if (std::fabs(dx) > pl.side * 0.5f + 100.0f || std::fabs(dz) > pl.side * 0.5f + 100.0f) continue;
        boxes.push_back({pl.cx - pl.side * 0.5f, pl.cx + pl.side * 0.5f, kSlabH, pl.cz - pl.side * 0.5f, pl.cz + pl.side * 0.5f});
        const float base = kSlabH + kPlateH;
        for (const auto& b : pl.blocks) {
            if (std::fabs(p.x - b.cx) > b.side * 0.5f + 20.0f || std::fabs(p.z - b.cz) > b.side * 0.5f + 20.0f) continue;
            for (const auto& t : b.towers) {
                if (t.grow < 0.3f) continue;
                const float hw = std::max(1.5f, t.curW) * 0.5f, hd = std::max(1.5f, t.curD) * 0.5f;
                if (std::fabs(p.x - t.x) > hw + rad + 1 || std::fabs(p.z - t.z) > hd + rad + 1) continue;
                towerParts(t.style, hw * 2, hd * 2, t.curH * ease(t.grow), tparts);
                for (const TPart& tp : tparts)
                    boxes.push_back({t.x + tp.cx - tp.sx * 0.5f, t.x + tp.cx + tp.sx * 0.5f, base + tp.y1, t.z + tp.cz - tp.sz * 0.5f, t.z + tp.cz + tp.sz * 0.5f});
            }
        }
    }
}

// The surface under an eye at p: the highest box top the feet can step onto, or the ground.
float City::floorAt(const Vec3& p, float rad, float eyeH) const {
    std::vector<Box> boxes;
    gatherBoxes(p, rad, boxes);
    const float feet = p.y - eyeH;
    float best = 0.0f;
    for (const auto& b : boxes) {
        if (p.x < b.x0 - rad * 0.5f || p.x > b.x1 + rad * 0.5f || p.z < b.z0 - rad * 0.5f || p.z > b.z1 + rad * 0.5f) continue;
        if (b.y1 <= feet + 0.6f) best = std::max(best, b.y1);
    }
    if (!netBuilt_ && built_) buildNetwork();
    for (const auto& b : bridgeFloor_) {
        if (p.x < b.x0 - rad * 0.5f || p.x > b.x1 + rad * 0.5f || p.z < b.z0 - rad * 0.5f || p.z > b.z1 + rad * 0.5f) continue;
        if (b.y1 <= feet + 0.6f) best = std::max(best, b.y1);
    }
    for (const auto& pl : platforms_) {
        if (pl.grow < 0.3f || std::fabs(p.x - pl.cx) > pl.side * 0.5f || std::fabs(p.z - pl.cz) > pl.side * 0.5f) continue;
        for (const auto& b : pl.blocks) {
            const float inside = std::min(b.side * 0.5f - 1.0f - std::fabs(p.x - b.cx), b.side * 0.5f - 1.0f - std::fabs(p.z - b.cz));
            if (inside < 0.0f) continue;
            best = std::max(best, kSlabH + kPlateH * std::min(1.0f, inside / kPadSlope));
        }
    }
    return best;
}

void City::resolve(Vec3& p, float rad, float eyeH) const {
    std::vector<Box> boxes;
    gatherBoxes(p, rad, boxes);
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& b : boxes) {
            if (p.x < b.x0 - rad || p.x > b.x1 + rad || p.z < b.z0 - rad || p.z > b.z1 + rad) continue;
            if (p.y >= b.y1 + eyeH) continue;
            if (b.y1 <= p.y - eyeH + 0.6f) continue;  // a step the feet can climb: floorAt lifts the walker onto it instead of blocking
            float up = b.y1 + eyeH - p.y;
            float l = p.x - (b.x0 - rad), rr = (b.x1 + rad) - p.x, n = p.z - (b.z0 - rad), f = (b.z1 + rad) - p.z;
            float m = std::min({up, l, rr, n, f});
            if (m == up) p.y = b.y1 + eyeH;
            else if (m == l) p.x = b.x0 - rad;
            else if (m == rr) p.x = b.x1 + rad;
            else if (m == n) p.z = b.z0 - rad;
            else p.z = b.z1 + rad;
        }
}

// Data lanes as polylines at their gantry height (also drives the click-to-ride shortcut).
void City::routes(std::vector<std::vector<Vec3>>& out) const {
    out.clear();
    std::vector<Bundle> bs;
    bundles(bs);
    for (const Bundle& b : bs)
        if (b.grow > 0.9f) out.push_back(b.pts);
}
bool City::pickRoute(Vec3 o, Vec3 d, std::vector<Vec3>& path) const {
    if (std::fabs(d.y) < 1e-3f) return false;
    std::vector<std::vector<Vec3>> rs;
    routes(rs);
    for (const auto& r : rs) {
        for (size_t k = 0; k + 1 < r.size(); ++k) {
            const Vec3 a = r[k], b = r[k + 1];
            const float t = ((a.y + b.y) * 0.5f - o.y) / d.y;
            if (t < 0 || t > 700.0f) continue;
            const float hx = o.x + d.x * t, hz = o.z + d.z * t;
            const float ex = b.x - a.x, ez = b.z - a.z, l2 = ex * ex + ez * ez;
            if (l2 < 1e-3f) continue;
            const float u = clampf(((hx - a.x) * ex + (hz - a.z) * ez) / l2, 0.0f, 1.0f);
            const float px = a.x + ex * u - hx, pz = a.z + ez * u - hz;
            if (px * px + pz * pz > 3.0f * 3.0f) continue;
            // Total length and the click position along it decide which end to carry the rider to.
            float total = 0, along = 0;
            for (size_t m = 0; m + 1 < r.size(); ++m) {
                const float sl = std::sqrt((r[m + 1].x - r[m].x) * (r[m + 1].x - r[m].x) + (r[m + 1].z - r[m].z) * (r[m + 1].z - r[m].z));
                if (m < k) along += sl;
                if (m == k) along += sl * u;
                total += sl;
            }
            const Vec3 hit{a.x + ex * u, a.y + (b.y - a.y) * u, a.z + ez * u};
            path.clear();
            path.push_back(hit);
            if (along < total * 0.5f) {
                for (size_t m = k + 1; m < r.size(); ++m) path.push_back(r[m]);
            } else {
                for (size_t m = k + 1; m-- > 0;) path.push_back(r[m]);
            }
            return path.size() > 1;
        }
    }
    return false;
}

// Highest solid surface within r of (x, z): towers, monuments and slabs.
float City::clearHeight(float x, float z, float r) const {
    std::vector<Box> boxes;
    gatherBoxes({x, 0, z}, r, boxes);
    float top = 0;
    for (const Box& b : boxes)
        if (x >= b.x0 - r && x <= b.x1 + r && z >= b.z0 - r && z <= b.z1 + r) top = std::max(top, b.y1);
    return top;
}

// Fly-through waypoints (y is the wanted altitude): the monuments, then for every storage region a dive in, a low run along the
// clearest street between the buildings, and a climb out.
void City::tourPoints(std::vector<Vec3>& out) const {
    out.clear();
    out.push_back({0, 75, 150});
    out.push_back({-35, 55, 95});
    out.push_back({-85, 60, 20});
    out.push_back({-60, 70, -70});
    for (const auto& p : platforms_) {
        if (p.grow < 0.9f) continue;
        const float h = p.side * 0.5f;
        const bool alongX = std::fabs(p.cx) >= std::fabs(p.cz);
        const float dir = alongX ? (p.cx > 0 ? 1.0f : -1.0f) : (p.cz > 0 ? 1.0f : -1.0f);
        float bestOff = 0, bestCost = 1e30f;
        for (int k = -3; k <= 3; ++k) {
            const float off = k * p.side * 0.11f;
            float cost = 0;
            for (float s = -h * 0.8f; s <= h * 0.8f; s += 6.0f) {
                const float x = alongX ? p.cx + s : p.cx + off, z = alongX ? p.cz + off : p.cz + s;
                cost += clearHeight(x, z, 4.0f);
            }
            if (cost < bestCost) { bestCost = cost; bestOff = off; }
        }
        auto at = [&](float s, float y) {
            return alongX ? Vec3{p.cx + dir * s, y, p.cz + bestOff} : Vec3{p.cx + bestOff, y, p.cz + dir * s};
        };
        out.push_back(at(-h - 60.0f, 80.0f));
        out.push_back(at(-h * 0.8f, 6.0f));
        out.push_back(at(-h * 0.3f, 15.0f));
        out.push_back(at(h * 0.3f, 5.5f));
        out.push_back(at(h * 0.8f, 11.0f));
        out.push_back(at(h + 40.0f, 60.0f));
    }
    out.push_back({0, 90, 160});
}
bool City::pick(Vec3 o, Vec3 d, std::string& path, std::string& name) {
    hoverP_ = hoverB_ = hoverT_ = -1;
    float best = 600.0f;
    for (size_t i = 0; i < platforms_.size(); ++i) {
        const auto& pl = platforms_[i];
        if (pl.grow < 0.9f) continue;
        const float base = kSlabH + kPlateH;
        for (size_t bi = 0; bi < pl.blocks.size(); ++bi) {
            const auto& b = pl.blocks[bi];
            for (size_t ti = 0; ti < b.towers.size(); ++ti) {
                const auto& t = b.towers[ti];
                if (t.grow < 0.9f) continue;
                float hw = std::max(1.5f, t.curW) * 0.5f, hd = std::max(1.5f, t.curD) * 0.5f, top = base + t.curH;
                float lo[3] = {t.x - hw, base, t.z - hd}, hi[3] = {t.x + hw, top, t.z + hd};
                float oo[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
                float t0 = 0, t1 = best;
                bool ok = true;
                for (int k = 0; k < 3 && ok; ++k) {
                    if (std::fabs(dd[k]) < 1e-6f) { if (oo[k] < lo[k] || oo[k] > hi[k]) ok = false; continue; }
                    float a = (lo[k] - oo[k]) / dd[k], c = (hi[k] - oo[k]) / dd[k];
                    if (a > c) std::swap(a, c);
                    t0 = std::max(t0, a);
                    t1 = std::min(t1, c);
                    if (t0 > t1) ok = false;
                }
                if (ok && t0 < best) { best = t0; hoverP_ = (int)i; hoverB_ = (int)bi; hoverT_ = (int)ti; }
            }
        }
    }
    if (hoverP_ < 0) return false;
    const auto& pl = platforms_[hoverP_];
    const auto& b = pl.blocks[hoverB_];
    const auto& t = b.towers[hoverT_];
    namespace fs = std::filesystem;
    const bool loose = t.name == "(FILES)" || t.name == "(MORE)";
    const std::string blockPath = hoverB_ == 0 ? pl.root : (fs::u8path(pl.root) / fs::u8path(b.name)).u8string();
    path = loose ? blockPath : (fs::u8path(blockPath) / fs::u8path(t.name)).u8string();
    name = loose ? (hoverB_ == 0 ? pl.name : b.name) : t.name;
    return true;
}

}  // namespace sf
