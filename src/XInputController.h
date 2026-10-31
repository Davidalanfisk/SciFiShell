#pragma once
// Xbox controller: XInput on Windows, SDL_GameController (same hardware, same layout) elsewhere.
namespace sf {

struct PadState {
    bool connected = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // sticks, -1..1, deadzone applied, +y = up
    float lt = 0, rt = 0;                  // analogue triggers, 0..1
    bool aPressed = false, bPressed = false, upPressed = false, downPressed = false, yPressed = false;  // edges
    bool leftPressed = false, rightPressed = false;
    bool xPressed = false, lbPressed = false, rbPressed = false;  // X: on-screen keyboard, LB / RB: previous / next window
};

class XInputController {
public:
    XInputController();
    ~XInputController();
    PadState poll();

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace sf
