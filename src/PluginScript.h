#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <array>
#include <cstdint>

#include "GameplayModel.h"

namespace sf {

class GameplayStorage;
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
        // -1 unavailable/over-budget, 0 miss, 1 hit distance. Uses city collision hulls.
        int (*raycast)(void*, const double*, const double*, double, double*) noexcept = nullptr;
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
    struct SampleEvent {
        int voice = 0, asset = 0;
        float volume = 0;
        bool stop = false, loop = false, loopUpdate = false;
    };
    struct SampleBatch { std::array<SampleEvent, 8> events{}; size_t count = 0; };
    struct MessageEvent {
        enum class Type { String, Number, Boolean };
        std::array<char, 32> topic{};
        std::array<char, 256> payload{};
        size_t topicSize = 0, payloadSize = 0;
        Type type = Type::String;
        double number = 0;
        bool boolean = false;
    };
    struct MessageBatch { std::array<MessageEvent, 8> events{}; size_t count = 0; };
    struct EngineSound { bool pending = false, active = false, boost = false; float speed = 0; };
    struct HudText { std::array<char, 65> text{}; std::uint64_t revision = 0; };
    // Plugin-owned 2D HUD panels. Placement is normalized to the visible view: the panel's anchor point
    // (0, 0.5 or 1 on each axis) sits at that screen anchor plus (x, y); w/h are fractions of the view.
    struct HudCommand {
        enum class Kind { Rect, Frame, Line, Circle, Text };
        Kind kind = Kind::Rect;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0, size = 0;
        std::array<std::uint8_t, 4> color{};
        int align = 0;  // text: 0 left, 1 centre, 2 right
        std::string text;
    };
    struct HudPanel {
        int id = 0;
        int anchor = 0;  // 0..8: row-major top_left .. bottom_right
        double x = 0, y = 0, width = 0, height = 0;
        int canvasWidth = 0, canvasHeight = 0;
        bool visible = true;
        std::array<std::uint8_t, 4> background{};
        std::vector<HudCommand> commands;
        std::uint64_t revision = 0;
        // Marquee: when scrollSpeed > 0 the panel shows viewWidth canvas units, looping horizontally.
        float scrollSpeed = 0;
        int viewWidth = 0;
    };
    // Read-only city map data. Coordinates are world metres on the ground plane (x east, z south).
    struct MapView { double x = 0, z = 0, heading = 0, minX = 0, minZ = 0, maxX = 0, maxZ = 0; };
    struct MapPlace {
        std::array<char, 32> id{}, name{}, kind{};
        double x = 0, z = 0, width = 0, depth = 0;  // centre; width/depth are 0 for points of interest
    };
    struct MapQuery {
        void* context = nullptr;
        // False with a short reason when no city map applies (interior, city unavailable).
        bool (*view)(void*, MapView*, char* reason, size_t reasonSize) noexcept = nullptr;
        size_t (*places)(void*, bool regions, MapPlace* out, size_t max) noexcept = nullptr;
    };
    // Optional host services that are not tied to a single manifest permission flag.
    struct HostServices {
        const MapQuery* map = nullptr;                                               // "map" permission
        const std::vector<std::pair<std::string, std::string>>* settings = nullptr;  // declared key, current value
        const std::vector<std::string>* actions = nullptr;                           // hotkey action names
        const std::vector<std::string>* linkHosts = nullptr;                         // open_link destinations
        void* context = nullptr;
        float (*measureText)(void*, const char* text, size_t size, float px) noexcept = nullptr;  // hud only
        bool (*hudView)(void*, int* width, int* height) noexcept = nullptr;                     // hud only
    };
    struct NetworkRequest { std::uint64_t id = 0; std::string url; };
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
        double opacity = 1, emission = 0;
        bool material = false;
    };
    struct Model {
        Box transform;
        int model = 0, texture = 0;
    };
    struct AssetCounts {
        int models = 0, textures = 0;
        std::vector<std::string> modelNames, textureNames, audioNames;
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
               bool inputGranted = false, GameplayStorage* storage = nullptr, bool messagingGranted = false,
               std::uint64_t initialFrame = 0, const std::vector<std::string>* networkHosts = nullptr,
               const HostServices* services = nullptr);
    void prepareFrame(std::uint64_t frame);
    bool tick(double seconds, std::string& error);
    bool input(const std::string& control, double value, std::string& error);
    bool message(std::string_view sender, const MessageEvent& event, std::string& error);
    // The user changed a declared setting on the plugin's room console; dispatches on_setting(key, value).
    bool setting(const std::string& key, const std::string& value, std::string& error);
    // A declared hotkey was freshly pressed; dispatches on_action(name). open_link is only valid inside it.
    bool action(const std::string& name, std::string& error);
    std::string takeLink();  // validated https URL requested by open_link during the last action, or empty
    bool stop(std::string& error);
    bool running() const;
    std::vector<Box> boxes() const;
    std::vector<Model> models() const;
    const GameplayModel* proceduralModel(int id) const;
    SoundBatch takeSounds();
    SampleBatch takeSamples();
    MessageBatch takeMessages();
    EngineSound takeEngineSound();
    HudText hudText() const;
    // Revision changes whenever any panel is created, moved, redrawn or removed.
    std::uint64_t hudPanelsRevision() const;
    std::vector<HudPanel> hudPanels() const;
    // Requests queued by net_get since the last call. The host must answer each id exactly once with
    // networkResult (or drop it when the runtime stops); unknown or cancelled ids are ignored.
    std::vector<NetworkRequest> takeNetworkRequests();
    std::vector<std::uint64_t> takeNetworkCancels();  // ids already taken by takeNetworkRequests, then net_cancel'd
    bool networkResult(std::uint64_t id, bool ok, int status, const std::string& text, std::string& error);
    Impulse takeImpulse();
    ControlOffer controlOffer() const;
    ControlPose takeControlPose();
    double controlZoom() const;
    bool controlExitY() const;
    bool setControlActive(bool active, std::string& error);
    bool interactiveHit(const double* origin, const double* direction, double& distance) const;
    bool capturesClicks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
