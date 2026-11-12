#include "GameplayStorage.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sf {
namespace {
constexpr size_t kMaxEntries = 256, kMaxKey = 64, kMaxString = 16384, kMaxFile = 262144;
constexpr char kMagic[] = "SFSSTORE1";

bool validKey(const std::string& key) {
    if (key.empty() || key.size() > kMaxKey) return false;
    for (unsigned char c : key) if (c < 0x20 || c > 0x7e) return false;
    return true;
}

bool validId(const std::string& id) {
    if (id.size() < 3 || id.size() > 80 || id.find('.') == std::string::npos ||
        id.front() == '.' || id.back() == '.' || id.find("..") != std::string::npos ||
        id.rfind("org.scifishell.", 0) == 0) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    return true;
}

void append32(std::string& out, std::uint32_t n) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((n >> (i * 8)) & 255));
}
bool read32(const std::string& in, size_t& pos, std::uint32_t& n) {
    if (in.size() - pos < 4) return false;
    n = 0;
    for (int i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos++])) << (i * 8);
    return true;
}
}

GameplayStorage::GameplayStorage(std::filesystem::path directory, std::string id)
    : directory_(std::move(directory)), id_(std::move(id)) {}

bool GameplayStorage::load(std::string& error) {
    if (directory_.empty() || !validId(id_)) { error = "Invalid plugin storage path or ID"; return false; }
    const auto path = directory_ / (id_ + ".store");
    std::error_code ec;
    const auto directoryStatus = std::filesystem::symlink_status(directory_, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !std::filesystem::exists(directoryStatus))) {
        values_.clear(); error.clear(); return true;
    }
    if (ec || !std::filesystem::is_directory(directoryStatus)) { error = "Invalid plugin storage directory"; return false; }
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !std::filesystem::exists(status))) {
        values_.clear(); error.clear(); return true;
    }
    if (ec || !std::filesystem::is_regular_file(status)) { error = "Invalid plugin storage file"; return false; }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxFile) { error = "Plugin storage file is unreadable or too large"; return false; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "Cannot open plugin storage"; return false; }
    std::string data(static_cast<size_t>(size), '\0');
    if (!in.read(data.data(), static_cast<std::streamsize>(data.size())) || in.peek() != std::ifstream::traits_type::eof() || in.bad()) {
        error = "Cannot read plugin storage"; return false;
    }
    if (data.size() < sizeof(kMagic) - 1 || data.compare(0, sizeof(kMagic) - 1, kMagic) != 0) {
        error = "Invalid plugin storage format"; return false;
    }
    size_t pos = sizeof(kMagic) - 1;
    std::uint32_t count = 0;
    if (!read32(data, pos, count) || count > kMaxEntries) { error = "Invalid plugin storage count"; return false; }
    std::map<std::string, Value> loaded;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t keySize = 0, valueSize = 0;
        if (!read32(data, pos, keySize) || keySize > kMaxKey || keySize > data.size() - pos) {
            error = "Invalid plugin storage key"; return false;
        }
        std::string key = data.substr(pos, keySize); pos += keySize;
        if (!validKey(key) || pos == data.size()) { error = "Invalid plugin storage entry"; return false; }
        const char type = data[pos++];
        if (!read32(data, pos, valueSize) || valueSize > data.size() - pos) {
            error = "Invalid plugin storage value"; return false;
        }
        Value value;
        if (type == 's' && valueSize <= kMaxString) value = data.substr(pos, valueSize);
        else if (type == 'b' && valueSize == 1 && (data[pos] == 0 || data[pos] == 1)) value = data[pos] != 0;
        else if (type == 'n' && valueSize == sizeof(double)) {
            double number;
            std::memcpy(&number, data.data() + pos, sizeof(number));
            if (!std::isfinite(number)) { error = "Invalid plugin storage number"; return false; }
            value = number;
        } else { error = "Invalid plugin storage value type"; return false; }
        pos += valueSize;
        if (!loaded.emplace(std::move(key), std::move(value)).second) {
            error = "Duplicate plugin storage key"; return false;
        }
    }
    if (pos != data.size()) { error = "Unexpected plugin storage data"; return false; }
    values_ = std::move(loaded);
    error.clear();
    return true;
}

