#include "PluginPackage.h"
#include "GameplayArchive.h"
#include "GameplayTexture.h"
#include "GameplayModel.h"
#include "GameplayGpuValidation.h"
#include "GameplayPackage.h"
#include "GameplayConsent.h"
#include "GameplayFileLock.h"
#include "GameplayLimits.h"
#include "GameplaySession.h"
#include "GameplayStorage.h"
#include "GameplayInput.h"
#include "GameplayFrame.h"
#include "GameplayControlLease.h"
#include "GameplayControlMotion.h"
#include "GameplayImpulse.h"
#include "PluginManager.h"
#include "PluginScript.h"
#include "PluginWorkspace.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include "miniz.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#undef far
#endif

namespace fs = std::filesystem;

std::string archive(const std::vector<std::pair<std::string, std::string>>& files,
                    int compression = MZ_DEFAULT_COMPRESSION) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return {};
    for (const auto& [name, contents] : files)
        if (!mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(), compression)) {
            mz_zip_writer_end(&zip);
            return {};
        }
    void* buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size)) { mz_zip_writer_end(&zip); return {}; }
    std::string result(static_cast<char*>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return result;
}

void appendBigEndian(std::string& data, unsigned value) {
    for (int shift = 24; shift >= 0; shift -= 8) data.push_back(static_cast<char>(value >> shift));
}

void pngChunk(std::string& data, const char* type, const std::string& contents) {
    appendBigEndian(data, static_cast<unsigned>(contents.size()));
    const size_t start = data.size();
    data.append(type, 4);
    data += contents;
    appendBigEndian(data, static_cast<unsigned>(mz_crc32(MZ_CRC32_INIT,
        reinterpret_cast<const unsigned char*>(data.data() + start), contents.size() + 4)));
}

std::string png(unsigned width, unsigned height, const std::string& raw) {
    std::string result("\x89PNG\r\n\x1a\n", 8), header;
    appendBigEndian(header, width);
    appendBigEndian(header, height);
    header.append("\x08\x06\0\0\0", 5);
    pngChunk(result, "IHDR", header);
    mz_ulong compressedSize = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::string compressed(compressedSize, '\0');
    if (mz_compress2(reinterpret_cast<unsigned char*>(compressed.data()), &compressedSize,
                     reinterpret_cast<const unsigned char*>(raw.data()), static_cast<mz_ulong>(raw.size()),
                     MZ_BEST_COMPRESSION) != MZ_OK) return {};
    compressed.resize(compressedSize);
    pngChunk(result, "IDAT", compressed);
    pngChunk(result, "IEND", {});
    return result;
}

void appendLittleEndian(std::string& data, std::uint32_t value) {
    for (int shift = 0; shift <= 24; shift += 8) data.push_back(static_cast<char>(value >> shift));
}

size_t paddedData(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
    const auto& prefix = *static_cast<const std::string*>(opaque);
    if (offset >= sf::kGameplayAssetLimit) return 0;
    size = std::min<std::uint64_t>(size, sf::kGameplayAssetLimit - offset);
    std::memset(buffer, 0, size);
    if (offset < prefix.size()) {
        const auto count = std::min<std::uint64_t>(size, prefix.size() - offset);
        std::memcpy(buffer, prefix.data() + offset, static_cast<size_t>(count));
    }
    return size;
}

