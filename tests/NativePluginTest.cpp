#include "NativePlugin.h"

#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 5) return 1;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    const auto path = std::filesystem::absolute(argv[1]);
    std::string error;
    sf::NativePlugin plugin;
    check(!plugin.start(error), "start requires loaded module");
    check(!plugin.load(path, "incorrect.id", error), "incorrect ID rejected");
    check(plugin.load(path, "test.fixture", error), "load module");
    check(!plugin.load(path, "test.fixture", error), "duplicate load rejected");
    check(plugin.start(error) && plugin.running(), "start module");
    check(plugin.start(error), "idempotent start");
    plugin.stop();
    check(!plugin.running(), "stop module");
    check(plugin.start(error), "restart module");
    plugin.unload();
    check(!plugin.running(), "unload stops module");
    check(plugin.load(path, "test.fixture", error) && plugin.start(error), "load after unload");
    plugin.unload();
    check(!plugin.load(path.parent_path() / "missing.dll", "test.fixture", error), "missing module rejected");
    check(!plugin.load(std::filesystem::absolute(argv[2]), "test.fixture", error), "incompatible ABI rejected");
    check(!plugin.load(std::filesystem::absolute(argv[3]), "test.fixture", error), "failed create rejected");
    check(plugin.load(std::filesystem::absolute(argv[4]), "test.fixture", error), "load failing start fixture");
    check(!plugin.start(error) && !plugin.running(), "failed start remains stopped");
    plugin.unload();
    check(plugin.load(path, "test.fixture", error) && plugin.start(error), "load after failures");
    plugin.unload();
    return failures ? 1 : 0;
}
