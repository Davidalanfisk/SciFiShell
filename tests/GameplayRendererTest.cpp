#include "GameplayModel.h"
#include "GameplayConsent.h"
#include "GameplaySceneRenderer.h"
#include "Renderer.h"

#include <SDL.h>
#include "miniz.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static std::string package(const std::string& model, const std::string& manifest, const std::string& script,
                           const char* modelPath = "assets/duck.glb", const std::string& texture = {}) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return {};
    bool ok = mz_zip_writer_add_mem(&zip, "plugin.json", manifest.data(), manifest.size(), MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "scripts/main.lua", script.data(), script.size(), MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, modelPath, model.data(), model.size(), MZ_DEFAULT_COMPRESSION);
    if (ok && !texture.empty())
        ok = mz_zip_writer_add_mem(&zip, "assets/paint.png", texture.data(), texture.size(), MZ_DEFAULT_COMPRESSION);
    void* data = nullptr;
    size_t size = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&zip, &data, &size) != 0;
    std::string result;
    if (ok) result.assign(static_cast<const char*>(data), size);
    mz_free(data);
    mz_zip_writer_end(&zip);
    return result;
}

static void appendU32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

static std::string scaledModel() {
    std::string json =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":76}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":3}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5121,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2},{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],"nodes":[{"children":[1,2]},{"mesh":0,"scale":[1,2,1]},{"mesh":0,"translation":[2,0,0]}],"scenes":[{"nodes":[0]}],"scene":0})";
    while (json.size() % 4) json.push_back(' ');
    std::string binary;
    for (std::uint32_t word : {0u, 0u, 0u, 0x3f800000u, 0u, 0u, 0u, 0x3f800000u, 0u})
        appendU32(binary, word);
    for (int i = 0; i < 3; ++i) {
        appendU32(binary, 0x3f800000u); appendU32(binary, 0x3f800000u); appendU32(binary, 0u);
    }
    binary.append("\0\1\2\0", 4);
    std::string out;
    appendU32(out, 0x46546c67); appendU32(out, 2);
    appendU32(out, static_cast<std::uint32_t>(28 + json.size() + binary.size()));
    appendU32(out, static_cast<std::uint32_t>(json.size())); appendU32(out, 0x4e4f534a);
    out += json;
    appendU32(out, static_cast<std::uint32_t>(binary.size())); appendU32(out, 0x004e4942);
    out += binary;
    return out;
}

