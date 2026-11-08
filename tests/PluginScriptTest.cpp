#include "PluginScript.h"
#include "GameplayCellGrid.h"
#include "GameplayControlLease.h"
#include "GameplayControlMotion.h"
#include "GameplayTrackClearance.h"
#include "GameplayClickRouter.h"

#include <iostream>
#include <limits>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    std::string result, error;
    struct Tower { double x, z; };
    std::vector<Tower> towers{{-14, -14}, {14, 14}};
    double cell[5]{};
    check(sf::gameplayCellInGrid(-14, -14, 0, 0, 28, 2, towers, cell) == 1 &&
          cell[0] == -14 && cell[2] == -14 && cell[3] == 28 && cell[4] == 1,
          "city grid identifies occupied cell and center");
    check(sf::gameplayCellInGrid(14, -14, 0, 0, 28, 2, towers, cell) == 1 &&
          cell[0] == 14 && cell[4] == 0, "city grid identifies vacant cell");
    check(sf::gameplayCellInGrid(28, 0, 0, 0, 28, 2, towers, cell) == 0 &&
          sf::gameplayCellInGrid(-28, -14, 0, 0, 28, 2, towers, cell) == 1,
          "city grid has half-open outer boundaries");
    towers.resize(4097);
    check(sf::gameplayCellInGrid(0, 0, 0, 0, 28, 2, towers, cell) == -1 &&
          sf::gameplayCellInGrid(0, 0, 0, 0, 0, 2, towers, cell) == -1,
          "city grid rejects excessive towers and invalid pitch");
    struct TrackTower { double x, z, curW, tgtW, curD, tgtD, curH, tgtH; };
    struct TrackBlock { std::vector<TrackTower> towers; };
    struct TrackPlatform { bool gone; double grow, cx, cz, side; std::vector<TrackBlock> blocks; };
    struct TrackMonument { double x0, x1, z0, z1, y1; };
    std::vector<TrackPlatform> platforms{{false, 1, 0, 0, 100,
                                          {{{{20, 0, 4, 8, 4, 6, 10, 30}}}}}};
    std::vector<TrackMonument> monuments{{-2, 2, -2, 2, 40}};
    double clearance = -1;
    check(sf::gameplayTrackClearance(platforms, monuments, 0, 0, 0, clearance) && clearance == 40 &&
          sf::gameplayTrackClearance(platforms, monuments, 25.5, 0, 1, clearance) &&
          clearance > 33.2 && clearance < 33.4 &&
          sf::gameplayTrackClearance(platforms, monuments, 200, 200, 100, clearance) && clearance == 0,
          "track clearance matches native monument and target tower footprints without distance culling");
    platforms[0].gone = true;
    platforms[0].grow = 0;
    check(sf::gameplayTrackClearance(platforms, monuments, 20, 0, 0, clearance) && clearance == 0,
          "track clearance excludes fully removed platforms");
    platforms[0].gone = false;
    platforms[0].blocks[0].towers.resize(4097);
    check(!sf::gameplayTrackClearance(platforms, monuments, 0, 0, 1, clearance) &&
          !sf::gameplayTrackClearance(platforms, monuments, 0, 0, 101, clearance),
          "track clearance rejects excessive work and radius");
    check(sf::runPluginScript("return 'HELLO ' .. (2 + 3)", result, error) && result == "HELLO 5", "valid execution");
    check(sf::runPluginScript("if unix_time() > 0 then return 'OK' end", result, error) && result == "OK", "clock API");
    check(!sf::runPluginScript("return os.execute('x')", result, error), "no OS library");
    check(!sf::runPluginScript("return require('io')", result, error), "no module loader");
    check(!sf::runPluginScript("while true do end", result, error) && error.find("instruction limit") != std::string::npos, "instruction budget");
    check(!sf::runPluginScript("local a='xx' for i=1,20 do a=a..a end return 'ok'", result, error) &&
          error.find("memory") != std::string::npos, "memory budget");
    check(!sf::runPluginScript("return 42", result, error), "non-string result rejected");
    check(!sf::runPluginScript("return '\\n'", result, error), "control characters rejected");
    check(!sf::runPluginScript(std::string("\x1bLua", 4), result, error), "bytecode rejected");
    check(sf::runPluginScript("return 'AFTER FAILURE'", result, error), "fresh runtime after failure");
    sf::PluginScriptRuntime runtime;
    {
        sf::GameplayClickRouter router;
        router.begin(true, true, 9);
        check(router.consume(true) && router.owner() == 9, "interactive press is routed and consumed");
        router.begin(true, true, 12);
        check(router.consume(true) && router.owner() == 9, "repeated down retains original owner");
        router.forget(9);
        check(router.consume(true) && router.owner() == 0, "stopped owner cannot leak held click to shell");
        router.finish(true);
        router.begin(true, true, 0);
        check(!router.consume(true) && router.owner() == 0, "miss leaves native click unconsumed");
        router.begin(false, false, 0);
        router.begin(true, true, 12);
        check(router.consume(true) && router.owner() == 12, "focus loss resets held input");
    }
    check(!runtime.start("scene_interactive(1,true) return {}", error, true),
          "interactive targets require input permission");
    check(!runtime.start("input_capture_clicks(true) return {}", error, true, nullptr, nullptr, nullptr,
                         false, false, nullptr, false, false, true),
          "click capture cannot be acquired without a routed press");
    check(runtime.start("local a=scene_box(0,0,-8,2,2,2,1,1,1) "
                        "local b=scene_box(0,0,-4,2,2,2,1,1,1) scene_interactive(a,true) "
                        "scene_interactive(b,true) return {on_input=function(k,v) "
                        "if k=='click' and v==1 then input_capture_clicks(true) end "
                        "if k=='click' and v==0 then input_capture_clicks(false) end end}",
                        error, true, nullptr, nullptr, nullptr, false, false, nullptr, false, false, true),
          "interactive targets start with scene and input permissions");
    const double rayOrigin[] = {0, 0, 0}, rayDirection[] = {0, 0, -1}, missDirection[] = {1, 0, 0};
    double hitDistance = 0;
    check(runtime.interactiveHit(rayOrigin, rayDirection, hitDistance) && hitDistance == 3 &&
          !runtime.interactiveHit(rayOrigin, missDirection, hitDistance) &&
          runtime.input("click", 1, error) && runtime.capturesClicks() &&
          runtime.input("click", 0, error) && !runtime.capturesClicks() && runtime.stop(error) &&
          !runtime.interactiveHit(rayOrigin, rayDirection, hitDistance),
          "nearest interactive hit, miss, capture release and stop cleanup");
    const std::string controlScript =
        "return {on_start=function() control_offer(1,2,3) end,"
        "on_tick=function() control_pose(4,5,6,0.2,0.3) end,"
        "on_input=function(k,v) if k=='control' and v==0 then control_offer(nil) end end}";
    check(!runtime.start(controlScript, error), "control host functions are absent without grant");
    check(runtime.start(controlScript, error, false, nullptr, nullptr, nullptr, false, false, nullptr, false, true) &&
          runtime.controlOffer().available && runtime.controlOffer().x == 1 &&
          runtime.setControlActive(true, error) && runtime.tick(0.1, error),
          "granted script can offer and propose control");
    auto proposal = runtime.takeControlPose();
    check(proposal.pending && proposal.x == 4 && proposal.pitch == 0.3 &&
          !runtime.takeControlPose().pending, "control proposal is consumed once");
    check(runtime.setControlActive(false, error) && !runtime.controlOffer().available &&
          runtime.stop(error) && !runtime.controlOffer().available && !runtime.takeControlPose().pending,
          "control offer can be withdrawn and stop clears state");
    check(runtime.start("local n=0 return {on_input=function(k,v) "
                        "if k=='control' then n=n+1 if n>2 then missing() end "
                        "if v==1 then control_pose(1,2,3,0,0) end end end}",
                        error, false, nullptr, nullptr, nullptr, false, false, nullptr, false, true) &&
          runtime.setControlActive(true, error) && runtime.setControlActive(true, error) &&
          runtime.takeControlPose().pending && runtime.setControlActive(false, error) &&
          runtime.setControlActive(false, error) && runtime.running(),
          "host control transitions dispatch once and repeated transitions are idempotent");
    check(!runtime.setControlActive(true, error) && !runtime.running() &&
          !runtime.takeControlPose().pending, "control event callback failure clears the runtime");
    check(runtime.start("return {}", error) && !runtime.setControlActive(true, error) &&
          runtime.running() && runtime.stop(error), "host cannot activate control without runtime grant");
    {
        sf::GameplayControlLease lease;
        using Pose = sf::GameplayControlLease::Pose;
        const Pose camera{0, 1.7, 0, 0, 0};
        const std::string source =
            "return {on_start=function() control_offer(1,1.7,0) end,"
            "on_tick=function() control_pose(2,1.7,0,0.2,0) end,"
            "on_input=function(k,v) if k=='control' and v==0 then control_offer(nil) end end}";
        check(runtime.start(source, error, false, nullptr, nullptr, nullptr, false, false, nullptr, false, true),
              "start control arbitration integration fixture");
        const auto offer = runtime.controlOffer();
        check(offer.available && lease.acquire(42, true, true, true, camera,
                                               Pose{offer.x, offer.y, offer.z, 0, 0}) &&
              runtime.setControlActive(true, error) && runtime.tick(0.1, error),
              "explicit host activation grants advertised nearby control");
        const auto requested = runtime.takeControlPose();
        check(requested.pending && lease.accepts(42, true, camera,
              Pose{requested.x, requested.y, requested.z, requested.yaw, requested.pitch}, 0.1),
              "host arbitrates granted script motion proposal");
        check(!lease.accepts(42, false, camera, camera, 0.1) && lease.owner() == 0 &&
              runtime.setControlActive(false, error) && !runtime.controlOffer().available &&
              !runtime.takeControlPose().pending && runtime.stop(error),
              "unavailable context revokes host lease and notifies script");
    }
    {
        sf::GameplayControlLease lease;
        using Pose = sf::GameplayControlLease::Pose;
        using Result = sf::GameplayControlMotionResult;
        const Pose camera{0, 1.7, 0, 0, 0};
        Pose resolved{};
        int calls = 0;
        check(lease.acquire(42, true, true, true, camera, camera), "acquire collision test lease");
        const auto wall = [&](Pose& p) {
            ++calls;
            p.x = std::min(p.x, 0.3);
            return true;
        };
        check(sf::resolveGameplayControlMotion(lease, 42, true, camera, Pose{1,1.7,0,0.2,0},
              0.1, 100, wall, resolved) == Result::Applied && calls == 4 &&
              resolved.x <= 0.3 && resolved.yaw == 0.2,
              "host substeps a valid proposal and stops at a synthetic wall");
        const auto tallStep = [](Pose& p) { p.y += 2; return true; };
        check(sf::resolveGameplayControlMotion(lease, 42, true, camera, Pose{0.1,1.7,0,0,0},
              0.01, 100, tallStep, resolved) == Result::Applied && resolved.x == camera.x &&
              resolved.y == camera.y && lease.owner() == 42,
              "large collision adjustment blocks motion without failing the plugin");
        const auto edge = [](Pose& p) { p.x = 101; return true; };
        check(sf::resolveGameplayControlMotion(lease, 42, true, camera, Pose{0.1,1.7,0,0,0},
              0.01, 100, edge, resolved) == Result::Applied && resolved.x == camera.x,
              "collision adjustment cannot move beyond city bounds");
        const auto badWorld = [](Pose& p) { p.y = std::numeric_limits<double>::quiet_NaN(); return true; };
        check(sf::resolveGameplayControlMotion(lease, 42, true, camera, camera,
              0.1, 100, badWorld, resolved) == Result::InvalidWorld,
              "invalid collision result fails explicitly");
        calls = 0;
        check(sf::resolveGameplayControlMotion(lease, 42, true, camera, Pose{3,1.7,0,0,0},
              0.1, 100, wall, resolved) == Result::InvalidProposal && calls == 0,
              "excessive proposal is rejected before collision queries");
    }
    check(!runtime.start("control_offer(0/0,0,0) return {}", error, false, nullptr, nullptr, nullptr,
                         false, false, nullptr, false, true) && !runtime.running(),
          "nonfinite control offer fails and clears state");
    check(runtime.start("return {on_tick=function() control_pose(0,0,0,0,1.5) end}", error,
                        false, nullptr, nullptr, nullptr, false, false, nullptr, false, true) &&
          runtime.setControlActive(true, error) && !runtime.tick(0.1, error) && !runtime.running(),
          "out of range control pitch fails");
    check(runtime.start("return {on_tick=function() control_pose(0,0,0,0,0) "
                        "control_pose(0,0,0,0,0) end}", error, false, nullptr, nullptr, nullptr,
                        false, false, nullptr, false, true) && runtime.setControlActive(true, error) &&
          !runtime.tick(0.1, error) && !runtime.running() && !runtime.takeControlPose().pending,
          "control queue overflow cleans up");
    check(!runtime.start("control_pose(0,0,0,0,0) return {}", error, false, nullptr, nullptr, nullptr,
                         false, false, nullptr, false, true) && !runtime.running(),
          "script cannot propose movement before host activation");
    check(!runtime.tick(0.1, error), "tick before start rejected");
    check(runtime.start("local elapsed=0 return {on_start=function() elapsed=1 end,"
                        "on_tick=function(dt) elapsed=elapsed+dt if elapsed>2 then missing() end end}",
                        error), "lifecycle start");
    check(runtime.running(), "runtime reports running");
    check(!runtime.start("return {}", error), "duplicate start rejected");
    check(runtime.tick(0.5, error) && runtime.tick(0.5, error), "persistent callback state");
    check(!runtime.tick(0.1, error) && !runtime.running(), "callback failure closes state");
    check(runtime.start("return {on_tick=function() while true do end end}", error), "restart after callback failure");
    check(!runtime.tick(0.1, error) && error.find("instruction limit") != std::string::npos && !runtime.running(),
          "callback instruction limit closes state");
    check(!runtime.start("return {on_tick=42}", error) && !runtime.running(), "invalid callback rejected at start");
    check(!runtime.start("return {on_start=function() missing() end}", error) && !runtime.running(), "start failure closes state");
    check(runtime.start("return {on_stop=function() while true do end end}", error), "start for stop failure");
    check(!runtime.stop(error) && !runtime.running(), "stop failure closes state");
    check(runtime.start("return {}", error), "restart after stop failure");
    check(!runtime.tick(std::numeric_limits<double>::quiet_NaN(), error) && runtime.running(), "invalid delta rejected");
    check(!runtime.tick(1.01, error) && runtime.running(), "excessive delta rejected");
    check(runtime.stop(error) && !runtime.running() && runtime.stop(error), "stop is idempotent");
    check(!runtime.start("return os.execute('x')", error), "lifecycle has no OS library");
    check(!runtime.start("while true do end", error) && error.find("instruction limit") != std::string::npos,
          "startup instruction limit");
    check(runtime.start("local s='xx' return {on_tick=function() for i=1,20 do s=s..s end end}", error),
          "start for lifecycle allocation limit");
    check(!runtime.tick(0.1, error) && !runtime.running() && error.find("memory") != std::string::npos,
          "callback allocation limit closes state");
    check(runtime.start("local t={} t.on_tick=function() t.on_tick=42 end return t", error),
          "start with mutable callbacks");
    check(runtime.tick(0.1, error), "mutate callback");
    check(!runtime.tick(0.1, error) && !runtime.running(), "invalid mutated callback closes state");
    check(runtime.start("local n=0 return {on_tick=function() for i=1,1000 do n=n+1 end end}", error),
          "start for repeated callbacks");
    bool ticksOk = true;
    for (int i = 0; i < 1000 && ticksOk; ++i) ticksOk = runtime.tick(0.016, error);
    check(ticksOk && runtime.stop(error), "callback budget resets and stack remains bounded");
    check(!runtime.start("scene_box(0,0,0,1,1,1,1,1,1) return {}", error),
          "scene capabilities are absent without grant");
    check(runtime.start("local id=scene_box(1,2,3,4,5,6,0.2,0.3,0.4) "
                        "return {on_tick=function() scene_remove(id) end}", error, true),
          "create granted box");
    auto boxes = runtime.boxes();
    check(boxes.size() == 1 && boxes[0].x == 1 && boxes[0].height == 5 && boxes[0].blue == 0.4,
          "box values stored");
    check(runtime.tick(0.1, error) && runtime.boxes().empty(), "remove owned box");
    check(runtime.stop(error) && runtime.boxes().empty(), "stop clears owned scene");
    check(!runtime.start("scene_box(0,0,0,0,1,1,1,1,1) return {}", error, true) && runtime.boxes().empty(),
          "invalid box rejected");
    check(!runtime.start("scene_box(0/0,0,0,1,1,1,1,1,1) return {}", error, true),
          "nonfinite box rejected");
    check(!runtime.start("for i=1,129 do scene_box(0,0,0,1,1,1,1,1,1) end return {}", error, true) &&
          runtime.boxes().empty(), "scene resource cap and failed start cleanup");
    check(runtime.start("scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() error_missing() end}",
                        error, true), "start with owned box");
    check(!runtime.tick(0.1, error) && runtime.boxes().empty(), "callback failure clears owned scene");
    check(runtime.start("scene_box(0,0,0,1,1,1,1,1,1) return {}", error, true), "restart after scene failure");
    check(runtime.stop(error) && runtime.boxes().empty(), "disable clears scene resources");
    check(runtime.start("local id=scene_box(0,0,0,1,1,1,1,1,1) "
                        "return {on_input=function(control,value) "
                        "if control=='steer' then scene_rotate(id,0,math.pi*value,0) "
                        "scene_resize(id,2,3,4) scene_colour(id,0.2,0.4,0.6) end end,"
                        "on_tick=function(dt) scene_move(id,math.sin(math.pi/2),dt,0) end}",
                        error, true), "start input and math script");
    check(!runtime.input("INVALID", 0.1, error) && runtime.running(), "invalid control rejected");
    check(!runtime.input("steer", std::numeric_limits<double>::infinity(), error) && runtime.running(),
          "nonfinite input rejected");
    check(!runtime.input("steer", 1.1, error) && runtime.running(), "input range enforced");
    check(runtime.input("steer", 0.5, error) && runtime.tick(0.25, error), "dispatch input and tick");
    boxes = runtime.boxes();
    check(boxes.size() == 1 && boxes[0].x == 1 && boxes[0].y == 0.25 &&
          boxes[0].rotateY > 1.57 && boxes[0].rotateY < 1.58 && boxes[0].height == 3 &&
          boxes[0].green == 0.4, "input updates owned box properties");
    check(runtime.stop(error) && runtime.boxes().empty(), "transformed box cleared on stop");
    check(!runtime.start("scene_hit(1,0,0,0,0,0,-1,10) return {}", error),
          "scene picking unavailable without scene permission");
    sf::PluginScriptRuntime::AssetCounts hitAssets{1, 0};
    check(runtime.start(R"(
        local b=scene_box(0,0,-5,2,2,4,1,1,1)
        local m=scene_model(1,0,0,0,-8,2,2,2,1,1,1)
        return {on_tick=function()
          if math.abs(scene_hit(b,0,0,0,0,0,-0.5,10)-3)>0.0001 then missing() end
          if scene_hit(b,2,0,0,0,0,-1,10) then missing() end
          if scene_hit(b,0,0,0,0,0,-1,2.9) then missing() end
          if scene_hit(b,0,0,-5,1,0,0,1) ~= 0 then missing() end
          scene_rotate(b,0,math.pi/2,0)
          if math.abs(scene_hit(b,0,0,0,0,0,-1,10)-4)>0.0001 then missing() end
          scene_rotate(b,math.pi/2,0,0)
          if math.abs(scene_hit(b,0,0,0,0,0,-1,10)-4)>0.0001 then missing() end
          scene_resize(b,4,2,2)
          scene_rotate(b,0,0,math.pi/2)
          if math.abs(scene_hit(b,0,0,0,0,0,-1,10)-4)>0.0001 then missing() end
          if math.abs(scene_hit(b,0,3,-5,0,-1,0,10)-1)>0.0001 then missing() end
          if math.abs(scene_hit(m,0,0,0,0,0,-1,10)-7)>0.0001 then missing() end
        end})", error, true, nullptr, &hitAssets),
          "scene hit test starts");
    check(runtime.tick(0.1, error) && runtime.stop(error), "scene hits use owned oriented boxes and normalized rays");
    check(runtime.start("local b=scene_box(0,0,0,1,1,1,1,1,1) scene_remove(b) "
                        "return {on_tick=function() scene_hit(b,0,0,0,1,0,0,10) end}", error, true) &&
          !runtime.tick(0.1, error) && !runtime.running(), "removed or foreign scene handle rejected");
    check(runtime.start("local b=scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() "
                        "scene_hit(b,0,0,0,0,0,0,10) end}", error, true) &&
          !runtime.tick(0.1, error), "zero scene ray rejected");
    check(runtime.start("local b=scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() "
                        "scene_hit(b,0/0,0,0,1,0,0,10) end}", error, true) &&
          !runtime.tick(0.1, error), "nonfinite scene ray rejected");
    check(runtime.start("local b=scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() "
                        "for i=1,65 do scene_hit(b,0,0,0,1,0,0,10) end end}", error, true) &&
          !runtime.tick(0.1, error) && error.find("query limit") != std::string::npos,
          "scene hit query budget enforced");
    check(runtime.start("local id=scene_box(0,0,0,1,1,1,1,1,1) return {"
                        "on_input=function() scene_move(id,0/0,0,0) end}", error, true),
          "start for invalid transform");
    check(!runtime.input("steer", 0, error) && !runtime.running() && runtime.boxes().empty(),
          "invalid transform closes runtime and clears scene");
    check(!runtime.start("return {on_input=5}", error), "invalid input callback rejected");
    check(runtime.start("return {on_input=function() while true do end end}", error),
          "start for input instruction limit");
    check(!runtime.input("throttle", 1, error) && !runtime.running() &&
          error.find("instruction limit") != std::string::npos, "input callback budget enforced");
    check(runtime.start("return {}", error), "start for repeated input without callback");
    bool inputsOk = true;
    for (int i = 0; i < 1000 && inputsOk; ++i) inputsOk = runtime.input("throttle", 1, error);
    check(inputsOk && runtime.stop(error), "absent input callback leaves stack bounded");
    const char* thrownToy = R"(
        local shot, x, y, vx, vy, age
        return {
          on_input=function(control, value)
            if control == 'throw' and value > 0 and not shot then
              x, y, vx, vy, age = 0, 1, 15, 3.5, 0
              shot = scene_box(x,y,0,0.5,0.15,0.2,1,0.62,0.2)
            end
          end,
          on_tick=function(dt)
            if shot then
              age = age + dt
              vy = vy - 20 * dt
              x = x + vx * dt
              y = y + vy * dt
              if y < 0.15 then y = 0.15 vy = -vy * 0.35 end
              scene_move(shot, x, y, 0)
              scene_rotate(shot, age*8, age*6, 0)
              if age >= 5 then scene_remove(shot) shot = nil end
            end
          end
        })";
    check(runtime.start(thrownToy, error, true) && runtime.input("throw", 1, error),
          "scripted toy input creates a shot");
    check(runtime.tick(0.1, error), "scripted toy advances");
    boxes = runtime.boxes();
    check(boxes.size() == 1 && boxes[0].x > 1.49 && boxes[0].x < 1.51 &&
          boxes[0].y > 1.14 && boxes[0].y < 1.16, "scripted toy ballistic position");
    bool toyTicks = true;
    for (int i = 0; i < 50 && toyTicks; ++i) toyTicks = runtime.tick(0.1, error);
    check(toyTicks && runtime.boxes().empty() && runtime.stop(error), "scripted toy expires and cleans up");
    struct World { int calls = 0; bool invalid = false; bool available = false; } world;
    sf::PluginScriptRuntime::WorldQueries queries{
        &world,
        [](void* context, double, double, double, double, double, double* out) noexcept {
            auto& w = *static_cast<World*>(context);
            ++w.calls;
            *out = w.invalid ? std::numeric_limits<double>::quiet_NaN() : 2;
            return true;
        },
        [](void* context, double* x, double*, double*, double, double) noexcept {
            ++static_cast<World*>(context)->calls;
            *x = 3;
            return true;
        },
        [](void* context) noexcept { return static_cast<World*>(context)->available; }};
    check(!runtime.start("world_floor(0,0,0,1,1) return {}", error),
          "world queries are absent without a host grant");
    check(!runtime.start("world_available() return {}", error),
          "world availability is absent without a host grant");
    check(runtime.start("local id=scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() "
                        "if world_available() then scene_move(id,world_floor(0,0,0,1,1),0,0) end end}",
                        error, true, &queries) && runtime.tick(0.1, error) && world.calls == 0,
          "unavailable world can be polled without querying it");
    world.available = true;
    check(runtime.tick(0.1, error) && world.calls == 1 && runtime.boxes()[0].x == 2 && runtime.stop(error),
          "world becomes available to a running script");
    world.calls = 0;
    check(runtime.start("local id=scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() "
                        "local x,y,z=world_resolve(0,0,0,1,1) "
                        "scene_move(id,x,world_floor(x,y,z,1,1),z) end}", error, true, &queries),
          "start with world service");
    check(runtime.tick(0.1, error) && world.calls == 2, "world query callbacks invoked");
    boxes = runtime.boxes();
    check(boxes.size() == 1 && boxes[0].x == 3 && boxes[0].y == 2, "world results update scene");
    world.invalid = true;
    check(!runtime.tick(0.1, error) && !runtime.running() && runtime.boxes().empty(),
          "invalid world result closes runtime and clears scene");
    world.invalid = false;
    queries.trackClearance = [](void* context, double, double, double, double* out) noexcept {
        auto& w = *static_cast<World*>(context);
        ++w.calls;
        if (!w.available) return false;
        *out = w.invalid ? std::numeric_limits<double>::quiet_NaN() : 35;
        return true;
    };
    check(!runtime.start("world_track_clearance(0,0,1) return {}", error) &&
          runtime.start("local id=scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() "
                        "scene_move(id,0,world_track_clearance(20,0,8),0) end}", error, true, &queries) &&
          runtime.tick(0.1, error) && runtime.boxes()[0].y == 35 && runtime.stop(error),
          "track clearance requires world permission and reaches host service");
    check(runtime.start("return {on_tick=function() world_track_clearance(0,0,101) end}",
                        error, false, &queries) && !runtime.tick(0.1, error) &&
          error.find("out of range") != std::string::npos, "track clearance rejects invalid arguments");
    world.invalid = true;
    check(runtime.start("return {on_tick=function() world_track_clearance(0,0,1) end}",
                        error, false, &queries) && !runtime.tick(0.1, error) &&
          error.find("clearance query failed") != std::string::npos,
          "track clearance rejects invalid service output");
    world.invalid = false;
    check(runtime.start("return {on_tick=function() for i=1,65 do world_track_clearance(0,0,1) end end}",
                        error, false, &queries) && !runtime.tick(0.1, error) &&
          error.find("query limit") != std::string::npos, "track clearance shares query budget");
    check(runtime.start("return {on_tick=function() for i=1,64 do world_floor(0,0,0,1,1) end end}",
                        error, false, &queries) && runtime.tick(0.1, error) && runtime.tick(0.1, error) &&
          runtime.stop(error), "world query budget resets per callback");
    check(runtime.start("return {on_tick=function() for i=1,65 do world_floor(0,0,0,1,1) end end}",
                        error, false, &queries) && !runtime.tick(0.1, error) && !runtime.running() &&
          error.find("query limit") != std::string::npos, "world query budget enforced");
    check(runtime.start("return {on_tick=function() world_floor(0,0,0,-1,1) end}", error, false, &queries) &&
          !runtime.tick(0.1, error), "invalid world query rejected");
    struct Layout { int status = 1; bool invalid = false; int calls = 0; } layout;
    sf::PluginScriptRuntime::LayoutQuery layoutQuery{&layout, [](void* context, double, double, double* out) noexcept {
        auto& data = *static_cast<Layout*>(context);
        ++data.calls;
        out[0] = 28; out[1] = 1.3; out[2] = -28; out[3] = data.invalid ? -1 : 28; out[4] = 0;
        return data.status;
    }};
    check(!runtime.start("city_cell(0,0) return {}", error), "city layout absent without permission");
    check(runtime.start("local id=scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() "
                        "local x,y,z,pitch,occupied=city_cell(0,0) "
                        "if x and not occupied then scene_move(id,x,y,z) end end}",
                        error, true, nullptr, nullptr, nullptr, false, false, &layoutQuery) &&
          runtime.tick(0.1, error) && runtime.boxes()[0].x == 28 && runtime.boxes()[0].z == -28,
          "city cell values are delivered to script");
    layout.status = 0;
    check(runtime.tick(0.1, error) && runtime.running(), "no city cell returns nil");
    layout.status = -1;
    check(!runtime.tick(0.1, error) && !runtime.running() && runtime.boxes().empty(),
          "unavailable city cell stops script and clears scene");
    layout.status = 1;
    layout.invalid = true;
    check(runtime.start("return {on_tick=function() city_cell(0,0) end}", error, false, nullptr, nullptr,
                        nullptr, false, false, &layoutQuery) && !runtime.tick(0.1, error),
          "invalid host city cell rejected");
    layout.invalid = false;
    check(runtime.start("return {on_tick=function() for i=1,65 do city_cell(0,0) end end}", error,
                        false, nullptr, nullptr, nullptr, false, false, &layoutQuery) &&
          !runtime.tick(0.1, error) && error.find("query limit") != std::string::npos,
          "city cell query budget enforced");
    check(runtime.start("return {on_tick=function() city_cell(0/0,0) end}", error, false, nullptr, nullptr,
                        nullptr, false, false, &layoutQuery) && !runtime.tick(0.1, error),
          "nonfinite city cell input rejected");
    check(!runtime.start("player_impulse(1,2,3) return {}", error),
          "player push unavailable without consent");
    check(runtime.start("return {on_tick=function() player_impulse(3,4,0) end}",
                        error, false, nullptr, nullptr, nullptr, false, false, nullptr, true) &&
          runtime.tick(0.1, error), "player push can be queued");
    auto impulse = runtime.takeImpulse();
    check(impulse.pending && impulse.x == 3 && impulse.y == 4 && impulse.z == 0 &&
          !runtime.takeImpulse().pending && runtime.stop(error), "player push drains once");
    check(runtime.start("return {on_tick=function() player_impulse(1,2,3) missing() end}",
                        error, false, nullptr, nullptr, nullptr, false, false, nullptr, true) &&
          !runtime.tick(0.1, error) && !runtime.takeImpulse().pending,
          "failed callback discards player push");
    check(!runtime.start("player_impulse(12,0,12) return {}", error, false, nullptr, nullptr, nullptr,
                         false, false, nullptr, true) &&
          !runtime.start("player_impulse(0,-1,0) return {}", error, false, nullptr, nullptr, nullptr,
                         false, false, nullptr, true) &&
          !runtime.start("player_impulse(0,0/0,0) return {}", error, false, nullptr, nullptr, nullptr,
                         false, false, nullptr, true), "invalid or excessive player push rejected");
    check(!runtime.start("player_impulse(1,0,0) player_impulse(1,0,0) return {}", error, false, nullptr,
                         nullptr, nullptr, false, false, nullptr, true),
          "only one pending push is allowed");
    struct View { bool available = false; bool invalid = false; } view;
    sf::PluginScriptRuntime::ViewQuery viewQuery{
        &view,
        [](void* context, double* pose) noexcept {
            auto& v = *static_cast<View*>(context);
            if (!v.available) return false;
            pose[0] = v.invalid ? std::numeric_limits<double>::quiet_NaN() : 4;
            pose[1] = 2; pose[2] = 3; pose[3] = 0; pose[4] = 0; pose[5] = -1;
            return true;
        }};
    check(!runtime.start("view_pose() return {}", error), "view API absent without grant");
    check(runtime.start("local id=scene_box(0,0,0,1,1,1,1,1,1) return {on_tick=function() "
                        "local x,y,z,dx,dy,dz=view_pose() if x then scene_move(id,x+dx,y+dy,z+dz) end end}",
                        error, true, nullptr, nullptr, &viewQuery) && runtime.tick(0.1, error) &&
          runtime.boxes()[0].x == 0, "unavailable view returns nil");
    view.available = true;
    check(runtime.tick(0.1, error) && runtime.boxes()[0].x == 4 && runtime.boxes()[0].z == 2,
          "granted view pose reaches script");
    view.invalid = true;
    check(!runtime.tick(0.1, error) && !runtime.running() && runtime.boxes().empty(),
          "invalid view pose stops and cleans up script");
    view.invalid = false;
    check(runtime.start("return {on_tick=function() for i=1,65 do view_pose() end end}",
                        error, false, nullptr, nullptr, &viewQuery) &&
          !runtime.tick(0.1, error) && error.find("query limit") != std::string::npos,
          "view query budget enforced");
    check(!runtime.start("sound_play('throw',1) return {}", error),
          "sound API absent without permission");
    check(runtime.start("sound_play('pickup',0.5) return {on_tick=function() sound_play('boom',1) end}",
                        error, false, nullptr, nullptr, nullptr, true),
          "sound events can be queued with permission");
    auto sounds = runtime.takeSounds();
    check(sounds.count == 1 && sounds.events[0].sound == sf::PluginScriptRuntime::Sound::Pickup &&
          sounds.events[0].volume == 0.5f && runtime.takeSounds().count == 0,
          "startup sound batch drains exactly once");
    check(runtime.tick(0.1, error), "sound callback runs");
    sounds = runtime.takeSounds();
    check(sounds.count == 1 && sounds.events[0].sound == sf::PluginScriptRuntime::Sound::Boom &&
          runtime.stop(error), "tick sound batch is delivered");
    check(runtime.start("return {on_tick=function() sound_play('throw',1) missing() end}",
                        error, false, nullptr, nullptr, nullptr, true) &&
          !runtime.tick(0.1, error) && runtime.takeSounds().count == 0,
          "failed callback discards queued sounds");
    check(runtime.start("return {on_tick=function() for i=1,9 do sound_play('boom',1) end end}",
                        error, false, nullptr, nullptr, nullptr, true) &&
          !runtime.tick(0.1, error) && runtime.takeSounds().count == 0 &&
          error.find("sound event limit") != std::string::npos,
          "sound queue is bounded");
    check(!runtime.start("sound_play('other',1) return {}", error, false, nullptr, nullptr, nullptr, true) &&
          !runtime.start("sound_play('boom',2) return {}", error, false, nullptr, nullptr, nullptr, true),
          "invalid sound and volume rejected");
    check(!runtime.start("sound_engine(true,1,false) return {}", error),
          "engine sound is absent without permission");
    check(runtime.start("sound_engine(true,12,false) sound_engine(true,30,true) "
                        "return {on_tick=function() sound_engine(false,0,false) end}",
                        error, false, nullptr, nullptr, nullptr, true),
          "engine sound is available with sound permission");
    auto engine = runtime.takeEngineSound();
    check(engine.pending && engine.active && engine.speed == 30 && engine.boost &&
          !runtime.takeEngineSound().pending, "engine sound updates coalesce and drain");
    check(runtime.tick(0.1, error) && runtime.takeEngineSound().pending && runtime.stop(error) &&
          !runtime.takeEngineSound().pending, "engine sound stop clears queued state");
    check(runtime.start("return {on_tick=function() sound_engine(true,1,false) missing() end}",
                        error, false, nullptr, nullptr, nullptr, true) &&
          !runtime.tick(0.1, error) && !runtime.takeEngineSound().pending,
          "failed callback discards engine sound updates");
    check(!runtime.start("sound_engine(true,111,false) return {}", error, false, nullptr, nullptr, nullptr, true) &&
          !runtime.start("sound_engine(true,0/0,false) return {}", error, false, nullptr, nullptr, nullptr, true) &&
          !runtime.start("sound_engine(1,1,false) return {}", error, false, nullptr, nullptr, nullptr, true),
          "invalid engine sound arguments rejected");
    check(!runtime.start("hud_set('SCORE 1') return {}", error), "HUD API absent without permission");
    check(runtime.start("hud_set('SCORE 1') return {on_tick=function() hud_set('SCORE 2') end}",
                        error, false, nullptr, nullptr, nullptr, false, true),
          "HUD API available with permission");
    auto hud = runtime.hudText();
    check(std::string(hud.text.data()) == "SCORE 1" && hud.revision == 1, "HUD text assigned");
    check(runtime.tick(0.1, error), "HUD update succeeds");
    hud = runtime.hudText();
    check(std::string(hud.text.data()) == "SCORE 2" && hud.revision == 2 &&
          runtime.tick(0.1, error) && runtime.hudText().revision == 2, "HUD revisions change only for new text");
    check(runtime.stop(error) && runtime.hudText().revision == 0 &&
          runtime.hudText().text[0] == 0, "HUD removed on stop");
    check(runtime.start("hud_set('TEXT') return {on_tick=function() hud_set('CHANGED') missing() end}",
                        error, false, nullptr, nullptr, nullptr, false, true) &&
          !runtime.tick(0.1, error) && runtime.hudText().text[0] == 0,
          "failed callback removes HUD");
    check(!runtime.start("hud_set('123456789012345678901234567890123') return {}",
                         error, false, nullptr, nullptr, nullptr, false, true) &&
          !runtime.start("hud_set('bad\\ntext') return {}", error, false, nullptr, nullptr, nullptr, false, true) &&
          !runtime.start("hud_set(123) return {}", error, false, nullptr, nullptr, nullptr, false, true),
          "HUD string type, length and printable text are enforced");
    sf::PluginScriptRuntime other;
    check(runtime.start("scene_box(1,0,0,1,1,1,1,1,1) return {}", error, true) &&
          other.start("scene_box(2,0,0,1,1,1,1,1,1) return {}", error, true),
          "independent scene owners start");
    check(runtime.stop(error) && runtime.boxes().empty() && other.boxes().size() == 1 &&
          other.boxes()[0].x == 2, "stopping one runtime preserves another owner's scene");
    check(other.stop(error) && other.boxes().empty(), "second scene owner stops cleanly");
    sf::PluginScriptRuntime::AssetCounts assets{1, 1, {"assets/duck.glb"}, {"assets/paint.png"}};
    check(!runtime.start("return {}", error, false, nullptr, &assets),
          "asset catalog requires scene grant");
    check(runtime.start("local id=scene_model(1,1,1,2,3,4,5,6,0.2,0.3,0.4) "
                        "return {on_tick=function() scene_move(id,4,5,6) end}", error, true, nullptr, &assets),
          "create model instance from catalog");
    auto models = runtime.models();
    check(models.size() == 1 && models[0].model == 1 && models[0].texture == 1 &&
          models[0].transform.x == 1 && models[0].transform.height == 5, "model instance values");
    check(runtime.tick(0.1, error) && runtime.models()[0].transform.x == 4,
          "model instance transform updated");
    check(runtime.stop(error) && runtime.models().empty(), "model instance cleared on stop");
    check(runtime.start("local id=scene_model(asset_model('assets/duck.glb'),asset_texture('assets/paint.png'),"
                        "0,0,0,1,1,1,1,1,1) return {}", error, true, nullptr, &assets) &&
          runtime.models().size() == 1 && runtime.stop(error), "lookup packaged asset names");
    check(!runtime.start("asset_model('assets/missing.glb') return {}", error, true, nullptr, &assets),
          "unknown packaged asset name rejected");
    sf::PluginScriptRuntime::AssetCounts duplicateAssets{2, 0, {"assets/a.glb", "assets/a.glb"}, {}};
    check(!runtime.start("return {}", error, true, nullptr, &duplicateAssets),
          "duplicate model asset names rejected");
    check(!runtime.start("scene_model(2,1,0,0,0,1,1,1,1,1,1) return {}",
                         error, true, nullptr, &assets) && runtime.models().empty(),
          "unknown model rejected");
    check(!runtime.start("scene_model(1,2,0,0,0,1,1,1,1,1,1) return {}",
                         error, true, nullptr, &assets), "unknown texture rejected");
    check(!runtime.start("for i=1,64 do scene_box(0,0,0,1,1,1,1,1,1) end "
                         "for i=1,65 do scene_model(1,0,0,0,0,1,1,1,1,1,1) end return {}",
                         error, true, nullptr, &assets) && runtime.boxes().empty() && runtime.models().empty(),
          "shared scene cap cleans up all objects on failure");
    check(runtime.start("local id=scene_model(1,0,0,0,0,1,1,1,1,1,1) "
                        "return {on_tick=function() scene_remove(id) end}", error, true, nullptr, &assets) &&
          runtime.tick(0.1, error) && runtime.models().empty() && runtime.stop(error),
          "remove model by owned handle");
    check(!sf::runPluginScript("return math.sin(1)", result, error), "one-shot script has no math library");
    return failures ? 1 : 0;
}
