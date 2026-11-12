#include "CodeEditor.h"
#include "ModelThumbnail.h"
#include "PluginLimits.h"
#include "TextRender.h"
#include "Thumbnails.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace fs = std::filesystem;
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
std::string read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(data.data(), data.size());
    check(bool(f), "Cannot write fixture");
}
std::string overdrawModel() {
    std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":720036}],"bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":720000}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5125,"count":180000,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    while (json.size() % 4) json += ' ';
    std::string result;
    auto word = [&](uint32_t value) { for (int i = 0; i < 4; ++i) result += char(value >> (i * 8)); };
    word(0x46546c67); word(2); word(uint32_t(28 + json.size() + 720036));
    word(uint32_t(json.size())); word(0x4e4f534a); result += json;
    word(720036); word(0x004e4942);
    for (float value : {-1.f, -1.f, 0.f, 1.f, -1.f, 0.f, 0.f, 1.f, 0.f}) {
        uint32_t bits; std::memcpy(&bits, &value, 4); word(bits);
    }
    for (uint32_t i = 0; i < 180000; ++i) word(i % 3);
    return result;
}

int main() {
    const auto assets = fs::path(SCIFI_TEST_ASSETS);
    check(sf::loadFonts((assets / "fonts").u8string()), "Fonts unavailable");
    const auto dir = fs::temp_directory_path() / ("scifi-file-preview-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    // Isolate thumbnail caches from the user's profile.
#ifdef _WIN32
    _putenv_s("LOCALAPPDATA", dir.u8string().c_str());
#else
    setenv("XDG_CACHE_HOME", dir.u8string().c_str(), 1);
#endif
    for (const char* path : {"a.lua", "a.LUA", "a.LuA", "a.txt", "a.json", "a.cpp"})
        check(sf::classifyFile(path, false) == sf::FileKind::Text, "Text classification regression");
    check(sf::classifyFile("a.lua", true) == sf::FileKind::Folder, "Folder classification regression");
    check(sf::classifyFile("a.png", false) == sf::FileKind::Image, "Image classification regression");
    check(sf::classifyFile("a.GLB", false) == sf::FileKind::Model, "GLB classification");
    const auto lua = dir / "source.LuA";
    const std::string source = "--[[ comment\r\nstill comment ]]\r\nlocal value = 'safe'\r\nreturn value\r\n";
    write(lua, source);
    sf::CodeEditor editor;
    check(editor.load(lua.u8string()) && editor.language() == "Lua" && !editor.readOnly(), "Lua editor recognition");
    check(editor.save() && read(lua) == source, "Lua CRLF round trip");
    editor.docEnd();
    editor.insertText("-- edited");
    check(editor.save() && read(lua) == source + "-- edited", "Lua source save");
    sf::Bitmap rendered;
    editor.render(rendered, true, true);
    check(!rendered.rgba.empty(), "Lua editor render");
    const auto large = dir / "large.lua";
    write(large, std::string(sf::kPluginTextLimit + 1, ' '));
    check(editor.load(large.u8string()) && editor.readOnly() && editor.truncated() && !editor.save(),
          "Large Lua must stay truncated and read only");
    check(fs::file_size(large) == sf::kPluginTextLimit + 1, "Large source was changed");

    sf::Bitmap preview;
    std::string error;
    const auto duck = read(assets / "models" / "Duck.glb");
    const bool duckOk = sf::renderModelThumbnail(duck, preview, error);
    check(duckOk, error.c_str());
    size_t modelPixels = 0;
    for (size_t p = 0; p < preview.rgba.size(); p += 4)
        if (preview.rgba[p] != 18 || preview.rgba[p + 1] != 25 || preview.rgba[p + 2] != 38) ++modelPixels;
    check(modelPixels > 1000, "GLB preview contains no geometry");
    check(!sf::renderModelThumbnail("not a model", preview, error) && !error.empty() && preview.rgba.empty(),
          "Invalid GLB must give an explicit error");
    check(!sf::renderModelThumbnail(duck, preview, error, [] { return true; }) &&
          error == "Model preview cancelled", "GLB cancellation");
    int cancellationChecks = 0;
    check(!sf::renderModelThumbnail(duck, preview, error, [&] { return ++cancellationChecks >= 20; }) &&
          error == "Model preview cancelled" && preview.rgba.empty(), "Cancellation during rasterization");
    check(!sf::renderModelThumbnail(std::string(sf::kModelPreviewBytes + 1, ' '), preview, error),
          "Oversized preview accepted");
    const auto room = read(assets.parent_path() / "tests" / "fixtures" / "network-hud-demo" / "assets" / "room.glb");
    const bool roomOk = sf::renderModelThumbnail(room, preview, error);
    check(roomOk, error.c_str());
    check(!sf::renderModelThumbnail(read(assets / "models" / "CarConcept.glb"), preview, error) &&
          !error.empty(), "Unsupported GLB must fail clearly");
    check(!sf::renderModelThumbnail(overdrawModel(), preview, error) &&
          (error == "Model preview raster budget exceeded" || error == "Model preview time budget exceeded"),
          "Pathological overdraw must be bounded");

    {
        sf::ThumbnailLoader loader;
        const auto model = dir / "model.glb";
        write(model, duck);
        const int generation = loader.reset();
        check(loader.request(generation, 1, lua.u8string(), "source.LuA", sf::FileKind::Text), "Lua thumbnail queued");
        check(loader.request(generation, 2, model.u8string(), "model.glb", sf::FileKind::Model), "Model thumbnail queued");
        bool luaResult = false, modelResult = false;
        std::vector<uint8_t> firstModelPixels;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        sf::ThumbResult result;
        while ((!luaResult || !modelResult) && std::chrono::steady_clock::now() < deadline) {
            if (loader.poll(result)) {
                check(!result.bitmap.rgba.empty(), "Empty thumbnail");
                luaResult |= result.index == 1;
                modelResult |= result.index == 2;
                if (result.index == 2) firstModelPixels = result.bitmap.rgba;
            } else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(luaResult && modelResult, "Background previews not delivered");
        loader.request(generation, 3, model.u8string(), "model.glb", sf::FileKind::Model);
        const int next = loader.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        check(!loader.poll(result), "Reset delivered stale model preview");
        check(!loader.request(generation, 4, model.u8string(), "old", sf::FileKind::Model), "Old generation accepted");
        // A replaced model must not reuse the previous file's cached pixels.
        write(model, "invalid replacement");
        check(loader.request(next, 5, model.u8string(), "model.glb", sf::FileKind::Model), "Replacement queued");
        bool fallback = false;
        while (!fallback && std::chrono::steady_clock::now() < deadline) {
            if (loader.poll(result)) fallback = result.index == 5 && !result.bitmap.rgba.empty() &&
                                               result.bitmap.rgba != firstModelPixels;
            else std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(fallback, "Invalid replacement must deliver a visible error card");
    }
    fs::remove_all(dir);
    std::cout << "File previews passed\n";
}
