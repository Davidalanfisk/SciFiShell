#include "GameplayAudio.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
void u16(std::string& out, unsigned n) {
    out.push_back(static_cast<char>(n));
    out.push_back(static_cast<char>(n >> 8));
}
void u32(std::string& out, unsigned n) {
    u16(out, n); u16(out, n >> 16);
}
std::string wave(unsigned frames) {
    std::string out = "RIFF";
    u32(out, 36 + frames * 2);
    out += "WAVEfmt ";
    u32(out, 16); u16(out, 1); u16(out, 1);
    u32(out, 8000); u32(out, 16000); u16(out, 2); u16(out, 16);
    out += "data"; u32(out, frames * 2);
    for (unsigned i = 0; i < frames; ++i) u16(out, i % 2 ? 0x7fff : 0x8000);
    return out;
}
}

int main() {
    sf::GameplayAudio audio;
    std::string error;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << ": " << error << '\n'; ++failures; }
    };
    const auto wav = wave(800);
    check(sf::decodeGameplayAudio("assets/tone.wav", wav, audio, error) && audio.sampleRate == 8000 &&
          audio.samples.size() == 800 && audio.samples[0] == -32768 && audio.samples[1] == 32767,
          "decode PCM WAV");
    check(!sf::decodeGameplayAudio("assets/tone.wav", wav.substr(0, wav.size() - 1), audio, error),
          "reject truncated WAV");
    check(!sf::decodeGameplayAudio("assets/tone.wav", wave(8000 * 30 + 1), audio, error),
          "reject WAV beyond duration limit");
    const auto path = std::filesystem::path(SCIFI_TEST_ASSETS).parent_path() /
                      "examples" / "plugins" / "audio-demo" / "assets" / "tone.ogg";
    std::ifstream in(path, std::ios::binary);
    const std::string ogg((std::istreambuf_iterator<char>(in)), {});
    check(!ogg.empty() && sf::decodeGameplayAudio("assets/tone.ogg", ogg, audio, error) &&
          audio.sampleRate == 8000 && !audio.samples.empty(), "decode packaged OGG");
    check(!sf::decodeGameplayAudio("assets/tone.ogg", "invalid", audio, error), "reject invalid OGG");
    check(!sf::decodeGameplayAudio("assets/tone.mp3", wav, audio, error), "reject unsupported format");
    return failures ? 1 : 0;
}
