// Tiny synthesiser for the shell's sound effects: 80s-style blips, swooshes and booms, plus the electric car's hum.
// Everything is generated on the fly in the audio callback, so there are no sound files.
#pragma once

#include <mutex>
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
    // Called every frame: speed in m/s, whether the car is being driven, and whether boost is held.
    void setCar(bool driving, float speed, bool boost);
    void setMasterVolume(float v) { master_ = v; }

private:
    struct Voice {
        SfxId id;
        float t = 0, vol = 1, phase = 0, phase2 = 0, lp = 0, lp2 = 0;
        unsigned seed = 1;
    };
    static void callback(void* user, unsigned char* stream, int len);
    void fill(short* out, int frames);
    float voiceSample(Voice& v, bool& alive);

    unsigned dev_ = 0;
    std::mutex mu_;
    std::vector<Voice> voices_;
    // Car hum state; the targets are written from the main thread and smoothed in the callback.
    float carTarget_ = 0, carSpeedTarget_ = 0, carBoostTarget_ = 0;
    float carGain_ = 0, carSpeed_ = 0, carBoost_ = 0, ph1_ = 0, ph2_ = 0, ph3_ = 0, whineLp_ = 0;
    unsigned noise_ = 12345;
    float master_ = 0.8f;
    static constexpr int kRate = 44100;
};

}  // namespace sf
