#pragma once

namespace sf {
struct GameplayInputContext {
    bool fileRoom = false, windowFocused = false, worldPointer = false, typing = false;
    bool focusedWindow = false, capturing = false, driving = false, riding = false, vr = false;
    bool harvi = false, settings = false, start = false, viewer = false, switcher = false;
    bool apps = false, keyboard = false;

    bool available() const {
        return fileRoom && windowFocused && worldPointer && !typing && !focusedWindow && !capturing &&
               !driving && !riding && !vr && !harvi && !settings && !start && !viewer && !switcher &&
               !apps && !keyboard;
    }
};

class GameplayInput {
public:
    void reset() { x_ = y_ = z_ = 0; click_ = boost_ = false; }

    template <typename Dispatch>
    bool update(bool permitted, bool available, float x, float y, float z,
                bool click, bool clickUp, Dispatch dispatch, bool boost = false) {
        if (!permitted) { reset(); return true; }
        const float axes[] = {available ? x : 0, available ? y : 0, available ? z : 0};
        float* previous[] = {&x_, &y_, &z_};
        const char* names[] = {"move_x", "move_y", "move_z"};
        for (int i = 0; i < 3; ++i) {
            if (*previous[i] != axes[i]) {
                *previous[i] = axes[i];
                if (!dispatch(names[i], axes[i])) return false;
            }
        }
        const bool boosting = available && boost;
        if (boost_ != boosting) {
            boost_ = boosting;
            if (!dispatch("boost", boosting ? 1 : 0)) return false;
        }
        if (available && click && !click_) {
            click_ = true;
            if (!dispatch("click", 1)) return false;
        }
        if (click_ && (!available || clickUp)) {
            click_ = false;
            if (!dispatch("click", 0)) return false;
        }
        return true;
    }

private:
    float x_ = 0, y_ = 0, z_ = 0;
    bool click_ = false, boost_ = false;
};
}  // namespace sf
