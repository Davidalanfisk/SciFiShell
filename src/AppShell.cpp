#include "AppShell.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "TextRender.h"
#include "Theme.h"
#include "stb_image_write.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstring>
#include "OsSettings.h"
#endif

namespace sf {

namespace {
// Orientation matrix whose +Z axis points along `dir` (used to draw the VR pointer ray).
Mat4 alignZ(Vec3 origin, Vec3 dir, Vec3 scale) {
    Vec3 up = std::fabs(dir.y) > 0.99f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 right = normalize(cross(up, dir));
    Vec3 up2 = cross(dir, right);
    Mat4 m = Mat4::identity();
    m.m[0] = right.x; m.m[1] = right.y; m.m[2] = right.z;
    m.m[4] = up2.x;   m.m[5] = up2.y;   m.m[6] = up2.z;
    m.m[8] = dir.x;   m.m[9] = dir.y;   m.m[10] = dir.z;
    m.m[12] = origin.x; m.m[13] = origin.y; m.m[14] = origin.z;
    return m * Mat4::scale(scale);
}

// Pyramid pointer: the apex sits exactly at `tip` (the active point); the body tilts back toward the viewer.
Mat4 pointerMatrix(Vec3 tip, Vec3 forward, Vec3 worldUp, float size) {
    Vec3 f = normalize(forward);
    Vec3 up = std::fabs(f.y) > 0.99f ? Vec3{0, 0, -1} : worldUp;
    Vec3 right = normalize(cross(f, up));
    Vec3 up2 = cross(right, f);
    Vec3 d = normalize(f * 0.7f + right * -0.35f + up2 * 0.45f);
    return alignZ(tip, d, {size * 0.3f, size * 0.3f, size});
}
}  // namespace

AppShell::AppShell() = default;

AppShell::~AppShell() {
    renderer_.waitIdle();
    vr_.stop();
#ifdef SCIFI_HAS_CEF
    browser_.reset();
#endif
#ifdef _WIN32
    apps_.clear();
    app_ = nullptr;
#endif
    if (city_) city_->stop();
    city_.reset();
    room_.reset();
    viewer_.reset();
    ring_.reset();
    settingsPanel_.reset();
    menu_.reset();
    crosshair_.reset();
    fpsLabel_.reset();
    renderer_.shutdown();
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool AppShell::init() {
#if !defined(_WIN32) && !defined(__APPLE__)
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");  // Diligent's Vulkan swapchain is created from an X11 window
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");  // hosted programs take focus; don't minimize the shell
    SDL_SetHint(SDL_HINT_ALLOW_ALT_TAB_WHILE_GRABBED, "0");   // with the keyboard grabbed, Alt+Tab goes to our task switcher
    Uint32 flags = SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_RESIZABLE;
#ifdef __APPLE__
    flags |= SDL_WINDOW_METAL;
#endif
    window_ = SDL_CreateWindow("SciFi Shell", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
#ifdef _WIN32
    SDL_SetWindowKeyboardGrab(window_, SDL_TRUE);  // lets us see Alt+Tab (and the Win key) for the task switcher
#endif
    char* base = SDL_GetBasePath();
    std::string shaderDir = std::string(base ? base : "./") + "shaders";
    std::string fontDir = std::string(base ? base : "./") + "assets/fonts";
    SDL_free(base);
    if (!loadFonts(fontDir)) std::fprintf(stderr, "No fonts found in %s\n", fontDir.c_str());
    if (!renderer_.init(window_, shaderDir)) {
        std::fprintf(stderr, "Renderer initialisation failed (shader dir: %s)\n", shaderDir.c_str());
        return false;
    }

    LCARSMenu::Callbacks cb;
    cb.enterFileRoom = [this] { enterFileRoom(); };
    cb.toggleVR = [this] { toggleVR(); };
    cb.exitShell = [this] { running_ = false; };
    menu_ = std::make_unique<LCARSMenu>(renderer_, cb);
    room_ = std::make_unique<FileRoom>(renderer_);
    city_ = std::make_unique<City>(renderer_);
    city_->start();
    viewer_ = std::make_unique<Viewer>(renderer_);
    ring_ = std::make_unique<ToolRing>(renderer_);
    switcher_ = std::make_unique<TaskSwitcher>(renderer_);
    settings_.load();
    settingsPanel_ = std::make_unique<SettingsHub>(renderer_);
#ifdef _WIN32
    {
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        void* hwnd = SDL_GetWindowWMInfo(window_, &wm) ? static_cast<void*>(wm.info.win.window) : nullptr;
        catalog_.push_back({"NOTEPAD", "notepad.exe"});
        for (AppEntry& e : loadTaskbarApps())
            if (e.name != "NOTEPAD") catalog_.push_back(std::move(e));
        for (const AppEntry& e : catalog_) apps_.push_back(std::make_unique<AppWindow>(renderer_, e.name, e.launch, hwnd));
        pinnedCount_ = catalog_.size();
        app_ = apps_[0].get();
    }
    buildAppsPanel();
#else
    appsPanel_ = std::make_unique<SettingsPanel>(renderer_, std::vector<std::string>{"CLOSE"}, "APPLICATIONS");
#endif
    buildSettingsHub();
#ifdef SCIFI_HAS_CEF
    browser_ = std::make_unique<WebBrowser>(renderer_);
    browser_->setQuality(settings_.webQuality);
#endif
    room_->onOpenMedia = [this](const OpenRequest& req) { viewer_->open(req, head_); };
    room_->onOpenFile = [this](const DirEntry& e) { openFileRoom(e); };
    room_->onExitBuilding = [this] { toggleCity(true); };
    viewer_->onStep = [this](int dir) { room_->stepFrom(viewer_->path(), dir); };
    crosshair_ = renderer_.createLabel(rasterizeLines({""}, 1, 4, 4));
    if (std::getenv("SCIFI_START_MENU")) {  // debugging aid: the old start menu
        state_ = State::Menu;
    } else {
        enterFileRoom();
        if (std::getenv("SCIFI_OPEN_RING")) ring_->open(head_.eye, head_.forward);  // debugging aid
#ifdef _WIN32
        // Installed builds quietly check GitHub for a newer release; the script only prompts if one exists.
        if (!std::getenv("SCIFI_NO_UPDATE") && std::filesystem::exists("update.ps1"))
            ShellExecuteA(nullptr, "open", "powershell.exe", "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File update.ps1", nullptr, SW_HIDE);
#endif
        if (std::getenv("SCIFI_OPEN_SETTINGS")) settingsPanel_->open(head_.eye, head_.forward);  // debugging aid
        if (std::getenv("SCIFI_OPEN_APPS")) { ring_->open(head_.eye, head_.forward); runTool(Tool::Apps); }  // debugging aid
        if (!std::getenv("SCIFI_START_ROOM") && !std::getenv("SCIFI_START_PATH")) toggleCity(true);  // the city is the home view
#ifdef _WIN32
        if (const char* oa = std::getenv("SCIFI_OPEN_APP")) launchApp(std::max(0, std::atoi(oa) - 1));  // debugging aid: 1 = Notepad, 2 = next catalog entry...
#endif
        if (const char* openFile = std::getenv("SCIFI_OPEN_FILE")) {  // debugging aid: open the viewer on a file
            OpenRequest req;
            req.path = openFile;
            req.name = std::filesystem::u8path(openFile).filename().u8string();
            req.kind = classifyFile(req.path, false);
            req.cardPos = {0, 1.5f, -6.0f};
            req.cardYaw = 0;
            viewer_->open(req, head_);
        }
#ifdef SCIFI_HAS_CEF
        if (const char* openUrl = std::getenv("SCIFI_OPEN_URL"))  // debugging aid: open the browser on a URL
            browser_->open(openUrl, head_.eye, head_.forward, false);
#endif
    }
    return true;
}

void AppShell::fastExit() {
    // Hide the window straight away and skip teardown that can block on worker threads or the GPU driver.
    if (window_) SDL_HideWindow(window_);
    vr_.stop();
#ifdef SCIFI_HAS_CEF
    browser_.reset();  // closes the CEF browser (short message-loop pump) so no child processes linger
#endif
#ifdef _WIN32
    for (auto& a : apps_) a->release();  // give the hosted programs' windows back to the desktop
#endif
    std::fflush(nullptr);
    std::_Exit(0);
}

Node& AppShell::activeRoot() { return state_ == State::Menu ? menu_->root() : room_->root(); }

void AppShell::enterFileRoom() {
    state_ = State::FileRoom;
    camera_ = Camera{};
    if (const char* yawDeg = std::getenv("SCIFI_CAM_YAW")) camera_.yaw = static_cast<float>(std::atof(yawDeg)) * kPi / 180.0f;  // debugging aid
    if (const char* camZ = std::getenv("SCIFI_CAM_Z")) camera_.position.z = static_cast<float>(std::atof(camZ));  // debugging aid
    const char* startPath = std::getenv("SCIFI_START_PATH");  // debugging aid: override the starting folder
    if (startPath && *startPath) {
        room_->load(startPath);  // debugging aid
        if (const char* sr = std::getenv("SCIFI_ROOM")) room_->debugEnter(std::atoi(sr));  // debugging aid
        if (room_->building()) { camera_.position = {0, 1.6f, std::getenv("SCIFI_CAM_Z") ? static_cast<float>(std::atof(std::getenv("SCIFI_CAM_Z"))) : 1.6f}; room_->setEye(camera_.position); }
    }
    else if (std::getenv("SCIFI_START_ROOM")) room_->load("");
    else room_->loadEmpty();  // start with just the grid; the Files tool on the ring loads "This PC"
    input_.setRelativeMouse(true);
    menu_->setStatus("Ready");
}

void AppShell::leaveFileRoom() {
    state_ = State::Menu;
    camera_ = Camera{};
    input_.setRelativeMouse(false);
}

void AppShell::buildSettingsHub() {
    auto onOff = [](bool b) { return std::string(b ? "ON" : "OFF"); };
    auto pct = [](float f) { return std::to_string(static_cast<int>(f * 100.0f + 0.5f)) + "%"; };
    auto save = [this] { settings_.save(); };
    std::vector<SettingsCategory> cats;

    SettingsCategory shell{"SHELL", "Look, controls, behaviour", "SHELL", {}};
    shell.items.push_back({"VR MODE", "Headset rendering", [this, onOff] { return onOff(vrOn_); }, [this] { toggleVR(); }});
    shell.items.push_back({"THEME", "Colour scheme", [] { return std::string(currentTheme().name); }, [this] { cycleTheme(); }});
    shell.items.push_back({"LOOK SENSITIVITY", "Mouse and stick look speed", [this, pct] { return pct(settings_.lookScale()); },
                           [this, save] { settings_.lookIdx = (settings_.lookIdx + 1) % Settings::kSteps; save(); }});
    shell.items.push_back({"MOVE SPEED", "Walking speed in the room", [this, pct] { return pct(settings_.speedScale()); },
                           [this, save] { settings_.speedIdx = (settings_.speedIdx + 1) % Settings::kSteps; save(); }});
    shell.items.push_back({"INVERT LOOK Y", "Flip vertical look", [this, onOff] { return onOff(settings_.invertY); },
                           [this, save] { settings_.invertY = !settings_.invertY; save(); }});
    shell.items.push_back({"CROSSHAIR", "Aiming dot", [this, onOff] { return onOff(settings_.crosshair); },
                           [this, save] { settings_.crosshair = !settings_.crosshair; save(); }});
    shell.items.push_back({"FPS COUNTER", "Frame rate readout", [this, onOff] { return onOff(settings_.showFps); },
                           [this, save] { settings_.showFps = !settings_.showFps; save(); }});
    shell.items.push_back({"BROWSER RESOLUTION", "Web page render quality",
#ifdef SCIFI_HAS_CEF
                           [this] { return WebBrowser::qualityText(settings_.webQuality); },
                           [this, save] {
                               settings_.webQuality = (settings_.webQuality + 1) % WebBrowser::kQualityCount;
                               if (browser_) browser_->setQuality(settings_.webQuality);
                               save();
                           }});
#else
                           [] { return std::string("N/A"); }, nullptr});
#endif
    shell.items.push_back({"DISPLAY", "Shell window size", [this] {
                               int w = 0, h = 0, dw = 0, dh = 0;
                               SDL_GetWindowSizeInPixels(window_, &w, &h);
                               SDL_DisplayMode dm{};
                               if (SDL_GetDesktopDisplayMode(SDL_GetWindowDisplayIndex(window_), &dm) == 0) { dw = dm.w; dh = dm.h; }
                               std::string s = std::to_string(w) + " x " + std::to_string(h);
                               if (dw > 0 && w >= dw && h >= dh) s += " NATIVE";
                               return s;
                           }, nullptr});
    shell.items.push_back({"EXIT SHELL", "Quit the application", nullptr, [this] { running_ = false; }});
    cats.push_back(std::move(shell));

#ifdef _WIN32
    // Windows options, all native: no hosted Windows pages.
    auto info = [](const char* title, const char* hint, std::function<std::string()> value) {
        return SettingsItem{title, hint, std::move(value), nullptr};
    };
    auto volumeText = [] {
        const float v = os::getVolume();
        return v < 0 ? std::string("NO DEVICE") : std::to_string(static_cast<int>(v * 100.0f + 0.5f)) + "%";
    };
    auto make = [&](const char* name, const char* sub, std::vector<SettingsItem> items) {
        cats.push_back({name, sub, "WINDOWS", std::move(items)});
    };
    make("DISPLAY", "Resolution and scaling",
         {info("RESOLUTION", "Current mode and refresh rate", [] { return os::displayText(); }),
          info("SCALING", "Windows display scale", [] { return os::scalingText(); }),
          info("MONITORS", "Connected displays", [] { return os::monitorCount(); })});
    make("SOUND", "Volume and mute",
         {SettingsItem{"VOLUME UP", "Master volume +10%", volumeText, [] { os::setVolume(os::getVolume() + 0.1f); }},
          SettingsItem{"VOLUME DOWN", "Master volume -10%", volumeText, [] { os::setVolume(os::getVolume() - 0.1f); }},
          SettingsItem{"MUTE", "Mute all sound", [onOff] { return onOff(os::getMute()); }, [] { os::setMute(!os::getMute()); }}});
    {
        std::vector<SettingsItem> net;
        for (const os::Adapter& a : os::adapters()) net.push_back(SettingsItem{a.kind, a.detail, [ip = a.ip] { return ip; }, nullptr});
        if (net.empty()) net.push_back(SettingsItem{"OFFLINE", "No active network connection", nullptr, nullptr});
        make("NETWORK", "Active connections", std::move(net));
    }
    make("MOUSE", "Pointer speed and buttons",
         {SettingsItem{"POINTER SPEED UP", "Faster pointer", [] { return std::to_string(os::mouseSpeed()) + " / 20"; },
                       [] { os::setMouseSpeed(os::mouseSpeed() + 1); }},
          SettingsItem{"POINTER SPEED DOWN", "Slower pointer", [] { return std::to_string(os::mouseSpeed()) + " / 20"; },
                       [] { os::setMouseSpeed(os::mouseSpeed() - 1); }},
          SettingsItem{"SWAP BUTTONS", "Left-handed buttons", [onOff] { return onOff(os::mouseSwapped()); },
                       [] { os::setMouseSwapped(!os::mouseSwapped()); }}});
    make("PERSONALISATION", "Colour mode and shell theme",
         {SettingsItem{"WINDOWS DARK MODE", "Apps and taskbar colour mode", [onOff] { return onOff(os::darkMode()); },
                       [] { os::setDarkMode(!os::darkMode()); }},
          SettingsItem{"SHELL THEME", "Colour scheme of this shell", [] { return std::string(currentTheme().name); }, [this] { cycleTheme(); }}});
    make("POWER", "Exit, restart, shut down, lock and sleep",
         {SettingsItem{"EXIT SHELL", "Quit the application", nullptr, [this] { running_ = false; }},
          SettingsItem{"RESTART PC", "Click twice to confirm",
                       [this] { return std::string(powerArmed_ == 1 ? "CLICK AGAIN TO CONFIRM" : "RESTART"); },
                       [this] { if (powerArmed_ == 1) os::powerOff(true); else powerArmed_ = 1; }},
          SettingsItem{"SHUT DOWN PC", "Click twice to confirm",
                       [this] { return std::string(powerArmed_ == 2 ? "CLICK AGAIN TO CONFIRM" : "SHUT DOWN"); },
                       [this] { if (powerArmed_ == 2) os::powerOff(false); else powerArmed_ = 2; }},
          info("POWER", "Battery / mains status", [] { return os::batteryText(); }),
          SettingsItem{"LOCK PC", "Lock the workstation", nullptr, [] { os::lockPc(); }},
          SettingsItem{"SLEEP", "Put the PC to sleep", nullptr, [] { os::sleepPc(); }}});
    {
        std::vector<SettingsItem> sys;
        sys.push_back(info("PC NAME", "Computer name", [] { return os::computerName(); }));
        sys.push_back(info("WINDOWS", "Edition and version", [] { return os::windowsName(); }));
        sys.push_back(info("PROCESSOR", "CPU model", [] { return os::cpuName(); }));
        sys.push_back(info("MEMORY", "In use / installed", [] { return os::memoryText(); }));
        sys.push_back(info("UPTIME", "Since last restart", [] { return os::uptimeText(); }));
        sys.push_back(info("DATE AND TIME", "Local clock", [] { return os::timeText(); }));
        for (const auto& d : os::drives()) sys.push_back(SettingsItem{"DRIVE " + d.first, "Fixed disk", [v = d.second] { return v; }, nullptr});
        make("SYSTEM", "About this PC, memory and disks", std::move(sys));
    }
#endif
    settingsPanel_->setCategories(std::move(cats));
}

// F12: grabs what is on screen inside the shell window and saves it as a PNG in Pictures\SciFiShell.
void AppShell::saveScreenshot() {
#ifdef _WIN32
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(window_, &wm)) return;
    HWND hwnd = wm.info.win.window;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        ok = BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT) != 0;
        SelectObject(mem, old);
        if (ok) {
            auto* px = static_cast<unsigned char*>(bits);
            for (int i = 0; i < w * h; ++i) {  // BGRA -> RGBA, opaque
                std::swap(px[i * 4], px[i * 4 + 2]);
                px[i * 4 + 3] = 255;
            }
            const char* pics = std::getenv("USERPROFILE");
            std::filesystem::path dir = std::filesystem::path(pics ? pics : ".") / "Pictures" / "SciFiShell";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            SYSTEMTIME t;
            GetLocalTime(&t);
            char name[64];
            std::snprintf(name, sizeof(name), "SciFiShell-%04d%02d%02d-%02d%02d%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            ok = stbi_write_png((dir / name).string().c_str(), w, h, 4, bits, w * 4) != 0;
            if (ok) std::fprintf(stderr, "Screenshot saved: %s\n", (dir / name).string().c_str());
        }
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    MessageBeep(ok ? MB_OK : MB_ICONERROR);  // audible confirmation: nothing is drawn on screen
#endif
}

