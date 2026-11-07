#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace sf {

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
    PluginPackageStore(std::filesystem::path directory, Register registerTheme, RegisterScript registerScript = {});
    void discover();
    bool install(const std::filesystem::path& source);
    bool uninstall(const std::string& id);
    const std::string& error() const { return error_; }
    static bool parse(const std::string& text, ThemePackage& out, std::string& error);
    static bool parseScript(const std::string& text, ScriptPackage& out, std::string& error);
    static bool readZip(const std::string& data, ScriptPackage& out, std::string& error);
private:
    bool load(const std::filesystem::path& path, ThemePackage& out);
    bool loadZip(const std::filesystem::path& path, ScriptPackage& out);
    std::filesystem::path dir_;
    Register register_;
    RegisterScript registerScript_;
    std::string error_;
};

}  // namespace sf
