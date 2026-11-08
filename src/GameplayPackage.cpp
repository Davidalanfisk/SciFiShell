#include "GameplayPackage.h"

#include "GameplayArchive.h"
#include "GameplayLimits.h"
#include "PluginPackage.h"

#include <new>

namespace sf {
bool readGameplayPackage(const std::string& data, GameplayPackage& out, std::string& error) {
    out = {};
    GameplayArchive archive;
    if (!readGameplayArchive(data, archive, error)) return false;
    GameplayPackage loaded;
    if (!PluginPackageStore::parseGameplayManifest(archive.manifest, loaded, error)) return false;
    size_t decoded = 0;
    try {
        loaded.script = std::move(archive.script);
        for (const auto& [path, bytes] : archive.assets) {
            std::string assetError;
            if (path.compare(path.size() - 4, 4, ".png") == 0) {
                GameplayTexture texture;
                if (!decodeGameplayTexture(bytes, texture, assetError)) { error = path + ": " + assetError; return false; }
                if (texture.rgba.size() > kGameplayDecodedLimit - decoded) { error = "Gameplay decoded assets exceed memory limit"; return false; }
                decoded += texture.rgba.size();
                loaded.textures.emplace(path, std::move(texture));
            } else {
                GameplayModel model;
                if (!decodeGameplayModel(bytes, model, assetError)) { error = path + ": " + assetError; return false; }
                const size_t size = model.vertices.size() * sizeof(float) +
                                    model.indices.size() * sizeof(std::uint32_t) + model.embeddedTexture.rgba.size();
                if (size > kGameplayDecodedLimit - decoded) { error = "Gameplay decoded assets exceed memory limit"; return false; }
                decoded += size;
                loaded.models.emplace(path, std::move(model));
            }
        }
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay package";
        return false;
    }
    out = std::move(loaded);
    error.clear();
    return true;
}
}  // namespace sf