int main(int argc, char** argv) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Gameplay renderer test", SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED, 320, 240, SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    int result = 0;
    {
        sf::Renderer renderer;
        const bool msaaResolveOnly = argc == 2 && std::string(argv[1]) == "--msaa-resolve-only";
        if (msaaResolveOnly) renderer.setMsaaPreference(1);
        if (!renderer.init(window, SCIFI_TEST_SHADER_DIR)) {
            std::fprintf(stderr, "Renderer initialization failed\n");
            result = 1;
        } else if (msaaResolveOnly) {
            if (renderer.msaaSamples() <= 1) {
                std::fprintf(stderr, "MSAA is unavailable; resolve test cannot run\n");
                result = 1;
            } else {
                renderer.setPostFx(true);
                renderer.beginDesktopFrame();
                renderer.beginReflection();
                renderer.endReflection();
                renderer.postWorld();
                renderer.endTarget();
                renderer.waitIdle();
                std::printf("MSAA post, desktop and reflection resolves completed (%u samples)\n",
                            renderer.msaaSamples());
            }
            renderer.shutdown();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return result;
        } else {
            sf::GameplayModel model;
            model.vertices = {-0.5f, -0.5f, 0, 0, 0, 1, 0, 0,
                               0.5f, -0.5f, 0, 0, 0, 1, 1, 0,
                               0.0f,  0.5f, 0, 0, 0, 1, 0.5f, 1};
            model.indices = {0, 1, 2};
            uint64_t handle = 0;
            std::string error;
            auto check = [&](bool ok, const char* message) {
                if (!ok) { std::fprintf(stderr, "%s: %s\n", message, error.c_str()); result = 1; }
            };
            check(renderer.createGameplayModel(1, model, nullptr, handle, error) && handle,
                  "GPU model creation");
            renderer.beginDesktopFrame();
            check(!renderer.drawGameplayModel(2, handle, sf::Mat4::identity(), {1, 1, 1}),
                  "Foreign owner cannot draw");
            check(renderer.drawGameplayModel(1, handle, sf::Mat4::identity(), {1, 1, 1}),
                  "Owner can draw");
            if (argc == 2 && std::string(argv[1]) == "--mesh-gpu-only") {
                namespace fs = std::filesystem;
                std::ifstream file(fs::path(SCIFI_TEST_ASSETS) / "models" / "Duck.glb", std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(file)), {});
                const auto zip = package(bytes,
                    R"({"id":"com.example.meshtest","name":"MESH TEST","version":"1.0.0","author":"Test","description":"Test","entry":"scripts/main.lua","type":"gameplay","platforms":["any"],"permissions":["scene"],"api_version":"1"})",
                    "local files=asset_find('ships/*.glb') if #files~=1 then error('nested asset missing') end "
                    "local mesh=mesh_create() local a=mesh_vertex(mesh,0,0,0) local b=mesh_vertex(mesh,1,0,0) "
                    "local c=mesh_vertex(mesh,0,1,0) mesh_triangle(mesh,a,b,c) scene_mesh(mesh_upload(mesh)) return {}",
                    "assets/ships/fighter.glb");
                const auto directory = fs::path(SCIFI_TEST_WORK_DIR) /
                    ("gameplay-mesh-gpu-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                sf::GameplayConsent consent(directory);
                sf::GameplaySession session;
                sf::GameplaySceneRenderer scene(renderer, 10);
                check(!zip.empty() && consent.grant("com.example.meshtest", zip, error) &&
                      session.start(zip, consent, nullptr, error) && session.models().size() == 1 &&
                      scene.prepare(session, error), "Prepare procedural mesh for GPU");
                const auto label = renderer.createLabel(sf::Bitmap{1, 1, {255, 255, 255, 255}});
                check(label && scene.draw(session, *label, error), "Draw procedural mesh on GPU");
                renderer.waitIdle();
                check(session.stop(error), "Stop procedural mesh session");
                scene.clear();
                check(consent.revoke("com.example.meshtest", error), "Revoke mesh test consent");
                std::error_code cleanup;
                fs::remove(directory, cleanup);
                check(!cleanup, "Remove mesh test directory");
                renderer.shutdown();
                SDL_DestroyWindow(window);
                SDL_Quit();
                return result;
            }
            renderer.present(false);
            renderer.releaseGameplayModels(1);
            check(!renderer.drawGameplayModel(1, handle, sf::Mat4::identity(), {1, 1, 1}),
                  "Released handle cannot draw");
            uint64_t reloaded = 0;
            check(renderer.createGameplayModel(1, model, nullptr, reloaded, error) && reloaded != handle,
                  "Model can be reloaded with a fresh handle");
            renderer.releaseGameplayModels(1);
            std::ifstream file(std::filesystem::path(SCIFI_TEST_ASSETS) / "models" / "Duck.glb", std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(file)), {});
            sf::GameplayModel duck;
            check(!bytes.empty() && sf::decodeGameplayModel(bytes, duck, error), "Decode bundled duck");
            uint64_t duckHandle = 0;
            check(renderer.createGameplayModel(1, duck, nullptr, duckHandle, error), "Upload bundled duck");
            renderer.beginDesktopFrame();
            check(renderer.drawGameplayModel(1, duckHandle, sf::Mat4::identity(), {1, 1, 1}), "Draw bundled duck");
            renderer.present(false);
            renderer.releaseGameplayModels(1);
            {
                namespace fs = std::filesystem;
                const auto directory = fs::path(SCIFI_TEST_WORK_DIR) /
                    ("gameplay-renderer-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                const auto zip = package(bytes,
                    R"({"id":"com.example.gputest","name":"GPU TEST","version":"1.0.0","author":"Test","description":"Test","entry":"scripts/main.lua","type":"gameplay","platforms":["any"],"permissions":["scene","input"],"api_version":"1"})",
                    "local m\nreturn {on_start=function() "
                    "scene_box(0,0,0,1,1,1,1,1,1) "
                    "local mesh=mesh_create({0,0,0,0,0,1,0,0, 1,0,0,0,0,1,1,0, 0,1,0,0,0,1,0,1},{1,2,3}) "
                    "scene_mesh(mesh,0,1,0,0,1,1,1,1,1,1) "
                    "m=scene_model(asset_model('assets/duck.glb'),0,0,0,0,1,1,1,1,1,1) end, "
                    "on_input=function(k,v) if k=='remove' then scene_remove(m) "
                    "elseif k=='fail' then while true do end end end}");
                sf::GameplayConsent consent(directory);
                sf::GameplaySession session;
                check(!zip.empty() && consent.grant("com.example.gputest", zip, error), "Store test consent");
                check(session.start(zip, consent, nullptr, error), "Start scripted gameplay session");
                const auto label = renderer.createLabel(sf::Bitmap{1, 1, {255, 255, 255, 255}});
                check(static_cast<bool>(label), "Create HUD-compatible label");
                if (!label) return 1;
                sf::GameplaySceneRenderer scene(renderer, 10);
                check(session.models().size() == 2 && scene.prepare(session, error),
                      "Prepare scripted model and procedural mesh");
                renderer.beginDesktopFrame();
                renderer.drawHudText(*label, sf::Mat4::identity());
                check(scene.draw(session, *label, error), "Draw scripted model, mesh and box");
                renderer.present(false);
                check(session.input("remove", 1, error), "Remove scripted model");
                check(scene.prepare(session, error), "Release removed scripted model");
                renderer.beginDesktopFrame();
                check(scene.draw(session, *label, error), "Draw scene after model removal");
                renderer.present(false);
                check(session.stop(error), "Stop scripted gameplay session");
                check(session.start(zip, consent, nullptr, error), "Restart scripted gameplay session");
                renderer.beginDesktopFrame();
                check(!scene.draw(session, *label, error), "Reject stale scene after restart");
                renderer.present(false);
                check(scene.prepare(session, error), "Prepare restarted scripted scene");
                renderer.beginDesktopFrame();
                check(scene.draw(session, *label, error), "Draw restarted scripted scene");
                renderer.present(false);
                check(!session.input("fail", 1, error) && !session.running(), "Callback failure stops session");
                check(!scene.prepare(session, error), "Stopped session discards scene resources");
                check(session.start(zip, consent, nullptr, error), "Restart after callback failure");
                check(scene.prepare(session, error), "Prepare scene after callback failure");
                check(session.stop(error), "Stop restarted session");
                scene.clear();
                check(consent.revoke("com.example.gputest", error), "Revoke test consent");
                const auto example = fs::path(SCIFI_TEST_ASSETS).parent_path() / "examples" / "plugins" / "snack-stand";
                const auto read = [](const fs::path& path) {
                    std::ifstream in(path, std::ios::binary);
                    return std::string(std::istreambuf_iterator<char>(in), {});
                };
                const auto snackZip = package(bytes, read(example / "plugin.json"), read(example / "scripts" / "main.lua"));
                sf::PluginScriptRuntime::WorldQueries world{nullptr,
                    [](void*, double, double, double, double, double, double* out) noexcept { *out = 0; return true; },
                    [](void*, double*, double*, double* z, double, double) noexcept { if (*z < -4) *z = -4; return true; },
                    [](void*) noexcept { return true; }};
                sf::PluginScriptRuntime::ViewQuery view{nullptr, [](void*, double* out) noexcept {
                    out[0] = 0; out[1] = 1.5; out[2] = 0;
                    out[3] = 0; out[4] = 0; out[5] = -1; return true;
                }};
                bool snackOk = !snackZip.empty() && consent.grant("com.example.snack-stand", snackZip, error) &&
                    session.start(snackZip, consent, &world, error, &view) && session.tick(0.1, error) &&
                    session.models().size() == 1 && scene.prepare(session, error);
                check(snackOk, "Prepare packaged snack stand and duck");
                if (snackOk) {
                    renderer.beginDesktopFrame();
                    check(scene.draw(session, *label, error), "Draw packaged snack stand and duck");
                    renderer.present(false);
                    snackOk = session.input("click", 1, error) && session.input("click", 1, error);
                    session.takeSounds();
                    for (int i = 0; i < 51 && snackOk; ++i) {
                        snackOk = session.tick(0.1, error);
                        session.takeSounds();
                    }
                    snackOk = snackOk && session.models().empty() && session.boxes().size() == 5 &&
                              scene.prepare(session, error);
                    check(snackOk, "Prepare scripted explosion and remove duck GPU model");
                    if (snackOk) {
                        renderer.beginDesktopFrame();
                        check(scene.draw(session, *label, error), "Draw scripted explosion");
                        renderer.present(false);
                    }
                    check(session.stop(error), "Stop snack example");
                    scene.clear();
                    check(session.start(snackZip, consent, &world, error, &view) && session.tick(0.1, error) &&
                          scene.prepare(session, error), "Restart snack example and reload duck GPU model");
                    check(session.stop(error), "Stop restarted snack example");
                    scene.clear();
                }
                check(consent.revoke("com.example.snack-stand", error), "Revoke snack example consent");
                const auto park = fs::path(SCIFI_TEST_ASSETS).parent_path() / "examples" / "plugins" / "park-cell";
                const auto parkZip = package(bytes, read(park / "plugin.json"), read(park / "scripts" / "main.lua"));
                bool occupied = false;
                sf::PluginScriptRuntime::LayoutQuery layout{&occupied, [](void* context, double x, double z, double* out) noexcept {
                    out[0] = std::floor(x / 28 + 0.5) * 28; out[1] = 0;
                    out[2] = std::floor(z / 28 + 0.5) * 28; out[3] = 28;
                    out[4] = *static_cast<bool*>(context) ? 1 : 0;
                    return 1;
                }};
                const bool parkOk = !parkZip.empty() && consent.grant("com.example.park-cell", parkZip, error) &&
                    session.start(parkZip, consent, &world, error, &view, &layout) && session.tick(0.1, error) &&
                    session.models().size() == 3 && session.boxes().size() == 18 && scene.prepare(session, error);
                check(parkOk, "Prepare three packaged parks");
                if (parkOk) {
                    renderer.beginDesktopFrame();
                    check(scene.draw(session, *label, error), "Draw three scripted parks");
                    renderer.present(false);
                    occupied = true;
                    bool ok = true;
                    for (int i = 0; i < 11 && ok; ++i) ok = session.tick(0.1, error);
                    check(ok && session.models().empty() && session.boxes().empty() && scene.prepare(session, error),
                          "Remove occupied park resources");
                }
                check(session.stop(error), "Stop park example");
                scene.clear();
                check(consent.revoke("com.example.park-cell", error), "Revoke park example consent");
                const auto car = fs::path(SCIFI_TEST_ASSETS).parent_path() / "examples" / "plugins" / "car-track";
                const auto carZip = package(read(car / "assets" / "car.glb"), read(car / "plugin.json"),
                    read(car / "scripts" / "main.lua"), "assets/car.glb", read(car / "assets" / "paint.png"));
                sf::PluginScriptRuntime::WorldQueries carWorld{nullptr, world.floor,
                    [](void*, double*, double*, double*, double, double) noexcept { return true; }, world.available};
                const bool carOk = !carZip.empty() && consent.grant("com.example.car-track", carZip, error) &&
                    session.start(carZip, consent, &carWorld, error, &view) && session.tick(0.1, error) &&
                    session.boxes().size() == 36 && session.models().size() == 1 && scene.prepare(session, error);
                check(carOk, "Prepare packaged scripted car and track");
                if (carOk) {
                    renderer.beginDesktopFrame();
                    check(scene.draw(session, *label, error), "Draw scripted car and track");
                    renderer.present(false);
                    check(session.input("move_y", 1, error) && session.tick(0.05, error) &&
                          scene.prepare(session, error), "Update scripted car movement");
                    renderer.beginDesktopFrame();
                    check(scene.draw(session, *label, error), "Draw moved scripted car");
                    renderer.present(false);
                    bool rail = true;
                    for (int i = 0; i < 20 && rail; ++i) rail = session.tick(0.05, error);
                    check(rail && std::string(session.hudText().text.data()) == "ON RAIL: S TO EXIT" &&
                          scene.prepare(session, error), "Advance scripted car on rail");
                    if (rail) {
                        renderer.beginDesktopFrame();
                        check(scene.draw(session, *label, error), "Draw scripted car on rail");
                        renderer.present(false);
                    }
                }
                check(session.stop(error), "Stop car example");
                scene.clear();
                check(consent.revoke("com.example.car-track", error), "Revoke car example consent");
                const auto scaledZip = package(scaledModel(),
                    R"({"id":"com.example.scaled","name":"SCALED","version":"1.0.0","author":"Test","description":"Test","entry":"scripts/main.lua","type":"gameplay","platforms":["any"],"permissions":["scene"],"api_version":"1"})",
                    "return {on_start=function() scene_model(asset_model('assets/duck.glb'),0,0,0,0,1,3,2,1,1,1) end}");
                const bool scaledOk = !scaledZip.empty() && consent.grant("com.example.scaled", scaledZip, error) &&
                    session.start(scaledZip, consent, nullptr, error) && scene.prepare(session, error);
                check(scaledOk, "Prepare packaged multi-node nonuniform GLB and instance");
                if (scaledOk) {
                    renderer.beginDesktopFrame();
                    check(scene.draw(session, *label, error), "Draw multi-node nonuniform model instance");
                    renderer.present(false);
                }
                check(session.stop(error), "Stop nonuniform model example");
                scene.clear();
                check(consent.revoke("com.example.scaled", error), "Revoke nonuniform example consent");
                std::error_code ec;
                fs::remove(directory, ec);
                check(!ec, "Remove test consent directory");
            }
            for (uint64_t owner = 1; owner <= 4; ++owner) {
                for (int i = 0; i < 32; ++i) {
                    uint64_t item = 0;
                    check(renderer.createGameplayModel(owner, model, nullptr, item, error), "Fill GPU resource budget");
                }
                uint64_t extra = 0;
                check(!renderer.createGameplayModel(owner, model, nullptr, extra, error) && !extra,
                      "Reject owner resource count overflow");
            }
            uint64_t extra = 0;
            check(!renderer.createGameplayModel(5, model, nullptr, extra, error) && !extra,
                  "Reject global resource count overflow");
            renderer.releaseGameplayModels(2);
            check(renderer.createGameplayModel(5, model, nullptr, extra, error) && extra,
                  "Released capacity can be reused");
            for (uint64_t owner = 1; owner <= 5; ++owner) renderer.releaseGameplayModels(owner);
        }
        renderer.shutdown();
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
