#pragma once
// Top-level application: window, subsystems, state machine (menu <-> file room) and main loop.
#include <memory>
#include <string>

#include "FileRoom.h"
#include "City.h"
#include "InputSystem.h"
#include "LCARSUI.h"
#include "Renderer.h"
#include "Settings.h"
#ifdef _WIN32
#include "AppWindow.h"
#include "AppCatalog.h"
#endif
#include "SettingsHub.h"
#include "SettingsPanel.h"
#include "TaskSwitcher.h"
#include "ToolRing.h"
#include "Viewer.h"
#include "VRSystem.h"
#include "XInputController.h"
#ifdef SCIFI_HAS_CEF
#include "WebBrowser.h"
#endif

struct SDL_Window;

namespace sf {

class AppShell {
public:
    AppShell();
    ~AppShell();
    bool init();
    int run();

private:
    enum class State { Menu, FileRoom };

    struct Camera {
        Vec3 position{0, 1.6f, 0};
        float yaw = 0, pitch = 0;
        Mat4 world() const { return Mat4::translate(position) * Mat4::rotateY(yaw) * Mat4::rotateX(pitch); }
        Vec3 forward() const { return world().transformDir({0, 0, -1}); }
    };

    void enterFileRoom();
    void leaveFileRoom();
    void toggleVR();
    void toggleCity(bool on);
    [[noreturn]] void fastExit();
    void tick(double dt);
    void handleSwitcher(FrameInput& in, PadState& pad);
    void switchTo(int id);
    void updateCamera(double dt, float moveX, float moveY, float moveZ, float lookX, float lookY, float speedMul = 1.0f);
    void renderScene(const Mat4& view, const Mat4& proj, const Vec3& eye, bool desktopPass);
    Node& activeRoot();
    Vec3 mouseRayDirection(int mx, int my) const;

    SDL_Window* window_ = nullptr;
    Renderer renderer_;
    InputSystem input_;
    XInputController pad_;
    VRSystem vr_;
    std::unique_ptr<LCARSMenu> menu_;
    std::unique_ptr<FileRoom> room_;
    std::unique_ptr<City> city_;
    bool cityMode_ = false, clearRoomPending_ = false;
    Camera savedCam_;
    bool cityCamSet_ = false;
    int powerArmed_ = 0;  // 1 restart / 2 shutdown awaiting a confirming second click
    std::unique_ptr<Viewer> viewer_;
    std::unique_ptr<ToolRing> ring_;
    std::unique_ptr<TaskSwitcher> switcher_;
    std::unique_ptr<SettingsHub> settingsPanel_;
    std::unique_ptr<SettingsPanel> appsPanel_;
#ifdef _WIN32
    std::vector<std::unique_ptr<AppWindow>> apps_;  // one per catalog entry, created on first launch
    AppWindow* app_ = nullptr;                      // the panel that has the pointer / keyboard
    std::vector<AppEntry> catalog_;
    int appsPage_ = 0;
    bool appsRebuild_ = false;
    bool appIconsLoaded_ = false;
    size_t pinnedCount_ = 0;  // catalog_ entries shown in the Applications panel; later ones are launched from settings
    void buildAppsPanel();
    void launchApp(int index);
    void pickApp(const Vec3& o, const Vec3& d);
#endif
    Settings settings_;
    void refreshSettings();
    void buildSettingsHub();
    void openFileRoom(const DirEntry& entry);
    int deleteArmed_ = 0;
    bool renaming_ = false;  // in-app rename text entry (file panel)
    std::string renameBuf_, renamePath_;
    void runTool(Tool tool);
    void saveScreenshot();
    bool shotPending_ = false;
    // Scripting: line-based commands read from a file (SCIFI_SCRIPT at start, or %LOCALAPPDATA%\SciFiShell\control.txt at any time).
    void scriptTick(double dt);
    bool scriptExec(const std::string& line);
    void scriptLog(const std::string& s);
    std::vector<std::string> script_;
    size_t scriptPc_ = 0;
    double scriptWait_ = 0, scriptPoll_ = 0;
    std::string shotPath_;
    struct ScriptMove {
        bool on = false, pos = false, ang = false, hasY = false;
        Vec3 from{}, to{};
        float yaw0 = 0, yaw1 = 0, pitch0 = 0, pitch1 = 0;
        double t = 0, dur = 0;
    } mv_;
#ifdef SCIFI_HAS_CEF
    std::unique_ptr<WebBrowser> browser_;
#endif
    HeadPose head_{{0, 1.6f, 0}, {0, 0, -1}, false, 16.0f / 9.0f};  // last rendered head position (camera or headset)
    std::shared_ptr<GpuLabel> crosshair_;
    float cursorX_ = 0, cursorY_ = 0;  // virtual cursor (NDC): fullscreen media, and the aim while a window has focus
    bool winFocus_ = false, wasWindowed_ = false;
    bool flying_ = false;                        // in a building: flying until a floor is touched, then walking with gravity
    float vy_ = 0.0f;
    bool lockedWin_ = false;                     // the view is pinned to a window by a long click
    float notOverT_ = 0.0f;
    bool holding_ = false;                       // mouse held on a window: a long press re-centres the view on it
    float holdT_ = 0, holdMove_ = 0, recT_ = -1;  // recT_ >= 0 while the view eases onto a window
    Vec3 recFromPos_{}, recToPos_{};
    float recFromYaw_ = 0, recToYaw_ = 0, recFromPitch_ = 0, recToPitch_ = 0;
    Vec3 aimDir_{0, 0, -1};
    bool vDrag_ = false;
    std::shared_ptr<GpuLabel> fpsLabel_;
    double fpsAccum_ = 0;
    int fpsFrames_ = 0;

    State state_ = State::Menu;
    Camera camera_;
    bool running_ = true;
    bool vrOn_ = false;
    VRInput vrInput_;
    double time_ = 0;
    static constexpr float kFovY = 1.2f;  // radians (~69 degrees)
};

}  // namespace sf
