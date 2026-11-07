#include "Theme.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace sf {

namespace {
const Theme kThemes[] = {
    {"CYAN", {0.05f, 0.80f, 1.0f}, {1.0f, 0.62f, 0.10f}},
    {"AMBER", {1.0f, 0.55f, 0.08f}, {0.15f, 0.80f, 1.0f}},
    {"MAGENTA", {1.0f, 0.18f, 0.72f}, {0.25f, 0.90f, 1.0f}},
};
std::filesystem::path settingsFile() {
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / "SciFiShell" / "theme.ini";
}

std::string& selection() {
    static std::string selected = [] {
        std::string v = "0";
        auto p = settingsFile();
        if (!p.empty()) {
            std::ifstream f(p);
            if (f) std::getline(f, v);
        }
        if (v == "1" || v == "2" || v.rfind("plugin:", 0) == 0) return v;
        return std::string("0");
    }();
    return selected;
}

void saveSelection() {
    auto p = settingsFile();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream(p) << selection();
}

struct PluginTheme {
    std::string id, name;
    Theme theme;
    PluginTheme(const std::string& i, const std::string& n, Vec3 a, Vec3 b)
        : id(i), name(n), theme{name.c_str(), a, b} {}
};
std::vector<std::unique_ptr<PluginTheme>>& pluginThemes() {
    static std::vector<std::unique_ptr<PluginTheme>> themes;
    return themes;
}
}  // namespace

const Theme& currentTheme() {
    const auto& id = selection();
    if (id == "1") return kThemes[1];
    if (id == "2") return kThemes[2];
    for (const auto& theme : pluginThemes()) if (id == "plugin:" + theme->id) return theme->theme;
    return kThemes[0];
}

std::string currentThemeKey() {
    const auto& id = selection();
    if (id == "1" || id == "2") return id;
    for (const auto& theme : pluginThemes()) if (id == "plugin:" + theme->id) return id;
    return "0";
}

void cycleTheme() {
    std::vector<std::string> ids{"0", "1", "2"};
    for (const auto& theme : pluginThemes()) ids.push_back("plugin:" + theme->id);
    size_t i = 0;
    for (; i < ids.size(); ++i) if (ids[i] == currentThemeKey()) break;
    selection() = ids[(i + 1) % ids.size()];
    saveSelection();
}

void registerPluginTheme(const std::string& id, const std::string& name, Vec3 accent, Vec3 alt) {
    for (const auto& t : pluginThemes()) if (t->id == id) return;
    pluginThemes().push_back(std::make_unique<PluginTheme>(id, name, accent, alt));
}

void unregisterPluginTheme(const std::string& id) {
    auto& themes = pluginThemes();
    for (auto it = themes.begin(); it != themes.end(); ++it) {
        if ((*it)->id != id) continue;
        themes.erase(it);
        return;
    }
}

}  // namespace sf
