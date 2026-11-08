#include "GameplayArchive.h"
#include "GameplayLimits.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <vector>

#include "miniz.h"

namespace sf {
namespace {
struct ZipBudget { size_t used = 0; };
struct alignas(std::max_align_t) ZipAllocation { size_t size; };
constexpr size_t kZipMetadataLimit = 4 * 1024 * 1024;
void* zipAlloc(void* opaque, size_t items, size_t size) {
    auto& budget = *static_cast<ZipBudget*>(opaque);
    if (size && items > std::numeric_limits<size_t>::max() / size) return nullptr;
    const size_t bytes = items * size;
    if (bytes > kZipMetadataLimit - budget.used || bytes > std::numeric_limits<size_t>::max() - sizeof(ZipAllocation))
        return nullptr;
    auto* allocation = static_cast<ZipAllocation*>(std::malloc(sizeof(ZipAllocation) + bytes));
    if (!allocation) return nullptr;
    allocation->size = bytes;
    budget.used += bytes;
    return allocation + 1;
}
void zipFree(void* opaque, void* pointer) {
    if (!pointer) return;
    auto* allocation = static_cast<ZipAllocation*>(pointer) - 1;
    static_cast<ZipBudget*>(opaque)->used -= allocation->size;
    std::free(allocation);
}
void* zipRealloc(void* opaque, void* pointer, size_t items, size_t size) {
    if (!pointer) return zipAlloc(opaque, items, size);
    if (!items || !size) { zipFree(opaque, pointer); return nullptr; }
    if (size && items > std::numeric_limits<size_t>::max() / size) return nullptr;
    auto& budget = *static_cast<ZipBudget*>(opaque);
    auto* previous = static_cast<ZipAllocation*>(pointer) - 1;
    const size_t bytes = items * size, old = previous->size;
    if ((bytes > old && bytes - old > kZipMetadataLimit - budget.used) ||
        bytes > std::numeric_limits<size_t>::max() - sizeof(ZipAllocation)) return nullptr;
    auto* allocation = static_cast<ZipAllocation*>(std::realloc(previous, sizeof(ZipAllocation) + bytes));
    if (!allocation) return nullptr;
    allocation->size = bytes;
    budget.used = budget.used - old + bytes;
    return allocation + 1;
}
void configureZip(mz_zip_archive& zip, ZipBudget& budget) {
    zip.m_pAlloc = zipAlloc;
    zip.m_pFree = zipFree;
    zip.m_pRealloc = zipRealloc;
    zip.m_pAlloc_opaque = &budget;
}

bool assetName(const std::string& path) {
    constexpr char prefix[] = "assets/";
    if (path.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
    const auto name = path.substr(sizeof(prefix) - 1);
    if (name.empty() || name.size() > 80 || name.front() == '.' || name.find("..") != std::string::npos)
        return false;
    bool segmentStart = true;
    for (char c : name) {
        if (c == '/') {
            if (segmentStart) return false;
            segmentStart = true;
        } else if (segmentStart && c == '.') {
            return false;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        } else {
            segmentStart = false;
        }
    }
    if (segmentStart) return false;
    const auto dot = name.rfind('.');
    return dot != std::string::npos && (name.substr(dot) == ".png" || name.substr(dot) == ".glb" ||
                                        name.substr(dot) == ".wav" || name.substr(dot) == ".ogg");
}

bool readArchive(mz_zip_archive& zip, const GameplayArchiveBegin& begin,
                 const GameplayArchiveAsset& asset, size_t expandedLimit, std::string& error) {
    struct Entry { mz_uint index; std::string path; size_t size; };
    try {
        const auto count = mz_zip_reader_get_num_files(&zip);
        if (count < 2 || count > kGameplayEntryLimit) { error = "Invalid gameplay ZIP entry count"; return false; }
        size_t total = 0;
        std::set<std::string> seen;
        std::vector<Entry> entries;
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat)) { error = "Invalid gameplay ZIP entry"; return false; }
            char name[256]{};
            const auto length = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
            if (!length || length > sizeof(name) ||
                mz_zip_reader_get_filename(&zip, i, name, sizeof(name)) != length || std::strlen(name) + 1 != length) {
                error = "Invalid gameplay ZIP entry name"; return false;
            }
            const std::string path(name);
            const bool text = path == "plugin.json" || path == "scripts/main.lua";
            if ((!text && !assetName(path)) || !seen.insert(path).second) {
                error = "Unexpected or duplicate gameplay ZIP path"; return false;
            }
            const auto type = (stat.m_external_attr >> 16) & 0170000;
            const size_t limit = text ? kGameplayTextLimit : kGameplayAssetLimit;
            if ((type && type != 0100000) || stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported ||
                stat.m_uncomp_size == 0 || stat.m_uncomp_size > limit ||
                stat.m_uncomp_size > expandedLimit - total) {
                error = "Unsupported gameplay ZIP entry type or size"; return false;
            }
            total += static_cast<size_t>(stat.m_uncomp_size);
            entries.push_back({i, path, static_cast<size_t>(stat.m_uncomp_size)});
        }
        if (!seen.count("plugin.json") || !seen.count("scripts/main.lua")) {
            error = "Gameplay ZIP is missing required files"; return false;
        }
        std::string manifest, script;
        for (const auto& entry : entries) {
            if (entry.path != "plugin.json" && entry.path != "scripts/main.lua") continue;
            std::string& contents = entry.path == "plugin.json" ? manifest : script;
            contents.resize(entry.size);
            if (!mz_zip_reader_extract_to_mem(&zip, entry.index, contents.data(), contents.size(), 0)) {
                error = "Cannot extract gameplay ZIP entry or CRC mismatch"; return false;
            }
        }
        if (!begin(manifest, script, error)) return false;
        for (const auto& entry : entries) {
            if (entry.path == "plugin.json" || entry.path == "scripts/main.lua") continue;
            std::string contents(entry.size, '\0');
            if (!mz_zip_reader_extract_to_mem(&zip, entry.index, contents.data(), contents.size(), 0)) {
                error = "Cannot extract gameplay ZIP entry or CRC mismatch"; return false;
            }
            if (!asset(entry.path, contents, error)) return false;
        }
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay archive";
        return false;
    }
}
}

