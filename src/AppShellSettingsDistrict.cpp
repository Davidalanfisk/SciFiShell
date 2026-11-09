#include "AppShell.h"

#include <cstdio>

namespace sf {
const SettingsCategory* AppShell::settingsGroup(const std::string& id) const {
    for (const auto& category : settingsCategories_)
        if (settingsGroupId(category.name)==id) return &category;
    return nullptr;
}

void AppShell::syncSettingsDistrict() {
    if (!settingsDistrict_) return;
    std::set<std::string> ids;
    std::vector<std::pair<std::string,std::string>> names;
    for (const auto& category : settingsCategories_) {
        const auto id=settingsGroupId(category.name);
        ids.insert(id); names.emplace_back(id,category.name);
    }
    const bool saved=settingsDistrict_->reconcile(ids);
    const bool resized=!(downtownRegion(settingsDistrict_->placements(),true)==city_->settingsDistrictBounds());
    if (resized) {
        evacuateSettingsBuilding();
        evacuateDowntown();
        releaseGameplayControl();
        if (driving_) toggleCar();
        riding_=false; knock_={}; carOn_=false;
        if (cityMode_) { camera_.position={0,1.7f,-110}; flying_=false; vy_=0; }
        if (cityCamSet_) savedCam_.position={0,1.7f,-110};
    }
    city_->setSettingsDistrict(settingsDistrict_->placements(),names);
    if (!saved) {
        std::fprintf(stderr,"Settings district: %s\n",settingsDistrict_->error().c_str());
        settingsPanel_->setNotice("SETTINGS DISTRICT: "+settingsDistrict_->error());
    }
}

void AppShell::evacuateSettingsBuilding() {
    if (settingsInterior_.empty()) return;
    Vec3 exterior{0,1.7f,-110};
    city_->settingsBuildingExit(settingsInterior_,exterior);
    settingsInterior_.clear();
    settingsRoomSignature_.clear();
    releaseGameplayControl();
    for (auto& [id,entry] : gameplay_) {
        gameplayClicks_.forget(entry->owner);
        entry->inputState.reset(); entry->controllerInput.reset();
    }
    input_.capturing=false;
    settingsPanel_->releaseButton();
    riding_=false; knock_={};
    camera_.position=exterior; camera_.yaw=0; camera_.pitch=0;
    flying_=false; vy_=0; cityMode_=true; clearRoomPending_=true;
    room_->setCardsVisible(false);
    powerArmed_=0;
    menu_->setStatus("SETTINGS DISTRICT - outside");
}

void AppShell::visitSettingsBuilding(const std::string& id) {
    if (!settings_.showCity) { settingsPanel_->setNotice("TURN ON CITY IN GRAPHICS FIRST"); return; }
    evacuateSettingsBuilding(); evacuateDowntown();
    Vec3 exterior;
    if (!city_->settingsBuildingExit(id,exterior)) { settingsPanel_->setNotice("SETTINGS BUILDING UNAVAILABLE"); return; }
    if (!cityMode_) toggleCity(true);
    releaseGameplayControl();
    if (driving_) toggleCar();
    riding_=false; knock_={};
    camera_.position=exterior; camera_.yaw=0; camera_.pitch=0;
    flying_=false; vy_=0;
    settingsPanel_->close(); focus_={};
    menu_->setStatus("SETTINGS DISTRICT - select the building door");
}

void AppShell::moveSettingsBuilding() {
    evacuateSettingsBuilding(); evacuateDowntown();
    releaseGameplayControl();
    if (driving_) toggleCar();
    riding_=false; knock_={}; carOn_=false;
    if (cityMode_) { camera_.position={0,1.7f,-110}; camera_.yaw=0; camera_.pitch=0; flying_=false; vy_=0; }
    if (!settingsDistrict_->move(settingsSelected_,settingsTarget_)) {
        settingsPanel_->setNotice(settingsDistrict_->error()); return;
    }
    syncSettingsDistrict();
    settingsPanel_->close(); openSettingsWin(); settingsPanel_->openPage("SETTINGS DISTRICT");
    settingsPanel_->setNotice("SETTINGS BUILDING ARRANGEMENT SAVED");
}

void AppShell::enterSettingsBuilding(const std::string& id) {
    buildSettingsHub();
    const auto* group=settingsGroup(id);
    if (!group || !settingsDistrict_->placements().count(id)) {
        settingsPanel_->setNotice("SETTINGS GROUP UNAVAILABLE"); return;
    }
    evacuateSettingsBuilding(); evacuateDowntown();
    releaseGameplayControl();
    if (driving_) toggleCar();
    riding_=false; knock_={};
    toggleCity(false);
    settingsPanel_->close();
    settingsPanel_->setNotice("");
    focus_={}; powerArmed_=0;
    settingsInterior_=id;
    settingsRoomSignature_.clear();
    clearRoomPending_=false;
    settingsRoomRefresh_=true;
    refreshSettingsRoom();
    camera_.position={0,1.7f,4}; camera_.yaw=camera_.pitch=0;
    flying_=false; vy_=0;
    room_->setEye(camera_.position);
    menu_->setStatus("INSIDE "+group->name+" - explore the consoles; ESC exits");
}

void AppShell::invokeRoomSetting(size_t index, int direction) {
    const auto* category=settingsGroup(settingsInterior_);
    if (!category || index>=category->items.size()) { settingsPanel_->setNotice("SETTING NO LONGER AVAILABLE"); return; }
    const auto item=category->items[index];
    const auto categoryName=category->name;
    if (direction) {
        if (!nudgeSetting(item,direction)) settingsPanel_->setNotice("SETTING IS NOT ADJUSTABLE");
    } else if (item.act) {
        const auto action=item.act;
        action();
    }
    // Keep long permission/error notices in the established readable confirmation surface.
    if (!settingsPanel_->notice().empty() && !settingsPanel_->isOpen()) {
        const auto notice=settingsPanel_->notice();
        openSettingsWin();
        settingsPanel_->openPage(categoryName);
        settingsPanel_->setNotice(notice);
    }
    settingsRoomRefresh_=true;
}

void AppShell::refreshSettingsRoom() {
    settingsRoomRefresh_=false;
    if (settingsInterior_.empty()) return;
    const auto* group=settingsGroup(settingsInterior_);
    if (!group) { evacuateSettingsBuilding(); return; }
    const auto signature=settingsRoomSignature(*group);
    if (signature==settingsRoomSignature_) return;
    const auto category=*group;
    settingsRoomSignature_=signature;
    room_->loadSettings(category,
        [this](size_t item,int direction) { invokeRoomSetting(item,direction); },
        [this](size_t index) {
            const auto* current=settingsGroup(settingsInterior_);
            if (!current || index>=current->items.size()) return std::vector<std::string>{"UNAVAILABLE"};
            const auto item=current->items[index];
            std::vector<std::string> text{item.value ? item.value() : std::string(),item.hint};
            if (!settingsPanel_->notice().empty()) text.push_back(settingsPanel_->notice());
            else text.push_back(item.sliderSet ? "Select +5%; +/- or left/right" : item.act ? "SELECT TO ACTIVATE" : "LIVE INFORMATION");
            return text;
        },
        [this] { return settingsPanel_->notice(); });
    room_->resolve(camera_.position);
    camera_.position.y=1.7f; flying_=false; vy_=0;
    room_->setEye(camera_.position);
}
}
