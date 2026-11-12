#pragma once

#include "GameplayModel.h"
#include "GameplayTexture.h"
#include "GameplayAudio.h"

#include <map>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sf {

// Host-owned plugin setting shown on room consoles; the first choice is the default.
struct GameplaySetting {
    std::string key, label;
    std::vector<std::pair<std::string, std::string>> choices;  // value, label
};

struct GameplayPackage {
    std::string id, name, version, author, description, script;
    std::string downtownModel; // Optional validated assets/*.glb; rendered by the host even when disabled.
    std::string roomModel;     // Optional validated assets/*.glb shown inside the plugin's room.
    bool scene = false;
    bool world = false;
    bool layout = false;
    bool impulse = false;
    bool control = false;
    bool input = false;
    bool controller = false;
    bool view = false;
    bool sound = false;
    bool hud = false;
    bool storage = false;
    bool messaging = false;
    bool network = false;
    bool map = false;
    std::vector<std::string> networkHosts; // Exact HTTPS host names shown at consent; required iff network.
    std::vector<std::string> linkHosts;    // HTTPS hosts open_link may show in the built-in browser.
    std::vector<GameplaySetting> settings;
    std::vector<std::pair<char, std::string>> hotkeys;  // lowercase letter, action name
    std::map<std::string, GameplayTexture> textures;
    std::map<std::string, GameplayModel> models;
    std::map<std::string, std::shared_ptr<const GameplayAudio>> audio;
};

// Validate a package and decode its bounded assets before registration.
bool readGameplayPackage(const std::string& data, GameplayPackage& out, std::string& error);
bool readGameplayPackageFile(const std::filesystem::path& path, GameplayPackage& out, std::string& error);
// Manifest encodings: settings "key|LABEL|v1=Label,v2=Label" (<=4, 2..8 choices) and hotkeys "n=action" (<=4).
bool parseGameplaySettings(const std::vector<std::string>& items, std::vector<GameplaySetting>& out, std::string& error);
bool parseGameplayHotkeys(const std::vector<std::string>& items, std::vector<std::pair<char, std::string>>& out, std::string& error);
bool validGameplayActionName(const std::string& name);
// Letters not used by shell shortcuts; plugins may bind only these.
constexpr const char* kGameplayHotkeyLetters = "hijkmnopqrtuxyz";

}  // namespace sf
