#include "../src/MediaSeekGesture.h"

#include <cassert>
#include <cmath>

int main() {
    sf::MediaSeekGesture seek;
    double target = 0;
    auto step = [&](bool enabled, bool lp, bool rp, bool lh, bool rh, bool lr, bool rr,
                    double time, double pos, double dur = 100.0) {
        return seek.update(enabled, lp, rp, lh, rh, lr, rr, time, pos, dur, target);
    };
    auto is = [&](double value) { assert(std::fabs(target - value) < 0.00001); };

    assert(!step(true, true, false, true, false, false, false, 0, 50));
    assert(step(true, false, false, false, false, true, false, 0.2, 50)); is(40);
    assert(!step(true, false, true, false, true, false, false, 1, 95));
    assert(step(true, false, false, false, false, false, true, 1.1, 95)); is(100);
    assert(!step(true, false, true, false, true, false, false, 2, 50));
    assert(!step(true, false, false, false, true, false, false, 2.4, 50));
    assert(step(true, false, false, false, true, false, false, 2.9, 50)); is(55);
    assert(step(true, false, false, false, false, false, true, 3.4, 55)); is(60);
    assert(!step(true, false, false, false, false, false, false, 4, 60));
    assert(!step(true, true, false, true, false, false, false, 5, 1));
    assert(!step(true, false, false, true, false, false, false, 5.4, 1));
    assert(step(true, false, false, true, false, false, false, 5.6, 1)); is(0);
    assert(step(true, false, false, false, false, true, false, 5.7, 0)); is(0);
    assert(!step(true, false, true, false, true, false, false, 6, 50));
    assert(!step(false, false, false, false, true, false, false, 6.1, 50));
    assert(!step(true, false, false, false, true, false, false, 7, 50));
    assert(!step(true, false, false, false, false, false, true, 7.1, 50));
    assert(!step(true, true, false, true, false, false, false, 8, 50));
    assert(!step(true, false, true, true, true, false, false, 8.1, 50));
    assert(!step(true, false, false, true, false, false, true, 8.2, 50));
    assert(step(true, true, false, false, false, true, false, 9, 5)); is(0);
    assert(!step(true, true, false, true, false, false, false, 10, 5, 0));
}
