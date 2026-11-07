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
    std::function<bool(std::string&)> prepare;
    std::function<void()> cancelPrepare;
};

// Built-in plugins are trusted; external script callbacks must enforce their own runtime limits.
class PluginManager {
public:
    explicit PluginManager(std::filesystem::path statePath);
    bool add(Plugin plugin);
    bool initialize();
    bool setEnabled(const std::string& id, bool enabled);
    bool remove(const std::string& id);
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
