#pragma once

#include <map>
#include <string>

namespace sf {

// Internal archive reader: callers must validate the manifest and asset contents separately.
struct GameplayArchive {
    std::string manifest;
    std::string script;
    std::map<std::string, std::string> assets;
};

bool readGameplayArchive(const std::string& data, GameplayArchive& out, std::string& error);

}  // namespace sf