void AppShell::runTool(Tool tool) {
    ring_->pulse();
    switch (tool) {
        case Tool::Browser:
#ifdef SCIFI_HAS_CEF
            if (browser_) browser_->open("", head_.eye, head_.forward, vrOn_);
#else
            menu_->setStatus("Browser not built in");
#endif
            break;
        case Tool::Files:
            if (cityMode_) toggleCity(false);
#ifdef SCIFI_HAS_CEF
            if (browser_ && browser_->active()) browser_->close();
#endif
            if (viewer_->active()) viewer_->close();
            if (!room_->loaded()) {
                room_->load("");  // "" = the "This PC" drives room
                menu_->setStatus("This PC");
            }
            break;
        case Tool::Editor:
            if (cityMode_) toggleCity(false);
            menu_->setStatus(room_->openFirst(true));
            break;
        case Tool::Media:
            if (cityMode_) toggleCity(false);
            menu_->setStatus(room_->openFirst(false));
            break;
        case Tool::Settings:
            buildSettingsHub();
            settingsPanel_->open(vrOn_ ? head_.eye : camera_.position,
                                 vrOn_ ? head_.forward : Vec3{-std::sin(camera_.yaw), 0.0f, -std::cos(camera_.yaw)});
            break;
        case Tool::Power:
            powerArmed_ = 0;
            buildSettingsHub();
            settingsPanel_->open(vrOn_ ? head_.eye : camera_.position,
                                 vrOn_ ? head_.forward : Vec3{-std::sin(camera_.yaw), 0.0f, -std::cos(camera_.yaw)});
            settingsPanel_->openPage("POWER");
            break;
        case Tool::Theme: cycleTheme(); break;
        case Tool::Apps:
#ifdef _WIN32
            if (!appIconsLoaded_) {
                appIconsLoaded_ = true;
                std::vector<std::string> names;
                std::vector<Bitmap> icons;
                for (size_t i = 0; i < pinnedCount_; ++i) {
                    Bitmap bm;
                    if (!loadAppIcon(catalog_[i].launch, 128, bm)) {  // no icon: a plain tile with the app's initial
                        bm = makeLabel(catalog_[i].name.substr(0, 1), 90.0f, FontFace::UI, Color8{}, 128, 128);
                    }
                    names.push_back(catalog_[i].name);
                    icons.push_back(std::move(bm));
                }
                ring_->setApps(names, icons);
            }
            ring_->showApps(!ring_->appsMode());
#endif
            return;  // the app ring stays open so an icon can be picked
    }
    if (tool != Tool::Theme) ring_->close();
}

