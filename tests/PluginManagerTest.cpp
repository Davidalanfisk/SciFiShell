#include "PluginManager.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("scifishell-plugin-test-" + std::to_string(suffix));
    if (!fs::create_directory(dir)) return 1;
    const fs::path file = dir / "plugins.ini";
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    std::vector<std::string> events;
    auto addPlugins = [&](sf::PluginManager& manager) {
        check(manager.add({"org.test.car", "Car", "", "1", [&] { events.push_back("car start"); }, [&] { events.push_back("car stop"); }}), "register car");
        check(manager.add({"org.test.park", "Park", "", "1", [&] { events.push_back("park start"); }, [&] { events.push_back("park stop"); }}), "register park");
    };
    {
        sf::PluginManager manager(file);
        addPlugins(manager);
        check(!manager.add({"org.test.car", "duplicate", "", "1", [] {}, [] {}}), "duplicate rejected");
        check(!manager.add({"bad/id", "invalid", "", "1", [] {}, [] {}}), "invalid id rejected");
        check(manager.plugins().size() == 2, "discovery");
        check(manager.initialize(), "initialization");
        check(events == std::vector<std::string>({"car start", "park start"}), "default start order");
        check(manager.add({"org.test.theme", "Theme", "", "1", [&] { events.push_back("theme start"); },
                           [&] { events.push_back("theme stop"); }, false}), "register after initialization");
        check(!manager.plugins().back().running, "installed plugin starts disabled");
        check(manager.setEnabled("org.test.car", false), "disable");
        check(!manager.plugins()[0].running && !manager.plugins()[0].enabled, "car stopped");
        check(manager.setEnabled("org.test.car", false), "disable is idempotent");
        check(events.size() == 3 && events.back() == "car stop", "stop once");
        check(!manager.setEnabled("unknown", true), "unknown id rejected");
        manager.shutdown();
        manager.shutdown();
        check(events.size() == 4 && events.back() == "park stop", "shutdown is idempotent");
        check(!manager.setEnabled("org.test.car", true), "cannot restart after shutdown");
    }
    events.clear();
    {
        std::ofstream out(file, std::ios::app);
        out << "org.test.future=0\n";
    }
    {
        sf::PluginManager manager(file);
        addPlugins(manager);
        check(manager.initialize(), "restore initialization");
        check(events == std::vector<std::string>({"park start"}), "disabled state restored");
        check(manager.setEnabled("org.test.car", true), "enable");
        check(manager.plugins()[0].running, "car restarted");
        check(manager.setEnabled("org.test.park", false), "disable other plugin");
        manager.shutdown();
    }
    {
        std::ifstream in(file);
        std::string contents((std::istreambuf_iterator<char>(in)), {});
        check(contents.find("org.test.car=1") != std::string::npos, "enabled state saved");
        check(contents.find("org.test.park=0") != std::string::npos, "disabled state saved");
        check(contents.find("org.test.future=0") != std::string::npos, "unknown plugin state retained");
    }
    events.clear();
    {
        sf::PluginManager manager(file);
        addPlugins(manager);
        check(manager.initialize(), "second restore initialization");
        check(events == std::vector<std::string>({"car start"}), "enabled and disabled states restored");
        check(manager.setEnabled("org.test.park", true), "re-enable park");
        manager.shutdown();
        check(events == std::vector<std::string>({"car start", "park start", "park stop", "car stop"}), "reverse shutdown order");
    }
    {
        std::ofstream out(file, std::ios::trunc);
        out << "malformed preferences\n";
    }
    {
        sf::PluginManager manager(file);
        addPlugins(manager);
        check(!manager.initialize() && !manager.error().empty(), "invalid storage reported");
        check(!manager.setEnabled("org.test.car", false), "invalid storage cannot be overwritten");
        manager.shutdown();
        std::ifstream in(file);
        std::string line;
        std::getline(in, line);
        check(line == "malformed preferences", "invalid storage preserved");
    }
    {
        sf::PluginManager manager({});
        addPlugins(manager);
        check(!manager.initialize() && !manager.error().empty(), "missing storage path reported");
        check(!manager.setEnabled("org.test.car", false), "missing storage refuses changes");
        manager.shutdown();
    }
    {
        const fs::path parentFile = dir / "not-a-directory";
        { std::ofstream out(parentFile); out << "keep"; }
        sf::PluginManager manager(parentFile / "plugins.ini");
        addPlugins(manager);
        check(manager.initialize(), "missing preference file starts normally");
        check(!manager.setEnabled("org.test.car", false) && !manager.error().empty(), "unwritable parent reported");
        check(manager.plugins()[0].running && manager.plugins()[0].enabled, "failed save leaves plugin running");
        manager.shutdown();
        fs::remove(parentFile);
    }
    fs::remove(file);
    fs::remove(dir);
    return failures == 0 ? 0 : 1;
}
