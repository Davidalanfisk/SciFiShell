#pragma once
#include <stdint.h>

#define SF_ARCADE_INTERFACE "org.scifishell.arcade.v1"
#define SF_ARCADE_ABI 1u
#define SF_ARCADE_WIDTH 320
#define SF_ARCADE_HEIGHT 240
#define SF_ARCADE_CABINETS 12

// Borrowed buffers, synchronous calls; no ownership crosses the native ABI.
struct SfArcadeInput {
    float x, y;
    int fire, alternate;
};
struct SfArcadeState {
    int cabinet, game, score, lives, over;
    uint64_t ticks;
};
struct SfArcadeV1 {
    uint32_t size, abi;
    void* context;
    int (*begin)(void*, int cabinet, int game, int difficulty);
    void (*end)(void*);
    void (*step)(void*, double, SfArcadeInput);
    SfArcadeState (*state)(void*);
    void (*paint)(void*, int game, double attractTime, uint8_t* rgba);
};
