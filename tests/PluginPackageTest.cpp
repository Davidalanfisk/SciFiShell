#include "PluginPackage.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include "miniz.h"

namespace fs = std::filesystem;

std::string archive(const std::vector<std::pair<std::string, std::string>>& files) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return {};
    for (const auto& [name, contents] : files)
        if (!mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(), MZ_DEFAULT_COMPRESSION)) {
            mz_zip_writer_end(&zip);
            return {};
        }
    void* buffer = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size)) { mz_zip_writer_end(&zip); return {}; }
    std::string result(static_cast<char*>(buffer), size);
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return result;
}

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
    const std::string manifest = R"({"id":"com.example.clock","name":"CLOCK","version":"1.0.0","author":"Example","description":"Clock message","entry":"scripts/main.lua","type":"script","platforms":["any"],"permissions":[],"min_shell_version":"0.1.0"})";
    const std::string code = "return 'CLOCK ' .. unix_time()";
    const auto zip = archive({{"plugin.json", manifest}, {"scripts/main.lua", code}});
    sf::ScriptPackage script;
    check(!zip.empty() && sf::PluginPackageStore::readZip(zip, script, error) && script.script == code, "valid ZIP");
    check(sf::PluginPackageStore::readZip(archive({{"plugin.json", manifest}, {"scripts/", ""}, {"scripts/main.lua", code}}),
                                         script, error), "optional directory entry");
    for (const auto& path : {"../escape", "/absolute", "C:\\evil", "scripts/../escape", "scripts\\main.lua"}) {
        const std::string placeholder(std::strlen(path), 'q');
        auto hostile = archive({{"plugin.json", manifest}, {placeholder, code}});
        size_t pos = 0;
        while ((pos = hostile.find(placeholder, pos)) != std::string::npos) {
            hostile.replace(pos, placeholder.size(), path);
            pos += placeholder.size();
        }
        check(!hostile.empty() && !sf::PluginPackageStore::readZip(hostile, script, error), "hostile path rejected");
    }
    check(!sf::PluginPackageStore::readZip(archive({{"plugin.json", manifest}, {"scripts/main.lua", code}, {"plugin.json", manifest}}), script, error), "duplicate rejected");
    check(!sf::PluginPackageStore::readZip(archive({{"plugin.json", manifest}, {"scripts/main.lua", std::string(17000, 'x')}}), script, error), "oversized entry rejected");
    auto symlink = zip;
    const auto central = symlink.find(std::string("PK\x01\x02", 4));
    if (central == std::string::npos) { ++failures; }
    else {
        symlink[central + 38] = 0; symlink[central + 39] = 0;
        symlink[central + 40] = static_cast<char>(0xff); symlink[central + 41] = static_cast<char>(0xa1);
        check(!sf::PluginPackageStore::readZip(symlink, script, error), "symlink rejected");
    }
    auto nulName = zip;
    const auto namePos = nulName.find("scripts/main.lua");
    if (namePos == std::string::npos) { ++failures; }
    else {
        nulName[namePos + 3] = '\0';
        const auto second = nulName.find("scripts/main.lua");
        if (second != std::string::npos) nulName[second + 3] = '\0';
        check(!sf::PluginPackageStore::readZip(nulName, script, error), "embedded NUL rejected");
    }
    check(!sf::PluginPackageStore::readZip(zip.substr(0, zip.size() - 10), script, error), "truncated ZIP rejected");
    auto corrupt = zip;
    if (corrupt.size() > 41) {
        corrupt[41] ^= 0x7f;
        check(!sf::PluginPackageStore::readZip(corrupt, script, error), "corrupted ZIP data rejected");
    } else ++failures;
    auto badPermission = manifest;
    badPermission.replace(badPermission.find("\"permissions\":[]"), 16, "\"permissions\":[\"network\"]");
    check(!sf::PluginPackageStore::readZip(archive({{"plugin.json", badPermission}, {"scripts/main.lua", code}}), script, error), "permissions rejected");
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
    const fs::path scriptSource = dir / "source.zip";
    { std::ofstream out(scriptSource, std::ios::binary); out.write(zip.data(), zip.size()); }
    sf::PluginPackageStore scriptStore(installed, [&](const sf::ThemePackage& pkg) { return ids.insert(pkg.id).second; },
        [&](const sf::ScriptPackage& pkg) { return ids.insert(pkg.id).second; });
    check(scriptStore.install(scriptSource), "install ZIP");
    check(!scriptStore.install(scriptSource), "duplicate ZIP rejected");
    check(scriptStore.uninstall("com.example.clock"), "uninstall ZIP");
    ids.erase("com.example.clock");
    check(!scriptStore.uninstall("com.example.clock"), "uninstall missing ZIP rejected");
    check(scriptStore.install(scriptSource), "reinstall ZIP");
    ids.clear();
    scriptStore.discover();
    check(scriptStore.error().empty() && ids.count("com.example.clock"), "ZIP discovered after restart");
    check(scriptStore.uninstall("com.example.clock"), "remove ZIP after discovery");
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
    fs::remove(scriptSource);
    fs::remove(dir);
    return failures ? 1 : 0;
}
