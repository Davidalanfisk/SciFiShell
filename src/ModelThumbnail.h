#pragma once

#include <functional>
#include <string>

#include "Utils.h"

namespace sf {
constexpr size_t kModelPreviewBytes = 8 * 1024 * 1024;

// Static, orthographic preview of the same restricted GLB subset as gameplay models; CPU worker only.
bool renderModelThumbnail(const std::string& data, Bitmap& out, std::string& error,
                          const std::function<bool()>& cancelled = {});
}
