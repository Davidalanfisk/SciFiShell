#include "SettingsModel.h"
#include "Downtown.h"

#include <cmath>
#include <iostream>
#include <set>

int main() {
    int failures=0;
    auto check=[&](bool ok,const char* text) { if (!ok) { std::cerr<<text<<'\n'; ++failures; } };
    bool enabled=false; int confirmations=0, committed=0; float level=0.5f;
    sf::SettingsCategory category{"GRAPHICS","Live controls","SHELL",{
        {"TOGGLE","Existing action",[&] { return enabled?"ON":"OFF"; },[&] { enabled=!enabled; }},
        {"READ ONLY","No action",[] { return "INFO"; },nullptr},
        {"CONFIRM","Same two-click gate",nullptr,[&] { if (++confirmations==2) ++committed; }}
    }};
    sf::SettingsItem slider;
    slider.title="LEVEL"; slider.sliderGet=[&] { return level; };
    slider.sliderSet=[&](float v) { level=v; }; slider.lo=0; slider.hi=1;
    category.items.push_back(slider);
    auto room=category;
    room.items[0].act();
    check(category.items[0].value()=="ON","room and menu share the exact action and live value");
    check(!room.items[1].act && !sf::nudgeSetting(room.items[1],1),"read-only values cannot be changed");
    room.items[2].act();
    check(committed==0,"room action retains the first-click confirmation gate");
    category.items[2].act();
    check(committed==1,"room and menu share confirmation state rather than bypassing it");
    check(sf::nudgeSetting(room.items[3],1) && std::fabs(level-0.55f)<0.001f,
          "room sliders use the menu's 5 percent step");
    for (int i=0;i<30;++i) sf::nudgeSetting(room.items[3],1);
    check(level==1,"slider clamps to upper bound");
    for (int i=0;i<30;++i) sf::nudgeSetting(room.items[3],-1);
    check(level==0,"slider clamps to lower bound");
    const auto signature=sf::settingsRoomSignature(category);
    room.items[0].act();
    check(signature==sf::settingsRoomSignature(category),"ordinary value edits do not rebuild or reset the room");
    room.items.push_back({"NEW OPTION","",nullptr,nullptr});
    check(signature!=sf::settingsRoomSignature(room),"changed option taxonomy updates the room");
    std::set<std::string> ids;
    check(sf::settingsHomeColumns(15)==5 && sf::settingsHomeColumns(17)==6,
          "all Settings home cards including the district remain on screen without changing the controls");
    for (const char* name : {"SHELL","GRAPHICS","WINDOWS","PLUGINS","DOWNTOWN","DISPLAY","SOUND",
             "NETWORK","MOUSE","PERSONALISATION","POWER","SYSTEM","MEDIA PLAYER","MEDIA FOLDERS",
             "MEDIA TRACKS","MEDIA QUEUE","SETTINGS DISTRICT"})
        check(ids.insert(sf::settingsGroupId(name)).second,"all actual settings-group IDs are distinct and stable");
    const auto north=sf::downtownRegion({{"settings.shell",0},{"settings.graphics",127}},true);
    check(north.south()==-124 && north.north()==-2188 && north.width==272,
          "northern district grows away from Computer while preserving distant plot gaps");
    check(!north.intersects(0,-95,20),"northern plots stay clear of Computer/CPU");
    check(sf::downtownRegion({},true).depth==80,"empty northern district matches doubled building scale");
    check(sf::downtownPlot(1,true).x-sf::downtownPlot(0,true).x==64 &&
          sf::downtownPlot(0,true).z-sf::downtownPlot(4,true).z==64,
          "Settings plots double their spacing on both axes");
    check(sf::downtownPlot(1).x-sf::downtownPlot(0).x==32 && sf::downtownPlot(0).z==128,
          "Downtown geometry remains unchanged by Settings scale");
    for (int slot=0;slot<sf::kDowntownSlots;++slot) {
        const auto p=sf::downtownPlot(slot,true);
        const auto region=sf::downtownRegion({{"settings.test",slot}},true);
        check(region.intersects(p.x,p.z,28) && region.south()<=-124,
              "northern outline and collision reservation enclose every selectable plot");
    }
    return failures?1:0;
}
