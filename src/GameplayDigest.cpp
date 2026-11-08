#include "GameplayDigest.h"
#include "GameplayFileLock.h"
#include "GameplayLimits.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <new>

#include "picosha2.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sf {
GameplayFileLock::~GameplayFileLock() {
#ifdef _WIN32
    if (handle_) CloseHandle(handle_);
#else
    if (fd_ >= 0) close(fd_);
#endif
}

bool GameplayFileLock::open(const std::filesystem::path& path, std::string& error) {
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle != INVALID_HANDLE_VALUE) handle_ = handle;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!handle_ || !GetFileInformationByHandle(handle_, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
#else
    fd_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    struct stat info{};
    if (fd_ < 0 || fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) || flock(fd_, LOCK_SH | LOCK_NB) != 0) {
#endif
        error = "Cannot lock gameplay ZIP for reading";
        return false;
    }
    return true;
}

bool gameplayDigestBytes(const std::string& data, GameplayDigest& digest, std::string& error) {
    if (data.empty() || data.size() > kGameplayArchiveLimit) { error = "Invalid gameplay ZIP size"; return false; }
    try {
        picosha2::hash256_one_by_one hash;
        constexpr size_t chunk = 65536;
        for (size_t pos = 0; pos < data.size();) {
            const size_t n = std::min(chunk, data.size() - pos);
            hash.process(data.data() + pos, data.data() + pos + n);
            pos += n;
        }
        hash.finish();
        digest.size = data.size();
        hash.get_hash_bytes(digest.sha256.begin(), digest.sha256.end());
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay digest";
        return false;
    }
}

bool gameplayDigestFile(const std::filesystem::path& path, GameplayDigest& digest, std::string& error) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    const auto size = ec || !std::filesystem::is_regular_file(status) ? 0 : std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kGameplayArchiveLimit) {
        error = "Gameplay ZIP is not a regular file, is empty, or is too large"; return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "Cannot open gameplay ZIP"; return false; }
    try {
        picosha2::hash256_one_by_one hash;
        std::array<char, 65536> buffer{};
        std::uint64_t remaining = size;
        while (remaining) {
            const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
            if (!in.read(buffer.data(), count)) { error = "Cannot read gameplay ZIP"; return false; }
            hash.process(buffer.data(), buffer.data() + count);
            remaining -= static_cast<std::uint64_t>(count);
        }
        if (in.peek() != std::ifstream::traits_type::eof() || in.bad()) {
            error = "Gameplay ZIP changed while reading"; return false;
        }
        hash.finish();
        digest.size = size;
        hash.get_hash_bytes(digest.sha256.begin(), digest.sha256.end());
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay digest";
        return false;
    }
}
}