void AppShell::toggleCity(bool on) {
    if (!city_ || on == cityMode_) return;
    cityMode_ = on;
    room_->setCardsVisible(!on);
    if (on) {
        if (cityCamSet_) {
            camera_ = savedCam_;
        } else {
            const float e = city_->extent();
            camera_.position = {0, e * 0.4f, e * 0.8f};
            camera_.yaw = 0;
            camera_.pitch = -std::atan2(e * 0.4f, e * 0.8f) * 0.9f;
        }
        flying_ = true;
        vy_ = 0.0f;
        Vec3 street;
        if (city_->exitSpawn(street)) {  // step out onto the street in front of the building's door
            camera_.position = street;
            camera_.yaw = kPi;
            camera_.pitch = 0.0f;
            flying_ = false;
        }
        menu_->setStatus("CITY - aim at a building and click to enter");
    } else {
        savedCam_ = camera_;
        cityCamSet_ = true;
        camera_ = Camera{};
        menu_->setStatus("");
    }
}

void AppShell::toggleVR() {
    if (vrOn_) {
        vr_.stop();
        vrOn_ = false;
        vrInput_ = VRInput{};
        menu_->setStatus("VR off");
    } else {
        std::string error;
        if (vr_.start(renderer_, error)) {
            vrOn_ = true;
            menu_->setStatus("VR active");
        } else {
            menu_->setStatus("VR error: " + error);
        }
    }
    menu_->setVRState(vrOn_);
}

Vec3 AppShell::mouseRayDirection(int mx, int my) const {
    int w = 1, h = 1;
    SDL_GetWindowSize(window_, &w, &h);
    float ndcX = 2.0f * mx / std::max(w, 1) - 1.0f;
    float ndcY = 1.0f - 2.0f * my / std::max(h, 1);
    float t = std::tan(kFovY * 0.5f);
    float aspect = static_cast<float>(w) / static_cast<float>(std::max(h, 1));
    return normalize(camera_.world().transformDir({ndcX * t * aspect, ndcY * t, -1.0f}));
}

void AppShell::updateCamera(double dt, float moveX, float moveY, float moveZ, float lookX, float lookY, float speedMul) {
    camera_.yaw -= lookX;
    camera_.pitch = clampf(camera_.pitch - lookY, -1.4f, 1.4f);
    const float speed = 3.0f * speedMul * (!cityMode_ && room_->building() ? room_->speedBoost() : 1.0f);
    Vec3 fwd{-std::sin(camera_.yaw), 0, -std::cos(camera_.yaw)};
    Vec3 right{std::cos(camera_.yaw), 0, -std::sin(camera_.yaw)};
    Vec3 p = camera_.position + fwd * (moveY * speed * static_cast<float>(dt)) +
             right * (moveX * speed * static_cast<float>(dt));
    const bool inBuilding = !cityMode_ && room_->building();
    float r = std::sqrt(p.x * p.x + p.z * p.z);
    const float maxR = cityMode_ && city_ ? city_->extent() * 1.3f : std::max(40.0f, room_->extent());  // stay inside the room
    if (!inBuilding && r > maxR) {
        p.x *= maxR / r;
        p.z *= maxR / r;
    }
    camera_.position = p;
    const bool walkable = !vrOn_ && (inBuilding || (cityMode_ && city_));
    if (!walkable) {
        flying_ = false;
        vy_ = 0.0f;
        const float lo = 0.35f, hi = cityMode_ ? 450.0f : (inBuilding ? room_->maxEye() : 24.0f);
        camera_.position.y = clampf(camera_.position.y + moveZ * speed * static_cast<float>(dt), lo, hi);
        if (cityMode_ && city_) city_->resolve(camera_.position, 0.8f);
        if (inBuilding) {
            room_->resolve(camera_.position);
            room_->setEye(camera_.position);
        }
        return;
    }
    // Creative-mode movement: Space jumps, a double-tap toggles flying; while flying Space rises and Ctrl descends.
    // Touching a floor with no climb input ends the flight and gravity takes over again.
    const float eyeH = 1.7f;
    const float fl = (inBuilding ? room_->floorHeight(camera_.position) : city_->floorAt(camera_.position, 0.8f, eyeH)) + eyeH;
    const bool up = moveZ > 0.05f;
    static bool prevUp = false;
    static Uint32 lastTap = 0;
    float y = camera_.position.y;
    if (up && !prevUp) {
        const Uint32 now = SDL_GetTicks();
        if (lastTap != 0 && now - lastTap < 320) {
            flying_ = !flying_;
            vy_ = 0.0f;
            lastTap = 0;
        } else {
            lastTap = now;
            if (!flying_ && y <= fl + 0.05f) vy_ = 7.5f;
        }
    }
    prevUp = up;
    if (flying_) {
        vy_ = 0.0f;
        y += moveZ * speed * static_cast<float>(dt);
    } else {
        vy_ -= 22.0f * static_cast<float>(dt);
        y += vy_ * static_cast<float>(dt);
    }
    if (y <= fl) {
        y = fl;
        if (vy_ < 0.0f) vy_ = 0.0f;
        if (!up) flying_ = false;
    }
    camera_.position.y = std::min(y, inBuilding ? room_->maxEye() : 450.0f);
    if (inBuilding) {
        room_->resolve(camera_.position);
        room_->setEye(camera_.position);
    } else {
        city_->resolve(camera_.position, 0.8f, flying_ ? 0.35f : eyeH);
    }
}
#ifdef _WIN32
// The row panel has a fixed row height, so the catalog is shown a page at a time.
void AppShell::buildAppsPanel() {
    constexpr int kPer = 7;
    const int total = static_cast<int>(pinnedCount_);
    const int pages = std::max(1, (total + kPer - 1) / kPer);
    appsPage_ = std::min(appsPage_, pages - 1);
    const int first = appsPage_ * kPer, last = std::min(total, first + kPer);
    std::vector<std::string> rows;
    for (int i = first; i < last; ++i) rows.push_back(catalog_[i].name);
    if (pages > 1) rows.push_back("NEXT PAGE  " + std::to_string(appsPage_ + 1) + "/" + std::to_string(pages));
    rows.push_back("CLOSE");
    appsPanel_ = std::make_unique<SettingsPanel>(renderer_, rows, "APPLICATIONS");
    const int count = last - first;
    appsPanel_->onRow = [this, first, count, pages](int row) {
        if (row < count) {
            appsPanel_->close();
            launchApp(first + row);
        } else if (pages > 1 && row == count) {
            appsPage_ = (appsPage_ + 1) % pages;
            appsRebuild_ = true;  // rebuilt next frame; this callback runs inside the panel being replaced
        } else {
            appsPanel_->close();
        }
    };
}

void AppShell::launchApp(int index) {
    if (index < 0 || index >= static_cast<int>(apps_.size())) return;
    AppWindow* a = apps_[index].get();
    if (app_ && app_ != a) app_->blur();
    app_ = a;
    a->setView(renderer_.aspect(), std::tan(kFovY * 0.5f));
    if (!a->active()) a->open(head_.eye, head_.forward,     vrOn_);
#ifdef SCIFI_HAS_CEF
    if (browser_ && browser_->active()) browser_->close();
#endif
}

// With several panels up, the one nearest along the aim ray owns the pointer.
void AppShell::pickApp(const Vec3& o, const Vec3& d) {
    AppWindow* best = nullptr;
    float bestDist = 1e9f;
    for (auto& a : apps_) {
        const float t = a->hitDistance(o, d);
        if (t >= 0 && t < bestDist) { bestDist = t; best = a.get(); }
    }
    if (best && best != app_) {
        if (app_) app_->blur();
        app_ = best;
    }
}
#endif

