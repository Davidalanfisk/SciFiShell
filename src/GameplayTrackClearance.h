#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sf {
// Mirrors the obstacle footprints used by the native track generator, not city collision.
template <typename Platforms, typename Monuments>
bool gameplayTrackClearance(const Platforms& platforms, const Monuments& monuments,
                            double x, double z, double radius, double& result) noexcept {
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(radius) ||
        std::abs(x) > 10000 || std::abs(z) > 10000 || radius < 0 || radius > 100) return false;
    std::size_t visited = 0;
    double top = 0;
    auto add = [&](double x0, double x1, double z0, double z1, double height) {
        if (++visited > 4096 || !std::isfinite(x0) || !std::isfinite(x1) ||
            !std::isfinite(z0) || !std::isfinite(z1) || !std::isfinite(height) ||
            x0 > x1 || z0 > z1 || height < 0) return false;
        if (x + radius >= x0 && x - radius <= x1 && z + radius >= z0 && z - radius <= z1)
            top = std::max(top, height);
        return true;
    };
    for (const auto& b : monuments)
        if (!add(b.x0, b.x1, b.z0, b.z1, b.y1)) return false;
    for (const auto& p : platforms) {
        if (++visited > 4096) return false;
        if (!std::isfinite(p.grow)) return false;
        if (p.gone && p.grow <= 0) continue;
        if (!std::isfinite(p.cx) || !std::isfinite(p.cz) || !std::isfinite(p.side) || p.side < 0 ||
            !add(p.cx - p.side * 0.5, p.cx + p.side * 0.5,
                 p.cz - p.side * 0.5, p.cz + p.side * 0.5, 1.3)) return false;
        for (const auto& b : p.blocks) {
            if (++visited > 4096) return false;
            for (const auto& t : b.towers) {
                if (!std::isfinite(t.x) || !std::isfinite(t.z) ||
                    !std::isfinite(t.curW) || !std::isfinite(t.tgtW) ||
                    !std::isfinite(t.curD) || !std::isfinite(t.tgtD) ||
                    !std::isfinite(t.curH) || !std::isfinite(t.tgtH)) return false;
                const double w = std::max(t.curW, t.tgtW) * 0.5 + 0.5;
                const double d = std::max(t.curD, t.tgtD) * 0.5 + 0.5;
                if (!add(t.x - w, t.x + w, t.z - d, t.z + d,
                         3.3 + std::max(t.curH, t.tgtH))) return false;
            }
        }
    }
    result = top;
    return true;
}
}  // namespace sf
