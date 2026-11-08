#include "GameplayTexture.h"
#include "GameplayLimits.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "miniz.h"

namespace {
constexpr size_t kDecoderMemoryLimit = 16 * 1024 * 1024;

struct Budget { size_t used = 0; };
thread_local Budget* activeBudget = nullptr;

struct alignas(std::max_align_t) Allocation {
    size_t size;
    Budget* budget;
};

void* imageAlloc(size_t size) {
    if (!activeBudget || size > kDecoderMemoryLimit - activeBudget->used ||
        size > std::numeric_limits<size_t>::max() - sizeof(Allocation)) return nullptr;
    auto* p = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
    if (!p) return nullptr;
    p->size = size;
    p->budget = activeBudget;
    activeBudget->used += size;
    return p + 1;
}

void imageFree(void* ptr) {
    if (!ptr) return;
    auto* p = static_cast<Allocation*>(ptr) - 1;
    p->budget->used -= p->size;
    std::free(p);
}

void* imageRealloc(void* ptr, size_t size) {
    if (!ptr) return imageAlloc(size);
    if (!size) { imageFree(ptr); return nullptr; }
    auto* p = static_cast<Allocation*>(ptr) - 1;
    Budget* budget = p->budget;
    const size_t old = p->size;
    if (budget != activeBudget || (size > old && size - old > kDecoderMemoryLimit - budget->used) ||
        size > std::numeric_limits<size_t>::max() - sizeof(Allocation)) return nullptr;
    auto* next = static_cast<Allocation*>(std::realloc(p, sizeof(Allocation) + size));
    if (!next) return nullptr;
    next->size = size;
    budget->used = budget->used - old + size;
    return next + 1;
}

unsigned readBigEndian(const unsigned char* p) {
    return (static_cast<unsigned>(p[0]) << 24) | (static_cast<unsigned>(p[1]) << 16) |
           (static_cast<unsigned>(p[2]) << 8) | static_cast<unsigned>(p[3]);
}
}

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 1024
#define STBI_MALLOC(size) imageAlloc(size)
#define STBI_REALLOC(ptr, size) imageRealloc(ptr, size)
#define STBI_FREE(ptr) imageFree(ptr)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
static_assert(STBI_MAX_DIMENSIONS == sf::kGameplayTextureDimensionLimit);

namespace sf {
bool decodeGameplayTexture(const std::string& data, GameplayTexture& out, std::string& error) {
    out = {};
    error.clear();
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    static constexpr unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (data.size() < sizeof(signature) || data.size() > kGameplayAssetLimit ||
        std::memcmp(bytes, signature, sizeof(signature)) != 0) {
        error = "Texture must be a PNG no larger than 100 MiB";
        return false;
    }
    size_t pos = sizeof(signature);
    bool first = true, end = false, imageData = false;
    while (pos < data.size()) {
        if (data.size() - pos < 12) { error = "Truncated PNG chunk"; return false; }
        const auto length = static_cast<size_t>(readBigEndian(bytes + pos));
        if (length > data.size() - pos - 12) { error = "Truncated PNG chunk"; return false; }
        const auto* type = bytes + pos + 4;
        if (first && (std::memcmp(type, "IHDR", 4) != 0 || length != 13)) {
            error = "PNG must begin with IHDR";
            return false;
        }
        const mz_ulong crc = mz_crc32(MZ_CRC32_INIT, type, length + 4);
        if (crc != readBigEndian(type + 4 + length)) { error = "PNG checksum mismatch"; return false; }
        if (std::memcmp(type, "IDAT", 4) == 0) imageData = true;
        if (std::memcmp(type, "IEND", 4) == 0) {
            if (length || !imageData || pos + 12 != data.size()) {
                error = "Invalid PNG end";
                return false;
            }
            end = true;
            break;
        }
        first = false;
        pos += length + 12;
    }
    if (!end) { error = "PNG is missing its end"; return false; }
    if (activeBudget) { error = "Nested texture decoding is not supported"; return false; }
    Budget budget;
    activeBudget = &budget;
    struct ResetBudget { ~ResetBudget() { activeBudget = nullptr; } } reset;
    int width = 0, height = 0, channels = 0;
    const int size = static_cast<int>(data.size());
    if (!stbi_info_from_memory(bytes, size, &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > kGameplayTextureDimensionLimit || height > kGameplayTextureDimensionLimit) {
        error = "Invalid PNG or dimensions exceed 1024";
        return false;
    }
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes, size, &width, &height, &channels, 4), stbi_image_free);
    if (!pixels) {
        error = "PNG decoding failed or exceeded its memory budget";
        return false;
    }
    const size_t decodedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    try {
        out.rgba.assign(pixels.get(), pixels.get() + decodedSize);
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate decoded PNG";
        return false;
    }
    out.width = width;
    out.height = height;
    return true;
}
}  // namespace sf
