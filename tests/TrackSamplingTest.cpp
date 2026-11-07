#include "NativePlugin.h"
#include "ParksSceneApi.h"
#include "TrackSamplingApi.h"
#include "Utils.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    std::vector<SfTrackFrame> frames{
        {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 0, 0}, 0},
        {{10, 4, 0}, {0, 0, 1}, {0, 1, 0}, {0, 0, 1}, 10},
        {{30, 8, 0}, {0, 0, 1}, {0, 1, 0}, {0, 0, 1}, 30}
    };
    SfTrackSourceV1 source{
        sizeof(SfTrackSourceV1), SF_TRACK_SAMPLING_ABI, &frames,
        [](const void* ctx) -> uint64_t { return static_cast<const std::vector<SfTrackFrame>*>(ctx)->size(); },
        [](const void* ctx, uint64_t index, SfTrackFrame* out) -> int {
            const auto& frames = *static_cast<const std::vector<SfTrackFrame>*>(ctx);
            if (!out || index >= frames.size()) return 0;
            *out = frames[static_cast<size_t>(index)];
            return 1;
        }
    };
    sf::NativePlugin module;
    sf::NativePlugin parks;
    std::string error;
    const auto path = std::filesystem::absolute(argv[1]);
    if (!parks.load(std::filesystem::absolute(argv[2]), "org.scifishell.parks-scene", error) || !parks.start(error)) {
        std::cerr << error << '\n'; return 1;
    }
    for (int cycle = 0; cycle != 2; ++cycle) {
        if (!module.load(path, "org.scifishell.track-sampling", error) || !module.start(error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const auto* api = static_cast<const SfTrackSamplingV1*>(module.getInterface(SF_TRACK_SAMPLING_INTERFACE));
        if (!api || api->abi != SF_TRACK_SAMPLING_ABI || api->size != sizeof(*api) ||
            !api->sample || !api->build || !api->render || !api->drive || !api->render_car) return 1;
        SfTrackFrame out{};
        check(api->sample(&source, -5, &out) && out.s == 0 && out.p.x == 0, "clamp start");
        check(api->sample(&source, 100, &out) && out.s == 30 && out.p.x == 30, "clamp end");
        check(api->sample(&source, 5, &out) && out.p.x == 5 && out.p.y == 2 &&
              std::fabs(out.t.x - 0.70710678f) < 1e-5f && std::fabs(out.t.z - 0.70710678f) < 1e-5f,
              "interpolation and orientation");
        check(api->sample(&source, 20, &out) && out.p.x == 20 && out.p.y == 6, "later segment");
        for (int step = -10; step <= 310; ++step) {
            const float s = std::clamp(step / 10.0f, 0.0f, 30.0f);
            const size_t lo = s < 10 ? 0 : 1;
            const auto& a = frames[lo];
            const auto& b = frames[lo + 1];
            const float u = (s - a.s) / (b.s - a.s);
            const sf::Vec3 t = sf::normalize(sf::Vec3{a.t.x, a.t.y, a.t.z} +
                (sf::Vec3{b.t.x, b.t.y, b.t.z} - sf::Vec3{a.t.x, a.t.y, a.t.z}) * u);
            check(api->sample(&source, step / 10.0f, &out) && std::fabs(out.s - s) < 1e-5f &&
                  std::fabs(out.p.x - (a.p.x + (b.p.x - a.p.x) * u)) < 1e-5f &&
                  std::fabs(out.t.x - t.x) < 1e-5f && std::fabs(out.t.z - t.z) < 1e-5f,
                  "sample matches original interpolation");
        }
        auto shortSource = source;
        shortSource.count = [](const void*) -> uint64_t { return 1; };
        check(!api->sample(&shortSource, 0, &out), "short track rejected");
        check(!api->sample(nullptr, 0, &out), "invalid source rejected");
        struct Generated {
            std::vector<SfTrackFrame> frames;
            std::vector<SfTrackStrut> struts;
            std::vector<SfTrackLoop> loops;
        } generated;
        const SfTrackObstacle obstacles[] = {{-10, 10, -10, 10, 20}, {100, 150, 100, 150, 40}};
        const auto frame = [](void* ctx, const SfTrackFrame* f) -> int {
            static_cast<Generated*>(ctx)->frames.push_back(*f); return 1;
        };
        const auto strut = [](void* ctx, const SfTrackStrut* s) -> int {
            static_cast<Generated*>(ctx)->struts.push_back(*s); return 1;
        };
        const auto loop = [](void* ctx, const SfTrackLoop* l) -> int {
            static_cast<Generated*>(ctx)->loops.push_back(*l); return 1;
        };
        check(api->build(obstacles, 2, 300, 1, &generated, frame, strut, loop) &&
              generated.frames.size() > 100 && generated.frames.back().s > 1000,
              "generate a complete track");
        check(!generated.frames.empty() && generated.frames.front().s == 0 &&
              std::isfinite(generated.frames.back().s), "generated track has valid arc length");
        Generated repeat;
        check(api->build(obstacles, 2, 300, 1, &repeat, frame, strut, loop) &&
              repeat.frames.size() == generated.frames.size() &&
              repeat.struts.size() == generated.struts.size() &&
              repeat.loops.size() == generated.loops.size() &&
              !repeat.frames.empty() && repeat.frames.back().s == generated.frames.back().s,
              "generation is deterministic");
        const auto failFrame = [](void*, const SfTrackFrame*) -> int { return 0; };
        check(!api->build(obstacles, 2, 300, 1, &repeat, failFrame, strut, loop),
              "output rejection propagates");
        check(!api->build(nullptr, 2, 300, 1, &repeat, frame, strut, loop),
              "invalid obstacle input rejected");
        struct RenderContext { Generated* data; int draws; bool fail; } render{&generated, 0, false};
        SfTrackRenderV1 renderer{
            sizeof(SfTrackRenderV1), SF_TRACK_SAMPLING_ABI, &render,
            [](void* ctx) -> uint64_t { return static_cast<const RenderContext*>(ctx)->data->frames.size(); },
            [](void* ctx, uint64_t i, SfTrackFrame* f) -> int {
                const auto& v = static_cast<const RenderContext*>(ctx)->data->frames;
                if (!f || i >= v.size()) return 0; *f = v[static_cast<size_t>(i)]; return 1;
            },
            [](void* ctx) -> uint64_t { return static_cast<const RenderContext*>(ctx)->data->struts.size(); },
            [](void* ctx, uint64_t i, SfTrackStrut* s) -> int {
                const auto& v = static_cast<const RenderContext*>(ctx)->data->struts;
                if (!s || i >= v.size()) return 0; *s = v[static_cast<size_t>(i)]; return 1;
            },
            [](void* ctx) -> uint64_t { return static_cast<const RenderContext*>(ctx)->data->loops.size(); },
            [](void* ctx, uint64_t i, SfTrackLoop* l) -> int {
                const auto& v = static_cast<const RenderContext*>(ctx)->data->loops;
                if (!l || i >= v.size()) return 0; *l = v[static_cast<size_t>(i)]; return 1;
            },
            [](void*, SfTrackVec3, float) -> int { return 1; },
            [](void* ctx, const float* m, SfTrackVec3, float, float, float) -> int {
                auto* c = static_cast<RenderContext*>(ctx);
                ++c->draws;
                return !c->fail && m && m[15] == 1;
            }
        };
        check(api->render(&renderer, {0, 0, 0}, 0) && render.draws == 0, "hidden track emits no draws");
        check(api->render(&renderer, {0, 0, 0}, 1) && render.draws > 0, "visible track emits geometry");
        render.fail = true;
        check(!api->render(&renderer, {0, 0, 0}, 1), "draw failure propagates");
        struct DriveHost { int railEvents = 0; } driveHost;
        const SfDrivingHostV1 driving{
            sizeof(SfDrivingHostV1), SF_TRACK_SAMPLING_ABI, &driveHost,
            [](void*) -> int { return 0; },
            [](void*) -> float { return 30; },
            [](void*, float s, SfTrackFrame* f) -> int {
                if (!f) return 0;
                *f = {{0, 0, -s}, {0, 0, -1}, {0, 1, 0}, {0, 0, -1}, s};
                return 1;
            },
            [](void*, SfTrackVec3*, float, float) {},
            [](void*, SfTrackVec3, float, float) -> float { return 0; },
            [](void*) -> float { return 300; },
            [](void* ctx) { ++static_cast<DriveHost*>(ctx)->railEvents; }
        };
        SfDrivingStateV1 car{};
        car.rail_t = {0, 0, -1}; car.rail_u = {0, 1, 0};
        for (int i = 0; i < 60; ++i)
            check(api->drive(&driving, &car, 1.0 / 60.0, 1, 0, 0, 0, 0, 0), "driving update");
        check(car.car_pos.z < -10 && car.car_speed > 20 && car.car_pos.y == 0 &&
              std::isfinite(car.camera_pos.z), "car accelerates on flat ground");
        car.on_track = 1; car.track_s = 5; car.track_v = 40;
        check(api->drive(&driving, &car, 1.0 / 60.0, 1, 0, 0, 0, 0, 0) &&
              car.on_track && car.track_s > 5 && car.car_pos.z < -5,
              "rail simulation uses track service");
        check(!api->drive(nullptr, &car, 0.1, 0, 0, 0, 0, 0, 0), "invalid driving host rejected");
        struct CarRender { int models = 0, boxes = 0; bool mirror = false; } carRender;
        const SfCarRenderV1 carHost{
            sizeof(SfCarRenderV1), SF_TRACK_SAMPLING_ABI, &carRender,
            [](void*, const char*) -> int { return 1; },
            [](void*, int) -> SfTrackVec3 { return {2, 1, 4}; },
            [](void* ctx) -> int { return static_cast<CarRender*>(ctx)->mirror; },
            [](void* ctx, int, const float*, SfTrackVec3, float) { ++static_cast<CarRender*>(ctx)->models; },
            [](void* ctx, const float*, SfTrackVec3) { ++static_cast<CarRender*>(ctx)->boxes; }
        };
        SfCarViewV1 carView{};
        int model = -2;
        check(api->render_car(&carHost, &carView, &model) && carRender.models == 0 &&
              carRender.boxes == 0 && model == -2, "hidden car draws nothing");
        carView.show = 1; carView.spawn = 1;
        check(api->render_car(&carHost, &carView, &model) && model == 1 &&
              carRender.models == 5 && carRender.boxes == 1, "car and wheels draw");
        carView.hide_body = 1; carRender.models = 0;
        check(api->render_car(&carHost, &carView, &model) && carRender.models == 0, "first person hides body");
        carRender.mirror = true;
        check(api->render_car(&carHost, &carView, &model) && carRender.models == 5, "mirror restores car body");
        module.unload();
        check(!module.getInterface(SF_TRACK_SAMPLING_INTERFACE), "interface unavailable after unload");
        check(parks.running() && parks.getInterface(SF_PARKS_TOYS_INTERFACE), "parks unaffected by driving unload");
    }
    parks.unload();
    return failures ? 1 : 0;
}
