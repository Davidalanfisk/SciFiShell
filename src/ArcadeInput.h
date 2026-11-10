#pragma once
#include <cmath>
#include <cstdint>
namespace sf {
struct ArcadeButtons {
    uint8_t current=0,pending=0;
    void set(uint8_t buttons) {current=buttons;pending|=buttons;}
    uint8_t sample() {const auto buttons=uint8_t(current|pending);pending=0;return buttons;}
};
struct ArcadeMenuAxis {
    int held=0;
    int step(float value) {
        if(!std::isfinite(value) || std::fabs(value)<.35f)held=0;
        if(value>.65f && held!=1) {held=1;return 1;}
        if(value<-.65f && held!=-1) {held=-1;return -1;}
        return 0;
    }
};
}
