#pragma once

#include "GameplayPackage.h"
#include "GameplayDigest.h"
#include "PluginScript.h"

#include <memory>
#include <string>
#include <cstdint>
#include <filesystem>

namespace sf {
class GameplayConsent;
class GameplayStorage;

// Internal host-side session for an installed gameplay package.
class GameplaySession {
public:
    GameplaySession();
    ~GameplaySession();
    bool start(const std::string& bytes, const GameplayConsent& consent,
               const PluginScriptRuntime::WorldQueries* world, std::string& error,
               const PluginScriptRuntime::ViewQuery* view = nullptr,
               const PluginScriptRuntime::LayoutQuery* layout = nullptr,
               const std::filesystem::path& storageDirectory = {}, std::uint64_t initialFrame = 0);
    void prepareFrame(std::uint64_t frame) { runtime_.prepareFrame(frame); }
    bool startFile(const std::filesystem::path& path, const GameplayDigest& expected, const GameplayConsent& consent,
                   const PluginScriptRuntime::WorldQueries* world, std::string& error,
                   const PluginScriptRuntime::ViewQuery* view = nullptr,
                   const PluginScriptRuntime::LayoutQuery* layout = nullptr,
                   const std::filesystem::path& storageDirectory = {}, std::uint64_t initialFrame = 0);
    bool tick(double seconds, std::string& error);
    bool input(const std::string& control, double value, std::string& error);
    bool message(std::string_view sender, const PluginScriptRuntime::MessageEvent& event, std::string& error);
    bool stop(std::string& error);
    bool running() const;
    std::uint64_t generation() const { return generation_; }
    const GameplayPackage* package() const { return package_.get(); }
    std::vector<PluginScriptRuntime::Box> boxes() const { return runtime_.boxes(); }
    std::vector<PluginScriptRuntime::Model> models() const { return runtime_.models(); }
    const GameplayModel* proceduralModel(int id) const { return runtime_.proceduralModel(id); }
    PluginScriptRuntime::SoundBatch takeSounds() { return runtime_.takeSounds(); }
    PluginScriptRuntime::SampleBatch takeSamples() { return runtime_.takeSamples(); }
    PluginScriptRuntime::MessageBatch takeMessages() { return runtime_.takeMessages(); }
    PluginScriptRuntime::EngineSound takeEngineSound() { return runtime_.takeEngineSound(); }
    PluginScriptRuntime::HudText hudText() const { return runtime_.hudText(); }
    PluginScriptRuntime::Impulse takeImpulse() { return runtime_.takeImpulse(); }
    PluginScriptRuntime::ControlOffer controlOffer() const { return runtime_.controlOffer(); }
    PluginScriptRuntime::ControlPose takeControlPose() { return runtime_.takeControlPose(); }
    bool setControlActive(bool active, std::string& error);
    bool interactiveHit(const double* origin, const double* direction, double& distance) const {
        return runtime_.interactiveHit(origin, direction, distance);
    }
    bool capturesClicks() const { return runtime_.capturesClicks(); }

private:
    bool startParsed(GameplayPackage&& parsed, const PluginScriptRuntime::WorldQueries* world,
                     std::string& error, const PluginScriptRuntime::ViewQuery* view,
                     const PluginScriptRuntime::LayoutQuery* layout, const std::filesystem::path& storageDirectory,
                     std::uint64_t initialFrame = 0);
    PluginScriptRuntime runtime_;
    std::unique_ptr<GameplayPackage> package_;
    std::unique_ptr<GameplayStorage> storage_;
    std::uint64_t generation_ = 0;
};
}  // namespace sf
