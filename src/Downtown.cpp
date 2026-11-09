#include "Downtown.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace sf {
namespace {
bool validId(const std::string& id) {
    if (id.empty() || id.size() > 160) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    return true;
}
}
bool Downtown::load() {
    writable_ = false;
    placements_.clear();
    dirty_ = false;
    error_.clear();
    if (path_.empty()) { error_ = "Downtown preferences path is unavailable"; return false; }
    std::error_code ec;
    const bool exists = std::filesystem::exists(path_, ec);
    if (ec) { error_ = "Cannot inspect Downtown preferences: " + ec.message(); return false; }
    if (exists) {
        const auto size = std::filesystem::file_size(path_, ec);
        if (ec || size > 32768) { error_ = "Downtown preferences are unreadable or exceed 32 KiB"; return false; }
        std::ifstream in(path_);
        if (!in) { error_ = "Cannot read Downtown preferences"; return false; }
        std::map<std::string, int> loaded;
        std::set<int> used;
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream row(line);
            std::string id, extra;
            int slot = -1;
            if (!(row >> id >> slot) || (row >> extra) || !validId(id) ||
                slot < 0 || slot >= kDowntownSlots || !used.insert(slot).second ||
                !loaded.emplace(id, slot).second) {
                error_ = "Invalid Downtown preferences; original file preserved";
                return false;
            }
        }
        if (in.bad()) { error_ = "Cannot finish reading Downtown preferences"; return false; }
        placements_ = std::move(loaded);
    }
    writable_ = true;
    return true;
}

bool Downtown::save(const std::map<std::string, int>& next) {
    if (!writable_) return false;
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) { error_ = "Cannot create Downtown directory: " + ec.message(); return false; }
    std::filesystem::path temporary;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto candidate = path_.parent_path() / ("downtown-tmp-" + std::to_string(nonce) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, ec)) { temporary = candidate; break; }
        if (ec) { error_ = "Cannot create temporary Downtown directory: " + ec.message(); return false; }
    }
    if (temporary.empty()) { error_ = "Cannot create unique Downtown temporary directory"; return false; }
    const auto file = temporary / "state";
    auto cleanup = [&] {
        std::error_code failure;
        std::filesystem::remove(file, failure);
        if (failure) std::fprintf(stderr, "Downtown cleanup: %s\n", failure.message().c_str());
        std::filesystem::remove(temporary, failure);
        if (failure) std::fprintf(stderr, "Downtown cleanup: %s\n", failure.message().c_str());
    };
    {
        std::ofstream out(file);
        for (const auto& [id, slot] : next) out << id << ' ' << slot << '\n';
        out.close();
        if (!out) { error_ = "Cannot write Downtown preferences"; cleanup(); return false; }
    }
#ifdef _WIN32
    if (!MoveFileExW(file.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error_ = "Cannot replace Downtown preferences: Windows error " + std::to_string(GetLastError());
        cleanup(); return false;
    }
#else
    std::filesystem::rename(file, path_, ec);
    if (ec) { error_ = "Cannot replace Downtown preferences: " + ec.message(); cleanup(); return false; }
#endif
    cleanup();
    placements_ = next;
    dirty_ = false;
    error_.clear();
    return true;
}

bool Downtown::reconcile(const std::set<std::string>& installed) {
    auto next = placements_;
    for (auto it = next.begin(); it != next.end();)
        if (!installed.count(it->first)) it = next.erase(it); else ++it;
    std::set<int> used;
    for (const auto& [id, slot] : next) used.insert(slot);
    for (const auto& id : installed) {
        if (!validId(id)) { error_ = "Invalid Downtown plugin ID"; return false; }
        if (next.count(id)) continue;
        int slot = 0;
        while (slot < kDowntownSlots && used.count(slot)) ++slot;
        if (slot == kDowntownSlots) { error_ = "Downtown has no available plots"; return false; }
        next.emplace(id, slot);
        used.insert(slot);
    }
    if (!writable_) {
        // Keep host management accessible in a clearly reported, read-only recovery layout.
        placements_ = std::move(next);
        return false;
    }
    if (next == placements_ && !dirty_) { error_.clear(); return true; }
    if (save(next)) return true;
    // Installation/removal has already happened; reconcile the live city even if persistence failed.
    placements_ = std::move(next);
    dirty_ = true;
    return false;
}

bool Downtown::move(const std::string& id, int slot) {
    if (!writable_) return false;
    auto next = placements_;
    const auto source = next.find(id);
    if (source == next.end() || slot < 0 || slot >= kDowntownSlots) {
        error_ = "Downtown move references an unavailable plugin or plot"; return false;
    }
    const int old = source->second;
    for (auto& [other, position] : next)
        if (other != id && position == slot) { position = old; break; }
    source->second = slot;
    return save(next);
}
}
