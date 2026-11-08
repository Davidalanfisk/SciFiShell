#include "GameplayConsent.h"

#include "GameplayPackage.h"
#include "GameplayFileLock.h"
#include "GameplayLimits.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr char kDigestMagic[] = "SFSGRNT2";
bool validId(const std::string& id) {
    if (id.size() < 3 || id.size() > 80 || id.find('.') == std::string::npos ||
        id.front() == '.' || id.back() == '.' || id.find("..") != std::string::npos ||
        id.rfind("org.scifishell.", 0) == 0) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    return true;
}
}

namespace sf {
GameplayConsent::GameplayConsent(std::filesystem::path directory) : directory_(std::move(directory)) {}

bool GameplayConsent::grant(const std::string& id, const std::string& bytes, std::string& error) {
    if (directory_.empty() || !validId(id)) { error = "Invalid consent storage or plugin ID"; return false; }
    GameplayPackage package;
    if (!readGameplayPackage(bytes, package, error) || package.id != id) {
        if (error.empty()) error = "Consent package ID does not match";
        return false;
    }
    return writeGrant(id, bytes, error);
}

bool GameplayConsent::grantFile(const std::string& id, const std::filesystem::path& path,
                                const GameplayDigest& expected, std::string& error) {
    if (directory_.empty() || !validId(id)) { error = "Invalid consent storage or plugin ID"; return false; }
    GameplayFileLock lock;
    if (!lock.open(path, error)) return false;
    GameplayDigest before, after;
    GameplayPackage package;
    if (!gameplayDigestFile(path, before, error) || !(before == expected) ||
        !readGameplayPackageFile(path, package, error) || package.id != id ||
        !gameplayDigestFile(path, after, error) || !(before == after)) {
        if (error.empty()) error = "Consent package changed or ID does not match";
        return false;
    }
    std::string record(kDigestMagic, sizeof(kDigestMagic) - 1);
    for (int i = 0; i < 8; ++i) record.push_back(static_cast<char>((before.size >> (i * 8)) & 255));
    record.append(reinterpret_cast<const char*>(before.sha256.data()), before.sha256.size());
    return writeGrant(id, record, error);
}

bool GameplayConsent::writeGrant(const std::string& id, const std::string& bytes, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) { error = "Cannot create consent directory: " + ec.message(); return false; }
    const auto status = std::filesystem::symlink_status(directory_, ec);
    if (ec || !std::filesystem::is_directory(status)) { error = "Consent directory is not a directory"; return false; }
    std::filesystem::path tempDir;
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 32; ++attempt) {
        tempDir = directory_ / ("consent-tmp-" + std::to_string(nonce) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(tempDir, ec)) break;
        if (ec) { error = "Cannot create temporary consent: " + ec.message(); return false; }
        tempDir.clear();
    }
    if (tempDir.empty()) { error = "Cannot create unique temporary consent"; return false; }
    const auto temp = tempDir / "grant", target = directory_ / (id + ".grant");
    auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        if (ignored) std::fprintf(stderr, "Cannot remove temporary consent file: %s\n", ignored.message().c_str());
        ignored.clear();
        std::filesystem::remove(tempDir, ignored);
        if (ignored) std::fprintf(stderr, "Cannot remove temporary consent directory: %s\n", ignored.message().c_str());
    };
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            error = "Cannot write consent"; cleanup(); return false;
        }
        out.close();
        if (!out) { error = "Cannot finish writing consent"; cleanup(); return false; }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Cannot replace consent (Windows error " + std::to_string(GetLastError()) + ")";
        cleanup();
        return false;
    }
#else
    std::filesystem::rename(temp, target, ec);
    if (ec) { error = "Cannot replace consent: " + ec.message(); cleanup(); return false; }
#endif
    cleanup();
    error.clear();
    return true;
}

bool GameplayConsent::matches(const std::string& id, const std::string& bytes, bool& granted, std::string& error) const {
    GameplayDigest digest;
    if (!gameplayDigestBytes(bytes, digest, error)) { granted = false; return false; }
    return matchesDigest(id, digest, granted, error);
}

bool GameplayConsent::matchesDigest(const std::string& id, const GameplayDigest& digest,
                                    bool& granted, std::string& error) const {
    granted = false;
    if (directory_.empty() || !validId(id) || !digest.size || digest.size > kGameplayArchiveLimit) {
        error = "Invalid consent query"; return false;
    }
    const auto path = directory_ / (id + ".grant");
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !std::filesystem::exists(status))) {
        error.clear();
        return true;
    }
    if (ec || !std::filesystem::is_regular_file(status)) { error = "Cannot inspect consent"; return false; }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kGameplayArchiveLimit) { error = "Invalid consent file"; return false; }
    if (size == 48) {
        std::ifstream in(path, std::ios::binary);
        std::array<unsigned char, 48> record{};
        if (!in.read(reinterpret_cast<char*>(record.data()), record.size()) ||
            in.peek() != std::ifstream::traits_type::eof() || in.bad()) {
            error = "Cannot read consent"; return false;
        }
        if (std::memcmp(record.data(), kDigestMagic, 8) == 0) {
            std::uint64_t recordedSize = 0;
            for (int i = 0; i < 8; ++i) recordedSize |= static_cast<std::uint64_t>(record[8 + i]) << (i * 8);
            granted = recordedSize == digest.size &&
                std::equal(digest.sha256.begin(), digest.sha256.end(), record.begin() + 16);
            error.clear(); return true;
        }
    }
    if (size != digest.size) { error.clear(); return true; }
    GameplayDigest legacy;
    if (!gameplayDigestFile(path, legacy, error)) return false;
    granted = legacy == digest;
    error.clear();
    return true;
}

bool GameplayConsent::revoke(const std::string& id, std::string& error) {
    if (directory_.empty() || !validId(id)) { error = "Invalid consent storage or plugin ID"; return false; }
    std::error_code ec;
    std::filesystem::remove(directory_ / (id + ".grant"), ec);
    if (ec) { error = "Cannot revoke consent: " + ec.message(); return false; }
    error.clear();
    return true;
}
}  // namespace sf
