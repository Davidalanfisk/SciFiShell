#include "GameplayControllerInput.h"
#include "GameplayControlInput.h"
#include "GameplayInput.h"
#include "GameplayMouseInput.h"

#include <cmath>
#include <iostream>
#include <limits>
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
    {
        sf::GameplayInput keyboard;
        values.clear();
        calls = 0;
        if (!keyboard.update(false, true, 0, 0, 0, false, false, dispatch, true) || calls)
            return fail("boost requires input permission");
        if (!keyboard.update(true, true, 0, 0, 0, false, false, dispatch, true) || values["boost"] != 1)
            return fail("left shift boost press");
        const int pressed = calls;
        if (!keyboard.update(true, true, 0, 0, 0, false, false, dispatch, true) || calls != pressed)
            return fail("held shift does not repeat press");
        if (!keyboard.update(true, false, 0, 0, 0, false, false, dispatch, true) || values["boost"] != 0)
            return fail("focus or overlay loss releases boost");
        if (!keyboard.update(true, true, 0, 0, 0, false, false, dispatch, true) || values["boost"] != 1 ||
            !keyboard.update(true, true, 0, 0, 0, false, false, dispatch, false) || values["boost"] != 0)
            return fail("boost regain and physical release");
        keyboard.reset();
        if (!keyboard.update(true, true, 1, 0, 0, false, false, dispatch, true) ||
            values["boost"] != 1 || values["move_x"] != 1)
            return fail("new owner receives held keyboard snapshot");
        keyboard.reset();
        if (keyboard.update(true, true, 0, 0, 0, false, false,
                            [](const char*, double) { return false; }, true))
            return fail("boost callback failure propagates");
    }
    {
        sf::GameplayMouseInput mouse;
        int count = 0;
        double yaw = 0, pitch = 0;
        auto relative = [&](const char* name, double value) {
            ++count;
            if (std::string(name) == "mouse_yaw") yaw += value;
            else if (std::string(name) == "mouse_pitch") pitch += value;
            else return false;
            return true;
        };
        if (!mouse.update(false, true, .01, -.02, .02, relative) || count ||
            !mouse.update(true, true, .01, -.02, .02, relative) || count)
            return fail("mouse permission and acquisition frame discarded");
        for (int i = 0; i < 2; ++i)
            if (!mouse.update(true, true, .01, -.02, .02, relative))
                return fail("mouse frame callbacks");
        if (count != 4 || std::abs(yaw - .02) > 1e-12 || std::abs(pitch + .04) > 1e-12)
            return fail("identical mouse deltas repeat without dt multiplication");
        mouse.update(true, true, 0, 0, .02, relative);
        mouse.update(true, false, 1, 1, .02, relative);
        mouse.update(true, true, 1, 1, .02, relative);
        if (count != 4) return fail("no held replay or context recovery motion");
        mouse.reset();
        mouse.update(true, true, 1, 1, .02, relative);
        if (count != 4) return fail("new owner discards accumulated mouse motion");
        mouse.update(true, true, 9, -9, .001, relative);
        if (std::abs(yaw - .024) > 1e-12 || std::abs(pitch + .044) > 1e-12)
            return fail("mouse angular clamp uses actual frame duration");
        mouse.update(true, true, std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN(), .02, relative);
        if (count != 6) return fail("invalid relative motion is discarded");
        for (double dt : {0.0, -1.0, .101, std::numeric_limits<double>::quiet_NaN()})
            mouse.update(true, true, 1, 1, dt, relative);
        mouse.update(true, true, 1, 1, .02, relative);
        if (count != 6) return fail("invalid frame durations disarm mouse delivery");
        if (mouse.update(true, true, .01, 0, .02, [](const char*, double) { return false; }))
            return fail("mouse callback failure propagates");
        for (int fps : {30, 60, 120, 240}) {
            mouse.reset();
            count = 0; yaw = pitch = 0;
            const double dt = 1.0 / fps;
            mouse.update(true, true, 0, 0, dt, relative);
            for (int frame = 0; frame < fps; ++frame)
                mouse.update(true, true, .6 / fps, -.3 / fps, dt, relative);
            if (count != 2 * fps || std::abs(yaw - .6) > 1e-12 || std::abs(pitch + .3) > 1e-12)
                return fail("relative mouse totals independent of frame rate");
        }
    }
    {
        sf::GameplayControlInput routing;
        using Action = sf::GameplayControlInput::Action;
        sf::PadState raw, shell, plugin;
        raw.connected = true;
        const auto update = [&](bool owned) {
            shell = raw;
            return routing.update(owned, shell, plugin);
        };
        if (update(false) != Action::None) return fail("neutral connection cannot activate");
        raw.startHeld = true;
        if (update(false) != Action::Acquire || update(false) != Action::None)
            return fail("start activation is an edge");
        raw.startHeld = false;
        raw.lx = 0.7f; raw.rt = 1;
        raw.xHeld = raw.xPressed = raw.yHeld = raw.yPressed = raw.rbHeld = raw.rbPressed = true;
        if (update(true) != Action::None || shell.connected || shell.xPressed || shell.yPressed ||
            shell.rbPressed || shell.lx != 0 || shell.rt != 0 ||
            plugin.lx != raw.lx || plugin.rt != raw.rt || !plugin.xHeld || !plugin.rbHeld)
            return fail("owner receives flight controls without keyboard, ring or window shortcuts");
        raw.bHeld = raw.bPressed = true;
        if (update(true) != Action::Release || plugin.bHeld || plugin.connected || shell.bPressed)
            return fail("B releases lease without script or shell back action");
        if (update(false) != Action::None || shell.lx != 0 || plugin.rt != 0)
            return fail("held flight controls cannot leak after release");
        raw = {}; raw.connected = true;
        update(false);
        raw.xHeld = raw.xPressed = true;
        if (update(false) != Action::None || !shell.xPressed)
            return fail("native shortcuts return after neutral without ownership");
        raw = {}; raw.connected = true;
        update(true);
        raw.startHeld = true;
        if (update(true) != Action::Release || plugin.startHeld)
            return fail("Start toggles owned control off");
        raw = {}; raw.connected = true;
        update(false); update(true);
        raw.backHeld = true;
        if (update(true) != Action::Release || plugin.backHeld)
            return fail("Back exits without reaching plugin");
        raw = {}; raw.connected = true;
        update(false); update(true);
        raw = {};
        if (update(true) != Action::Release || plugin.connected)
            return fail("controller disconnect releases owned flight");
        update(false);
        raw.connected = raw.startHeld = true;
        if (update(false) != Action::None)
            return fail("reconnecting with Start held cannot acquire");
    }
    input.reset();
    pad.rbHeld = true;
    if (!input.update(true, true, pad, dispatch) || values["pad_rb"] != 1)
        return fail("new owner receives current controller snapshot");
    return 0;
}
