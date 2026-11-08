#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sf {
struct GameplayAudio {
    unsigned sampleRate = 0;
    std::vector<std::int16_t> samples;
};

// Decodes a short mono or stereo WAV/OGG into bounded mono PCM.
bool decodeGameplayAudio(const std::string& path, const std::string& bytes,
                         GameplayAudio& out, std::string& error);
}
