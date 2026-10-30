#pragma once
// The radial tool ring: a procedural, shader-animated identity-disc menu that floats in front of the user.
// Look at a segment (or spin the wheel / stick) and select it. Labels are LCARS pills around the rim.
#include <memory>
#include <vector>

#include "LCARSUI.h"
#include "Renderer.h"

namespace sf {

enum class Tool { Browser = 0, Files, Editor, Media, Settings, Theme, Apps, Power, Monitor, FileBrowser, Terminal };

class ToolRing {
public:
    explicit ToolRing(Renderer& renderer);

    void open(Vec3 eye, Vec3 forward);  // placed ~1.5 m ahead of the eye, facing it
    void close();
    bool active() const { return open_ || anim_ > 0.01f; }
    bool isOpen() const { return open_; }

    void hoverIntent(int i) { if (i != intent_) { intent_ = i; dwell_ = 0; } }
    void pointer(Vec3 origin, Vec3 dir);  // ray -> hovered segment
    void cycle(int delta);                // wheel / stick / d-pad (moves through the apps while the app ring is out)
    Tool selected() const { return kMap[sel_]; }
    // Starts the "activated" pulse; the caller performs the tool action and closes the ring.
    void pulse() { pulse_ = 1.0f; }

    void update(double dt, double time);
    void render(Renderer& renderer);

    // The app ring: a second, larger ring around the first with one selectable icon segment per app.
    // Level 0 holds the built-in tools (Web, Editor, Media, ...), level 1 the Windows apps; they replace each other in the outer ring.
    void setLevel(int level, const std::vector<std::string>& names, const std::vector<Bitmap>& icons, bool transparent);
    void showApps(bool on);
    void enterLevel(int level);  // switch the outer ring between the two item sets
    int appLevel() const { return level_; }
    bool appsReady(int level) const { return !sets_[level].icons.empty(); }
    bool appsMode() const { return appsMode_; }
    bool appHot() const { return appsMode_ && appHot_; }  // the pointer is over the outer ring
    bool wantApps() const { return intent_ == 1 && !appsMode_ && dwell_ > 0.12f; }  // hovering Apps: the shell should open the sub-ring
    int appSelected() const { return appSel_; }
    int appCount() const { return static_cast<int>(sets_[level_].names.size()); }

    static constexpr int kCount = 5;  // Files, Settings, Theme, Apps, Power

private:
    Renderer& renderer_;
    std::shared_ptr<GpuLabel> dummy_;  // supplies a shader resource binding for the procedural quad
    std::vector<std::unique_ptr<PanelNode>> pills_;
    bool open_ = false;
    float anim_ = 0, hoverSmooth_ = 0, pulse_ = 0, time_ = 0;
    int sel_ = 0;
    Vec3 center_{0, 1.6f, -1.5f};
    Vec3 fallbackFlat_{0, 0, -1};
    float yaw_ = 0;
    struct ItemSet {
        std::vector<std::shared_ptr<GpuLabel>> icons;
        std::vector<std::shared_ptr<PanelNode>> names;
        bool transparent = false;
    };
    ItemSet sets_[2];
    int level_ = 0;
    static constexpr Tool kMap[kCount] = {Tool::Files, Tool::Settings, Tool::Theme, Tool::Apps, Tool::Power};
    bool appsMode_ = false, appHot_ = false;
    int appSel_ = 0;
    int intent_ = 0;      // hover intent: 1 open apps, 2 close, 3 enter windows-apps level, 4 back to the tools level
    float dwell_ = 0;     // how long the intent has been held
    float appsAnim_ = 0;  // 0 = single ring, 1 = both rings (the whole disc shrinks to make room)
    static constexpr float kDiameter = 0.975f;
    static constexpr float kOuter = 2.1f;  // outer ring diameter relative to the inner ring
};

}  // namespace sf