void AppShell::switchTo(int id) {
    // ids: 0 room, 1 hosted program, 2 browser, 3 Windows desktop
    if (id == 3) {
        SDL_MinimizeWindow(window_);
        return;
    }
#ifdef _WIN32
    for (size_t i = 0; i < apps_.size(); ++i) {
        AppWindow* a = apps_[i].get();
        if (id == 10 + static_cast<int>(i)) {
            if (!a->active()) a->open(head_.eye, head_.forward, vrOn_);
            if (app_ && app_ != a) app_->blur();
            app_ = a;
        } else if (id != 2 && id != 0 && a->active()) {
            // keep other panels while switching between apps
        } else if (a->active()) {
            a->hide();
        }
    }
    if (id >= 10 && id < 10 + static_cast<int>(apps_.size())) {
#ifdef SCIFI_HAS_CEF
        if (browser_ && browser_->active()) browser_->close();
#endif
        return;
    }
#endif
#ifdef SCIFI_HAS_CEF
    if (browser_) {
        if (id == 2 && !browser_->active()) browser_->open("", head_.eye, head_.forward, vrOn_);
        else if (id != 2 && browser_->active()) browser_->close();
    }
#endif
}

// Alt+Tab / Ctrl+Tab task switcher. While it is open it swallows the input that would otherwise reach the scene.
void AppShell::handleSwitcher(FrameInput& in, PadState& pad) {
    if (state_ != State::FileRoom) return;
    if (!switcher_->isOpen()) {
        if (in.tabStep == 0) return;
        int current = 0;
        std::vector<TaskSwitcher::Item> items;
        items.push_back({0, "ROOM", "File room", false});
#ifdef _WIN32
        for (size_t i = 0; i < apps_.size(); ++i)
            if (apps_[i]->running()) {
                items.push_back({10 + static_cast<int>(i), catalog_[i].name, "Program", false});
                if (apps_[i]->active() && apps_[i].get() == app_) current = 10 + static_cast<int>(i);
            }
#endif
#ifdef SCIFI_HAS_CEF
        if (browser_ && browser_->started()) {
            items.push_back({2, "BROWSER", "Web", false});
            if (browser_->active()) current = 2;
        }
#endif
        items.push_back({3, "DESKTOP", "Minimise shell", false});
        for (auto& it : items) it.current = it.id == current;
        switcher_->open(std::move(items), currentTheme().accent);
        if (in.tabStep < 0) switcher_->step(-2);  // opened on the next task; Shift goes to the previous one instead
        in.tabStep = 0;
    } else if (in.tabStep != 0) {
        switcher_->step(in.tabStep);
        in.tabStep = 0;
    }
    if (in.left || pad.leftPressed) switcher_->step(-1);
    if (in.right || pad.rightPressed) switcher_->step(1);
    if (in.wheel != 0.0f) switcher_->step(in.wheel > 0 ? -1 : 1);
    if (in.modReleased || in.select || in.space || pad.aPressed || in.click) {
        const int id = switcher_->selectedId();
        switcher_->close();
        switchTo(id);
    } else if (in.escape || in.back || pad.bPressed) {
        switcher_->close();
    }
    in.keys.clear();
    in.text.clear();
    in.click = false;
    in.escape = in.back = in.select = in.space = in.left = in.right = in.up = in.down = false;
    in.wheel = 0.0f;
    pad.aPressed = pad.bPressed = pad.leftPressed = pad.rightPressed = pad.upPressed = pad.downPressed = false;
}

