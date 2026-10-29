#pragma once
// Chromium (CEF, off-screen rendering) browser panel for the file room, plus its depth compositor.
//
// The panel is a world-anchored page quad (1920x1080 texture) with a toolbar quad above it. The page is rendered by
// CEF into a CPU buffer, uploaded to a dynamic GPU texture, and drawn through WebDepthCompositor. Rays (mouse
// crosshair, VR controller) are intersected with the base page plane and converted to page pixels for CEF.
#include <memory>
#include <string>

#include "InputSystem.h"
#include "Renderer.h"
#include "Utils.h"

namespace sf {

struct WebState;

class WebBrowser {
public:
    // CEF process-level hooks. executeSubprocess() must run first thing in main(): it returns >= 0 when this process
    // was started by CEF as a renderer/GPU/utility child (the value is the exit code), -1 for the normal browser process.
    static int executeSubprocess();
    static bool ensureCef();  // lazy CefInitialize (first time the browser is opened)
    static void pump();       // CefDoMessageLoopWork(); cheap when CEF is not running

    explicit WebBrowser(Renderer& renderer);
    ~WebBrowser();

    bool active() const { return visible_; }
    bool started() const;  // the page has been created (it may be hidden)
    // Shows the panel in front of the viewer. A non-empty url navigates; otherwise the last page (or home) is kept.
    void open(const std::string& url, Vec3 eye, Vec3 forward, bool vr);
    void close();  // hides, mutes and pauses rendering; the page stays alive for the next open()

    void update(double dt, Vec3 eye, bool vr);
    void render(Renderer& renderer);

    // Pointer: a world-space ray (camera crosshair or VR controller).
    void pointer(Vec3 origin, Vec3 dir);
    void press(bool autoRelease);  // autoRelease: the source has no release event (VR, gamepad) -> release next frame
    void release();
    void scrollNotches(float notches);  // mouse wheel
    void scrollPixels(float pixels);    // analog stick (positive = down)

    void key(EditKey key);
    void text(const std::string& utf8);
    bool addressFocused() const { return addressFocused_; }
    // True when keystrokes belong to the page/address bar (address focused or a page text field has focus); otherwise
    // WASD etc. move the camera.
    bool wantsKeyboard() const;
    // Escape: leaves the address bar, otherwise closes the panel.
    void escape();

    void focusAddress();
    // Aim helpers: distance along the ray to the page or toolbar (-1 on a miss), and view parameters for 85% launch sizing.
    float hitDistance(Vec3 origin, Vec3 dir) const;
    bool bounds(Vec3& centre, Vec3& normal, float& w, float& h) const;
    bool busy() const { return dragging_ || mouseDown_; }
    void toggleFullscreen();
    bool fullscreen() const { return fullscreen_; }
    void setView(float aspect, float tanHalfFov) { viewAspect_ = aspect; viewTan_ = tanHalfFov; }
    void navigate(const std::string& input);
    void goBack();
    void goForward();
    void reload();
    void cycleDepth();
    void toggleSite();
    std::string statusText() const;

    // Backing-texture quality presets (page is laid out at 1280x720 CSS px and rasterised at 1.5x/2x/3x/4x).
    static constexpr int kQualityCount = 4;
    static float scaleForQuality(int q);
    static std::string qualityText(int q);
    void setQuality(int q);

private:
    enum class Hit { None, Back, Forward, Reload, Address, Full, Close, Grip };

    Hit toolbarHit(float px, float py) const;
    bool rayToPlane(Vec3 origin, Vec3 dir, float& hx, float& hy) const;
    void drawToolbar();
    void applyFullscreen();
    void scheduleAnalysis(double delay);
    void markInput();
    void sendMouse(bool move, bool up, int px, int py);

    Renderer& renderer_;
    std::unique_ptr<WebState> s_;
    bool visible_ = false;
    bool addressFocused_ = false;
    bool addressFresh_ = false;
    std::string address_;
    Hit hover_ = Hit::None;
    bool toolbarDirty_ = true;
    bool releasePending_ = false;
    bool mouseDown_ = false;
    bool dragging_ = false;
    bool fullscreen_ = false;
    Mat4 savedBase_;
    float savedWorldW_ = 2.4f;
    float grabX_ = 0, grabY_ = 0, grabDist_ = 2.0f, hoverX_ = 0, hoverY_ = 0, hoverDist_ = 2.0f;
    float viewAspect_ = 16.0f / 9.0f, viewTan_ = 0.7f;
    double clock_ = 0;
};

}  // namespace sf
