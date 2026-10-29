#pragma once
// Native Windows OS settings and information used by the settings hub (no hosted Windows pages needed).
#include <string>
#include <utility>
#include <vector>

namespace sf::os {

// Sound: master volume 0..1 (-1 when no audio endpoint) and mute.
float getVolume();
void setVolume(float v);
bool getMute();
void setMute(bool m);

// Mouse.
int mouseSpeed();  // 1..20
void setMouseSpeed(int v);
bool mouseSwapped();
void setMouseSwapped(bool swapped);

// Personalisation: Windows app + system colour mode.
bool darkMode();
void setDarkMode(bool dark);

// Power.
void lockPc();
void sleepPc();
void powerOff(bool reboot);  // shut down or restart Windows
std::string batteryText();

// Read-only information.
std::string computerName();
std::string windowsName();
std::string cpuName();
std::string memoryText();
std::string uptimeText();
std::string timeText();
std::string displayText();
std::string scalingText();
std::string monitorCount();
std::vector<std::pair<std::string, std::string>> drives();    // {"C:", "120 GB free of 500 GB"}
struct Adapter {
    std::string kind, ip, detail;
};
std::vector<Adapter> adapters();

}  // namespace sf::os