void AppShell::tick(double dt) {
    // Windowed mode: with the cursor off every window the keyboard drives the camera, whatever has typing focus.
    const bool kbOff = wasWindowed_ && !winFocus_;
#ifdef SCIFI_HAS_CEF
    WebBrowser::pump();
    const bool browserOpen = browser_ && browser_->active();
    const bool browserFs = browserOpen && browser_->fullscreen();
    const bool browserTyping = browserOpen && !kbOff && browser_->wantsKeyboard();
#else
    const bool browserOpen = false;
    const bool browserFs = false;
    const bool browserTyping = false;
#endif
#ifdef _WIN32
    if (appsRebuild_) {
        appsRebuild_ = false;
        buildAppsPanel();
        appsPanel_->open(head_.eye, head_.forward);
    }
    if (!app_ || !app_->active())
        for (auto& a : apps_)
            if (a->active()) { app_ = a.get(); break; }
    if (!app_ && !apps_.empty()) app_ = apps_[0].get();
    const bool appOpen = app_ && app_->active();
    const bool appTyping = appOpen && !kbOff && app_->wantsKeyboard();
#else
    const bool appOpen = false;
    const bool appTyping = false;
#endif
    FrameInput in = input_.poll();
    PadState pad = pad_.poll();
    if (in.quit) running_ = false;
    if (in.resized) renderer_.resize();
    {
        const float th = std::tan(kFovY * 0.5f), asp = renderer_.aspect();
#ifdef _WIN32
        for (auto& a : apps_) a->setView(asp, th);
#endif
#ifdef SCIFI_HAS_CEF
        if (browser_) browser_->setView(asp, th);
#endif
    }
    handleSwitcher(in, pad);
    if (renaming_ && !settingsPanel_->isOpen()) renaming_ = false;
    const bool vEditing = viewer_->editing() && !kbOff;
    if (in.toggleVR && !vEditing && !browserTyping && !appTyping) toggleVR();
    input_.setTextInput(vEditing || browserTyping || appTyping || renaming_);
    if (renaming_) in.fullscreen = in.browser = in.toggleVR = in.space = in.up = in.down = in.left = in.right = in.back = false;

    const float fdt = static_cast<float>(dt);
    // Typing only captures the keyboard when a text field (page or address bar) has focus, so WASD walks otherwise.
    const bool typing = vEditing || browserTyping || appTyping || renaming_;
    // While typing, WASD belong to the text field; hold Alt to walk with the keyboard anyway (pad / VR sticks always walk).
    const bool kbMove = !typing || (SDL_GetModState() & KMOD_ALT) != 0;
    float kbX = kbMove ? in.moveX : 0.0f, kbY = kbMove ? in.moveY : 0.0f, kbZ = kbMove ? in.moveZ : 0.0f;
    if (viewer_->active() && !viewer_->isText()) kbZ = std::min(kbZ, 0.0f);  // Space is play/pause in the media viewer
    kbZ = clampf(kbZ + pad.rt - pad.lt, -1, 1);  // right trigger = up, left trigger = down
    float moveX = clampf(kbX + pad.lx + vrInput_.moveX, -1, 1);
    float moveY = clampf(kbY + pad.ly + vrInput_.moveY, -1, 1);
    const float freeSpeed = (in.shift ? 1.69f : 1.0f) * settings_.speedScale() * (cityMode_ ? 8.0f : 1.0f);
    float lookX = in.lookX * 0.0025f * settings_.lookScale() + pad.rx * 2.2f * fdt * settings_.lookScale();
    float lookY = (in.lookY * 0.0025f - pad.ry * 2.2f * fdt) * settings_.lookScale() * (settings_.invertY ? -1.0f : 1.0f);
    bool up = in.up || pad.upPressed, down = in.down || pad.downPressed;
    bool keySelect = in.select || pad.aPressed || (state_ == State::Menu && in.space);
    bool pointerSelect = in.click || vrInput_.selectPressed;
    bool back = in.back || pad.bPressed || vrInput_.backPressed;
    bool vrPointer = vrOn_ && vrInput_.valid;
    const bool ringToggle = in.ring || pad.yPressed || vrInput_.menuPressed;
    if (in.themeCycle) cycleTheme();
    if (in.screenshot) shotPending_ = true;
    const bool ringWasOpen = ring_->isOpen();
    if (state_ == State::FileRoom && !viewer_->editing() && !browserOpen && !appOpen && !ringWasOpen && !settingsPanel_->isOpen() && !appsPanel_->isOpen() && !viewer_->overlay() && ringToggle) {
        // Desktop: use the live camera (head_ lags a frame); VR: the headset pose captured at render time.
        if (vrOn_) ring_->open(head_.eye, head_.forward);
        else ring_->open(camera_.position, Vec3{-std::sin(camera_.yaw), 0.0f, -std::cos(camera_.yaw)});
    }

    if (state_ == State::Menu) {
        Vec3 o = camera_.position, d = mouseRayDirection(in.mouseX, in.mouseY);
        if (vrPointer) {
            o = vrInput_.pointerOrigin;
            d = vrInput_.pointerDir;
        }
        Node* hit = pickNode(menu_->root(), o, d);
        if (hit && (in.mouseMoved || vrPointer)) menu_->select(hit);
        if (up) menu_->moveSelection(-1);
        if (down) menu_->moveSelection(1);
        if (pointerSelect && hit) menu_->activate(hit);
        else if (keySelect) menu_->activateSelected();
    } else {
        Vec3 o = camera_.position, d = camera_.forward();
        if (vrPointer) {
            o = vrInput_.pointerOrigin;
            d = vrInput_.pointerDir;
        }
        aimDir_ = d;
        // Desktop windows behave like fixed windows: while the aim is over one, the view and walking freeze and the mouse
        // moves a cursor. Moving off it hands control back to the camera, which eases round to where the cursor was.
        const bool windowed = !vrOn_ && !appsPanel_->isOpen() && !ring_->isOpen() &&
                              ((settingsPanel_->isOpen() && !settingsPanel_->fullscreen()) || (appOpen && !app_->fullscreen()) || (!appOpen && browserOpen && !browserFs) ||
                               (!appOpen && !browserOpen && viewer_->active() && !viewer_->overlay()));
        if (!windowed) {
            winFocus_ = false;
            lockedWin_ = false;
            wasWindowed_ = false;
            holding_ = false;
            recT_ = -1;
        } else {
            if (!wasWindowed_) {
                wasWindowed_ = true;
                winFocus_ = false;
                cursorX_ = cursorY_ = 0;
            }
            const float th = std::tan(kFovY * 0.5f), asp = renderer_.aspect();
            auto overWindow = [&](const Vec3& oo, const Vec3& dd) {
                bool h = false;
#ifdef _WIN32
                for (auto& a : apps_) if (a->active() && a->hitDistance(oo, dd) >= 0) h = true;
#endif
#ifdef SCIFI_HAS_CEF
                if (browserOpen && browser_->hitDistance(oo, dd) >= 0) h = true;
#endif
                if (viewer_->hits(oo, dd)) h = true;
                if (settingsPanel_->isOpen() && settingsPanel_->hitDistance(oo, dd) >= 0) h = true;
                return h;
            };
            auto winBounds = [&](const Vec3& oo, const Vec3& dd, Vec3& c, Vec3& n, float& w, float& h) {
#ifdef _WIN32
                for (auto& a : apps_) if (a->active() && a->hitDistance(oo, dd) >= 0 && a->bounds(c, n, w, h)) return true;
#endif
#ifdef SCIFI_HAS_CEF
                if (browserOpen && browser_->hitDistance(oo, dd) >= 0 && browser_->bounds(c, n, w, h)) return true;
#endif
                if (viewer_->hits(oo, dd) && viewer_->bounds(c, n, w, h)) return true;
                return settingsPanel_->isOpen() && settingsPanel_->hitDistance(oo, dd) >= 0 && settingsPanel_->bounds(c, n, w, h);
            };
            bool dragging = viewer_->dragging();
#ifdef _WIN32
            if (appOpen && app_->busy()) dragging = true;
#endif
#ifdef SCIFI_HAS_CEF
            if (browserOpen && browser_->busy()) dragging = true;
#endif
            // Long press on a window (without moving): ease the view round until the window is centred at 85% size again.
            if (recT_ < 0.0f) {
                if ((winFocus_ || overWindow(o, d)) && in.click) {
                    holding_ = true;
                    holdT_ = holdMove_ = 0.0f;
                }
                if (holding_) {
                    holdT_ += fdt;
                    holdMove_ += std::fabs(in.lookX) + std::fabs(in.lookY);
                    Vec3 c, n;
                    float w, h;
                    if (in.clickUp || holdMove_ > 30.0f) {
                        holding_ = false;
                    } else if (holdT_ > 0.6f) {
                        holding_ = false;
                        const Vec3 cd = normalize(camera_.world().transformDir({cursorX_ * th * asp, cursorY_ * th, -1.0f}));
                        if (winBounds(o, cd, c, n, w, h)) {
                            n.y = 0;
                            n = length(n) > 1e-4f ? normalize(n) : Vec3{0, 0, 1};
                            if (dot(n, camera_.position - c) < 0) n = n * -1.0f;
                            const float dist = fitDistance(w, h, asp, th);
                            const float eyeY = clampf(c.y, 0.35f, 24.0f);
                            recFromPos_ = camera_.position;
                            recToPos_ = {c.x + n.x * dist, eyeY, c.z + n.z * dist};
                            recFromYaw_ = camera_.yaw;
                            recToYaw_ = std::atan2(n.x, n.z);
                            recFromPitch_ = camera_.pitch;
                            recToPitch_ = std::atan2(c.y - eyeY, dist);  // aim straight at the window centre
                            recT_ = 0.0f;
                        }
                    }
                }
            }
            if (recT_ >= 0.0f) {
                recT_ += fdt / 0.7f;
                const float rt = std::min(recT_, 1.0f), e = rt * rt * (3.0f - 2.0f * rt);
                camera_.position = {lerpf(recFromPos_.x, recToPos_.x, e), lerpf(recFromPos_.y, recToPos_.y, e),
                                    lerpf(recFromPos_.z, recToPos_.z, e)};
                float dy = std::fmod(recToYaw_ - recFromYaw_ + 3.14159265f, 6.2831853f);
                if (dy < 0) dy += 6.2831853f;
                camera_.yaw = recFromYaw_ + (dy - 3.14159265f) * e;
                camera_.pitch = lerpf(recFromPitch_, recToPitch_, e);
                cursorX_ = cursorY_ = 0.0f;
                lookX = lookY = moveX = moveY = kbZ = 0.0f;
                if (recT_ >= 1.0f) {
                    recT_ = -1.0f;
                    lockedWin_ = true;  // a long click pinned the view to this window
                    notOverT_ = 0.0f;
                }
            } else if (winFocus_) {
                int ww = 1, wh = 1;
                SDL_GetWindowSize(window_, &ww, &wh);
                cursorX_ = clampf(cursorX_ + in.lookX * 2.0f / std::max(ww, 1) + pad.rx * 1.5f * fdt, -1.0f, 1.0f);
                cursorY_ = clampf(cursorY_ - in.lookY * 2.0f / std::max(wh, 1) - pad.ry * 1.5f * fdt, -1.0f, 1.0f);
                lookX = lookY = moveX = moveY = kbZ = 0.0f;
            } else if (cursorX_ != 0.0f || cursorY_ != 0.0f) {
                // Ease the camera round to the cursor so the aim does not jump when control returns.
                const float k = std::min(1.0f, fdt * 12.0f);
                camera_.yaw -= std::atan(cursorX_ * th * asp) * k;
                camera_.pitch = clampf(camera_.pitch + std::atan(cursorY_ * th) * k, -1.4f, 1.4f);
                cursorX_ *= 1.0f - k;
                cursorY_ *= 1.0f - k;
                if (std::fabs(cursorX_) < 0.002f && std::fabs(cursorY_) < 0.002f) cursorX_ = cursorY_ = 0.0f;
            }
            d = normalize(camera_.world().transformDir({cursorX_ * th * asp, cursorY_ * th, -1.0f}));
            aimDir_ = d;
            const bool over = overWindow(o, d);
            // The view only locks to a window after a long click. Once locked it stays put while the window is briefly
            // empty (e.g. the next image loading) and releases only after the aim has been off it for a moment.
            if (over || dragging) notOverT_ = 0.0f;
            else if (lockedWin_) notOverT_ += fdt;
            if (winFocus_ && !over && !dragging && notOverT_ > 0.6f) {
                winFocus_ = false;
                lockedWin_ = false;
            } else if (!winFocus_ && over && lockedWin_) winFocus_ = true;
        }
        if (appsPanel_->isOpen()) {
            updateCamera(dt, 0, 0, 0, lookX, lookY);
            room_->setHover(nullptr);
            appsPanel_->pointer(o, d);
            if (in.wheel != 0.0f) appsPanel_->moveSelection(in.wheel > 0 ? -1 : 1);
            if (up) appsPanel_->moveSelection(-1);
            if (down) appsPanel_->moveSelection(1);
            if (pointerSelect) {
                if (!appsPanel_->activateHit(o, d)) appsPanel_->activateSelected();
            } else if (keySelect) appsPanel_->activateSelected();
            else if (back || in.escape || ringToggle) appsPanel_->close();
        } else if (settingsPanel_->isOpen()) {
            // Settings panel: movement frozen, aim at a row and click / A / trigger to change it.
            const float stan = std::tan(kFovY * 0.5f);
            if (vrOn_ && settingsPanel_->fullscreen()) settingsPanel_->toggleFullscreen();
            settingsPanel_->setView(stan, renderer_.aspect());
            if (settingsPanel_->fullscreen()) {
                int ww = 1, wh = 1;
                SDL_GetWindowSize(window_, &ww, &wh);
                updateCamera(dt, 0, 0, 0, 0, 0);
                cursorX_ = clampf(cursorX_ + in.lookX * 2.0f / std::max(ww, 1) + pad.rx * 1.5f * fdt, -1.0f, 1.0f);
                cursorY_ = clampf(cursorY_ - in.lookY * 2.0f / std::max(wh, 1) - pad.ry * 1.5f * fdt, -1.0f, 1.0f);
                o = {0, 0, 0};
                d = normalize({cursorX_ * stan * renderer_.aspect(), cursorY_ * stan, -1.0f});
            } else {
                updateCamera(dt, moveX, moveY, kbZ, lookX, lookY, freeSpeed);
            }
            room_->setHover(nullptr);
            if (renaming_) {
                for (EditKey k : in.keys) {
                    if (k == EditKey::Backspace) {
                        while (!renameBuf_.empty() && (renameBuf_.back() & 0xC0) == 0x80) renameBuf_.pop_back();
                        if (!renameBuf_.empty()) renameBuf_.pop_back();
                    } else if (k == EditKey::Enter) {
                        namespace fs = std::filesystem;
                        const fs::path src = fs::u8path(renamePath_);
                        const bool bad = renameBuf_.empty() || renameBuf_.find_first_of("\\/:*?\"<>|") != std::string::npos;
                        std::error_code e;
                        const fs::path dst = src.parent_path() / fs::u8path(renameBuf_);
                        if (!bad && !fs::exists(dst, e)) fs::rename(src, dst, e); else e = std::make_error_code(std::errc::invalid_argument);
                        renaming_ = false;
                        menu_->setStatus(e ? "Rename failed (invalid or existing name)" : "Renamed");
                        if (!e) { settingsPanel_->close(); room_->refresh(); }
                    }
                }
                if (renaming_ && !in.text.empty()) renameBuf_ += in.text;
                if (in.escape) renaming_ = false;
                in.escape = in.select = in.click = keySelect = pointerSelect = back = false;
            }
            settingsPanel_->pointer(o, d);
            if (in.wheel != 0.0f) settingsPanel_->step(in.wheel > 0 ? -1 : 1);
            if (up && !kbOff) settingsPanel_->move(0, -1);
            if (down && !kbOff) settingsPanel_->move(0, 1);
            if ((in.left && !kbOff) || pad.leftPressed) settingsPanel_->move(-1, 0);
            if ((in.right && !kbOff) || pad.rightPressed) settingsPanel_->move(1, 0);
            if (pointerSelect) {
                settingsPanel_->activateHit(o, d);
            } else if (keySelect) settingsPanel_->activateSelected();
            else if (back || in.escape || ringToggle) { if (!settingsPanel_->back()) settingsPanel_->close(); }
        } else if (ring_->isOpen()) {
            // Tool ring: movement is frozen, looking aims at a segment, click / A / trigger launches it.
            updateCamera(dt, 0, 0, 0, lookX, lookY);
            room_->setHover(nullptr);
            ring_->pointer(o, d);
            if (in.wheel != 0.0f) ring_->cycle(in.wheel > 0 ? -1 : 1);
            if (up || in.left) ring_->cycle(-1);
            if (down || in.right) ring_->cycle(1);
            if (ring_->appsMode() && (pointerSelect || keySelect) && (ring_->appHot() || keySelect)) {
                const int idx = ring_->appSelected();
                ring_->close();
#ifdef _WIN32
                launchApp(idx);
#endif
            } else if (pointerSelect || keySelect) runTool(ring_->selected());
            else if (ringWasOpen && ringToggle) ring_->close();
            else if (back || in.escape) {
                if (ring_->appsMode()) ring_->showApps(false);
                else ring_->close();
            }
        } else if (appOpen) {
#ifdef _WIN32
            const float tanHalf = std::tan(kFovY * 0.5f);
            if (!app_->busy()) pickApp(o, d);
            for (auto& a : apps_) if (a->active()) a->setView(renderer_.aspect(), tanHalf);
            if (vrOn_ && app_->fullscreen()) app_->toggleFullscreen();
            if (in.appFullscreen && !vrOn_) app_->toggleFullscreen();
            if (app_->fullscreen()) {
                // Fullscreen: the world is frozen; the mouse / right stick drive a cursor over the program.
                int ww = 1, wh = 1;
                SDL_GetWindowSize(window_, &ww, &wh);
                updateCamera(dt, 0, 0, 0, 0, 0);
                cursorX_ = clampf(cursorX_ + in.lookX * 2.0f / std::max(ww, 1) + pad.rx * 1.5f * fdt, -1.0f, 1.0f);
                cursorY_ = clampf(cursorY_ - in.lookY * 2.0f / std::max(wh, 1) - pad.ry * 1.5f * fdt, -1.0f, 1.0f);
                o = {0, 0, 0};
                d = normalize({cursorX_ * tanHalf * renderer_.aspect(), cursorY_ * tanHalf, -1.0f});
            } else {
                updateCamera(dt, moveX, moveY, kbZ, lookX, lookY, freeSpeed);
            }
            room_->setHover(nullptr);
            app_->pointer(o, d);
            if (in.click) app_->press(false);
            else if (pad.aPressed || vrInput_.selectPressed) app_->press(true);
            if (in.clickUp) app_->releaseButton();
            if (in.wheel != 0.0f) app_->scrollNotches(in.wheel);
            if (appTyping) {
                for (EditKey k : in.keys) app_->key(k);
                if (!in.text.empty()) app_->text(in.text);
            }
            if (in.escape || pad.bPressed || vrInput_.backPressed) app_->escape();
#endif
        } else if (browserOpen) {
#ifdef SCIFI_HAS_CEF
            // Browser open: you can still walk around; input drives the page (or the address bar).
            if (vrOn_ && browser_->fullscreen()) browser_->toggleFullscreen();
            if (browser_->fullscreen()) {
                int ww = 1, wh = 1;
                SDL_GetWindowSize(window_, &ww, &wh);
                const float bt = std::tan(kFovY * 0.5f);
                updateCamera(dt, 0, 0, 0, 0, 0);
                cursorX_ = clampf(cursorX_ + in.lookX * 2.0f / std::max(ww, 1) + pad.rx * 1.5f * fdt, -1.0f, 1.0f);
                cursorY_ = clampf(cursorY_ - in.lookY * 2.0f / std::max(wh, 1) - pad.ry * 1.5f * fdt, -1.0f, 1.0f);
                o = {0, 0, 0};
                d = normalize({cursorX_ * bt * renderer_.aspect(), cursorY_ * bt, -1.0f});
            } else {
                updateCamera(dt, moveX, moveY, kbZ, lookX, lookY, freeSpeed);
            }
            room_->setHover(nullptr);
            browser_->pointer(o, d);
            if (in.click) browser_->press(false);
            else if (pad.aPressed || vrInput_.selectPressed) browser_->press(true);
            if (in.clickUp) browser_->release();
            if (in.wheel != 0.0f) browser_->scrollNotches(in.wheel);
            if (pad.upPressed) browser_->scrollNotches(1.0f);
            if (pad.downPressed) browser_->scrollNotches(-1.0f);
            for (EditKey k : in.keys) {
                const bool scrollKey = k == EditKey::Up || k == EditKey::Down || k == EditKey::PageUp ||
                                       k == EditKey::PageDown || k == EditKey::Home || k == EditKey::End;
                if (!kbOff && (browserTyping || scrollKey)) browser_->key(k);
            }
            if (!in.text.empty() && browserTyping) browser_->text(in.text);
            if (in.focusAddress) browser_->focusAddress();
            if (in.depthCycle) browser_->cycleDepth();
            if (in.depthSite) browser_->toggleSite();
            if (in.reload) browser_->reload();
            if (in.navBack) browser_->goBack();
            if (in.navForward) browser_->goForward();
            if (in.escape) browser_->escape();
            if (pad.bPressed || vrInput_.backPressed) browser_->close();
#endif
        } else if (viewer_->active()) {
            const bool ov = viewer_->overlay();
            if (ov) {
                // Fullscreen: the world is frozen; the mouse / right stick drive a cursor over the screen.
                int ww = 1, wh = 1;
                SDL_GetWindowSize(window_, &ww, &wh);
                updateCamera(dt, 0, 0, 0, 0, 0);
                cursorX_ = clampf(cursorX_ + in.lookX * 2.0f / std::max(ww, 1) + pad.rx * 1.5f * fdt, -1.0f, 1.0f);
                cursorY_ = clampf(cursorY_ - in.lookY * 2.0f / std::max(wh, 1) - pad.ry * 1.5f * fdt, -1.0f, 1.0f);
                const float t = std::tan(kFovY * 0.5f);
                o = {0, 0, 0};
                d = normalize({cursorX_ * t * renderer_.aspect(), cursorY_ * t, -1.0f});
                if (in.click) vDrag_ = true;
                if (in.clickUp) vDrag_ = false;
                if (vDrag_) viewer_->panBy(in.lookX * 2.0f / std::max(ww, 1), -in.lookY * 2.0f / std::max(wh, 1));
                // Images: wheel and analogue triggers zoom. Video / audio: the wheel is volume.
                if (!viewer_->isMedia() && !viewer_->isText()) {
                    if (in.wheel != 0.0f) viewer_->zoomBy(std::pow(1.2f, in.wheel));
                    if (pad.rt != pad.lt) viewer_->zoomBy(std::exp((pad.rt - pad.lt) * 1.6f * fdt));
                } else if (viewer_->isMedia() && in.wheel != 0.0f) {
                    viewer_->changeVolume(0.05f * in.wheel);
                }
            } else {
                vDrag_ = false;
                updateCamera(dt, moveX, moveY, kbZ, lookX, lookY, freeSpeed);
                // Windowed picture: the wheel zooms into the spot under the cursor.
                if (in.wheel != 0.0f && !vrOn_ && !viewer_->isMedia() && !viewer_->isText()) viewer_->zoomAt(o, d, std::pow(1.2f, in.wheel));
            }
            // Media viewer open: you can still walk around; input drives playback / the text pane.
            room_->setHover(nullptr);
            const bool activity = in.lookX != 0.0f || in.lookY != 0.0f || in.click || in.wheel != 0.0f || in.left || in.right ||
                                  up || down || in.space || in.select || in.fullscreen || in.back || in.escape || !in.keys.empty() ||
                                  !in.text.empty() || pad.aPressed || pad.bPressed || pad.leftPressed || pad.rightPressed ||
                                  pad.upPressed || pad.downPressed || std::fabs(pad.rx) > 0.2f || std::fabs(pad.ry) > 0.2f;
            if (activity) viewer_->wake();
            // A click that misses the viewer picks another file card (switch) or empty space (close).
            auto press = [&]() {
                if (viewer_->pointerPress(o, d)) return;
                if (viewer_->overlay()) return;
                Node* card = pickNode(room_->root(), o, d);
                if (card) {
                    if (card->isCard() && !static_cast<CardNode*>(card)->entry.isDir) room_->activate(card);
                    else { viewer_->close(); if (!viewer_->active() || viewer_->closing()) menu_->setStatus(room_->activate(card)); }
                } else {
                    viewer_->close();
                }
            };
            if (!ov && !vrOn_) {
                if (viewer_->dragging()) {
                    viewer_->dragTo(o, d);
                    pointerSelect = false;
                    if (in.clickUp) viewer_->dragEnd();
                } else if (in.click && viewer_->dragStart(o, d)) {
                    pointerSelect = false;
                }
            }
            if (!viewer_->closing() && viewer_->isText()) {
                const bool editing = viewer_->editing();
                if (pointerSelect) press();
                for (EditKey k : in.keys) if (!kbOff) viewer_->editKey(k);
                if (!in.text.empty() && !kbOff) viewer_->typeText(in.text);
                if (in.wheel != 0.0f) viewer_->scrollText(-in.wheel * 3.0f);
                if (!editing) {
                    if (pad.upPressed) viewer_->scrollText(-3.0f);
                    if (pad.downPressed) viewer_->scrollText(3.0f);
                }
                if (!editing && pad.aPressed) viewer_->pointerPress(o, d);
                if (!editing && in.fullscreen) viewer_->toggleFullscreen(head_);
                bool pressedBack = pad.bPressed || vrInput_.backPressed || (!editing && in.back);
                if (in.escape && editing) viewer_->stopEditing();
                else if (pressedBack || in.escape) {
                    if (viewer_->fullscreen()) viewer_->toggleFullscreen(head_);
                    else viewer_->close();
                }
                if (viewer_->wantsClose()) viewer_->close();
            } else if (!viewer_->closing()) {
                if (pointerSelect) press();
                if (ov && pad.aPressed) press();
                else if (keySelect || in.space) viewer_->togglePause();
                const bool image = !viewer_->isMedia();
                if ((in.left && !kbOff) || pad.leftPressed) { if (image) viewer_->step(-1); else viewer_->seekBy(-5.0); }
                if ((in.right && !kbOff) || pad.rightPressed) { if (image) viewer_->step(1); else viewer_->seekBy(5.0); }
                if (up && !kbOff) viewer_->changeVolume(0.1f);
                if (down && !kbOff) viewer_->changeVolume(-0.1f);
                if (in.fullscreen) viewer_->toggleFullscreen(head_);
                if (back || in.escape) {
                    if (viewer_->fullscreen()) viewer_->toggleFullscreen(head_);
                    else viewer_->close();
                }
                if (viewer_->wantsClose()) viewer_->close();
            }
        } else {
            updateCamera(dt, moveX, moveY, kbZ, lookX, lookY, freeSpeed);
            static bool dbgPlaced = std::getenv("SCIFI_CITY_STREET") == nullptr;
            if (cityMode_ && !dbgPlaced && city_ && city_->debugStreet(camera_.position)) {
                dbgPlaced = true;
                flying_ = false;
                camera_.yaw = kPi;
                camera_.pitch = 0.0f;
                camera_.yaw = 0.0f;
            }
            if (cityMode_) {
                room_->setHover(nullptr);
                std::string path, name;
                if (city_ && city_->pick(o, d, path, name)) {
                    if (pointerSelect || keySelect) {
                        city_->markEntered();
                        city_->clearHover();
                        room_->load(path);
                        toggleCity(false);
                        if (room_->building()) {
                            camera_.position = {0, 1.6f, 1.6f};
                            camera_.yaw = 0;
                            camera_.pitch = 0;
                            room_->setEye(camera_.position);
                        } else {
                            camera_.yaw = kPi + kPi * std::min(1.0f, room_->cardCount() / 14.0f);
                        }
                        menu_->setStatus("INSIDE " + name + " - ESC returns to the city");
                    }
                }
            } else {
            if (city_) city_->clearHover();
            Node* hit = pickNode(room_->root(), o, d);
            room_->setHover(hit);
            if ((pointerSelect || keySelect) && hit) menu_->setStatus(room_->activate(hit));
            if (back || (in.escape && room_->inRoom())) menu_->setStatus(room_->goUp());
            else if (in.escape) toggleCity(true);
            }
#ifdef SCIFI_HAS_CEF
            if (in.browser && browser_) browser_->open("", head_.eye, head_.forward, vrOn_);
#endif
        }
    }

    time_ += dt;
    fpsAccum_ += dt;
    ++fpsFrames_;
    if (fpsAccum_ >= 0.5) {
        const int fps = static_cast<int>(fpsFrames_ / fpsAccum_ + 0.5);
        fpsAccum_ = 0;
        fpsFrames_ = 0;
        if (settings_.showFps)
            fpsLabel_ = renderer_.createLabel(makeLabel(std::to_string(fps) + " FPS", 54.0f, FontFace::UI, Color8{}, 96, 4));
    }
    {
        const Theme& th = currentTheme();
        renderer_.setTheme(th.accent, th.alt);
        renderer_.setTime(static_cast<float>(time_));
        room_->setAccent(th.accent);
        ring_->update(dt, time_);
        settingsPanel_->update(dt, time_);
        appsPanel_->update(dt, time_);
    }
    if (state_ == State::FileRoom) {
        room_->pump();
        { Vec3 sp; float sy; if (room_->takeSpawn(sp, sy)) { camera_.position = sp; camera_.yaw = sy; camera_.pitch = 0; room_->setEye(sp); } }
        viewer_->update(dt, head_);
        static bool autoFs = std::getenv("SCIFI_OPEN_FS") != nullptr;  // debugging aid: go fullscreen once settled
        if (autoFs && viewer_->active() && !viewer_->fullscreen() && time_ > 2.0) {
            viewer_->toggleFullscreen(head_);
            autoFs = false;
        }
#ifdef SCIFI_HAS_CEF
        browser_->update(dt, head_.eye, vrOn_);
#endif
#ifdef _WIN32
    for (auto& a : apps_) a->update(dt);
#endif
    }
    if (city_) city_->update(dt, state_ == State::FileRoom, head_.eye);
    activeRoot().updateTree(dt, time_);
}

