#pragma once

#include <cmath>
#include <cstdint>

namespace sf {

// Host-side arbitration only: the app must independently obtain the user's
// activation and resolve collision before applying any accepted pose.
class GameplayControlLease {
public:
    struct Pose { double x, y, z, yaw, pitch; };

    bool acquire(std::uint64_t owner, bool permission, bool available, bool userActivated,
                 const Pose& current, const Pose& anchor) {
        if (owner_ || !owner || !permission || !available || !userActivated ||
            !valid(current) || !valid(anchor)) return false;
        const double dx = current.x - anchor.x, dy = current.y - anchor.y, dz = current.z - anchor.z;
        if (dx * dx + dy * dy + dz * dz > 16.0) return false;
        owner_ = owner;
        return true;
    }

    bool accepts(std::uint64_t owner, bool available, const Pose& current,
                 const Pose& proposed, double dt) {
        if (!owner_ || owner != owner_) return false;
        if (!available) { release(); return false; }
        if (!valid(current) || !valid(proposed) || !std::isfinite(dt) || dt <= 0 || dt > 0.1)
            return false;
        const double dx = current.x - proposed.x, dy = current.y - proposed.y, dz = current.z - proposed.z;
        const double angle = std::remainder(proposed.yaw - current.yaw, 6.283185307179586);
        const double limit = 25.0 * dt;
        return dx * dx + dy * dy + dz * dz <= limit * limit &&
               std::abs(angle) <= 4.0 * dt &&
               std::abs(proposed.pitch - current.pitch) <= 4.0 * dt;
    }

    void release() { owner_ = 0; }
    void releaseOwner(std::uint64_t owner) { if (owner && owner == owner_) release(); }
    std::uint64_t owner() const { return owner_; }

private:
    static bool valid(const Pose& p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
               std::isfinite(p.yaw) && std::isfinite(p.pitch) &&
               std::abs(p.x) <= 10000 && std::abs(p.y) <= 10000 && std::abs(p.z) <= 10000 &&
               std::abs(p.yaw) <= 10000 && std::abs(p.pitch) <= 1.4;
    }
    std::uint64_t owner_ = 0;
};

}  // namespace sf
