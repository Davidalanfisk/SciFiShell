#pragma once

#include "GameplayModel.h"
#include "GameplayTexture.h"
#include "GameplayAudio.h"

#include <map>
#include <filesystem>
#include <memory>
#include <string>

namespace sf {

struct GameplayPackage {
    std::string id, name, version, author, description, script;
    std::string downtownModel; // Optional validated assets/*.glb; rendered by the host even when disabled.
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
    std::map<std::string, GameplayTexture> textures;
    std::map<std::string, GameplayModel> models;
    std::map<std::string, std::shared_ptr<const GameplayAudio>> audio;
};

// Validate a package and decode its bounded assets before registration.
bool readGameplayPackage(const std::string& data, GameplayPackage& out, std::string& error);
bool readGameplayPackageFile(const std::filesystem::path& path, GameplayPackage& out, std::string& error);

}  // namespace sf
