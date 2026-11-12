#pragma once

#include <string>
#include <vector>

namespace sf {
inline std::string gameplayConsentNotice(const std::string& id, bool world, bool input, bool view, bool sound, bool hud, bool layout, bool impulse, bool control, bool storage = false, bool controller = false, bool messaging = false,
                                         const std::vector<std::string>& networkHosts = {}, bool map = false,
                                         const std::string& hotkeys = {}, const std::vector<std::string>& linkHosts = {}) {
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
    if (map) text += ", CITY MAP + YOUR POSITION";
    if (!hotkeys.empty()) {
        text += ", HOTKEYS";
        for (char key : hotkeys) { text += ' '; text += key >= 'a' && key <= 'z' ? static_cast<char>(key - 'a' + 'A') : key; }
    }
    if (!networkHosts.empty()) {
        text += ", HTTPS TEXT FROM";
        for (size_t i = 0; i < networkHosts.size(); ++i) text += (i ? ", " : " ") + networkHosts[i];
    }
    if (!linkHosts.empty()) {
        text += ", OPEN LINKS IN BUILT-IN BROWSER FROM";
        for (size_t i = 0; i < linkHosts.size(); ++i) text += (i ? ", " : " ") + linkHosts[i];
    }
    return text + "? CLICK AGAIN WITHIN 30 SECONDS";
}
}  // namespace sf
