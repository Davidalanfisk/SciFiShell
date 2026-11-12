#include "InputSystem.h"

namespace sf {

void InputSystem::setRelativeMouse(bool enabled) {
    if (relative_ != enabled) resetLook_ = true;
    relative_ = enabled;
    SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE);
}

void InputSystem::setTextInput(bool enabled) {
    if (enabled) SDL_StartTextInput();
    else SDL_StopTextInput();
}

FrameInput InputSystem::poll() {
    FrameInput in;
    in.mouseReset = resetLook_;
    resetLook_ = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT: in.quit = true; break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED)
                    in.resized = true;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    in.mouseReset = true;
                    seekLeftHeld_ = seekRightHeld_ = false;
                    in.mediaSeekLeft = in.mediaSeekRight = false;
                    in.mediaSeekLeftReleased = in.mediaSeekRightReleased = false;
                }
                // A hosted program taking focus drops SDL's mouse capture; re-establish it as soon as focus returns.
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e.window.event == SDL_WINDOWEVENT_ENTER) {
                    in.mouseReset = true;
                    if (relative_) {
                        SDL_SetRelativeMouseMode(SDL_FALSE);
                        SDL_SetRelativeMouseMode(SDL_TRUE);
                    }
                }
                break;
            case SDL_DROPFILE:
                if (e.drop.file) in.dropped.emplace_back(e.drop.file);
                SDL_free(e.drop.file);
                break;
            case SDL_TEXTINPUT: in.text += e.text.text; break;
            case SDL_MOUSEWHEEL: in.wheel += static_cast<float>(e.wheel.y); break;
            case SDL_KEYDOWN: {
                const bool ctrl = (e.key.keysym.mod & KMOD_CTRL) != 0;
                switch (e.key.keysym.sym) {
                    case SDLK_BACKSPACE: in.keys.push_back(EditKey::Backspace); break;
                    case SDLK_DELETE: in.keys.push_back(EditKey::Delete); break;
                    case SDLK_RETURN: case SDLK_KP_ENTER: in.keys.push_back(EditKey::Enter); break;
                    case SDLK_TAB: in.keys.push_back(EditKey::Tab); break;
                    case SDLK_LEFT: in.keys.push_back(EditKey::Left); break;
                    case SDLK_RIGHT: in.keys.push_back(EditKey::Right); break;
                    case SDLK_UP: in.keys.push_back(EditKey::Up); break;
                    case SDLK_DOWN: in.keys.push_back(EditKey::Down); break;
                    case SDLK_HOME: in.keys.push_back(ctrl ? EditKey::DocStart : EditKey::Home); break;
                    case SDLK_END: in.keys.push_back(ctrl ? EditKey::DocEnd : EditKey::End); break;
                    case SDLK_PAGEUP: in.keys.push_back(EditKey::PageUp); break;
                    case SDLK_PAGEDOWN: in.keys.push_back(EditKey::PageDown); break;
                    case SDLK_s: if (ctrl) in.keys.push_back(EditKey::Save); break;
                    case SDLK_v: if (ctrl) in.keys.push_back(EditKey::Paste); break;
                    case SDLK_c: if (ctrl) in.keys.push_back(EditKey::Copy); break;
                    case SDLK_x: if (ctrl) in.keys.push_back(EditKey::Cut); break;
                    case SDLK_a: if (ctrl) in.keys.push_back(EditKey::SelectAll); break;
                    case SDLK_z: if (ctrl) in.keys.push_back(EditKey::Undo); break;
                    case SDLK_e: if (!ctrl && !e.key.repeat) in.keys.push_back(EditKey::Edit); break;
                    default: break;
                }
                if (e.key.repeat) break;
                if (capturing) {
                    const SDL_Keycode k = e.key.keysym.sym;
                    if (k >= SDLK_LCTRL && k <= SDLK_RGUI) break;  // modifiers alone cannot be the key
                    capturing = false;
                    if (k != SDLK_ESCAPE) in.keyCaptured = static_cast<int>(k);
                    in.escape = false;
                    break;
                }
                if (e.key.keysym.sym == SDLK_TAB && (e.key.keysym.mod & (KMOD_ALT | KMOD_CTRL)))
                    in.tabStep = (e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1;
                if (e.key.keysym.sym == SDLK_F11) in.appFullscreen = true;
                if (e.key.keysym.sym == SDLK_F12) in.screenshot = true;
                if (e.key.keysym.sym == SDLK_l && ctrl) in.focusAddress = true;
                if (e.key.keysym.sym == SDLK_F3) in.depthCycle = true;
                if (e.key.keysym.sym == SDLK_F4) in.depthSite = true;
                if (e.key.keysym.sym == SDLK_F5) in.reload = true;
                if (e.key.keysym.sym == SDLK_F2) in.ring = true;
                if (e.key.keysym.sym == SDLK_F6) in.themeCycle = true;
                if (e.key.keysym.sym == SDLK_F7) in.osk = true;
                if (e.key.keysym.sym == SDLK_F9) in.cityToggle = true;
                if (e.key.keysym.sym == SDLK_F10) in.carToggle = true;
                if (e.key.keysym.sym == SDLK_F1) in.harvi = true;
                if (e.key.keysym.sym == SDLK_c && !ctrl) in.carCam = true;
                if (e.key.keysym.sym == SDLK_g && !(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI | KMOD_SHIFT)))
                    in.gameplayControl = true;
                if (e.key.keysym.sym == arrangeKey) in.arrange = true;
                if ((e.key.keysym.mod & KMOD_ALT) && e.key.keysym.sym == SDLK_LEFT) in.navBack = true;
                if ((e.key.keysym.mod & KMOD_ALT) && e.key.keysym.sym == SDLK_RIGHT) in.navForward = true;
                if (!(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI | KMOD_SHIFT))) {
                    if (e.key.keysym.sym == SDLK_LEFT) { in.mediaSeekLeft = true; seekLeftHeld_ = true; }
                    if (e.key.keysym.sym == SDLK_RIGHT) { in.mediaSeekRight = true; seekRightHeld_ = true; }
                    if (e.key.keysym.sym == SDLK_UP) in.mediaVolumeUp = true;
                    if (e.key.keysym.sym == SDLK_DOWN) in.mediaVolumeDown = true;
                    if (e.key.keysym.sym == SDLK_SPACE) in.mediaPlayPause = true;
                }
                if (e.key.keysym.sym == SDLK_b && !ctrl) in.browser = true;
                if (!(e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI | KMOD_SHIFT)) &&
                    e.key.keysym.sym >= SDLK_a && e.key.keysym.sym <= SDLK_z && in.pluginKeys.size() < 8)
                    in.pluginKeys.push_back(static_cast<char>('a' + (e.key.keysym.sym - SDLK_a)));
                if (ctrl && e.key.keysym.sym == SDLK_v) break;  // Ctrl+V is paste, not "toggle VR"
                switch (e.key.keysym.sym) {
                    case SDLK_UP: in.up = true; break;
                    case SDLK_DOWN: in.down = true; break;
                    case SDLK_RETURN: in.select = true; break;
                    case SDLK_SPACE: in.space = true; break;
                    case SDLK_ESCAPE: in.escape = true; break;
                    case SDLK_BACKSPACE: in.back = true; break;
                    case SDLK_LEFT: in.left = true; break;
                    case SDLK_RIGHT: in.right = true; break;
                    case SDLK_f: in.fullscreen = true; break;
                    case SDLK_v: in.toggleVR = true; break;
                    default: break;
                }
                break;
            }
            case SDL_KEYUP:
                if (e.key.keysym.sym == SDLK_LEFT) { in.mediaSeekLeftReleased = seekLeftHeld_; seekLeftHeld_ = false; }
                if (e.key.keysym.sym == SDLK_RIGHT) { in.mediaSeekRightReleased = seekRightHeld_; seekRightHeld_ = false; }
                switch (e.key.keysym.sym) {
                    case SDLK_LALT: case SDLK_RALT: case SDLK_LCTRL: case SDLK_RCTRL: in.modReleased = true; break;
                    default: break;
                }
                break;
            case SDL_MOUSEMOTION:
                in.mouseMoved = true;
                in.lookX += static_cast<float>(e.motion.xrel);
                in.lookY += static_cast<float>(e.motion.yrel);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) in.click = true;
                if (e.button.button == SDL_BUTTON_MIDDLE) in.ring = true;
            if (e.button.button == SDL_BUTTON_RIGHT) in.pin = true;
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) in.clickUp = true;
                break;
            default: break;
        }
    }
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    in.mediaSeekLeftHeld = seekLeftHeld_;
    in.mediaSeekRightHeld = seekRightHeld_;
    in.moveY = (k[SDL_SCANCODE_W] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_S] ? 1.0f : 0.0f);
    in.moveX = (k[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_A] ? 1.0f : 0.0f);
    in.moveZ = (k[SDL_SCANCODE_SPACE] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_LCTRL] ? 1.0f : 0.0f);
    in.shift = k[SDL_SCANCODE_LSHIFT] != 0;
    const Uint32 buttons = SDL_GetMouseState(&in.mouseX, &in.mouseY);
    in.rightHeld = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    if (!relative_ || in.mouseReset) in.lookX = in.lookY = 0;
    return in;
}

}  // namespace sf
