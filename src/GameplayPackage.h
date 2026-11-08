#pragma once

#include "GameplayModel.h"
#include "GameplayTexture.h"

#include <map>
#include <string>

namespace sf {

struct GameplayPackage {
    std::string id, name, version, author, description, script;
    bool scene = false;
    bool world = false;
    bool layout = false;
    bool impulse = false;
    bool control = false;
    bool input = false;
    bool view = false;
    bool sound = false;
    bool hud = false;
    std::map<std::string, GameplayTexture> textures;
    std::map<std::string, GameplayModel> models;
};

// Validate a package and decode its bounded assets before registration.
bool readGameplayPackage(const std::string& data, GameplayPackage& out, std::string& error);

}  // namespace sf
