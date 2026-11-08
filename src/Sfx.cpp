#include "Sfx.h"
#include "GameplayAudio.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <new>

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
    std::lock_guard<std::mutex> lk(mu_);
    voices_.clear();
    samples_.clear();
    pluginEngines_.clear();
    carEngine_ = {};
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

bool Sfx::playOwned(std::uint64_t owner, SfxId id, float volume) {
    if (!owner || !dev_ || !std::isfinite(volume) || volume < 0 || volume > 1) return false;
    if (volume <= 0.01f) return true;
    std::lock_guard<std::mutex> lk(mu_);
    if (voices_.size() >= 25) return false;
    Voice v;
    v.id = id;
    v.vol = volume;
    v.owner = owner;
    v.seed = 977u + static_cast<unsigned>(SDL_GetTicks()) * 31u;
    try { voices_.push_back(v); }
    catch (const std::bad_alloc&) { return false; }
    return true;
}

bool Sfx::playOwnedSample(std::uint64_t owner, int voice, std::shared_ptr<const GameplayAudio> sample,
                          float volume, bool loop) {
    if (!owner || voice <= 0 || !dev_ || !sample || sample->samples.empty() ||
        sample->sampleRate < 8000 || sample->sampleRate > 48000 ||
        !std::isfinite(volume) || volume < 0 || volume > 1) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (samples_.size() >= 16 ||
        std::count_if(samples_.begin(), samples_.end(), [owner](const SampleVoice& v) {
            return v.owner == owner;
        }) >= 4 ||
        std::any_of(samples_.begin(), samples_.end(), [owner, voice](const SampleVoice& v) {
            return v.owner == owner && v.id == voice;
        })) return false;
    try { samples_.push_back({owner, voice, std::move(sample), 0, volume, loop, false}); }
    catch (const std::bad_alloc&) { return false; }
    return true;
}

void Sfx::stopOwnedSample(std::uint64_t owner, int voice) {
    std::lock_guard<std::mutex> lk(mu_);
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(), [owner, voice](const SampleVoice& v) {
        return v.owner == owner && v.id == voice;
    }), samples_.end());
}

bool Sfx::loopOwnedSample(std::uint64_t owner, int voice) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& sample : samples_) {
        if (sample.owner == owner && sample.id == voice) {
            sample.loop = true;
            return true;
        }
    }
    return false;
}

void Sfx::stopOwner(std::uint64_t owner) {
    if (!owner) return;
    std::lock_guard<std::mutex> lk(mu_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [owner](const Voice& v) { return v.owner == owner; }), voices_.end());
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(),
                                  [owner](const SampleVoice& v) { return v.owner == owner; }), samples_.end());
    pluginEngines_.erase(std::remove_if(pluginEngines_.begin(), pluginEngines_.end(),
                                        [owner](const Engine& e) { return e.owner == owner; }), pluginEngines_.end());
}

#ifdef SCIFI_TEST_SYNTHETIC_CITY
bool Sfx::testHasOwnedEngine(std::uint64_t owner) {
    std::lock_guard<std::mutex> lk(mu_);
    return std::any_of(pluginEngines_.begin(), pluginEngines_.end(),
                       [owner](const Engine& e) { return e.owner == owner; });
}
std::size_t Sfx::testOwnedSampleCount(std::uint64_t owner) {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<std::size_t>(std::count_if(samples_.begin(), samples_.end(),
        [owner](const SampleVoice& sample) { return sample.owner == owner; }));
}
#endif

bool Sfx::setOwnedEngine(std::uint64_t owner, bool active, float speed, bool boost) {
    if (!owner || !std::isfinite(speed) || speed < 0 || speed > 110) return false;
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(pluginEngines_.begin(), pluginEngines_.end(),
                           [owner](const Engine& e) { return e.owner == owner; });
    if (!active) {
        if (it != pluginEngines_.end()) pluginEngines_.erase(it);
        return true;
    }
    if (!dev_) return false;
    if (it == pluginEngines_.end()) {
        if (pluginEngines_.size() >= 4) return false;
        try { pluginEngines_.push_back({}); }
        catch (const std::bad_alloc&) { return false; }
        it = pluginEngines_.end() - 1;
        it->owner = owner;
    }
    it->target = 1;
    it->speedTarget = speed;
    it->boostTarget = boost ? 1.0f : 0.0f;
    return true;
}

void Sfx::setCar(bool driving, float speed, bool boost) {
    std::lock_guard<std::mutex> lk(mu_);
    carEngine_.target = driving ? 1.0f : 0.0f;
    carEngine_.speedTarget = std::min(std::fabs(speed), 110.0f);
    carEngine_.boostTarget = boost ? 1.0f : 0.0f;
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

float Sfx::engineSample(Engine& engine) {
    const float k = 1.0f / kRate;
    engine.gain += (engine.target - engine.gain) * 3.0f * k;
    engine.speed += (engine.speedTarget - engine.speed) * 2.5f * k;
    engine.boost += (engine.boostTarget - engine.boost) * 4.0f * k;
    if (engine.gain <= 0.002f) return 0;
    const float sp = engine.speed / 100.0f;
    const float f = 42.0f + sp * 78.0f + engine.boost * 8.0f;
    engine.phase1 += kTau * f * k;
    engine.phase2 += kTau * f * 2.0f * k;
    const float raw = std::sin(engine.phase1) * 0.7f + std::sin(engine.phase2) * 0.25f * (0.3f + sp);
    engine.lowpass += (raw - engine.lowpass) * 0.04f;
    const float idle = 0.45f + 0.55f * std::min(1.0f, sp * 1.6f);
    return engine.lowpass * idle * 0.2f * engine.gain * (1.0f + engine.boost * 0.2f);
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
        for (auto& sample : samples_) {
            if (sample.finished) continue;
            s += sample.sample->samples[static_cast<size_t>(sample.position)] * (sample.volume / 32768.0f);
            sample.position += static_cast<double>(sample.sample->sampleRate) / kRate;
            if (sample.position >= sample.sample->samples.size()) {
                if (sample.loop) sample.position = std::fmod(sample.position, sample.sample->samples.size());
                else sample.finished = true;
            }
        }
        s += engineSample(carEngine_);
        for (auto& engine : pluginEngines_) s += engineSample(engine);
        s *= master_;
        s = std::tanh(s * 1.2f);
        out[i] = static_cast<short>(std::clamp(s, -1.0f, 1.0f) * 30000.0f);
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.vol == 0.0f; }), voices_.end());
    samples_.erase(std::remove_if(samples_.begin(), samples_.end(), [](const SampleVoice& v) { return v.finished; }),
                   samples_.end());
}

}  // namespace sf
