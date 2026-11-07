#include "PluginManager.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sf {

namespace {
bool validId(const std::string& id) {
    if (id.empty()) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    return true;
}
}

PluginManager::PluginManager(std::filesystem::path statePath) : path_(std::move(statePath)) {}

std::filesystem::path PluginManager::defaultStatePath() {
#ifdef _WIN32
    const char* base = std::getenv("LOCALAPPDATA");
    return base && *base ? std::filesystem::path(base) / "SciFiShell" / "plugins.ini" : std::filesystem::path{};
#else
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) / ".config" / "scifishell" / "plugins.ini" : std::filesystem::path{};
#endif
}

bool PluginManager::add(Plugin plugin) {
    if (stopped_ || (initialized_ && !storageReady_) || !validId(plugin.id) || !plugin.start || !plugin.stop) return false;
    for (const auto& p : plugins_) if (p.id == plugin.id) return false;
    plugin.running = false;
    if (initialized_) {
        const auto it = state_.find(plugin.id);
        if (it != state_.end()) plugin.enabled = it->second;
        if (plugin.enabled) { plugin.start(); plugin.running = true; }
    }
    plugins_.push_back(std::move(plugin));
    return true;
}

bool PluginManager::initialize() {
    if (initialized_) return false;
    initialized_ = true;
    storageReady_ = false;
    if (path_.empty()) error_ = "Plugin preferences path is unavailable";
    else {
        std::error_code ec;
        const bool exists = std::filesystem::exists(path_, ec);
        if (ec) error_ = "Cannot inspect plugin preferences: " + ec.message();
        else if (!exists) storageReady_ = true;
        else {
            std::ifstream in(path_);
            if (!in) error_ = "Cannot read plugin preferences";
            else {
                std::map<std::string, bool> loaded;
                std::string line;
                bool valid = true;
                while (std::getline(in, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    const auto pos = line.find('=');
                    if (pos == std::string::npos || !validId(line.substr(0, pos)) ||
                        (line.substr(pos + 1) != "0" && line.substr(pos + 1) != "1") ||
                        !loaded.emplace(line.substr(0, pos), line.substr(pos + 1) == "1").second) {
                        valid = false;
                        break;
                    }
                }
                if (!valid || in.bad()) error_ = "Plugin preferences are invalid or unreadable; file was not changed";
                else { state_ = std::move(loaded); storageReady_ = true; }
            }
        }
    }
    if (!error_.empty()) std::fprintf(stderr, "%s\n", error_.c_str());
    for (auto& p : plugins_) {
        const auto it = state_.find(p.id);
        if (it != state_.end()) p.enabled = it->second;
        if (p.enabled) { p.start(); p.running = true; }
    }
    return storageReady_;
}

bool PluginManager::save(const std::map<std::string, bool>& state) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) { error_ = "Cannot create plugin preferences directory: " + ec.message(); return false; }
    std::filesystem::path tempDir;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 32; ++attempt) {
        tempDir = path_.parent_path() / ("plugins-tmp-" + std::to_string(nonce) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(tempDir, ec)) break;
        if (ec) { error_ = "Cannot create temporary plugin preferences: " + ec.message(); return false; }
        tempDir.clear();
    }
    if (tempDir.empty()) { error_ = "Cannot create unique temporary plugin preferences"; return false; }
    const auto temp = tempDir / "state";
    auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        if (ignored) std::fprintf(stderr, "Cannot remove temporary plugin preferences: %s\n", ignored.message().c_str());
        ignored.clear();
        std::filesystem::remove(tempDir, ignored);
        if (ignored) std::fprintf(stderr, "Cannot remove temporary plugin directory: %s\n", ignored.message().c_str());
    };
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) { error_ = "Cannot write plugin preferences"; cleanup(); return false; }
        for (const auto& [id, enabled] : state) out << id << '=' << (enabled ? '1' : '0') << '\n';
        out.close();
        if (!out) { error_ = "Cannot finish writing plugin preferences"; cleanup(); return false; }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error_ = "Cannot replace plugin preferences (Windows error " + std::to_string(GetLastError()) + ")";
        cleanup();
        return false;
    }
#else
    std::filesystem::rename(temp, path_, ec);
    if (ec) { error_ = "Cannot replace plugin preferences: " + ec.message(); cleanup(); return false; }
#endif
    cleanup();
    return true;
}

bool PluginManager::setEnabled(const std::string& id, bool enabled) {
    if (!initialized_ || stopped_ || !storageReady_) return false;
    for (auto& p : plugins_) {
        if (p.id != id) continue;
        if (p.enabled == enabled) return true;
        auto next = state_;
        next[id] = enabled;
        if (!save(next)) { std::fprintf(stderr, "%s\n", error_.c_str()); return false; }
        state_ = std::move(next);
        error_.clear();
        p.enabled = enabled;
        if (enabled) { p.start(); p.running = true; }
        else { p.stop(); p.running = false; }
        return true;
    }
    return false;
}

void PluginManager::shutdown() {
    stopped_ = true;
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it)
        if (it->running) { it->stop(); it->running = false; }
}

}  // namespace sf
