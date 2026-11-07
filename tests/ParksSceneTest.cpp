#include "NativePlugin.h"
#include "ParksSceneApi.h"

#include <filesystem>
#include <iostream>

struct Counts { int draws = 0, ducks = 0, ponds = 0, stands = 0, sounds = 0, explosions = 0; bool alive = true; };

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    sf::NativePlugin plugin;
    std::string error;
    const auto path = std::filesystem::absolute(argv[1]);
    for (int cycle = 0; cycle != 2; ++cycle) {
        if (!plugin.load(path, "org.scifishell.parks-scene", error) || !plugin.start(error)) {
            std::cerr << error << '\n'; return 1;
        }
        const auto* api = static_cast<const SfParksSceneV1*>(plugin.getInterface(SF_PARKS_SCENE_INTERFACE));
        if (!api || api->size != sizeof(*api) || api->abi != SF_PARKS_SCENE_ABI || !api->render) return 1;
        const auto* toys = static_cast<const SfParksToysV1*>(plugin.getInterface(SF_PARKS_TOYS_INTERFACE));
        if (!toys || toys->size != sizeof(*toys) || toys->abi != SF_PARKS_SCENE_ABI) return 1;
        Counts counts;
        const SfParksSceneHostV1 host{
            sizeof(SfParksSceneHostV1), SF_PARKS_SCENE_ABI, &counts,
            [](void*) -> uint64_t { return 0; },
            [](void*, uint64_t, float*, float*) -> int { return 0; },
            [](void* ctx, uint32_t) -> int { return static_cast<Counts*>(ctx)->alive; },
            [](void* ctx, float, float, int) -> int { ++static_cast<Counts*>(ctx)->stands; return 1; },
            [](void* ctx, SfTrackVec3) -> int { ++static_cast<Counts*>(ctx)->ponds; return 1; },
            [](void* ctx, uint32_t, SfTrackVec3) -> int { ++static_cast<Counts*>(ctx)->ducks; return 1; },
            [](void* ctx, SfTrackVec3, SfTrackVec3, SfTrackVec3, float, int, float, float, float) -> int {
                ++static_cast<Counts*>(ctx)->draws; return 1;
            },
            [](void* ctx, const float*, SfTrackVec3) -> int { ++static_cast<Counts*>(ctx)->draws; return 1; }
        };
        SfParksBlockV1 block{12, 0, 0, 24, 1, {0.5f, 0.8f, 1}, {0, 2, 0}, 0, 0, 1};
        uint32_t duckSeed = 0;
        bool foundDuck = false, foundStand = false, foundPond = false;
        for (uint32_t seed = 0; seed < 1000 && !(foundDuck && foundStand && foundPond); ++seed) {
            counts = {};
            block.seed = seed;
            int draws = 0;
            check(api->render(&host, &block, &draws) && counts.draws > 0 && draws > 0, "park scenery draws");
            if (counts.ducks && !foundDuck) { duckSeed = seed; foundDuck = true; }
            foundStand |= counts.stands > 0;
            foundPond |= counts.ponds > 0;
        }
        check(foundDuck && foundStand && foundPond, "ducks, stands and ponds generated");
        counts = {}; counts.alive = false; block.seed = duckSeed;
        int draws = 0;
        check(api->render(&host, &block, &draws) && counts.ducks == 0, "dead ducks excluded");
        check(!api->render(nullptr, &block, &draws), "invalid host rejected");
        counts = {};
        const SfParksToysHostV1 toyHost{
            sizeof(SfParksToysHostV1), SF_PARKS_SCENE_ABI, &counts,
            [](void*, SfTrackVec3* p, float, float) { p->z = 0; },
            [](void*, SfTrackVec3, float, float) -> float { return 0; },
            [](void* ctx, SfTrackVec3) -> int { ++static_cast<Counts*>(ctx)->explosions; return 1; },
            [](void* ctx, int, SfTrackVec3) -> int { ++static_cast<Counts*>(ctx)->sounds; return 1; },
            [](void* ctx, const float*, SfTrackVec3) -> int { ++static_cast<Counts*>(ctx)->draws; return 1; }
        };
        check(!toys->holding(toys->context), "reload starts without held item");
        toys->give(toys->context, 0);
        check(toys->holding(toys->context), "hold hot dog");
        check(toys->render(toys->context, &toyHost, 0, {0, 2, 0}) && counts.draws > 0, "held snack draws");
        check(toys->throw_held(toys->context, &toyHost, {0, 2, 0}) &&
              !toys->holding(toys->context) && counts.sounds == 1, "throw clears item and emits sound");
        check(toys->step(toys->context, &toyHost, 5.1, {0, 0, 0}) &&
              counts.sounds == 2 && counts.explosions == 1, "thrown item explodes");
        SfTrackVec3 impulse{};
        check(toys->take_blast(toys->context, &impulse) && impulse.y > 0 &&
              !toys->take_blast(toys->context, &impulse), "blast consumed once");
        toys->give(toys->context, 1);
        check(toys->holding(toys->context), "hold ice cream");
        plugin.unload();
        check(!plugin.getInterface(SF_PARKS_SCENE_INTERFACE), "interface unavailable after unload");
    }
    return failures ? 1 : 0;
}
