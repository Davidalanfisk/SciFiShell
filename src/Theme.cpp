#include "Theme.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace sf {

namespace {
const Theme kThemes[] = {
    {"CYAN", {0.05f, 0.80f, 1.0f}, {1.0f, 0.62f, 0.10f}},
    {"AMBER", {1.0f, 0.55f, 0.08f}, {0.15f, 0.80f, 1.0f}},
    {"MAGENTA", {1.0f, 0.18f, 0.72f}, {0.25f, 0.90f, 1.0f}},
};
constexpr int kThemeCount = static_cast<int>(sizeof(kThemes) / sizeof(kThemes[0]));

std::filesystem::path settingsFile() {
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / "SciFiShell" / "theme.ini";
}

int& index() {
    static int idx = [] {
        int v = 0;
        auto p = settingsFile();
        if (!p.empty()) {
            std::ifstream f(p);
            if (f) f >> v;
        }
        return (v >= 0 && v < kThemeCount) ? v : 0;
    }();
    return idx;
}
}  // namespace

const Theme& currentTheme() { return kThemes[index()]; }

void cycleTheme() {
    index() = (index() + 1) % kThemeCount;
    auto p = settingsFile();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream(p) << index();
}

}  // namespace sf
