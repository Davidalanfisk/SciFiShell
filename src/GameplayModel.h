#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "GameplayTexture.h"

namespace sf {

struct GameplayModel {
    // Interleaved position, normal, and UV components.
    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    GameplayTexture embeddedTexture;
    float baseColour[4] = {1, 1, 1, 1};
};

// Internal, deliberately narrow GLB decoder. This does not create GPU resources.
bool decodeGameplayModel(const std::string& data, GameplayModel& out, std::string& error);

}  // namespace sf
