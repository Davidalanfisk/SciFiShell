#pragma once

#include <cmath>
#include <cstddef>

namespace sf {
// Match the native park renderer's nearest grid-cell calculation without allocating.
template <typename Towers>
int gameplayCellInGrid(double x, double z, double cx, double cz, double pitch, int grid,
                       const Towers& towers, double* result) noexcept {
    if (!result || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(cx) ||
        !std::isfinite(cz) || !std::isfinite(pitch) || pitch <= 0 || grid <= 0) return -1;
    const double half = (grid - 1) * 0.5;
    const double gx = (x - cx) / pitch + half, gz = (z - cz) / pitch + half;
    if (gx < -0.5 || gz < -0.5 || gx >= grid - 0.5 || gz >= grid - 0.5) return 0;
    const int ix = static_cast<int>(std::floor(gx + 0.5));
    const int iz = static_cast<int>(std::floor(gz + 0.5));
    if (towers.size() > 4096) return -1;
    bool occupied = false;
    for (const auto& t : towers) {
        const double tx = (t.x - cx) / pitch + half, tz = (t.z - cz) / pitch + half;
        if (!std::isfinite(tx) || !std::isfinite(tz)) return -1;
        if (tx < -1 || tz < -1 || tx > grid || tz > grid) continue;
        if (std::lround(tx) == ix && std::lround(tz) == iz) { occupied = true; break; }
    }
    result[0] = cx + (ix - half) * pitch;
    result[2] = cz + (iz - half) * pitch;
    result[3] = pitch;
    result[4] = occupied ? 1 : 0;
    return 1;
}
}  // namespace sf
