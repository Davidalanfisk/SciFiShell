#include "GameplaySession.h"

#include "GameplayConsent.h"

#include <new>
#include <limits>

namespace sf {
bool GameplaySession::start(const std::string& bytes, const GameplayConsent& consent,
                            const PluginScriptRuntime::WorldQueries* world, std::string& error,
                            const PluginScriptRuntime::ViewQuery* view,
                            const PluginScriptRuntime::LayoutQuery* layout) {
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
    if (parsed.world && !world) { error = "Gameplay world service is unavailable"; return false; }
    if (parsed.view && !view) { error = "Gameplay view service is unavailable"; return false; }
    if (parsed.layout && !layout) { error = "Gameplay layout service is unavailable"; return false; }
    try {
        package_ = std::make_unique<GameplayPackage>(std::move(parsed));
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay session";
        return false;
    }
    PluginScriptRuntime::AssetCounts assets{static_cast<int>(package_->models.size()),
                                            static_cast<int>(package_->textures.size())};
    try {
        for (const auto& entry : package_->models) assets.modelNames.push_back(entry.first);
        for (const auto& entry : package_->textures) assets.textureNames.push_back(entry.first);
    } catch (const std::bad_alloc&) {
        package_.reset();
        error = "Cannot allocate scene asset catalog";
        return false;
    }
    if (!runtime_.start(package_->script, error, package_->scene, package_->world ? world : nullptr, &assets,
                        package_->view ? view : nullptr, package_->sound, package_->hud,
                        package_->layout ? layout : nullptr, package_->impulse, package_->control, package_->input)) {
        package_.reset();
        return false;
    }
    ++generation_;
    return true;
}

bool GameplaySession::tick(double seconds, std::string& error) {
    const bool ok = runtime_.tick(seconds, error);
    if (!ok && !runtime_.running()) package_.reset();
    return ok;
}

bool GameplaySession::input(const std::string& control, double value, std::string& error) {
    if (package_ && !package_->input) {
        error = "Gameplay package did not request input permission";
        return false;
    }
    const bool ok = runtime_.input(control, value, error);
    if (!ok && !runtime_.running()) package_.reset();
    return ok;
}

bool GameplaySession::setControlActive(bool active, std::string& error) {
    const bool ok = runtime_.setControlActive(active, error);
    if (!ok && !runtime_.running()) package_.reset();
    return ok;
}

bool GameplaySession::stop(std::string& error) {
    const bool ok = runtime_.stop(error);
    package_.reset();
    return ok;
}

bool GameplaySession::running() const { return runtime_.running(); }
}  // namespace sf
