#include "Theme.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    const fs::path dir = fs::temp_directory_path() / ("scifi-theme-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(dir)) return 1;
#ifdef _WIN32
    if (_putenv_s("LOCALAPPDATA", dir.string().c_str()) != 0) return 1;
#else
    if (setenv("LOCALAPPDATA", dir.string().c_str(), 1) != 0) return 1;
#endif
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    sf::registerPluginTheme("com.example.one", "CYAN", {1, 0, 0}, {0, 1, 0});
    sf::registerPluginTheme("com.example.two", "CYAN", {0, 1, 0}, {1, 0, 0});
    check(sf::currentThemeKey() == "0", "default theme");
    sf::cycleTheme();
    sf::cycleTheme();
    sf::cycleTheme();
    check(sf::currentThemeKey() == "plugin:com.example.one" && sf::currentTheme().accent.x == 1, "first same-name theme");
    sf::cycleTheme();
    check(sf::currentThemeKey() == "plugin:com.example.two" && sf::currentTheme().accent.y == 1, "second same-name theme");
    sf::unregisterPluginTheme("com.example.two");
    check(sf::currentThemeKey() == "0", "disabled selected theme falls back");
    sf::registerPluginTheme("com.example.two", "CYAN", {0, 1, 0}, {1, 0, 0});
    check(sf::currentThemeKey() == "plugin:com.example.two", "re-enabled theme restores selection");
    sf::unregisterPluginTheme("com.example.two");
    sf::cycleTheme();
    check(sf::currentThemeKey() == "1", "cycling from fallback advances to amber");
    sf::unregisterPluginTheme("com.example.one");
    fs::remove(dir / "SciFiShell" / "theme.ini");
    fs::remove(dir / "SciFiShell");
    fs::remove(dir);
    return failures ? 1 : 0;
}
