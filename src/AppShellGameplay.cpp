#include "AppShell.h"

#include "GameplayLimits.h"
#include "GameplayFrame.h"
#include "GameplayControlMotion.h"
#include "GameplayPermissions.h"
#include "TextRender.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>

namespace sf {
namespace {
constexpr size_t kGameplayStoredBytes = 64 * 1024 * 1024;
constexpr size_t kGameplayRunningLimit = 4;

bool sameInstalledPackage(const std::string& id, const std::string& bytes, std::string& error) {
    const auto state = PluginManager::defaultStatePath();
    if (state.empty()) { error = "Plugin package path is unavailable"; return false; }
    const auto path = state.parent_path() / "plugins" / (id + ".zip");
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        error = "Gameplay package is missing or not a regular file";
        return false;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size != bytes.size() || size > kGameplayArchiveLimit) {
        error = "Gameplay package changed; reinstall it before enabling";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    char buffer[8192];
    for (size_t pos = 0; pos < bytes.size();) {
        const auto count = std::min(sizeof(buffer), bytes.size() - pos);
        if (!in.read(buffer, static_cast<std::streamsize>(count)) ||
            std::memcmp(buffer, bytes.data() + pos, count) != 0) {
            error = "Gameplay package changed; reinstall it before enabling";
            return false;
        }
        pos += count;
    }
    if (in.peek() != std::ifstream::traits_type::eof() || in.bad()) {
        error = "Cannot finish reading gameplay package";
        return false;
    }
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

bool AppShell::registerGameplay(const GameplayPackage& package, const std::string& bytes) {
    size_t total = 0;
    for (const auto& entry : gameplay_) total += entry.second->bytes.size();
    if (bytes.size() > kGameplayStoredBytes - total || nextGameplayOwner_ == std::numeric_limits<uint64_t>::max())
        return false;
    auto entry = std::make_shared<GameplayEntry>();
    entry->bytes = bytes;
    entry->world = package.world;
    entry->layout = package.layout;
    entry->impulse = package.impulse;
    entry->control = package.control;
    entry->input = package.input;
    entry->view = package.view;
    entry->sound = package.sound;
    entry->hud = package.hud;
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
        entry->motion.reset();
        entry->hudLabel.reset();
        entry->hudRevision = 0;
        entry->hudDelay = 0;
        entry->pendingError.clear();
        entry->inputState.reset();
    }, false};
    plugin.prepare = [this, entry, id = package.id](std::string& error) {
        size_t active = 0;
        for (const auto& item : gameplay_) if (item.second->session.running()) ++active;
        if (active >= kGameplayRunningLimit) { error = "At most four gameplay plugins may run"; return false; }
        if (!sameInstalledPackage(id, entry->bytes, error)) return false;
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
        if (!entry->session.start(entry->bytes, *gameplayConsent_, entry->world ? &world : nullptr, error,
                                  entry->view ? &view : nullptr, entry->layout ? &layout : nullptr))
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
    if (!sameInstalledPackage(id, entry->second->bytes, error) ||
        !gameplayConsent_->matches(id, entry->second->bytes, granted, error)) {
        settingsPanel_->setNotice(error);
        return;
    }
    bool newGrant = false;
    if (!granted) {
        const auto notice = gameplayConsentNotice(id, entry->second->world, entry->second->input,
                                                  entry->second->view, entry->second->sound, entry->second->hud,
                                                  entry->second->layout, entry->second->impulse, entry->second->control);
        if (gameplayConfirm_ != id || std::chrono::steady_clock::now() >= gameplayDeadline_ ||
            settingsPanel_->notice() != notice) {
            gameplayConfirm_ = id;
            gameplayDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            settingsPanel_->setNotice(notice);
            return;
        }
        gameplayConfirm_.clear();
        if (!gameplayConsent_->grant(id, entry->second->bytes, error)) { settingsPanel_->setNotice(error); return; }
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
        std::string error;
        if (!entry->session.setControlActive(false, error)) {
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
        }
        break;
    }
}

std::string AppShell::nearbyGameplayControl() const {
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
    return nearestId;
}

void AppShell::acquireGameplayControl() {
    const auto nearestId = nearbyGameplayControl();
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

void AppShell::tickGameplay(double dt, const FrameInput& input, bool allowInput) {
    if (gameplayControl_.owner() && (!allowInput || !gameplayWorldAvailable())) releaseGameplayControl();
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
        bool ok = runGameplayFrame(entry->session, entry->inputState, entry->input, pluginInput,
                                   input.moveX, input.moveY, input.moveZ,
                                   input.click && gameplayClicks_.owner() == entry->owner, input.clickUp,
                                   dt, error, [&](const std::string& reason) {
            plugins_.markFailed(id, reason);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
        });
        if (!ok) continue;
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
                        city_->resolve(p, 0.8f, 1.7f);
                        const float floor = city_->floorAt(p, 0.8f, 1.7f);
                        if (!std::isfinite(floor)) return false;
                        candidate.x = p.x;
                        candidate.y = std::max(p.y, floor + 1.7f);
                        candidate.z = p.z;
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
        if (!ok) {
            plugins_.markFailed(id, error);
            if (settingsPanel_->isOpen()) settingsPanel_->setNotice(plugins_.error());
        }
    }
}

void AppShell::prepareGameplay() {
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
    }
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
