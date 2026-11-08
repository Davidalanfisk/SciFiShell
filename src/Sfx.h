// Tiny synthesiser for the shell's sound effects: 80s-style blips, swooshes and booms, plus the electric car's hum.
// Everything is generated on the fly in the audio callback, so there are no sound files.
#pragma once

#include <mutex>
#include <cstdint>
#include <vector>

namespace sf {

enum class SfxId { Pickup, Throw, Boom, DuckPop, CarSummon, RailOn };

class Sfx {
public:
    Sfx() = default;
    ~Sfx();
    bool init();
    void shutdown();
    void play(SfxId id, float volume = 1.0f);
    bool playOwned(std::uint64_t owner, SfxId id, float volume);
    bool setOwnedEngine(std::uint64_t owner, bool active, float speed, bool boost);
    void stopOwner(std::uint64_t owner);
#ifdef SCIFI_TEST_SYNTHETIC_CITY
    bool testHasOwnedEngine(std::uint64_t owner);
#endif
    // Called every frame: speed in m/s, whether the car is being driven, and whether boost is held.
    void setCar(bool driving, float speed, bool boost);
    void setMasterVolume(float v) { master_ = 0.8f * v; }

private:
    friend struct SfxTestAccess;
    struct Voice {
        SfxId id;
        float t = 0, vol = 1, phase = 0, phase2 = 0, lp = 0, lp2 = 0;
        unsigned seed = 1;
        std::uint64_t owner = 0;
    };
    struct Engine {
        std::uint64_t owner = 0;
        float target = 0, speedTarget = 0, boostTarget = 0;
        float gain = 0, speed = 0, boost = 0, phase1 = 0, phase2 = 0, lowpass = 0;
    };
    static void callback(void* user, unsigned char* stream, int len);
    void fill(short* out, int frames);
    float voiceSample(Voice& v, bool& alive);
    static float engineSample(Engine& engine);

    unsigned dev_ = 0;
    std::mutex mu_;
    std::vector<Voice> voices_;
    Engine carEngine_;
    std::vector<Engine> pluginEngines_;
    float master_ = 0.8f;
    static constexpr int kRate = 44100;
};

}  // namespace sf
