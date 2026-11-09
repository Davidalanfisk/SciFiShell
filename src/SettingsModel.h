#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace sf {
inline int settingsHomeColumns(size_t categories) {
    return std::max(5,int((categories+2)/3));
}

struct SettingsItem {
    std::string title, hint;
    std::function<std::string()> value;
    std::function<void()> act;
    std::function<float()> sliderGet;
    std::function<void(float)> sliderSet;
    float lo = 0.0f, hi = 1.0f;
};

struct SettingsCategory {
    std::string name, subtitle, tag;
    std::vector<SettingsItem> items;
};

inline bool nudgeSetting(const SettingsItem& item, int dir) {
    if (!item.sliderGet || !item.sliderSet) return false;
    const auto get = item.sliderGet;
    const auto set = item.sliderSet;
    set(std::clamp(get() + dir * (item.hi-item.lo)/20.0f, item.lo, item.hi));
    return true;
}

inline std::string settingsGroupId(const std::string& name) {
    std::string id = "settings.";
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') id += char(c-'A'+'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) id += char(c);
        else id += '-';
    }
    return id;
}

inline std::string settingsRoomSignature(const SettingsCategory& category) {
    std::string signature = category.name;
    for (const auto& item : category.items)
        signature += "\n" + item.title + (item.sliderSet ? ":slider" : ":option");
    return signature;
}
}
