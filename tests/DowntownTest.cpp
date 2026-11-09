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
    const auto region = sf::downtownRegion();
    check(region.width == 160 && region.depth == 1048 &&
          region.x-region.width*0.5f == sf::kDowntownWest &&
          region.x+region.width*0.5f == sf::kDowntownEast &&
          region.z-region.depth*0.5f == sf::kDowntownNorth &&
          region.z+region.depth*0.5f == sf::kDowntownSouth,
          "rendered region outline exactly matches the reserved district on all four edges");
    for (int i = 0; i < sf::kDowntownSlots; ++i) {
        const auto p = sf::downtownPlot(i);
        check(p.z-14 > 75 && p.x-14 > sf::kDowntownWest && p.x+14 < sf::kDowntownEast,
              "plots south of Computer and inside reserved corridor");
        check(sf::downtownIntersects(p.x, p.z, 14), "drive packing excludes every plot");
        check(p.x-14 >= region.x-region.width*0.5f && p.x+14 <= region.x+region.width*0.5f &&
              p.z-14 >= region.z-region.depth*0.5f && p.z+14 <= region.z+region.depth*0.5f,
              "colored outline encloses every selectable plot including the final row");
        for (int j = 0; j < i; ++j) {
            const auto q = sf::downtownPlot(j);
            check(std::abs(p.x-q.x) >= 32 || std::abs(p.z-q.z) >= 32, "plots never overlap");
        }
    }
    check(!sf::downtownIntersects(0, -55, 20), "Computer not inside Downtown");
    check(!sf::downtownIntersects(300, 128, 40, 42), "unrelated city space remains available");
    std::set<std::string> many;
    for (int i = 0; i < 64; ++i) many.insert("plugin." + std::to_string(i));
    check(restart.reconcile(many) && restart.placements().size() == 64, "full package-store capacity supported");
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
    fs::remove(file);
    fs::remove(root);
    return failures ? 1 : 0;
}
