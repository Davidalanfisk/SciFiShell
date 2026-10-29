#pragma once
// The radial tool ring: a procedural, shader-animated identity-disc menu that floats in front of the user.
// Look at a segment (or spin the wheel / stick) and select it. Labels are LCARS pills around the rim.
#include <memory>
#include <vector>

#include "LCARSUI.h"
#include "Renderer.h"

namespace sf {

enum class Tool { Browser = 0, Files, Editor, Media, Settings, Theme, Apps, Power };

class ToolRing {
public:
    explicit ToolRing(Renderer& renderer);

    void open(Vec3 eye, Vec3 forward);  // placed ~1.5 m ahead of the eye, facing it
    void close();
    bool active() const { return open_ || anim_ > 0.01f; }
    bool isOpen() const { return open_; }

    void pointer(Vec3 origin, Vec3 dir);  // ray -> hovered segment
    void cycle(int delta);                // wheel / stick / d-pad (moves through the apps while the app ring is out)
    Tool selected() const { return static_cast<Tool>(sel_); }
    // Starts the "activated" pulse; the caller performs the tool action and closes the ring.
    void pulse() { pulse_ = 1.0f; }

    void update(double dt, double time);
    void render(Renderer& renderer);

    // The app ring: a second, larger ring around the first with one selectable icon segment per app.
    void setApps(const std::vector<std::string>& names, const std::vector<Bitmap>& icons);
    void showApps(bool on);
    bool appsMode() const { return appsMode_; }
    bool appHot() const { return appsMode_ && appHot_; }  // the pointer is over the outer ring
    int appSelected() const { return appSel_; }

    static constexpr int kCount = 8;

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
    std::vector<std::shared_ptr<GpuLabel>> appIcons_;
    std::vector<std::unique_ptr<PanelNode>> appNames_;
    bool appsMode_ = false, appHot_ = false;
    int appSel_ = 0;
    float appsAnim_ = 0;  // 0 = single ring, 1 = both rings (the whole disc shrinks to make room)
    static constexpr float kDiameter = 1.5f;
    static constexpr float kOuter = 2.1f;  // outer ring diameter relative to the inner ring
};

}  // namespace sf
