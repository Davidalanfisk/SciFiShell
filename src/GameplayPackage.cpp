#include "GameplayPackage.h"

#include "GameplayArchive.h"
#include "GameplayLimits.h"
#include "PluginPackage.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace sf {
namespace {
bool validateHostModel(const GameplayPackage& package, const std::string& path, const char* what, std::string& error) {
    if (path.empty()) return true;
    const auto it = package.models.find(path);
    if (it == package.models.end()) { error = std::string(what) + " model is missing from package assets"; return false; }
    const auto& model = it->second;
    if (model.vertices.empty() || model.vertices.size() / 8 > 8192 || model.indices.size() > 24576 ||
        model.embeddedTexture.width > 256 || model.embeddedTexture.height > 256) {
        error = std::string(what) + " model exceeds 8192 vertices/triangles or 256px embedded texture"; return false;
    }
    return true;
}
bool validateDowntown(const GameplayPackage& package, std::string& error) {
    return validateHostModel(package, package.downtownModel, "Downtown", error) &&
           validateHostModel(package, package.roomModel, "Room", error);
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

bool validGameplayActionName(const std::string& name) {
    if (name.empty() || name.size() > 24) return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool parseGameplaySettings(const std::vector<std::string>& items, std::vector<GameplaySetting>& out, std::string& error) {
    out.clear();
    if (items.empty() || items.size() > 4) { error = "settings must declare 1 to 4 entries"; return false; }
    auto label = [](const std::string& s, size_t max) {
        if (s.empty() || s.size() > max) return false;
        for (char c : s) if (c == '|' || c == ',' || c == '=') return false;
        return true;
    };
    for (const auto& item : items) {
        const auto a = item.find('|'), b = a == std::string::npos ? a : item.find('|', a + 1);
        if (b == std::string::npos) { error = "setting must be key|LABEL|value=Label,..."; return false; }
        GameplaySetting setting{item.substr(0, a), item.substr(a + 1, b - a - 1), {}};
        if (!validGameplayActionName(setting.key) || !label(setting.label, 32)) { error = "invalid setting key or label"; return false; }
        for (const auto& existing : out)
            if (existing.key == setting.key) { error = "duplicate setting key"; return false; }
        size_t start = b + 1;
        while (start <= item.size()) {
            const auto end = std::min(item.find(',', start), item.size());
            const auto pair = item.substr(start, end - start);
            const auto eq = pair.find('=');
            if (eq == std::string::npos) { error = "setting choice must be value=Label"; return false; }
            std::string value = pair.substr(0, eq), text = pair.substr(eq + 1);
            if (!validGameplayActionName(value) || !label(text, 24)) { error = "invalid setting choice"; return false; }
            for (const auto& existing : setting.choices)
                if (existing.first == value) { error = "duplicate setting choice"; return false; }
            setting.choices.emplace_back(std::move(value), std::move(text));
            start = end + 1;
        }
        if (setting.choices.size() < 2 || setting.choices.size() > 8) { error = "settings need 2 to 8 choices"; return false; }
        out.push_back(std::move(setting));
    }
    return true;
}

bool parseGameplayHotkeys(const std::vector<std::string>& items, std::vector<std::pair<char, std::string>>& out, std::string& error) {
    out.clear();
    if (items.empty() || items.size() > 4) { error = "hotkeys must declare 1 to 4 entries"; return false; }
    for (const auto& item : items) {
        if (item.size() < 3 || item[1] != '=' || !std::strchr(kGameplayHotkeyLetters, item[0]) || !item[0] ||
            !validGameplayActionName(item.substr(2))) {
            error = std::string("hotkeys must be letter=action using one of ") + kGameplayHotkeyLetters;
            return false;
        }
        for (const auto& existing : out)
            if (existing.first == item[0]) { error = "duplicate hotkey"; return false; }
        out.emplace_back(item[0], item.substr(2));
    }
    return true;
}

bool readGameplayPackage(const std::string& data, GameplayPackage& out, std::string& error) {    out = {};
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
