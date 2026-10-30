#include "City.h"

#include <algorithm>
#include <queue>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "TextRender.h"
#include "Theme.h"

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
    if (i == 0) return currentTheme().accent * 0.8f;
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
    return c;
}
std::shared_ptr<GpuLabel> City::makeText(const std::string& s) {
    return renderer_.createLabel(makeLabel(s, 54.0f, FontFace::UI, Color8{}, 96, 4));
}

void City::layoutPlatforms(const std::vector<CityDrive>& snap) {
    platforms_.clear();
    const int n = (int)snap.size();
    constexpr float kPitch = 24.0f, kMargin = 10.0f, kGap = 14.0f;
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
                t.tgtH = clampf(3.0f + 12.0f * std::log10(1.0f + float(t.files)), 3.0f, 95.0f);
                float share = p.total ? clampf(float(t.bytes) / (0.05f * float(p.total)), 0.0f, 1.0f) : 0.3f;
                t.tgtW = p.pitch * (0.56f + 0.26f * std::sqrt(share));
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
    // Floating white text only: no frame, the texture alpha shapes it.
    const float white[3] = {1, 1, 1}, uvAll[4] = {0, 0, 1, 1};
    r.drawWebRegion(l, Mat4::translate(at) * Mat4::rotateY(yaw) * Mat4::scale({w, h, 1.0f}), uvAll, l.aspect, 0.0f, -1.0f, 1.0f, white);
}

std::shared_ptr<GpuLabel> City::makeDoorLabel(const std::string& name) {
    Bitmap bm;
    bm.w = 400;
    bm.h = 560;
    bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
    drawDoorOutline(bm);
    const auto lines = wrapText(name, 38.0f, FontFace::UI, 300.0f, 3);
    float y = bm.h * 0.5f - lines.size() * textLineHeight(38.0f, FontFace::UI) * 0.5f;
    for (const std::string& l : lines) {
        drawText(bm, (bm.w - measureText(l, 38.0f, FontFace::UI)) * 0.5f, y + textAscent(38.0f, FontFace::UI), l, 38.0f, FontFace::UI, Color8{});
        y += textLineHeight(38.0f, FontFace::UI);
    }
    return renderer_.createLabel(bm);
}

void City::markEntered() {
    hasEntered_ = false;
    if (hoverP_ < 0 || hoverB_ < 0 || hoverT_ < 0) return;
    const auto& t = platforms_[hoverP_].blocks[hoverB_].towers[hoverT_];
    enteredDoor_ = {t.x, kSlabH + kPlateH, t.z + std::max(1.5f, t.curW) * 0.5f};
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
                    pos = {t.x, kSlabH + kPlateH + 1.7f, t.z + std::max(1.5f, t.curW) * 0.5f + 5.0f};
                    return true;
                }
    return false;
}

void City::renderMonuments(Renderer& r, Vec3 eye) {
    const Theme& th = currentTheme();
    const SystemStats& s = data_.stats();
    const bool mirror = r.mirror();

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
            float vgb = float(s.vramTotal / 1073741824.0);
            float ph = std::clamp(20.0f + vgb * 6.0f, 24.0f, 110.0f);
            renderBox(r, {c.x, kSlabH + 24.0f * g + ph * 0.5f * g, c.z}, {4, ph * g, 4}, gpuCol, 0.8f, 4, coreGrow_[2],
                      std::max(0.03f, vramUsed_), 0);
            char b[160];
            std::snprintf(b, sizeof b, "GPU  %s  %.0f GB VRAM  %s", upper(s.gpuName).c_str(), vgb, s.gpuLoad >= 0 ? pct(shownGpu_).c_str() : "--");
            labelFor(mon_[2], b, {c.x, kSlabH + 24.0f + ph + 8.0f, c.z}, 5.0f, gpuCol);
        }
    }
}

