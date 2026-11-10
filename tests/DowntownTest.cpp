#include "Downtown.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("scifi-downtown-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    const auto file = root / "downtown.ini";
    sf::Downtown district(file);
    check(district.load(), "new profile loads");
    check(district.reconcile({"z.plugin", "a.plugin"}), "initial allocation saves");
    check(district.placements().at("a.plugin") == 0 && district.placements().at("z.plugin") == 1,
          "initial allocation sorted by stable ID");
    check(district.move("a.plugin", 10), "move into empty plot");
    check(district.move("z.plugin", 10), "swap occupied plot");
    check(district.placements().at("a.plugin") == 1 && district.placements().at("z.plugin") == 10,
          "swap is atomic and preserves both plugins");
    check(district.reconcile({"a.plugin", "z.plugin", "b.plugin"}), "new install");
    check(district.placements().at("b.plugin") == 0 && district.placements().at("z.plugin") == 10,
          "installs fill gaps without disturbing arrangements");
    const auto installed = district.placements();
    check(district.reconcile({"a.plugin", "b.plugin", "z.plugin"}) && district.placements() == installed,
          "disable, enable, failure and city rebuild retain installed package plots");
    sf::Downtown restart(file);
    check(restart.load() && restart.placements() == installed, "restart preserves exact arrangement");
    check(restart.reconcile({"z.plugin", "b.plugin"}), "uninstall reconciles");
    check(!restart.placements().count("a.plugin") && restart.placements().at("z.plugin") == 10,
          "uninstall releases only removed plot");
    check(restart.reconcile({"z.plugin", "b.plugin", "c.plugin"}) && restart.placements().at("c.plugin") == 1,
          "released plot reused");
    const auto before = restart.placements();
    check(!restart.move("missing", 3) && restart.placements() == before, "unknown move rejected");
    check(!restart.move("z.plugin", sf::kDowntownSlots) && restart.placements() == before, "out of range rejected");
    const auto empty = sf::downtownRegion();
    check(empty.width == 32 && empty.depth == 40 && empty.north() > 75,
          "empty district has a minimal footprint south of Computer");
    const std::map<std::string, int> compact{{sf::kDowntownDrivingId,0}, {sf::kDowntownParksId,1}};
    const auto small = sf::downtownRegion(compact);
    check(small.west() == -68 && small.east() == 4 && small.north() == 108 && small.south() == 148,
          "two bundled buildings have an exact compact outline");
    const auto sparse = sf::downtownRegion({{"one",0}, {"far",127}});
    check(sparse.width == 136 && sparse.depth == 1032 && sparse.south() == 1140,
          "user-selected far plot expands bounds including intentional gaps");
    check(sf::downtownRegion({{"one",0}}).depth == 40 && small.depth == 40,
          "removing the distant occupant shrinks the region without renumbering plots");
    check(!small.intersects(0, 500, 20) && sparse.intersects(0,500,20),
          "unused future rows are not reserved; intentional gaps remain reserved");
    check(sf::downtownBuiltin(sf::kDowntownDrivingId) && sf::downtownBuiltin(sf::kDowntownParksId) &&
          sf::downtownBuiltin(sf::kDowntownArcadeId) &&
          !sf::downtownBuiltin("com.example.plugin"), "only bundled IDs have protected management");
    for (int i = 0; i < sf::kDowntownSlots; ++i) {
        const auto p = sf::downtownPlot(i);
        const auto region = sf::downtownRegion({{"occupant",i}});
        check(p.z-20 > 75 && region.width == 40 && region.depth == 40,
              "any single occupied plot has a compact footprint south of Computer");
        check(region.intersects(p.x, p.z, 14), "drive packing excludes each occupied plot");
        check(p.x-14 >= region.west() && p.x+14 <= region.east() &&
              p.z-14 >= region.north() && p.z+14 <= region.south(),
              "colored outline encloses the occupied pavement and host entrance");
        for (int j = 0; j < i; ++j) {
            const auto q = sf::downtownPlot(j);
            check(std::abs(p.x-q.x) >= 32 || std::abs(p.z-q.z) >= 32, "plots never overlap");
        }
    }
    check(!small.intersects(0, -55, 20), "Computer not inside Downtown");
    check(!small.intersects(300, 128, 40, 42), "unrelated city space remains available");
    std::set<std::string> many;
    for (int i = 0; i < 64; ++i) many.insert("plugin." + std::to_string(i));
    check(restart.reconcile(many) && restart.placements().size() == 64, "full package-store capacity supported");
    const auto packages = restart.placements();
    many.insert(sf::kDowntownDrivingId); many.insert(sf::kDowntownParksId);many.insert(sf::kDowntownArcadeId);
    check(restart.reconcile(many) && restart.placements().size() == 67, "bundled buildings fit beside full package capacity");
    for (const auto& [id, slot] : packages)
        check(restart.placements().at(id) == slot, "adding native buildings preserves existing package placements");
    const auto withBuiltins = restart.placements();
    check(restart.reconcile(many) && restart.placements() == withBuiltins,
          "disabled and re-enabled native plugins retain their buildings and placements");
    fs::remove(file);
    fs::create_directory(file); // Deterministic failed atomic replacement, even with elevated permissions.
    const auto saved = restart.placements();
    check(!restart.move("plugin.0", 100) && restart.placements() == saved && !restart.error().empty(),
          "failed persistence does not commit a move");
    check(!restart.reconcile({"new.plugin"}) && restart.placements().count("new.plugin"),
          "failed lifecycle save still reconciles accessible live buildings with explicit error");
    check(!restart.reconcile({"new.plugin"}) && !restart.error().empty(),
          "unchanged reconciliation retries unsaved live state instead of claiming persistence");
    fs::remove(file);
    { std::ofstream out(file); out << "a.plugin 1\nb.plugin 1\n"; }
    sf::Downtown corrupt(file);
    check(!corrupt.load() && !corrupt.error().empty(), "duplicate persisted slot rejected");
    check(!corrupt.reconcile({"a.plugin"}) && corrupt.placements().at("a.plugin") == 0,
          "corrupt state has accessible read-only recovery layout");
    check(!corrupt.move("a.plugin", 3), "corrupt state never overwritten by rearrangement");
    std::ifstream original(file);
    std::string first;
    std::getline(original, first);
    check(first == "a.plugin 1", "corrupt preferences preserved");
    original.close();
    const auto settingsFile=root/"settings-district.ini";
    sf::Downtown settings(settingsFile);
    check(settings.load() && settings.reconcile({"settings.graphics","settings.shell"}),
          "Settings reuses the same allocator in its independent preference file");
    check(settings.move("settings.graphics",127),"Settings can move to a distant plot");
    sf::Downtown settingsRestart(settingsFile);
    check(settingsRestart.load() && settingsRestart.placements().at("settings.graphics")==127,
          "Settings positions survive restart independently of plugins");
    check(settingsRestart.reconcile({"settings.graphics","settings.shell","settings.sound"}) &&
          settingsRestart.placements().at("settings.graphics")==127,
          "new Settings groups fill gaps without disturbing rearrangements");
    check(settingsRestart.move("settings.sound",127) &&
          settingsRestart.placements().at("settings.sound")==127,
          "Settings buildings use the same persistent move/swap behavior");
    fs::remove(settingsFile);
    fs::remove(file);
    fs::remove(root);
    return failures ? 1 : 0;
}
