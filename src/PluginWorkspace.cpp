#include "PluginWorkspace.h"

#include "GameplayArchive.h"
#include "GameplayLimits.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "miniz.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace sf {
namespace {
namespace fs = std::filesystem;
constexpr size_t kWorkspaceVisitLimit = 512;
constexpr int kWorkspaceDepthLimit = 8;

FILE* openFile(const fs::path& path, bool write) {
    FILE* file = nullptr;
#ifdef _WIN32
    _wfopen_s(&file, path.c_str(), write ? L"wbx" : L"rb");
#else
    file = std::fopen(path.c_str(), write ? "wbx" : "rb");
#endif
    return file;
}

bool writeNew(const fs::path& path, const std::string& bytes, std::string& error) {
    std::unique_ptr<FILE, decltype(&std::fclose)> file(openFile(path, true), std::fclose);
    if (!file) { error = "Cannot create workspace file"; return false; }
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
        error = "Cannot write workspace file"; return false;
    }
    if (std::fclose(file.release()) != 0) { error = "Cannot finish workspace file"; return false; }
    return true;
}

// Only plain directories are traversed; a junction or symlink (MSVC reports junctions as their own type) is refused.
bool plainDirectory(const fs::path& path, std::error_code& ec) {
    const auto status = fs::symlink_status(path, ec);
    return !ec && status.type() == fs::file_type::directory;
}

void removeTree(const fs::path& path) {
    std::error_code ec;
    if (plainDirectory(path, ec)) fs::remove_all(path, ec);
    else if (!ec) fs::remove(path, ec);
}
}

bool extractGameplayWorkspace(const fs::path& zip, const fs::path& workspace, bool& created, std::string& error) {
    created = false;
    std::error_code ec;
    if (fs::exists(fs::symlink_status(workspace, ec))) {
        if (!plainDirectory(workspace, ec)) { error = "Plugin workspace is not a plain folder"; return false; }
        return true;
    }
    fs::create_directories(workspace.parent_path(), ec);
    if (ec || !plainDirectory(workspace.parent_path(), ec)) { error = "Cannot create plugin workspace folder"; return false; }
    const fs::path staging = workspace.parent_path() / (workspace.filename().native() + fs::path(".extracting").native());
    removeTree(staging);
    if (!fs::create_directory(staging, ec) || ec) { error = "Cannot create plugin workspace staging folder"; return false; }
    auto store = [&](const std::string& name, const std::string& bytes, std::string& writeError) {
        // Names were validated by the package reader: fixed text paths or assets/<lowercase safe segments>.
        const fs::path target = staging / fs::path(name);
        std::error_code dirError;
        fs::create_directories(target.parent_path(), dirError);
        if (dirError) { writeError = "Cannot create workspace subfolder"; return false; }
        return writeNew(target, bytes, writeError);
    };
    const bool ok = readGameplayArchiveFile(zip,
        [&](const std::string& manifest, const std::string& script, std::string& e) {
            return store("plugin.json", manifest, e) && store("scripts/main.lua", script, e);
        },
        store, error);
    if (!ok) { removeTree(staging); return false; }
    fs::rename(staging, workspace, ec);
    if (ec) { removeTree(staging); error = "Cannot publish plugin workspace: " + ec.message(); return false; }
    created = true;
    error.clear();
    return true;
}

