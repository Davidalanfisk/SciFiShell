#include "GameplayPackage.h"

#include "GameplayArchive.h"
#include "GameplayLimits.h"
#include "PluginPackage.h"

#include <new>

namespace sf {
namespace {
bool validateDowntown(const GameplayPackage& package, std::string& error) {
    if (package.downtownModel.empty()) return true;
    const auto it = package.models.find(package.downtownModel);
    if (it == package.models.end()) { error = "Downtown model is missing from package assets"; return false; }
    const auto& model = it->second;
    if (model.vertices.empty() || model.vertices.size() / 8 > 8192 || model.indices.size() > 24576 ||
        model.embeddedTexture.width > 256 || model.embeddedTexture.height > 256) {
        error = "Downtown model exceeds 8192 vertices/triangles or 256px embedded texture"; return false;
    }
    return true;
}
bool decodeAsset(const std::string& path, const std::string& bytes, GameplayPackage& loaded,
                 size_t& decoded, std::string& error) {
    std::string assetError;
    if (path.compare(path.size() - 4, 4, ".wav") == 0 ||
        path.compare(path.size() - 4, 4, ".ogg") == 0) {
        if (!loaded.sound) { error = path + ": audio requires sound permission"; return false; }
        auto audio = std::make_shared<GameplayAudio>();
        if (!decodeGameplayAudio(path, bytes, *audio, assetError)) { error = path + ": " + assetError; return false; }
        const size_t size = audio->samples.size() * sizeof(audio->samples[0]);
        if (size > kGameplayDecodedLimit - decoded) { error = "Gameplay decoded assets exceed memory limit"; return false; }
        decoded += size;
        loaded.audio.emplace(path, std::move(audio));
    } else if (path.compare(path.size() - 4, 4, ".png") == 0) {
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
    return true;
}
}

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
            if (!decodeAsset(path, bytes, loaded, decoded, error)) return false;
        }
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay package";
        return false;
    }
    if (!validateDowntown(loaded, error)) return false;
    out = std::move(loaded);
    error.clear();
    return true;
}

bool readGameplayPackageFile(const std::filesystem::path& path, GameplayPackage& out, std::string& error) {
    out = {};
    GameplayPackage loaded;
    size_t decoded = 0;
    const bool ok = readGameplayArchiveFile(path,
        [&](const std::string& manifest, const std::string& script, std::string& failure) {
            if (!PluginPackageStore::parseGameplayManifest(manifest, loaded, failure)) return false;
            loaded.script = script;
            return true;
        },
        [&](const std::string& name, const std::string& bytes, std::string& failure) {
            return decodeAsset(name, bytes, loaded, decoded, failure);
        }, error);
    if (!ok || !validateDowntown(loaded, error)) return false;
    out = std::move(loaded);
    return true;
}
}  // namespace sf
