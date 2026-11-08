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
    {
        const fs::path parentFile = dir / "not-a-directory";
        { std::ofstream out(parentFile); out << "keep"; }
        sf::PluginManager manager(parentFile / "plugins.ini");
        int prepared = 0, cancelled = 0, started = 0;
        sf::Plugin plugin{"org.test.prepared", "Prepared", "", "1", [&] { ++started; }, [] {}, false};
        plugin.prepare = [&](std::string&) { ++prepared; return true; };
        plugin.cancelPrepare = [&] { ++cancelled; };
        check(manager.add(std::move(plugin)) && manager.initialize(), "prepare rollback setup");
        check(!manager.setEnabled("org.test.prepared", true), "prepared plugin save failure reported");
        check(prepared == 1 && cancelled == 1 && started == 0 && !manager.plugins()[0].enabled,
              "failed save cancels preparation");
        manager.shutdown();
        fs::remove(parentFile);
    }
    {
        fs::remove(file);
        sf::PluginManager manager(file);
        sf::Plugin plugin{"com.example.script", "Script", "", "1", [&] { events.push_back("script start"); },
                          [&] { events.push_back("script stop"); }, false};
        bool ready = false;
        plugin.prepare = [&](std::string& error) { if (!ready) error = "script failed"; return ready; };
        check(manager.add(std::move(plugin)) && manager.initialize(), "script setup");
        check(!manager.setEnabled("com.example.script", true), "failed start rejected");
        check(!manager.plugins()[0].enabled && !manager.plugins()[0].running && !fs::exists(file), "failed start does not persist");
        ready = true;
        check(manager.setEnabled("com.example.script", true) && manager.plugins()[0].running, "successful start");
        check(manager.remove("com.example.script") && manager.plugins().empty(), "remove stops plugin");
        std::ifstream in(file);
        std::string line;
        std::getline(in, line);
        check(line == "com.example.script=0", "removed plugin remains disabled after reinstall");
    }
    {
        { std::ofstream out(file, std::ios::trunc); out << "com.example.script=1\n"; }
        sf::PluginManager manager(file);
        sf::Plugin plugin{"com.example.script", "Script", "", "1", [] {}, [] {}, false};
        plugin.prepare = [](std::string& error) { error = "script failed"; return false; };
        check(manager.add(std::move(plugin)) && manager.initialize(), "startup with failing plugin");
        check(manager.plugins()[0].enabled && !manager.plugins()[0].running && !manager.error().empty(), "startup failure surfaced");
        check(manager.setEnabled("com.example.script", false), "failed plugin can be disabled");
        manager.shutdown();
    }
    {
        fs::remove(file);
        sf::PluginManager manager(file);
        int starts = 0, stops = 0;
        bool ready = true;
        sf::Plugin plugin{"com.example.game", "Game", "", "1", [&] { ++starts; }, [&] { ++stops; }, false};
        plugin.prepare = [&](std::string& error) { if (!ready) error = "still broken"; return ready; };
        check(manager.add(std::move(plugin)) && manager.initialize() &&
              manager.setEnabled("com.example.game", true), "runtime failure setup");
        check(!manager.markFailed("com.example.game", ""), "empty runtime failure rejected");
        check(manager.markFailed("com.example.game", "tick failed") &&
              manager.plugins()[0].enabled && !manager.plugins()[0].running && stops == 1 &&
              manager.error().find("tick failed") != std::string::npos, "runtime failure stops and reports");
        check(!manager.markFailed("com.example.game", "duplicate") && stops == 1, "runtime failure stops once");
        {
            std::ifstream in(file);
            std::string line;
            std::getline(in, line);
            check(line == "com.example.game=1", "runtime failure preserves enabled preference");
        }
        ready = false;
        check(!manager.setEnabled("com.example.game", true) && starts == 1 &&
              manager.error().find("still broken") != std::string::npos, "failed retry remains stopped");
        ready = true;
        check(manager.setEnabled("com.example.game", true) && manager.plugins()[0].running &&
              starts == 2 && manager.error().empty(), "retry restarts enabled failed plugin");
        manager.shutdown();
        check(stops == 2, "retried plugin stops at shutdown");
    }
    {
        { std::ofstream out(file, std::ios::trunc); out << "com.example.bad=1\ncom.example.good=1\n"; }
        sf::PluginManager manager(file);
        sf::Plugin bad{"com.example.bad", "Bad", "", "1", [] {}, [] {}, false};
        bad.prepare = [](std::string& error) { error = "script failed"; return false; };
        sf::Plugin good{"com.example.good", "Good", "", "1", [] {}, [] {}, false};
        good.prepare = [](std::string& error) { error.clear(); return true; };
        check(manager.add(std::move(bad)) && manager.add(std::move(good)) && manager.initialize(), "mixed script startup");
        check(manager.error().find("com.example.bad: script failed") != std::string::npos, "successful start preserves previous error");
        check(!manager.plugins()[0].running && manager.plugins()[1].running, "other scripts still start");
        manager.shutdown();
    }
    {
        { std::ofstream out(file, std::ios::trunc); out << "invalid\n"; }
        sf::PluginManager manager(file);
        sf::Plugin good{"com.example.good", "Good", "", "1", [] {}, [] {}};
        good.prepare = [](std::string& error) { error.clear(); return true; };
        check(manager.add(std::move(good)) && !manager.initialize(), "invalid storage with script");
        check(manager.error().find("preferences are invalid") != std::string::npos, "successful start preserves storage error");
        manager.shutdown();
    }
    {
        { std::ofstream out(file, std::ios::trunc); out << "com.example.script=1\n"; }
        sf::PluginManager manager(file);
        check(manager.initialize(), "initialize before installing retained script");
        sf::Plugin plugin{"com.example.script", "Script", "", "1", [] {}, [] {}, false};
        plugin.prepare = [](std::string& error) { error = "script failed"; return false; };
        check(manager.add(std::move(plugin)), "failed retained script remains registered");
        check(manager.plugins().size() == 1 && manager.plugins()[0].enabled && !manager.plugins()[0].running &&
              manager.error().find("script failed") != std::string::npos, "failed retained script shown as error");
        check(manager.setEnabled("com.example.script", false), "failed retained script can be disabled");
        manager.shutdown();
    }
    fs::remove(file);
    fs::remove(dir);
    return failures == 0 ? 0 : 1;
}
