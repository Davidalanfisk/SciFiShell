#pragma once

#include <filesystem>
#include <string>

namespace sf {

// Internal consent store: an explicit grant is bound to the exact validated ZIP bytes.
class GameplayConsent {
public:
    explicit GameplayConsent(std::filesystem::path directory);
    bool grant(const std::string& id, const std::string& packageBytes, std::string& error);
    bool matches(const std::string& id, const std::string& packageBytes, bool& granted, std::string& error) const;
    bool revoke(const std::string& id, std::string& error);

private:
    std::filesystem::path directory_;
};

}  // namespace sf
