#pragma once

#include <string>

namespace sf {
inline std::string gameplayConsentNotice(const std::string& id, bool world, bool input, bool view, bool sound, bool hud, bool layout, bool impulse, bool control, bool storage = false, bool controller = false, bool messaging = false) {
    std::string text = id + ": ALLOW SCENE";
    if (world) text += ", CITY COLLISION";
    if (layout) text += ", CITY LAYOUT";
    if (impulse) text += ", PLAYER PUSH";
    if (control) text += ", PLAYER/CAMERA CONTROL";
    if (input) text += ", MOVEMENT/EXCLUSIVE CLICKS";
    if (view) text += ", VIEW POSITION/DIRECTION";
    if (sound) text += ", SOUND";
    if (hud) text += ", HUD";
    if (storage) text += ", PERSISTENT STORAGE";
    if (controller) text += ", CONTROLLER INPUT";
    if (messaging) text += ", PLUGIN MESSAGES";
    return text + "? CLICK AGAIN WITHIN 30 SECONDS";
}
}  // namespace sf
