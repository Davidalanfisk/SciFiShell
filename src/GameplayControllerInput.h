#pragma once

#include "XInputController.h"

#include <array>
#include <cstddef>

namespace sf {
class GameplayControllerInput {
public:
    void reset() { previous_ = {}; }

    template <typename Dispatch>
    bool update(bool permitted, bool available, const PadState& pad, Dispatch dispatch) {
        if (!permitted) { reset(); return true; }
        const bool active = available && pad.connected;
        const std::array<float, 21> values{
            active ? 1.0f : 0.0f, active ? pad.lx : 0, active ? pad.ly : 0,
            active ? pad.rx : 0, active ? pad.ry : 0, active ? pad.lt : 0, active ? pad.rt : 0,
            active && pad.aHeld ? 1.0f : 0, active && pad.bHeld ? 1.0f : 0,
            active && pad.xHeld ? 1.0f : 0, active && pad.yHeld ? 1.0f : 0,
            active && pad.startHeld ? 1.0f : 0, active && pad.backHeld ? 1.0f : 0,
            active && pad.upHeld ? 1.0f : 0, active && pad.downHeld ? 1.0f : 0,
            active && pad.leftHeld ? 1.0f : 0, active && pad.rightHeld ? 1.0f : 0,
            active && pad.lbHeld ? 1.0f : 0, active && pad.rbHeld ? 1.0f : 0,
            active && pad.leftStickHeld ? 1.0f : 0, active && pad.rightStickHeld ? 1.0f : 0};
        constexpr std::array<const char*, 21> names{
            "pad_connected", "pad_lx", "pad_ly", "pad_rx", "pad_ry", "pad_lt", "pad_rt",
            "pad_a", "pad_b", "pad_x", "pad_y", "pad_start", "pad_back",
            "pad_up", "pad_down", "pad_left", "pad_right", "pad_lb", "pad_rb", "pad_ls", "pad_rs"};
        constexpr std::array<const char*, 21> aliases{
            nullptr, "gamepad_left_x", "gamepad_left_y", "gamepad_right_x", "gamepad_right_y",
            "left_trigger", "right_trigger", "button_a", "button_b", "button_x", "button_y",
            "button_start", "button_back", "dpad_up", "dpad_down", "dpad_left", "dpad_right",
            nullptr, nullptr, nullptr, nullptr};
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (previous_[i] != values[i]) {
                previous_[i] = values[i];
                if (!dispatch(names[i], values[i])) return false;
                if (aliases[i] && !dispatch(aliases[i], values[i])) return false;
            }
        }
        return true;
    }

private:
    std::array<float, 21> previous_{};
};
}
