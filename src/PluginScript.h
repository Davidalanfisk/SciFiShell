#pragma once

#include <memory>
#include <string>
#include <vector>
#include <array>
#include <cstdint>

namespace sf {

// Each invocation gets a fresh Lua state with no standard libraries or ambient capabilities.
bool runPluginScript(const std::string& source, std::string& result, std::string& error);

// Internal lifecycle runtime for restricted gameplay plugins.
class PluginScriptRuntime {
public:
    struct WorldQueries {
        void* context = nullptr;
        bool (*floor)(void*, double, double, double, double, double, double*) noexcept = nullptr;
        bool (*resolve)(void*, double*, double*, double*, double, double) noexcept = nullptr;
        bool (*available)(void*) noexcept = nullptr;
        bool (*trackClearance)(void*, double, double, double, double*) noexcept = nullptr;
    };
    struct ViewQuery {
        void* context = nullptr;
        bool (*pose)(void*, double*) noexcept = nullptr;
    };
    struct LayoutQuery {
        void* context = nullptr;
        // -1: unavailable/error, 0: no cell, 1: cell; result is x, y, z, pitch and occupancy.
        int (*cell)(void*, double, double, double*) noexcept = nullptr;
    };
    enum class Sound { Pickup, Throw, Boom, DuckPop, CarSummon, RailOn };
    struct SoundEvent { Sound sound; float volume; };
    struct SoundBatch { std::array<SoundEvent, 8> events{}; size_t count = 0; };
    struct EngineSound { bool pending = false, active = false, boost = false; float speed = 0; };
    struct HudText { std::array<char, 33> text{}; std::uint64_t revision = 0; };
    struct Impulse { double x = 0, y = 0, z = 0; bool pending = false; };
    struct ControlOffer { double x = 0, y = 0, z = 0; bool available = false; };
    struct ControlPose { double x = 0, y = 0, z = 0, yaw = 0, pitch = 0; bool pending = false; };
    struct Box {
        int id = 0;
        double x = 0, y = 0, z = 0;
        double width = 0, height = 0, depth = 0;
        double red = 0, green = 0, blue = 0;
        double rotateX = 0, rotateY = 0, rotateZ = 0;
        bool interactive = false;
    };
    struct Model {
        Box transform;
        int model = 0, texture = 0;
    };
    struct AssetCounts {
        int models = 0, textures = 0;
        std::vector<std::string> modelNames, textureNames;
    };
    PluginScriptRuntime();
    ~PluginScriptRuntime();
    PluginScriptRuntime(const PluginScriptRuntime&) = delete;
    PluginScriptRuntime& operator=(const PluginScriptRuntime&) = delete;

    // The host keeps service contexts alive until the runtime stops.
    bool start(const std::string& source, std::string& error, bool sceneGranted = false,
               const WorldQueries* world = nullptr, const AssetCounts* assets = nullptr,
               const ViewQuery* view = nullptr, bool soundGranted = false, bool hudGranted = false,
               const LayoutQuery* layout = nullptr, bool impulseGranted = false, bool controlGranted = false,
               bool inputGranted = false);
    bool tick(double seconds, std::string& error);
    bool input(const std::string& control, double value, std::string& error);
    bool stop(std::string& error);
    bool running() const;
    std::vector<Box> boxes() const;
    std::vector<Model> models() const;
    SoundBatch takeSounds();
    EngineSound takeEngineSound();
    HudText hudText() const;
    Impulse takeImpulse();
    ControlOffer controlOffer() const;
    ControlPose takeControlPose();
    bool setControlActive(bool active, std::string& error);
    bool interactiveHit(const double* origin, const double* direction, double& distance) const;
    bool capturesClicks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
