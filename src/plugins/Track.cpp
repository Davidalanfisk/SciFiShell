// The race track: a glass deck on a grid that spirals up from the hub's CPU region, tours over the city with a couple of loops,
// and spirals back down. It is laid out once per city layout, riding above every building, and only drawn while driving.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include "City.h"
#include "Renderer.h"

namespace sf {

namespace {
constexpr float kSlabH = 1.0f, kPlateH = 0.3f;

struct Obst { float x0, x1, z0, z1, top; };

// Coarse spatial grid over building footprints, answering "how tall is the tallest thing near here".
struct Grid {
    static constexpr float C = 32.0f;
    float x0 = 0, z0 = 0;
    int nx = 0, nz = 0;
    std::vector<Obst> obs;
    std::vector<std::vector<int>> cells;
    void build(std::vector<Obst> o) {
        obs = std::move(o);
        nx = nz = 0;
        if (obs.empty()) return;
        float xmin = 1e30f, zmin = 1e30f, xmax = -1e30f, zmax = -1e30f;
        for (const Obst& b : obs) {
            xmin = std::min(xmin, b.x0); xmax = std::max(xmax, b.x1);
            zmin = std::min(zmin, b.z0); zmax = std::max(zmax, b.z1);
        }
        x0 = xmin; z0 = zmin;
        nx = static_cast<int>((xmax - xmin) / C) + 1;
        nz = static_cast<int>((zmax - zmin) / C) + 1;
        cells.assign(static_cast<size_t>(nx) * nz, {});
        for (size_t i = 0; i < obs.size(); ++i) {
            const Obst& b = obs[i];
            const int a0 = static_cast<int>((b.x0 - x0) / C), a1 = static_cast<int>((b.x1 - x0) / C);
            const int c0 = static_cast<int>((b.z0 - z0) / C), c1 = static_cast<int>((b.z1 - z0) / C);
            for (int cz = c0; cz <= c1; ++cz)
                for (int cx = a0; cx <= a1; ++cx) cells[static_cast<size_t>(cz) * nx + cx].push_back(static_cast<int>(i));
        }
    }
    float maxTop(float x, float z, float r) const {
        if (!nx) return 0.0f;
        const int a0 = static_cast<int>(std::floor((x - r - x0) / C)), a1 = static_cast<int>(std::floor((x + r - x0) / C));
        const int c0 = static_cast<int>(std::floor((z - r - z0) / C)), c1 = static_cast<int>(std::floor((z + r - z0) / C));
        if (a1 < 0 || c1 < 0 || a0 >= nx || c0 >= nz) return 0.0f;
        float best = 0.0f;
        for (int cz = std::max(c0, 0); cz <= std::min(c1, nz - 1); ++cz)
            for (int cx = std::max(a0, 0); cx <= std::min(a1, nx - 1); ++cx)
                for (int idx : cells[static_cast<size_t>(cz) * nx + cx]) {
                    const Obst& b = obs[idx];
                    if (x + r < b.x0 || x - r > b.x1 || z + r < b.z0 || z - r > b.z1) continue;
                    best = std::max(best, b.top);
                }
        return best;
    }
};

Vec3 flatDir(Vec3 t) {
    const float l = std::hypot(t.x, t.z);
    return l > 1e-4f ? Vec3{t.x / l, 0, t.z / l} : Vec3{0, 0, -1};
}
}  // namespace

void City::ensureTrack() {
    if (!trackDirty_ && !track_.empty()) return;
    trackDirty_ = false;
    buildTrack();
}

bool City::trackSample(float s, TrackFrame& out) const {
    if (track_.size() < 2) return false;
    s = clampf(s, 0.0f, track_.back().s);
    size_t lo = 0, hi = track_.size() - 1;
    while (hi - lo > 1) {
        const size_t mid = (lo + hi) / 2;
        if (track_[mid].s <= s) lo = mid; else hi = mid;
    }
    const TrackFrame &a = track_[lo], &b = track_[hi];
    const float u = b.s > a.s ? clampf((s - a.s) / (b.s - a.s), 0.0f, 1.0f) : 0.0f;
    out.p = a.p + (b.p - a.p) * u;
    out.t = normalize(a.t + (b.t - a.t) * u);
    out.u = normalize(a.u + (b.u - a.u) * u);
    out.flat = normalize(a.flat + (b.flat - a.flat) * u);
    out.s = s;
    return true;
}

void City::buildTrack() {
    track_.clear();
    struts_.clear();
    loops_.clear();

    Grid grid;
    {
        std::vector<Obst> o;
        std::vector<Box> mb;
        monumentBoxes(mb);
        for (const Box& b : mb) o.push_back({b.x0, b.x1, b.z0, b.z1, b.y1});
        for (const Platform& pl : platforms_) {
            if (pl.gone && pl.grow <= 0.0f) continue;
            o.push_back({pl.cx - pl.side * 0.5f, pl.cx + pl.side * 0.5f, pl.cz - pl.side * 0.5f, pl.cz + pl.side * 0.5f, kSlabH + kPlateH});
            for (const Block& b : pl.blocks)
                for (const Tower& t : b.towers) {
                    const float w = std::max(t.curW, t.tgtW) * 0.5f + 0.5f, d = std::max(t.curD, t.tgtD) * 0.5f + 0.5f;
                    o.push_back({t.x - w, t.x + w, t.z - d, t.z + d, kSlabH + kPlateH + std::max(t.curH, t.tgtH) + 2.0f});
                }
        }
        grid.build(std::move(o));
    }

    constexpr float ds = 3.0f, hw = kTrackHalf;
    constexpr float rAsc = 104.0f, rDesc = 90.0f, a0 = -kPi * 0.5f;  // the ramps start just north of the hub, beside the CPU region
    const float rHi = clampf(extent_ * 0.7f, 260.0f, 800.0f);
    std::mt19937 rng(7u + static_cast<unsigned>(platforms_.size()) * 131u);
    auto frand = [&](float a, float b) { return a + (b - a) * static_cast<float>(rng() & 0xFFFFFF) / 16777215.0f; };

    struct Cand { std::vector<Vec3> pts; std::vector<float> h; std::vector<int> sites; float cost = 1e30f; };
    Cand best;

    auto polar = [](float th, float r) { return Vec3{r * std::cos(th), 0.0f, r * std::sin(th)}; };
    auto catmull = [&](const std::vector<Vec3>& C) {
        std::vector<Vec3> out;
        for (size_t i = 0; i + 1 < C.size(); ++i) {
            const Vec3 p1 = C[i], p2 = C[i + 1];
            const Vec3 p0 = i > 0 ? C[i - 1] : p1 + (p1 - p2);
            const Vec3 p3 = i + 2 < C.size() ? C[i + 2] : p2 + (p2 - p1);
            const float seg = length(p2 - p1);
            const int steps = std::max(4, static_cast<int>(seg / 1.5f));
            for (int k = 0; k < steps; ++k) {
                const float t = float(k) / steps, t2 = t * t, t3 = t2 * t;
                out.push_back((p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 + (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f);
            }
        }
        out.push_back(C.back());
        return out;
    };
    auto resample = [&](const std::vector<Vec3>& dense) {
        std::vector<Vec3> out{dense[0]};
        float carry = 0;
        for (size_t i = 1; i < dense.size(); ++i) {
            const Vec3 a = dense[i - 1], seg = dense[i] - a;
            const float l = length(seg);
            if (l < 1e-5f) continue;
            float pos = ds - carry;
            while (pos <= l) { out.push_back(a + seg * (pos / l)); pos += ds; }
            carry = l - (pos - ds);
        }
        return out;
    };

    const float thE = a0 + 2.0f * kPi, thR = a0 + 3.0f * kPi;  // exit from the climbing ring (north); entry to the descending ring (south)
    for (int cand = 0; cand < 80; ++cand) {
        std::vector<Vec3> C;
        for (int j = 0; j <= 24; ++j) C.push_back(polar(a0 + j * (2.0f * kPi / 24.0f), rAsc));
        C.push_back(polar(thE + 0.3f, 132.0f));
        const int N = 5 + static_cast<int>(rng() % 5);
        float r = frand(170.0f, rHi);
        for (int k = 0; k < N; ++k) {
            const float th = thE + 0.6f + (thR - thE - 1.1f) * (float(k) / (N - 1)) + frand(-0.06f, 0.06f);
            r = clampf(r + frand(-0.45f, 0.45f) * rHi, 165.0f, rHi);
            C.push_back(polar(th, r));
        }
        C.push_back(polar(thR - 0.5f, 140.0f));
        for (int j = 0; j <= 36; ++j) C.push_back(polar(thR + j * (2.0f * kPi / 24.0f), rDesc));

        Cand c;
        c.pts = resample(catmull(C));
        const int n = static_cast<int>(c.pts.size());
        if (n < 20) continue;
        const float L = (n - 1) * ds, hc = frand(60.0f, 95.0f);
        c.h.assign(n, 0.0f);
        std::vector<float> baseH(n), req(n);
        float lift = 0;
        // Loop sites: straight stretches of the tour, far from the hub; the deck is lifted clear of everything near them.
        {
            auto dir = [&](int a, int b) { return flatDir(c.pts[b] - c.pts[a]); };
            int last = -1000;
            for (int i = 60; i < n - 60 && c.sites.size() < 4; ++i) {
                if (i - last < 70 || std::hypot(c.pts[i].x, c.pts[i].z) < 140.0f) continue;
                if (dot(dir(i - 14, i - 2), dir(i + 2, i + 14)) < 0.97f) continue;
                c.sites.push_back(i);
                last = i;
            }
            if (c.sites.size() % 2) c.sites.pop_back();
        }
        for (int i = 0; i < n; ++i) {
            const float s = i * ds;
            baseH[i] = std::min(hc, std::min(std::max(0.0f, (s - 10.0f) * 0.13f), std::max(0.0f, (L - 10.0f - s) * 0.085f)));
            const float top = grid.maxTop(c.pts[i].x, c.pts[i].z, hw + 3.5f);
            req[i] = top > 0.5f ? top + 7.0f : 0.0f;
        }
        for (int site : c.sites) {
            const float need = grid.maxTop(c.pts[site].x, c.pts[site].z, 20.0f) + 5.0f;
            for (int j = site - 14; j <= site + 14; ++j) req[j] = std::max(req[j], need);
        }
        for (int i = 0; i < n; ++i) c.h[i] = std::max(baseH[i], req[i]);
        for (int i = 1; i < n; ++i) c.h[i] = std::max(c.h[i], c.h[i - 1] - 0.3f * ds);
        for (int i = n - 2; i >= 0; --i) c.h[i] = std::max(c.h[i], c.h[i + 1] - 0.3f * ds);
        for (int pass = 0; pass < 4; ++pass) {
            std::vector<float> sm = c.h;
            for (int i = 3; i < n - 3; ++i) {
                float a = 0;
                for (int k = -3; k <= 3; ++k) a += c.h[i + k];
                sm[i] = a / 7.0f;
            }
            c.h = sm;
        }
        float peak = 0;
        for (int i = 0; i < n; ++i) {
            c.h[i] = std::max(c.h[i], req[i]);
            lift += std::max(0.0f, c.h[i] - baseH[i]);
            peak = std::max(peak, c.h[i]);
        }
        float cost = lift * ds * 0.003f + peak * 0.8f + (peak > 220.0f ? 1000.0f : 0.0f);
        // Penalise a path that crosses itself at a similar height.
        int clash = 0;
        for (int i = 0; i < n; i += 2)
            for (int j = i + 25; j < n; j += 2) {
                const float dx = c.pts[i].x - c.pts[j].x, dz = c.pts[i].z - c.pts[j].z;
                if (dx * dx + dz * dz > 100.0f) continue;
                if (std::fabs(c.h[i] - c.h[j]) < 12.0f) ++clash;
            }
        cost += clash * 40.0f;
        c.cost = cost - 120.0f * static_cast<float>(c.sites.size());
        if (c.cost < best.cost) best = std::move(c);
    }
    if (best.pts.size() < 20) return;

    const int n = static_cast<int>(best.pts.size());
    std::vector<Vec3> P(n), T(n), F(n);
    for (int i = 0; i < n; ++i) P[i] = {best.pts[i].x, best.h[i], best.pts[i].z};
    for (int i = 0; i < n; ++i) {
        T[i] = normalize(P[std::min(i + 1, n - 1)] - P[std::max(i - 1, 0)]);
        F[i] = flatDir(T[i]);
    }

    // Pick the loops: straight, level stretches of the tour with clear air above the deck.
    std::vector<int> loopAt = best.sites;
    std::fprintf(stderr, "loop sites: %zu\n", loopAt.size());

    // Banking from the horizontal curvature.
    std::vector<float> bank(n, 0.0f);
    for (int i = 4; i < n - 4; ++i) {
        const Vec3 a = F[i - 4], b = F[i + 4];
        bank[i] = clampf((a.z * b.x - a.x * b.z) / (8.0f * ds) * 38.0f, -0.38f, 0.38f);
    }
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<float> sm = bank;
        for (int i = 2; i < n - 2; ++i) sm[i] = (bank[i - 2] + bank[i - 1] + bank[i] + bank[i + 1] + bank[i + 2]) * 0.2f;
        bank = sm;
    }

    constexpr float kLoopR = 14.0f, kLoopShift = 8.5f, lds = 2.0f;
    Vec3 shift{0, 0, 0};
    float sgn = 1.0f;
    size_t nextLoop = 0;
    std::vector<TrackFrame> fr;
    for (int i = 0; i < n; ++i) {
        const Vec3 p = P[i] + shift;
        const Vec3 rh = normalize(cross(T[i], Vec3{0, 1, 0}));
        const Vec3 u0 = cross(rh, T[i]);
        const float cb = std::cos(bank[i]), sb = std::sin(bank[i]);
        fr.push_back({p, T[i], normalize(u0 * cb - rh * sb), F[i], 0.0f});
        if (i % 5 == 2 && p.y > 2.0f && grid.maxTop(p.x, p.z, 1.6f) < 1.5f) struts_.push_back({p, p.y - 0.35f});
        if (nextLoop < loopAt.size() && i == loopAt[nextLoop]) {
            const Vec3 th = F[i], wu{0, 1, 0}, rgt = normalize(cross(th, wu)) * sgn;
            const float s0 = static_cast<float>(fr.size());
            const int steps = static_cast<int>(2.0f * kPi * kLoopR / lds);
            for (int k = 1; k <= steps; ++k) {
                const float phi = 2.0f * kPi * k / steps;
                const float w = kLoopShift * (phi - std::sin(phi)) / (2.0f * kPi), dw = kLoopShift * (1.0f - std::cos(phi)) / (2.0f * kPi);
                const Vec3 lp = p + th * (kLoopR * std::sin(phi)) + wu * (kLoopR * (1.0f - std::cos(phi))) + rgt * w;
                const Vec3 lt = normalize(th * std::cos(phi) + wu * std::sin(phi) + rgt * (dw / kLoopR));
                Vec3 lu = th * (-std::sin(phi)) + wu * std::cos(phi);
                lu = normalize(lu - lt * dot(lu, lt));
                fr.push_back({lp, lt, lu, th, 0.0f});
            }
            (void)s0;
            shift = shift + rgt * kLoopShift;
            sgn = -sgn;
            ++nextLoop;
        }
    }
    // Arc length, and the loop ranges for colouring.
    float s = 0;
    for (size_t i = 0; i < fr.size(); ++i) {
        if (i) s += length(fr[i].p - fr[i - 1].p);
        fr[i].s = s;
    }
    track_ = std::move(fr);
    // Loop ranges: contiguous runs of frames spaced at the loop step length.
    for (size_t i = 1; i + 1 < track_.size(); ++i) {
        const float d0 = track_[i].s - track_[i - 1].s;
        if (d0 < lds + 0.25f && d0 > lds - 0.25f && std::fabs(track_[i].p.y - track_[i - 1].p.y) > 0.0f) {
            const float a = track_[i - 1].s;
            size_t j = i;
            while (j + 1 < track_.size() && std::fabs((track_[j + 1].s - track_[j].s) - lds) < 0.25f) ++j;
            if (track_[j].s - a > 2.0f * kPi * kLoopR * 0.9f) loops_.push_back({a, track_[j].s});
            i = j;
        }
    }
    std::fprintf(stderr, "track: length %.0f, %zu loops, %zu struts", track_.back().s, loops_.size(), struts_.size());
    for (auto& l : loops_) std::fprintf(stderr, " [%.0f]", l.first);
    std::fprintf(stderr, "\n");
}

void City::renderTrack(Renderer& r, Vec3 eye) {
    if (trackFade_ <= 0.01f || track_.size() < 2 || dry_) return;
    const float L = track_.back().s;
    const Vec3 cyan{0.1f, 0.85f, 1.0f}, mag{1.0f, 0.3f, 0.9f};
    size_t li = 0;
    for (size_t i = 0; i + 1 < track_.size(); ++i) {
        const TrackFrame &f = track_[i], &g = track_[i + 1];
        while (li < loops_.size() && f.s > loops_[li].second) ++li;
        const Vec3 d = f.p - eye;
        if (dot(d, d) > 400.0f * 400.0f) continue;
        const float e = clampf(trackFade_ * 2.2f - (f.s / L) * 1.2f, 0.0f, 1.0f);
        if (e < 0.01f || !r.visible(f.p, 5.0f)) continue;
        const bool inLoop = li < loops_.size() && f.s >= loops_[li].first;
        const Vec3 seg = g.p - f.p;
        const float len = length(seg) + 0.2f;
        const Vec3 rt = cross(f.t, f.u);
        const Vec3 ax[3] = {rt * (kTrackHalf * 2.0f), f.u * 0.35f, f.t * len};
        const Vec3 c = f.p + seg * 0.5f - f.u * 0.175f;
        Mat4 m = Mat4::identity();
        for (int k = 0; k < 3; ++k) { m.m[k * 4] = ax[k].x; m.m[k * 4 + 1] = ax[k].y; m.m[k * 4 + 2] = ax[k].z; }
        m.m[12] = c.x; m.m[13] = c.y; m.m[14] = c.z;
        r.drawBuilding(*dummy_, m, inLoop ? mag : cyan, f.s, 10, e, 0.0f, 0.0f);
    }
    const float e = clampf(trackFade_ * 1.6f, 0.0f, 1.0f);
    for (const TrackStrut& st : struts_) {
        const Vec3 d = st.p - eye;
        if (dot(d, d) > 400.0f * 400.0f || !r.visible({st.p.x, st.top * 0.5f, st.p.z}, st.top * 0.5f + 1.0f)) continue;
        r.drawBuilding(*dummy_, Mat4::translate({st.p.x, st.top * 0.5f, st.p.z}) * Mat4::scale({0.7f, st.top, 0.7f}), cyan, 0.0f, 10, e, 1.0f, 0.0f);
    }
}

}  // namespace sf
