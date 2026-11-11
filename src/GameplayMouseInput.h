#pragma once

#include "GameplayLimits.h"
#include <algorithm>
#include <cmath>

namespace sf {

class GameplayMouseInput {
public:
    void reset() { ready_ = false; }

    template <typename Dispatch>
    bool update(bool permitted, bool available, double yaw, double pitch, double dt, Dispatch dispatch) {
        if (!permitted || !available || !std::isfinite(dt) || dt <= 0 ||
            dt > kGameplayControlDeltaTimeLimit) {
            reset();
            return true;
        }
        // Discard the frame that acquired control or restored relative mouse capture.
        if (!ready_) { ready_ = true; return true; }
        const double limit = kGameplayControlAngularSpeedLimit * dt;
        const double deltas[] = {yaw, pitch};
        const char* names[] = {"mouse_yaw", "mouse_pitch"};
        for (int i = 0; i < 2; ++i) {
            if (!std::isfinite(deltas[i]) || deltas[i] == 0) continue;
            if (!dispatch(names[i], std::clamp(deltas[i], -limit, limit))) return false;
        }
        return true;
    }

private:
    bool ready_ = false;
};

}  // namespace sf
