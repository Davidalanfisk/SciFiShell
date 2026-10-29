#pragma once
// Data behind the Tron-style city: live system stats (CPU / RAM / GPU) plus a background scan and
// change watcher for every drive. The scene (City) only reads thread-safe snapshots of this.
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace sf {

// A first-level folder of a drive is a city block; its immediate subfolders are the buildings.
// Special building names: "(FILES)" = files directly inside the block, "(MORE)" = subfolders beyond kMaxSubs.
struct CityFolder {
    std::string name;  // first-level folder of the drive ("(ROOT FILES)" is index 0)
    uint64_t files = 0, bytes = 0;
    bool done = false;
    std::vector<CityFolder> subs;
};

struct CityDrive {
    bool listed = false;  // first-level listing finished; the drive can be shown
    std::string root, name;
    uint64_t total = 0, freeBytes = 0;
    uint64_t files = 0, bytes = 0;  // scanned so far
    bool done = false;
    std::vector<CityFolder> folders;
};

// Aggregated filesystem activity since the last takeChanges().
struct CityChange {
    int drive = 0, folder = 0, sub = -1;
    int added = 0, removed = 0, modified = 0;
};

struct SystemStats {
    int cores = 1;
    std::string cpuName, gpuName;
    float cpuLoad = 0;
    std::vector<float> coreLoad;
    uint64_t ramTotal = 0, ramUsed = 0;
    uint64_t vramTotal = 0;
    float vramUsed = -1;  // 0..1, -1 = unknown
    float gpuLoad = -1;   // 0..1, -1 = unknown
};

class CityData {
public:
    CityData();
    ~CityData();
    static constexpr size_t kMaxSubs = 256;  // buildings per block
    void start();
    void stop();

    // Copies the drive table when it changed since `version`. `ready` is true once every drive has been listed.
    bool snapshot(std::vector<CityDrive>& out, uint64_t& version, bool& ready);
    void takeChanges(std::vector<CityChange>& out);

    // Main thread: refreshes the system stats (rate limited internally).
    void pollStats(double dt);
    const SystemStats& stats() const { return stats_; }

private:
    struct PdhState;
    void scanDrive(int slot, std::string root);
    void watchDrive(int slot, std::string root);
    void publishFolder(int slot, size_t folder, uint64_t files, uint64_t bytes, bool done);
    void publishSub(int slot, size_t folder, size_t sub, uint64_t files, uint64_t bytes, bool done);
    void onChange(int slot, const std::wstring& rel, unsigned long action);
    void sleepMs(int ms);
    void saveCache();
    bool loadCache(const std::vector<std::string>& roots);

    std::mutex mu_;
    std::vector<CityDrive> drives_;
    std::vector<std::map<std::string, size_t>> folderIndex_;
    std::map<std::pair<int, int>, std::map<std::string, size_t>> subIndex_;  // (drive, folder) -> sub name -> index
    std::map<std::tuple<int, int, int>, CityChange> changes_;
    std::atomic<uint64_t> version_{1};
    std::atomic<bool> stop_{false};
    std::atomic<int> pending_{0};
    bool started_ = false;
    std::vector<std::thread> threads_;
    std::vector<void*> watchHandles_;  // HANDLEs, guarded by mu_

    SystemStats stats_;
    double statAccum_ = 10.0;
    bool statInit_ = false;
    std::unique_ptr<PdhState> pdh_;
};

}  // namespace sf
