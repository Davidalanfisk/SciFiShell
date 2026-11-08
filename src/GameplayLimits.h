#pragma once

#include <cstddef>
#include <cstdint>

namespace sf {
inline constexpr std::size_t kGameplayArchiveLimit = 1024ull * 1024 * 1024;
inline constexpr std::size_t kGameplayInlineArchiveLimit = 8 * 1024 * 1024;
inline constexpr std::uint64_t kGameplayInstalledZipLimit = 4ull * 1024 * 1024 * 1024;
inline constexpr std::size_t kGameplayArchiveExpandedLimit = 1024ull * 1024 * 1024;
inline constexpr std::size_t kGameplayAssetLimit = 100ull * 1024 * 1024;
inline constexpr std::size_t kGameplayTextLimit = 16384;
inline constexpr unsigned kGameplayEntryLimit = 34;
inline constexpr std::size_t kGameplayDecodedLimit = 16 * 1024 * 1024;
inline constexpr int kGameplayTextureDimensionLimit = 1024;
}  // namespace sf
