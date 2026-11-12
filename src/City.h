#pragma once
// The Tron-style city: CPU / RAM / GPU monuments in the middle and one platform per drive around them. On a platform every
// top-level folder is a city block and each of its subfolders a building. Built from the CityData snapshot; grows out of
// the floor and reacts to filesystem changes.
#include <memory>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "CityData.h"
#include "SceneGraph.h"
#include "NativePlugin.h"
#include "TrackSamplingApi.h"
#include "ParksSceneApi.h"
#include "Downtown.h"
#include "GameplayModel.h"

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
    // The light cycle used while riding a bus lane, with its light ribbon trailing along the route behind it.
    void renderPacket(Renderer& renderer, Vec3 pos, const std::vector<Vec3>& path, float dist);

    // Pushes a point (a camera eye) out of the buildings; landing on a roof is allowed.
    void resolve(Vec3& p, float radius, float eyeH = 0.35f) const;
    // Height of the ground or rooftop a player standing at p would stand on.
    float floorAt(const Vec3& p, float radius, float eyeH) const;
    // Ray pick over the towers; sets the hover highlight. Returns false when nothing is hit.
    bool pick(Vec3 origin, Vec3 dir, std::string& path, std::string& name);
    // Click a hardware monument: 0 = CPU, 1 = RAM, 2 = GPU, -1 = none. Independent of the building geometry.
    int pickMonument(Vec3 origin, Vec3 dir) const;
    const SystemStats& sysStats() const { return data_.stats(); }
    // Click a bus lane: fills the points of the ride to the far end of that route.
    bool pickRoute(Vec3 origin, Vec3 dir, std::vector<Vec3>& path) const;
    void routes(std::vector<std::vector<Vec3>>& out) const;
    float clearHeight(float x, float z, float r) const;
    void setDrawScale(float s) { drawScale_ = s; }  // graphics option: scales every draw-distance cut-off
    void tourPoints(std::vector<Vec3>& out) const;  // monuments + storage regions for the idle fly-through
    void clearHover() { hoverP_ = hoverB_ = hoverT_ = -1; }
    // Remembers the hovered building as the one being entered; exitSpawn() is street level in front of its door.
    void markEntered();
    bool exitSpawn(Vec3& pos) const;
    bool debugPond(Vec3& pos);  // nearest pond park to the hub, for scripted checks
    bool debugStreet(Vec3& pos) const;  // debugging aid: street in front of the first grown tower
    void setDowntown(const std::map<std::string, int>& plots,
                     const std::vector<std::pair<std::string, std::string>>& names);
    bool setDowntownModel(const std::string& id, const GameplayModel& model, std::string& error);
    void clearDowntownModel(const std::string& id);
    bool pickDowntown(Vec3 origin, Vec3 dir, std::string& id) const;
    bool downtownExit(const std::string& id, Vec3& pos) const;
    bool downtownCustom(const std::string& id) const { return downtownModels_.count(id) != 0; }
    const DowntownRegion& downtownBounds() const { return downtownRegion_; }
    bool downtownLayoutClear() const; // Actual drive/route geometry must honor the occupied district.
    void setSettingsDistrict(const std::map<std::string, int>& plots,
                             const std::vector<std::pair<std::string, std::string>>& names);
    bool pickSettingsBuilding(Vec3 origin, Vec3 dir, std::string& id) const;
    bool settingsBuildingExit(const std::string& id, Vec3& pos) const;
    const DowntownRegion& settingsDistrictBounds() const { return settingsRegion_; }
    // Returns -1 on unavailable/over-budget, 0 outside a grid cell, 1 with center, pitch and occupancy.
    int gameplayCell(float x, float z, double* result) const noexcept;
    // Public map data for plugins with the "map" permission. Never includes folder names or drive paths.
    struct MapPlace { std::string id, name, kind; float x = 0, z = 0, width = 0, depth = 0; };
    void mapPlaces(bool regions, std::vector<MapPlace>& out) const;
    bool gameplayTrackClearance(float x, float z, float radius, double* result) const;
    void setParksEnabled(bool enabled);
    void setDrivingEnabled(bool enabled);
    bool prepareDrivingModule(const std::filesystem::path& path, std::string& error);
    void releaseDrivingModule();
    bool prepareParksModule(const std::filesystem::path& path, std::string& error);
    void releaseParksModule();
    const SfTrackSamplingV1* drivingModule() const { return trackSampling_; }

    // Street toys: hot dog carts and ice cream stands hand out a wireframe snack (0 = hot dog, 1 = ice cream) that is held,
    // then thrown like a grenade and blows up after five seconds, shoving anyone nearby.
    bool pickStand(Vec3 origin, Vec3 dir, int& kind);
    void giveItem(int kind);
    bool holding() const;
    // The drivable car: AppShell owns the physics and tells the city where to draw it.
    struct CarView { bool show = false, hideBody = false, solid = false; Vec3 pos{0, 0, 0}; float yaw = 0, pitch = 0, steer = 0, speed = 0, spawn = 1, spin = 0;
        bool rail = false;  // riding the race track: orientation comes from the track's own frame instead of yaw/pitch
        Vec3 railT{0, 0, -1}, railU{0, 1, 0};
    };
    void setCar(const CarView& v) { car_ = v; }
    // The race track: a ramp up from the hub's CPU region, a tour over the city with loops, and a ramp back down. Frames sit on the deck centre.
    struct TrackFrame { Vec3 p, t, u, flat; float s; };
    static constexpr float kTrackHalf = 3.7f;
    void ensureTrack();  // (re)builds the layout when the city changed
    bool trackReady() const { return !track_.empty(); }
    float trackLength() const { return track_.empty() ? 0.0f : track_.back().s; }
    bool trackSample(float s, TrackFrame& out) const;
    void setTrackFade(float f) { trackFade_ = f; }
    void throwHeld(Vec3 eye);
    void setLook(Vec3 forward);
    bool takeScore(int& points, int& total);  // ducks popped since the last call
    struct SfxEvent { int id; Vec3 p; };  // 1 throw, 2 explosion, 3 duck popped
    bool takeSfx(SfxEvent& e) { if (sfxQ_.empty()) return false; e = sfxQ_.front(); sfxQ_.erase(sfxQ_.begin()); return true; }
    bool takeBlast(Vec3& impulse);  // a blast that reached the player since the last call

    float extent() const {
        float result = extent_;
        for (const auto& b : downtown_) result = std::max(result, downtownPlot(b.slot).z + 32);
        for (const auto& b : settingsBuildings_) result = std::max(result, -downtownPlot(b.slot, true).z + 32);
        return result;
    }
    bool built() const { return built_; }
    bool settled() const {  // every drive scanned and its buildings laid out
        if (!built_ || snap_.empty()) return false;
        for (const auto& d : snap_) if (!d.done) return false;
        return true;
    }
    float emergeProgress() const;

