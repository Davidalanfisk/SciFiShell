#pragma once

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace sf {
// +Z is south. Plot coordinates stay stable while the occupied district grows and shrinks.
struct DowntownPlot { float x, z; };
inline constexpr int kDowntownSlots = 128; // Includes spare user-selected plots and the two bundled plugins.
inline constexpr const char* kDowntownDrivingId = "org.scifishell.driving";
inline constexpr const char* kDowntownParksId = "org.scifishell.parks";
inline bool downtownBuiltin(const std::string& id) {
    return id == kDowntownDrivingId || id == kDowntownParksId;
}
struct DowntownRegion {
    float x = 0, z = 124, width = 32, depth = 40;
    float west() const { return x-width*0.5f; }
    float east() const { return x+width*0.5f; }
    float north() const { return z-depth*0.5f; }
    float south() const { return z+depth*0.5f; }
    bool operator==(const DowntownRegion& other) const {
        return x == other.x && z == other.z && width == other.width && depth == other.depth;
    }
    bool intersects(float cx, float cz, float half, float clearance = 0) const {
        return cx+half+clearance > west() && cx-half-clearance < east() &&
               cz+half+clearance > north() && cz-half-clearance < south();
    }
};
inline DowntownPlot downtownPlot(int slot, bool north = false) {
    if (north) return {-96.0f+(slot%4)*64.0f,-164.0f-(slot/4)*64.0f};
    return {-48.0f + (slot % 4) * 32.0f, 128.0f+(slot/4)*32.0f};
}
inline DowntownRegion downtownRegion(const std::map<std::string, int>& plots = {}, bool northern = false) {
    if (plots.empty()) return northern ? DowntownRegion{0,-164,64,80} : DowntownRegion{};
    float west = 1e30f, east = -1e30f, north = 1e30f, south = -1e30f;
    const float margin = northern ? 40.0f : 20.0f;
    for (const auto& [id, slot] : plots) {
        const auto p = downtownPlot(slot, northern);
        west = std::min(west, p.x-margin); east = std::max(east, p.x+margin);
        north = std::min(north, p.z-margin); south = std::max(south, p.z+margin);
    }
    return {(west+east)*0.5f, (north+south)*0.5f, east-west, south-north};
}

class Downtown {
public:
    explicit Downtown(std::filesystem::path path) : path_(std::move(path)) {}
    bool load();
    bool reconcile(const std::set<std::string>& installed);
    bool move(const std::string& id, int slot); // Occupied target swaps with the source.
    const std::map<std::string, int>& placements() const { return placements_; }
    const std::string& error() const { return error_; }
private:
    bool save(const std::map<std::string, int>& next);
    std::filesystem::path path_;
    std::map<std::string, int> placements_;
    std::string error_;
    bool writable_ = false;
    bool dirty_ = false;
};
}
