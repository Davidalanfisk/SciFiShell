#include "AppShell.h"

#include <cstdio>

namespace sf {
void AppShell::syncDowntown() {
    if (!downtown_) return;
    auto ids = installedPluginIds_;
    std::vector<std::pair<std::string, std::string>> names;
    for (const auto& p : plugins_.plugins())
        if (installedPluginIds_.count(p.id) || downtownBuiltin(p.id)) {
            ids.insert(p.id);
            names.emplace_back(p.id, p.name);
        }
    const bool saved = downtown_->reconcile(ids);
    const bool resized = !(downtownRegion(downtown_->placements()) == city_->downtownBounds());
    if (resized) {
        // Resizing repacks nearby drives and regenerates routes/track. Leave old geometry first.
        evacuateSettingsBuilding();
        evacuateDowntown();
        releaseGameplayControl();
        if (driving_) toggleCar();
        riding_ = false;
        knock_ = {};
        carOn_ = false;
        if (cityMode_) {
            camera_.position = {0,1.7f,96};
            flying_ = false; vy_ = 0;
        }
        if (cityCamSet_) savedCam_.position = {0,1.7f,96};
    }
    city_->setDowntown(downtown_->placements(), names);
    if (resized && cityMode_ && settingsPanel_ && settingsPanel_->isOpen()) {
        settingsPanel_->close();
        openSettingsWin();
        settingsPanel_->openPage("DOWNTOWN");
    }
    if (!saved) {
        std::fprintf(stderr, "%s\n", downtown_->error().c_str());
        if (settingsPanel_) settingsPanel_->setNotice(downtown_->error());
    }
}

void AppShell::evacuateDowntown() {
    if (downtownInterior_.empty()) return;
    if(arcade_)arcade_->leave();
    Vec3 exterior{0,1.7f,96};
    city_->downtownExit(downtownInterior_, exterior);
    downtownInterior_.clear();
    releaseGameplayControl();
    for (auto& [id, entry] : gameplay_) {
        gameplayClicks_.forget(entry->owner);
        entry->inputState.reset();
        entry->controllerInput.reset();
    }
    input_.capturing = false;
    if (settingsPanel_) settingsPanel_->releaseButton();
    riding_ = false;
    knock_ = {};
    camera_.position = exterior;
    camera_.yaw = kPi;
    camera_.pitch = 0;
    flying_ = false;
    vy_ = 0;
    cityMode_ = true;
    clearRoomPending_ = true;
    room_->setCardsVisible(false);
    menu_->setStatus("DOWNTOWN - safely outside");
}

void AppShell::visitDowntown(const std::string& id) {
    if (!settings_.showCity) { settingsPanel_->setNotice("TURN ON CITY IN GRAPHICS FIRST"); return; }
    evacuateSettingsBuilding();
    evacuateDowntown();
    Vec3 exterior;
    if (!city_->downtownExit(id, exterior)) { settingsPanel_->setNotice("DOWNTOWN BUILDING UNAVAILABLE"); return; }
    if (!cityMode_) toggleCity(true);
    releaseGameplayControl();
    if (driving_) toggleCar();
    riding_ = false;
    knock_ = {};
    camera_.position = exterior; camera_.yaw = kPi; camera_.pitch = 0;
    vy_ = 0; flying_ = false;
    settingsPanel_->close();
    focus_ = {};
    menu_->setStatus("DOWNTOWN - aim at the entrance and click");
}

void AppShell::moveDowntown() {
    evacuateSettingsBuilding();
    evacuateDowntown();
    if (cityMode_) {
        releaseGameplayControl();
        if (driving_) toggleCar();
        riding_ = false;
        knock_ = {};
        camera_.position = {0,1.7f,96};
        city_->resolve(camera_.position, 0.8f, 1.7f);
        camera_.position.y = city_->floorAt(camera_.position, 0.8f, 1.7f)+1.7f;
        camera_.yaw = kPi; camera_.pitch = 0;
        flying_ = false; vy_ = 0;
        settingsPanel_->close();
        openSettingsWin();
        settingsPanel_->openPage("DOWNTOWN");
    }
    if (!downtown_->move(downtownSelected_, downtownTarget_)) {
        settingsPanel_->setNotice(downtown_->error()); return;
    }
    syncDowntown();
    settingsPanel_->setNotice("DOWNTOWN ARRANGEMENT SAVED");
}

void AppShell::enterDowntown(const std::string& id) {
    for (const auto& p : plugins_.plugins()) {
        if (p.id != id || (!installedPluginIds_.count(id) && !downtownBuiltin(id))) continue;
        releaseGameplayControl();
        if (driving_) toggleCar();
        riding_ = false;
        knock_ = {};
        toggleCity(false);
        downtownInterior_ = id;
        clearRoomPending_ = false;
        if(id==kDowntownArcadeId && p.running && arcade_->running()) {
            room_->loadArcade(arcade_->screens(),[this](int i){selectArcade(i);},
                [this,id]{togglePlugin(id);},[this,id]{
                    evacuateDowntown(); downtownSelected_=id; downtownTarget_=downtown_->placements().at(id);
                    openSettingsWin(); settingsPanel_->openPage("DOWNTOWN");
                });
            camera_.position={0,1.7f,3}; camera_.yaw=camera_.pitch=0;
            vy_=0; flying_=false; room_->setEye(camera_.position);
            menu_->setStatus("NEON CIRCUIT - select a cabinet; stairs on right; ESC exits");
            return;
        }
        room_->loadPlugin(p.name, {p.id, "VERSION " + p.version, p.description,
                          p.running ? "ON" : (p.enabled ? "FAILED - RETRY OR DISABLE" : "OFF"),
                          downtownBuiltin(id) ? "Bundled native plugin - cannot uninstall; ESC exits" :
                                                "Host managed room - ESC exits"},
            [this, id] { togglePlugin(id); },
            downtownBuiltin(id) ? std::function<void()>{} : std::function<void()>{[this, id] { removePlugin(id); }},
            [this, id] {
                evacuateDowntown();
                downtownSelected_ = id;
                downtownTarget_ = downtown_->placements().at(id);
                openSettingsWin(); settingsPanel_->openPage("DOWNTOWN");
            });
        camera_.position = {0,1.7f,2}; camera_.yaw = camera_.pitch = 0;
        vy_ = 0; flying_ = false;
        room_->setEye(camera_.position);
        menu_->setStatus("INSIDE " + p.name + " - host plugin controls; ESC exits");
        return;
    }
    settingsPanel_->setNotice("PLUGIN BUILDING UNAVAILABLE");
}

void AppShell::togglePlugin(const std::string& id) {
    const bool inside = !downtownInterior_.empty() || !settingsInterior_.empty();
    evacuateSettingsBuilding();
    evacuateDowntown();
    if (inside) { settingsPanel_->close(); openSettingsWin(); settingsPanel_->openPage("PLUGINS"); }
    if (gameplay_.count(id)) { toggleGameplay(id); return; }
    for (const auto& p : plugins_.plugins()) if (p.id == id) {
        if (!plugins_.setEnabled(id, !p.enabled)) settingsPanel_->setNotice(plugins_.error());
        else {
            if (!p.enabled && pluginScripts_.count(id)) pluginMessages_.erase(id);
            settingsPanel_->setNotice(p.running ? "PLUGIN ENABLED" : "PLUGIN DISABLED");
        }
        return;
    }
    settingsPanel_->setNotice("PLUGIN UNAVAILABLE");
}

void AppShell::removePlugin(const std::string& id) {
    if (downtownBuiltin(id)) { settingsPanel_->setNotice("BUNDLED PLUGIN - CANNOT UNINSTALL"); return; }
    if (!installedPluginIds_.count(id)) { settingsPanel_->setNotice("PACKAGE UNAVAILABLE"); return; }
    const bool inside = !downtownInterior_.empty() || !settingsInterior_.empty();
    evacuateSettingsBuilding();
    evacuateDowntown();
    if (inside) { settingsPanel_->close(); openSettingsWin(); settingsPanel_->openPage("PLUGINS"); }
    if (uninstallConfirm_ != id || std::chrono::steady_clock::now() >= uninstallDeadline_) {
        uninstallConfirm_ = id;
        uninstallDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        settingsPanel_->setNotice("CLICK REMOVE AGAIN WITHIN 15 SECONDS");
        return;
    }
    uninstallConfirm_.clear();
    Vec3 exterior;
    if (cityMode_ && city_->downtownExit(id, exterior)) {
        const float dx = camera_.position.x-exterior.x, dz = camera_.position.z-(exterior.z+12);
        if (std::fabs(dx) < 10 && std::fabs(dz) < 10) {
            releaseGameplayControl();
            if (driving_) toggleCar();
            riding_ = false;
            knock_ = {};
            camera_.position = exterior;
            flying_ = false; vy_ = 0;
        }
    }
    if (!plugins_.setEnabled(id, false)) { settingsPanel_->setNotice(plugins_.error()); return; }
    if (gameplay_.count(id)) {
        std::string error;
        if (!gameplayConsent_->revoke(id, error)) { settingsPanel_->setNotice(error); return; }
    }
    if (!pluginPackages_->uninstall(id)) { settingsPanel_->setNotice(pluginPackages_->error()); return; }
    if (!plugins_.remove(id)) { settingsPanel_->setNotice(plugins_.error()); return; }
    gameplay_.erase(id);
    pluginScripts_.erase(id);
    pluginMessages_.erase(id);
    installedPluginIds_.erase(id);
    syncDowntown();
    buildSettingsHub();
    settingsPanel_->openPage("PLUGINS");
    settingsPanel_->setNotice(downtown_->error().empty() ? "PLUGIN UNINSTALLED" :
                              "PLUGIN UNINSTALLED; " + downtown_->error());
}
}
