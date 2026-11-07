#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace sf {

struct ThemePackage {
    std::string id, name, version, author, description, accent, alt;
};

class PluginPackageStore {
public:
    using Register = std::function<bool(const ThemePackage&)>;
    PluginPackageStore(std::filesystem::path directory, Register registerTheme);
    void discover();
    bool install(const std::filesystem::path& source);
    const std::string& error() const { return error_; }
    static bool parse(const std::string& text, ThemePackage& out, std::string& error);
private:
    bool load(const std::filesystem::path& path, ThemePackage& out);
    std::filesystem::path dir_;
    Register register_;
    std::string error_;
};

}  // namespace sf
