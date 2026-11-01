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
        int r = 1, p = 1, v = 1;
        if (f >> aaIdx >> distIdx >> r >> p >> v) {
            reflections = r != 0;
            postFx = p != 0;
            vsync = v != 0;
            int cw = 1, sw = 1;
            if (f >> cw >> sw) {
                carryWins = cw != 0;
                snapWins = sw != 0;
                int ak = 0;
                if (f >> ak) arrangeKey = ak;
            }
        }
    }
    aaIdx = std::clamp(aaIdx, 0, 2);
    distIdx = std::clamp(distIdx, 0, 3);
    webQuality = std::clamp(webQuality, 0, 3);
    lookIdx = std::clamp(lookIdx, 0, kSteps - 1);
    speedIdx = std::clamp(speedIdx, 0, kSteps - 1);
}

void Settings::save() const {
    auto p = file();
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream(p) << lookIdx << ' ' << speedIdx << ' ' << (invertY ? 1 : 0) << ' ' << (crosshair ? 1 : 0) << ' ' << (showFps ? 1 : 0) << ' ' << webQuality << ' ' << aaIdx << ' ' << distIdx << ' ' << (reflections ? 1 : 0) << ' ' << (postFx ? 1 : 0) << ' ' << (vsync ? 1 : 0) << ' ' << (carryWins ? 1 : 0) << ' ' << (snapWins ? 1 : 0) << ' ' << arrangeKey;
}

}  // namespace sf
