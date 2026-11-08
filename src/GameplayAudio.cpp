#include "GameplayAudio.h"
#include "GameplayLimits.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

namespace sf {
namespace {
constexpr unsigned kMaxSeconds = 30;
constexpr unsigned kMinRate = 8000, kMaxRate = 48000;

unsigned read16(const char* p) {
    return static_cast<unsigned char>(p[0]) | (static_cast<unsigned>(static_cast<unsigned char>(p[1])) << 8);
}
std::uint32_t read32(const char* p) {
    return read16(p) | (static_cast<std::uint32_t>(read16(p + 2)) << 16);
}
bool formatAllowed(unsigned rate, unsigned channels) {
    return rate >= kMinRate && rate <= kMaxRate && (channels == 1 || channels == 2);
}

bool decodeWave(const std::string& bytes, GameplayAudio& out, std::string& error) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) || read32(bytes.data() + 4) != bytes.size() - 8) {
        error = "Invalid WAV container"; return false;
    }
    bool foundFormat = false, foundData = false;
    unsigned rate = 0, channels = 0;
    size_t dataOffset = 0, dataSize = 0;
    for (size_t pos = 12; pos < bytes.size();) {
        if (bytes.size() - pos < 8) { error = "Truncated WAV chunk"; return false; }
        const char* chunk = bytes.data() + pos;
        const size_t size = read32(chunk + 4);
        pos += 8;
        if (size > bytes.size() - pos || (size & 1 && size == bytes.size() - pos)) {
            error = "Truncated WAV chunk"; return false;
        }
        if (!std::memcmp(chunk, "fmt ", 4)) {
            if (foundFormat || size < 16) { error = "Invalid WAV format"; return false; }
            foundFormat = true;
            const char* fmt = bytes.data() + pos;
            channels = read16(fmt + 2);
            rate = read32(fmt + 4);
            if (read16(fmt) != 1 || !formatAllowed(rate, channels) || read16(fmt + 14) != 16 ||
                read16(fmt + 12) != channels * 2 || read32(fmt + 8) != rate * channels * 2) {
                error = "WAV must be 16-bit PCM, mono or stereo, 8-48 kHz"; return false;
            }
        } else if (!std::memcmp(chunk, "data", 4)) {
            if (foundData) { error = "Duplicate WAV data"; return false; }
            foundData = true;
            dataOffset = pos; dataSize = size;
        }
        pos += size + (size & 1);
    }
    if (!foundFormat || !foundData || !dataSize || dataSize % (channels * 2) ||
        dataSize / (channels * 2) > static_cast<size_t>(rate) * kMaxSeconds) {
        error = "Invalid or oversized WAV samples"; return false;
    }
    const size_t frames = dataSize / (channels * 2);
    out.sampleRate = rate;
    out.samples.reserve(frames);
    for (size_t i = 0; i < frames; ++i) {
        const char* p = bytes.data() + dataOffset + i * channels * 2;
        const auto first = static_cast<std::int16_t>(read16(p));
        const int value = channels == 1 ? first : (static_cast<int>(first) +
            static_cast<std::int16_t>(read16(p + 2))) / 2;
        out.samples.push_back(static_cast<std::int16_t>(value));
    }
    return true;
}

bool decodeOgg(const std::string& bytes, GameplayAudio& out, std::string& error) {
    std::vector<char> arena(4 * 1024 * 1024);
    stb_vorbis_alloc alloc{arena.data(), static_cast<int>(arena.size())};
    int status = 0;
    auto* raw = stb_vorbis_open_memory(reinterpret_cast<const unsigned char*>(bytes.data()),
                                       static_cast<int>(bytes.size()), &status, &alloc);
    if (!raw) { error = "Invalid OGG Vorbis or decoder memory limit exceeded"; return false; }
    std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> stream(raw, stb_vorbis_close);
    const auto info = stb_vorbis_get_info(stream.get());
    if (!formatAllowed(info.sample_rate, static_cast<unsigned>(info.channels))) {
        error = "OGG must be mono or stereo, 8-48 kHz"; return false;
    }
    out.sampleRate = info.sample_rate;
    const size_t limit = static_cast<size_t>(info.sample_rate) * kMaxSeconds;
    std::array<short, 8192> block{};
    for (;;) {
        const int frames = stb_vorbis_get_samples_short_interleaved(stream.get(), info.channels,
                                                                     block.data(), static_cast<int>(block.size()));
        if (frames < 0 || static_cast<size_t>(frames) > limit - out.samples.size()) {
            error = "OGG sample duration exceeds limit"; return false;
        }
        if (!frames) break;
        for (int i = 0; i < frames; ++i) {
            const int a = block[static_cast<size_t>(i) * info.channels];
            const int b = info.channels == 2 ? block[static_cast<size_t>(i) * 2 + 1] : a;
            out.samples.push_back(static_cast<std::int16_t>((a + b) / 2));
        }
    }
    if (stb_vorbis_get_error(stream.get()) != VORBIS__no_error || out.samples.empty()) {
        error = "Invalid or empty OGG samples"; return false;
    }
    return true;
}
}

bool decodeGameplayAudio(const std::string& path, const std::string& bytes,
                         GameplayAudio& out, std::string& error) {
    out = {};
    error.clear();
    if (bytes.empty() || bytes.size() > kGameplayAssetLimit) { error = "Audio asset exceeds encoded limit"; return false; }
    try {
        GameplayAudio decoded;
        const bool ok = path.size() >= 4 && path.compare(path.size() - 4, 4, ".wav") == 0 ?
            decodeWave(bytes, decoded, error) :
            path.size() >= 4 && path.compare(path.size() - 4, 4, ".ogg") == 0 ?
            decodeOgg(bytes, decoded, error) : false;
        if (!ok) {
            if (error.empty()) error = "Unsupported audio asset";
            return false;
        }
        out = std::move(decoded);
        error.clear();
        return true;
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate audio asset";
        return false;
    }
}
}
