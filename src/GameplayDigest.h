#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace sf {
struct GameplayDigest {
    std::uint64_t size = 0;
    std::array<unsigned char, 32> sha256{};
    bool operator==(const GameplayDigest&) const = default;
};

bool gameplayDigestBytes(const std::string& data, GameplayDigest& digest, std::string& error);
bool gameplayDigestFile(const std::filesystem::path& path, GameplayDigest& digest, std::string& error);
}
