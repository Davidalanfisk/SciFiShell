#pragma once

#include <cstddef>

namespace sf {
inline constexpr std::size_t kGameplayArchiveLimit = 8 * 1024 * 1024;
inline constexpr std::size_t kGameplayArchiveExpandedLimit = 8 * 1024 * 1024;
inline constexpr std::size_t kGameplayAssetLimit = 2 * 1024 * 1024;
inline constexpr std::size_t kGameplayTextLimit = 16384;
inline constexpr unsigned kGameplayEntryLimit = 34;
inline constexpr std::size_t kGameplayDecodedLimit = 16 * 1024 * 1024;
inline constexpr int kGameplayTextureDimensionLimit = 1024;
}  // namespace sf
