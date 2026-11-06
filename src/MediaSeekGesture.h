#pragma once

#include <algorithm>

namespace sf {

class MediaSeekGesture {
public:
    bool update(bool eligible, bool leftPress, bool rightPress, bool leftHeld, bool rightHeld,
                bool leftRelease, bool rightRelease, double time, double position, double duration, double& target) {
        if (!eligible || duration <= 0 || (leftHeld && rightHeld)) {
            reset();
            return false;
        }
        if (direction_ == 0) {
            if (leftPress == rightPress) return false;
            direction_ = leftPress ? -1 : 1;
            pressedAt_ = time;
        }
        const bool released = direction_ < 0 ? leftRelease : rightRelease;
        const bool held = direction_ < 0 ? leftHeld : rightHeld;
        if (!released && !held) {
            reset();
            return false;
        }
        if (!scrubbing_ && time - pressedAt_ >= 0.35) {
            scrubbing_ = true;
            startedAt_ = time;
            lastSeekAt_ = time;
            startPosition_ = position;
        }
        if (released) {
            target = std::clamp(scrubbing_ ? startPosition_ + direction_ * 10.0 * (time - startedAt_)
                                           : position + direction_ * 10.0, 0.0, duration);
            reset();
            return true;
        }
        if (scrubbing_ && time - lastSeekAt_ >= 0.1) {
            lastSeekAt_ = time;
            target = std::clamp(startPosition_ + direction_ * 10.0 * (time - startedAt_), 0.0, duration);
            return true;
        }
        return false;
    }

    void reset() { direction_ = 0; scrubbing_ = false; }

private:
    int direction_ = 0;
    bool scrubbing_ = false;
    double pressedAt_ = 0, startedAt_ = 0, lastSeekAt_ = 0, startPosition_ = 0;
};

}  // namespace sf