void AppShell::renderScene(const Mat4& realView, const Mat4& proj, const Vec3& realEye, bool desktopPass) {
    renderer_.setView(realView, proj);
    if (desktopPass != vrOn_) {  // the pass that matches what the user is actually looking through
        head_.eye = realEye;
        head_.forward = realView.rigidInverse().transformDir({0, 0, -1});
        head_.vr = vrOn_;
        head_.aspect = renderer_.aspect();
        head_.tanHalf = std::tan(kFovY * 0.5f);
    }
    // Desktop fullscreen media: the world is not drawn, the viewer fills the screen in view space.
    const bool overlay = state_ == State::FileRoom && desktopPass && !vrOn_ && viewer_->overlay();
    const Mat4 view = overlay ? Mat4::identity() : realView;
    const Vec3 eye = overlay ? Vec3{0, 0, 0} : realEye;
    if (overlay) {
        renderer_.setView(view, proj);
        renderer_.clearDepth();
        viewer_->root().renderTree(renderer_);
        if (viewer_->controlsShown() && crosshair_) {
            const float t = std::tan(kFovY * 0.5f), dist = 1.5f;
            Vec3 p{cursorX_ * t * renderer_.aspect() * dist, cursorY_ * t * dist, -dist};
            renderer_.drawPointer(*crosshair_, pointerMatrix(p, {0, 0, -1}, {0, 1, 0}, 0.1f), currentTheme().accent);
                    }
                    return;
                }
    // Planar reflection: draw the room flipped under the floor first; the translucent floor then lets it show through.
#ifdef _WIN32
    if (state_ == State::FileRoom && desktopPass && !vrOn_ && app_ && app_->active() && app_->fullscreen()) {
        // Fullscreen hosted program: it fills the view; the pointer is a virtual cursor over it.
        renderer_.setView(Mat4::identity(), proj);
        renderer_.clearDepth();
        app_->render(renderer_);
        if (crosshair_ && !switcher_->isOpen()) {
            const float t = std::tan(kFovY * 0.5f), dist = 1.5f;
            Vec3 p{cursorX_ * t * renderer_.aspect() * dist, cursorY_ * t * dist, -dist};
            renderer_.drawPointer(*crosshair_, pointerMatrix(p, {0, 0, -1}, {0, 1, 0}, 0.1f), currentTheme().accent);
        }
        switcher_->render(Mat4::identity(), renderer_.aspect(), std::tan(kFovY * 0.5f), false);
        return;
    }
#endif
    if (state_ == State::FileRoom && desktopPass && !vrOn_ && settingsPanel_->isOpen() && settingsPanel_->fullscreen()) {
        renderer_.setView(Mat4::identity(), proj);
        renderer_.clearDepth();
        settingsPanel_->render();
        if (crosshair_) {
            const float t = std::tan(kFovY * 0.5f), dist = 0.9f;
            Vec3 p{cursorX_ * t * renderer_.aspect() * dist, cursorY_ * t * dist, -dist};
            renderer_.drawPointer(*crosshair_, pointerMatrix(p, {0, 0, -1}, {0, 1, 0}, 0.1f), currentTheme().accent);
        }
        return;
    }
#ifdef SCIFI_HAS_CEF
    if (state_ == State::FileRoom && desktopPass && !vrOn_ && browser_ && browser_->active() && browser_->fullscreen()) {
        renderer_.setView(Mat4::identity(), proj);
        renderer_.clearDepth();
        browser_->render(renderer_);
        if (crosshair_) {
            const float t = std::tan(kFovY * 0.5f), dist = 0.9f;
            Vec3 p{cursorX_ * t * renderer_.aspect() * dist, cursorY_ * t * dist, -dist};
            renderer_.drawPointer(*crosshair_, pointerMatrix(p, {0, 0, -1}, {0, 1, 0}, 0.1f), currentTheme().accent);
        }
        return;
    }
#endif
    if (state_ == State::FileRoom) {
        renderer_.beginReflection();
        renderer_.setMirror(true);
        activeRoot().renderTree(renderer_);
        if (cityMode_ && city_) city_->render(renderer_, realEye);
        renderer_.setMirror(false);
        renderer_.endReflection();
    }
    activeRoot().renderTree(renderer_);
    if (cityMode_ && city_) city_->render(renderer_, realEye);
    if (state_ == State::FileRoom && viewer_->active()) {
        renderer_.clearDepth();
        viewer_->root().renderTree(renderer_);
    }
#ifdef SCIFI_HAS_CEF
    if (state_ == State::FileRoom && browser_ && browser_->active()) {
        renderer_.clearDepth();
        browser_->render(renderer_);
    }
#endif
#ifdef _WIN32
    if (state_ == State::FileRoom) {
        std::vector<AppWindow*> shown;
        for (auto& a : apps_) if (a->active()) shown.push_back(a.get());
        std::sort(shown.begin(), shown.end(), [&](AppWindow* x, AppWindow* y) { return x->distanceTo(realEye) > y->distanceTo(realEye); });
        for (AppWindow* a : shown) {
            renderer_.clearDepth();
            a->render(renderer_);
        }
    }
#endif
    if (state_ == State::FileRoom && ring_->active()) {
        renderer_.clearDepth();
        ring_->render(renderer_);
    }
    if (state_ == State::FileRoom && settingsPanel_->isOpen()) settingsPanel_->render();
    if (state_ == State::FileRoom && appsPanel_->isOpen()) {
        renderer_.clearDepth();
        appsPanel_->root().renderTree(renderer_);
    }
    if (switcher_->isOpen()) switcher_->render(view, renderer_.aspect(), std::tan(kFovY * 0.5f), vrOn_);
    if (settings_.showFps && fpsLabel_) {
        // Head-locked readout in the top-left of the view.
        renderer_.clearDepth();
        const float h = 0.07f, w = h * fpsLabel_->aspect;
        const float dist = 1.2f, halfH = std::tan(kFovY * 0.5f) * dist;
        Vec3 at = vrOn_ ? Vec3{-0.28f, 0.2f, -dist} : Vec3{-halfH * renderer_.aspect() + w * 0.6f + 0.05f, halfH - h * 0.6f - 0.04f, -dist};
        PanelParams pp;
        pp.time = static_cast<float>(time_);
        pp.aspect = w / h;
        pp.radius = 0.35f;
        pp.selected = 0.0f;
        pp.segments = 0.0f;
        renderer_.drawPanel(*fpsLabel_, view.rigidInverse() * Mat4::translate(at) * Mat4::scale({w, h, 1.0f}),
                            currentTheme().accent, pp);
    }
    if (vrOn_ && vrInput_.valid) {
        renderer_.drawCube(*crosshair_, alignZ(vrInput_.pointerOrigin + vrInput_.pointerDir * 1.5f, vrInput_.pointerDir,
                                                {0.006f, 0.006f, 3.0f}),
                           {1.0f, 0.7f, 0.2f});
    } else if (state_ == State::FileRoom && settings_.crosshair) {
        Vec3 p = eye + aimDir_ * 2.0f;
        renderer_.drawPointer(*crosshair_, pointerMatrix(p, aimDir_, {0, 1, 0}, 0.12f), currentTheme().accent);
    }
}

