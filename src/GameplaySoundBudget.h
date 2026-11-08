#pragma once

#include <algorithm>
#include <cstddef>

namespace sf {
class GameplaySoundBudget {
public:
    void reset() { tokens_ = 4; }
    bool consume(double seconds, std::size_t count) {
        tokens_ = std::min(4.0, tokens_ + seconds * 4.0);
        if (count > static_cast<std::size_t>(tokens_)) return false;
        tokens_ -= count;
        return true;
    }

private:
    double tokens_ = 4;
};
}  // namespace sf