private:
    struct Tower {
        std::string name;
        float curH = 0, curW = 0, tgtH = 3, tgtW = 3;
        float curD = 0, tgtD = 3;  // depth (z); width is x
        int style = 0;             // silhouette, see towerParts()
        int prop = -1;             // rooftop hologram model (index into the city prop list), -1 = none
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
        std::vector<int> cellOrder;       // grid cells nearest the platform centre first
        std::shared_ptr<GpuLabel> label;
        float maxH = 4;
    };
    struct Platform {
        std::string name, text, root;
        float cx = 0, cz = 0, side = 60, pitch = 12;
        float usedFrac = 0;
        bool sorted = false;  // towers re-seated tallest-nearest-the-hub once the scan finishes
        float delay = 0, grow = 0;
        uint64_t files = 0, bytes = 0, total = 0;
        bool done = false;
        bool gone = false;  // drive unplugged: the region fades away
        int tint = 0;
        int kind = 0;  // drive type, see CityDrive::kind
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
    // A low step round the foot of every slab so it can be climbed from anywhere, and a station at the end of each drive lane.
    struct StepRect { float cx, cz, sx, sz; int plat; float grow; };
    void stepRects(std::vector<StepRect>& out) const;
    struct Station { float x, z, fx, fz, level; int plat; };  // x,z: lane end; f: unit direction towards the slab
    void stations(std::vector<Station>& out) const;
    void renderAccess(Renderer& r, const struct Theme& th, Vec3 eye);
    void gatherBoxes(const Vec3& p, float rad, std::vector<Box>& out) const;
    // The bus network: every storage route is planned so it never runs under a platform or monument, and where it has to cross
    // another lane it rides over it on a ramped bridge. Heights are relative to the road surface.
    struct Piece { float x0, x1, z0, z1, yc, th, f; int plat; bool slope = false; Vec3 a{}, b{}; };
    struct Bundle { int kind; int plat; int lanes; float gap, laneW; float grow; std::vector<Vec3> pts; };
    void bundles(std::vector<Bundle>& out) const;
    void drawBundle(Renderer& r, const Bundle& b, Vec3 col, float lf, float lb, bool anim, Vec3 eye);
    std::vector<SfxEvent> sfxQ_;
    const std::vector<Bundle>* allBundles_ = nullptr;  // set while drawing, so gantries can avoid other bundles' lanes
    void renderBundles(Renderer& r, const struct Theme& th, Vec3 eye);
    struct NetRoute { int plat; std::vector<Vec3> pts; };  // pts: x, height above road, z
    void buildNetwork() const;
    void drawSlope(Renderer& r, Vec3 a, Vec3 b, float width, float th, Vec3 neon, float seed, int kind, float emerge);
    void renderStreetLights(Renderer& r, const Platform& p, const Block& b, Vec3 tint, bool detail, int& draws);
    void renderBlockScenery(Renderer& r, const Platform& p, const Block& b, uint32_t seed, Vec3 tint, bool detail, int& draws);
    struct Stand { float x, z; int kind; };
    struct DuckPos { uint32_t key; Vec3 p; };
    std::vector<DuckPos>* duckSink_ = nullptr;
    std::unordered_map<uint32_t, double> deadDucks_;  // duck key -> wall_ time it was popped; they return after a while
    int scorePending_ = 0, scoreTotal_ = 0;
    void popDucks(Vec3 at);
    std::vector<Vec3>* pondSink_ = nullptr;
    Vec3 lodEye_{0,0,0};
    CarView car_;
    int carModel_ = -2;
    void renderCar(Renderer& r);
    void renderTrack(Renderer& r, Vec3 eye);
    void buildTrack();
    std::vector<TrackFrame> track_;
    struct TrackStrut { Vec3 p; float top; };
    std::vector<TrackStrut> struts_;
    std::vector<std::pair<float, float>> loops_;  // arc-length ranges of the loops
    bool trackDirty_ = true;
    float trackFade_ = 0;
    bool dry_ = false;                    // scenery pass that only collects stands (for picking) and draws nothing
    bool parksEnabled_ = false, drivingEnabled_ = false;
    NativePlugin trackSamplingModule_;
    const SfTrackSamplingV1* trackSampling_ = nullptr;
    NativePlugin parksModule_;
    const SfParksSceneV1* parksScene_ = nullptr;
    const SfParksToysV1* parksToys_ = nullptr;
    std::vector<Stand>* standSink_ = nullptr;
    void stepShots(double dt, Vec3 eye);
    void renderToys(Renderer& r, Vec3 eye);
    struct ToysContext { City* city; Renderer* renderer; };
    SfParksToysHostV1 toysHost(ToysContext* context);
    void renderBoxM(Renderer& r, const Mat4& m, Vec3 neon);
    mutable bool netBuilt_ = false;
    mutable std::vector<NetRoute> net_;
    mutable std::vector<Piece> pieces_;
    mutable std::vector<Box> bridgeFloor_;
    int hoverP_ = -1, hoverB_ = -1, hoverT_ = -1;
    bool hasEntered_ = false;
    Vec3 enteredDoor_;

    void rebuild(const std::vector<CityDrive>& snap);
    void layoutPlatforms(const std::vector<CityDrive>& snap, int first = 0, int count = -1);  // first > 0: add platforms without moving the others
    void refreshTargets(const std::vector<CityDrive>& snap);
    void renderBox(Renderer& r, Vec3 c, Vec3 size, Vec3 neon, float seed, int kind, float emerge, float fill, float flash);
    void renderMonuments(Renderer& r, Vec3 eye);
    void renderModules(Renderer& r, const struct Theme& th, Vec3 eye);
    void moduleLabel(Renderer& r, Monument& m, const std::string& text, Vec3 at, float h, Vec3 eye, Vec3 col);
    void renderDriveBeacon(Renderer& r, const Platform& p, Vec3 tint, float pg);
    void renderCache(Renderer& r, const struct Theme& th, Vec3 eye);
    void renderHub(Renderer& r, const struct Theme& th, float g, Vec3 eye);
    void drawLabel(Renderer& r, GpuLabel& l, Vec3 at, float h, Vec3 eye, Vec3 color, float alpha = 1.0f);
    std::shared_ptr<GpuLabel> makeText(const std::string& s);
    std::shared_ptr<GpuLabel> makeDoorLabel(const std::string& name, Vec3 tint);
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
    Monument cacheLabel_[3], modLabel_[8];
    float shownTemp_ = 40.0f;
    Monument hubLabel_, pylonLabel_[4];
    float shownDiskR_ = 0, shownDiskW_ = 0, shownNetRx_ = 0, shownNetTx_ = 0;
    int labelBudget_ = 0;
    float drawScale_ = 1.0f;
    struct PendingLabel {
        GpuLabel* label;
        Mat4 world;
        float aspect, dist;
    };
    std::vector<PendingLabel> pendingLabels_;  // blended text is drawn after all opaque geometry, far to near
    void flushLabels(Renderer& r);
    uint64_t lastRebuildVersion_ = 0;
    double lastRefresh_ = 0;
    std::vector<CityDrive> snap_;
    struct DowntownBuilding {
        std::string id, name;
        int slot = 0;
        std::shared_ptr<GpuLabel> label;
    };
    struct DowntownModel { uint64_t owner = 0, handle = 0; Mat4 fit; };
    std::vector<DowntownBuilding> downtown_;
    DowntownRegion downtownRegion_;
    std::vector<DowntownBuilding> settingsBuildings_;
    DowntownRegion settingsRegion_{0,-164,64,80};
    Monument settingsDistrictLabel_;
    void resizeDistrictGeometry();
    void renderSettingsDistrict(Renderer& r, Vec3 eye);
    void renderDistrictDoor(Renderer& r, DowntownBuilding& building, Vec3 plot, Vec3 tint, bool north, float scale = 0);
    static constexpr float kDowntownDefaultScale = 1.5f;  // default plugin buildings: 18 x 12 x 18 on a 32 m plot
    float downtownScale(const DowntownBuilding& building) const;
    void renderDefaultDowntown(Renderer& r, const DowntownBuilding& building, Vec3 plot, Vec3 accent, Vec3 alt);
    std::map<std::string, DowntownModel> downtownModels_;
    uint64_t downtownOwner_ = UINT64_MAX;
    Monument downtownLabel_;
    void renderDowntown(Renderer& r, Vec3 eye);
};

}  // namespace sf
