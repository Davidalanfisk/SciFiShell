#include "../src/InputSystem.h"

#include <cassert>

int main() {
    assert(SDL_Init(SDL_INIT_EVENTS) == 0);
    sf::InputSystem input;
    auto key = [](Uint32 type, SDL_Keycode code, Uint16 mod = KMOD_NONE, Uint8 repeat = 0) {
        SDL_Event event{};
        event.type = type;
        event.key.keysym.sym = code;
        event.key.keysym.mod = mod;
        event.key.repeat = repeat;
        assert(SDL_PushEvent(&event) == 1);
    };
    key(SDL_KEYDOWN, SDLK_LEFT);
    auto in = input.poll();
    assert(in.mediaSeekLeft && in.mediaSeekLeftHeld && !in.mediaSeekLeftReleased);
    key(SDL_KEYDOWN, SDLK_LEFT, KMOD_NONE, 1);
    in = input.poll();
    assert(!in.mediaSeekLeft && in.mediaSeekLeftHeld);
    key(SDL_KEYUP, SDLK_LEFT);
    in = input.poll();
    assert(in.mediaSeekLeftReleased && !in.mediaSeekLeftHeld);
    key(SDL_KEYDOWN, SDLK_RIGHT);
    key(SDL_KEYUP, SDLK_RIGHT);
    in = input.poll();
    assert(in.mediaSeekRight && in.mediaSeekRightReleased && !in.mediaSeekRightHeld);
    key(SDL_KEYDOWN, SDLK_RIGHT);
    input.poll();
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    assert(SDL_PushEvent(&event) == 1);
    in = input.poll();
    assert(!in.mediaSeekRight && !in.mediaSeekRightHeld && !in.mediaSeekRightReleased);
    key(SDL_KEYUP, SDLK_RIGHT);
    assert(!input.poll().mediaSeekRightReleased);
    for (Uint16 mod : {Uint16(KMOD_SHIFT), Uint16(KMOD_CTRL), Uint16(KMOD_ALT), Uint16(KMOD_GUI)}) {
        key(SDL_KEYDOWN, SDLK_RIGHT, mod);
        assert(!input.poll().mediaSeekRight);
        key(SDL_KEYDOWN, SDLK_UP, mod);
        assert(!input.poll().mediaVolumeUp);
        key(SDL_KEYDOWN, SDLK_SPACE, mod);
        assert(!input.poll().mediaPlayPause);
    }
    key(SDL_KEYDOWN, SDLK_DOWN);
    assert(input.poll().mediaVolumeDown);
    key(SDL_KEYDOWN, SDLK_UP);
    assert(input.poll().mediaVolumeUp);
    key(SDL_KEYDOWN, SDLK_SPACE);
    assert(input.poll().mediaPlayPause);
    input.capturing = true;
    key(SDL_KEYDOWN, SDLK_LEFT);
    assert(!input.poll().mediaSeekLeft);
    SDL_Quit();
}
