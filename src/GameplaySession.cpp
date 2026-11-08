#include "GameplaySession.h"

#include "GameplayConsent.h"
#include "GameplayFileLock.h"
#include "GameplayStorage.h"

#include <new>
#include <limits>

namespace sf {
GameplaySession::GameplaySession() = default;
GameplaySession::~GameplaySession() = default;

bool GameplaySession::start(const std::string& bytes, const GameplayConsent& consent,
                            const PluginScriptRuntime::WorldQueries* world, std::string& error,
                            const PluginScriptRuntime::ViewQuery* view,
                            const PluginScriptRuntime::LayoutQuery* layout,
                            const std::filesystem::path& storageDirectory, std::uint64_t initialFrame) {
    if (runtime_.running()) { error = "Gameplay session is already running"; return false; }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        error = "Gameplay session generation limit reached";
        return false;
    }
    GameplayPackage parsed;
    if (!readGameplayPackage(bytes, parsed, error)) return false;
    bool granted = false;
    if (!consent.matches(parsed.id, bytes, granted, error)) return false;
    if (!granted) { error = "Gameplay package requires explicit consent"; return false; }
    return startParsed(std::move(parsed), world, error, view, layout, storageDirectory, initialFrame);
}

bool GameplaySession::startFile(const std::filesystem::path& path, const GameplayDigest& expected,
                                const GameplayConsent& consent, const PluginScriptRuntime::WorldQueries* world,
                                std::string& error, const PluginScriptRuntime::ViewQuery* view,
                                const PluginScriptRuntime::LayoutQuery* layout,
                                const std::filesystem::path& storageDirectory, std::uint64_t initialFrame) {
    if (runtime_.running()) { error = "Gameplay session is already running"; return false; }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        error = "Gameplay session generation limit reached"; return false;
    }
    GameplayFileLock lock;
    if (!lock.open(path, error)) return false;
    GameplayDigest before, after;
    GameplayPackage parsed;
    bool granted = false;
    if (!gameplayDigestFile(path, before, error) || !(before == expected)) {
        if (error.empty()) error = "Gameplay package changed; reinstall it before enabling";
        return false;
    }
    if (!readGameplayPackageFile(path, parsed, error) || !gameplayDigestFile(path, after, error) || !(before == after)) {
        if (error.empty()) error = "Gameplay package changed while loading";
        return false;
    }
    if (!consent.matchesDigest(parsed.id, before, granted, error)) return false;
    if (!granted) { error = "Gameplay package requires explicit consent"; return false; }
    return startParsed(std::move(parsed), world, error, view, layout, storageDirectory, initialFrame);
}

bool GameplaySession::startParsed(GameplayPackage&& parsed, const PluginScriptRuntime::WorldQueries* world,
                                  std::string& error, const PluginScriptRuntime::ViewQuery* view,
                                  const PluginScriptRuntime::LayoutQuery* layout,
                                  const std::filesystem::path& storageDirectory, std::uint64_t initialFrame) {
    if (parsed.world && !world) { error = "Gameplay world service is unavailable"; return false; }
    if (parsed.view && !view) { error = "Gameplay view service is unavailable"; return false; }
    if (parsed.layout && !layout) { error = "Gameplay layout service is unavailable"; return false; }
    try {
        if (parsed.storage) {
            storage_ = std::make_unique<GameplayStorage>(storageDirectory, parsed.id);
            if (!storage_->load(error)) { storage_.reset(); return false; }
        }
        package_ = std::make_unique<GameplayPackage>(std::move(parsed));
    } catch (const std::bad_alloc&) {
        storage_.reset();
        error = "Cannot allocate gameplay session";
        return false;
    }
    PluginScriptRuntime::AssetCounts assets{static_cast<int>(package_->models.size()),
                                            static_cast<int>(package_->textures.size())};
    try {
        for (const auto& entry : package_->models) assets.modelNames.push_back(entry.first);
        for (const auto& entry : package_->textures) assets.textureNames.push_back(entry.first);
        for (const auto& entry : package_->audio) assets.audioNames.push_back(entry.first);
    } catch (const std::bad_alloc&) {
        package_.reset();
        storage_.reset();
        error = "Cannot allocate scene asset catalog";
        return false;
    }
    if (!runtime_.start(package_->script, error, package_->scene, package_->world ? world : nullptr, &assets,
                        package_->view ? view : nullptr, package_->sound, package_->hud,
                        package_->layout ? layout : nullptr, package_->impulse, package_->control, package_->input,
                        storage_.get(), package_->messaging, initialFrame)) {
        package_.reset();
        storage_.reset();
        return false;
    }
    ++generation_;
    return true;
}

bool GameplaySession::tick(double seconds, std::string& error) {
    const bool ok = runtime_.tick(seconds, error);
    if (!ok && !runtime_.running()) { package_.reset(); storage_.reset(); }
    return ok;
}

bool GameplaySession::input(const std::string& control, double value, std::string& error) {
    if (package_ && !package_->input) {
        error = "Gameplay package did not request input permission";
        return false;
    }
    const bool ok = runtime_.input(control, value, error);
    if (!ok && !runtime_.running()) { package_.reset(); storage_.reset(); }
    return ok;
}

bool GameplaySession::message(std::string_view sender, const PluginScriptRuntime::MessageEvent& event,
                              std::string& error) {
    const bool ok = runtime_.message(sender, event, error);
    if (!ok && !runtime_.running()) { package_.reset(); storage_.reset(); }
    return ok;
}

bool GameplaySession::setControlActive(bool active, std::string& error) {
    const bool ok = runtime_.setControlActive(active, error);
    if (!ok && !runtime_.running()) { package_.reset(); storage_.reset(); }
    return ok;
}

bool GameplaySession::stop(std::string& error) {
    const bool ok = runtime_.stop(error);
    package_.reset();
    storage_.reset();
    return ok;
}

bool GameplaySession::running() const { return runtime_.running(); }
}  // namespace sf
