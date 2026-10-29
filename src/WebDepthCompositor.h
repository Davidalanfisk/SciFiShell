#pragma once
// 3D Web Depth Compositor.
//
// The page is still rendered by CEF into one texture and drawn as one base plane, so hit-testing stays a trivial
// "ray -> base plane -> page pixel". Depth is added purely as extra geometry in front of / behind that plane:
//   * stacking contexts get a soft shadow card plus a slightly raised copy of their region,
//   * images get a raised, rim-lit panel,
//   * videos get a raised panel with a glow frame (and a little extra float in VR).
// Every extra quad samples a sub-rectangle of the same page texture. Real perspective (camera / headset motion) gives
// the parallax; on desktop a small pointer-driven lateral shift per band adds to it. The DOM is never touched.
#include <string>
#include <vector>

#include "Renderer.h"
#include "Utils.h"
#include "WebLayerAnalyzer.h"

namespace sf {

enum class DepthMode { Off = 0, Subtle = 1, Cinematic = 2 };

const char* depthModeName(DepthMode m);

class WebDepthCompositor {
public:
    WebDepthCompositor();

    DepthMode mode() const { return mode_; }
    void setMode(DepthMode m);
    void cycleMode();

    // Per-site overrides, persisted in %LOCALAPPDATA%/SciFiShell/webdepth.ini (or ~/.scifishell on other systems).
    void setHost(const std::string& host);
    bool hostDisabled() const { return hostDisabled_; }
    bool toggleHostDisabled();

    // True when extra geometry is currently being drawn.
    bool effective() const { return mode_ != DepthMode::Off && !hostDisabled_ && !fallback_; }
    bool fallbackActive() const { return fallback_; }
    bool vrReduced() const { return vrReduced_; }
    std::string statusText() const;

    // pointerU/V: pointer position on the page (-0.5..0.5, v up), used for the desktop lateral parallax.
    // fresh: false while the layout is stale (page scrolled / changed since the last analysis) -> layers flatten out.
    void update(double dt, const WebLayout& layout, float pointerU, float pointerV, bool vr, float frameMs, bool fresh);

    // pageBase = page centre + orientation (no scale). worldW/H = page size in metres. texW/H = page texture size.
    void render(Renderer& r, const GpuLabel& page, const Mat4& pageBase, float worldW, float worldH, int texW, int texH,
                const WebLayout& layout, double time) const;

    // Hit-test a ray (in page-local space: origin at the page centre, +z towards the viewer) against the raised layers
    // drawn last frame. On success returns the page position (view CSS pixels) of the content the user actually sees
    // under the ray, so clicks land on the visible element rather than on the base plane behind it.
    bool pick(Vec3 origin, Vec3 dir, float& viewX, float& viewY) const;

private:
    struct PickItem {
        float cx, cy, z, sw, sh, ex, ey, ew, eh;
    };
    mutable std::vector<PickItem> pickList_;

    void loadSettings();
    void saveSettings() const;

    DepthMode mode_ = DepthMode::Subtle;
    std::string host_;
    std::vector<std::string> disabledHosts_;
    bool hostDisabled_ = false;
    bool fallback_ = false;
    bool vrReduced_ = false;
    bool vr_ = false;
    float depthScale_ = 1.0f;  // smoothed multiplier (0 when off)
    float freshness_ = 0.0f;   // 0..1, follows layout validity
    float eff_ = 0.0f;         // depthScale_ * freshness_
    float px_ = 0, py_ = 0;     // smoothed pointer
    float avgFrameMs_ = 8.0f;
    float calmTime_ = 0;
    int maxCards_ = 12;
    int maxMedia_ = 16;
};

}  // namespace sf