bool readGameplayArchive(const std::string& data, GameplayArchive& out, std::string& error) {
    out = {};
    if (data.empty() || data.size() > kGameplayInlineArchiveLimit) {
        error = "Gameplay archive is empty or too large for memory loading"; return false;
    }
    mz_zip_archive zip{};
    ZipBudget budget;
    configureZip(zip, budget);
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) { error = "Invalid gameplay ZIP"; return false; }
    struct ZipGuard { mz_zip_archive* zip; ~ZipGuard() { mz_zip_reader_end(zip); } } guard{&zip};
    GameplayArchive loaded;
    size_t total = 0;
    const bool ok = readArchive(zip,
        [&](const std::string& manifest, const std::string& script, std::string&) {
            loaded.manifest = manifest; loaded.script = script; total = manifest.size() + script.size(); return true;
        },
        [&](const std::string& path, const std::string& bytes, std::string& assetError) {
            if (bytes.size() > kGameplayInlineArchiveLimit - total) {
                assetError = "Gameplay archive exceeds memory loading limit"; return false;
            }
            total += bytes.size();
            loaded.assets.emplace(path, bytes);
            return true;
        }, kGameplayInlineArchiveLimit, error);
    if (ok) out = std::move(loaded);
    return ok;
}

bool readGameplayArchiveFile(const std::filesystem::path& path, const GameplayArchiveBegin& begin,
                             const GameplayArchiveAsset& asset, std::string& error) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    const auto size = ec || !std::filesystem::is_regular_file(status) ? 0 : std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kGameplayArchiveLimit) {
        error = "Gameplay ZIP is not a regular file, is empty, or is too large"; return false;
    }
#ifdef _WIN32
    FILE* raw = nullptr;
    _wfopen_s(&raw, path.c_str(), L"rb");
#else
    FILE* raw = std::fopen(path.c_str(), "rb");
#endif
    if (!raw) { error = "Cannot open gameplay ZIP"; return false; }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(raw, std::fclose);
    mz_zip_archive zip{};
    ZipBudget budget;
    configureZip(zip, budget);
    if (!mz_zip_reader_init_cfile(&zip, file.get(), size, 0)) { error = "Invalid gameplay ZIP"; return false; }
    struct ZipGuard { mz_zip_archive* zip; ~ZipGuard() { mz_zip_reader_end(zip); } } guard{&zip};
    return readArchive(zip, begin, asset, kGameplayArchiveExpandedLimit, error);
}
}
