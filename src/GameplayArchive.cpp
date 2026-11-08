#include "GameplayArchive.h"
#include "GameplayLimits.h"

#include <cstring>
#include <new>
#include <set>

#include "miniz.h"

namespace sf {
namespace {
bool assetName(const std::string& path) {
    constexpr char prefix[] = "assets/";
    if (path.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
    const auto name = path.substr(sizeof(prefix) - 1);
    if (name.empty() || name.size() > 80 || name.front() == '.' || name.find("..") != std::string::npos)
        return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
    const auto dot = name.rfind('.');
    return dot != std::string::npos && (name.substr(dot) == ".png" || name.substr(dot) == ".glb");
}
}

bool readGameplayArchive(const std::string& data, GameplayArchive& out, std::string& error) {
    out = {};
    if (data.empty() || data.size() > kGameplayArchiveLimit) { error = "Gameplay archive is empty or too large"; return false; }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) { error = "Invalid gameplay ZIP"; return false; }
    struct ZipGuard {
        mz_zip_archive* zip;
        ~ZipGuard() { mz_zip_reader_end(zip); }
    } guard{&zip};
    GameplayArchive loaded;
    auto done = [&](const std::string& message) {
        error = message;
        if (message.empty()) out = std::move(loaded);
        return message.empty();
    };
    try {
        const auto count = mz_zip_reader_get_num_files(&zip);
        if (count < 2 || count > kGameplayEntryLimit) return done("Invalid gameplay ZIP entry count");
        size_t total = 0;
        std::set<std::string> seen;
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) return done("Invalid gameplay ZIP entry");
            char name[256]{};
            const auto length = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
            if (!length || length > sizeof(name) ||
                mz_zip_reader_get_filename(&zip, i, name, sizeof(name)) != length || std::strlen(name) + 1 != length)
                return done("Invalid gameplay ZIP entry name");
            const std::string path(name);
            const bool manifest = path == "plugin.json";
            const bool script = path == "scripts/main.lua";
            if ((!manifest && !script && !assetName(path)) || !seen.insert(path).second)
                return done("Unexpected or duplicate gameplay ZIP path");
            const auto type = (stat.m_external_attr >> 16) & 0170000;
            const size_t limit = (manifest || script) ? kGameplayTextLimit : kGameplayAssetLimit;
            if ((type && type != 0100000) || stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported ||
                stat.m_uncomp_size == 0 || stat.m_uncomp_size > limit || stat.m_uncomp_size > kGameplayArchiveExpandedLimit - total)
                return done("Unsupported gameplay ZIP entry type or size");
            total += static_cast<size_t>(stat.m_uncomp_size);
            std::string contents(static_cast<size_t>(stat.m_uncomp_size), '\0');
            if (!mz_zip_reader_extract_to_mem(&zip, i, contents.data(), contents.size(), 0))
                return done("Cannot extract gameplay ZIP entry or CRC mismatch");
            if (manifest) loaded.manifest = std::move(contents);
            else if (script) loaded.script = std::move(contents);
            else loaded.assets.emplace(path, std::move(contents));
        }
        if (loaded.manifest.empty() || loaded.script.empty()) return done("Gameplay ZIP is missing required files");
        return done({});
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay archive";
        return false;
    }
}
}  // namespace sf