void City::render(Renderer& r, Vec3 eye) {
    labelBudget_ = 2;
    renderMonuments(r, eye);
    // While the file system is still being read, square scan pulses ripple out of the hub across the ground.
    bool scanning = !built_;
    for (const auto& d : snap_) scanning = scanning || !d.done;
    if (scanning && coreGrow_[0] > 0.3f) {
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
            renderBox(r, {0, (kSlabH + 0.6f) * 0.5f * g, 0}, {8, (kSlabH + 0.6f) * g, 8}, th.alt, 0.35f, 6, coreGrow_[0], 1.0f, 0);
            // Activity: data packets stream from the hub to the CPU / RAM / GPU, faster and denser the busier each is.
            if (!r.mirror() && g > 0.99f) {
                // Packets ride the real road polylines (same data as the roads and click-to-ride) so they follow every corner,
                // ramp and bridge, sitting on the road surface.
                auto packets = [&](const std::vector<Vec3>& pts, const Vec3& col, float speed, int P, float seed, bool rev) {
                    if (pts.size() < 2) return;
                    auto surf = [](float h) { return kRoadY + 0.075f + h + (h > 0.01f ? 0.075f : 0.0f); };
                    std::vector<float> cum{0.0f};
                    for (size_t k = 0; k + 1 < pts.size(); ++k)
                        cum.push_back(cum.back() + std::fabs(pts[k + 1].x - pts[k].x) + std::fabs(pts[k + 1].z - pts[k].z));
                    const float len = cum.back();
                    if (len < 1.0f) return;
                    for (int k = 0; k < P; ++k) {
                        const float ph0 = float(std::fmod(clock_ * speed + (k + seed) / float(P), 1.0));
                        const float ph = rev ? 1.0f - ph0 : ph0;  // the receive lane runs the route backwards
                        const float d = ph * len;
                        size_t s0 = 0;
                        while (s0 + 2 < cum.size() && cum[s0 + 1] < d) ++s0;
                        const float u = clampf((d - cum[s0]) / std::max(cum[s0 + 1] - cum[s0], 1e-3f), 0.0f, 1.0f);
                        const Vec3 a = pts[s0], b = pts[s0 + 1];
                        // Each bus has two lanes (send and receive); traffic keeps to its own side of the centre line.
                        const float sl = std::fabs(b.x - a.x) + std::fabs(b.z - a.z) + 1e-4f;
                        const float lane = rev ? -0.75f : 0.75f;
                        const float px = a.x + (b.x - a.x) * u + (b.z - a.z) / sl * lane, pz = a.z + (b.z - a.z) * u - (b.x - a.x) / sl * lane;
                        if (std::fabs(px) < 13.5f && std::fabs(pz) < 13.5f) continue;
                        const float ph2 = 0.5f + 0.5f * std::sin(ph * kPi);
                        const float sy = surf(a.y) + (surf(b.y) - surf(a.y)) * u;
                        renderBox(r, {px, sy + 0.26f, pz}, {1.1f, 0.5f, 1.1f}, col * (0.4f + 1.2f * ph2), 0.3f, 5, 1, 0, 0);
                    }
                };
                const float loads[3] = {shownCpu_, shownRam_, shownGpu_};
                const Vec3 cols[3] = {th.accent, th.alt, Vec3{0.12f, 0.85f, 0.42f}};
                {
                    std::vector<std::vector<Vec3>> rs;
                    routes(rs);
                    int idx = 0;
                    for (int m = 0; m < 3; ++m) {
                        if (coreGrow_[m] <= 0.9f) continue;
                        std::vector<Vec3> pts;
                        for (const Vec3& v : rs[idx++]) pts.push_back({v.x, v.y - kRoadY, v.z});
                        packets(pts, cols[m], 0.08f + 0.35f * loads[m], 4, 0.0f, false);
                        packets(pts, cols[m] * 0.8f, 0.06f + 0.2f * loads[m], 3, 0.5f, true);
                    }
                    for (const NetRoute& nr : net_) {
                        if (platforms_[nr.plat].grow < 0.9f) continue;
                        if (std::fabs(eye.x - nr.pts.back().x) > 400.0f && std::fabs(eye.z - nr.pts.back().z) > 400.0f) continue;
                        const Vec3 pc = tintColor(platforms_[nr.plat].tint);
                        packets(nr.pts, pc, 0.05f + 0.1f * (nr.plat % 3), 5, nr.plat * 0.37f, false);
                        packets(nr.pts, pc * 0.8f, 0.05f + 0.1f * ((nr.plat + 1) % 3), 4, nr.plat * 0.61f + 0.5f, true);
                    }
                }                const float beat = 0.5f + 0.5f * std::sin(float(clock_) * 3.0f);
                renderBox(r, {0, kSlabH + 0.8f + 0.5f * beat, 0}, {5.0f, 0.3f, 5.0f}, th.accent * (0.5f + beat), 0.3f, 5, 1, 0, 0);
            }
            const float y = kRoadY + 0.02f;
            int N = std::clamp<int>(s.cores, 1, 32);
            float plinth = std::max(14.0f, N * 4.2f / (2.0f * kPi)) * 2.0f + 10.0f;
            auto zRoad = [&](float x, float z0, float z1, float grow) {
                float len = std::fabs(z1 - z0) * ease(grow);
                if (len > 0.5f) renderBox(r, {x, y, z0 + (z1 > z0 ? 1 : -1) * len * 0.5f}, {3.0f, 0.15f, len}, th.accent, 0.3f, 5, grow, 0, 0);
            };
            auto xRoad = [&](float z, float x0, float x1, float grow) {
                float len = std::fabs(x1 - x0) * ease(grow);
                if (len > 0.5f) renderBox(r, {x0 + (x1 > x0 ? 1 : -1) * len * 0.5f, y, z}, {len, 0.15f, 3.0f}, th.accent, 0.3f, 5, grow, 0, 0);
            };
            // Corner pieces fill the notch where each spoke turns.
            if (coreGrow_[1] > 0.5f || coreGrow_[2] > 0.5f) renderBox(r, {0, y, 30}, {3.0f, 0.15f, 3.0f}, th.accent, 0.3f, 5, 1, 0, 0);
            zRoad(0, 0, -55 + plinth * 0.5f, coreGrow_[0]);
            double gb = s.ramTotal / 1073741824.0;
            int K = std::clamp<int>((int)(gb / 4.0 + 0.5), 2, 16);
            // Each spoke stops at the edge of the 3 m corner square so the bend is one solid 90 degree piece.
            zRoad(0, 0, 28.5f, coreGrow_[1]);
            xRoad(30, -1.5f, -52 + (K * 3.4f + 6.0f) * 0.5f, coreGrow_[1]);
            zRoad(0.0f, 0, 28.5f, coreGrow_[2]);
            xRoad(30, 1.5f, 52 - 23.0f, coreGrow_[2]);
        }
    }
    {
        std::vector<Ramp> ramps;
        rampList(ramps);
        for (const Ramp& rp : ramps) {
            if (r.mirror() || std::fabs(eye.x - rp.x) > 160.0f || std::fabs(eye.z - rp.z) > 160.0f) continue;
            const Vec3 col = rp.plat >= 0 ? tintColor(platforms_[rp.plat].tint) : currentTheme().accent;
            const float ex = rp.dir == 0 ? 1.0f : (rp.dir == 1 ? -1.0f : 0.0f), ez = rp.dir == 2 ? 1.0f : (rp.dir == 3 ? -1.0f : 0.0f);
            const float rl = kRampSteps * kRampStep;
            drawSlope(r, {rp.x + ex * rl, 0.0f, rp.z + ez * rl}, {rp.x, kSlabH, rp.z}, kRampW, 0.3f, col, 0.5f, 6, 1.0f);
        }
    }
    if (!built_) return;
    const bool mirror = r.mirror();
    const float maxD2 = 900.0f * 900.0f;
    int draws = 0;

    if (!netBuilt_ && built_) buildNetwork();
    const int n = (int)platforms_.size();
    for (const Piece& pc : pieces_) {
        const auto& p = platforms_[pc.plat];
        if (p.grow <= 0) continue;
        const float rg = ease(std::min(1.0f, p.grow * 1.5f));
        if (pc.f > rg + 0.001f) continue;
        const float ddx = std::max({pc.x0 - eye.x, 0.0f, eye.x - pc.x1}), ddz = std::max({pc.z0 - eye.z, 0.0f, eye.z - pc.z1});
        if (ddx * ddx + ddz * ddz > maxD2) continue;
        const Vec3 tc = tintColor(p.tint);
        const float sd = (pc.plat % 2) ? 0.9f : 0.1f;
        if (pc.slope) drawSlope(r, pc.a, pc.b, 3.0f, 0.3f, tc, sd, 5, p.grow);
        else renderBox(r, {(pc.x0 + pc.x1) * 0.5f, pc.yc, (pc.z0 + pc.z1) * 0.5f}, {pc.x1 - pc.x0, pc.th, pc.z1 - pc.z0}, tc, sd, 5, p.grow, 0, 0);
    }
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
            if (bd2 > (mirror ? 450.0f * 450.0f : maxD2)) continue;
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
            for (size_t ti = 0; ti < b.towers.size(); ++ti) {
                auto& t = b.towers[ti];
                if (t.grow <= 0 || draws > 3000) continue;
                float h = t.curH * ease(t.grow);
                float w = std::max(1.5f, t.curW);
                float tdx = eye.x - t.x, tdz = eye.z - t.z;
                if (tdx * tdx + tdz * tdz > (mirror ? 350.0f * 350.0f : maxD2)) continue;
                float seed = rnd01(uint32_t(std::hash<std::string>{}(t.name) & 0xFFFFFF));
                const bool hovered = !mirror && hoverP_ == i && hoverB_ == (int)bi && hoverT_ == (int)ti;
                if (!mirror && tdx * tdx + tdz * tdz < 180.0f * 180.0f)  // raised sidewalk around the building
                    renderBox(r, {t.x, base + 0.07f, t.z}, {w + 4.0f, 0.14f, w + 4.0f}, tint, 0.5f, 6, t.grow, 1.0f, 0);
                renderBox(r, {t.x, base + h * 0.5f, t.z}, {w, h, w}, tint, seed, 3, t.grow, 0, hovered ? 0.6f : t.flash);
                ++draws;
                if (!mirror && h > 3.4f && tdx * tdx + tdz * tdz < 70.0f * 70.0f && t.grow > 0.95f) {  // street door on the +z face
                    if (!t.doorLabel && labelBudget_ > 0) {
                        t.doorLabel = makeDoorLabel(t.name == "(FILES)" || t.name == "(MORE)" ? b.name + " " + t.name : t.name);
                        --labelBudget_;
                    }
                    if (t.doorLabel) {
                        const float dh = std::min(3.2f, h * 0.8f), dw = std::min(dh * 0.714f, w * 0.85f);
                        r.drawCube(*t.doorLabel, Mat4::translate({t.x, base + dh * 0.5f, t.z + w * 0.5f + 0.03f}) * Mat4::scale({dw, dh, 0.06f}),
                                   {0.02f, 0.035f, 0.06f}, false, false, -1.0f);
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
        // Drive label above the platform.
        {
            char b[160];
            if (p.done)
                std::snprintf(b, sizeof b, "%s  %s / %s  %s FILES", p.name.c_str(), fmtBytes(p.bytes).c_str(),
                              fmtBytes(p.total).c_str(), fmtCount(p.files).c_str());
            else
                std::snprintf(b, sizeof b, "%s  SCANNING  %s FILES  %s", p.name.c_str(), fmtCount(p.files).c_str(),
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
}


void City::rampList(std::vector<Ramp>& out) const {
    const SystemStats& s = data_.stats();
    const int n = (int)platforms_.size();
    for (int i = 0; i < n; ++i) {
        const auto& p = platforms_[i];
        if (p.grow < 0.3f) continue;
        if (!netBuilt_ && built_) buildNetwork();
        for (const NetRoute& nr : net_) {
            if (nr.plat != i || nr.pts.size() < 2) continue;
            const Vec3 e = nr.pts.back();
            Vec3 q = nr.pts[nr.pts.size() - 2];
            for (size_t k = nr.pts.size() - 1; k-- > 0;) {
                q = nr.pts[k];
                if (std::fabs(q.x - e.x) + std::fabs(q.z - e.z) > 0.01f) break;
            }
            const float ex = e.x - q.x, ez = e.z - q.z;
            const int dir = std::fabs(ex) > std::fabs(ez) ? (ex > 0 ? 1 : 0) : (ez > 0 ? 3 : 2);
            out.push_back({e.x, e.z, dir, i});
        }
    }
    if (coreGrow_[0] > 0.3f) {
        out.push_back({0, -13.0f, 3, -1});
        out.push_back({0, 13.0f, 2, -1});
        const float ringR = std::max(14.0f, std::clamp<int>(s.cores, 1, 32) * 4.2f / (2.0f * kPi));
        const float edge = -55.0f + (ringR * 2.0f + 10.0f) * 0.5f;
        if (edge < -22.0f) out.push_back({0, edge, 2, -1});
    }
    if (coreGrow_[1] > 0.3f) {
        const int K = std::clamp<int>((int)(s.ramTotal / 1073741824.0 / 4.0 + 0.5), 2, 16);
        out.push_back({-52.0f + (K * 3.4f + 6.0f) * 0.5f, 30.0f, 0, -1});
    }
    if (coreGrow_[2] > 0.3f) out.push_back({29.0f, 30.0f, 1, -1});
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
    const Poly coreRoutes[3] = {{{0, 0, 0}, {0, 0, -55 + plinth * 0.5f}}, {{0, 0, 0}, {0, 0, 30}, {rx, 0, 30}}, {{0, 0, 0}, {0, 0, 30}, {29, 0, 30}}};
    const Rect solids[3] = {{-plinth * 0.5f, plinth * 0.5f, -55 - plinth * 0.5f, -55 + plinth * 0.5f},
                            {-52 - (K * 3.4f + 6.0f) * 0.5f, rx, 20, 40}, {29, 75, 7, 53}};
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
    auto planRoute = [&](int own, Poly& out) {
        const auto& pl = platforms_[own];
        const float h = pl.side * 0.5f;
        std::vector<float> dist(size_t(W) * D * 4, 1e30f);
        std::vector<int> prev(dist.size(), -1);
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        auto blocked = [&](int c) { return solid[c] || raw[c] >= 0 || inf[c] == -3 || (inf[c] >= 0 && inf[c] != own); };
        auto cellAt = [&](float x, float z) { return (int)std::lround(z / cs - gz0) * W + (int)std::lround(x / cs - gx0); };
        const int m = (int)std::floor(12.0f / cs);
        for (int k = -m; k <= m; ++k)
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
        out.push_back(d0 < 2 ? Vec3{DX[d0] * 13.0f, 0, cz(c0)} : Vec3{cx(c0), 0, DZ[d0] * 13.0f});
        for (size_t k = 1; k < seq.size(); ++k)
            if ((seq[k] & 3) != (seq[k - 1] & 3)) out.push_back({cx(seq[k] >> 2), 0, cz(seq[k] >> 2)});
        const int cg = goal >> 2, dg = goal & 3;
        if (dg == 0) out.push_back({pl.cx - h, 0, cz(cg)});
        else if (dg == 1) out.push_back({pl.cx + h, 0, cz(cg)});
        else if (dg == 2) out.push_back({cx(cg), 0, pl.cz - h});
        else out.push_back({cx(cg), 0, pl.cz + h});
        return out.size() >= 2;
    };

    for (int i = 0; i < n; ++i) {
        const auto& p = platforms_[i];
        Poly bp;
        if (!planRoute(i, bp)) {
            const float lane = (i - (n - 1) * 0.5f) * 3.5f, sz = p.cz >= 0 ? 1.0f : -1.0f;
            bp = {{0, 0, lane}, {p.cx, 0, lane}, {p.cx, 0, p.cz - sz * p.side * 0.5f}};
        }
        markPoly(bp);
        chosen[i] = bp;
    }
    // Bridges: wherever a route crosses a core spoke or an earlier route it climbs over it.
    const float H = 4.0f, deckHalf = 3.5f, rampLen = 9.0f;
    for (int i = 0; i < n; ++i) {
        const Poly& src = chosen[i];
        if (src.size() < 2) continue;
        NetRoute nr{i, {}};
        nr.pts.push_back({src[0].x, 0, src[0].z});
        for (size_t k = 0; k + 1 < src.size(); ++k) {
            const Seg sg = segOf(src, k);
            const float L = segLen(sg);
            const float dx = (sg.bx - sg.ax) / std::max(L, 1e-3f), dz = (sg.bz - sg.az) / std::max(L, 1e-3f);
            std::vector<float> hits;
            others(i, [&](const Seg& o) {
                float cx, cz;
                if (cross(sg, o, cx, cz)) hits.push_back(horiz(sg) ? std::fabs(cx - sg.ax) : std::fabs(cz - sg.az));
            });
            std::sort(hits.begin(), hits.end());
            struct Iv { float rs, ds, de, re; };
            std::vector<Iv> ivs;
            for (float t : hits) {
                Iv v{std::max(0.0f, t - deckHalf - rampLen), t - deckHalf, t + deckHalf, std::min(L, t + deckHalf + rampLen)};
                v.ds = std::max(v.ds, v.rs + 1.0f);
                v.de = std::min(v.de, v.re - 1.0f);
                if (!ivs.empty() && v.rs <= ivs.back().re + 1.0f) { ivs.back().de = std::max(ivs.back().de, v.de); ivs.back().re = std::max(ivs.back().re, v.re); }
                else ivs.push_back(v);
            }
            auto at = [&](float t, float hgt) { nr.pts.push_back({sg.ax + dx * t, hgt, sg.az + dz * t}); };
            for (const Iv& v : ivs) { at(v.rs, 0); at(v.ds, H); at(v.de, H); at(v.re, 0); }
            nr.pts.push_back({sg.bx, 0, sg.bz});
        }
        // Remove duplicate consecutive points.
        std::vector<Vec3> clean;
        for (const Vec3& v : nr.pts)
            if (clean.empty() || std::fabs(v.x - clean.back().x) + std::fabs(v.z - clean.back().z) + std::fabs(v.y - clean.back().y) > 0.01f) clean.push_back(v);
        nr.pts = clean;
        net_.push_back(std::move(nr));
    }

    // Turn each route into road pieces: flat boxes, straight sloped slabs and corner squares.
    auto surf = [](float h) { return kRoadY + 0.075f + h + (h > 0.01f ? 0.075f : 0.0f); };
    for (size_t ri = 0; ri < net_.size(); ++ri) {
        const auto& pts = net_[ri].pts;
        float total = 0, run = 0;
        for (size_t k = 0; k + 1 < pts.size(); ++k) total += std::fabs(pts[k + 1].x - pts[k].x) + std::fabs(pts[k + 1].z - pts[k].z);
        total = std::max(total, 1.0f);
        auto rect = [&](float ax, float az, float bx, float bz, float yc, float th, float f, bool draw) {
            const float px = std::fabs(ax - bx) < 1e-3f ? 1.5f : 0.0f, pz = std::fabs(az - bz) < 1e-3f ? 1.5f : 0.0f;
            Piece pc{std::min(ax, bx) - px, std::max(ax, bx) + px, std::min(az, bz) - pz, std::max(az, bz) + pz, yc, th, f, net_[ri].plat};
            if (draw) pieces_.push_back(pc);
            if (yc > kRoadY + 0.1f) bridgeFloor_.push_back({pc.x0, pc.x1, yc + th * 0.5f, pc.z0, pc.z1});
        };
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const Vec3 a = pts[k], b = pts[k + 1];
            const float L = std::fabs(b.x - a.x) + std::fabs(b.z - a.z);
            if (std::fabs(a.y - b.y) < 1e-3f) {
                if (a.y < 0.01f) {
                    // Pull each end back by half a road width where the lane turns; the corner square fills the bend.
                    const float ux = (b.x - a.x) / std::max(L, 1e-3f), uz = (b.z - a.z) / std::max(L, 1e-3f);
                    auto turnAt = [&](const Vec3& p, const Vec3& q, const Vec3& s) {
                        return (std::fabs(q.x - p.x) > 1e-3f) != (std::fabs(s.x - q.x) > 1e-3f);
                    };
                    const float t0 = (k > 0 && pts[k - 1].y < 0.01f && turnAt(pts[k - 1], a, b)) ? 1.5f : 0.0f;
                    const float t1 = (k + 2 < pts.size() && pts[k + 2].y < 0.01f && turnAt(a, b, pts[k + 2])) ? 1.5f : 0.0f;
                    if (L - t0 - t1 > 0.05f)
                        rect(a.x + ux * t0, a.z + uz * t0, b.x - ux * t1, b.z - uz * t1, kRoadY, 0.15f, (run + L) / total, true);
                }
                else rect(a.x, a.z, b.x, b.z, surf(a.y) - 0.15f, 0.3f, (run + L) / total, true);
            } else {
                Piece pc{std::min(a.x, b.x) - 1.5f, std::max(a.x, b.x) + 1.5f, std::min(a.z, b.z) - 1.5f, std::max(a.z, b.z) + 1.5f, 0, 0.3f, (run + L) / total, net_[ri].plat};
                pc.slope = true;
                pc.a = {a.x, surf(a.y), a.z};
                pc.b = {b.x, surf(b.y), b.z};
                pieces_.push_back(pc);
                const int steps = std::max(1, (int)std::ceil(L * 2.0f));
                for (int m = 0; m < steps; ++m) {
                    const float u0 = float(m) / steps, u1 = float(m + 1) / steps;
                    const float top = surf(std::max(a.y + (b.y - a.y) * u0, a.y + (b.y - a.y) * u1));
                    rect(a.x + (b.x - a.x) * u0, a.z + (b.z - a.z) * u0, a.x + (b.x - a.x) * u1, a.z + (b.z - a.z) * u1, top - 0.15f, 0.3f, 0, false);
                }
            }
            run += L;
            if (k + 2 < pts.size() && b.y < 0.01f) {
                const Vec3 c = pts[k + 2];
                const bool turn = (std::fabs(b.x - a.x) > 1e-3f) != (std::fabs(c.x - b.x) > 1e-3f);
                if (turn) pieces_.push_back({b.x - 1.5f, b.x + 1.5f, b.z - 1.5f, b.z + 1.5f, kRoadY, 0.15f, run / total, net_[ri].plat});
            }
        }
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
        for (int i = 0; i < N; ++i) {
            float a = 2.0f * kPi * i / N;
            add(ringR * std::cos(a), -55 + ringR * std::sin(a), 1.3f, 1.3f, kSlabH + 30);
        }
        add(0, 0, 13, 13, kSlabH);
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
    std::vector<Ramp> ramps;
    rampList(ramps);
    for (const Ramp& rp : ramps) {
        if (std::fabs(p.x - rp.x) > 12.0f || std::fabs(p.z - rp.z) > 12.0f) continue;
        for (int k = 0; k < kRampSteps; ++k) {
            const RampStep st = rampStep(rp.x, rp.z, rp.dir, k);
            boxes.push_back({st.cx - st.sx * 0.5f, st.cx + st.sx * 0.5f, st.top, st.cz - st.sz * 0.5f, st.cz + st.sz * 0.5f});
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
                float hw = std::max(1.5f, t.curW) * 0.5f;
                if (std::fabs(p.x - t.x) > hw + rad + 1 || std::fabs(p.z - t.z) > hw + rad + 1) continue;
                boxes.push_back({t.x - hw, t.x + hw, base + t.curH * ease(t.grow), t.z - hw, t.z + hw});
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

// Bus routes as polylines from the hub / core out to the destination (also drives the click-to-ride shortcut).
void City::routes(std::vector<std::vector<Vec3>>& out) const {
    out.clear();
    const SystemStats& s = data_.stats();
    const int N = std::clamp<int>(s.cores, 1, 32);
    const float plinth = std::max(14.0f, N * 4.2f / (2.0f * kPi)) * 2.0f + 10.0f;
    const float y = kRoadY;
    const double gb = s.ramTotal / 1073741824.0;
    const int K = std::clamp<int>((int)(gb / 4.0 + 0.5), 2, 16);
    if (coreGrow_[0] > 0.9f) out.push_back({{0, y, 0}, {0, y, -55 + plinth * 0.5f}});
    if (coreGrow_[1] > 0.9f) out.push_back({{0, y, 0}, {0, y, 30}, {-52 + (K * 3.4f + 6.0f) * 0.5f, y, 30}});
    if (coreGrow_[2] > 0.9f) out.push_back({{0, y, 0}, {0, y, 30}, {52 - 23.0f, y, 30}});
    if (!netBuilt_ && built_) buildNetwork();
    for (const NetRoute& nr : net_) {
        if (platforms_[nr.plat].grow < 0.9f) continue;
        std::vector<Vec3> pts;
        for (const Vec3& v : nr.pts) pts.push_back({v.x, y + v.y, v.z});
        out.push_back(std::move(pts));
    }
}

bool City::pickRoute(Vec3 o, Vec3 d, std::vector<Vec3>& path) const {
    if (d.y > -1e-3f) return false;
    std::vector<std::vector<Vec3>> rs;
    routes(rs);
    for (const auto& r : rs) {
        for (size_t k = 0; k + 1 < r.size(); ++k) {
            const Vec3 a = r[k], b = r[k + 1];
            const float t = ((a.y + b.y) * 0.5f + 0.1f - o.y) / d.y;
            if (t < 0 || t > 700.0f) continue;
            const float hx = o.x + d.x * t, hz = o.z + d.z * t;
            const float ex = b.x - a.x, ez = b.z - a.z, l2 = ex * ex + ez * ez;
            if (l2 < 1e-3f) continue;
            const float u = clampf(((hx - a.x) * ex + (hz - a.z) * ez) / l2, 0.0f, 1.0f);
            const float px = a.x + ex * u - hx, pz = a.z + ez * u - hz;
            if (px * px + pz * pz > 2.2f * 2.2f) continue;
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
    out.push_back({0, 70, 130});
    out.push_back({0, 30, 40});
    out.push_back({40, 50, -30});
    out.push_back({0, 25, -105});
    out.push_back({-45, 55, -40});
    out.push_back({-52, 22, 62});
    out.push_back({10, 40, 70});
    out.push_back({52, 20, 75});
    out.push_back({50, 85, 20});
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
                float hw = std::max(1.5f, t.curW) * 0.5f, top = base + t.curH;
                float lo[3] = {t.x - hw, base, t.z - hw}, hi[3] = {t.x + hw, top, t.z + hw};
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