std::string glb(std::string json, std::string binary) {
    while (json.size() % 4) json.push_back(' ');
    while (binary.size() % 4) binary.push_back('\0');
    std::string result;
    appendLittleEndian(result, 0x46546c67);
    appendLittleEndian(result, 2);
    appendLittleEndian(result, static_cast<std::uint32_t>(28 + json.size() + binary.size()));
    appendLittleEndian(result, static_cast<std::uint32_t>(json.size()));
    appendLittleEndian(result, 0x4e4f534a);
    result += json;
    appendLittleEndian(result, static_cast<std::uint32_t>(binary.size()));
    appendLittleEndian(result, 0x004e4942);
    result += binary;
    return result;
}

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    const std::string good = R"({"id":"com.example.twilight","name":"TWILIGHT","version":"1.0.0","author":"Example","description":"A violet theme","type":"theme","platforms":["any"],"permissions":[],"accent":"#aa22ff","alt":"#12ccdd"})";
    sf::ThemePackage p;
    std::string error;
    check(sf::PluginPackageStore::parse(good, p, error) && p.id == "com.example.twilight", "valid manifest");
    auto bad = [&](std::string s, const std::string& from, const std::string& to) {
        const auto pos = s.find(from);
        if (pos == std::string::npos) { ++failures; return; }
        s.replace(pos, from.size(), to);
        check(!sf::PluginPackageStore::parse(s, p, error), "invalid manifest accepted");
    };
    bad(good, "\"theme\"", "\"native-module\"");
    bad(good, "\"permissions\":[]", "\"permissions\":[\"network\"]");
    bad(good, "com.example.twilight", "../escape");
    bad(good, "com.example.twilight", "org.scifishell.driving");
    bad(good, "#aa22ff", "#oops");
    bad(good, "1.0.0", "1.0");
    bad(good, "\"name\":\"TWILIGHT\"", "\"name\":\"A\",\"name\":\"B\"");
    bad(good, "\"platforms\":[\"any\"]", "\"platforms\":[\"windows\"]");
    check(!sf::PluginPackageStore::parse(good + "garbage", p, error), "trailing data accepted");
    check(!sf::PluginPackageStore::parse(good.substr(0, good.size() - 1), p, error), "truncated data accepted");
    {
        std::string large = good;
        large.resize(sf::kPluginTextLimit, ' ');
        check(sf::PluginPackageStore::parse(large, p, error), "theme manifest accepts exactly 10 MiB");
        large.push_back(' ');
        check(!sf::PluginPackageStore::parse(large, p, error), "theme manifest rejects 10 MiB plus one byte");
        large = good;
        large.replace(large.find("A violet theme"), std::string("A violet theme").size(),
                      std::string(sf::kPluginTextLimit - good.size(), 'x'));
        check(!sf::PluginPackageStore::parse(large, p, error),
              "larger JSON file cap does not permit oversized metadata strings");
    }
    const std::string manifest = R"({"id":"com.example.clock","name":"CLOCK","version":"1.0.0","author":"Example","description":"Clock message","entry":"scripts/main.lua","type":"script","platforms":["any"],"permissions":[],"min_shell_version":"0.1.0"})";
    const std::string code = "return 'CLOCK ' .. unix_time()";
    const auto zip = archive({{"plugin.json", manifest}, {"scripts/main.lua", code}});
    sf::ScriptPackage script;
    check(!zip.empty() && sf::PluginPackageStore::readZip(zip, script, error) && script.script == code, "valid ZIP");
    check(sf::PluginPackageStore::readZip(archive({{"plugin.json", manifest}, {"scripts/", ""}, {"scripts/main.lua", code}}),
                                         script, error), "optional directory entry");
    for (const auto& path : {"../escape", "/absolute", "C:\\evil", "scripts/../escape", "scripts\\main.lua"}) {
        const std::string placeholder(std::strlen(path), 'q');
        auto hostile = archive({{"plugin.json", manifest}, {placeholder, code}});
        size_t pos = 0;
        while ((pos = hostile.find(placeholder, pos)) != std::string::npos) {
            hostile.replace(pos, placeholder.size(), path);
            pos += placeholder.size();
        }
        check(!hostile.empty() && !sf::PluginPackageStore::readZip(hostile, script, error), "hostile path rejected");
    }
    check(!sf::PluginPackageStore::readZip(archive({{"plugin.json", manifest}, {"scripts/main.lua", code}, {"plugin.json", manifest}}), script, error), "duplicate rejected");
    {
        std::string largeManifest = manifest, largeScript = code;
        largeManifest.resize(sf::kPluginTextLimit, ' ');
        largeScript.resize(sf::kPluginTextLimit, ' ');
        check(sf::PluginPackageStore::parseScript(largeManifest, script, error),
              "script manifest parser accepts exactly 10 MiB");
        check(sf::PluginPackageStore::readZip(archive({{"plugin.json", largeManifest}, {"scripts/main.lua", largeScript}}),
                                             script, error) && script.script == largeScript,
              "script ZIP accepts both text files at exactly 10 MiB");
        largeManifest.push_back(' ');
        check(!sf::PluginPackageStore::parseScript(largeManifest, script, error) &&
              !sf::PluginPackageStore::readZip(archive({{"plugin.json", largeManifest}, {"scripts/main.lua", code}}),
                                              script, error),
              "script manifest parser and ZIP reject 10 MiB plus one byte");
        largeScript.push_back(' ');
        check(!sf::PluginPackageStore::readZip(archive({{"plugin.json", manifest}, {"scripts/main.lua", largeScript}}),
                                              script, error),
              "script ZIP source rejects 10 MiB plus one byte");
        check(!sf::PluginPackageStore::readZip(std::string(sf::kScriptArchiveLimit + 1, 'x'), script, error) &&
              error.find("20 MiB") != std::string::npos, "script ZIP disk cap remains bounded");
    }
    auto symlink = zip;
    const auto central = symlink.find(std::string("PK\x01\x02", 4));
    if (central == std::string::npos) { ++failures; }
    else {
        symlink[central + 38] = 0; symlink[central + 39] = 0;
        symlink[central + 40] = static_cast<char>(0xff); symlink[central + 41] = static_cast<char>(0xa1);
        check(!sf::PluginPackageStore::readZip(symlink, script, error), "symlink rejected");
    }
    auto nulName = zip;
    const auto namePos = nulName.find("scripts/main.lua");
    if (namePos == std::string::npos) { ++failures; }
    else {
        nulName[namePos + 3] = '\0';
        const auto second = nulName.find("scripts/main.lua");
        if (second != std::string::npos) nulName[second + 3] = '\0';
        check(!sf::PluginPackageStore::readZip(nulName, script, error), "embedded NUL rejected");
    }
    check(!sf::PluginPackageStore::readZip(zip.substr(0, zip.size() - 10), script, error), "truncated ZIP rejected");
    sf::GameplayArchive gameplay;
    const auto gameplayZip = archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                      {"assets/car.glb", "model"}, {"assets/car.png", "texture"}});
    check(sf::readGameplayArchive(gameplayZip, gameplay, error) && gameplay.assets.size() == 2 &&
          gameplay.assets.at("assets/car.glb") == "model", "gameplay archive loads asset bytes");
    check(!sf::readGameplayArchive(archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                            {"assets/../bad.glb", "data"}}), gameplay, error) &&
          gameplay.assets.empty(), "gameplay traversal rejected without partial output");
    for (const auto& path : {"assets\\bad.glb", "/assets/bad.glb", "assets/.hidden.png", "assets/UPPER.png",
                             "assets/dir//file.png", "assets/dir/.hidden.png", "assets/dir/../file.png",
                             "assets/file.exe", "assets/car..png"}) {
        const std::string placeholder(std::strlen(path), 'q');
        auto hostile = archive({{"plugin.json", manifest}, {"scripts/main.lua", code}, {placeholder, "data"}});
        size_t pos = 0;
        while ((pos = hostile.find(placeholder, pos)) != std::string::npos) {
            hostile.replace(pos, placeholder.size(), path);
            pos += placeholder.size();
        }
        check(!hostile.empty() && !sf::readGameplayArchive(hostile, gameplay, error), "hostile asset path rejected");
    }
    check(sf::readGameplayArchive(archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                           {"assets/ships/fighter.glb", "data"}}), gameplay, error) &&
          gameplay.assets.count("assets/ships/fighter.glb") == 1, "canonical nested asset accepted");
    const std::string path80 = "assets/ships/" + std::string(70, 'x') + ".glb";
    check(sf::readGameplayArchive(archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                           {path80, "data"}}), gameplay, error) &&
          !sf::readGameplayArchive(archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                           {"assets/ships/" + std::string(71, 'x') + ".glb", "data"}}), gameplay, error),
          "nested asset path length boundary");
    check(!sf::readGameplayArchive(archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                           {"assets/ships/", ""}}), gameplay, error),
          "asset directory entries rejected");
    auto nestedLink = archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                               {"assets/ships/link.glb", "data"}});
    const auto nestedName = nestedLink.rfind("assets/ships/link.glb");
    if (nestedName == std::string::npos || nestedName < 46) ++failures;
    else {
        const auto header = nestedName - 46;
        nestedLink[header + 38] = 0; nestedLink[header + 39] = 0;
        nestedLink[header + 40] = static_cast<char>(0xff); nestedLink[header + 41] = static_cast<char>(0xa1);
        check(!sf::readGameplayArchive(nestedLink, gameplay, error), "nested asset symlink rejected");
    }
    check(!sf::readGameplayArchive(archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                            {"assets/car.glb", "one"}, {"assets/car.glb", "two"}}), gameplay, error),
          "duplicate asset rejected");
    check(sf::readGameplayArchive(archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                            {"assets/car.glb", std::string(2 * 1024 * 1024 + 1, 'x')}}),
                                  gameplay, error), "asset over the old 2 MiB limit accepted");
    const std::string maxAsset(2 * 1024 * 1024, 'x');
    check(!sf::readGameplayArchive(archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                            {"assets/a.glb", maxAsset}, {"assets/b.glb", maxAsset},
                                            {"assets/c.glb", maxAsset}, {"assets/d.glb", maxAsset}}),
                                   gameplay, error) && gameplay.assets.empty(), "archive total size limited");
    std::vector<std::pair<std::string, std::string>> manyAssets{{"plugin.json", manifest}, {"scripts/main.lua", code}};
    for (int i = 0; i < 33; ++i) manyAssets.emplace_back("assets/" + std::to_string(i) + ".png", "data");
    check(!sf::readGameplayArchive(archive(manyAssets), gameplay, error), "archive entry count limited");
    check(!sf::readGameplayArchive(gameplayZip.substr(0, gameplayZip.size() - 10), gameplay, error),
          "truncated gameplay archive rejected");
    sf::GameplayTexture texture;
    const auto validPng = png(1, 1, std::string("\0\x12\x34\x56\xff", 5));
    check(sf::decodeGameplayTexture(validPng, texture, error) && texture.width == 1 && texture.height == 1 &&
          texture.rgba == std::vector<std::uint8_t>({0x12, 0x34, 0x56, 0xff}), "decode PNG pixels");
    const auto assetZip = archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                   {"assets/car.png", validPng}});
    check(sf::readGameplayArchive(assetZip, gameplay, error) &&
          sf::decodeGameplayTexture(gameplay.assets.at("assets/car.png"), texture, error) &&
          texture.rgba == std::vector<std::uint8_t>({0x12, 0x34, 0x56, 0xff}),
          "decode packaged texture bytes");
    check(!sf::decodeGameplayTexture(validPng.substr(0, validPng.size() - 1), texture, error) &&
          texture.rgba.empty() && texture.width == 0, "truncated PNG clears output");
    auto corruptPng = validPng;
    corruptPng[20] ^= 1;
    check(!sf::decodeGameplayTexture(corruptPng, texture, error), "PNG checksum rejected");
    check(!sf::decodeGameplayTexture(png(1025, 1, std::string("\0\0\0\0\0", 5)), texture, error),
          "PNG dimensions limited");
    const auto maxPng = png(1024, 1024, std::string(1024 * (1 + 1024 * 4), '\0'));
    check(sf::decodeGameplayTexture(maxPng, texture, error) && texture.width == 1024 && texture.height == 1024 &&
          texture.rgba.size() == 1024 * 1024 * 4, "maximum PNG dimensions decode within budget");
    check(!sf::decodeGameplayTexture(png(1, 1, {}), texture, error), "incomplete PNG pixels rejected");
    check(!sf::decodeGameplayTexture(validPng + "trailing", texture, error), "trailing PNG data rejected");
    check(!sf::decodeGameplayTexture("not a PNG", texture, error), "non-PNG rejected");
    check(!sf::decodeGameplayTexture(std::string(2 * 1024 * 1024 + 1, 'x'), texture, error),
          "PNG input size limited");
    const auto bomb = png(1, 1, std::string(20 * 1024 * 1024, '\0'));
    check(!bomb.empty() && !sf::decodeGameplayTexture(bomb, texture, error) && texture.rgba.empty(),
          "PNG decompression memory limited");
    check(sf::decodeGameplayTexture(validPng, texture, error), "PNG decoder recovers after failure");
    const std::string modelJson =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":40}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":3}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5121,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    std::string binary;
    for (std::uint32_t word : {0u, 0u, 0u, 0x3f800000u, 0u, 0u, 0u, 0x3f800000u, 0u})
        appendLittleEndian(binary, word);
    binary.append("\0\1\2\0", 4);
    const auto modelData = glb(modelJson, binary);
    if (std::getenv("SCIFI_LARGE_PACKAGE_TEST")) {
        auto largeJson = modelJson;
        const auto lengthPos = largeJson.find("\"byteLength\":40");
        largeJson.replace(lengthPos, std::string("\"byteLength\":40").size(), "\"byteLength\":104857000");
        const size_t paddedJson = (largeJson.size() + 3) & ~size_t(3);
        const auto binSize = static_cast<std::uint32_t>(sf::kGameplayAssetLimit - 28 - paddedJson);
        largeJson.replace(largeJson.find("104857000"), 9, std::to_string(binSize));
        auto prefix = glb(largeJson, binary);
        for (int i = 0; i < 4; ++i) {
            prefix[8 + i] = static_cast<char>(sf::kGameplayAssetLimit >> (8 * i));
            prefix[20 + paddedJson + i] = static_cast<char>(binSize >> (8 * i));
        }
        const auto path = fs::temp_directory_path() / ("scifi-large-model-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zip");
        const std::string largeManifest =
            R"({"id":"com.example.large","name":"LARGE","version":"1.0.0","author":"Example","description":"Example","entry":"scripts/main.lua","type":"gameplay","platforms":["any"],"permissions":["scene"],"api_version":"1"})";
        mz_zip_archive writer{};
        bool ok = mz_zip_writer_init_file(&writer, path.string().c_str(), 0) &&
            mz_zip_writer_add_mem(&writer, "plugin.json", largeManifest.data(), largeManifest.size(), MZ_DEFAULT_COMPRESSION) &&
            mz_zip_writer_add_mem(&writer, "scripts/main.lua", "return {}", 9, MZ_DEFAULT_COMPRESSION) &&
            mz_zip_writer_add_read_buf_callback(&writer, "assets/large.glb", paddedData, &prefix,
                sf::kGameplayAssetLimit, nullptr, nullptr, 0, MZ_DEFAULT_COMPRESSION, nullptr, 0, nullptr, 0) &&
            mz_zip_writer_finalize_archive(&writer);
        mz_zip_writer_end(&writer);
        sf::GameplayPackage largePackage;
        check(ok && sf::readGameplayPackageFile(path, largePackage, error) &&
              largePackage.models.size() == 1 && largePackage.models.begin()->second.vertices.size() == 24,
              "valid 100 MiB GLB loads with bounded decoded geometry");
        std::error_code cleanup;
        fs::remove(path, cleanup);
        check(!cleanup, "remove large model fixture");
    }
    sf::GameplayModel model;
    check(sf::decodeGameplayModel(modelData, model, error) && model.vertices.size() == 24 &&
          model.vertices[8] == 1 && model.vertices[17] == 1 && model.vertices[5] == 1 &&
          model.indices == std::vector<std::uint32_t>({0, 1, 2}), "decode bounded GLB triangle");
    const std::string primitive = "{\"attributes\":{\"POSITION\":0},\"indices\":1}";
    auto multiJson = modelJson;
    multiJson.replace(multiJson.find(primitive), primitive.size(), primitive + "," + primitive);
    check(sf::decodeGameplayModel(glb(multiJson, binary), model, error) && model.vertices.size() == 48 &&
          model.indices == std::vector<std::uint32_t>({0, 1, 2, 3, 4, 5}) &&
          model.vertices[5] == 1 && model.vertices[29] == 1,
          "GLB primitives share a material and are combined with separate indices and normals");
    const std::string mesh = "{\"primitives\":[" + primitive + "]}";
    auto nodesJson = modelJson;
    nodesJson.replace(nodesJson.find(mesh), mesh.size(), mesh + "," + mesh);
    const std::string nodeAndScene = "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]";
    nodesJson.replace(nodesJson.find(nodeAndScene), nodeAndScene.size(),
                     "\"nodes\":[{\"translation\":[3,0,0],\"children\":[1,2]},"
                     "{\"mesh\":0,\"translation\":[1,0,0]},"
                     "{\"mesh\":1,\"translation\":[2,0,0]}],\"scenes\":[{\"nodes\":[0]}]");
    check(sf::decodeGameplayModel(glb(nodesJson, binary), model, error) &&
          model.indices == std::vector<std::uint32_t>({0, 1, 2, 3, 4, 5}) &&
          model.vertices[0] == 4 && model.vertices[24] == 5,
          "GLB flattens multiple mesh nodes and their parent transforms");
    auto instancesJson = nodesJson;
    instancesJson.replace(instancesJson.find("\"mesh\":1"), std::string("\"mesh\":1").size(), "\"mesh\":0");
    check(sf::decodeGameplayModel(glb(instancesJson, binary), model, error) &&
          model.vertices[0] == 4 && model.vertices[24] == 5,
          "GLB preserves repeated mesh instances with distinct transforms");
    auto invalidNodeJson = nodesJson;
    invalidNodeJson.replace(invalidNodeJson.find("{\"mesh\":1,\"translation\":[2,0,0]}"),
                            std::string("{\"mesh\":1,\"translation\":[2,0,0]}").size(),
                            "{\"mesh\":1,\"scale\":[1,0,1]}");
    check(!sf::decodeGameplayModel(glb(invalidNodeJson, binary), model, error) &&
          model.vertices.empty() && model.indices.empty(),
          "invalid later mesh transform rejects all flattened output");
    auto unreachableJson = nodesJson;
    unreachableJson.replace(unreachableJson.find("\"nodes\":[0]}"), std::string("\"nodes\":[0]}").size(),
                           "\"nodes\":[1]}");
    check(!sf::decodeGameplayModel(glb(unreachableJson, binary), model, error) && model.vertices.empty(),
          "GLB rejects mesh nodes outside the selected scene");
    auto tooManyInstances = modelJson;
    tooManyInstances.replace(tooManyInstances.find(primitive), primitive.size(),
                            primitive + "," + primitive + "," + primitive);
    std::string nodeList = "\"nodes\":[";
    for (int i = 0; i < 32; ++i) nodeList += (i ? "," : "") + std::string("{\"mesh\":0}");
    nodeList += "]";
    auto maxInstances = multiJson;
    maxInstances.replace(maxInstances.find(nodeAndScene), nodeAndScene.size(), nodeList);
    maxInstances.erase(maxInstances.find(",\"scene\":0"), std::string(",\"scene\":0").size());
    check(sf::decodeGameplayModel(glb(maxInstances, binary), model, error) &&
          model.indices.size() == 64 * 3 && model.vertices.size() == 64 * 3 * 8,
          "GLB accepts 64 primitive instances across repeated mesh nodes");
    tooManyInstances.replace(tooManyInstances.find(nodeAndScene), nodeAndScene.size(), nodeList);
    tooManyInstances.erase(tooManyInstances.find(",\"scene\":0"), std::string(",\"scene\":0").size());
    check(!sf::decodeGameplayModel(glb(tooManyInstances, binary), model, error) && model.vertices.empty() &&
          error.find("instance limit") != std::string::npos,
          "GLB bounds total primitive instances even when meshes are reused");
    auto manyJson = modelJson;
    std::string primitives = primitive;
    for (int i = 1; i < 65; ++i) primitives += "," + primitive;
    manyJson.replace(manyJson.find(primitive), primitive.size(), primitives);
    check(!sf::decodeGameplayModel(glb(manyJson, binary), model, error) && model.vertices.empty(),
          "GLB primitive count limit rejects partial output");
    auto aggregateJson = modelJson;
    const std::string positionView = "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}";
    const std::string indexView = "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":3}";
    aggregateJson.replace(aggregateJson.find("\"byteLength\":40"), std::string("\"byteLength\":40").size(),
                          "\"byteLength\":24580");
    aggregateJson.replace(aggregateJson.find(positionView), positionView.size(),
                          "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":24576}");
    aggregateJson.replace(aggregateJson.find(indexView), indexView.size(),
                          "{\"buffer\":0,\"byteOffset\":24576,\"byteLength\":3}");
    aggregateJson.replace(aggregateJson.find("\"count\":3"), std::string("\"count\":3").size(), "\"count\":2048");
    primitives = primitive;
    for (int i = 1; i < 33; ++i) primitives += "," + primitive;
    aggregateJson.replace(aggregateJson.find(primitive), primitive.size(), primitives);
    std::string aggregateBinary(24576, '\0');
    aggregateBinary.append("\0\1\2\0", 4);
    check(!sf::decodeGameplayModel(glb(aggregateJson, aggregateBinary), model, error) &&
          model.vertices.empty() && error.find("aggregate") != std::string::npos,
          "GLB aggregate vertex limit applies across primitives");
    auto mixedJson = multiJson;
    mixedJson.replace(mixedJson.find("\"meshes\":"), std::string("\"meshes\":").size(),
                      "\"materials\":[{\"pbrMetallicRoughness\":{}}],\"meshes\":");
    const auto secondPrimitive = mixedJson.find(primitive, mixedJson.find(primitive) + primitive.size());
    mixedJson.replace(secondPrimitive, primitive.size(), "{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}");
    check(!sf::decodeGameplayModel(glb(mixedJson, binary), model, error) && model.vertices.empty() &&
          error.find("share a material") != std::string::npos, "mixed primitive materials are rejected");
    std::string textureBinary = binary.substr(0, 36);
    textureBinary.append(24, '\0');
    textureBinary.append("\0\1\2\0", 4);
    textureBinary += validPng;
    while (textureBinary.size() % 4) textureBinary.push_back('\0');
    const std::string texturedPrefix =
        "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(textureBinary.size()) +
        "}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24},{\"buffer\":0,\"byteOffset\":60,\"byteLength\":3},"
        "{\"buffer\":0,\"byteOffset\":64,\"byteLength\":" + std::to_string(validPng.size()) +
        "}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"SCALAR\"}],"
        "\"images\":[{\"bufferView\":3,\"mimeType\":\"image/png\"}],\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}],\"meshes\":[{\"primitives\":[";
    const std::string texturedPrimitive = "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":2,\"material\":0}";
    const std::string untexturedPrimitive = "{\"attributes\":{\"POSITION\":0},\"indices\":2,\"material\":0}";
    const std::string texturedSuffix = "]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    check(sf::decodeGameplayModel(glb(texturedPrefix + texturedPrimitive + "," + texturedPrimitive + texturedSuffix,
                                      textureBinary), model, error) &&
          model.indices.size() == 6 && !model.embeddedTexture.rgba.empty(),
          "multiple textured primitives retain shared texture");
    check(!sf::decodeGameplayModel(glb(texturedPrefix + texturedPrimitive + "," + untexturedPrimitive + texturedSuffix,
                                       textureBinary), model, error) && model.vertices.empty() &&
          error.find("UV") != std::string::npos, "each textured primitive must provide UV coordinates");
    check(sf::decodeGameplayModel(modelData, model, error), "GLB decoder recovers after primitive failures");
    size_t gpuBytes = 0;
    check(sf::gameplayGpuBytes(model, nullptr, gpuBytes, error) &&
          gpuBytes == 24 * sizeof(float) + 3 * sizeof(std::uint32_t) + 4,
          "GPU resource size includes fallback texture");
    auto invalidGpuModel = model;
    invalidGpuModel.indices[0] = 3;
    check(!sf::gameplayGpuBytes(invalidGpuModel, nullptr, gpuBytes, error) && gpuBytes == 0,
          "GPU upload rejects invalid index");
    invalidGpuModel = model;
    invalidGpuModel.vertices[0] = std::numeric_limits<float>::infinity();
    check(!sf::gameplayGpuBytes(invalidGpuModel, nullptr, gpuBytes, error),
          "GPU upload rejects nonfinite vertex");
    sf::GameplayTexture invalidGpuTexture;
    invalidGpuTexture.width = 2;
    invalidGpuTexture.height = 2;
    invalidGpuTexture.rgba.resize(15);
    check(!sf::gameplayGpuBytes(model, &invalidGpuTexture, gpuBytes, error),
          "GPU upload rejects incomplete texture");
    invalidGpuTexture.rgba.assign(16, 255);
    check(sf::gameplayGpuBytes(model, &invalidGpuTexture, gpuBytes, error) &&
          gpuBytes == 24 * sizeof(float) + 3 * sizeof(std::uint32_t) + 16,
          "GPU resource size includes override texture");
    invalidGpuTexture.rgba[3] = 0;
    check(!sf::gameplayGpuBytes(model, &invalidGpuTexture, gpuBytes, error),
          "GPU upload rejects unsupported transparent texture");
    const auto modelZip = archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                   {"assets/car.glb", modelData}});
    check(sf::readGameplayArchive(modelZip, gameplay, error) &&
          sf::decodeGameplayModel(gameplay.assets.at("assets/car.glb"), model, error),
          "decode packaged GLB bytes");
    auto badIndex = binary;
    badIndex[38] = 3;
    check(!sf::decodeGameplayModel(glb(modelJson, badIndex), model, error) &&
          model.vertices.empty() && model.indices.empty(), "out of range GLB index rejected");
    auto nonfinite = binary;
    nonfinite[0] = 0; nonfinite[1] = 0; nonfinite[2] = static_cast<char>(0xc0);
    nonfinite[3] = static_cast<char>(0x7f);
    check(!sf::decodeGameplayModel(glb(modelJson, nonfinite), model, error), "nonfinite vertex rejected");
    auto external = modelJson;
    const std::string length40 = "\"byteLength\":40";
    external.replace(external.find(length40), length40.size(), "\"byteLength\":40,\"uri\":\"../file\"");
    check(!sf::decodeGameplayModel(glb(external, binary), model, error) &&
          error.find("external resources") != std::string::npos, "external GLB buffer rejected");
    auto oversizedView = modelJson;
    const std::string length36 = "\"byteLength\":36";
    oversizedView.replace(oversizedView.find(length36), length36.size(), "\"byteLength\":999999999");
    check(!sf::decodeGameplayModel(glb(oversizedView, binary), model, error) &&
          error.find("buffer view") != std::string::npos, "oversized GLB view rejected");
    auto invalidAccessor = modelJson;
    const std::string count3 = "\"count\":3";
    invalidAccessor.replace(invalidAccessor.find(count3), count3.size(), "\"count\":65537");
    check(!sf::decodeGameplayModel(glb(invalidAccessor, binary), model, error) &&
          error.find("vertex accessor") != std::string::npos, "oversized GLB accessor rejected");
    auto oldVersion = modelJson;
    oldVersion.replace(oldVersion.find("\"version\":\"2.0\""), std::string("\"version\":\"2.0\"").size(),
                       "\"version\":\"1.0\"");
    check(!sf::decodeGameplayModel(glb(oldVersion, binary), model, error), "old GLB asset version rejected");
    auto transformed = modelJson;
    transformed.replace(transformed.find("{\"mesh\":0}"), std::string("{\"mesh\":0}").size(),
                        "{\"mesh\":0,\"translation\":[1,2,3]}");
    check(sf::decodeGameplayModel(glb(transformed, binary), model, error) &&
          model.vertices[0] == 1 && model.vertices[1] == 2 && model.vertices[2] == 3,
          "GLB node translation applied");
    auto scaled = modelJson;
    scaled.replace(scaled.find("{\"mesh\":0}"), std::string("{\"mesh\":0}").size(),
                   "{\"mesh\":0,\"scale\":[1,2,1]}");
    check(sf::decodeGameplayModel(glb(scaled, binary), model, error) &&
          model.vertices[17] == 2 && model.vertices[5] == 1,
          "GLB nonuniform scale transforms geometry and generated normals");
    const std::string normalJson =
        R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":76}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},{"buffer":0,"byteOffset":72,"byteLength":3}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5121,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2}]}],"nodes":[{"mesh":0,"scale":[1,2,1]}],"scenes":[{"nodes":[0]}],"scene":0})";
    std::string normalBinary = binary.substr(0, 36);
    for (int i = 0; i < 3; ++i) {
        appendLittleEndian(normalBinary, 0x3f800000u);
        appendLittleEndian(normalBinary, 0x3f800000u);
        appendLittleEndian(normalBinary, 0u);
    }
    normalBinary.append("\0\1\2\0", 4);
    check(sf::decodeGameplayModel(glb(normalJson, normalBinary), model, error) &&
          std::abs(model.vertices[3] - 0.8944272f) < 0.00001f &&
          std::abs(model.vertices[4] - 0.4472136f) < 0.00001f,
          "GLB inverse-transpose normal handles nonuniform scale");
    auto shearJson = normalJson;
    const std::string scaleNode = "{\"mesh\":0,\"scale\":[1,2,1]}";
    shearJson.replace(shearJson.find(scaleNode), scaleNode.size(),
                      "{\"mesh\":0,\"matrix\":[1,0,0,0,1,1,0,0,0,0,1,0,0,0,0,1]}");
    auto shearBinary = normalBinary;
    for (int i = 0; i < 3; ++i) {
        const size_t y = 36 + i * 12 + 4;
        shearBinary[y] = shearBinary[y + 1] = shearBinary[y + 2] = shearBinary[y + 3] = 0;
    }
    check(sf::decodeGameplayModel(glb(shearJson, shearBinary), model, error) &&
          model.vertices[16] == 1 && model.vertices[17] == 1 &&
          std::abs(model.vertices[3] - 0.7071068f) < 0.00001f &&
          std::abs(model.vertices[4] + 0.7071068f) < 0.00001f,
          "GLB shear transforms positions and inverse-transpose normals");
    auto singular = modelJson;
    singular.replace(singular.find("{\"mesh\":0}"), std::string("{\"mesh\":0}").size(),
                     "{\"mesh\":0,\"scale\":[1,0,1]}");
    check(!sf::decodeGameplayModel(glb(singular, binary), model, error) &&
          model.vertices.empty() && error.find("singular") != std::string::npos,
          "singular GLB transform rejected without partial output");
    auto reflected = modelJson;
    reflected.replace(reflected.find("{\"mesh\":0}"), std::string("{\"mesh\":0}").size(),
                      "{\"mesh\":0,\"scale\":[-1,1,1]}");
    check(!sf::decodeGameplayModel(glb(reflected, binary), model, error),
          "reflected GLB transform remains rejected");
    auto illConditioned = modelJson;
    illConditioned.replace(illConditioned.find("{\"mesh\":0}"), std::string("{\"mesh\":0}").size(),
                           "{\"mesh\":0,\"scale\":[1,1,0.000000001]}");
    check(!sf::decodeGameplayModel(glb(illConditioned, binary), model, error),
          "ill-conditioned GLB transform rejected");
    {
        std::ifstream in(fs::path(SCIFI_TEST_ASSETS) / "models" / "Duck.glb", std::ios::binary);
        const std::string duck((std::istreambuf_iterator<char>(in)), {});
        check(in.eof() || !in.bad(), "read bundled duck model fixture");
        check(!duck.empty() && sf::decodeGameplayModel(duck, model, error) &&
              !model.vertices.empty() && model.indices.size() == 12636 &&
              std::abs(model.vertices[0] + 0.239364f) < 0.00001f &&
              std::abs(model.vertices[1] - 0.115353f) < 0.00001f &&
              std::abs(model.vertices[2] - 0.306125f) < 0.00001f &&
              model.embeddedTexture.width == 512 && model.embeddedTexture.height == 512,
              "decode bundled duck mesh and embedded texture");
        check(sf::gameplayGpuBytes(model, nullptr, gpuBytes, error) && gpuBytes > 512 * 512 * 4,
              "bundled duck satisfies GPU upload validation");
        const auto duckZip = archive({{"plugin.json", manifest}, {"scripts/main.lua", code},
                                      {"assets/duck.glb", duck}});
        check(sf::readGameplayArchive(duckZip, gameplay, error) &&
              sf::decodeGameplayModel(gameplay.assets.at("assets/duck.glb"), model, error) &&
              model.embeddedTexture.rgba.size() == 512 * 512 * 4, "decode packaged bundled duck");
        auto damagedDuck = duck;
        const auto pngPos = damagedDuck.find(std::string("\x89PNG\r\n\x1a\n", 8));
        if (pngPos == std::string::npos || pngPos + 50 >= damagedDuck.size()) ++failures;
        else {
            damagedDuck[pngPos + 50] ^= 1;
            check(!sf::decodeGameplayModel(damagedDuck, model, error) && model.vertices.empty(),
                  "corrupt embedded duck texture rejected");
        }
    }
    check(!sf::decodeGameplayModel(modelData.substr(0, modelData.size() - 1), model, error),
          "truncated GLB rejected");
    check(!sf::decodeGameplayModel(modelData + "junk", model, error), "trailing GLB rejected");
    check(sf::decodeGameplayModel(modelData, model, error), "GLB decoder recovers after failures");
    const std::string gameplayManifest =
        R"({"id":"com.example.game","name":"GAME","version":"1.0.0","author":"Example","description":"Example game","entry":"scripts/main.lua","type":"gameplay","platforms":["any"],"permissions":["scene","world"],"api_version":"1"})";
    sf::GameplayPackage game;
    const auto downtownManifest = gameplayManifest.substr(0, gameplayManifest.size()-1) +
        R"(,"downtown_model":"assets/garage.glb"})";
    check(sf::readGameplayPackage(archive({{"plugin.json", downtownManifest}, {"scripts/main.lua", "return {}"},
                                          {"assets/garage.glb", modelData}}), game, error) &&
          game.downtownModel == "assets/garage.glb", "optional Downtown model validated without script execution");
    auto downtownGeometry = [&](size_t vertices, size_t triangles) {
        std::string data = binary.substr(0, 36);
        data.resize(vertices*12, '\0');
        const auto offset = data.size();
        for (size_t i = 0; i < triangles; ++i) data.append("\0\1\2", 3);
        while (data.size()%4) data += '\0';
        const std::string json =
            "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(data.size()) +
            "}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(offset) +
            "},{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(triangles*3) +
            "}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(vertices) +
            ",\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5121,\"count\":" + std::to_string(triangles*3) +
            ",\"type\":\"SCALAR\"}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
            "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
        return archive({{"plugin.json", downtownManifest}, {"scripts/main.lua", "return {}"},
                        {"assets/garage.glb", glb(json, data)}});
    };
    check(sf::readGameplayPackage(downtownGeometry(8192,8192), game, error),
          "Downtown accepts inclusive 8192 vertex and triangle limits");
    check(!sf::readGameplayPackage(downtownGeometry(8193,1), game, error) &&
          error.find("Downtown model exceeds") != std::string::npos, "Downtown rejects 8193 vertices");
    check(!sf::readGameplayPackage(downtownGeometry(3,8193), game, error) &&
          error.find("Downtown model exceeds") != std::string::npos, "Downtown rejects 8193 triangles");
    check(!sf::readGameplayPackage(archive({{"plugin.json", downtownManifest},
                                           {"scripts/main.lua", "return {}"}}), game, error) &&
          error.find("Downtown model is missing") != std::string::npos, "missing Downtown model rejected");
    auto invalidDowntown = gameplayManifest.substr(0, gameplayManifest.size()-1) +
        R"(,"downtown_model":"assets/../garage.glb"})";
    check(!sf::PluginPackageStore::parseGameplayManifest(invalidDowntown, game, error), "Downtown traversal rejected");
    invalidDowntown = gameplayManifest.substr(0, gameplayManifest.size()-1) +
        R"(,"downtown_model":["assets/garage.glb"]})";
    check(!sf::PluginPackageStore::parseGameplayManifest(invalidDowntown, game, error), "Downtown field type enforced");
    {
        std::ifstream in(fs::path(SCIFI_TEST_ASSETS) / "models" / "Duck.glb", std::ios::binary);
        const std::string duck((std::istreambuf_iterator<char>(in)), {});
        check(!sf::readGameplayPackage(archive({{"plugin.json", downtownManifest}, {"scripts/main.lua", "return {}"},
                                               {"assets/garage.glb", duck}}), game, error) &&
              error.find("256px") != std::string::npos, "Downtown texture budget stricter than gameplay model budget");
    }
    check(sf::readGameplayPackage(archive({{"plugin.json", gameplayManifest}, {"scripts/main.lua", code},
                                          {"assets/nodes.glb", glb(nodesJson, binary)}}), game, error) &&
          game.models.at("assets/nodes.glb").vertices[24] == 5,
          "packaged multi-node model retains its baked transforms");
    check(sf::readGameplayPackage(archive({{"plugin.json", gameplayManifest}, {"scripts/main.lua", code},
                                          {"assets/scaled.glb", glb(normalJson, normalBinary)}}), game, error) &&
          std::abs(game.models.at("assets/scaled.glb").vertices[4] - 0.4472136f) < 0.00001f,
          "packaged scaled GLB retains transformed normals");
    const auto fullGameZip = archive({{"plugin.json", gameplayManifest}, {"scripts/main.lua", "return {}"},
                                      {"assets/car.glb", modelData}, {"assets/car.png", validPng}});
    check(sf::readGameplayPackage(fullGameZip, game, error) && game.id == "com.example.game" &&
          game.scene && game.world && game.script == "return {}" &&
          game.downtownModel.empty() &&
          game.models.at("assets/car.glb").indices.size() == 3 &&
          game.textures.at("assets/car.png").rgba.size() == 4, "validate gameplay package and assets");
    const std::string sceneScript =
        "local car=scene_model(1,1,0,0,0,1,1,1,1,1,1) "
        "return {on_input=function(control,value) if control=='steer' then scene_rotate(car,0,value,0) end end,"
        "on_tick=function(dt) scene_move(car,dt,0,0) end}";
    const auto executableZip = archive({{"plugin.json", gameplayManifest}, {"scripts/main.lua", sceneScript},
                                        {"assets/car.glb", modelData}, {"assets/car.png", validPng}});
    check(sf::readGameplayPackage(executableZip, game, error), "validate executable gameplay package");
    sf::PluginScriptRuntime runtime;
    sf::PluginScriptRuntime::AssetCounts catalog{static_cast<int>(game.models.size()),
                                                  static_cast<int>(game.textures.size())};
    check(!runtime.start(game.script, error), "package scene API unavailable without explicit grant");
    check(runtime.start(game.script, error, true, nullptr, &catalog) &&
          runtime.input("steer", 0.5, error) && runtime.tick(0.25, error) &&
          runtime.models().size() == 1 && runtime.models()[0].transform.x == 0.25 &&
          runtime.models()[0].transform.rotateY == 0.5, "packaged script drives model instance");
    check(runtime.stop(error) && runtime.models().empty(), "packaged script releases model instances");
    check(runtime.start(game.script, error, true, nullptr, &catalog) && runtime.models().size() == 1 &&
          runtime.stop(error) && runtime.models().empty(), "packaged script can restart");
    auto invalidGame = gameplayManifest;
    const std::string sceneWorld = "\"scene\",\"world\"";
    auto inputManifest = gameplayManifest;
    inputManifest.replace(inputManifest.find(sceneWorld), sceneWorld.size(), "\"input\",\"scene\",\"world\"");
    check(sf::PluginPackageStore::parseGameplayManifest(inputManifest, game, error) && game.input && game.scene && game.world,
          "input permission is explicit and order independent");
    auto viewManifest = gameplayManifest;
    viewManifest.replace(viewManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"view\"");
    check(sf::PluginPackageStore::parseGameplayManifest(viewManifest, game, error) && game.view && !game.world,
          "view permission is explicit");
    auto soundManifest = gameplayManifest;
    soundManifest.replace(soundManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"sound\"");
    check(sf::PluginPackageStore::parseGameplayManifest(soundManifest, game, error) && game.sound && !game.world,
          "sound permission is explicit");
    auto storageManifest = gameplayManifest;
    storageManifest.replace(storageManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"storage\"");
    check(sf::PluginPackageStore::parseGameplayManifest(storageManifest, game, error) && game.storage,
          "persistent storage permission is explicit");
    auto messageManifest = gameplayManifest;
    messageManifest.replace(messageManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"messaging\"");
    check(sf::PluginPackageStore::parseGameplayManifest(messageManifest, game, error) && game.messaging,
          "messaging permission is explicit");
    auto controllerManifest = gameplayManifest;
    controllerManifest.replace(controllerManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"input\",\"controller\"");
    check(sf::PluginPackageStore::parseGameplayManifest(controllerManifest, game, error) && game.controller && game.input,
          "controller permission is explicit");
    auto invalidController = gameplayManifest;
    invalidController.replace(invalidController.find(sceneWorld), sceneWorld.size(), "\"scene\",\"controller\"");
    check(!sf::PluginPackageStore::parseGameplayManifest(invalidController, game, error),
          "controller permission requires input permission");
    auto hudManifest = gameplayManifest;
    hudManifest.replace(hudManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"hud\"");
    check(sf::PluginPackageStore::parseGameplayManifest(hudManifest, game, error) && game.hud && !game.world,
          "HUD permission is explicit");
    auto layoutManifest = gameplayManifest;
    layoutManifest.replace(layoutManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"world\",\"layout\"");
    check(sf::PluginPackageStore::parseGameplayManifest(layoutManifest, game, error) && game.layout && game.world,
          "city layout permission is explicit");
    auto layoutOnlyManifest = gameplayManifest;
    layoutOnlyManifest.replace(layoutOnlyManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"layout\"");
    check(!sf::PluginPackageStore::parseGameplayManifest(layoutOnlyManifest, game, error),
          "city layout requires world availability permission");
    auto impulseManifest = gameplayManifest;
    impulseManifest.replace(impulseManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"world\",\"impulse\"");
    check(sf::PluginPackageStore::parseGameplayManifest(impulseManifest, game, error) && game.impulse && game.world,
          "player push permission is explicit");
    auto impulseOnlyManifest = gameplayManifest;
    impulseOnlyManifest.replace(impulseOnlyManifest.find(sceneWorld), sceneWorld.size(), "\"scene\",\"impulse\"");
    check(!sf::PluginPackageStore::parseGameplayManifest(impulseOnlyManifest, game, error),
          "player push requires world permission");
    auto controlManifest = gameplayManifest;
    controlManifest.replace(controlManifest.find(sceneWorld), sceneWorld.size(),
                            "\"scene\",\"world\",\"input\",\"view\",\"control\"");
    check(sf::PluginPackageStore::parseGameplayManifest(controlManifest, game, error) && game.control,
          "control requires a distinct explicit permission");
    for (const std::string missing : {"world", "input", "view"}) {
        auto incomplete = controlManifest;
        const auto token = "\"" + missing + "\",";
        incomplete.erase(incomplete.find(token), token.size());
        check(!sf::PluginPackageStore::parseGameplayManifest(incomplete, game, error),
              "control rejects missing required permission");
    }
    const auto inputZip = archive({{"plugin.json", inputManifest}, {"scripts/main.lua", sceneScript},
                                   {"assets/car.glb", modelData}, {"assets/car.png", validPng}});
    {
        sf::GameplayControlLease lease;
        using Pose = sf::GameplayControlLease::Pose;
        const Pose origin{0, 1.7, 0, 0, 0}, nearby{3, 1.7, 0, 0, 0}, far{5, 1.7, 0, 0, 0};
        const Pose good{2, 1.7, 0, 0.3, 0.3},
                   fast{sf::kGameplayControlSpeedLimit * 0.1 + 0.001, 1.7, 0, 0, 0};
        const Pose invalid{0, 1.7, 0, std::numeric_limits<double>::quiet_NaN(), 0};
        check(!lease.acquire(1, true, true, false, origin, nearby) &&
              !lease.acquire(1, false, true, true, origin, nearby) &&
              !lease.acquire(1, true, false, true, origin, nearby) &&
              !lease.acquire(1, true, true, true, origin, far) &&
              !lease.acquire(1, true, true, true, invalid, nearby),
              "control acquisition requires consent, focused world, explicit activation and proximity");
        check(lease.acquire(1, true, true, true, origin, nearby) &&
              !lease.acquire(2, true, true, true, origin, nearby) &&
              !lease.accepts(2, true, origin, good, 0.1) &&
              lease.accepts(1, true, origin, good, 0.1),
              "control lease grants one owner and accepts bounded motion");
        check(!lease.accepts(1, true, origin, fast, 0.1) &&
              !lease.accepts(1, true, origin, invalid, 0.1) &&
              !lease.accepts(1, true, origin, good, 0) &&
              !lease.accepts(1, true, origin, good, 0.11) &&
              !lease.accepts(1, true, origin, good, std::numeric_limits<double>::quiet_NaN()) &&
              !lease.accepts(1, true, origin, Pose{0, 1.7, 0, 0, 1.5}, 0.1) &&
              lease.accepts(1, true, Pose{0, 1.7, 0, 3.13, 0},
                            Pose{0, 1.7, 0, -3.13, 0}, 0.1) && lease.owner() == 1,
              "control lease rejects nonfinite, oversized and invalid-time poses");
        lease.releaseOwner(2);
        check(lease.owner() == 1 && !lease.accepts(1, false, origin, good, 0.1) &&
              lease.owner() == 0 && lease.acquire(2, true, true, true, origin, nearby),
              "unavailable context revokes lease and another owner can acquire it");
        lease.releaseOwner(2);
        check(lease.owner() == 0 && !lease.accepts(2, true, origin, good, 0.1),
              "owner shutdown revokes control");
    }
    {
        sf::GameplayInput input;
        sf::GameplayImpulse first, second;
        check(first.request(12, 8, 0) && !first.request(1, 0, 0) && second.request(1, 0, 0),
              "player push rate limit is independent per owner");
        double x = 0, y = 0, z = 0;
        first.step(0.1, x, y, z);
        check(x > 1.19 && x < 1.21 && y > 0.79 && y < 0.81,
              "player push integrates bounded owner velocity");
        first.clearMotion();
        x = y = z = 0;
        first.step(0.1, x, y, z);
        second.step(0.1, x, y, z);
        check(x > 0.09 && x < 0.11 && y == 0, "revocation removes only owner's motion");
        first.advance(2);
        check(first.request(1, 0, 0), "player push token replenishes after two seconds");
        first.reset();
        x = y = z = 0;
        first.step(0.1, x, y, z);
        check(x == 0 && y == 0 && z == 0, "player push reset clears residual velocity");
        sf::GameplayInputContext context;
        check(!context.available(), "world input unavailable outside focused world");
        context.fileRoom = context.windowFocused = context.worldPointer = true;
        check(context.available(), "focused world permits plugin input");
        bool sf::GameplayInputContext::* blocked[] = {
            &sf::GameplayInputContext::typing, &sf::GameplayInputContext::focusedWindow,
            &sf::GameplayInputContext::capturing, &sf::GameplayInputContext::driving,
            &sf::GameplayInputContext::riding, &sf::GameplayInputContext::vr,
            &sf::GameplayInputContext::harvi, &sf::GameplayInputContext::settings,
            &sf::GameplayInputContext::start, &sf::GameplayInputContext::viewer,
            &sf::GameplayInputContext::switcher, &sf::GameplayInputContext::apps,
            &sf::GameplayInputContext::keyboard};
        for (auto member : blocked) {
            context.*member = true;
            check(!context.available(), "world input blocked by UI or exclusive mode");
            context.*member = false;
        }
        context.worldPointer = false;
        check(!context.available(), "world input blocked while pointer is over window");
        context.worldPointer = true;
        context.windowFocused = false;
        check(!context.available(), "world input blocked when OS focus lost");
        std::vector<std::pair<std::string, double>> events;
        const auto dispatch = [&](const char* name, double value) {
            events.emplace_back(name, value);
            return true;
        };
        check(input.update(false, true, 1, 0, 0, true, false, dispatch) && events.empty(),
              "input router suppresses events without permission");
        check(input.update(true, true, 1, 0, 0, true, false, dispatch) &&
              events == std::vector<std::pair<std::string, double>>{{"move_x", 1}, {"click", 1}},
              "input router dispatches permitted controls");
        events.clear();
        check(input.update(true, false, 1, 0, 0, false, false, dispatch) &&
              events == std::vector<std::pair<std::string, double>>{{"move_x", 0}, {"click", 0}},
              "input router releases held controls when focus or overlay blocks input");
        events.clear();
        check(input.update(true, false, -1, 1, 1, true, false, dispatch) && events.empty(),
              "input router does not leak input while unavailable");
        check(input.update(true, true, -1, 0, 0, false, false, dispatch) &&
              events == std::vector<std::pair<std::string, double>>{{"move_x", -1}},
              "input router resumes after focus returns");
    }
    auto duplicatePermission = gameplayManifest;
    duplicatePermission.replace(duplicatePermission.find(sceneWorld), sceneWorld.size(), "\"scene\",\"scene\"");
    check(!sf::PluginPackageStore::parseGameplayManifest(duplicatePermission, game, error),
          "duplicate permission rejected");
    {
        const auto networked = [&](const std::string& permissions, const std::string& hosts) {
            auto text = gameplayManifest;
            text.replace(text.find(sceneWorld), sceneWorld.size(), permissions);
            if (!hosts.empty()) text.insert(text.size() - 1, ",\"network_hosts\":" + hosts);
            sf::GameplayPackage parsed;
            std::string parseError;
            const bool ok = sf::PluginPackageStore::parseGameplayManifest(text, parsed, parseError);
            return std::make_pair(ok ? parsed : sf::GameplayPackage{}, ok ? std::string{} : parseError);
        };
        auto valid = networked("\"scene\",\"network\"", R"(["api.example.com","news.example.org"])");
        check(valid.second.empty() && valid.first.network &&
                  valid.first.networkHosts == std::vector<std::string>{"api.example.com", "news.example.org"},
              "network permission with declared hosts accepted");
        check(networked("\"scene\",\"network\"", "").second.find("manifest") != std::string::npos,
              "network permission without hosts rejected");
        check(!networked(sceneWorld, R"(["api.example.com"])").second.empty(),
              "network hosts without permission rejected");
        for (const char* bad : {R"(["localhost"])", R"(["127.0.0.1"])", R"(["10.0.0.1"])", R"(["[::1]"])",
                                R"(["https://api.example.com"])", R"(["api.example.com:8080"])", R"(["*.example.com"])",
                                R"(["metadata.google.internal"])", R"(["Api.example.com"])", R"([])", R"(["a.com","a.com"])",
                                R"(["a1.com","a2.com","a3.com","a4.com","a5.com"])"})
            check(!networked("\"scene\",\"network\"", bad).second.empty(), (std::string("invalid network host rejected: ") + bad).c_str());
        check(!networked("\"scene\",\"network\"", "[\"" + std::string(60, 'a') + ".com\"]").second.empty(),
              "network host longer than 63 characters rejected");
        check(networked("\"scene\",\"network\"", "[\"" + std::string(59, 'a') + ".com\"]").second.empty(),
              "63-character network host accepted");
        check(!networked("\"scene\",\"network\"",
                         "[\"" + std::string(40, 'a') + ".com\",\"" + std::string(40, 'b') + ".com\",\"" + std::string(40, 'c') + ".com\"]")
                   .second.empty(),
              "network host list total length capped");
    }
    invalidGame.replace(invalidGame.find(sceneWorld), sceneWorld.size(), "\"scene\",\"telepathy\"");
    check(!sf::readGameplayPackage(archive({{"plugin.json", invalidGame}, {"scripts/main.lua", code}}),
                                   game, error) && game.id.empty() && error.find("manifest") != std::string::npos,
          "unsupported permission clears package");
    invalidGame = gameplayManifest;
    const std::string api1 = "\"api_version\":\"1\"";
    invalidGame.replace(invalidGame.find(api1), api1.size(), "\"api_version\":\"2\"");
    check(!sf::readGameplayPackage(archive({{"plugin.json", invalidGame}, {"scripts/main.lua", code}}),
                                   game, error), "unknown gameplay API rejected");
    check(!sf::readGameplayPackage(archive({{"plugin.json", gameplayManifest}, {"scripts/main.lua", code},
                                            {"assets/a.png", "not a PNG"}}), game, error) &&
          game.textures.empty() && game.id.empty() && error.find("assets/a.png") != std::string::npos,
          "invalid asset rejects entire package with error");
    check(!sf::readGameplayPackage(archive({{"plugin.json", gameplayManifest}, {"scripts/main.lua", code},
                                            {"assets/1.png", maxPng}, {"assets/2.png", maxPng},
                                            {"assets/3.png", maxPng}, {"assets/4.png", maxPng},
                                            {"assets/5.png", maxPng}}), game, error) &&
          game.id.empty() && error.find("memory limit") != std::string::npos,
          "aggregate decoded asset budget enforced");
    auto gameplayLink = gameplayZip;
    const auto gameplayCentral = gameplayLink.find(std::string("PK\x01\x02", 4));
    if (gameplayCentral == std::string::npos) ++failures;
    else {
        gameplayLink[gameplayCentral + 38] = 0; gameplayLink[gameplayCentral + 39] = 0;
        gameplayLink[gameplayCentral + 40] = static_cast<char>(0xff);
        gameplayLink[gameplayCentral + 41] = static_cast<char>(0xa1);
        check(!sf::readGameplayArchive(gameplayLink, gameplay, error), "gameplay symlink rejected");
    }
    auto corrupt = zip;
    if (corrupt.size() > 41) {
        corrupt[41] ^= 0x7f;
        check(!sf::PluginPackageStore::readZip(corrupt, script, error), "corrupted ZIP data rejected");
    } else ++failures;
    auto badPermission = manifest;
    badPermission.replace(badPermission.find("\"permissions\":[]"), 16, "\"permissions\":[\"network\"]");
    check(!sf::PluginPackageStore::readZip(archive({{"plugin.json", badPermission}, {"scripts/main.lua", code}}), script, error), "permissions rejected");
    const fs::path dir = fs::temp_directory_path() / ("scifi-package-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    const fs::path source = dir / "source.sfplugin", installed = dir / "installed";
    {
        const auto path = dir / "large-text.zip";
        const auto write = [&](const std::string& bytes) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            check(bool(out), "write large text ZIP fixture");
        };
        std::string largeManifest = gameplayManifest;
        largeManifest.erase(largeManifest.find(",\"world\""), std::string(",\"world\"").size());
        largeManifest.resize(sf::kPluginTextLimit, ' ');
        std::string largeSource = "return {on_tick=function(dt) if dt<0 then missing() end end}";
        largeSource.resize(sf::kPluginTextLimit, ' ');
        sf::GameplayPackage largeGame;
        check(sf::PluginPackageStore::parseGameplayManifest(largeManifest, largeGame, error),
              "gameplay manifest parser accepts exactly 10 MiB");
        write(archive({{"plugin.json", largeManifest}, {"scripts/main.lua", largeSource}}, MZ_NO_COMPRESSION));
        check(sf::readGameplayPackageFile(path, largeGame, error) &&
              largeGame.script == largeSource, "file-backed gameplay accepts both text files at exactly 10 MiB");
        sf::GameplayDigest digest;
        sf::GameplayConsent consent(dir / "large-consent");
        sf::GameplaySession session;
        check(sf::gameplayDigestFile(path, digest, error) &&
              consent.grantFile("com.example.game", path, digest, error) &&
              session.startFile(path, digest, consent, nullptr, error) &&
              session.tick(0.01, error) && session.stop(error),
              "10 MiB installed-path script validates, receives consent, loads and runs");
        largeManifest.push_back(' ');
        check(!sf::PluginPackageStore::parseGameplayManifest(largeManifest, largeGame, error),
              "gameplay manifest parser rejects 10 MiB plus one byte");
        write(archive({{"plugin.json", largeManifest}, {"scripts/main.lua", "return {}"}}));
        check(!sf::readGameplayPackageFile(path, largeGame, error) && largeGame.id.empty(),
              "file-backed gameplay rejects manifest one byte over without partial output");
        largeSource.push_back(' ');
        write(archive({{"plugin.json", gameplayManifest}, {"scripts/main.lua", largeSource}}));
        check(!sf::readGameplayPackageFile(path, largeGame, error) && largeGame.id.empty(),
              "file-backed gameplay rejects source one byte over without partial output");

        largeManifest = manifest;
        largeManifest.resize(sf::kPluginTextLimit, ' ');
        largeSource = code;
        largeSource.resize(sf::kPluginTextLimit, ' ');
        const auto bytes = archive({{"plugin.json", largeManifest}, {"scripts/main.lua", largeSource}},
                                   MZ_NO_COMPRESSION);
        write(bytes);
        check(bytes.size() > 20 * 1024 * 1024 && bytes.size() <= sf::kScriptArchiveLimit &&
              sf::PluginPackageStore::readZip(bytes, script, error),
              "script ZIP disk cap fits two uncompressed 10 MiB files");
        sf::PluginPackageStore largeStore(dir / "large-installed",
            [](const sf::ThemePackage&) { return true; },
            [&](const sf::ScriptPackage& pkg) { return pkg.script == largeSource; });
        check(largeStore.install(path), "script disk loader and installation accept uncompressed boundary ZIP");
        const auto themePath = dir / "large.sfplugin";
        std::string largeTheme = good;
        largeTheme.resize(sf::kPluginTextLimit, ' ');
        { std::ofstream out(themePath, std::ios::binary); out << largeTheme; }
        check(largeStore.install(themePath), "theme disk loader accepts exactly 10 MiB");
        largeTheme.push_back(' ');
        { std::ofstream out(themePath, std::ios::binary | std::ios::trunc); out << largeTheme; }
        check(!largeStore.install(themePath) && largeStore.error().find("too large") != std::string::npos,
              "theme disk loader rejects 10 MiB plus one byte");
        check(largeStore.uninstall("com.example.clock") &&
              largeStore.uninstall("com.example.twilight"), "remove large installed packages");
        check(consent.revoke("com.example.game", error), "remove large package consent");
        fs::remove(dir / "large-installed");
        fs::remove(dir / "large-consent");
        fs::remove(themePath);
        fs::remove(path);
    }
    {
        sf::GameplayDigest abc;
        const std::array<unsigned char, 32> expected = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
        check(sf::gameplayDigestBytes("abc", abc, error) && abc.size == 3 && abc.sha256 == expected,
              "SHA-256 known vector");
        auto sceneManifest = gameplayManifest;
        sceneManifest.replace(sceneManifest.find("\"scene\",\"world\""), 15, "\"scene\"");
        const auto bytes = archive({{"plugin.json", sceneManifest}, {"scripts/main.lua", "return {}"}});
        const auto packagePath = dir / "file-consent.zip";
        const auto consentDir = dir / "file-consents";
        auto write = [&](const std::string& value) {
            std::ofstream out(packagePath, std::ios::binary | std::ios::trunc);
            return static_cast<bool>(out.write(value.data(), static_cast<std::streamsize>(value.size())));
        };
        check(write(bytes), "write file-backed consent fixture");
        sf::GameplayDigest digest, memoryDigest;
        check(sf::gameplayDigestFile(packagePath, digest, error) &&
              sf::gameplayDigestBytes(bytes, memoryDigest, error) && digest == memoryDigest,
              "file and memory package digests agree");
        sf::GameplayConsent fileConsent(consentDir);
        bool granted = false;
        sf::GameplaySession fileSession;
        check(!fileSession.startFile(packagePath, digest, fileConsent, nullptr, error) &&
              !fileSession.running(), "file-backed session requires consent");
        check(fileConsent.grant("com.example.game", bytes, error) &&
              fileConsent.matchesDigest("com.example.game", digest, granted, error) && granted,
              "legacy consent is compatible with file digest");
#ifdef _WIN32
        {
            sf::GameplayFileLock lock;
            check(lock.open(packagePath, error), "acquire package read lock");
            std::ofstream writer(packagePath, std::ios::binary | std::ios::trunc);
            check(!writer, "read lock prevents concurrent package modification");
        }
#endif
        check(fileConsent.grantFile("com.example.game", packagePath, digest, error) &&
              fs::file_size(consentDir / "com.example.game.grant") == 48 &&
              fileSession.startFile(packagePath, digest, fileConsent, nullptr, error) &&
              fileSession.stop(error), "file-backed consent starts session");
        sf::GameplayConsent restarted(consentDir);
        check(restarted.matchesDigest("com.example.game", digest, granted, error) && granted,
              "digest consent survives restart");
        auto modified = bytes;
        modified[modified.size() / 2] ^= 1;
        sf::GameplayDigest changed;
        check(write(modified) && sf::gameplayDigestFile(packagePath, changed, error) &&
              !(changed == digest) && !fileSession.startFile(packagePath, digest, restarted, nullptr, error) &&
              restarted.matchesDigest("com.example.game", changed, granted, error) && !granted &&
              !restarted.grantFile("com.example.game", packagePath, digest, error),
              "same-size package mutation is rejected");
        check(write(bytes) && restarted.matchesDigest("com.example.game", digest, granted, error) && granted,
              "failed file grant preserves previous approval");
        { std::ofstream out(consentDir / "com.example.game.grant", std::ios::binary | std::ios::trunc);
          out << std::string(48, 'x'); }
        check(restarted.matchesDigest("com.example.game", digest, granted, error) && !granted &&
              !fileSession.startFile(packagePath, digest, restarted, nullptr, error),
              "corrupted consent cannot authorize package");
        check(restarted.revoke("com.example.game", error), "file consent revocation");
        const auto firstCentral = bytes.find(std::string("PK\x01\x02", 4));
        if (firstCentral == std::string::npos) {
            check(false, "find file-backed ZIP central directory");
        } else {
            auto corrupted = bytes;
            corrupted[firstCentral + 16] ^= 1;
            sf::GameplayPackage invalid;
            check(write(corrupted) && !sf::readGameplayPackageFile(packagePath, invalid, error) &&
                  invalid.id.empty(), "file-backed ZIP rejects CRC mismatch without partial output");
            auto encrypted = bytes;
            encrypted[firstCentral + 8] |= 1;
            check(write(encrypted) && !sf::readGameplayPackageFile(packagePath, invalid, error) &&
                  invalid.id.empty(), "file-backed ZIP rejects encrypted entry");
            auto link = bytes;
            link[firstCentral + 38] = 0; link[firstCentral + 39] = 0;
            link[firstCentral + 40] = static_cast<char>(0xff); link[firstCentral + 41] = static_cast<char>(0xa1);
            check(write(link) && !sf::readGameplayPackageFile(packagePath, invalid, error) &&
                  invalid.id.empty(), "file-backed ZIP rejects symlink entry");
            check(write(bytes.substr(0, bytes.size() - 10)) &&
                  !sf::readGameplayPackageFile(packagePath, invalid, error) && invalid.id.empty(),
                  "file-backed ZIP rejects truncated archive");
        }
        fs::remove(consentDir);
        fs::remove(packagePath);
    }
    {
        const auto gameSource = dir / "game.zip";
        const auto gameDir = dir / "games";
        { std::ofstream out(gameSource, std::ios::binary); out.write(executableZip.data(), executableZip.size()); }
        std::set<std::string> registered;
        sf::PluginPackageStore gameStore(gameDir, [](const sf::ThemePackage&) { return true; }, {},
            [&](const sf::GameplayPackage& package, const fs::path& path, const sf::GameplayDigest& digest) {
                sf::GameplayDigest expected;
                return package.id == "com.example.game" && path == gameDir / "com.example.game.zip" &&
                       sf::gameplayDigestBytes(executableZip, expected, error) && digest == expected &&
                       registered.insert(package.id).second;
            });
        check(gameStore.install(gameSource) && registered.count("com.example.game") == 1,
              "install validated gameplay package");
        check(!gameStore.install(gameSource), "duplicate gameplay package rejected");
        const auto badSource = dir / "bad-game.zip";
        const auto badGameZip = archive({{"plugin.json", gameplayManifest}, {"scripts/main.lua", code},
                                         {"assets/duck.glb", "invalid"}});
        { std::ofstream out(badSource, std::ios::binary); out.write(badGameZip.data(), badGameZip.size()); }
        check(!gameStore.install(badSource) && registered.size() == 1, "invalid asset is not registered");
        check(fs::file_size(gameDir / "com.example.game.zip") == executableZip.size(),
              "failed install preserves previously installed package");
        registered.clear();
        sf::PluginPackageStore restartedGameStore(gameDir, [](const sf::ThemePackage&) { return true; }, {},
            [&](const sf::GameplayPackage& package, const fs::path& path, const sf::GameplayDigest& digest) {
                sf::GameplayDigest expected;
                return package.id == "com.example.game" && path == gameDir / "com.example.game.zip" &&
                       sf::gameplayDigestBytes(executableZip, expected, error) && digest == expected &&
                       registered.insert(package.id).second;
            });
        restartedGameStore.discover();
        check(restartedGameStore.error().empty() && registered.count("com.example.game") == 1,
              "gameplay package discovered after restart");
        check(restartedGameStore.uninstall("com.example.game"), "uninstall gameplay package");
        registered.clear();
        restartedGameStore.discover();
        check(registered.empty(), "uninstalled gameplay package not rediscovered");
        sf::PluginPackageStore rejectStore(gameDir, [](const sf::ThemePackage&) { return true; }, {},
            [](const sf::GameplayPackage&, const fs::path&, const sf::GameplayDigest&) { return false; });
        check(!rejectStore.install(gameSource) && !fs::exists(gameDir / "com.example.game.zip"),
              "failed registration removes copied package");
        {
            const auto quotaFile = gameDir / "quota-fixture.zip";
            bool sparse = true;
#ifdef _WIN32
            HANDLE file = CreateFileW(quotaFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD ignored = 0;
            sparse = file != INVALID_HANDLE_VALUE &&
                     DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
#else
            { std::ofstream out(quotaFile, std::ios::binary); sparse = bool(out); }
#endif
            std::error_code ec;
            if (sparse) fs::resize_file(quotaFile, sf::kGameplayInstalledZipLimit - executableZip.size(), ec);
            check(sparse && !ec, "create sparse ZIP quota fixture");
            if (sparse && !ec) {
                registered.clear();
                check(gameStore.install(gameSource), "installed ZIP quota accepts exact 4 GiB");
                check(gameStore.uninstall("com.example.game"), "remove exact quota package");
                registered.clear();
                fs::resize_file(quotaFile, sf::kGameplayInstalledZipLimit - executableZip.size() + 1, ec);
                check(!ec && !gameStore.install(gameSource) &&
                      gameStore.error().find("limit") != std::string::npos,
                      "installed ZIP quota rejects one byte over 4 GiB");
                const auto quotaScript = dir / "quota-script.zip";
                { std::ofstream out(quotaScript, std::ios::binary); out.write(zip.data(), zip.size()); }
                fs::resize_file(quotaFile, sf::kGameplayInstalledZipLimit - zip.size() + 1, ec);
                sf::PluginPackageStore quotaScriptStore(gameDir, [](const sf::ThemePackage&) { return true; },
                    [](const sf::ScriptPackage&) { return true; });
                check(!ec && !quotaScriptStore.install(quotaScript) &&
                      quotaScriptStore.error().find("limit") != std::string::npos,
                      "script packages also respect installed ZIP quota");
                fs::remove(quotaScript, ec);
                check(!ec, "remove script quota fixture");
            }
            fs::remove(quotaFile, ec);
            check(!ec, "remove sparse ZIP quota fixture");
        }
        fs::remove(gameDir);
        fs::remove(gameSource);
        fs::remove(badSource);
    }
    {
        sf::GameplayConsent consent(dir / "consents");
        bool granted = true;
        check(consent.matches("com.example.game", executableZip, granted, error) && !granted,
              "gameplay consent absent by default");
        check(!consent.grant("com.example.wrong", executableZip, error), "consent refuses different plugin ID");
        check(!consent.grant("com.example.game", fullGameZip.substr(0, fullGameZip.size() / 2), error),
              "consent refuses invalid package");
        check(consent.grant("com.example.game", executableZip, error) &&
              consent.matches("com.example.game", executableZip, granted, error) && granted,
              "explicit consent binds exact package bytes");
        check(!consent.grant("com.example.game", executableZip.substr(0, 10), error) &&
              consent.matches("com.example.game", executableZip, granted, error) && granted,
              "failed replacement preserves existing consent");
        sf::GameplaySession session;
        {
            const auto storageZip = archive({{"plugin.json", storageManifest}, {"scripts/main.lua",
                "local n=storage_get('count') or 0 storage_set('count',n+1) return {}"}});
            check(consent.grant("com.example.game", storageZip, error) &&
                  !session.start(storageZip, consent, nullptr, error) &&
                  session.start(storageZip, consent, nullptr, error, nullptr, nullptr, dir / "storage") &&
                  session.stop(error) &&
                  session.start(storageZip, consent, nullptr, error, nullptr, nullptr, dir / "storage") &&
                  session.stop(error), "consented session loads and persists private storage");
            sf::GameplayStorage stored(dir / "storage", "com.example.game");
            const sf::GameplayStorage::Value* storedValue = nullptr;
            check(stored.load(error) && stored.get("count", storedValue, error) && storedValue &&
                  std::get<double>(*storedValue) == 2, "storage survives a session restart");
            check(consent.grant("com.example.game", executableZip, error), "restore original package consent");
            fs::remove(dir / "storage" / "com.example.game.store");
            fs::remove(dir / "storage");
        }
        {
            const auto example = fs::path(SCIFI_TEST_ASSETS).parent_path() / "examples" / "plugins" / "storage-demo";
            const auto read = [](const fs::path& path) {
                std::ifstream in(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), {});
            };
            const auto exampleZip = archive({{"plugin.json", read(example / "plugin.json")},
                                             {"scripts/main.lua", read(example / "scripts" / "main.lua")}});
            bool exampleOk = consent.grant("com.example.storage-demo", exampleZip, error) &&
                session.start(exampleZip, consent, nullptr, error, nullptr, nullptr, dir / "storage-example");
            if (exampleOk) {
                exampleOk = std::string(session.hudText().text.data()) == "STARTS: 1";
                exampleOk = session.stop(error) && exampleOk;
            }
            if (exampleOk) {
                exampleOk = session.start(exampleZip, consent, nullptr, error, nullptr, nullptr, dir / "storage-example");
                if (exampleOk) {
                    exampleOk = std::string(session.hudText().text.data()) == "STARTS: 2";
                    exampleOk = session.stop(error) && exampleOk;
                }
            }
            check(exampleOk, "packaged storage example counts launches across restarts");
            check(consent.revoke("com.example.storage-demo", error), "revoke storage example consent");
            fs::remove(dir / "storage-example" / "com.example.storage-demo.store");
            fs::remove(dir / "storage-example");
        }
        {
            const auto example = fs::path(SCIFI_TEST_ASSETS).parent_path() / "examples" / "plugins" / "audio-demo";
            const auto read = [](const fs::path& path) {
                std::ifstream in(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), {});
            };
            const auto manifest = read(example / "plugin.json");
            const auto script = read(example / "scripts" / "main.lua");
            const auto sound = read(example / "assets" / "tone.ogg");
            const auto zip = archive({{"plugin.json", manifest}, {"scripts/main.lua", script},
                                      {"assets/tone.ogg", sound}});
            bool ok = !sound.empty() && consent.grant("com.example.audio-demo", zip, error) &&
                session.start(zip, consent, nullptr, error);
            if (ok) {
                const auto commands = session.takeSamples();
                ok = commands.count == 1 && commands.events[0].asset == 0 &&
                     commands.events[0].voice == 1 && commands.events[0].loop &&
                     commands.events[0].volume == 0.5f && session.package()->audio.size() == 1;
                ok = session.stop(error) && ok;
            }
            check(ok, "packaged OGG example creates a looping sample voice");
            check(consent.revoke("com.example.audio-demo", error), "revoke audio example consent");
            auto withoutSound = manifest;
            const auto soundPermission = withoutSound.find(",\"sound\"");
            if (soundPermission != std::string::npos) withoutSound.erase(soundPermission, 8);
            const auto noSound = archive({{"plugin.json", withoutSound}, {"scripts/main.lua", code},
                                          {"assets/tone.ogg", sound}});
            sf::GameplayPackage invalidAudio;
            check(soundPermission != std::string::npos && !sf::readGameplayPackage(noSound, invalidAudio, error),
                  "audio requires sound permission");
            const auto badSound = archive({{"plugin.json", manifest}, {"scripts/main.lua", script},
                                           {"assets/tone.ogg", "invalid"}});
            check(!sf::readGameplayPackage(badSound, invalidAudio, error), "reject malformed audio asset");
        }
        sf::PluginScriptRuntime::WorldQueries world{
            nullptr,
            [](void*, double, double, double, double, double, double* out) noexcept { *out = 0; return true; },
            [](void*, double*, double*, double*, double, double) noexcept { return true; },
            [](void*) noexcept { return true; }};
        {
            const auto pushZip = archive({{"plugin.json", impulseManifest},
                {"scripts/main.lua", "return {on_tick=function() player_impulse(1,2,3) end}"}});
            check(consent.grant("com.example.game", pushZip, error) &&
                  session.start(pushZip, consent, &world, error) && session.tick(0.1, error) &&
                  session.takeImpulse().pending && !session.takeImpulse().pending && session.stop(error),
                  "consented session delivers player push once");
            const auto noPushZip = archive({{"plugin.json", gameplayManifest},
                {"scripts/main.lua", "return {on_tick=function() player_impulse(1,2,3) end}"}});
            check(consent.grant("com.example.game", noPushZip, error) &&
                  session.start(noPushZip, consent, &world, error) && !session.tick(0.1, error) &&
                  !session.running(), "undeclared player push is unavailable");
            check(consent.grant("com.example.game", executableZip, error), "restore original package consent");
        }
        {
            const auto layoutZip = archive({{"plugin.json", layoutManifest},
                {"scripts/main.lua", "return {on_tick=function() local x=city_cell(0,0) hud_missing=x end}"}});
            sf::PluginScriptRuntime::LayoutQuery layout{nullptr, [](void*, double, double, double* out) noexcept {
                out[0] = 1; out[1] = 1.3; out[2] = 2; out[3] = 28; out[4] = 0; return 1;
            }};
            check(consent.grant("com.example.game", layoutZip, error) &&
                  !session.start(layoutZip, consent, &world, error) &&
                  session.start(layoutZip, consent, &world, error, nullptr, &layout) &&
                  session.tick(0.1, error) && session.stop(error),
                  "layout package requires granted host service and runs through session");
            const auto noLayoutZip = archive({{"plugin.json", gameplayManifest},
                {"scripts/main.lua", "return {on_tick=function() city_cell(0,0) end}"}});
            check(consent.grant("com.example.game", noLayoutZip, error) &&
                  session.start(noLayoutZip, consent, &world, error, nullptr, &layout) &&
                  !session.tick(0.1, error) && !session.running(),
                  "host does not expose city layout without declared permission");
            check(consent.grant("com.example.game", executableZip, error), "restore original package consent");
        }
        {
            const auto root = fs::path(SCIFI_TEST_ASSETS).parent_path();
            const auto read = [](const fs::path& path) {
                std::ifstream in(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), {});
            };
            const auto example = root / "examples" / "plugins" / "duck-throw";
            const auto duck = read(fs::path(SCIFI_TEST_ASSETS) / "models" / "Duck.glb");
            const auto exampleZip = archive({{"plugin.json", read(example / "plugin.json")},
                                             {"scripts/main.lua", read(example / "scripts" / "main.lua")},
                                             {"assets/duck.glb", duck}});
            sf::PluginScriptRuntime::ViewQuery exampleView{nullptr, [](void*, double* out) noexcept {
                out[0] = 0; out[1] = 2; out[2] = 0;
                out[3] = 0; out[4] = 0; out[5] = -1;
                return true;
            }};
            check(!duck.empty() && consent.grant("com.example.duck-throw", exampleZip, error) &&
                  session.start(exampleZip, consent, &world, error, &exampleView) &&
                  std::string(session.hudText().text.data()) == "DUCKS ACTIVE: 0" &&
                  session.input("click", 1, error) && session.models().size() == 1 &&
                  std::string(session.hudText().text.data()) == "DUCKS ACTIVE: 1" &&
                  session.tick(0.1, error) && session.models()[0].transform.z < -2,
                  "packaged duck example launches a moving projectile from view pose");
            auto exampleSounds = session.takeSounds();
            check(exampleSounds.count == 1 &&
                  exampleSounds.events[0].sound == sf::PluginScriptRuntime::Sound::Throw,
                  "duck example queues throw sound");
            bool throwsOk = session.running();
            for (int i = 0; i < 20 && throwsOk; ++i) {
                throwsOk = session.input("click", 1, error);
                session.takeSounds();
            }
            check(throwsOk && session.models().size() == 16 && session.tick(0.1, error),
                  "duck example limits projectiles and stays within the host query budget");
            bool ticks = session.running();
            bool boom = false;
            for (int i = 0; i < 51 && ticks; ++i) {
                ticks = session.tick(0.1, error);
                auto events = session.takeSounds();
                for (size_t j = 0; j < events.count; ++j)
                    if (events.events[j].sound == sf::PluginScriptRuntime::Sound::Boom) boom = true;
            }
            check(ticks && boom && session.models().empty() &&
                  std::string(session.hudText().text.data()) == "DUCKS ACTIVE: 0" && session.stop(error) &&
                  session.hudText().text[0] == 0,
                  "packaged duck projectile expires and cleans up");
            check(session.start(exampleZip, consent, &world, error, &exampleView) &&
                  session.input("click", 1, error) && session.stop(error) && session.models().empty(),
                  "packaged duck example cleans up on disable and restart");
            check(consent.revoke("com.example.duck-throw", error), "revoke duck example consent");
        }
        {
            const auto root = fs::path(SCIFI_TEST_ASSETS).parent_path();
            const auto read = [](const fs::path& path) {
                std::ifstream in(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), {});
            };
            const auto example = root / "examples" / "plugins" / "park-cell";
            const auto exampleZip = archive({{"plugin.json", read(example / "plugin.json")},
                                             {"scripts/main.lua", read(example / "scripts" / "main.lua")},
                                             {"assets/duck.glb", read(fs::path(SCIFI_TEST_ASSETS) / "models" / "Duck.glb")}});
            struct CellState {
                bool occupied = false, available = true, blocked = false, sameCell = false;
                double viewX = 0, blockedX = 0, blockedZ = 0;
            } state;
            sf::PluginScriptRuntime::WorldQueries parkWorld{
                &state, world.floor, world.resolve,
                [](void* context) noexcept { return static_cast<CellState*>(context)->available; }};
            sf::PluginScriptRuntime::ViewQuery parkView{&state, [](void* context, double* out) noexcept {
                out[0] = static_cast<CellState*>(context)->viewX; out[1] = 2; out[2] = 0;
                out[3] = 0; out[4] = 0; out[5] = -1; return true;
            }};
            sf::PluginScriptRuntime::LayoutQuery parkLayout{&state,
                [](void* context, double x, double z, double* out) noexcept {
                    out[0] = std::floor(x / 28 + 0.5) * 28; out[1] = 1.3;
                    out[2] = std::floor(z / 28 + 0.5) * 28; out[3] = 28;
                    const auto& cell = *static_cast<CellState*>(context);
                    if (cell.sameCell) out[0] = out[2] = 0;
                    out[4] = cell.occupied || (cell.blocked && out[0] == cell.blockedX && out[2] == cell.blockedZ) ? 1 : 0;
                    return 1;
                }};
            check(consent.grant("com.example.park-cell", exampleZip, error) &&
                  session.start(exampleZip, consent, &parkWorld, error, &parkView, &parkLayout) &&
                  session.tick(0.1, error) && session.boxes().size() == 18 && session.models().size() == 3 &&
                  session.models()[0].transform.x != session.models()[1].transform.x &&
                  std::string(session.hudText().text.data()) == "PARK CELLS: 3",
                  "packaged park example creates three distinct parks and ducks");
            if (session.models().size() == 3) {
                const auto initial = session.models();
                state.blocked = true;
                state.blockedX = initial[0].transform.x;
                state.blockedZ = initial[0].transform.z;
                bool changed = true;
                for (int i = 0; i < 11 && changed; ++i) changed = session.tick(0.1, error);
                bool blockedGone = true, other1 = false, other2 = false;
                for (const auto& model : session.models()) {
                    blockedGone = blockedGone && model.transform.id != initial[0].transform.id;
                    other1 = other1 || model.transform.id == initial[1].transform.id;
                    other2 = other2 || model.transform.id == initial[2].transform.id;
                }
                check(changed && session.models().size() == 3 && blockedGone && other1 && other2,
                      "occupied park is replaced without disturbing other parks");
                state.blocked = false;
            }
            state.occupied = true;
            bool ticks = true;
            for (int i = 0; i < 11 && ticks; ++i) ticks = session.tick(0.1, error);
            check(ticks && session.boxes().empty() && session.models().empty() &&
                  std::string(session.hudText().text.data()) == "NO VACANT CELL NEARBY",
                  "park example removes scenery when cell becomes occupied");
            state.occupied = false;
            for (int i = 0; i < 11 && ticks; ++i) ticks = session.tick(0.1, error);
            check(ticks && session.boxes().size() == 18 && session.models().size() == 3,
                  "park example recreates scenery when cells are vacant");
            state.viewX = 300;
            for (int i = 0; i < 11 && ticks; ++i) ticks = session.tick(0.1, error);
            bool nearNewView = session.models().size() == 3;
            for (const auto& model : session.models()) nearNewView = nearNewView && model.transform.x > 200;
            check(ticks && nearNewView && session.boxes().size() == 18,
                  "park example retires distant cells and creates parks near moved view");
            state.available = false;
            for (int i = 0; i < 11 && ticks; ++i) ticks = session.tick(0.1, error);
            check(ticks && session.boxes().empty() && session.models().empty() &&
                  std::string(session.hudText().text.data()) == "WAITING FOR CITY" && session.stop(error),
                  "park example cleans up when city becomes unavailable");
            state.available = true;
            check(session.start(exampleZip, consent, &parkWorld, error, &parkView, &parkLayout) &&
                  session.tick(0.1, error) && session.stop(error) && session.models().empty(),
                  "park example restart and disable cleanup");
            state.sameCell = true;
            state.viewX = 0;
            check(session.start(exampleZip, consent, &parkWorld, error, &parkView, &parkLayout) &&
                  session.tick(0.1, error) && session.models().size() == 1 && session.boxes().size() == 6 &&
                  session.stop(error), "repeated layout samples do not duplicate a park");
            check(consent.revoke("com.example.park-cell", error), "revoke park example consent");
        }
        {
            const auto example = fs::path(SCIFI_TEST_ASSETS).parent_path() / "examples" / "plugins" / "car-track";
            const auto read = [](const fs::path& path) {
                std::ifstream in(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), {});
            };
            const auto exampleZip = archive({{"plugin.json", read(example / "plugin.json")},
                                            {"scripts/main.lua", read(example / "scripts" / "main.lua")},
                                            {"assets/car.glb", read(example / "assets" / "car.glb")},
                                            {"assets/paint.png", read(example / "assets" / "paint.png")}});
            struct CarWorld {
                bool available = true, wall = false, fail = false, slope = false, failFloor = false;
                int resolves = 0, floors = 0;
                double height = 0;
            } state;
            sf::PluginScriptRuntime::WorldQueries carWorld{&state,
                [](void* context, double x, double, double, double, double, double* out) noexcept {
                    auto& world = *static_cast<CarWorld*>(context);
                    ++world.floors;
                    if (world.failFloor) return false;
                    *out = world.height + (world.slope ? x * 0.1 : 0);
                    return true;
                },
                [](void* context, double* x, double*, double*, double, double) noexcept {
                    auto& world = *static_cast<CarWorld*>(context);
                    ++world.resolves;
                    if (world.fail) return false;
                    if (world.wall) *x += 0.1;
                    return true;
                },
                [](void* context) noexcept { return static_cast<CarWorld*>(context)->available; }};
            sf::PluginScriptRuntime::ViewQuery carView{nullptr, [](void*, double* out) noexcept {
                out[0] = 0; out[1] = 1.5; out[2] = 0;
                out[3] = 0; out[4] = 0; out[5] = -1; return true;
            }};
            bool carOk = consent.grant("com.example.car-track", exampleZip, error) &&
                session.start(exampleZip, consent, &carWorld, error, &carView) &&
                session.tick(0.1, error) && session.boxes().size() == 36 && session.models().size() == 1 &&
                std::string(session.hudText().text.data()) == "WASD: REMOTE CAR";
            check(carOk, "packaged car example creates oval, car and wheels");
            if (carOk) {
                const auto offer = session.controlOffer();
                check(offer.available && session.package()->control && session.setControlActive(true, error) &&
                      session.tick(0.05, error) && session.takeControlPose().pending,
                      "consented car offers control and proposes a chase view while active");
                const auto engineOn = session.takeEngineSound();
                check(engineOn.pending && engineOn.active && engineOn.speed >= 0 &&
                      session.setControlActive(false, error) && !session.takeControlPose().pending,
                      "scripted car starts engine sound and releases chase control");
                const auto engineOff = session.takeEngineSound();
                check(engineOff.pending && !engineOff.active,
                      "scripted car stops engine sound when control is released");
                const auto before = session.models();
                const auto beforeWheels = session.boxes();
                state.resolves = 0;
                carOk = session.input("move_y", 1, error) && session.input("move_x", 1, error);
                for (int i = 0; i < 20 && carOk; ++i) carOk = session.tick(0.05, error);
                const auto after = session.models();
                const auto afterWheels = session.boxes();
                check(carOk && after.size() == 1 && afterWheels.size() == 36 && state.resolves == 20 &&
                      std::hypot(after[0].transform.x - before[0].transform.x,
                                 after[0].transform.z - before[0].transform.z) > 0.5 &&
                      std::abs(after[0].transform.rotateY - before[0].transform.rotateY) > 0.1 &&
                      std::abs(afterWheels[32].rotateX - beforeWheels[32].rotateX) > 0.01,
                      "scripted car accelerates, steers and spins wheels using collision service");
                state.wall = true;
                check(session.tick(0.05, error) && state.resolves == 21,
                      "scripted car handles collision-adjusted movement");
                state.available = false;
                check(session.tick(0.05, error) && session.boxes().empty() &&
                      std::string(session.hudText().text.data()) == "WAITING FOR CITY",
                      "scripted track clears when city disappears");
                state.available = true;
                check(session.tick(0.1, error) && session.boxes().size() == 36 && session.models().size() == 1 &&
                      session.stop(error) && session.boxes().empty() && session.models().empty(),
                      "scripted track recreates and cleans up on disable");
            }
            check(session.start(exampleZip, consent, &carWorld, error, &carView) &&
                  session.tick(0.1, error) && session.stop(error) && session.boxes().empty() && session.models().empty(),
                  "scripted track reloads after disable");
            state.height = 0;
            bool refreshed = session.start(exampleZip, consent, &carWorld, error, &carView) &&
                             session.tick(0.1, error);
            state.height = 3;
            const int floorsBeforeRefresh = state.floors;
            for (int i = 0; i < 100 && refreshed; ++i) refreshed = session.tick(0.1, error);
            const auto refreshedTrack = session.boxes();
            bool trackRaised = refreshedTrack.size() == 36;
            for (int i = 0; i < 32 && trackRaised; ++i)
                trackRaised = std::abs(refreshedTrack[i].y - 3.03) < 0.001;
            check(refreshed && trackRaised && state.floors - floorsBeforeRefresh < 150 &&
                  session.stop(error), "track refreshes all terrain samples incrementally within query budget");
            state.height = 0;
            check(session.start(exampleZip, consent, &carWorld, error, &carView) && session.tick(0.1, error),
                  "start track for terrain refresh failure");
            state.failFloor = true;
            check(!session.tick(0.1, error) && !session.running() && session.boxes().empty() &&
                  session.models().empty(), "terrain refresh failure stops plugin and releases track");
            state.failFloor = false;
            state.wall = false;
            bool rail = session.start(exampleZip, consent, &carWorld, error, &carView) &&
                        session.tick(0.1, error) && session.input("move_y", 1, error);
            for (int i = 0; i < 20 && rail; ++i) rail = session.tick(0.05, error);
            check(rail && std::string(session.hudText().text.data()) == "ON RAIL: S TO EXIT",
                  "aligned accelerating car enters scripted rail");
            if (rail) {
                const auto before = session.models();
                rail = session.input("move_x", 1, error);
                for (int i = 0; i < 20 && rail; ++i) rail = session.tick(0.05, error);
                const auto after = session.models();
                check(rail && after.size() == 1 &&
                      std::hypot(after[0].transform.x - before[0].transform.x,
                                 after[0].transform.z - before[0].transform.z) > 1 &&
                      std::string(session.hudText().text.data()) == "ON RAIL: S TO EXIT",
                      "scripted car advances and steers along rail");
                rail = rail && session.input("move_x", 0, error);
                for (int i = 0; i < 600 && rail; ++i) rail = session.tick(0.05, error);
                const auto lapped = session.models();
                check(rail && lapped.size() == 1 &&
                      std::hypot(lapped[0].transform.x / 18, (lapped[0].transform.z + 25) / 12) < 1.2 &&
                      std::string(session.hudText().text.data()) == "ON RAIL: S TO EXIT",
                      "scripted car follows bounded looping rail for multiple laps");
                check(session.input("move_y", -1, error) && session.tick(0.05, error) &&
                      std::string(session.hudText().text.data()) == "WASD: REMOTE CAR",
                      "reverse input exits scripted rail");
            }
            check(session.stop(error), "stop scripted rail session");
            rail = session.start(exampleZip, consent, &carWorld, error, &carView) &&
                   session.tick(0.1, error) && session.input("move_y", 1, error);
            for (int i = 0; i < 20 && rail; ++i) rail = session.tick(0.05, error);
            state.wall = true;
            check(rail && std::string(session.hudText().text.data()) == "ON RAIL: S TO EXIT" &&
                  session.tick(0.05, error) &&
                  std::string(session.hudText().text.data()) == "WASD: REMOTE CAR" &&
                  session.stop(error), "collision exits scripted rail and session cleans up");
            state.fail = true;
            check(session.start(exampleZip, consent, &carWorld, error, &carView) &&
                  session.tick(0.1, error) && !session.tick(0.05, error) && !session.running() &&
                  session.boxes().empty() && session.models().empty(), "car world-service failure stops and clears scene");
            state.fail = false;
            state.slope = true;
            check(session.start(exampleZip, consent, &carWorld, error, &carView) && session.tick(0.1, error) &&
                  session.boxes().size() == 36 && std::abs(session.boxes()[0].rotateX) > 0.001 &&
                  session.models().size() == 1 && session.models()[0].transform.y > 2.3 &&
                  session.stop(error), "scripted oval samples terrain height and slope");
            state.slope = false;
            struct CameraState { double x = 0, y = 1.7, z = 0, yaw = 0, pitch = 0; } camera;
            sf::PluginScriptRuntime::ViewQuery chaseView{&camera, [](void* context, double* out) noexcept {
                const auto& c = *static_cast<CameraState*>(context);
                out[0] = c.x; out[1] = c.y; out[2] = c.z;
                out[3] = -std::sin(c.yaw) * std::cos(c.pitch);
                out[4] = std::sin(c.pitch);
                out[5] = -std::cos(c.yaw) * std::cos(c.pitch);
                return true;
            }};
            bool chase = session.start(exampleZip, consent, &carWorld, error, &chaseView) &&
                         session.tick(0.1, error);
            const auto offer = session.controlOffer();
            camera.x = offer.x; camera.y = offer.y; camera.z = offer.z;
            sf::GameplayControlLease lease;
            using Pose = sf::GameplayControlLease::Pose;
            chase = chase && offer.available &&
                    lease.acquire(1, true, true, true, {camera.x,camera.y,camera.z,camera.yaw,camera.pitch},
                                  {offer.x,offer.y,offer.z,0,0}) &&
                    session.setControlActive(true, error) && session.input("move_y", 1, error);
            int accepted = 0;
            for (int i = 0; i < 80 && chase; ++i) {
                chase = session.tick(0.05, error);
                const auto p = session.takeControlPose();
                const Pose current{camera.x, camera.y, camera.z, camera.yaw, camera.pitch};
                const Pose proposed{p.x, p.y, p.z, p.yaw, p.pitch};
                Pose resolved{};
                chase = chase && p.pending &&
                        sf::resolveGameplayControlMotion(lease, 1, true, current, proposed, 0.05, 10000,
                            [](Pose& candidate) {
                                candidate.y = std::max(candidate.y, 1.7);
                                return true;
                            }, resolved) == sf::GameplayControlMotionResult::Applied;
                if (chase) {
                    camera = {resolved.x, resolved.y, resolved.z, resolved.yaw, resolved.pitch};
                    ++accepted;
                }
            }
            check(chase && accepted == 80 && session.models().size() == 1 &&
                  std::hypot(camera.x - offer.x, camera.z - offer.z) > 1,
                  "scripted car chase proposals stay within lease bounds while driving");
            lease.release();
            check(session.setControlActive(false, error) && session.tick(0.05, error) &&
                  !session.takeControlPose().pending && session.stop(error),
                  "releasing control stops chase proposals and allows clean disable");
            check(consent.revoke("com.example.car-track", error), "revoke car example consent");
        }
        {
            const auto example = fs::path(SCIFI_TEST_ASSETS).parent_path() / "examples" / "plugins" / "snack-stand";
            const auto read = [](const fs::path& path) {
                std::ifstream in(path, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(in), {});
            };
            const auto exampleZip = archive({{"plugin.json", read(example / "plugin.json")},
                                            {"scripts/main.lua", read(example / "scripts" / "main.lua")},
                                            {"assets/duck.glb", read(fs::path(SCIFI_TEST_ASSETS) / "models" / "Duck.glb")}});
            struct SnackTest { double x = 0; bool available = true; bool wall = true; } state;
            sf::PluginScriptRuntime::WorldQueries snackWorld{&state,
                [](void*, double, double, double, double, double, double* out) noexcept { *out = 0; return true; },
                [](void* context, double*, double*, double* z, double, double) noexcept {
                    if (static_cast<SnackTest*>(context)->wall && *z < -4) *z = -4;
                    return true;
                },
                [](void* context) noexcept { return static_cast<SnackTest*>(context)->available; }};
            sf::PluginScriptRuntime::ViewQuery view{&state, [](void* context, double* out) noexcept {
                out[0] = static_cast<SnackTest*>(context)->x; out[1] = 1.5; out[2] = 0;
                out[3] = 0; out[4] = 0; out[5] = -1; return true;
            }};
            check(consent.grant("com.example.snack-stand", exampleZip, error) &&
                  session.start(exampleZip, consent, &snackWorld, error, &view) && session.tick(0.1, error) &&
                  session.boxes().size() == 2 && session.models().size() == 1 &&
                  session.input("click", 1, error) && session.boxes().size() == 3 &&
                  std::string(session.hudText().text.data()) == "CLICK TO THROW hotdog",
                  "scripted hotdog stand receives click and creates held snack");
            auto sounds = session.takeSounds();
            check(sounds.count == 1 && sounds.events[0].sound == sf::PluginScriptRuntime::Sound::Pickup,
                  "scripted pickup queues sound");
            check(session.input("click", 1, error) && session.tick(0.1, error) &&
                  session.boxes()[2].z < -2, "scripted snack is thrown and moves");
            sounds = session.takeSounds();
            check(sounds.count == 1 && sounds.events[0].sound == sf::PluginScriptRuntime::Sound::Throw,
                  "scripted throw queues sound");
            bool ticks = session.running(), boom = false, pop = false, sawExplosion = false, pushed = false;
            for (int i = 0; i < 51 && ticks; ++i) {
                ticks = session.tick(0.1, error);
                const auto push = session.takeImpulse();
                if (push.pending) pushed = push.y > 0 && std::hypot(push.x, push.z) <= 12;
                if (session.boxes().size() == 5) sawExplosion = true;
                sounds = session.takeSounds();
                for (size_t j = 0; j < sounds.count; ++j)
                    if (sounds.events[j].sound == sf::PluginScriptRuntime::Sound::Boom) boom = true;
                    else if (sounds.events[j].sound == sf::PluginScriptRuntime::Sound::DuckPop) pop = true;
            }
            check(ticks && boom && pop && sawExplosion && pushed && session.models().empty() &&
                  std::string(session.hudText().text.data()) == "SCORE 100",
                  "nearby snack explosion pushes player, pops duck and scores");
            for (int i = 0; i < 14 && ticks; ++i) ticks = session.tick(0.1, error);
            check(ticks && session.boxes().size() == 2, "explosion effects expire");
            for (int i = 0; i < 900 && ticks; ++i) ticks = session.tick(0.1, error);
            check(ticks && session.models().size() == 1, "duck respawns after cooldown");
            state.x = 4;
            check(session.input("click", 1, error) && session.boxes().size() == 3 &&
                  std::string(session.hudText().text.data()) == "CLICK TO THROW icecream",
                  "ice cream stand provides a different snack");
            session.takeSounds();
            state.available = false;
            check(session.tick(0.1, error) && session.boxes().empty() && session.models().empty() &&
                  std::string(session.hudText().text.data()) == "WAITING FOR CITY",
                  "city removal clears stands, held snack and duck");
            state.available = true;
            state.x = 0;
            check(session.stop(error) && session.start(exampleZip, consent, &snackWorld, error, &view) &&
                  session.tick(0.1, error) && session.input("click", 1, error) &&
                  session.stop(error) && session.boxes().empty() && session.models().empty(),
                  "snack pickup cleans up when disabled and restarted");
            check(session.start(exampleZip, consent, &snackWorld, error, &view) &&
                  session.tick(0.1, error) && session.input("click", 1, error) &&
                  session.input("click", 1, error), "restart snack example for explosion cleanup");
            session.takeSounds();
            state.x = 100;
            ticks = session.running();
            bool farPush = false;
            for (int i = 0; i < 51 && ticks; ++i) {
                ticks = session.tick(0.1, error);
                farPush = session.takeImpulse().pending || farPush;
                session.takeSounds();
            }
            check(ticks && !farPush && session.boxes().size() == 5 && session.stop(error) &&
                  session.boxes().empty() && session.models().empty() && session.hudText().text[0] == 0,
                  "distant explosion does not push player and disabling removes owned effects");
            check(consent.revoke("com.example.snack-stand", error), "revoke snack example consent");
        }
        check(!session.start(executableZip, consent, nullptr, error) && !session.running(),
              "requested world service is required");
        check(session.start(executableZip, consent, &world, error) && session.running() &&
              session.package() && session.package()->textures.size() == 1 && session.models().size() == 1,
              "consented session loads model and texture resources");
        check(!session.input("steer", 0.5, error) && session.running() &&
              session.models()[0].transform.rotateY == 0 &&
              error.find("input permission") != std::string::npos,
              "session refuses undeclared input without invoking callback");
        check(session.stop(error) && consent.grant("com.example.game", inputZip, error) &&
              session.start(inputZip, consent, &world, error), "start session with consented input permission");
        check(session.input("steer", 0.5, error) && session.tick(0.25, error) &&
              session.models()[0].transform.x == 0.25 && session.models()[0].transform.rotateY == 0.5,
              "consented session receives input and tick");
        check(session.stop(error) && !session.running() && !session.package() && session.models().empty(),
              "session stop releases assets and instances");
        check(session.start(inputZip, consent, &world, error) && session.stop(error),
              "consented session can restart");
        auto sceneOnlyManifest = gameplayManifest;
        const std::string bothPermissions = "\"scene\",\"world\"";
        sceneOnlyManifest.replace(sceneOnlyManifest.find(bothPermissions), bothPermissions.size(), "\"scene\"");
        const auto sceneOnlyZip = archive({{"plugin.json", sceneOnlyManifest},
            {"scripts/main.lua", "return {on_tick=function() world_floor(0,0,0,1,1) end}"}});
        check(consent.grant("com.example.game", sceneOnlyZip, error) &&
              session.start(sceneOnlyZip, consent, &world, error) && !session.tick(0.1, error) &&
              !session.running(), "undeclared world permission remains unavailable even with host service");
        const auto viewZip = archive({{"plugin.json", viewManifest},
            {"scripts/main.lua", "return {on_tick=function() view_pose() end}"}});
        sf::PluginScriptRuntime::ViewQuery view{nullptr, [](void*, double* out) noexcept {
            for (int i = 0; i < 6; ++i) out[i] = 0;
            out[5] = -1;
            return true;
        }};
        const auto controlZip = archive({{"plugin.json", controlManifest},
            {"scripts/main.lua", "return {on_start=function() control_offer(0,0,0) end,"
                                 "on_input=function(k,v) if k=='control' and v==1 then missing() end end}"}});
        check(!session.start(controlZip, consent, &world, error, &view) &&
              consent.grant("com.example.game", controlZip, error) &&
              session.start(controlZip, consent, &world, error, &view) && session.controlOffer().available &&
              !session.setControlActive(true, error) && !session.running() && !session.package() &&
              !session.controlOffer().available, "control requires matching consent and callback failure clears session");
        const auto noControlZip = archive({{"plugin.json", inputManifest},
            {"scripts/main.lua", "return {on_tick=function() control_offer(0,0,0) end}"}});
        check(consent.grant("com.example.game", noControlZip, error) &&
              session.start(noControlZip, consent, &world, error, &view) &&
              !session.tick(0.1, error) && !session.running(),
              "control functions are absent without declared permission");
        check(consent.grant("com.example.game", viewZip, error) &&
              !session.start(viewZip, consent, nullptr, error) &&
              session.start(viewZip, consent, nullptr, error, &view) && session.tick(0.1, error) &&
              session.stop(error), "view permission requires and receives host service");
        const auto undeclaredViewZip = archive({{"plugin.json", sceneOnlyManifest},
            {"scripts/main.lua", "return {on_tick=function() view_pose() end}"}});
        check(consent.grant("com.example.game", undeclaredViewZip, error) &&
              session.start(undeclaredViewZip, consent, nullptr, error, &view) &&
              !session.tick(0.1, error) && !session.running(),
              "view service is absent without declared permission");
        const auto soundZip = archive({{"plugin.json", soundManifest},
            {"scripts/main.lua", "return {on_tick=function() sound_play('throw',0.5) end}"}});
        check(consent.grant("com.example.game", soundZip, error) &&
              session.start(soundZip, consent, nullptr, error) && session.tick(0.1, error) &&
              session.takeSounds().count == 1 && session.stop(error),
              "sound permission enables queued sound events");
        const auto undeclaredSoundZip = archive({{"plugin.json", sceneOnlyManifest},
            {"scripts/main.lua", "return {on_tick=function() sound_play('throw',0.5) end}"}});
        check(consent.grant("com.example.game", undeclaredSoundZip, error) &&
              session.start(undeclaredSoundZip, consent, nullptr, error) && !session.tick(0.1, error) &&
              session.takeSounds().count == 0 && !session.running(),
              "sound is unavailable without declared permission");
        const auto hudZip = archive({{"plugin.json", hudManifest},
            {"scripts/main.lua", "return {on_tick=function() hud_set('SCORE 42') end}"}});
        check(consent.grant("com.example.game", hudZip, error) &&
              session.start(hudZip, consent, nullptr, error) && session.tick(0.1, error) &&
              std::string(session.hudText().text.data()) == "SCORE 42" && session.stop(error) &&
              session.hudText().text[0] == 0, "HUD permission enables owned text and cleans it up");
        const auto undeclaredHudZip = archive({{"plugin.json", sceneOnlyManifest},
            {"scripts/main.lua", "return {on_tick=function() hud_set('SCORE 42') end}"}});
        check(consent.grant("com.example.game", undeclaredHudZip, error) &&
              session.start(undeclaredHudZip, consent, nullptr, error) && !session.tick(0.1, error) &&
              !session.running(), "HUD is unavailable without declared permission");
        const auto failureZip = archive({{"plugin.json", gameplayManifest},
            {"scripts/main.lua", "scene_model(1,1,0,0,0,1,1,1,1,1,1) "
                                 "return {on_tick=function() missing() end}"},
            {"assets/car.glb", modelData}, {"assets/car.png", validPng}});
        check(consent.grant("com.example.game", failureZip, error) &&
              session.start(failureZip, consent, &world, error) &&
              !session.tick(0.1, error) && !session.running() && !session.package() && session.models().empty(),
              "failed gameplay callback releases session resources");
        {
            std::string current = failureZip;
            sf::PluginManager manager(dir / "gameplay-preferences.ini");
            sf::GameplayControlLease lease;
            sf::GameplayInput inputState;
            int stops = 0;
            sf::Plugin plugin{"com.example.game", "Game", "", "1", [] {}, [&] {
                ++stops;
                lease.releaseOwner(1);
                std::string stopError;
                check(session.stop(stopError), "manager session stop");
            }, false};
            plugin.prepare = [&](std::string& startError) { return session.start(current, consent, &world, startError); };
            plugin.cancelPrepare = [&] { std::string ignored; session.stop(ignored); };
            check(manager.add(std::move(plugin)) && manager.initialize() &&
                  manager.setEnabled("com.example.game", true) && session.running(), "manager enables consented session");
            using Pose = sf::GameplayControlLease::Pose;
            const Pose position{0, 1.7, 0, 0, 0};
            check(lease.acquire(1, true, true, true, position, position), "failure test control acquired");
            check(!sf::runGameplayFrame(session, inputState, true, true, 0, 0, 0, false, false,
                                        0.1, error, [&](const std::string& reason) {
                                            manager.markFailed("com.example.game", reason);
                                        }) &&
                  !manager.plugins()[0].running && !session.package() && !lease.owner() && stops == 1 &&
                  manager.error().find("missing") != std::string::npos,
                  "host frame reports tick failure and releases stopped session's resources and control");
            auto inputManifest = gameplayManifest;
            inputManifest.replace(inputManifest.find(bothPermissions), bothPermissions.size(),
                                  "\"scene\",\"world\",\"input\"");
            current = archive({{"plugin.json", inputManifest},
                               {"scripts/main.lua", "return {on_input=function() missing() end}"}});
            check(consent.grant("com.example.game", current, error) &&
                  manager.setEnabled("com.example.game", true) &&
                  lease.acquire(1, true, true, true, position, position),
                  "restart for input failure");
            check(!sf::runGameplayFrame(session, inputState, true, true, 1, 0, 0, false, false,
                                        0.1, error, [&](const std::string& reason) {
                                            manager.markFailed("com.example.game", reason);
                                        }) &&
                  !manager.plugins()[0].running && !session.package() && !lease.owner() && stops == 2 &&
                  manager.error().find("missing") != std::string::npos,
                  "host frame reports input failure and releases stopped session's resources and control");
            current = executableZip;
            check(!manager.setEnabled("com.example.game", true) &&
                  manager.error().find("consent") != std::string::npos, "manager retry requires new consent");
            check(consent.grant("com.example.game", executableZip, error) &&
                  manager.setEnabled("com.example.game", true) && session.running() &&
                  manager.setEnabled("com.example.game", false) && !session.package(),
                  "manager retry and disable release resources");
            manager.shutdown();
            fs::remove(dir / "gameplay-preferences.ini");
        }
        check(consent.grant("com.example.game", executableZip, error), "restore explicit consent for original package");
        {
            const auto blocked = dir / "blocked-preferences";
            { std::ofstream out(blocked); out << "preserve"; }
            sf::PluginManager manager(blocked / "state");
            sf::Plugin plugin{"com.example.game", "Game", "", "1", [] {}, [] {}, false};
            plugin.prepare = [&](std::string& startError) { return session.start(executableZip, consent, &world, startError); };
            plugin.cancelPrepare = [&] { std::string ignored; session.stop(ignored); };
            check(manager.add(std::move(plugin)) && manager.initialize(), "rollback manager setup");
            check(!manager.setEnabled("com.example.game", true) && !session.running() && !session.package() &&
                  !manager.plugins()[0].enabled, "preference failure rolls back prepared gameplay session");
            manager.shutdown();
            fs::remove(blocked);
        }
        check(consent.matches("com.example.game", fullGameZip, granted, error) && !granted,
              "changed content requires new consent");
        check(!session.start(fullGameZip, consent, &world, error) &&
              error.find("consent") != std::string::npos && !session.package(),
              "changed package cannot start without consent");
        auto sameSize = executableZip;
        sameSize[sameSize.size() / 2] ^= 1;
        check(consent.matches("com.example.game", sameSize, granted, error) && !granted,
              "same-size content change requires new consent");
        check(!consent.grant("../escape", executableZip, error), "consent ID traversal rejected");
        sf::GameplayConsent restartedConsent(dir / "consents");
        check(restartedConsent.matches("com.example.game", executableZip, granted, error) && granted,
              "consent persists across restart");
        check(restartedConsent.revoke("com.example.game", error) &&
              restartedConsent.matches("com.example.game", executableZip, granted, error) && !granted,
              "revocation removes consent");
        check(!session.start(executableZip, restartedConsent, &world, error) && !session.running(),
              "revocation prevents restart");
        check(restartedConsent.revoke("com.example.game", error), "consent revocation is idempotent");
        fs::remove(dir / "consents");
    }
    {
        // Sample plugin: settings, hotkeys, link hosts and the custom room model all validate from the shipped fixture.
        const fs::path sample = fs::path(SCIFI_TEST_ASSETS) / ".." / "tests" / "fixtures" / "network-hud-demo";
        const auto slurp = [](const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };
        const auto sampleManifest = slurp(sample / "plugin.json"), sampleScript = slurp(sample / "scripts" / "main.lua"),
                   roomModel = slurp(sample / "assets" / "room.glb");
        sf::GameplayPackage demo;
        check(sf::readGameplayPackage(archive({{"plugin.json", sampleManifest}, {"scripts/main.lua", sampleScript},
                                               {"assets/room.glb", roomModel}}), demo, error) &&
                  demo.roomModel == "assets/room.glb" && demo.settings.size() == 2 &&
                  demo.settings[0].key == "location" && demo.settings[1].choices.size() == 3 &&
                  demo.hotkeys.size() == 1 && demo.hotkeys[0].first == 'n' && demo.hotkeys[0].second == "open_story" &&
                  demo.linkHosts.size() == 4 && demo.networkHosts.size() == 4,
              "sample plugin manifest with settings, hotkeys, link hosts and room model");
        sf::GameplayModel room;
        check(sf::decodeGameplayModel(roomModel, room, error) && room.vertices.size() > 1000,
              "sample room GLB decodes within model limits");
        const auto withField = [&](const std::string& field) {
            auto text = gameplayManifest;
            text.insert(text.size() - 1, "," + field);
            sf::GameplayPackage parsed;
            std::string parseError;
            return sf::PluginPackageStore::parseGameplayManifest(text, parsed, parseError);
        };
        check(withField(R"("settings":["mode|MODE|a=Alpha,b=Beta"])"), "minimal setting accepted");
        for (const char* bad : {R"("settings":["mode|MODE|a=Alpha"])", R"("settings":["Mode|MODE|a=A,b=B"])",
                                R"("settings":["mode|MODE|a=A,a=B"])", R"("settings":["m|M|a=A,b=B","m|M|a=A,b=B"])",
                                R"("hotkeys":["w=go"])", R"("hotkeys":["n=go","n=stop"])", R"("hotkeys":["n=Go Now"])",
                                R"("link_hosts":["localhost"])", R"("room_model":"assets/missing.txt")"})
            check(!withField(bad), (std::string("invalid host service manifest field rejected: ") + bad).c_str());
        check(!sf::readGameplayPackage(archive({{"plugin.json", gameplayManifest.substr(0, gameplayManifest.size() - 1) +
                                                     R"(,"room_model":"assets/room.glb"})"},
                                                {"scripts/main.lua", "return {}"}}), demo, error),
              "room model must exist in the archive");

        // Editable workspace round trip and the safe apply (replace) flow.
        const fs::path work = dir / "workspace-test", zipPath = work / "com.example.game.zip", ws = work / "edit" / "com.example.game";
        fs::create_directories(work);
        const auto writeFile = [](const fs::path& path, const std::string& bytes) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        };
        const auto roomManifest = gameplayManifest.substr(0, gameplayManifest.size() - 1) + R"(,"room_model":"assets/room.glb"})";
        writeFile(zipPath, archive({{"plugin.json", roomManifest}, {"scripts/main.lua", "return {}"}, {"assets/room.glb", roomModel}}));
        bool created = false;
        check(sf::extractGameplayWorkspace(zipPath, ws, created, error) && created &&
                  slurp(ws / "scripts" / "main.lua") == "return {}" && slurp(ws / "assets" / "room.glb") == roomModel,
              "workspace extraction copies validated package files");
        writeFile(ws / "scripts" / "main.lua", "return {on_tick=function() end}");
        check(sf::extractGameplayWorkspace(zipPath, ws, created, error) && !created &&
                  slurp(ws / "scripts" / "main.lua") == "return {on_tick=function() end}",
              "re-browsing preserves workspace edits");
        const fs::path packed = work / "packed.zip";
        check(sf::packGameplayWorkspace(ws, packed, error) && sf::readGameplayPackageFile(packed, demo, error) &&
                  demo.script == "return {on_tick=function() end}" && demo.roomModel == "assets/room.glb",
              "workspace packs into a valid gameplay package");
        check(!sf::packGameplayWorkspace(ws, packed, error), "packing never overwrites an existing ZIP");
        writeFile(ws / "notes.txt", "hello");
        check(!sf::packGameplayWorkspace(ws, work / "extra.zip", error) && error.find("notes.txt") != std::string::npos &&
                  !fs::exists(work / "extra.zip"),
              "unexpected workspace file rejected");
        fs::remove(ws / "notes.txt");
        {
            const fs::path settingsFile = work / "plugin-settings" / "com.example.game.txt";
            check(sf::savePluginSettings(settingsFile, {{"location", "london"}, {"news", "bbc"}}, error),
                  "first plugin settings save");
            check(sf::savePluginSettings(settingsFile, {{"location", "new_york"}, {"news", "bbc"}}, error) &&
                      sf::savePluginSettings(settingsFile, {{"location", "new_york"}, {"news", "npr"}}, error),
                  "repeated plugin settings saves replace the existing file");
            const auto reloaded = sf::loadPluginSettings(settingsFile);
            check(reloaded.size() == 2 && reloaded.at("location") == "new_york" && reloaded.at("news") == "npr" &&
                      !fs::exists(fs::path(settingsFile).concat(".tmp")),
                  "latest plugin settings survive a reload without temp leftovers");
            std::vector<std::pair<std::string, std::string>> tooMany(9, {"k", "v"});
            check(!sf::savePluginSettings(settingsFile, tooMany, error) &&
                      sf::loadPluginSettings(settingsFile).at("news") == "npr",
                  "rejected settings save keeps the previous file");
        }
