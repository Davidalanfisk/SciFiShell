#pragma once

#include <cstdint>

namespace sf {
class GameplayClickRouter {
public:
    void begin(bool available, bool down, std::uint64_t candidate) {
        if (!available) { held_ = claimed_ = false; owner_ = 0; return; }
        if (down && !held_) {
            held_ = true;
            owner_ = candidate;
            claimed_ = candidate != 0;
        }
    }
    bool consume(bool down) const { return down && held_ && claimed_; }
    std::uint64_t owner() const { return owner_; }
    void forget(std::uint64_t owner) { if (owner_ == owner) owner_ = 0; }
    void finish(bool up) { if (up) { held_ = claimed_ = false; owner_ = 0; } }

private:
    std::uint64_t owner_ = 0;
    bool held_ = false, claimed_ = false;
};
}  // namespace sf
