#include "GameplayControllerInput.h"

#include <iostream>
#include <map>
#include <string>
#include <vector>

int main() {
    sf::GameplayControllerInput input;
    sf::PadState pad;
    std::map<std::string, float> values;
    int calls = 0;
    std::vector<std::string> events;
    auto dispatch = [&](const char* name, float value) {
        values[name] = value;
        events.emplace_back(name);
        ++calls;
        return true;
    };
    auto fail = [](const char* message) { std::cerr << message << '\n'; return 1; };
    pad.connected = true;
    pad.lx = -1; pad.ry = 0.5f; pad.lt = 1;
    pad.aHeld = pad.startHeld = pad.upHeld = true;
    if (!input.update(false, true, pad, dispatch) || calls) return fail("permission gate");
    if (!input.update(true, true, pad, dispatch) || values["pad_connected"] != 1 ||
        values["pad_lx"] != -1 || values["pad_ry"] != 0.5f || values["pad_lt"] != 1 ||
        values["pad_a"] != 1 || values["pad_start"] != 1 || values["pad_up"] != 1 ||
        values["gamepad_left_x"] != -1 || values["gamepad_right_y"] != 0.5f ||
        values["left_trigger"] != 1 || values["button_a"] != 1 ||
        values["button_start"] != 1 || values["dpad_up"] != 1)
        return fail("connected controls");
    for (std::size_t i = 0; i + 1 < events.size(); ++i)
        if (events[i] == "pad_a" && events[i + 1] != "button_a") return fail("legacy alias ordering");
    const int first = calls;
    if (!input.update(true, true, pad, dispatch) || calls != first) return fail("unchanged state");
    if (!input.update(true, false, pad, dispatch) || values["pad_connected"] != 0 ||
        values["pad_lx"] != 0 || values["pad_a"] != 0 || values["pad_start"] != 0)
        return fail("focus loss clears held inputs");
    if (!input.update(true, true, pad, dispatch) || values["pad_a"] != 1) return fail("focus regain");
    pad.connected = false;
    if (!input.update(true, true, pad, dispatch) || values["pad_connected"] != 0 ||
        values["pad_lt"] != 0 || values["pad_a"] != 0) return fail("disconnect clears inputs");
    pad.connected = true;
    if (input.update(true, true, pad, [](const char*, float) { return false; }))
        return fail("callback failure propagation");
    input.reset();
    pad.lx = pad.ly = pad.rx = pad.ry = pad.lt = pad.rt = 1;
    pad.aHeld = pad.bHeld = pad.xHeld = pad.yHeld = pad.startHeld = pad.backHeld = true;
    pad.upHeld = pad.downHeld = pad.leftHeld = pad.rightHeld = true;
    if (!input.update(true, true, pad, dispatch)) return fail("all named controller controls");
    for (const char* name : {"gamepad_left_x", "gamepad_left_y", "gamepad_right_x", "gamepad_right_y",
                             "left_trigger", "right_trigger", "button_a", "button_b", "button_x", "button_y",
                             "button_start", "button_back", "dpad_up", "dpad_down", "dpad_left", "dpad_right"})
        if (values[name] != 1) return fail(name);
    return 0;
}
