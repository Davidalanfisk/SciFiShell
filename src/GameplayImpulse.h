#pragma once

#include <algorithm>
#include <cmath>

namespace sf {
// A plugin owns its residual motion; revocation discards only that plugin's velocity.
class GameplayImpulse {
public:
    void reset() { x_ = y_ = z_ = 0; tokens_ = 1; }
    void clearMotion() { x_ = y_ = z_ = 0; }
    void advance(double dt) { tokens_ = std::min(1.0, tokens_ + dt * 0.5); }
    bool request(double x, double y, double z) {
        if (tokens_ < 1) return false;
        tokens_ -= 1;
        x_ = x; y_ = y; z_ = z;
        return true;
    }
    void step(double dt, double& x, double& y, double& z) {
        x += x_ * dt; y += y_ * dt; z += z_ * dt;
        const double decay = std::exp(-3 * dt);
        x_ *= decay; y_ *= decay; z_ *= decay;
    }
private:
    double x_ = 0, y_ = 0, z_ = 0, tokens_ = 1;
};
}  // namespace sf
