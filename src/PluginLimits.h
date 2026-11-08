#pragma once

#include <cstddef>

namespace sf {
inline constexpr std::size_t kPluginTextLimit = 10 * 1024 * 1024;
// Both files can be stored uncompressed, with room for ZIP headers/comments.
inline constexpr std::size_t kScriptArchiveLimit = 2 * kPluginTextLimit + 64 * 1024;
}  // namespace sf
