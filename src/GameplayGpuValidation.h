#pragma once

#include "GameplayModel.h"

#include <cstddef>
#include <string>

namespace sf {
inline constexpr size_t kGameplayGpuOwnerLimit = 16 * 1024 * 1024;
inline constexpr size_t kGameplayGpuTotalLimit = 64 * 1024 * 1024;
inline constexpr size_t kGameplayGpuModelLimit = 32;
inline constexpr size_t kGameplayGpuTotalModelLimit = 128;

bool gameplayGpuBytes(const GameplayModel& model, const GameplayTexture* overrideTexture,
                      size_t& bytes, std::string& error);
}  // namespace sf
