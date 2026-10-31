#include "XInputController.h"

#include <cmath>

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#    include <xinput.h>
#else
#    include <SDL.h>
#endif

namespace sf {

namespace {
float applyDeadzone(float v, float dz) {
    float a = std::fabs(v);
    if (a < dz) return 0.0f;
    float s = (a - dz) / (1.0f - dz);
    return v < 0 ? -s : s;
}
}  // namespace

struct XInputController::Impl {
    bool prevA = false, prevB = false, prevUp = false, prevDown = false, prevY = false, prevLeft = false, prevRight = false, prevX = false, prevLB = false, prevRB = false;
#ifndef _WIN32
    SDL_GameController* pad = nullptr;
#endif
};

XInputController::XInputController() : impl_(new Impl) {}

XInputController::~XInputController() {
#ifndef _WIN32
    if (impl_->pad) SDL_GameControllerClose(impl_->pad);
#endif
    delete impl_;
}

PadState XInputController::poll() {
    PadState s;
    bool a = false, b = false, up = false, down = false, y = false, left = false, right = false, x = false, lb = false, rb = false;
#ifdef _WIN32
    XINPUT_STATE st{};
    if (XInputGetState(0, &st) == ERROR_SUCCESS) {
        s.connected = true;
        const XINPUT_GAMEPAD& g = st.Gamepad;
        s.lx = applyDeadzone(g.sThumbLX / 32767.0f, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32767.0f);
        s.ly = applyDeadzone(g.sThumbLY / 32767.0f, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32767.0f);
        s.rx = applyDeadzone(g.sThumbRX / 32767.0f, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32767.0f);
        s.ry = applyDeadzone(g.sThumbRY / 32767.0f, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32767.0f);
        s.rt = applyDeadzone(g.bRightTrigger / 255.0f, 0.1f);
        s.lt = applyDeadzone(g.bLeftTrigger / 255.0f, 0.1f);
        a = (g.wButtons & XINPUT_GAMEPAD_A) != 0;
        b = (g.wButtons & XINPUT_GAMEPAD_B) != 0;
        up = (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
        down = (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        left = (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        right = (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        y = (g.wButtons & XINPUT_GAMEPAD_Y) != 0;
        x = (g.wButtons & XINPUT_GAMEPAD_X) != 0;
        lb = (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
        rb = (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    }
#else
    if (!impl_->pad || !SDL_GameControllerGetAttached(impl_->pad)) {
        impl_->pad = nullptr;
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_IsGameController(i) && (impl_->pad = SDL_GameControllerOpen(i))) break;
    }
    if (impl_->pad) {
        s.connected = true;
        auto axis = [&](SDL_GameControllerAxis ax) {
            return applyDeadzone(SDL_GameControllerGetAxis(impl_->pad, ax) / 32767.0f, 0.24f);
        };
        s.lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
        s.ly = -axis(SDL_CONTROLLER_AXIS_LEFTY);
        s.rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        s.ry = -axis(SDL_CONTROLLER_AXIS_RIGHTY);
        s.lt = applyDeadzone(SDL_GameControllerGetAxis(impl_->pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f, 0.1f);
        s.rt = applyDeadzone(SDL_GameControllerGetAxis(impl_->pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f, 0.1f);
        a = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_A) != 0;
        b = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_B) != 0;
        up = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
        down = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
        left = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
        right = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;
        y = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_Y) != 0;
        x = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_X) != 0;
        lb = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
        rb = SDL_GameControllerGetButton(impl_->pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
    }
#endif
    s.yPressed = y && !impl_->prevY;
    impl_->prevY = y;
    s.aPressed = a && !impl_->prevA;
    s.bPressed = b && !impl_->prevB;
    s.upPressed = up && !impl_->prevUp;
    s.downPressed = down && !impl_->prevDown;
    s.leftPressed = left && !impl_->prevLeft;
    s.rightPressed = right && !impl_->prevRight;
    s.xPressed = x && !impl_->prevX;
    s.lbPressed = lb && !impl_->prevLB;
    s.rbPressed = rb && !impl_->prevRB;
    impl_->prevX = x;
    impl_->prevLB = lb;
    impl_->prevRB = rb;
    impl_->prevLeft = left;
    impl_->prevRight = right;
    impl_->prevA = a;
    impl_->prevB = b;
    impl_->prevUp = up;
    impl_->prevDown = down;
    return s;
}

}  // namespace sf
