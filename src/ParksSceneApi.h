#pragma once
#include "TrackSamplingApi.h"

#define SF_PARKS_SCENE_INTERFACE "org.scifishell.parks-scene.v1"
#define SF_PARKS_TOYS_INTERFACE "org.scifishell.parks-toys.v1"
#define SF_PARKS_SCENE_ABI 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SfParksSceneHostV1 {
    uint32_t size;
    uint32_t abi;
    void* context;
    uint64_t (*tower_count)(void*);
    int (*tower)(void*, uint64_t, float*, float*);
    int (*duck_alive)(void*, uint32_t);
    int (*stand)(void*, float, float, int);
    int (*pond)(void*, SfTrackVec3);
    int (*duck)(void*, uint32_t, SfTrackVec3);
    int (*draw_box)(void*, SfTrackVec3, SfTrackVec3, SfTrackVec3, float, int, float, float, float);
    int (*draw_box_matrix)(void*, const float*, SfTrackVec3);
} SfParksSceneHostV1;

typedef struct SfParksBlockV1 {
    float pitch, cx, cz, side;
    int32_t grid;
    SfTrackVec3 tint, eye;
    double wall;
    uint32_t seed;
    int32_t detail;
} SfParksBlockV1;

typedef struct SfParksSceneV1 {
    uint32_t size;
    uint32_t abi;
    int (*render)(const SfParksSceneHostV1*, const SfParksBlockV1*, int*);
} SfParksSceneV1;

typedef struct SfParksToysHostV1 {
    uint32_t size;
    uint32_t abi;
    void* context;
    void (*resolve)(void*, SfTrackVec3*, float, float);
    float (*floor_at)(void*, SfTrackVec3, float, float);
    int (*pop_ducks)(void*, SfTrackVec3);
    int (*sound)(void*, int, SfTrackVec3);
    int (*draw_box)(void*, const float*, SfTrackVec3);
} SfParksToysHostV1;

typedef struct SfParksToysV1 {
    uint32_t size;
    uint32_t abi;
    void* context;
    void (*give)(void*, int);
    int (*holding)(void*);
    void (*set_look)(void*, SfTrackVec3);
    int (*throw_held)(void*, const SfParksToysHostV1*, SfTrackVec3);
    int (*step)(void*, const SfParksToysHostV1*, double, SfTrackVec3);
    int (*render)(void*, const SfParksToysHostV1*, double, SfTrackVec3);
    int (*take_blast)(void*, SfTrackVec3*);
} SfParksToysV1;

#ifdef __cplusplus
}
#endif
