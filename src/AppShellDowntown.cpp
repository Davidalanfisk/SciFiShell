#include "AppShell.h"

#include "GameplayConsent.h"
#include "PluginWorkspace.h"

#include <cstdio>
#include <fstream>

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
        PluginRoomSpec spec;
        spec.details = {p.id, "VERSION " + p.version, p.description,
                        p.running ? "ON" : (p.enabled ? "FAILED - RETRY OR DISABLE" : "OFF"),
                        downtownBuiltin(id) ? "Bundled native plugin - cannot uninstall; ESC exits" :
                                              "Host managed room - ESC exits"};
        spec.toggle = [this, id] { togglePlugin(id); };
        if (!downtownBuiltin(id)) spec.remove = [this, id] { removePlugin(id); };
        spec.arrange = [this, id] {
            evacuateDowntown();
            downtownSelected_ = id;
            downtownTarget_ = downtown_->placements().at(id);
            openSettingsWin(); settingsPanel_->openPage("DOWNTOWN");
        };
        spec.filesInfo = [this, id] { return pluginFilesInfo(id); };
        if (const auto entry = gameplay_.find(id); entry != gameplay_.end()) {
            spec.browse = [this, id] { browsePluginFiles(id); };
            spec.apply = [this, id] { applyPluginEdits(id); };
            spec.model = entry->second->roomModel;
            for (size_t i = 0; i < entry->second->settings.size(); ++i)
                spec.settings.push_back({entry->second->settings[i].label,
                                         [this, id, i] { return pluginSettingLabel(id, i); },
                                         [this, id, i](int direction) { changePluginSetting(id, i, direction); }});
        }
        room_->loadPlugin(p.name, std::move(spec));        camera_.position = {0,1.7f,4}; camera_.yaw = camera_.pitch = 0;
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
        const float dx = camera_.position.x-exterior.x, dz = camera_.position.z-(exterior.z+13);
        if (std::fabs(dx) < 12 && std::fabs(dz) < 12) {
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
    {
        std::error_code ec;
        const auto settings = pluginStateDirectory("plugin-settings");
        const auto workspaces = pluginStateDirectory("plugin-workspaces");
        if (!settings.empty()) std::filesystem::remove(settings / (id + ".txt"), ec);
        if (!workspaces.empty() && std::filesystem::symlink_status(workspaces / id, ec).type() ==
                                       std::filesystem::file_type::directory)
            std::filesystem::remove_all(workspaces / id, ec);
    }
    gameplay_.erase(id);
    pluginScripts_.erase(id);
    pluginFilesStatus_.erase(id);
    pluginMessages_.erase(id);
    installedPluginIds_.erase(id);
    syncDowntown();
    buildSettingsHub();
    settingsPanel_->openPage("PLUGINS");
    settingsPanel_->setNotice(downtown_->error().empty() ? "PLUGIN UNINSTALLED" :
                              "PLUGIN UNINSTALLED; " + downtown_->error());
}
std::filesystem::path AppShell::pluginStateDirectory(const char* leaf) const {
    const auto state = PluginManager::defaultStatePath();
    return state.empty() ? std::filesystem::path{} : state.parent_path() / leaf;
}

void AppShell::loadPluginSettings(const std::string& id, GameplayEntry& entry) {
    const auto dir = pluginStateDirectory("plugin-settings");
    const auto stored = dir.empty() ? std::map<std::string, std::string>{} : sf::loadPluginSettings(dir / (id + ".txt"));
    entry.settingValues.clear();
    for (const auto& setting : entry.settings) {
        std::string value = setting.choices.front().first;
        const auto it = stored.find(setting.key);
        if (it != stored.end())
            for (const auto& choice : setting.choices) if (choice.first == it->second) value = it->second;
        entry.settingValues.emplace_back(setting.key, value);
    }
}

std::string AppShell::pluginSettingLabel(const std::string& id, size_t index) const {
    const auto it = gameplay_.find(id);
    if (it == gameplay_.end() || index >= it->second->settings.size() || index >= it->second->settingValues.size())
        return "UNAVAILABLE";
    const auto& setting = it->second->settings[index];
    for (size_t i = 0; i < setting.choices.size(); ++i)
        if (setting.choices[i].first == it->second->settingValues[index].second)
            return setting.choices[i].second + "  (" + std::to_string(i + 1) + "/" +
                   std::to_string(setting.choices.size()) + ")";
    return "UNAVAILABLE";
}

void AppShell::changePluginSetting(const std::string& id, size_t index, int direction) {
    const auto it = gameplay_.find(id);
    if (it == gameplay_.end() || index >= it->second->settings.size() || index >= it->second->settingValues.size())
        return;
    auto& entry = *it->second;
    const auto& setting = entry.settings[index];
    const int count = static_cast<int>(setting.choices.size());
    int current = 0;
    for (int i = 0; i < count; ++i)
        if (setting.choices[i].first == entry.settingValues[index].second) current = i;
    const int next = ((current + (direction < 0 ? -1 : 1)) % count + count) % count;
    entry.settingValues[index].second = setting.choices[next].first;
    std::string status = setting.label + ": " + setting.choices[next].second;
    const auto dir = pluginStateDirectory("plugin-settings");
    std::string saveError = "no state folder";
    if (dir.empty() || !sf::savePluginSettings(dir / (id + ".txt"), entry.settingValues, saveError))
        status += " (NOT SAVED: " + saveError + ")";
    if (entry.session.running()) {
        std::string error;
        if (!entry.session.setting(setting.key, setting.choices[next].first, error)) {
            plugins_.markFailed(id, error);
            status = "SETTING FAILED: " + error;
        }
    }
    menu_->setStatus(status);
}

