#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
namespace sf {
bool readArcadeRom(const std::filesystem::path& path,std::string& bytes,std::string& error);
class ArcadeEmulator {
public:
    ArcadeEmulator();
    ~ArcadeEmulator();
    bool start(const std::filesystem::path& engine,const std::filesystem::path& rom,std::string& error);
    void input(float x,float y,bool a,bool b,bool start,bool select);
    uint64_t frames() const;
    void volume(float value);
    void update(double dt);
    void pixels(std::vector<uint8_t>& rgba) const;
    const std::string& error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
