#pragma once

#include <string>

namespace sf {
inline std::string gameplayConsentNotice(const std::string& id, bool world, bool input, bool view, bool sound, bool hud, bool layout, bool impulse, bool control) {
    std::string text = id + ": ALLOW SCENE";
    if (world) text += ", CITY COLLISION";
    if (layout) text += ", CITY LAYOUT";
    if (impulse) text += ", PLAYER PUSH";
    if (control) text += ", PLAYER/CAMERA CONTROL";
    if (input) text += ", MOVEMENT/EXCLUSIVE CLICKS";
    if (view) text += ", VIEW POSITION/DIRECTION";
    if (sound) text += ", SOUND EFFECTS";
    if (hud) text += ", HUD TEXT";
    return text + "? CLICK AGAIN WITHIN 30 SECONDS";
}
}  // namespace sf
