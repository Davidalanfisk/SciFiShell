#pragma once

#include "GameplayControlLease.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sf {

enum class GameplayControlMotionResult { Applied, InvalidProposal, InvalidWorld };

template <typename Resolve>
GameplayControlMotionResult resolveGameplayControlMotion(
    GameplayControlLease& lease, std::uint64_t owner, bool available,
    const GameplayControlLease::Pose& current, const GameplayControlLease::Pose& proposed,
    double dt, double extent, Resolve resolve, GameplayControlLease::Pose& result) {
    using Pose = GameplayControlLease::Pose;
    const auto withinWorld = [extent](const Pose& p) {
        return std::hypot(p.x, p.z) <= extent && p.y >= 0.35 && p.y <= 450;
    };
    if (!std::isfinite(extent) || extent <= 0 || !withinWorld(proposed) ||
        !lease.accepts(owner, available, current, proposed, dt))
        return GameplayControlMotionResult::InvalidProposal;

    const double dx = proposed.x - current.x, dy = proposed.y - current.y, dz = proposed.z - current.z;
    const int steps = std::max(1, static_cast<int>(std::ceil(
        std::sqrt(dx*dx + dy*dy + dz*dz) / kGameplayControlStepLimit)));
    result = current;
    result.yaw = proposed.yaw;
    result.pitch = proposed.pitch;
    for (int step = 0; step < steps; ++step) {
        Pose candidate{result.x + dx / steps, result.y + dy / steps, result.z + dz / steps,
                       proposed.yaw, proposed.pitch};
        if (!resolve(candidate) || !std::isfinite(candidate.x) || !std::isfinite(candidate.y) ||
            !std::isfinite(candidate.z))
            return GameplayControlMotionResult::InvalidWorld;
        if (!withinWorld(candidate) || !lease.accepts(owner, true, current, candidate, dt)) break;
        result = candidate;
    }
    return GameplayControlMotionResult::Applied;
}

}  // namespace sf
