#pragma once
// The Tron-style city: CPU / RAM / GPU monuments in the middle and one platform per drive around them. On a platform every
// top-level folder is a city block and each of its subfolders a building. Built from the CityData snapshot; grows out of
// the floor and reacts to filesystem changes.
#include <memory>
#include <string>
#include <vector>

#include "CityData.h"
#include "SceneGraph.h"

namespace sf {

class City {
public:
    explicit City(Renderer& renderer);
    ~City();

    void start() { data_.start(); }
    void stop() { data_.stop(); }

    // Advances the data/animation. `active` = the city is on screen (emergence only progresses while it is).
    void update(double dt, bool active, Vec3 eye);
    void render(Renderer& renderer, Vec3 eye);

    // Pushes a point (a camera eye) out of the buildings; landing on a roof is allowed.
    void resolve(Vec3& p, float radius, float eyeH = 0.35f) const;
    // Height of the ground or rooftop a player standing at p would stand on.
    float floorAt(const Vec3& p, float radius, float eyeH) const;
    // Ray pick over the towers; sets the hover highlight. Returns false when nothing is hit.
    bool pick(Vec3 origin, Vec3 dir, std::string& path, std::string& name);
    void clearHover() { hoverP_ = hoverB_ = hoverT_ = -1; }
    // Remembers the hovered building as the one being entered; exitSpawn() is street level in front of its door.
    void markEntered();
    bool exitSpawn(Vec3& pos) const;
    bool debugStreet(Vec3& pos) const;  // debugging aid: street in front of the first grown tower

    float extent() const { return extent_; }
    bool built() const { return built_; }
    float emergeProgress() const;

private:
    struct Tower {
        std::string name;
        float curH = 0, curW = 0, tgtH = 3, tgtW = 3;
        float x = 0, z = 0;
        float delay = 0, grow = 0;
        float flash = 0;
        std::string labelText;
        std::shared_ptr<GpuLabel> label;
        std::shared_ptr<GpuLabel> doorLabel;
        float labelTime = -10;
        uint64_t files = 0, bytes = 0;
    };
    struct Block {
        std::string name;
        float cx = 0, cz = 0, side = 20;  // plate footprint, world space
        int g = 1;                        // buildings per row
        std::vector<Tower> towers;
        std::shared_ptr<GpuLabel> label;
        float maxH = 4;
    };
    struct Platform {
        std::string name, text, root;
        float cx = 0, cz = 0, side = 60, pitch = 12;
        float usedFrac = 0;
        float delay = 0, grow = 0;
        uint64_t files = 0, bytes = 0, total = 0;
        bool done = false;
        int tint = 0;
        std::vector<Block> blocks;
        std::shared_ptr<GpuLabel> label;
        std::string labelText;
        float labelTime = -10;
        float roadAngle = 0;
        float maxH = 10;
    };
    struct Monument {
        std::shared_ptr<GpuLabel> label;
        std::string text;
        float labelTime = -10;
    };

    struct Box { float x0, x1, y1, z0, z1; };
    void monumentBoxes(std::vector<Box>& out) const;
    // Gentle stepped ramps where the roads meet raised slabs, so the city can be walked into and out of without jumping.
    struct Ramp { float x, z; int dir; int plat; };  // dir: 0 +x, 1 -x, 2 +z, 3 -z (the way the ramp runs away from the slab edge)
    void rampList(std::vector<Ramp>& out) const;
    void gatherBoxes(const Vec3& p, float rad, std::vector<Box>& out) const;
    int hoverP_ = -1, hoverB_ = -1, hoverT_ = -1;
    bool hasEntered_ = false;
    Vec3 enteredDoor_;

    void rebuild(const std::vector<CityDrive>& snap);
    void layoutPlatforms(const std::vector<CityDrive>& snap);
    void refreshTargets(const std::vector<CityDrive>& snap);
    void renderBox(Renderer& r, Vec3 c, Vec3 size, Vec3 neon, float seed, int kind, float emerge, float fill, float flash);
    void renderMonuments(Renderer& r, Vec3 eye);
    void drawLabel(Renderer& r, GpuLabel& l, Vec3 at, float h, Vec3 eye, Vec3 color, float alpha = 1.0f);
    std::shared_ptr<GpuLabel> makeText(const std::string& s);
    std::shared_ptr<GpuLabel> makeDoorLabel(const std::string& name);
    Vec3 tintColor(int i) const;
    static float ease(float g) { g = g < 0 ? 0 : (g > 1 ? 1 : g); return 1.0f - (1.0f - g) * (1.0f - g) * (1.0f - g); }

    Renderer& renderer_;
    CityData data_;
    std::shared_ptr<GpuLabel> dummy_;
    std::vector<Platform> platforms_;
    uint64_t version_ = 0;
    bool ready_ = false, built_ = false;
    double clock_ = 0;     // emergence clock, advances only while the city is active
    double wall_ = 0;      // always-running time for label throttling
    double statTimer_ = 0;
    float extent_ = 300;
    float coreGrow_[3] = {0, 0, 0};
    float coreDelay_[3] = {0.3f, 0.9f, 1.5f};
    float cpuLoad_ = 0, ramUsed_ = 0, gpuLoad_ = 0, vramUsed_ = 0;
    std::vector<float> coreLoad_;
    float shownCpu_ = 0, shownRam_ = 0, shownGpu_ = 0;
    std::vector<float> shownCore_;
    Monument mon_[3];
    int labelBudget_ = 0;
    uint64_t lastRebuildVersion_ = 0;
    double lastRefresh_ = 0;
    std::vector<CityDrive> snap_;
};

}  // namespace sf
