#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "GameplayDigest.h"

namespace sf {

struct GameplayPackage;

struct ThemePackage {
    std::string id, name, version, author, description, accent, alt;
};

struct ScriptPackage {
    std::string id, name, version, author, description, script;
};

class PluginPackageStore {
public:
    using Register = std::function<bool(const ThemePackage&)>;
    using RegisterScript = std::function<bool(const ScriptPackage&)>;
    using RegisterGameplay = std::function<bool(const GameplayPackage&, const std::filesystem::path&, const GameplayDigest&)>;
    PluginPackageStore(std::filesystem::path directory, Register registerTheme, RegisterScript registerScript = {},
                       RegisterGameplay registerGameplay = {});
    void discover();
    bool install(const std::filesystem::path& source);
    // Called after discover, before PluginManager::initialize. A per-id receipt remembers removal;
    // an existing user package is never replaced, and install only registers (does not enable).
    bool seedDefault(const std::filesystem::path& source, const std::string& id);
    bool uninstall(const std::string& id);
    // Replaces an installed gameplay ZIP with a validated package of the same id. `unregister` must remove the
    // old registration; on failure the previous ZIP is restored and registered again.
    bool replaceGameplay(const std::string& id, const std::filesystem::path& source,
                         const std::function<void()>& unregister);
    const std::string& error() const { return error_; }
    static bool parse(const std::string& text, ThemePackage& out, std::string& error);
    static bool parseScript(const std::string& text, ScriptPackage& out, std::string& error);
    static bool parseGameplayManifest(const std::string& text, GameplayPackage& out, std::string& error);
    static bool readZip(const std::string& data, ScriptPackage& out, std::string& error);
private:
    bool load(const std::filesystem::path& path, ThemePackage& out);
    bool loadZip(const std::filesystem::path& path, ScriptPackage& out);
    bool loadGameplayZip(const std::filesystem::path& path, GameplayPackage& out, GameplayDigest& digest);
    std::filesystem::path dir_;
    Register register_;
    RegisterScript registerScript_;
    RegisterGameplay registerGameplay_;
    std::string error_;
};

}  // namespace sf
