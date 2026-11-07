#include "PluginPackage.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    const std::string good = R"({"id":"com.example.twilight","name":"TWILIGHT","version":"1.0.0","author":"Example","description":"A violet theme","type":"theme","platforms":["any"],"permissions":[],"accent":"#aa22ff","alt":"#12ccdd"})";
    sf::ThemePackage p;
    std::string error;
    check(sf::PluginPackageStore::parse(good, p, error) && p.id == "com.example.twilight", "valid manifest");
    auto bad = [&](std::string s, const std::string& from, const std::string& to) {
        const auto pos = s.find(from);
        if (pos == std::string::npos) { ++failures; return; }
        s.replace(pos, from.size(), to);
        check(!sf::PluginPackageStore::parse(s, p, error), "invalid manifest accepted");
    };
    bad(good, "\"theme\"", "\"native-module\"");
    bad(good, "\"permissions\":[]", "\"permissions\":[\"network\"]");
    bad(good, "com.example.twilight", "../escape");
    bad(good, "com.example.twilight", "org.scifishell.driving");
    bad(good, "#aa22ff", "#oops");
    bad(good, "1.0.0", "1.0");
    bad(good, "\"name\":\"TWILIGHT\"", "\"name\":\"A\",\"name\":\"B\"");
    bad(good, "\"platforms\":[\"any\"]", "\"platforms\":[\"windows\"]");
    check(!sf::PluginPackageStore::parse(good + "garbage", p, error), "trailing data accepted");
    check(!sf::PluginPackageStore::parse(good.substr(0, good.size() - 1), p, error), "truncated data accepted");
    check(!sf::PluginPackageStore::parse(std::string(17000, 'x'), p, error), "oversize accepted");
    const fs::path dir = fs::temp_directory_path() / ("scifi-package-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    const fs::path source = dir / "source.sfplugin", installed = dir / "installed";
    { std::ofstream out(source); out << good; }
    std::set<std::string> ids;
    sf::PluginPackageStore store(installed, [&](const sf::ThemePackage& pkg) { return ids.insert(pkg.id).second; });
    store.discover();
    check(store.error().empty(), "missing directory discovery");
    check(store.install(source), "install");
    check(ids.count("com.example.twilight") == 1, "register installed plugin");
    check(!store.install(source), "duplicate install rejected");
    ids.clear();
    sf::PluginPackageStore restarted(installed, [&](const sf::ThemePackage& pkg) { return ids.insert(pkg.id).second; });
    restarted.discover();
    check(restarted.error().empty() && ids.count("com.example.twilight"), "restart discovery");
    { std::ofstream out(installed / "wrong.sfplugin"); out << good; }
    ids.clear();
    restarted.discover();
    check(!restarted.error().empty() && ids.count("com.example.twilight"), "bad filename reported without hiding good package");
    fs::remove(installed / "wrong.sfplugin");
    fs::remove(installed / "com.example.twilight.sfplugin");
    fs::remove(installed);
    fs::remove(source);
    fs::remove(dir);
    return failures ? 1 : 0;
}
