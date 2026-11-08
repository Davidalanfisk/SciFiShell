#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <variant>

namespace sf {
class GameplayStorage {
public:
    using Value = std::variant<std::string, double, bool>;
    GameplayStorage(std::filesystem::path directory, std::string id);
    bool load(std::string& error);
    bool get(const std::string& key, const Value*& value, std::string& error) const;
    bool set(const std::string& key, Value value, std::string& error);
    bool erase(const std::string& key, std::string& error);

private:
    bool save(const std::map<std::string, Value>& values, std::string& error);
    std::filesystem::path directory_;
    std::string id_;
    std::map<std::string, Value> values_;
};
}
