#pragma once
#include "ArcadeApi.h"
#include "GameplayStorage.h"
#include "NativePlugin.h"
#include "Renderer.h"
#include "Sfx.h"
#include <array>
#include <utility>

namespace sf {
class ArcadeEmulator;
class Arcade {
public:
    Arcade(Renderer& renderer,Sfx& sound);
    ~Arcade();
    bool prepare(const std::filesystem::path& module, std::string& error);
    void stop();
    bool running() const { return api_!=nullptr; }
    bool engaged() const { return selected_>=0; }
    bool playing() const { return playing_; }
    int selected() const { return selected_; }
    void select(int cabinet);
    void leave();
    void controls(double dt, float x, float y, bool fire, bool alternate, int vertical, int horizontal, bool confirm, bool back,
                  bool start=false,bool select=false);
    void update(double dt, double time,float volume=1);
    const std::array<std::shared_ptr<GpuLabel>,SF_ARCADE_CABINETS>& screens() const { return screens_; }
    const std::string& notice() const { return notice_; }
    bool importRom(const std::filesystem::path& path);
    bool takeBrowseRequest() { return std::exchange(browseRequested_,false); }
    void browseResult(const std::filesystem::path& path,const std::string& error);
    SfArcadeState state() const;
private:
    struct Config { int game=0,difficulty=1,invert=0; };
    bool saveConfig(int cabinet, Config config);
    void finishGame();
    void saveScore(SfArcadeState state);
    void play();
    void paint(int cabinet, double time);
    Renderer& renderer_;
    Sfx& sound_;
    NativePlugin module_;
    const SfArcadeV1* api_=nullptr;
    GameplayStorage storage_;
    std::filesystem::path dataDir_;
    std::array<Config,SF_ARCADE_CABINETS> configs_{};
    std::array<int,4> highs_{};
    std::array<std::shared_ptr<GpuLabel>,SF_ARCADE_CABINETS> screens_{};
    std::unique_ptr<ArcadeEmulator> emulator_;
    std::string notice_;
    int selected_=-1, row_=0;
    bool playing_=false,browseRequested_=false;
    double previewAt_=0;
};
}
