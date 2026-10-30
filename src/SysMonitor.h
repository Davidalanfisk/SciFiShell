#pragma once
// System monitor window: live CPU / memory / GPU history, per core load, top processes, network and disks.
// Opens when the CPU (or RAM / GPU) monument in the city is clicked.
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "CityData.h"
#include "PanelApp.h"

namespace sf {

class SysMonitor : public PanelApp {
public:
    enum Focus { kCpu = 0, kMemory = 1, kGpu = 2 };

    SysMonitor(Renderer& renderer, std::function<SystemStats()> stats);
    const char* name() const override { return "SYSTEM MONITOR"; }
    void setFocus(int f) { focus_ = f; invalidate(); }

protected:
    void tick(double dt) override;
    void paint(Bitmap& bm) override;
    bool fsResizable() const override { return true; }
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    double repaintInterval() const override { return 0.5; }

private:
    struct Proc {
        std::string name;
        float cpu = 0;
        unsigned long long mem = 0;
        unsigned long pid = 0;
    };
    void sampleProcesses(double wall);
    void sampleNetwork(double wall);

    std::function<SystemStats()> stats_;
    SystemStats cur_;
    std::deque<float> cpuH_, ramH_, gpuH_, vramH_;
    std::vector<Proc> procs_;
    std::map<unsigned long, unsigned long long> prevTimes_;
    double sampleT_ = 0, procT_ = 99, netT_ = 99, procWall_ = 0;
    unsigned long long lastIn_ = 0, lastOut_ = 0;
    bool netInit_ = false;
    double rxRate_ = 0, txRate_ = 0;
    std::deque<float> rxH_, txH_;
    int focus_ = kCpu, hoverCard_ = -1;
    std::vector<std::pair<std::string, std::string>> drives_;
    double driveT_ = 99;
    ui::Rect cardRect_[3];
};

}  // namespace sf
