#pragma once

#include <map>
#include <filesystem>
#include <functional>
#include <string>

namespace sf {

// Internal archive reader: callers must validate the manifest and asset contents separately.
struct GameplayArchive {
    std::string manifest;
    std::string script;
    std::map<std::string, std::string> assets;
};

bool readGameplayArchive(const std::string& data, GameplayArchive& out, std::string& error);
using GameplayArchiveBegin = std::function<bool(const std::string&, const std::string&, std::string&)>;
using GameplayArchiveAsset = std::function<bool(const std::string&, const std::string&, std::string&)>;
bool readGameplayArchiveFile(const std::filesystem::path& path, const GameplayArchiveBegin& begin,
                             const GameplayArchiveAsset& asset, std::string& error);

}  // namespace sf