bool packGameplayWorkspace(const fs::path& workspace, const fs::path& zip, std::string& error) {
    struct File { std::string name; fs::path path; uintmax_t size; };
    std::vector<File> files;
    size_t visited = 0;
    uintmax_t total = 0;
    std::error_code ec;
    if (!plainDirectory(workspace, ec)) { error = "Plugin workspace is missing or not a plain folder"; return false; }
    try {
        std::function<bool(const fs::path&, const std::string&, int)> walk =
            [&](const fs::path& dir, const std::string& prefix, int depth) {
            fs::directory_iterator it(dir, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                if (++visited > kWorkspaceVisitLimit) { error = "Plugin workspace has too many entries"; return false; }
                const auto status = it->symlink_status(ec);
                if (ec) break;
                const auto u8 = it->path().filename().u8string();
                const std::string leaf(u8.begin(), u8.end());
                const std::string name = prefix + leaf;
                if (status.type() == fs::file_type::directory) {
                    if (depth >= kWorkspaceDepthLimit || !walk(it->path(), name + "/", depth + 1)) {
                        if (error.empty()) error = "Plugin workspace folders are nested too deeply";
                        return false;
                    }
                    continue;
                }
                if (status.type() != fs::file_type::regular) {
                    error = "Links, junctions and special files are not packaged: " + name; return false;
                }
                const bool text = name == "plugin.json" || name == "scripts/main.lua";
                if (!text && name.rfind("assets/", 0) != 0) {
                    error = "Unexpected workspace file (only plugin.json, scripts/main.lua, assets/): " + name;
                    return false;
                }
                const auto size = it->file_size(ec);
                if (ec) break;
                if (size == 0 || size > (text ? kGameplayTextLimit : kGameplayAssetLimit) ||
                    size > kGameplayArchiveExpandedLimit - total || files.size() >= kGameplayEntryLimit) {
                    error = "Workspace file is empty or exceeds package limits: " + name; return false;
                }
                total += size;
                files.push_back({name, it->path(), size});
            }
            if (ec) { error = "Cannot read plugin workspace: " + ec.message(); return false; }
            return true;
        };
        if (!walk(workspace, "", 0)) return false;
    } catch (const std::exception&) {
        error = "Plugin workspace contains names that cannot be packaged"; return false;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> out(openFile(zip, true), std::fclose);
    if (!out) { error = "Cannot create packaged plugin ZIP"; return false; }
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_cfile(&archive, out.get(), 0)) { error = "Cannot start plugin ZIP"; return false; }
    bool ok = true;
    try {
        for (const auto& file : files) {
            std::string bytes(static_cast<size_t>(file.size), '\0');
            std::unique_ptr<FILE, decltype(&std::fclose)> in(openFile(file.path, false), std::fclose);
            if (!in || std::fread(bytes.data(), 1, bytes.size(), in.get()) != bytes.size() ||
                std::fgetc(in.get()) != EOF) {
                error = "Workspace file changed while packaging: " + file.name; ok = false; break;
            }
            if (!mz_zip_writer_add_mem(&archive, file.name.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_LEVEL)) {
                error = "Cannot add workspace file to ZIP: " + file.name; ok = false; break;
            }
        }
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate workspace file for packaging"; ok = false;
    }
    if (ok && !mz_zip_writer_finalize_archive(&archive)) { error = "Cannot finish plugin ZIP"; ok = false; }
    mz_zip_writer_end(&archive);
    if (std::fclose(out.release()) != 0 && ok) { error = "Cannot write plugin ZIP"; ok = false; }
    if (!ok) { std::error_code cleanup; fs::remove(zip, cleanup); return false; }
    error.clear();
    return true;
}

bool savePluginSettings(const fs::path& file, const std::vector<std::pair<std::string, std::string>>& values,
                        std::string& error) {
    std::string bytes;
    for (const auto& [key, value] : values) bytes += key + '=' + value + '\n';
    if (values.size() > 8 || bytes.size() > 4096) { error = "Settings too large"; return false; }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec) { error = "Cannot create settings folder: " + ec.message(); return false; }
    fs::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !out.flush()) {
            out.close();
            fs::remove(temp, ec);
            error = "Cannot write settings";
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace settings (Windows error " + std::to_string(GetLastError()) + ")";
        fs::remove(temp, ec);
        return false;
    }
#else
    fs::rename(temp, file, ec);
    if (ec) { error = "Cannot replace settings: " + ec.message(); fs::remove(temp, ec); return false; }
#endif
    error.clear();
    return true;
}

std::map<std::string, std::string> loadPluginSettings(const fs::path& file) {
    std::map<std::string, std::string> stored;
    std::error_code ec;
    if (fs::file_size(file, ec) > 4096 || ec) return stored;
    std::ifstream in(file, std::ios::binary);
    for (std::string line; std::getline(in, line) && stored.size() < 8;) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq != std::string::npos) stored[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return stored;
}

}  // namespace sf