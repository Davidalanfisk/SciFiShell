#pragma once
#include <stdint.h>

#define SF_TRACK_SAMPLING_INTERFACE "org.scifishell.track-sampling.v1"
#define SF_TRACK_SAMPLING_ABI 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SfTrackVec3 { float x, y, z; } SfTrackVec3;
typedef struct SfTrackFrame { SfTrackVec3 p, t, u, flat; float s; } SfTrackFrame;
typedef struct SfTrackObstacle { float x0, x1, z0, z1, top; } SfTrackObstacle;
typedef struct SfTrackStrut { SfTrackVec3 p; float top; } SfTrackStrut;
typedef struct SfTrackLoop { float begin, end; } SfTrackLoop;

// Host callbacks are borrowed for the duration of a sample call only.
typedef struct SfTrackSourceV1 {
    uint32_t size;
    uint32_t abi;
    const void* context;
    uint64_t (*count)(const void*);
    int (*frame)(const void*, uint64_t, SfTrackFrame*);
} SfTrackSourceV1;

typedef struct SfTrackRenderV1 {
    uint32_t size;
    uint32_t abi;
    void* context;
    uint64_t (*frame_count)(void*);
    int (*frame)(void*, uint64_t, SfTrackFrame*);
    uint64_t (*strut_count)(void*);
    int (*strut)(void*, uint64_t, SfTrackStrut*);
    uint64_t (*loop_count)(void*);
    int (*loop)(void*, uint64_t, SfTrackLoop*);
    int (*visible)(void*, SfTrackVec3, float);
    int (*draw)(void*, const float*, SfTrackVec3, float, float, float);
} SfTrackRenderV1;

typedef struct SfDrivingStateV1 {
    SfTrackVec3 car_pos, car_vel, rail_t, rail_u, camera_pos;
    float car_yaw, car_pitch, car_vy, car_steer, car_speed, car_spin, chase_yaw, look_x, look_y;
    float track_s, track_d, track_v, track_cool, rail_yaw, camera_yaw, camera_pitch;
    int32_t on_track, boost_on, first_person;
} SfDrivingStateV1;

typedef struct SfDrivingHostV1 {
    uint32_t size;
    uint32_t abi;
    void* context;
    int (*track_ready)(void*);
    float (*track_length)(void*);
    int (*track_sample)(void*, float, SfTrackFrame*);
    void (*resolve)(void*, SfTrackVec3*, float, float);
    float (*floor_at)(void*, SfTrackVec3, float, float);
    float (*extent)(void*);
    void (*rail_on)(void*);
} SfDrivingHostV1;

typedef struct SfCarViewV1 {
    SfTrackVec3 pos, rail_t, rail_u;
    float yaw, pitch, steer, speed, spawn, spin;
    int32_t show, hide_body, rail;
} SfCarViewV1;

typedef struct SfCarRenderV1 {
    uint32_t size;
    uint32_t abi;
    void* context;
    int (*model)(void*, const char*);
    SfTrackVec3 (*model_size)(void*, int);
    int (*mirror)(void*);
    void (*draw_model)(void*, int, const float*, SfTrackVec3, float);
    void (*draw_box)(void*, const float*, SfTrackVec3);
} SfCarRenderV1;

typedef struct SfTrackSamplingV1 {
    uint32_t size;
    uint32_t abi;
    int (*sample)(const SfTrackSourceV1*, float, SfTrackFrame*);
    int (*build)(const SfTrackObstacle*, uint64_t, float, uint32_t, void*,
                 int (*emit_frame)(void*, const SfTrackFrame*),
                 int (*emit_strut)(void*, const SfTrackStrut*),
                 int (*emit_loop)(void*, const SfTrackLoop*));
    int (*render)(const SfTrackRenderV1*, SfTrackVec3, float);
    int (*drive)(const SfDrivingHostV1*, SfDrivingStateV1*, double, float, float, int, int, float, float);
    int (*render_car)(const SfCarRenderV1*, const SfCarViewV1*, int*);
} SfTrackSamplingV1;

#ifdef __cplusplus
}
#endif
