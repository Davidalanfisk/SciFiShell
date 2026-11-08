#include "GameplayGpuValidation.h"

#include "GameplayLimits.h"

#include <cmath>
#include <limits>

namespace sf {
bool gameplayGpuBytes(const GameplayModel& model, const GameplayTexture* overrideTexture,
                      size_t& bytes, std::string& error) {
    bytes = 0;
    if (model.vertices.empty() || model.vertices.size() % 8 ||
        model.indices.empty() || model.indices.size() % 3 ||
        model.vertices.size() > kGameplayGpuOwnerLimit / sizeof(float) ||
        model.indices.size() > kGameplayGpuOwnerLimit / sizeof(std::uint32_t) ||
        model.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "Invalid gameplay GPU geometry";
        return false;
    }
    for (float v : model.vertices) {
        if (!std::isfinite(v) || std::abs(v) > 10000) {
            error = "Invalid gameplay GPU vertex";
            return false;
        }
    }
    const size_t count = model.vertices.size() / 8;
    for (auto index : model.indices) {
        if (index >= count) { error = "Gameplay GPU index out of range"; return false; }
    }
    for (float component : model.baseColour) {
        if (!std::isfinite(component) || component < 0 || component > 1) {
            error = "Invalid gameplay GPU colour";
            return false;
        }
        if (model.baseColour[3] != 1) {
            error = "Transparent gameplay materials are unsupported";
            return false;
        }
    }
    const auto& texture = overrideTexture ? *overrideTexture : model.embeddedTexture;
    size_t textureBytes = 4;
    if (texture.width || texture.height || !texture.rgba.empty()) {
        if (texture.width <= 0 || texture.height <= 0 ||
            texture.width > kGameplayTextureDimensionLimit || texture.height > kGameplayTextureDimensionLimit) {
            error = "Invalid gameplay GPU texture dimensions";
            return false;
        }
        textureBytes = static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height) * 4;
        if (texture.rgba.size() != textureBytes) {
            error = "Invalid gameplay GPU texture data";
            return false;
        }
        for (size_t i = 3; i < textureBytes; i += 4) {
            if (texture.rgba[i] != 255) {
                error = "Transparent gameplay textures are unsupported";
                return false;
            }
        }
    } else if (overrideTexture) {
        error = "Empty gameplay GPU texture override";
        return false;
    }
    const size_t geometry = model.vertices.size() * sizeof(float) + model.indices.size() * sizeof(std::uint32_t);
    if (geometry > kGameplayGpuOwnerLimit || textureBytes > kGameplayGpuOwnerLimit - geometry) {
        error = "Gameplay GPU model exceeds memory limit";
        return false;
    }
    bytes = geometry + textureBytes;
    error.clear();
    return true;
}
}  // namespace sf
