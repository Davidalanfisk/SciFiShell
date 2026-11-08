#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace sf {
template <typename Box>
bool gameplayRayHit(const Box& box, const double* origin, const double* direction,
                    double maxDistance, double& distance) {
    double p[3] = {origin[0] - box.x, origin[1] - box.y, origin[2] - box.z};
    double d[3] = {direction[0], direction[1], direction[2]};
    const double length = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (!std::isfinite(length) || length < 0.000001) return false;
    for (double& value : d) value /= length;
    for (double* a : {p, d}) {
        double c = std::cos(box.rotateY), s = std::sin(box.rotateY), x = a[0], z = a[2];
        a[0] = c*x - s*z; a[2] = s*x + c*z;
        c = std::cos(box.rotateX); s = std::sin(box.rotateX);
        double y = a[1]; z = a[2];
        a[1] = c*y + s*z; a[2] = -s*y + c*z;
        c = std::cos(box.rotateZ); s = std::sin(box.rotateZ);
        x = a[0]; y = a[1];
        a[0] = c*x + s*y; a[1] = -s*x + c*y;
    }
    const double half[] = {box.width * 0.5, box.height * 0.5, box.depth * 0.5};
    double near = 0, far = maxDistance;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-12) {
            if (p[i] < -half[i] || p[i] > half[i]) return false;
            continue;
        }
        double a = (-half[i] - p[i]) / d[i], b = (half[i] - p[i]) / d[i];
        if (a > b) std::swap(a, b);
        near = std::max(near, a); far = std::min(far, b);
        if (near > far) return false;
    }
    distance = near;
    return true;
}
}  // namespace sf
