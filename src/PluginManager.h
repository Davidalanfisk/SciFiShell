#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sf {

struct Plugin {
    std::string id, name, description, version;
    std::function<void()> start, stop;
    bool enabled = true;
    bool running = false;
};

// Trusted, statically linked plugins only. No external code is loaded.
class PluginManager {
public:
    explicit PluginManager(std::filesystem::path statePath);
    bool add(Plugin plugin);
    bool initialize();
    bool setEnabled(const std::string& id, bool enabled);
    void shutdown();
    const std::vector<Plugin>& plugins() const { return plugins_; }
    const std::string& error() const { return error_; }
    static std::filesystem::path defaultStatePath();

private:
    bool save(const std::map<std::string, bool>& state);
    std::filesystem::path path_;
    std::vector<Plugin> plugins_;
    std::map<std::string, bool> state_;
    std::string error_;
    bool initialized_ = false, storageReady_ = false, stopped_ = false;
};

}  // namespace sf
