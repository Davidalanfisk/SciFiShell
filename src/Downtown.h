#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace sf {
// +Z is south. Reserve this corridor before packing drives or routing their buses.
struct DowntownPlot { float x, z; };
inline constexpr int kDowntownSlots = 128; // Room for the package store's 64 packages and user-selected spare plots.
inline constexpr float kDowntownWest = -80, kDowntownEast = 80;
inline constexpr float kDowntownNorth = 104, kDowntownSouth = 1152;
struct DowntownRegion { float x, z, width, depth; };
inline constexpr DowntownRegion downtownRegion() {
    return {(kDowntownWest+kDowntownEast)*0.5f, (kDowntownNorth+kDowntownSouth)*0.5f,
            kDowntownEast-kDowntownWest, kDowntownSouth-kDowntownNorth};
}
inline DowntownPlot downtownPlot(int slot) {
    return {-48.0f + (slot % 4) * 32.0f, 128.0f + (slot / 4) * 32.0f};
}
inline bool downtownIntersects(float x, float z, float half, float clearance = 0) {
    return x + half + clearance > kDowntownWest && x - half - clearance < kDowntownEast &&
           z + half + clearance > kDowntownNorth && z - half - clearance < kDowntownSouth;
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
