#pragma once
// Hosts a native Windows program inside the 3D environment.
//
// The program is launched normally, its top-level window is parked off-screen, and a worker thread copies the window
// contents (PrintWindow) into a CPU buffer that is uploaded to a dynamic GPU texture and drawn on a floating panel.
// Rays (mouse crosshair, VR controller) are converted to window pixels and delivered as window messages, so the program
// receives input without ever taking the real mouse or keyboard.
#include <memory>
#include <string>

#include "InputSystem.h"
#include "Renderer.h"
#include "Utils.h"

namespace sf {

struct AppWindowState;

class AppWindow {
public:
    // ownerWindow: native handle of the shell window (used to take focus back after the app's window appears).
    AppWindow(Renderer& renderer, std::string title, std::string exe, void* ownerWindow);
    ~AppWindow();

    const std::string& title() const { return title_; }
    // Launches the program (or re-shows it when it is already running) and places the panel in front of the viewer.
    void open(Vec3 eye, Vec3 forward, bool vr);
    void hide();         // hides the panel; the program keeps running
    void closeProgram(); // asks the program to close (it may prompt to save)
    // Gives the window back to the desktop (on shell exit) so nothing is left stranded off-screen.
    void release();

    bool active() const { return visible_; }
    bool running() const;
    // Aim helpers for a workspace with several panels: distance along the ray to this panel (or its bar), -1 on a miss.
    float hitDistance(Vec3 origin, Vec3 dir) const;
    // Centre and size (world units) of the window including its title bar; false when fullscreen / hidden.
    bool bounds(Vec3& centre, Vec3& normal, float& w, float& h) const;
    float distanceTo(Vec3 eye) const;
    bool busy() const { return dragging_ || mouseDown_ || fullscreen_; }
    void blur();  // another panel took the pointer: drop focus and any held button

    void update(double dt);
    void render(Renderer& renderer);

    void pointer(Vec3 origin, Vec3 dir);
    void press(bool autoRelease);
    void releaseButton();
    void scriptClick(int px, int py);  // test hook: click at content pixel
    void scriptTextKeys(const std::string& utf8);
    void scrollNotches(float notches);
    void key(EditKey key);
    void text(const std::string& utf8);
    // True once the user has clicked into the program: keystrokes then go to it instead of moving the camera.
    bool wantsKeyboard() const { return visible_ && focused_ && running(); }
    void escape();  // leaves fullscreen first, otherwise hides the panel

    // Fullscreen: the panel fills the screen (desktop only) and the program's window is resized to the screen's aspect.
    bool fullscreen() const { return fullscreen_; }
    void toggleFullscreen();
    void setView(float aspect, float tanHalfFov) { viewAspect_ = aspect; viewTan_ = tanHalfFov; }

private:
    void lendForeground();
    enum class Hit { None, Hide, Close, Full, Title };
    Hit barHit(float px, float py) const;
    bool rayToPlane(Vec3 origin, Vec3 dir, float& hx, float& hy) const;
    void applyFullscreenLayout();
    void drawBar();
    void sendMouse(int msg, int wparam, bool wheelDelta = false, int delta = 0);

    Renderer& renderer_;
    std::string title_, exe_;
    std::unique_ptr<AppWindowState> s_;
    bool visible_ = false;
    bool focused_ = false;
    bool fullscreen_ = false;
    float viewAspect_ = 16.0f / 9.0f, viewTan_ = 0.5f;
    Mat4 savedBase_ = Mat4::identity();
    float savedW_ = 2.6f;
    bool releasePending_ = false;
    bool mouseDown_ = false;
    // Title-bar drag: the grabbed point on the bar (panel-local) follows the pointer at a fixed distance from the viewer.
    bool dragging_ = false;
    float grabX_ = 0, grabY_ = 0, grabDist_ = 2.0f, hoverX_ = 0, hoverY_ = 0, hoverDist_ = 2.0f;
    bool barDirty_ = true;
    Hit hover_ = Hit::None;
    double clock_ = 0;
};

}  // namespace sf
