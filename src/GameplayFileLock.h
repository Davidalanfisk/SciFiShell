#pragma once

#include <filesystem>
#include <string>

namespace sf {
class GameplayFileLock {
public:
    GameplayFileLock() = default;
    GameplayFileLock(const GameplayFileLock&) = delete;
    GameplayFileLock& operator=(const GameplayFileLock&) = delete;
    ~GameplayFileLock();
    bool open(const std::filesystem::path& path, std::string& error);
private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};
}