std::vector<std::string> AppShell::pluginFilesInfo(const std::string& id) const {
    if (downtownBuiltin(id))
        return {"Bundled native plugin - not editable", "Compiled code; no package files to browse"};
    if (!gameplay_.count(id))
        return {"Theme or script package - not editable", "In-shell editing supports gameplay ZIPs only"};
    const auto status = pluginFilesStatus_.find(id);
    return {status == pluginFilesStatus_.end() ? "Select to open an editable copy" : status->second,
            "Edits stay in plugin-workspaces/" + id, "APPLY EDITS validates + reinstalls",
            "Changed code needs fresh consent"};
}

void AppShell::browsePluginFiles(const std::string& id) {
    const auto it = gameplay_.find(id);
    const auto dir = pluginStateDirectory("plugin-workspaces");
    if (it == gameplay_.end() || dir.empty()) {
        menu_->setStatus(downtownBuiltin(id) ? "NATIVE PLUGIN - NO EDITABLE FILES"
                                             : "NOT EDITABLE IN-SHELL - GAMEPLAY ZIPS ONLY");
        return;
    }
    bool created = false;
    std::string error;
    if (!extractGameplayWorkspace(it->second->path, dir / id, created, error)) {
        pluginFilesStatus_[id] = "EDIT COPY FAILED: " + error;
        menu_->setStatus(pluginFilesStatus_[id]);
        return;
    }
    pluginFilesStatus_[id] = created ? "EDIT COPY CREATED" : "EDIT COPY OPENED (KEEPS YOUR EDITS)";
    if (files2d_ && files2d_->openRoom) files2d_->openRoom((dir / id).u8string());
}

void AppShell::applyPluginEdits(const std::string& id) {
    const auto it = gameplay_.find(id);
    const auto dir = pluginStateDirectory("plugin-workspaces");
    std::error_code ec;
    if (it == gameplay_.end() || dir.empty() ||
        std::filesystem::symlink_status(dir / id, ec).type() != std::filesystem::file_type::directory) {
        pluginFilesStatus_[id] = "NO EDIT COPY - BROWSE FILES FIRST";
        menu_->setStatus(pluginFilesStatus_[id]);
        return;
    }
    const auto staged = dir / (id + ".apply.zip");
    std::filesystem::remove(staged, ec);
    std::string error;
    GameplayDigest digest;
    if (!packGameplayWorkspace(dir / id, staged, error) || !gameplayDigestFile(staged, digest, error)) {
        pluginFilesStatus_[id] = "EDITS NOT APPLIED: " + error;
        menu_->setStatus(pluginFilesStatus_[id]);
        return;
    }
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove(path, e); } }
        cleanup{staged};
    if (digest == it->second->digest) {
        pluginFilesStatus_[id] = "NO CHANGES TO APPLY";
        menu_->setStatus(pluginFilesStatus_[id]);
        return;
    }
    // The confirmation is bound to the exact staged digest: edits made between the two clicks need a new confirm.
    if (applyConfirm_ != id || std::chrono::steady_clock::now() >= applyDeadline_ || !(applyDigest_ == digest)) {
        const bool changed = applyConfirm_ == id && std::chrono::steady_clock::now() < applyDeadline_;
        applyConfirm_ = id;
        applyDigest_ = digest;
        applyDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        pluginFilesStatus_[id] = changed ? "FILES CHANGED - SELECT APPLY AGAIN WITHIN 15 S"
                                         : "SELECT APPLY AGAIN WITHIN 15 S TO REINSTALL";
        menu_->setStatus(pluginFilesStatus_[id] + " - plugin stops and asks for consent again");
        return;
    }
    applyConfirm_.clear();
    // Never hot-reload under the old grant: stop, revoke consent, then swap the validated package.
    if (!plugins_.setEnabled(id, false) ||
        !gameplayConsent_->revoke(id, error)) {
        pluginFilesStatus_[id] = "EDITS NOT APPLIED: " + (error.empty() ? plugins_.error() : error);
        menu_->setStatus(pluginFilesStatus_[id]);
        return;
    }
    const bool replaced = pluginPackages_->replaceGameplay(id, staged, [this, id] {
        plugins_.remove(id);
        gameplay_.erase(id);
        installedPluginIds_.erase(id);
        city_->clearDowntownModel(id);
    });
    pluginFilesStatus_[id] = replaced ? "EDITS INSTALLED - TURN ON TO REVIEW CONSENT" :
                                        "EDITS REJECTED: " + pluginPackages_->error();
    menu_->setStatus(pluginFilesStatus_[id]);
    if (deferred_) return;
    deferred_ = [this, id] {
        syncDowntown();
        if (downtownInterior_ == id) enterDowntown(id);
    };
    deferDelay_ = 3;
}
}
