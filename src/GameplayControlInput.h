#pragma once

#include "XInputController.h"

namespace sf {

class GameplayControlInput {
public:
    enum class Action { None, Acquire, Release };

    Action update(bool ownsController, PadState& shell, PadState& plugin, bool exitWithY = false) {
        const PadState raw = shell;
        plugin = raw;
        const bool start = connected_ && raw.connected && raw.startHeld && !start_;
        const bool back = connected_ && raw.connected && raw.backHeld && !back_;
        const bool exit = connected_ && raw.connected && raw.bHeld && !exit_;
        const bool exitY = connected_ && raw.connected && raw.yHeld && !exitY_;
        const bool disconnected = connected_ && !raw.connected;
        connected_ = raw.connected;
        start_ = raw.startHeld;
        back_ = raw.backHeld;
        exit_ = raw.bHeld;
        exitY_ = raw.yHeld;
        if (ownsController) {
            captured_ = true;
            shell = {};
            reserve(plugin, exitWithY);
            if (start || back || (exitWithY ? exitY : exit) || disconnected) {
                plugin = {};
                return Action::Release;
            }
        } else if (captured_) {
            shell = plugin = {};
            if (neutral(raw)) captured_ = false;
        } else if (start) {
            return Action::Acquire;
        }
        return Action::None;
    }

    static void reserve(PadState& pad, bool exitWithY = false) {
        pad.startHeld = pad.backHeld = false;
        if (exitWithY) pad.yHeld = pad.yPressed = false;
        else pad.bHeld = pad.bPressed = false;
    }

private:
    static bool neutral(const PadState& p) {
        return !p.connected ||
            (p.lx == 0 && p.ly == 0 && p.rx == 0 && p.ry == 0 && p.lt == 0 && p.rt == 0 &&
             !p.aHeld && !p.bHeld && !p.xHeld && !p.yHeld && !p.startHeld && !p.backHeld &&
             !p.upHeld && !p.downHeld && !p.leftHeld && !p.rightHeld && !p.lbHeld && !p.rbHeld &&
             !p.leftStickHeld && !p.rightStickHeld);
    }
    bool connected_ = false, start_ = false, back_ = false, exit_ = false, exitY_ = false, captured_ = false;
};

}  // namespace sf
