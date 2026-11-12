#include "AppShell.h"

#include "GameplayLimits.h"
#include "GameplayFrame.h"
#include "GameplayControlMotion.h"
#include "GameplayPermissions.h"
#include "GameplayHudPanel.h"
#include "TextRender.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>

namespace sf {
namespace {
constexpr size_t kGameplayRunningLimit = 4;

bool sameInstalledPackage(const std::filesystem::path& path, const GameplayDigest& expected, std::string& error) {
    GameplayDigest actual;
    if (!gameplayDigestFile(path, actual, error)) return false;
    if (!(actual == expected)) { error = "Gameplay package changed; reinstall it before enabling"; return false; }
    return true;
}
}

bool AppShell::gameplayWorldAvailable() const {
    return state_ == State::FileRoom && cityMode_ && city_ && city_->built();
}

bool AppShell::gameplayWindowFocused() const {
#ifdef SCIFI_TEST_SYNTHETIC_CITY
    return syntheticGameplayFocus_;
#else
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
}

bool AppShell::registerGameplay(const GameplayPackage& package, const std::filesystem::path& path,
                                const GameplayDigest& digest) {
    if (nextGameplayOwner_ == std::numeric_limits<uint64_t>::max())
        return false;
    auto entry = std::make_shared<GameplayEntry>();
    entry->path = path;
    entry->digest = digest;
    entry->world = package.world;
    entry->layout = package.layout;
    entry->impulse = package.impulse;
    entry->control = package.control;
    entry->input = package.input;
    entry->storage = package.storage;
    entry->messaging = package.messaging;
    entry->controller = package.controller;
    entry->view = package.view;
    entry->sound = package.sound;
    entry->hud = package.hud;
    entry->networkHosts = package.networkHosts;
    entry->map = package.map;
    entry->linkHosts = package.linkHosts;
    entry->hotkeys = package.hotkeys;
    entry->settings = package.settings;
    loadPluginSettings(package.id, *entry);
    entry->name = package.name;
    if (!package.roomModel.empty())
        entry->roomModel = std::make_shared<const GameplayModel>(package.models.at(package.roomModel));
    entry->owner = nextGameplayOwner_++;
    entry->scene = std::make_unique<GameplaySceneRenderer>(renderer_, entry->owner);
    if (!gameplay_.emplace(package.id, entry).second) return false;

    Plugin plugin{package.id, package.name, package.description, package.version, [] {}, [this, entry] {
        gameplayControl_.releaseOwner(entry->owner);
        gameplayClicks_.forget(entry->owner);
        std::string error;
        if (!entry->session.stop(error)) std::fprintf(stderr, "Gameplay stop: %s\n", error.c_str());
        entry->scene->clear();
        sfx_.stopOwner(entry->owner);
        entry->soundBudget.reset();
        entry->messageBudget.reset();
        entry->motion.reset();
        entry->hudLabel.reset();
        entry->hudRevision = 0;
        entry->hudDelay = 0;
        if (entry->networkToken) gameplayNetwork_.cancelOwner(entry->networkToken);
        entry->networkToken = 0;
        entry->panelsRevision = 0;
        entry->panelDelay = 0;
        entry->panels = {};
        entry->panelLabels = {};
        entry->panelScrollStart = {};
        entry->panelRasterScales = {};
        entry->panelAreaWidth = entry->panelAreaHeight = 0;
        entry->panelPixelBudget = 0;
        entry->panelUploads = 0;
        entry->pendingError.clear();
        entry->inputState.reset();
        entry->controllerInput.reset();
    }, false};
    plugin.prepare = [this, entry, id = package.id](std::string& error) {
        size_t active = 0;
        for (const auto& item : gameplay_) if (item.second->session.running()) ++active;
        if (active >= kGameplayRunningLimit) { error = "At most four gameplay plugins may run"; return false; }
        if (!sameInstalledPackage(entry->path, entry->digest, error)) return false;
        PluginScriptRuntime::WorldQueries world;
        world.context = this;
        world.available = [](void* context) noexcept {
            return static_cast<AppShell*>(context)->gameplayWorldAvailable();
        };
        world.floor = [](void* context, double x, double y, double z, double radius, double eye, double* out) noexcept {
            auto& app = *static_cast<AppShell*>(context);
            if (!app.gameplayWorldAvailable()) return false;
            try {
                *out = app.city_->floorAt({static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
                                         static_cast<float>(radius), static_cast<float>(eye));
            } catch (const std::bad_alloc&) {
                return false;
            }
            return true;
        };
        world.resolve = [](void* context, double* x, double* y, double* z, double radius, double eye) noexcept {
            auto& app = *static_cast<AppShell*>(context);
            if (!app.gameplayWorldAvailable()) return false;
            Vec3 p{static_cast<float>(*x), static_cast<float>(*y), static_cast<float>(*z)};
            try {
                app.city_->resolve(p, static_cast<float>(radius), static_cast<float>(eye));
            } catch (const std::bad_alloc&) {
                return false;
            }
            *x = p.x; *y = p.y; *z = p.z;
            return true;
        };
        world.trackClearance = [](void* context, double x, double z, double radius, double* out) noexcept {
            auto& app = *static_cast<AppShell*>(context);
            if (!app.gameplayWorldAvailable()) return false;
            try {
                return app.city_->gameplayTrackClearance(static_cast<float>(x), static_cast<float>(z),
                                                         static_cast<float>(radius), out);
            } catch (const std::bad_alloc&) {
                return false;
            }
        };
        PluginScriptRuntime::ViewQuery view;
        view.context = this;
        view.pose = [](void* context, double* out) noexcept {
            auto& app = *static_cast<AppShell*>(context);
            if (app.state_ != State::FileRoom) return false;
            const Vec3 eye = app.viewEye();
            const Vec3 direction = app.vrOn_ ? app.head_.forward : app.camera_.forward();
            out[0] = eye.x; out[1] = eye.y; out[2] = eye.z;
            out[3] = direction.x; out[4] = direction.y; out[5] = direction.z;
            return true;
        };
        PluginScriptRuntime::LayoutQuery layout;
        layout.context = this;
        layout.cell = [](void* context, double x, double z, double* out) noexcept {
            auto& app = *static_cast<AppShell*>(context);
            if (!app.gameplayWorldAvailable()) return -1;
            return app.city_->gameplayCell(static_cast<float>(x), static_cast<float>(z), out);
        };
        const auto preferences = PluginManager::defaultStatePath();
        const auto storageDirectory = preferences.empty() ? std::filesystem::path{} :
            preferences.parent_path() / "plugin-storage";
        PluginScriptRuntime::MapQuery map;
        map.context = this;
        map.view = [](void* context, PluginScriptRuntime::MapView* out, char* reason, size_t size) noexcept {
            auto& app = *static_cast<AppShell*>(context);
            const char* why = nullptr;
            if (app.state_ != State::FileRoom || !app.city_) why = "city unavailable";
            else if (!app.cityMode_) why = "interior";
            else if (!app.city_->built()) why = "city unavailable";
            if (why) { std::snprintf(reason, size, "%s", why); return false; }
            const Vec3 eye = app.viewEye();
            const Vec3 forward = app.vrOn_ ? app.head_.forward : app.camera_.forward();
            const float extent = app.city_->extent();
            out->x = eye.x; out->z = eye.z;
            // Heading in degrees clockwise from north (-Z), matching a map drawn with north up.
            out->heading = std::atan2(forward.x, -forward.z) * 180.0 / 3.14159265358979323846;
            if (out->heading < 0) out->heading += 360.0;
            out->minX = -extent; out->minZ = -extent; out->maxX = extent; out->maxZ = extent;
            return true;
        };
        map.places = [](void* context, bool regions, PluginScriptRuntime::MapPlace* out, size_t max) noexcept -> size_t {
            auto& app = *static_cast<AppShell*>(context);
            if (!app.gameplayWorldAvailable()) return 0;
            std::vector<City::MapPlace> places;
            try { app.city_->mapPlaces(regions, places); } catch (const std::bad_alloc&) { return 0; }
            size_t count = 0;
            auto copy = [](std::array<char, 32>& to, const std::string& from) {
                to.fill(0);
                size_t n = 0;
                for (unsigned char c : from) {
                    if (n + 1 >= to.size()) break;
                    if (c >= 0x20 && c < 0x7f) to[n++] = static_cast<char>(c);
                }
            };
            for (const auto& place : places) {
                if (count >= max) break;
                auto& item = out[count++];
                copy(item.id, place.id); copy(item.name, place.name); copy(item.kind, place.kind);
                item.x = place.x; item.z = place.z; item.width = place.width; item.depth = place.depth;
            }
            return count;
        };
        PluginScriptRuntime::HostServices services;
        services.context = this;
        services.map = entry->map ? &map : nullptr;
        services.settings = &entry->settingValues;
        if (entry->hud) {
            services.measureText = [](void*, const char* text, size_t size, float px) noexcept -> float {
                try { return measureText(std::string(text, size), px, FontFace::UI); }
                catch (...) { return -1.0f; }
            };
            services.hudView = [](void* context, int* width, int* height) noexcept {
                auto& app = *static_cast<AppShell*>(context);
                if (app.gameplayHudAreaWidth_ <= 0 || app.gameplayHudAreaHeight_ <= 0) return false;
                *width = app.gameplayHudAreaWidth_; *height = app.gameplayHudAreaHeight_;
                return true;
            };
        }
        if (!entry->session.startFile(entry->path, entry->digest, *gameplayConsent_, entry->world ? &world : nullptr,
                                      error, entry->view ? &view : nullptr, entry->layout ? &layout : nullptr,
                                      storageDirectory, gameplayFrame_, &services))
            return false;
        if (!entry->scene->prepare(entry->session, error)) {
            std::string stopError;
            if (!entry->session.stop(stopError)) std::fprintf(stderr, "Gameplay stop: %s\n", stopError.c_str());
            entry->scene->clear();
            return false;
        }
        return true;
    };
    plugin.cancelPrepare = [entry] {
        std::string error;
        if (!entry->session.stop(error)) std::fprintf(stderr, "Gameplay rollback: %s\n", error.c_str());
        entry->scene->clear();
    };
    if (!plugins_.add(std::move(plugin))) { gameplay_.erase(package.id); return false; }
    installedPluginIds_.insert(package.id);
    if (!package.downtownModel.empty()) {
        std::string error;
        if (!city_->setDowntownModel(package.id, package.models.at(package.downtownModel), error)) {
            pluginMessages_[package.id] = "DEFAULT BUILDING: " + error;
            std::fprintf(stderr, "Downtown %s: %s\n", package.id.c_str(), error.c_str());
        }
    }
    return true;
}

void AppShell::toggleGameplay(const std::string& id) {
    auto entry = gameplay_.find(id);
    if (entry == gameplay_.end()) { settingsPanel_->setNotice("GAMEPLAY PLUGIN UNAVAILABLE"); return; }
    bool running = false;
    for (const auto& plugin : plugins_.plugins()) if (plugin.id == id) { running = plugin.running; break; }
    if (running) {
        gameplayConfirm_.clear();
        if (!plugins_.setEnabled(id, false)) settingsPanel_->setNotice(plugins_.error());
        else settingsPanel_->setNotice("PLUGIN DISABLED");
        return;
    }
    std::string error;
    bool granted = false;
    if (!sameInstalledPackage(entry->second->path, entry->second->digest, error) ||
        !gameplayConsent_->matchesDigest(id, entry->second->digest, granted, error)) {
        settingsPanel_->setNotice(error);
        return;
    }
    bool newGrant = false;
    if (!granted) {
        const auto notice = gameplayConsentNotice(id, entry->second->world, entry->second->input,
                                                  entry->second->view, entry->second->sound, entry->second->hud,
                                                  entry->second->layout, entry->second->impulse, entry->second->control,
                                                  entry->second->storage, entry->second->controller,
                                                  entry->second->messaging, entry->second->networkHosts,
                                                  entry->second->map, entry->second->hotkeyLetters(),
                                                  entry->second->linkHosts);
        if (gameplayConfirm_ != id || std::chrono::steady_clock::now() >= gameplayDeadline_ ||
            settingsPanel_->notice() != notice) {
            gameplayConfirm_ = id;
            gameplayConfirmNotice_ = notice;
            gameplayDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            settingsPanel_->setNotice(notice);
            return;
        }
        gameplayConfirm_.clear();
        if (!gameplayConsent_->grantFile(id, entry->second->path, entry->second->digest, error)) {
            settingsPanel_->setNotice(error); return;
        }
        newGrant = true;
    }
    if (!plugins_.setEnabled(id, true)) {
        const std::string failure = plugins_.error();
        if (newGrant && !gameplayConsent_->revoke(id, error)) {
            settingsPanel_->setNotice(failure + "; CONSENT CLEANUP FAILED: " + error);
            return;
        }
        settingsPanel_->setNotice(failure.empty() ? "CANNOT ENABLE GAMEPLAY PLUGIN" : failure);
    } else {
        settingsPanel_->setNotice("PLUGIN ENABLED");
    }
}

void AppShell::releaseGameplayControl() {
    const auto owner = gameplayControl_.owner();
    gameplayControl_.release();
    if (!owner) return;
    for (auto& [id, entry] : gameplay_) {
        if (entry->owner != owner || !entry->session.running()) continue;
        entry->mouseInput.reset();
        std::string error;
        const auto dispatch = [&](const char* name, double value) {
            return entry->session.input(name, value, error);
        };
        if (!entry->inputState.update(entry->input, false, 0, 0, 0, false, false, dispatch) ||
            !entry->controllerInput.update(entry->controller, false, {}, dispatch) ||
            !entry->session.setControlActive(false, error)) {
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
        }
        break;
    }
}

std::string AppShell::nearbyGameplayControl(bool controllerOnly) const {
    if (gameplayControl_.owner() || !gameplayWorldAvailable()) return {};
    const GameplayControlLease::Pose current{camera_.position.x, camera_.position.y, camera_.position.z,
                                              camera_.yaw, camera_.pitch};
    std::string nearestId;
    double best = 16.0;
    for (const auto& [id, entry] : gameplay_) {
        if (!entry->control || !entry->session.running()) continue;
        const auto offer = entry->session.controlOffer();
        if (!offer.available) continue;
        const double dx = offer.x - current.x, dy = offer.y - current.y, dz = offer.z - current.z;
        const double distance = dx*dx + dy*dy + dz*dz;
        if (distance < best || (distance == best && nearestId.empty())) { best = distance; nearestId = id; }
    }
    if (controllerOnly && !nearestId.empty() && !gameplay_.at(nearestId)->controller) return {};
    return nearestId;
}

void AppShell::acquireGameplayControl(bool controllerOnly) {
    const auto nearestId = nearbyGameplayControl(controllerOnly);
    if (nearestId.empty()) return;
    const auto nearest = gameplay_.at(nearestId);
    const GameplayControlLease::Pose current{camera_.position.x, camera_.position.y, camera_.position.z,
                                              camera_.yaw, camera_.pitch};
    const auto offer = nearest->session.controlOffer();
    if (!gameplayControl_.acquire(nearest->owner, nearest->control, true, true, current,
                                  {offer.x, offer.y, offer.z, 0, 0})) return;
    std::string error;
    if (!nearest->session.setControlActive(true, error)) {
        gameplayControl_.release();
        plugins_.markFailed(nearestId, error);
        if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
    } else {
        nearest->inputState.reset();
        nearest->mouseInput.reset();
        nearest->controllerInput.reset();
    }
}

uint64_t AppShell::gameplayClickTarget(const Vec3& origin, const Vec3& direction) const {
    if (gameplayControl_.owner()) return gameplayControl_.owner();
    for (const auto& [id, entry] : gameplay_)
        if (entry->input && entry->session.running() && entry->session.capturesClicks()) return entry->owner;
    const double o[] = {origin.x, origin.y, origin.z};
    const double d[] = {direction.x, direction.y, direction.z};
    double best = 14.0;
    uint64_t owner = 0;
    for (const auto& [id, entry] : gameplay_) {
        double distance = 0;
        if (entry->input && entry->session.running() &&
            entry->session.interactiveHit(o, d, distance) &&
            (!owner || distance < best)) {
            best = distance;
            owner = entry->owner;
        }
    }
    return owner;
}

void AppShell::tickGameplay(double dt, const FrameInput& input, const PadState& pad, bool allowInput) {
    if (gameplayFrame_ < static_cast<std::uint64_t>(INT64_MAX)) ++gameplayFrame_;
    for (auto& [id, entry] : gameplay_)
        if (entry->session.running()) entry->session.prepareFrame(gameplayFrame_);
    if (gameplayControl_.owner() && (!allowInput || !gameplayWorldAvailable())) releaseGameplayControl();
    const auto incoming = gameplayMessages_.take();
    for (size_t i = 0; i < incoming.count; ++i) {
        const auto& message = incoming.messages[i];
        bool senderRunning = false;
        for (const auto& [id, entry] : gameplay_) {
            if (entry->owner == message.owner && entry->session.running() &&
                entry->session.generation() == message.generation) {
                senderRunning = true;
                break;
            }
        }
        if (!senderRunning) continue;
        for (const auto& [id, entry] : gameplay_) {
            if (!entry->messaging || entry->owner == message.owner || !entry->session.running()) continue;
            std::string error;
            if (!entry->session.message(std::string_view(message.sender.data(), message.senderSize),
                                        message.event, error)) {
                plugins_.markFailed(id, error);
                if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
            }
        }
    }
    for (auto& result : gameplayNetwork_.take()) {
        for (auto& [id, entry] : gameplay_) {
            if (!result.owner || entry->networkToken != result.owner || !entry->session.running()) continue;
            std::string error;
            if (!entry->session.networkResult(result.id, result.ok, result.status, result.text, error)) {
                plugins_.markFailed(id, error);
                if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
            }
            break;
        }
    }
    for (auto& [id, entry] : gameplay_) {
        if (!entry->session.running()) continue;
        std::string error;
        if (!entry->pendingError.empty()) {
            error = std::move(entry->pendingError);
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
            continue;
        }
        const bool pluginInput = allowInput && (!gameplayControl_.owner() || gameplayControl_.owner() == entry->owner);
        const bool controllerOk = entry->controllerInput.update(entry->controller, pluginInput, pad,
            [&](const char* name, double value) { return entry->session.input(name, value, error); });
        if (!controllerOk) {
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
            continue;
        }
        const bool mouseOk = entry->mouseInput.update(entry->input,
            pluginInput && gameplayControl_.owner() == entry->owner &&
                input_.relativeMouse() && !input.mouseReset,
            input.lookX * 0.0025 * settings_.lookScale(),
            input.lookY * 0.0025 * settings_.lookScale() * (settings_.invertY ? -1 : 1), dt,
            [&](const char* name, double value) { return entry->session.input(name, value, error); });
        if (!mouseOk) {
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
            continue;
        }
        bool ok = runGameplayFrame(entry->session, entry->inputState, entry->input, pluginInput,
                                   input.moveX, input.moveY, input.moveZ,
                                   input.click && gameplayClicks_.owner() == entry->owner, input.clickUp,
                                   dt, error, [&](const std::string& reason) {
            plugins_.markFailed(id, reason);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
        }, input.shift);
        if (!ok) continue;
        if (pluginInput && gameplayWorldAvailable()) {
            for (const char key : input.pluginKeys) {
                for (const auto& [letter, action] : entry->hotkeys) {
                    if (letter != key) continue;
                    if (!entry->session.action(action, error)) { ok = false; break; }
                    const std::string link = entry->session.takeLink();
                    if (link.empty()) continue;
                    if (!script_.empty()) scriptLog("plugin link " + link);
#ifdef SCIFI_HAS_CEF
                    if (browser_) {
                        browser_->open(link, head_.eye, head_.forward, vrOn_);
                        raiseFocus({kWinBrowser, browser_.get()});
                    }
#else
                    settingsPanel_->setNotice("BUILT-IN BROWSER UNAVAILABLE IN THIS BUILD");
#endif
                }
                if (!ok) break;
            }
            if (!ok) {
                plugins_.markFailed(id, error);
                if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
                continue;
            }
            if (!entry->session.running()) continue;
        }
        const auto pose = entry->session.takeControlPose();
        if (gameplayControl_.owner() == entry->owner && !entry->session.controlOffer().available)
            releaseGameplayControl();
        if (!entry->session.running()) continue;
        if (ok && pose.pending && dt > 0 && gameplayControl_.owner() == entry->owner) {
            const GameplayControlLease::Pose current{camera_.position.x, camera_.position.y, camera_.position.z,
                                                      camera_.yaw, camera_.pitch};
            const GameplayControlLease::Pose proposed{pose.x, pose.y, pose.z, pose.yaw, pose.pitch};
            const double extent = city_->extent() * 1.3;
            GameplayControlLease::Pose resolved{};
            GameplayControlMotionResult motion = GameplayControlMotionResult::InvalidWorld;
            try {
                motion = resolveGameplayControlMotion(gameplayControl_, entry->owner,
                    allowInput && gameplayWorldAvailable(), current, proposed, dt, extent,
                    [&](GameplayControlLease::Pose& candidate) {
                        Vec3 p{static_cast<float>(candidate.x), static_cast<float>(candidate.y),
                               static_cast<float>(candidate.z)};
                        const Vec3 before = p;
                        city_->resolve(p, 0.8f, 1.7f);
                        const float floor = city_->floorAt(p, 0.8f, 1.7f);
                        if (!std::isfinite(floor)) return false;
                        p.y = std::max(p.y, floor + 1.7f);
                        // Don't accumulate float conversion error across hundreds of clear substeps.
                        if (p.x != before.x) candidate.x = p.x;
                        if (p.y != before.y) candidate.y = p.y;
                        if (p.z != before.z) candidate.z = p.z;
                        return true;
                    }, resolved);
            } catch (const std::bad_alloc&) {
                error = "Gameplay control collision query failed";
                ok = false;
            }
            if (ok && motion != GameplayControlMotionResult::Applied) {
                error = motion == GameplayControlMotionResult::InvalidProposal ?
                    "Gameplay control pose exceeds motion limits" : "Gameplay control collision query failed";
                ok = false;
            }
            if (ok) {
                camera_.position = {static_cast<float>(resolved.x), static_cast<float>(resolved.y),
                                    static_cast<float>(resolved.z)};
                camera_.yaw = static_cast<float>(resolved.yaw);
                camera_.pitch = static_cast<float>(resolved.pitch);
                flying_ = false;
                vy_ = 0;
            }
        }
        entry->motion.advance(dt);
        if (!allowInput || !gameplayWorldAvailable()) entry->motion.clearMotion();
        if (ok) {
            const auto impulse = entry->session.takeImpulse();
            if (impulse.pending && allowInput && gameplayWorldAvailable() &&
                !entry->motion.request(impulse.x, impulse.y, impulse.z)) {
                error = "Gameplay player push rate limit exceeded";
                ok = false;
            }
        }
        entry->hudDelay = std::max(0.0, entry->hudDelay - dt);
        entry->panelDelay = std::max(0.0, entry->panelDelay - dt);
        if (ok) {
            const auto requests = entry->session.takeNetworkRequests();
            if (!requests.empty() && !entry->networkToken) entry->networkToken = ++nextGameplayNetworkToken_;
            for (const auto& request : requests)
                if (!gameplayNetwork_.submit(entry->networkToken, request.id, request.url, error)) { ok = false; break; }
            for (const auto cancelled : entry->session.takeNetworkCancels())
                if (entry->networkToken) gameplayNetwork_.cancel(entry->networkToken, cancelled);
        }
        if (ok) {
            const auto sounds = entry->session.takeSounds();
            if (!entry->soundBudget.consume(dt, sounds.count)) {
                error = "Gameplay sound rate limit exceeded";
                ok = false;
            } else {
                static constexpr SfxId ids[] = {SfxId::Pickup, SfxId::Throw, SfxId::Boom,
                                                 SfxId::DuckPop, SfxId::CarSummon, SfxId::RailOn};
                for (size_t i = 0; i < sounds.count && ok; ++i) {
                    if (!sfx_.playOwned(entry->owner, ids[static_cast<int>(sounds.events[i].sound)],
                                        sounds.events[i].volume)) {
                        error = "Gameplay sound unavailable or audio voice limit exceeded";
                        ok = false;
                    }
                }
            }
        }
        if (ok) {
            const auto engine = entry->session.takeEngineSound();
            if (engine.pending && !sfx_.setOwnedEngine(entry->owner, engine.active, engine.speed, engine.boost)) {
                error = "Gameplay engine sound unavailable or audio voice limit exceeded";
                ok = false;
            }
        }
        if (ok) {
            const auto samples = entry->session.takeSamples();
            if (!entry->soundBudget.consume(0, samples.count)) {
                error = "Gameplay sound rate limit exceeded";
                ok = false;
            } else {
                for (size_t i = 0; i < samples.count && ok; ++i) {
                    const auto& event = samples.events[i];
                    if (event.stop) {
                        sfx_.stopOwnedSample(entry->owner, event.voice);
                        continue;
                    }
                    if (event.loopUpdate) {
                        if (!sfx_.loopOwnedSample(entry->owner, event.voice)) {
                            error = "Gameplay sample voice is no longer playing";
                            ok = false;
                        }
                        continue;
                    }
                    const auto* package = entry->session.package();
                    if (!package || event.asset < 0 || static_cast<size_t>(event.asset) >= package->audio.size()) {
                        error = "Invalid gameplay audio asset";
                        ok = false;
                        break;
                    }
                    auto asset = package->audio.begin();
                    std::advance(asset, event.asset);
                    if (!sfx_.playOwnedSample(entry->owner, event.voice, asset->second, event.volume, event.loop)) {
                        error = "Gameplay sample unavailable or audio voice limit exceeded";
                        ok = false;
                    }
                }
                if (ok && entry->messaging) {
                    const auto messages = entry->session.takeMessages();
                    if (!entry->messageBudget.consume(dt, messages.count)) {
                        error = "Gameplay message rate limit exceeded";
                        ok = false;
                    } else {
                        for (size_t i = 0; i < messages.count; ++i) {
                            if (!gameplayMessages_.push(id, entry->owner, entry->session.generation(), messages.events[i])) {
                                error = "Gameplay message queue limit exceeded";
                                ok = false;
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (!ok) {
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
        }
    }
}

void AppShell::prepareGameplay() {
    const auto hudPlugins = std::count_if(gameplay_.begin(), gameplay_.end(), [](const auto& pair) {
        return pair.second->session.running() && pair.second->hud;
    });
    const size_t budget = std::min(kGameplayHudPluginPixels, kGameplayHudGlobalPixels / std::max<size_t>(1, hudPlugins));
    if (budget != gameplayHudPixelBudget_) {
        // Release old allocations before redistributing the global budget.
        for (auto& [id, entry] : gameplay_) {
            entry->panelLabels = {};
            entry->panelRasterScales = {};
            entry->panelDelay = 0;
        }
        gameplayHudPixelBudget_ = budget;
    }
    gameplayHudAreaWidth_ = static_cast<int>(renderer_.desktopWidth());
    gameplayHudAreaHeight_ = static_cast<int>(renderer_.desktopHeight());
    if (vrOn_) {
        const float halfH = std::tan(kFovY * 0.5f) * 1.2f;
        gameplayHudAreaWidth_ = std::max(gameplayHudVrWidth_,
            static_cast<int>(renderer_.desktopHeight() * 0.7f / (halfH * 2)));
        gameplayHudAreaHeight_ = std::max(gameplayHudVrHeight_,
            static_cast<int>(renderer_.desktopHeight() * 0.5f / (halfH * 2)));
    }
    for (auto& [id, entry] : gameplay_) {
        if (!entry->session.running()) continue;
        std::string error;
        if (!entry->scene->prepare(entry->session, error)) {
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
            continue;
        }
        const auto text = entry->session.hudText();
        if (entry->hud && entry->hudRevision != text.revision && entry->hudDelay <= 0) {
            try {
                if (!text.text[0]) entry->hudLabel.reset();
                else {
                    entry->hudLabel = renderer_.createLabel(makeLabel(id + ": " + text.text.data(),
                        40.0f, FontFace::UI, Color8{}, 96, 4));
                    if (!entry->hudLabel) error = "Cannot allocate gameplay HUD";
                }
            } catch (const std::bad_alloc&) {
                error = "Cannot allocate gameplay HUD";
            }
            if (!error.empty()) {
                plugins_.markFailed(id, error);
                if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
                continue;
            }
            entry->hudRevision = text.revision;
            entry->hudDelay = 0.25;
        }
        if (entry->hud && !updateGameplayPanels(*entry, error)) {
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
        }
    }
}

bool AppShell::updateGameplayPanels(GameplayEntry& entry, std::string& error) {
    const auto revision = entry.session.hudPanelsRevision();
    if (revision == entry.panelsRevision && entry.panelAreaWidth == gameplayHudAreaWidth_ &&
        entry.panelAreaHeight == gameplayHudAreaHeight_ && entry.panelPixelBudget == gameplayHudPixelBudget_) return true;
    std::vector<PluginScriptRuntime::HudPanel> panels;
    try {
        panels = entry.session.hudPanels();
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay HUD panels";
        return false;
    }
    auto sameContent = [](const PluginScriptRuntime::HudPanel& a, const PluginScriptRuntime::HudPanel& b) {
        if (a.id != b.id || a.canvasWidth != b.canvasWidth || a.canvasHeight != b.canvasHeight ||
            a.background != b.background || a.commands.size() != b.commands.size()) return false;
        for (size_t i = 0; i < a.commands.size(); ++i) {
            const auto& x = a.commands[i];
            const auto& y = b.commands[i];
            if (x.kind != y.kind || x.x0 != y.x0 || x.y0 != y.y0 || x.x1 != y.x1 || x.y1 != y.y1 ||
                x.size != y.size || x.color != y.color || x.align != y.align || x.text != y.text) return false;
        }
        return true;
    };
    std::array<const PluginScriptRuntime::HudPanel*, 4> next{};
    for (const auto& panel : panels)
        if (panel.id >= 1 && panel.id <= 4) next[static_cast<size_t>(panel.id - 1)] = &panel;
    const auto scales = gameplayHudRasterScales(panels, gameplayHudAreaWidth_, gameplayHudAreaHeight_, gameplayHudPixelBudget_);
    bool redraw = false;
    for (size_t i = 0; i < next.size(); ++i)
        if (next[i] && scales[i] > 0 && (!entry.panelLabels[i] || scales[i] != entry.panelRasterScales[i] ||
            !sameContent(*next[i], entry.panels[i]))) redraw = true;
    // Removal, placement and visibility apply immediately; pixel uploads are throttled to 10 Hz per plugin.
    if (redraw && entry.panelDelay > 0) {
        for (size_t i = 0; i < next.size(); ++i) {
            if (!next[i] || scales[i] <= 0) {
                entry.panels[i] = next[i] ? *next[i] : PluginScriptRuntime::HudPanel{};
                entry.panelLabels[i].reset();
                continue;
            }
            auto& shown = entry.panels[i];
            if (!entry.panelLabels[i] || shown.id != next[i]->id) continue;
            shown.anchor = next[i]->anchor; shown.x = next[i]->x; shown.y = next[i]->y;
            shown.width = next[i]->width; shown.height = next[i]->height; shown.visible = next[i]->visible;
            if (shown.scrollSpeed != next[i]->scrollSpeed) entry.panelScrollStart[i] = time_;
            shown.scrollSpeed = next[i]->scrollSpeed; shown.viewWidth = next[i]->viewWidth;
        }
        return true;
    }
    try {
        for (size_t i = 0; i < next.size(); ++i) {
            if (!next[i] || scales[i] <= 0) {
                entry.panels[i] = next[i] ? *next[i] : PluginScriptRuntime::HudPanel{};
                entry.panelLabels[i].reset();
                continue;
            }
            if (!entry.panelLabels[i] || scales[i] != entry.panelRasterScales[i] || !sameContent(*next[i], entry.panels[i])) {
                if (!sameContent(*next[i], entry.panels[i]) || entry.panels[i].scrollSpeed != next[i]->scrollSpeed)
                    entry.panelScrollStart[i] = time_;
                entry.panelLabels[i].reset();
                std::shared_ptr<GpuLabel> texture = renderer_.createLabel(rasterizeGameplayHudPanel(*next[i], scales[i]));
                if (!texture) { error = "Cannot allocate gameplay HUD panel texture"; return false; }
                entry.panelLabels[i] = std::move(texture);
                ++entry.panelUploads;
            }
            entry.panels[i] = *next[i];
        }
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate gameplay HUD panel";
        return false;
    }
    if (redraw) entry.panelDelay = 0.1;
    entry.panelsRevision = revision;
    entry.panelRasterScales = scales;
    entry.panelAreaWidth = gameplayHudAreaWidth_;
    entry.panelAreaHeight = gameplayHudAreaHeight_;
    entry.panelPixelBudget = gameplayHudPixelBudget_;
    return true;
}

void AppShell::renderGameplay() {
    if (!crosshair_ || state_ != State::FileRoom) return;
    for (auto& [id, entry] : gameplay_) {
        if (!entry->session.running() || !entry->pendingError.empty()) continue;
        std::string error;
        if (!entry->scene->draw(entry->session, *crosshair_, error)) entry->pendingError = error;
    }
}
}  // namespace sf
