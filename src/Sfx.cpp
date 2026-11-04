#include "Sfx.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>

namespace sf {

namespace {
constexpr float kTau = 6.28318530718f;

float rnd(unsigned& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
}
float square(float ph) { return std::sin(ph) >= 0.0f ? 1.0f : -1.0f; }
float saw(float ph) { return std::fmod(ph / kTau, 1.0f) * 2.0f - 1.0f; }
float tri(float ph) { return std::fabs(std::fmod(ph / kTau, 1.0f) * 4.0f - 2.0f) - 1.0f; }
}  // namespace

Sfx::~Sfx() { shutdown(); }

bool Sfx::init() {
    if (dev_) return true;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want{}, have{};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = &Sfx::callback;
    want.userdata = this;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) return false;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void Sfx::shutdown() {
    if (!dev_) return;
    SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

void Sfx::play(SfxId id, float volume) {
    if (!dev_ || volume <= 0.01f) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (voices_.size() > 24) return;
    Voice v;
    v.id = id;
    v.vol = volume;
    v.seed = 977u + static_cast<unsigned>(SDL_GetTicks()) * 31u;
    voices_.push_back(v);
}

void Sfx::setCar(bool driving, float speed, bool boost) {
    std::lock_guard<std::mutex> lk(mu_);
    carTarget_ = driving ? 1.0f : 0.0f;
    carSpeedTarget_ = std::min(std::fabs(speed), 110.0f);
    carBoostTarget_ = boost ? 1.0f : 0.0f;
}

void Sfx::callback(void* user, unsigned char* stream, int len) {
    static_cast<Sfx*>(user)->fill(reinterpret_cast<short*>(stream), len / 2);
}

// One voice, one sample. Each effect is a few oscillators under an envelope, in the style of an old arcade board.
float Sfx::voiceSample(Voice& v, bool& alive) {
    const float dt = 1.0f / kRate, t = v.t;
    v.t += dt;
    float s = 0.0f;
    switch (v.id) {
    case SfxId::Pickup: {  // rising three-note chirp
        const float dur = 0.28f;
        if (t > dur) { alive = false; break; }
        const float notes[3] = {523.25f, 659.25f, 1046.5f};
        const int n = std::min(2, static_cast<int>(t / (dur / 3.0f)));
        v.phase += kTau * notes[n] * dt;
        const float local = std::fmod(t, dur / 3.0f) / (dur / 3.0f);
        s = square(v.phase) * 0.35f * (1.0f - local * 0.6f);
        break;
    }
    case SfxId::Throw: {  // noise swoosh with a falling zap
        const float dur = 0.38f;
        if (t > dur) { alive = false; break; }
        const float env = std::sin(kTau * 0.5f * (t / dur)) * (1.0f - t / dur);
        v.phase += kTau * (900.0f - 1500.0f * t) * dt;
        v.lp += (rnd(v.seed) - v.lp) * (0.15f + 0.5f * t / dur);
        s = (v.lp * 1.6f + saw(v.phase) * 0.35f) * env * 0.7f;
        break;
    }
    case SfxId::Boom: {  // low thump, falling saw and a long filtered rumble
        const float dur = 1.5f;
        if (t > dur) { alive = false; break; }
        const float e1 = std::exp(-t * 3.2f), e2 = std::exp(-t * 7.0f);
        v.phase += kTau * (28.0f + 120.0f * std::exp(-t * 9.0f)) * dt;
        v.phase2 += kTau * (700.0f * std::exp(-t * 6.0f) + 60.0f) * dt;
        v.lp += (rnd(v.seed) - v.lp) * (0.04f + 0.25f * e2);
        s = std::sin(v.phase) * 0.9f * e1 + saw(v.phase2) * 0.28f * e2 + v.lp * 1.8f * e1;
        s *= std::min(1.0f, t * 400.0f);
        break;
    }
    case SfxId::DuckPop: {  // a squeaky wobbling quack, then a bright blip
        const float dur = 0.45f;
        if (t > dur) { alive = false; break; }
        if (t < 0.2f) {
            const float f = 640.0f - 1200.0f * t + 40.0f * std::sin(kTau * 38.0f * t);
            v.phase += kTau * f * dt;
            v.phase2 += kTau * f * 2.01f * dt;
            s = (square(v.phase) * 0.28f + tri(v.phase2) * 0.2f) * (1.0f - t / 0.2f * 0.4f);
        } else {
            v.phase += kTau * (1318.5f + (t > 0.32f ? 660.0f : 0.0f)) * dt;
            s = square(v.phase) * 0.28f * std::exp(-(t - 0.2f) * 7.0f);
        }
        break;
    }
    case SfxId::CarSummon: {  // synth rise
        const float dur = 0.7f;
        if (t > dur) { alive = false; break; }
        const float k = t / dur;
        v.phase += kTau * (110.0f * std::pow(8.0f, k)) * dt;
        v.phase2 += kTau * (110.0f * std::pow(8.0f, k) * 1.004f) * dt;
        s = (saw(v.phase) + saw(v.phase2)) * 0.18f * std::sin(kTau * 0.5f * k);
        break;
    }
    case SfxId::RailOn: {  // quick up-sweep when the car locks onto the track
        const float dur = 0.5f;
        if (t > dur) { alive = false; break; }
        const float k = t / dur;
        v.phase += kTau * (300.0f + 1500.0f * k * k) * dt;
        s = (tri(v.phase) * 0.3f + square(v.phase * 0.5f) * 0.1f) * (1.0f - k);
        break;
    }
    }
    return s * v.vol;
}

void Sfx::fill(short* out, int frames) {
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < frames; ++i) {
        float s = 0.0f;
        for (Voice& v : voices_) {
            bool alive = true;
            s += voiceSample(v, alive);
            if (!alive) v.vol = 0.0f;
        }
        // Electric hum: two detuned triangles that climb with speed, a faint whine and a touch of air at the top end.
        const float k = 1.0f / kRate;
        carGain_ += (carTarget_ - carGain_) * 3.0f * k;
        carSpeed_ += (carSpeedTarget_ - carSpeed_) * 2.5f * k;
        carBoost_ += (carBoostTarget_ - carBoost_) * 4.0f * k;
        if (carGain_ > 0.002f) {
            const float sp = carSpeed_ / 100.0f;
            const float f = 48.0f + sp * 150.0f + carBoost_ * 12.0f;
            ph1_ += kTau * f * k;
            ph2_ += kTau * f * 1.498f * k;
            ph3_ += kTau * (380.0f + sp * 1500.0f) * k;
            whineLp_ += (rnd(noise_) - whineLp_) * 0.08f;
            const float body = tri(ph1_) * 0.55f + std::sin(ph2_) * 0.3f;
            const float whine = std::sin(ph3_) * (0.03f + 0.09f * sp) + whineLp_ * 0.08f * sp;
            const float idle = 0.35f + 0.65f * std::min(1.0f, sp * 1.6f);
            s += (body * idle * 0.16f + whine) * carGain_ * (1.0f + carBoost_ * 0.25f);
        }
        s *= master_;
        s = std::tanh(s * 1.2f);
        out[i] = static_cast<short>(std::clamp(s, -1.0f, 1.0f) * 30000.0f);
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.vol == 0.0f; }), voices_.end());
}

}  // namespace sf
