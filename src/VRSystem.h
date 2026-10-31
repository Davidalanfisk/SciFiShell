#pragma once
// OpenXR integration (instance, session, stereo swapchains, headset pose, controller pointer).
// The graphics binding is implemented for Direct3D 12 (Windows / SteamVR). On other platforms
// start() reports that VR is unavailable and the shell keeps running in desktop mode.
#include <functional>
#include <memory>
#include <string>

#include "Renderer.h"
#include "Utils.h"

namespace sf {

struct EyeView {
    Mat4 view, proj;
    uint32_t width = 0, height = 0;
};

struct VRInput {
    bool valid = false;           // pointer pose tracked
    Vec3 pointerOrigin, pointerDir;  // world space (rig applied)
    float moveX = 0, moveY = 0;   // left thumbstick
    bool selectPressed = false;   // right trigger edge
    bool backPressed = false;     // right B / menu edge
    bool menuPressed = false;     // left Y / B / menu edge (tool ring)
    float lookX = 0, lookY = 0;   // right thumbstick / trackpad: turn and climb
    bool kbdPressed = false;      // left grip or X / A edge (on-screen keyboard)
    bool switchPressed = false;   // right grip edge (window switcher)
    bool active() const {         // a controller is being used right now
        return selectPressed || backPressed || menuPressed || kbdPressed || switchPressed || moveX != 0 || moveY != 0 || lookX != 0 || lookY != 0;
    }
};

class VRSystem {
public:
    VRSystem();
    ~VRSystem();

    bool start(Renderer& renderer, std::string& error);
    void stop();
    bool running() const;

    // Handles runtime events and, when the session is ready, renders both eyes via drawScene.
    // `rig` is the world transform of the tracking-space origin (player position + yaw).
    using DrawScene = std::function<void(const EyeView&)>;
    void frame(Renderer& renderer, const Mat4& rig, const DrawScene& drawScene, VRInput& input);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sf