bool GameplayStorage::get(const std::string& key, const Value*& value, std::string& error) const {
    if (!validKey(key)) { error = "Storage key must be 1-64 printable ASCII characters"; return false; }
    const auto it = values_.find(key);
    value = it == values_.end() ? nullptr : &it->second;
    error.clear();
    return true;
}

bool GameplayStorage::set(const std::string& key, Value value, std::string& error) {
    if (!validKey(key)) { error = "Storage key must be 1-64 printable ASCII characters"; return false; }
    if ((std::holds_alternative<std::string>(value) && std::get<std::string>(value).size() > kMaxString) ||
        (std::holds_alternative<double>(value) && !std::isfinite(std::get<double>(value)))) {
        error = "Storage value is too large or not finite"; return false;
    }
    if (!values_.count(key) && values_.size() >= kMaxEntries) { error = "Plugin storage entry limit exceeded"; return false; }
    auto next = values_;
    next.insert_or_assign(key, std::move(value));
    if (!save(next, error)) return false;
    values_ = std::move(next);
    return true;
}

bool GameplayStorage::erase(const std::string& key, std::string& error) {
    if (!validKey(key)) { error = "Storage key must be 1-64 printable ASCII characters"; return false; }
    if (!values_.count(key)) { error.clear(); return true; }
    auto next = values_;
    next.erase(key);
    if (!save(next, error)) return false;
    values_ = std::move(next);
    return true;
}

bool GameplayStorage::save(const std::map<std::string, Value>& values, std::string& error) {
    std::string data(kMagic, sizeof(kMagic) - 1);
    append32(data, static_cast<std::uint32_t>(values.size()));
    for (const auto& [key, value] : values) {
        append32(data, static_cast<std::uint32_t>(key.size()));
        data += key;
        if (const auto* text = std::get_if<std::string>(&value)) {
            data.push_back('s'); append32(data, static_cast<std::uint32_t>(text->size())); data += *text;
        } else if (const auto* number = std::get_if<double>(&value)) {
            data.push_back('n'); append32(data, sizeof(*number));
            data.append(reinterpret_cast<const char*>(number), sizeof(*number));
        } else {
            data.push_back('b'); append32(data, 1); data.push_back(std::get<bool>(value) ? 1 : 0);
        }
    }
    if (data.size() > kMaxFile) { error = "Plugin storage byte limit exceeded"; return false; }
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec || !std::filesystem::is_directory(std::filesystem::symlink_status(directory_, ec)) || ec) {
        error = "Cannot create plugin storage directory"; return false;
    }
    const auto target = directory_ / (id_ + ".store");
    const auto status = std::filesystem::symlink_status(target, ec);
    if (ec == std::errc::no_such_file_or_directory) ec.clear();
    if (ec || (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))) {
        error = "Invalid plugin storage target"; return false;
    }
    std::filesystem::path tempDir;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int i = 0; i < 32; ++i) {
        tempDir = directory_ / ("storage-tmp-" + std::to_string(nonce) + "-" + std::to_string(i));
        if (std::filesystem::create_directory(tempDir, ec)) break;
        if (ec) { error = "Cannot create temporary plugin storage: " + ec.message(); return false; }
        tempDir.clear();
    }
    if (tempDir.empty()) { error = "Cannot create temporary plugin storage"; return false; }
    const auto temp = tempDir / "data";
    auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        if (ignored) std::fprintf(stderr, "Cannot remove temporary plugin storage: %s\n", ignored.message().c_str());
        ignored.clear();
        std::filesystem::remove(tempDir, ignored);
        if (ignored) std::fprintf(stderr, "Cannot remove temporary plugin storage directory: %s\n", ignored.message().c_str());
    };
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(data.data(), static_cast<std::streamsize>(data.size()))) {
            error = "Cannot write plugin storage"; cleanup(); return false;
        }
        out.close();
        if (!out) { error = "Cannot finish writing plugin storage"; cleanup(); return false; }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace plugin storage (Windows error " + std::to_string(GetLastError()) + ")";
        cleanup(); return false;
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec) { error = "Cannot replace plugin storage: " + ec.message(); cleanup(); return false; }
#endif
    cleanup();
    error.clear();
    return true;
}
}