#ifdef _WIN32
        fs::create_directories(work / "outside");
        writeFile(work / "outside" / "secret.png", "x");
        const std::string junction = "cmd /c mklink /J \"" + (ws / "assets" / "linked").string() + "\" \"" +
                                     (work / "outside").string() + "\" >nul";
        if (std::system(junction.c_str()) == 0) {
            check(!sf::packGameplayWorkspace(ws, work / "junction.zip", error) && error.find("junction") != std::string::npos,
                  "workspace junctions are never followed");
            fs::remove(ws / "assets" / "linked");
        } else {
            check(false, "create workspace junction fixture");
        }
        check(fs::exists(work / "outside" / "secret.png"), "junction rejection leaves the target untouched");
#endif

        sf::GameplayPackage registeredPackage;
        int failRegistrations = 0;
        int registrations = 0;
        const fs::path storeDir = work / "store";
        sf::PluginPackageStore gameStore(storeDir, [](const sf::ThemePackage&) { return true; }, {},
            [&](const sf::GameplayPackage& pkg, const fs::path&, const sf::GameplayDigest&) {
                if (failRegistrations > 0) { --failRegistrations; return false; }
                ++registrations;
                registeredPackage = pkg;
                return true;
            });
        gameStore.discover();
        check(gameStore.install(zipPath) && registeredPackage.script == "return {}", "install gameplay ZIP for replace");
        int unregistered = 0;
        check(gameStore.replaceGameplay("com.example.game", packed, [&] { ++unregistered; }) && unregistered == 1 &&
                  registeredPackage.script == "return {on_tick=function() end}" &&
                  !fs::exists(storeDir / "com.example.game.zip.bak"),
              "apply edits replaces the installed ZIP and re-registers it");
        auto otherManifest = roomManifest;
        otherManifest.replace(otherManifest.find("com.example.game"), 16, "com.example.gamf");
        writeFile(work / "other.zip", archive({{"plugin.json", otherManifest}, {"scripts/main.lua", "return {}"},
                                               {"assets/room.glb", roomModel}}));
        unregistered = 0;
        check(!gameStore.replaceGameplay("com.example.game", work / "other.zip", [&] { ++unregistered; }) &&
                  unregistered == 0 && gameStore.error().find("id") != std::string::npos,
              "edited package cannot change its id");
        writeFile(work / "broken.zip", "not a zip");
        check(!gameStore.replaceGameplay("com.example.game", work / "broken.zip", [&] { ++unregistered; }) && unregistered == 0,
              "invalid edited package rejected before touching the installed copy");
        failRegistrations = 1;
        const int before = registrations;
        check(!gameStore.replaceGameplay("com.example.game", zipPath, [&] { ++unregistered; }) &&
                  gameStore.error().find("previous package restored") != std::string::npos &&
                  registrations == before + 1 && registeredPackage.script == "return {on_tick=function() end}" &&
                  !fs::exists(storeDir / "com.example.game.zip.bak"),
              "failed registration rolls back to the previous package and re-registers it");
        sf::GameplayPackage onDisk;
        check(sf::readGameplayPackageFile(storeDir / "com.example.game.zip", onDisk, error) &&
                  onDisk.script == "return {on_tick=function() end}",
              "failed replace restores the previous installed ZIP");
        std::error_code cleanup;
        fs::remove_all(work, cleanup);
    }
    { std::ofstream out(source); out << good; }
    std::set<std::string> ids;
    sf::PluginPackageStore store(installed, [&](const sf::ThemePackage& pkg) { return ids.insert(pkg.id).second; });
    store.discover();
    check(store.error().empty(), "missing directory discovery");
    check(store.install(source), "install");
    check(ids.count("com.example.twilight") == 1, "register installed plugin");
    check(!store.install(source), "duplicate install rejected");
    const fs::path scriptSource = dir / "source.zip";
    { std::ofstream out(scriptSource, std::ios::binary); out.write(zip.data(), zip.size()); }
    sf::PluginPackageStore scriptStore(installed, [&](const sf::ThemePackage& pkg) { return ids.insert(pkg.id).second; },
        [&](const sf::ScriptPackage& pkg) { return ids.insert(pkg.id).second; });
    check(scriptStore.install(scriptSource), "install ZIP");
    check(!scriptStore.install(scriptSource), "duplicate ZIP rejected");
    check(scriptStore.uninstall("com.example.clock"), "uninstall ZIP");
    ids.erase("com.example.clock");
    check(!scriptStore.uninstall("com.example.clock"), "uninstall missing ZIP rejected");
    check(scriptStore.install(scriptSource), "reinstall ZIP");
    ids.clear();
    scriptStore.discover();
    check(scriptStore.error().empty() && ids.count("com.example.clock"), "ZIP discovered after restart");
    check(scriptStore.uninstall("com.example.clock"), "remove ZIP after discovery");
    ids.clear();
    sf::PluginPackageStore restarted(installed, [&](const sf::ThemePackage& pkg) { return ids.insert(pkg.id).second; });
    restarted.discover();
    check(restarted.error().empty() && ids.count("com.example.twilight"), "restart discovery");
    { std::ofstream out(installed / "wrong.sfplugin"); out << good; }
    ids.clear();
    restarted.discover();
    check(!restarted.error().empty() && ids.count("com.example.twilight"), "bad filename reported without hiding good package");
    fs::remove(installed / "wrong.sfplugin");
    fs::remove(installed / "com.example.twilight.sfplugin");
    fs::remove(installed);
    fs::remove(source);
    fs::remove(scriptSource);
    fs::remove(dir);
    return failures ? 1 : 0;
}