int AppShell::run() {
    Uint64 prev = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    while (running_) {
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = std::min(0.1, static_cast<double>(now - prev) / freq);
        prev = now;
        tick(dt);

        if (vrOn_) {
            // In VR the rig is the tracking-space origin: floor level, at the player's position and heading.
            Mat4 rig = Mat4::translate({camera_.position.x, 0, camera_.position.z}) * Mat4::rotateY(camera_.yaw);
            vr_.frame(
                renderer_, rig,
                [&](const EyeView& ev) {
                    Vec3 eyePos = ev.view.rigidInverse().transformPoint({0, 0, 0});
                    renderScene(ev.view, ev.proj, eyePos, false);
                },
                vrInput_);
        }
        renderer_.beginDesktopFrame();
        renderScene(camera_.world().rigidInverse(), Mat4::perspective(kFovY, renderer_.aspect(), 0.05f, 2500.0f),
                    camera_.position, true);
        renderer_.present(!vrOn_);
        if (shotPending_) {
            shotPending_ = false;
            saveScreenshot();
        }
    }
    fastExit();
}

}  // namespace sf

namespace sf {

// A file's own room: everything we know about it plus action cards (open, reveal, copy, send, delete...).
void AppShell::openFileRoom(const DirEntry& entry) {
    namespace fs = std::filesystem;
    const fs::path p = fs::u8path(entry.path);
    std::error_code ec;
    auto sizeText = [](uintmax_t b) {
        const char* u[] = {"B", "KB", "MB", "GB", "TB"};
        double v = static_cast<double>(b);
        int i = 0;
        while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
        char buf[48];
        std::snprintf(buf, sizeof buf, i ? "%.1f %s" : "%.0f %s", v, u[i]);
        return std::string(buf);
    };
    auto timeOf = [&](bool created) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return std::string("-");
        FILETIME ft = created ? d.ftCreationTime : d.ftLastWriteTime, lt;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&ft, &lt);
        FileTimeToSystemTime(&lt, &st);
        char buf[48];
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        return std::string(buf);
    };
    const uintmax_t size = fs::file_size(p, ec);
    std::string ext = p.extension().u8string();
    if (ext.empty()) ext = "none";
    DWORD attr = GetFileAttributesW(p.c_str());
    std::string attrs;
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (attr & FILE_ATTRIBUTE_READONLY) attrs += "READ-ONLY ";
        if (attr & FILE_ATTRIBUTE_HIDDEN) attrs += "HIDDEN ";
        if (attr & FILE_ATTRIBUTE_SYSTEM) attrs += "SYSTEM ";
        if (attr & FILE_ATTRIBUTE_ARCHIVE) attrs += "ARCHIVE ";
    }
    if (attrs.empty()) attrs = "NORMAL";

    const std::string path = entry.path;
    const std::string folder = room_->currentPath();
    deleteArmed_ = 0;
    auto info = [](std::string t, std::string h, std::string v) {
        return SettingsItem{std::move(t), std::move(h), [v] { return v; }, [] {}};
    };
    SettingsCategory cat{"FILE", entry.name, "FILE", {}};
    cat.items.push_back(info("NAME", "File name", entry.name));
    cat.items.push_back(info("TYPE", "Extension", ext));
    cat.items.push_back(info("SIZE", "On disk", ec ? "-" : sizeText(size)));
    cat.items.push_back(info("MODIFIED", "Last written", timeOf(false)));
    cat.items.push_back(info("CREATED", "Created", timeOf(true)));
    cat.items.push_back(info("ATTRIBUTES", "Windows flags", attrs));
    cat.items.push_back({"OPEN", "Open with the default program", nullptr, [this, path] {
        ShellExecuteW(nullptr, L"open", fs::u8path(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }});
    cat.items.push_back({"COPY FILE", "Put the file on the clipboard (paste in Explorer)", nullptr, [this, path] {
        std::wstring w = fs::u8path(path).wstring();
        const size_t bytes = sizeof(DROPFILES) + (w.size() + 2) * sizeof(wchar_t);
        HGLOBAL g = GlobalAlloc(GHND, bytes);
        if (!g) return;
        auto* df = static_cast<DROPFILES*>(GlobalLock(g));
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        std::memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), w.c_str(), w.size() * sizeof(wchar_t));
        GlobalUnlock(g);
        if (OpenClipboard(nullptr)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_HDROP, g)) GlobalFree(g);
            CloseClipboard();
        } else {
            GlobalFree(g);
        }
        menu_->setStatus("File copied to clipboard");
    }});
    cat.items.push_back({"COPY PATH", "Copy the full path as text", nullptr, [this, path] {
        SDL_SetClipboardText(path.c_str());
        menu_->setStatus("Path copied");
    }});
    cat.items.push_back({"SEND TO DESKTOP", "Place a copy on the Desktop", nullptr, [this, path] {
        wchar_t desk[MAX_PATH]{};
        if (SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk) != S_OK) return;
        std::error_code e;
        fs::copy_file(fs::u8path(path), fs::path(desk) / fs::u8path(path).filename(), fs::copy_options::skip_existing, e);
        menu_->setStatus(e ? "Could not send file" : "Sent to Desktop");
    }});
    cat.items.push_back({"DUPLICATE", "Make a copy next to the original", nullptr, [this, path, folder] {
        fs::path src = fs::u8path(path), dst;
        for (int i = 1; i < 1000; ++i) {
            dst = src.parent_path() / (src.stem().wstring() + L" - Copy" + (i > 1 ? L" " + std::to_wstring(i) : L"") + src.extension().wstring());
            if (!fs::exists(dst)) break;
        }
        std::error_code e;
        fs::copy_file(src, dst, e);
        if (!e) room_->refresh();
        menu_->setStatus(e ? "Could not duplicate" : "Duplicated");
    }});
    cat.items.push_back({"RENAME", "Type a new name, Enter to apply, Esc to cancel",
        [this] { return renaming_ ? renameBuf_ + "|" : std::string(); },
        [this, path, name = entry.name] { renaming_ = true; renamePath_ = path; renameBuf_ = name; }});
    cat.items.push_back({"REVEAL IN EXPLORER", "Show the containing folder", nullptr, [this, path] {
        std::wstring args = L"/select,\"" + fs::u8path(path).wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }});
    cat.items.push_back({"DELETE", "Move to the Recycle Bin (click twice)",
        [this] { return std::string(deleteArmed_ ? "CLICK AGAIN TO CONFIRM" : ""); },
        [this, path, folder] {
            if (!deleteArmed_) { deleteArmed_ = 1; return; }
            deleteArmed_ = 0;
            std::wstring w = fs::u8path(path).wstring();
            w.push_back(L'\0');
            SHFILEOPSTRUCTW op{};
            op.wFunc = FO_DELETE;
            op.pFrom = w.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
            const bool ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
            settingsPanel_->close();
            if (ok) room_->refresh();
            menu_->setStatus(ok ? "Moved to Recycle Bin" : "Delete failed");
        }});

    std::vector<SettingsCategory> cats;
    cats.push_back(std::move(cat));
    settingsPanel_->setCategories(std::move(cats));
    settingsPanel_->open(vrOn_ ? head_.eye : camera_.position,
                         vrOn_ ? head_.forward : Vec3{-std::sin(camera_.yaw), 0.0f, -std::cos(camera_.yaw)});
    settingsPanel_->openPage("FILE");
}

}  // namespace sf
