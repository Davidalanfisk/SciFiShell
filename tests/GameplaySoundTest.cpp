#include "Sfx.h"
#include "GameplaySoundBudget.h"
#include "GameplayAudio.h"

#include <SDL.h>
#include <iostream>
#include <limits>
#include <vector>
#include <algorithm>

namespace sf {
struct SfxTestAccess {
    static unsigned device(const Sfx& sfx) { return sfx.dev_; }
    static size_t engines(const Sfx& sfx) { return sfx.pluginEngines_.size(); }
    static size_t samples(const Sfx& sfx) { return sfx.samples_.size(); }
    static void mix(Sfx& sfx, std::vector<short>& samples) { sfx.fill(samples.data(), static_cast<int>(samples.size())); }
};
}

struct AudioLock {
    explicit AudioLock(unsigned device) : device(device) { SDL_LockAudioDevice(device); }
    ~AudioLock() { SDL_UnlockAudioDevice(device); }
    unsigned device;
};

int main() {
    sf::GameplaySoundBudget budget;
    if (!budget.consume(0, 4) || budget.consume(0, 1) ||
        !budget.consume(0.25, 1) || budget.consume(0, 1)) return 1;
    budget.reset();
    if (!budget.consume(10, 4) || budget.consume(0, 1)) return 1;
    if (SDL_setenv("SDL_AUDIODRIVER", "dummy", 1) != 0) return 1;
    sf::Sfx sfx;
    if (!sfx.init()) {
        std::cerr << "Cannot initialize dummy audio device: " << SDL_GetError() << '\n';
        return 1;
    }
    {
        AudioLock lock(sf::SfxTestAccess::device(sfx));
        if (sfx.playOwned(0, sf::SfxId::Boom, 1) ||
            sfx.playOwned(1, sf::SfxId::Boom, 2)) return 1;
        for (int i = 0; i < 25; ++i)
            if (!sfx.playOwned(i < 12 ? 1 : 2, sf::SfxId::Boom, 1)) return 1;
        if (sfx.playOwned(3, sf::SfxId::Boom, 1)) return 1;
        sfx.stopOwner(1);
        for (int i = 0; i < 12; ++i)
            if (!sfx.playOwned(3, sf::SfxId::Boom, 1)) return 1;
        if (sfx.playOwned(3, sf::SfxId::Boom, 1)) return 1;
        sfx.stopOwner(2);
        sfx.stopOwner(3);
        if (sfx.setOwnedEngine(0, true, 1, false) ||
            sfx.setOwnedEngine(1, true, -1, false) ||
            sfx.setOwnedEngine(1, true, std::numeric_limits<float>::infinity(), false)) return 1;
        for (int i = 1; i <= 4; ++i)
            if (!sfx.setOwnedEngine(i, true, 10.0f * i, i == 1)) return 1;
        if (sfx.setOwnedEngine(5, true, 10, false) || sf::SfxTestAccess::engines(sfx) != 4) return 1;
        std::vector<short> samples(8192);
        sf::SfxTestAccess::mix(sfx, samples);
        if (std::none_of(samples.begin(), samples.end(), [](short sample) { return sample != 0; })) return 1;
        sfx.stopOwner(1);
        if (sf::SfxTestAccess::engines(sfx) != 3 || !sfx.setOwnedEngine(5, true, 20, false)) return 1;
        for (int i = 2; i <= 5; ++i)
            if (!sfx.setOwnedEngine(i, false, 0, false)) return 1;
        sf::SfxTestAccess::mix(sfx, samples);
        if (std::any_of(samples.begin(), samples.end(), [](short sample) { return sample != 0; })) return 1;
        auto clip = std::make_shared<sf::GameplayAudio>();
        clip->sampleRate = 44100;
        clip->samples = {10000, 10000, 10000, 10000};
        if (!sfx.playOwnedSample(1, 1, clip, 0.5f, false)) return 1;
        sf::SfxTestAccess::mix(sfx, samples);
        if (samples[0] == 0 || samples[4] != 0 || sf::SfxTestAccess::samples(sfx) != 0) return 1;
        if (!sfx.playOwnedSample(1, 99, clip, 0.5f, false) || !sfx.loopOwnedSample(1, 99) ||
            sfx.loopOwnedSample(2, 99)) return 1;
        sf::SfxTestAccess::mix(sfx, samples);
        if (samples.back() == 0 || sf::SfxTestAccess::samples(sfx) != 1) return 1;
        sfx.stopOwnedSample(1, 99);
        for (int i = 1; i <= 4; ++i)
            if (!sfx.playOwnedSample(1, i, clip, 0.5f, true)) return 1;
        if (sfx.playOwnedSample(1, 5, clip, 0.5f, true)) return 1;
        sf::SfxTestAccess::mix(sfx, samples);
        if (samples.back() == 0 || sf::SfxTestAccess::samples(sfx) != 4) return 1;
        sfx.stopOwnedSample(1, 1);
        if (sf::SfxTestAccess::samples(sfx) != 3) return 1;
        sfx.stopOwner(1);
        if (sf::SfxTestAccess::samples(sfx) != 0) return 1;
        if (!sfx.setOwnedEngine(1, true, 20, false) || !sfx.playOwned(1, sf::SfxId::Boom, 1)) return 1;
        sfx.setCar(true, 20, true);
    }
    sfx.shutdown();
    if (sfx.playOwned(1, sf::SfxId::Boom, 1) || sf::SfxTestAccess::engines(sfx) != 0 || !sfx.init()) return 1;
    {
        AudioLock lock(sf::SfxTestAccess::device(sfx));
        std::vector<short> samples(8192);
        sf::SfxTestAccess::mix(sfx, samples);
        if (std::any_of(samples.begin(), samples.end(), [](short sample) { return sample != 0; })) return 1;
    }
    sfx.shutdown();
    SDL_Quit();
    return 0;
}
