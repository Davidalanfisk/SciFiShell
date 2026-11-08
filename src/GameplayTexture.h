#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sf {

struct GameplayTexture {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

// Internal PNG decoder. It does not grant access to assets or upload GPU resources.
bool decodeGameplayTexture(const std::string& data, GameplayTexture& out, std::string& error);

}  // namespace sf
