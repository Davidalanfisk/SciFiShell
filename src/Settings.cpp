#include "Settings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace sf {

namespace {
std::filesystem::path file() {
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base || !*base) return {};
    return std::filesystem::path(base) / "SciFiShell" / "settings.ini";
}
}  // namespace

void Settings::load() {
    auto p = file();
    if (p.empty()) return;
    std::ifstream f(p);
    int inv = 0, ch = 1;
    if (f >> lookIdx >> speedIdx >> inv >> ch) {
        invertY = inv != 0;
        crosshair = ch != 0;
        int fps = 0;
        if (f >> fps) showFps = fps != 0;
        f >> webQuality;
    }
    webQuality = std::clamp(webQuality, 0, 3);
    lookIdx = std::clamp(lookIdx, 0, kSteps - 1);
    speedIdx = std::clamp(speedIdx, 0, kSteps - 1);
}

void Settings::save() const {
    auto p = file();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream(p) << lookIdx << ' ' << speedIdx << ' ' << (invertY ? 1 : 0) << ' ' << (crosshair ? 1 : 0) << ' ' << (showFps ? 1 : 0) << ' ' << webQuality;
}

}  // namespace sf
