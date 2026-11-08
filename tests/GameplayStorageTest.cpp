#include "GameplayStorage.h"
#include "PluginScript.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("scifishell-storage-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    std::string error;
    sf::GameplayStorage first(root, "com.example.first");
    sf::GameplayStorage second(root, "com.example.second");
    check(first.load(error) && second.load(error), "load missing stores");
    check(first.set("text", std::string("hello\0world", 11), error) &&
          first.set("number", 1.25, error) && first.set("flag", true, error), "write typed values");
    sf::GameplayStorage restored(root, "com.example.first");
    const sf::GameplayStorage::Value* value = nullptr;
    check(restored.load(error) && restored.get("text", value, error) && value &&
          std::get<std::string>(*value) == std::string("hello\0world", 11), "restore string");
    check(restored.get("number", value, error) && value && std::get<double>(*value) == 1.25, "restore number");
    check(restored.get("flag", value, error) && value && std::get<bool>(*value), "restore boolean");
    check(second.get("text", value, error) && !value, "stores are isolated by plugin ID");
    check(!first.set("", true, error) && !first.set(std::string(65, 'a'), true, error) &&
          !first.set("large", std::string(1025, 'x'), error) &&
          !first.set("nan", std::numeric_limits<double>::quiet_NaN(), error), "reject invalid keys and values");
    check(restored.erase("flag", error) && restored.erase("missing", error) && restored.load(error) &&
          restored.get("flag", value, error) && !value, "delete persists and missing delete is harmless");
    sf::PluginScriptRuntime runtime;
    check(!runtime.start("storage_set('x',1) return {}", error), "storage unavailable without permission");
    check(runtime.start("storage_set('a',12.5) storage_set('b',false) storage_set('c','hi') "
                        "if storage_get('a')~=12.5 or storage_get('b')~=false or "
                        "storage_get('c')~='hi' or storage_exists('missing') then missing() end "
                        "storage_delete('a') if storage_exists('a') then missing() end return {}",
                        error, false, nullptr, nullptr, nullptr, false, false, nullptr, false, false, false, &second) &&
          runtime.stop(error), "Lua storage API");
    sf::GameplayStorage secondRestored(root, "com.example.second");
    check(secondRestored.load(error) && secondRestored.get("b", value, error) && value &&
          !std::get<bool>(*value), "Lua writes persist on restart");
    check(!runtime.start("storage_set('x',{}) return {}", error, false, nullptr, nullptr, nullptr,
                         false, false, nullptr, false, false, false, &second), "invalid Lua type rejected");
    check(!runtime.start("for i=1,9 do storage_set('x',i) end return {}", error, false, nullptr, nullptr, nullptr,
                         false, false, nullptr, false, false, false, &second) &&
          error.find("write limit") != std::string::npos, "write rate bounded");
    for (int i = 0; i < 64; ++i) check(first.set("key" + std::to_string(i), false, error) == (i < 61),
                                         "entry limit includes existing keys");
    sf::GameplayStorage large(root, "com.example.large");
    check(large.load(error), "load missing large store");
    for (int i = 0; i < 63; ++i)
        check(large.set("n" + std::to_string(i), std::string(1024, 'x'), error), "write within byte limit");
    check(!large.set("n63", std::string(1024, 'x'), error) &&
          error.find("byte limit") != std::string::npos, "reject storage exceeding serialized byte limit");
    sf::GameplayStorage largeRestored(root, "com.example.large");
    check(largeRestored.load(error) && largeRestored.get("n62", value, error) && value &&
          std::get<std::string>(*value).size() == 1024 &&
          largeRestored.get("n63", value, error) && !value,
          "failed oversized write preserves persisted data");
    const auto path = root / "com.example.second.store";
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << "bad"; }
    check(!secondRestored.load(error) && secondRestored.get("b", value, error) && value &&
          !std::get<bool>(*value), "corrupted storage rejected without replacing loaded values");
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << std::string(65537, 'x'); }
    check(!secondRestored.load(error), "oversized storage file rejected");
    std::error_code ec;
    std::filesystem::remove(root / "com.example.first.store", ec);
    if (ec) { std::cerr << ec.message() << '\n'; ++failures; }
    std::filesystem::remove(root / "com.example.large.store", ec);
    if (ec) { std::cerr << ec.message() << '\n'; ++failures; }
    std::filesystem::remove(path, ec);
    if (ec) { std::cerr << ec.message() << '\n'; ++failures; }
    std::filesystem::remove(root, ec);
    if (ec) { std::cerr << ec.message() << '\n'; ++failures; }
    return failures ? 1 : 0;
}
