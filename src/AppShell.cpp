#include "AppShell.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "TextRender.h"
#include "Theme.h"
#include "PluginScript.h"
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

Vec3 pluginColour(const std::string& hex) {
    auto byte = [&](size_t i) { return static_cast<float>(std::stoul(hex.substr(i, 2), nullptr, 16)) / 255.0f; };
    return {byte(1), byte(3), byte(5)};
}

// Flat arrow pointer for fullscreen windows: white fill, dark outline, tip at the bitmap's top-left corner.
Bitmap makeArrowBitmap() {
    constexpr int N = 48;
    const float px[7] = {1.5f, 1.5f, 10.0f, 17.0f, 23.0f, 16.0f, 28.0f}, py[7] = {1.5f, 33.0f, 25.5f, 40.0f, 37.0f, 23.0f, 22.5f};
    auto inside = [&](float x, float y) {
        bool in = false;
        for (int i = 0, j = 6; i < 7; j = i++)
            if ((py[i] > y) != (py[j] > y) && x < (px[j] - px[i]) * (y - py[i]) / (py[j] - py[i]) + px[i]) in = !in;
        return in;
    };
    auto edgeDist = [&](float x, float y) {
        float best = 1e9f;
        for (int i = 0, j = 6; i < 7; j = i++) {
            const float dx = px[j] - px[i], dy = py[j] - py[i];
            const float t = std::clamp(((x - px[i]) * dx + (y - py[i]) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
            best = std::min(best, std::hypot(x - (px[i] + t * dx), y - (py[i] + t * dy)));
        }
        return best;
    };
    Bitmap b;
    b.w = b.h = N;
    b.rgba.assign(static_cast<size_t>(N) * N * 4, 0);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            float fill = 0, cover = 0;
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx) {
                    const float fx = x + (sx + 0.5f) / 3, fy = y + (sy + 0.5f) / 3;
                    const bool in = inside(fx, fy);
                    if (in) fill += 1.0f / 9;
                    if (in || edgeDist(fx, fy) < 2.0f) cover += 1.0f / 9;
                }
            uint8_t* p = &b.rgba[(static_cast<size_t>(y) * N + x) * 4];
            const float c = 255.0f * fill + 18.0f * (1.0f - fill);
            p[0] = p[1] = static_cast<uint8_t>(c);
            p[2] = static_cast<uint8_t>(255.0f * fill + 30.0f * (1.0f - fill));
            p[3] = static_cast<uint8_t>(255.0f * cover);
        }
    return b;
}
}  // namespace

AppShell::AppShell() = default;

AppShell::~AppShell() {
    plugins_.shutdown();
    sfx_.shutdown();
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
    start_.reset();
    settingsPanel_.reset();
    menu_.reset();
    crosshair_.reset();
    arrow_.reset();
    fpsLabel_.reset();
    hudTimeLabel_.reset();
    hudDateLabel_.reset();
    hudTargetLabel_.reset();
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
    sfx_.init();
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
    settings_.load();
    sfx_.setMasterVolume(settings_.sfxVol);
    renderer_.setMsaaPreference(settings_.aaIdx);
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
    settings_.load();
    city_ = std::make_unique<City>(renderer_);
    const bool drivingRegistered = plugins_.add(makeDrivingPlugin());
    const bool parksRegistered = plugins_.add(makeParksPlugin());
    if (!drivingRegistered || !parksRegistered) {
        std::fprintf(stderr, "Failed to register built-in plugins\n");
        return false;
    }
    const auto statePath = PluginManager::defaultStatePath();
    pluginPackages_ = std::make_unique<PluginPackageStore>(
        statePath.empty() ? std::filesystem::path{} : statePath.parent_path() / "plugins",
        [this](const ThemePackage& p) {
            const Vec3 accent = pluginColour(p.accent), alt = pluginColour(p.alt);
            const bool added = plugins_.add({p.id, p.name, p.description, p.version,
                                 [p, accent, alt] { registerPluginTheme(p.id, p.name, accent, alt); },
                                 [id = p.id] { unregisterPluginTheme(id); }, false});
            if (added) installedPluginIds_.insert(p.id);
            return added;
        },
        [this](const ScriptPackage& p) {
            Plugin plugin{p.id, p.name, p.description, p.version, [] {}, [] {}, false};
            plugin.prepare = [script = p.script](std::string& error) {
                std::string result;
                return runPluginScript(script, result, error);
            };
            if (!plugins_.add(std::move(plugin))) return false;
            installedPluginIds_.insert(p.id);
            pluginScripts_[p.id] = p.script;
            return true;
        });
    pluginPackages_->discover();
    if (!pluginPackages_->error().empty()) std::fprintf(stderr, "%s\n", pluginPackages_->error().c_str());
    plugins_.initialize();
    if (settings_.showCity) {
        city_->start();
        cityStarted_ = true;
    }
    viewer_ = std::make_unique<Viewer>(renderer_);
    switcher_ = std::make_unique<TaskSwitcher>(renderer_);
    settingsPanel_ = std::make_unique<SettingsHub>(renderer_);
    osk_ = std::make_unique<OnScreenKeyboard>(renderer_);
    harvi_ = std::make_unique<Harvi>(renderer_);
    {
        StartHost h;
        h.tool = [this](Tool t) { syncWins(); runTool(t); };
        h.focusRow = [this](int i) { syncWins(); startFocusRow(i); };
        h.action = [this](const std::string& a) { syncWins(); startAction(a); };
        h.rows = [this] { return startRows(); };
        start_ = std::make_unique<StartMenu>(renderer_, std::move(h));
    }
    monitor_ = std::make_unique<SysMonitor>(renderer_, [this] { return city_ ? city_->sysStats() : SystemStats{}; });
    if (!mediaLib_) mediaLib_ = std::make_unique<MediaLibrary>();
    {
        MediaHost h;
        h.tracks = [this] { return mediaLib_->tracks(); };
        h.count = [this] { return mediaLib_->count(); };
        h.scanning = [this] { return mediaLib_->scanning(); };
        h.scanned = [this] { return mediaLib_->scanned(); };
        h.addDefaultFolders = [this] {
            const std::string home = homeDirectory();
            for (const char* n : {"Music", "Videos"}) {
                const std::string p = home + "\\" + n;
                if (std::filesystem::exists(std::filesystem::u8path(p)) && !mediaLib_->hasFolder(p)) mediaLib_->addFolder(p);
            }
            mediaLib_->rescan();
        };
        h.pickFolders = [this] {
            openPanel(files2d_.get());
            files2d_->folderHandler = [this](const std::vector<std::string>& folders) {
                addMediaFolders(folders);
                openPanel(media_.get());
            };
        };
        h.pickBusy = [this] { return files2d_ && static_cast<bool>(files2d_->folderHandler); };
        h.folders = [this] { return mediaLib_->folders(); };
        h.removeFolder = [this](const std::string& p) { mediaLib_->removeFolder(p); };
        h.openFolderSettings = [this] {
            if (!settingsPanel_->isOpen()) buildSettingsHub();
            openSettingsWin();
            settingsPanel_->openPage("MEDIA FOLDERS");
        };
        h.play = [this](const std::vector<std::string>& p, int i) { mediaPlayList(p, i); };
        h.playStream = [this](const std::string& n, const std::string& u) { mediaPlayStream(n, u); };
        h.now = [this] { return mediaNow(); };
        h.toggle = [this] {
            if (!mediaPlayer_.isOpen()) return;
            if (mediaPlayer_.playing()) mediaPlayer_.pause();
            else mediaPlayer_.play();
        };
        h.next = [this] { mediaNext(1); };
        h.prev = [this] { mediaNext(-1); };
        h.toggleShuffle = [this] { shuffle_ = !shuffle_; };
        h.cycleRepeat = [this] { repeat_ = (repeat_ + 1) % 3; };
        h.seek = [this](double s) { if (mediaPlayer_.isOpen()) mediaPlayer_.seek(s); };
        h.setVolume = [this](float v) { mediaVol_ = v; mediaPlayer_.setVolume(v); };
        h.spectrum = [this](float* b, int n) { return mediaPlayer_.isOpen() && mediaPlayer_.spectrum(b, n); };
        h.videoFrame = [this](std::vector<uint8_t>& px, int& w, int& hh) {
            return mediaPlayer_.isOpen() && mediaPlayer_.hasVideo() && mediaPlayer_.takeFrame(px, w, hh);
        };
        h.videoTarget = [this](int w, int hh) { mediaPlayer_.setMaxVideoSize(w, hh); };
        media_ = std::make_unique<MediaApp>(renderer_, std::move(h));
    }
    terminal_ = std::make_unique<Terminal>(renderer_);
    editor2d_ = std::make_unique<TextEditor>(renderer_);
    editor2d_->requestOpen = [this] {
        openPanel(files2d_.get());
        files2d_->pickHandler = [this](const std::string& path) {
            openPanel(editor2d_.get());
            editor2d_->openPath(path);
        };
    };
    imageEd_ = std::make_unique<ImageEditor>(renderer_);
    imageEd_->requestOpen = [this] {
        openPanel(files2d_.get());
        files2d_->pickHandler = [this](const std::string& path) {
            openPanel(imageEd_.get());
            imageEd_->openPath(path);
        };
    };
    imageEd_->takeShot = [this](const std::string& out) {
        shotPath_ = out;
        shotPending_ = true;
    };
    imageEd_->restore = [this] { openPanel(imageEd_.get()); };
    ftp_ = std::make_unique<FtpApp>(renderer_);
    calc_ = std::make_unique<Calculator>(renderer_);
    ftp_->requestUpload = [this] {
        openPanel(files2d_.get());
        files2d_->pickHandler = [this](const std::string& path) {
            openPanel(ftp_.get());
            ftp_->upload(path);
        };
    };
    files2d_ = std::make_unique<FileBrowser>(renderer_);
    files2d_->openFile = [this](const std::string& path) {
        namespace fs = std::filesystem;
        const FileKind k = classifyFile(path, false);
        if (k == FileKind::Other) {
            ShellExecuteW(nullptr, L"open", fs::u8path(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        }
        OpenRequest req;
        req.path = path;
        req.name = fs::u8path(path).filename().u8string();
        req.kind = k;
        if (k == FileKind::Video || k == FileKind::Audio) {
            mediaOpenFile(path);
            return;
        }
        if (k == FileKind::Text) {
            openPanel(editor2d_.get());
            editor2d_->openPath(path);
            return;
        }
        if (k == FileKind::Image) {
            openPanel(imageEd_.get());
            imageEd_->openPath(path);
            return;
        }
        const Vec3 f = camera_.forward();
        req.cardPos = camera_.position + f * 2.0f;
        req.cardYaw = camera_.yaw;
        viewer_->open(req, head_);
    };
    files2d_->openRoom = [this](const std::string& folder) {
        if (deferred_) return;
        if (city_) { city_->markEntered(); city_->clearHover(); }
        deferred_ = [this, folder] {
            room_->load(folder);
            if (cityMode_) toggleCity(false);
            if (room_->building()) {
                camera_.position = {0, 1.6f, 1.6f};
                camera_.yaw = 0;
                camera_.pitch = 0;
                room_->setEye(camera_.position);
            } else {
                camera_.yaw = kPi + kPi * std::min(1.0f, room_->cardCount() / 14.0f);
            }
            menu_->setStatus("INSIDE " + folder);
        };
        deferDelay_ = 3;
    };
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
    room_->onOpenMedia = [this](const OpenRequest& req) {
        if (req.kind == FileKind::Video || req.kind == FileKind::Audio) mediaOpenFile(req.path);
        else if (req.kind == FileKind::Text) {
            openPanel(editor2d_.get());
            editor2d_->openPath(req.path);
        } else if (req.kind == FileKind::Image) {
            openPanel(imageEd_.get());
            imageEd_->openPath(req.path);
        } else viewer_->open(req, head_);
    };
    room_->onOpenFile = [this](const DirEntry& e) { openFileRoom(e); };
    room_->onExitBuilding = [this] { toggleCity(true); };
    viewer_->onStep = [this](int dir) { room_->stepFrom(viewer_->path(), dir); };
    crosshair_ = renderer_.createLabel(rasterizeLines({""}, 1, 4, 4));
    arrow_ = renderer_.createLabel(makeArrowBitmap());
    {
        Bitmap lb;
        lb.w = lb.h = 1;
        lb.rgba = {255, 255, 255, 255};
        loadLabel_ = renderer_.createLabel(lb);
    }
    if (std::getenv("SCIFI_START_MENU")) {  // debugging aid: the old start menu
        state_ = State::Menu;
    } else {
        enterFileRoom();
        if (std::getenv("SCIFI_OPEN_START")) toggleStart();  // debugging aid
#ifdef _WIN32
        // Installed builds quietly check GitHub for a newer release; the script only prompts if one exists.
        if (!std::getenv("SCIFI_NO_UPDATE") && std::filesystem::exists("update.ps1"))
            ShellExecuteA(nullptr, "open", "powershell.exe", "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File update.ps1", nullptr, SW_HIDE);
#endif
        if (std::getenv("SCIFI_OPEN_SETTINGS")) settingsPanel_->open(head_.eye, head_.forward);  // debugging aid
        if (!std::getenv("SCIFI_START_PATH") && !std::getenv("SCIFI_NO_CITY")) toggleCity(true);  // SCIFI_NO_CITY: skip the city build when testing UI  // the city is the home view
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
    plugins_.shutdown();
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
    else room_->loadEmpty();  // start with just the grid; the city is the home view
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
    shell.items.push_back({"SOUND EFFECTS", "Effects and car volume", [this, pct] { return pct(settings_.sfxVol); },
                           [this, save] {
                               settings_.sfxVol = settings_.sfxVol <= 0.01f ? 1.0f : settings_.sfxVol - 0.25f;
                               sfx_.setMasterVolume(settings_.sfxVol);
                               sfx_.play(SfxId::Pickup);
                               save();
                           }});
    shell.items.push_back({"INVERT LOOK Y", "Flip vertical look", [this, onOff] { return onOff(settings_.invertY); },
                           [this, save] { settings_.invertY = !settings_.invertY; save(); }});
    SettingsCategory gfx{"GRAPHICS", "Quality, lighting and fog", "SHELL", {}};
    gfx.items.push_back({"CROSSHAIR", "Aiming dot", [this, onOff] { return onOff(settings_.crosshair); },
                           [this, save] { settings_.crosshair = !settings_.crosshair; save(); }});
    gfx.items.push_back({"FPS COUNTER", "Frame rate readout", [this, onOff] { return onOff(settings_.showFps); },
                           [this, save] { settings_.showFps = !settings_.showFps; save(); }});
    gfx.items.push_back({"BROWSER RESOLUTION", "Web page render quality",
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
    gfx.items.push_back({"DISPLAY", "Shell window size", [this] {
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

    gfx.items.push_back({"CITY", "Show the city (F9). Off saves a lot of GPU and memory", [this, onOff] { return onOff(settings_.showCity); },
                         [this] { setCityEnabled(!settings_.showCity); }});
    gfx.items.push_back({"ANTI-ALIASING", "Edge smoothing (restart to apply)",
                         [this] {
                             static const char* n[] = {"OFF", "2X", "4X"};
                             std::string s = n[settings_.aaIdx];
                             if (static_cast<int>(renderer_.msaaSamples()) != (settings_.aaIdx == 0 ? 1 : settings_.aaIdx == 1 ? 2 : 4)) s += " (RESTART)";
                             return s;
                         },
                         [this, save] { settings_.aaIdx = (settings_.aaIdx + 1) % 3; save(); }});
    gfx.items.push_back({"DRAW DISTANCE", "How far the city is drawn",
                         [this] {
                             static const char* n[] = {"LOW", "MEDIUM", "HIGH", "ULTRA"};
                             return std::string(n[settings_.distIdx]);
                         },
                         [this, save] { settings_.distIdx = (settings_.distIdx + 1) % 4; save(); }});
    gfx.items.push_back({"REFLECTIONS", "Mirrored floor (large saving when off)", [this, onOff] { return onOff(settings_.reflections); },
                         [this, save] { settings_.reflections = !settings_.reflections; save(); }});
    gfx.items.push_back({"POST EFFECTS", "Bloom, light streaks, colour grade", [this, onOff] { return onOff(settings_.postFx); },
                         [this, save] { settings_.postFx = !settings_.postFx; save(); }});
    gfx.items.push_back({"VSYNC", "Sync to monitor refresh", [this, onOff] { return onOff(settings_.vsync); },
                         [this, save] { settings_.vsync = !settings_.vsync; save(); }});
    auto slider = [this, &gfx](const char* title, float* v, float lo, float hi) {
        SettingsItem it;
        it.title = title;
        it.value = [v] { char b[16]; std::snprintf(b, sizeof b, "%.2f", *v); return std::string(b); };
        it.sliderGet = [v] { return *v; };
        it.sliderSet = [this, v](float x) { *v = x; settings_.save(); };
        it.lo = lo;
        it.hi = hi;
        gfx.items.push_back(std::move(it));
    };
    slider("SPOT LIGHT OUTPUT", &settings_.lightSpot, 0.0f, 3.0f);
    slider("SPOT FALLOFF", &settings_.lightFalloff, 0.04f, 0.6f);
    slider("WALL BRIGHTNESS CAP", &settings_.lightCap, 0.02f, 1.5f);
    slider("WALL BASE LEVEL", &settings_.lightBase, 0.0f, 0.08f);
    slider("AMBIENT", &settings_.lightAmbient, 0.0f, 3.0f);
    gfx.items.push_back({"RESET LIGHTING", "Back to the default interior lighting", nullptr, [this, save] {
                             settings_.lightSpot = 1.07f; settings_.lightCap = 0.02f; settings_.lightBase = 0.01f;
                             settings_.lightAmbient = 0.0f; settings_.lightFalloff = 0.6f;
                             save();
                         }});
    auto fogSlider = [this, &gfx](const char* title, const char* hint, float* v, float lo, float hi) {
        SettingsItem it;
        it.title = title;
        it.hint = hint;
        it.value = [v] { char b[16]; std::snprintf(b, sizeof b, "%.2f", *v); return std::string(b); };
        it.sliderGet = [v] { return *v; };
        it.sliderSet = [this, v](float x) { *v = x; settings_.save(); };
        it.lo = lo;
        it.hi = hi;
        gfx.items.push_back(std::move(it));
    };
    fogSlider("FOG DENSITY", "How thick the fog is with distance", &settings_.fogDensity, 0.0f, 3.0f);
    fogSlider("FOG HEIGHT", "How high it climbs up the towers", &settings_.fogHeight, 0.5f, 3.0f);
    fogSlider("FOG BRIGHTNESS", "Lower keeps floors dark", &settings_.fogBright, 0.3f, 3.0f);
    fogSlider("FOG COLOUR", "Violet, blue, teal, grey", &settings_.fogTint, 0.0f, 1.0f);
    fogSlider("FOG START DISTANCE", "Clear space around you", &settings_.fogStart, 0.0f, 150.0f);
    fogSlider("GROUND MIST", "Low bank hugging the streets", &settings_.fogMist, 0.0f, 3.0f);
    fogSlider("FLOOR CLARITY", "Keeps floors dark seen from above", &settings_.fogClarity, 0.1f, 1.0f);
    fogSlider("NEON GLOW", "How much the fog catches neon", &settings_.fogGlow, 0.0f, 0.4f);
    gfx.items.push_back({"RESET FOG", "Back to the default fog", nullptr, [this, save] {
                             settings_.fogDensity = 1.6f; settings_.fogHeight = 1.0f; settings_.fogBright = 1.0f;
                             settings_.fogTint = 0.66f; settings_.fogStart = 0.0f; settings_.fogMist = 1.0f;
                             settings_.fogClarity = 0.5f; settings_.fogGlow = 0.07f;
                             save();
                         }});
    cats.push_back(std::move(gfx));

    SettingsCategory wm{"WINDOWS", "Arrange and move windows", "SHELL", {}};
    wm.items.push_back({"ARRANGE IN ARC", "Curve open windows around you", nullptr, [this] { arrangeArc(); }});
    wm.items.push_back({"TILE WINDOWS", "Lay open windows out in a grid", nullptr, [this] { arrangeTile(); }});
    wm.items.push_back({"MINIMISE ALL", "Tuck every window away", nullptr, [this] { minimiseAll(); }});
    wm.items.push_back({"ARRANGE KEY", "Press to change, then tap the new key (Esc cancels)",
                        [this] {
                            if (input_.capturing) return std::string("PRESS A KEY...");
                            return std::string(SDL_GetKeyName(settings_.arrangeKey ? settings_.arrangeKey : SDLK_F8));
                        },
                        [this] { input_.capturing = true; }});
    wm.items.push_back({"CARRY WINDOWS", "Windows follow you between city, lobby and rooms", [this, onOff] { return onOff(settings_.carryWins); },
                        [this, save] { settings_.carryWins = !settings_.carryWins; save(); }});
    wm.items.push_back({"SNAPPING", "Dragged windows snap to neighbours and eye height", [this, onOff] { return onOff(settings_.snapWins); },
                        [this, save] { settings_.snapWins = !settings_.snapWins; save(); }});
    cats.push_back(std::move(wm));

    SettingsCategory plugins{"PLUGINS", "Built-in features, themes and restricted scripts", "SHELL", {}};
    plugins.items.push_back({"INSTALL PACKAGE", "Choose a .zip script or .sfplugin theme (no permissions)",
                             nullptr, [this] {
                                 openPanel(files2d_.get());
                                 files2d_->pickHandler = [this](const std::string& path) {
                                     const bool installed = pluginPackages_->install(std::filesystem::u8path(path));
                                     buildSettingsHub();
                                     openSettingsWin();
                                     settingsPanel_->openPage("PLUGINS");
                                     settingsPanel_->setNotice(installed ? "PLUGIN INSTALLED - manage it in Plugins" : pluginPackages_->error());
                                 };
                             }});
    for (const auto& plugin : plugins_.plugins()) {
        const auto id = plugin.id;
        if (pluginScripts_.count(id)) {
            plugins.items.push_back({"RUN " + plugin.name, id + " - run this restricted script",
                                 [this, id] {
                                     const auto it = pluginMessages_.find(id);
                                     return it == pluginMessages_.end() ? std::string("SELECT TO RUN") : it->second;
                                 }, [this, id] {
                                     bool running = false;
                                     for (const auto& p : plugins_.plugins()) if (p.id == id) running = p.running;
                                     if (!running) { settingsPanel_->setNotice("ENABLE THE PLUGIN FIRST"); return; }
                                     std::string result, error;
                                     if (runPluginScript(pluginScripts_.at(id), result, error)) pluginMessages_[id] = result;
                                     else pluginMessages_[id] = "ERROR: " + error;
                                     settingsPanel_->setNotice(pluginMessages_[id]);
                                 }});
        }
        if (installedPluginIds_.count(id)) {
            plugins.items.push_back({"REMOVE " + plugin.name, id + " - click twice to uninstall",
                                 [this, id] {
                                     return uninstallConfirm_ == id && std::chrono::steady_clock::now() < uninstallDeadline_
                                         ? std::string("CONFIRM REMOVE") : std::string();
                                 },
                                 [this, id] {
                                     if (uninstallConfirm_ != id || std::chrono::steady_clock::now() >= uninstallDeadline_) {
                                         uninstallConfirm_ = id;
                                         uninstallDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                                         settingsPanel_->setNotice("CLICK REMOVE AGAIN WITHIN 15 SECONDS");
                                         return;
                                     }
                                     uninstallConfirm_.clear();
                                     if (!plugins_.setEnabled(id, false)) { settingsPanel_->setNotice(plugins_.error()); return; }
                                     if (!pluginPackages_->uninstall(id)) { settingsPanel_->setNotice(pluginPackages_->error()); return; }
                                     if (!plugins_.remove(id)) { settingsPanel_->setNotice(plugins_.error()); return; }
                                     pluginScripts_.erase(id);
                                     pluginMessages_.erase(id);
                                     installedPluginIds_.erase(id);
                                     buildSettingsHub();
                                     settingsPanel_->openPage("PLUGINS");
                                     settingsPanel_->setNotice("PLUGIN UNINSTALLED");
                                 }});
        }
        plugins.items.push_back({plugin.name, plugin.description + " - v" + plugin.version + " - " + id,
                                 [this, id, onOff] {
                                     for (const auto& p : plugins_.plugins()) if (p.id == id)
                                         return p.enabled && !p.running ? std::string("ERROR") : onOff(p.running);
                                     return std::string("UNAVAILABLE");
                                 },
                                 [this, id] {
                                     for (const auto& p : plugins_.plugins()) {
                                         if (p.id == id) {
                                             if (!plugins_.setEnabled(id, !p.enabled)) settingsPanel_->setNotice(plugins_.error());
                                             else {
                                                 if (!p.enabled) pluginMessages_.erase(id);
                                                 settingsPanel_->setNotice(p.running ? "PLUGIN ENABLED" : "PLUGIN DISABLED");
                                             }
                                             break;
                                         }
                                     }
                                 }});
    }
    plugins.items.push_back({"PLUGIN STORAGE", "Status of plugin preferences",
                             [this] { return plugins_.error().empty() ? std::string("OK") : plugins_.error(); }, nullptr});
    plugins.items.push_back({"PACKAGES", "Discovery and install status",
                             [this] { return pluginPackages_->error().empty() ? std::string("OK") : pluginPackages_->error(); }, nullptr});
    cats.push_back(std::move(plugins));

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
    for (SettingsCategory& c : mediaCategories()) cats.push_back(std::move(c));
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
            const std::filesystem::path out = shotPath_.empty() ? dir / name : std::filesystem::path(shotPath_);
            shotPath_.clear();
            ok = stbi_write_png(out.string().c_str(), w, h, 4, bits, w * 4) != 0;
            if (ok) std::fprintf(stderr, "Screenshot saved: %s\n", out.string().c_str());
        }
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    MessageBeep(ok ? MB_OK : MB_ICONERROR);  // audible confirmation: nothing is drawn on screen
#endif
}

std::vector<AppShell::WinRef> AppShell::allWins() {
    std::vector<WinRef> v;
#ifdef SCIFI_HAS_CEF
    if (browser_) v.push_back({kWinBrowser, browser_.get()});
#endif
#ifdef _WIN32
    for (auto& a : apps_) v.push_back({kWinApp, a.get()});
#endif
    for (PanelApp* p : {static_cast<PanelApp*>(monitor_.get()), static_cast<PanelApp*>(terminal_.get()), static_cast<PanelApp*>(start_.get()),
                        static_cast<PanelApp*>(files2d_.get()), static_cast<PanelApp*>(editor2d_.get()), static_cast<PanelApp*>(imageEd_.get()), static_cast<PanelApp*>(ftp_.get()), static_cast<PanelApp*>(calc_.get()), static_cast<PanelApp*>(calc_.get()),
                        static_cast<PanelApp*>(media_.get())})
        if (p) v.push_back({kWinPanel, p});
    v.push_back({kWinSettings, settingsPanel_.get()});
    return v;
}

bool AppShell::winOpen(WinRef w) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: return static_cast<WebBrowser*>(w.p)->active();
#endif
#ifdef _WIN32
        case kWinApp: return static_cast<AppWindow*>(w.p)->active();
#endif
        case kWinPanel: return static_cast<PanelApp*>(w.p)->isOpen();
        case kWinSettings: return static_cast<SettingsHub*>(w.p)->isOpen();
        default: return false;
    }
}

// Keeps winOrder_ equal to the set of open windows: a window that has just opened goes to the front with focus.
void AppShell::syncWins() {
    winOrder_.erase(std::remove_if(winOrder_.begin(), winOrder_.end(), [&](const WinRef& w) { return !winOpen(w); }), winOrder_.end());
    for (const WinRef& w : allWins())
        if (winOpen(w) && std::find(winOrder_.begin(), winOrder_.end(), w) == winOrder_.end()) {
            winOrder_.push_back(w);
            setFocus(w);
        }
    if (focus_.p && std::find(winOrder_.begin(), winOrder_.end(), focus_) == winOrder_.end()) focus_ = {};
}

void AppShell::setFocus(WinRef w) {
    if (focus_ == w) return;
#ifdef _WIN32
    if (focus_.kind == kWinApp) static_cast<AppWindow*>(focus_.p)->blur();
    if (w.kind == kWinApp) app_ = static_cast<AppWindow*>(w.p);
#endif
    focus_ = w;
}

void AppShell::raiseFocus(WinRef w) {
    auto it = std::find(winOrder_.begin(), winOrder_.end(), w);
    if (it != winOrder_.end()) {
        winOrder_.erase(it);
        winOrder_.push_back(w);
    }
    setFocus(w);
}

float AppShell::winHit(WinRef w, const Vec3& o, const Vec3& d) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: return static_cast<WebBrowser*>(w.p)->hitDistance(o, d);
#endif
#ifdef _WIN32
        case kWinApp: return static_cast<AppWindow*>(w.p)->hitDistance(o, d);
#endif
        case kWinPanel: return static_cast<PanelApp*>(w.p)->hitDistance(o, d);
        case kWinSettings: return static_cast<SettingsHub*>(w.p)->hitDistance(o, d);
        default: return -1.0f;
    }
}

// The window drawn in front wins, however near or far it is.
AppShell::WinRef AppShell::topWinAt(const Vec3& o, const Vec3& d) {
    for (size_t i = winOrder_.size(); i-- > 0;)
        if (!winFull(winOrder_[i]) && winHit(winOrder_[i], o, d) >= 0) return winOrder_[i];
    return {};
}

// Fullscreen is remembered per window; only the focused one is shown that way, so Alt+Tab can move between them.
AppShell::WinRef AppShell::fullWin() {
    return focus_.p && winFull(focus_) ? focus_ : WinRef{};
}

bool AppShell::winFull(WinRef w) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: return static_cast<WebBrowser*>(w.p)->fullscreen();
#endif
#ifdef _WIN32
        case kWinApp: return static_cast<AppWindow*>(w.p)->fullscreen();
#endif
        case kWinPanel: return static_cast<PanelApp*>(w.p)->fullscreen();
        case kWinSettings: return static_cast<SettingsHub*>(w.p)->fullscreen();
        default: return false;
    }
}

bool AppShell::winBusy(WinRef w) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: return static_cast<WebBrowser*>(w.p)->busy();
#endif
#ifdef _WIN32
        case kWinApp: return static_cast<AppWindow*>(w.p)->busy();
#endif
        case kWinPanel: return static_cast<PanelApp*>(w.p)->busy();
        case kWinSettings: return static_cast<SettingsHub*>(w.p)->busy();
        default: return false;
    }
}

bool AppShell::winTyping(WinRef w) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: return static_cast<WebBrowser*>(w.p)->wantsKeyboard();
#endif
#ifdef _WIN32
        case kWinApp: return static_cast<AppWindow*>(w.p)->wantsKeyboard();
#endif
        case kWinPanel: return static_cast<PanelApp*>(w.p)->wantsText();
        default: return false;
    }
}

bool AppShell::winBoundsOf(WinRef w, Vec3& c, Vec3& n, float& bw, float& bh) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: return static_cast<WebBrowser*>(w.p)->bounds(c, n, bw, bh);
#endif
#ifdef _WIN32
        case kWinApp: return static_cast<AppWindow*>(w.p)->bounds(c, n, bw, bh);
#endif
        case kWinPanel: return static_cast<PanelApp*>(w.p)->bounds(c, n, bw, bh);
        case kWinSettings: return static_cast<SettingsHub*>(w.p)->bounds(c, n, bw, bh);
        default: return false;
    }
}

void AppShell::winPointer(WinRef w, const Vec3& o, const Vec3& d) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->pointer(o, d); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->pointer(o, d); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->pointer(o, d); break;
        case kWinSettings: static_cast<SettingsHub*>(w.p)->pointer(o, d); break;
        default: break;
    }
}

// The aim is on another window: clear this one's hover. A ray pointing backwards hits nothing.
void AppShell::winLeave(WinRef w, const Vec3& o, const Vec3& d) {
    switch (w.kind) {
        case kWinPanel: static_cast<PanelApp*>(w.p)->leave(); break;
        case kWinSettings: static_cast<SettingsHub*>(w.p)->leave(); break;
        default: winPointer(w, o, d * -1.0f); break;
    }
}

void AppShell::winPress(WinRef w, const Vec3& o, const Vec3& d, bool autoRelease) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->press(autoRelease); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->press(autoRelease); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->click(o, d); break;
        case kWinSettings: static_cast<SettingsHub*>(w.p)->activateHit(o, d); break;
        default: break;
    }
}

void AppShell::winRelease(WinRef w) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->release(); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->releaseButton(); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->releaseButton(); break;
        case kWinSettings: static_cast<SettingsHub*>(w.p)->releaseButton(); break;
        default: break;
    }
}

void AppShell::winWheel(WinRef w, float notches) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->scrollNotches(notches); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->scrollNotches(notches); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->wheel(notches); break;
        case kWinSettings: static_cast<SettingsHub*>(w.p)->step(notches > 0 ? -1 : 1); break;
        default: break;
    }
}

void AppShell::winKey(WinRef w, EditKey k) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->key(k); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->key(k); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->key(k); break;
        default: break;
    }
}

void AppShell::winText(WinRef w, const std::string& s) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->text(s); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->text(s); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->text(s); break;
        default: break;
    }
}

void AppShell::winHide(WinRef w) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->close(); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->hide(); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->hide(); break;
        case kWinSettings: static_cast<SettingsHub*>(w.p)->hide(); break;
        default: break;
    }
}

void AppShell::winToggleFull(WinRef w) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->toggleFullscreen(); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->toggleFullscreen(); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->toggleFullscreen(); break;
        case kWinSettings: static_cast<SettingsHub*>(w.p)->toggleFullscreen(); break;
        default: break;
    }
}

void AppShell::winRender(WinRef w) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: static_cast<WebBrowser*>(w.p)->render(renderer_); break;
#endif
#ifdef _WIN32
        case kWinApp: static_cast<AppWindow*>(w.p)->render(renderer_); break;
#endif
        case kWinPanel: static_cast<PanelApp*>(w.p)->render(); break;
        case kWinSettings: static_cast<SettingsHub*>(w.p)->render(); break;
        default: break;
    }
}

bool AppShell::winEscape(WinRef w) {
    if (winFull(w)) {
        winToggleFull(w);
        return true;
    }
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: {
            WebBrowser* b = static_cast<WebBrowser*>(w.p);
            if (!b->addressFocused()) return false;
            b->escape();
            return true;
        }
#endif
        case kWinPanel: {
            PanelApp* p = static_cast<PanelApp*>(w.p);
            if (p->wantsText()) {
                p->ctrl("esc");
                return true;
            }
            return p->back();
        }
        case kWinSettings: return static_cast<SettingsHub*>(w.p)->back();
        default: return false;
    }
}

bool AppShell::winBack(WinRef w) {
    if (winEscape(w)) return true;
    winHide(w);
    return true;
}

Vec3 AppShell::winForward() const {
    Vec3 f = vrOn_ ? head_.forward : camera_.forward();
    const int n = static_cast<int>(winOrder_.size());
    if (n == 0) return f;
    const float a = (n % 2 ? 1.0f : -1.0f) * 0.5f * static_cast<float>((n + 1) / 2);
    const float c = std::cos(a), s = std::sin(a);
    return {f.x * c + f.z * s, f.y, -f.x * s + f.z * c};
}

// Glides the camera to a spot squarely in front of a window (the window stays put). The pin is applied when it arrives.
void AppShell::addMediaFolders(const std::vector<std::string>& paths) {
    namespace fs = std::filesystem;
    bool any = false;
    for (const std::string& raw : paths) {
        std::error_code ec;
        fs::path p = fs::u8path(raw);
        if (fs::is_regular_file(p, ec)) p = p.parent_path();
        if (!fs::is_directory(p, ec)) continue;
        const std::string s = p.u8string();
        if (!mediaLib_->hasFolder(s)) {
            mediaLib_->addFolder(s);
            any = true;
        }
    }
    if (any && media_ && !media_->isOpen()) openPanel(media_.get());
}

void AppShell::flyToWin(const WinRef& t) {
    if (!t.p || vrOn_ || recT_ >= 0.0f) return;
    // A window still gliding into its arranged slot is snapped there first so the camera aims at where it will end up.
    for (size_t i = 0; i < winAnims_.size(); ++i) {
        if (winAnims_[i].w != t) continue;
        const WinAnim a = winAnims_[i];
        winAnims_.erase(winAnims_.begin() + static_cast<long>(i));
        winMoveTo(a.w, a.c1, a.yaw1, a.w1);
        break;
    }
    Vec3 c, n;
    float bw, bh;
    if (!winBoundsOf(t, c, n, bw, bh)) return;
    raiseFocus(t);
    flying_ = true;  // the glide must not be undone by gravity
    vy_ = 0.0f;
    n.y = 0;
    n = length(n) > 1e-4f ? normalize(n) : Vec3{0, 0, 1};
    // n is the window's front (+z); always glide to that side, even when the user is behind it.
    const float dist = fitDistance(bw, bh, renderer_.aspect(), std::tan(kFovY * 0.5f));
    const float eyeHi = cityMode_ ? 450.0f : (room_->building() ? room_->maxEye() : 24.0f);
    const float eyeY = clampf(c.y, 0.35f, eyeHi);
    recFromPos_ = camera_.position;
    recToPos_ = {c.x + n.x * dist, eyeY, c.z + n.z * dist};
    if (cityMode_ && city_) city_->resolve(recToPos_, 0.8f, flying_ ? 0.35f : 1.7f);
    recFromYaw_ = camera_.yaw;
    recToYaw_ = std::atan2(n.x, n.z);
    recFromPitch_ = camera_.pitch;
    recToPitch_ = std::atan2(c.y - eyeY, dist);
    recT_ = 0.0f;
}
void AppShell::openSettingsWin() {
    settingsPanel_->open(vrOn_ ? head_.eye : camera_.position, winForward());
    raiseFocus({kWinSettings, settingsPanel_.get()});
}
void AppShell::openPanel(PanelApp* p) {
    panel_ = p;
    if (p->isOpen()) {
        raiseFocus({kWinPanel, p});
        return;
    }
    p->open(vrOn_ ? head_.eye : camera_.position, winForward());
    raiseFocus({kWinPanel, p});
}
// ---- Start menu ------------------------------------------------------------------------------------------------
void AppShell::toggleStart() {
    if (start_->isOpen()) {
        start_->close();
        return;
    }
    openPanel(start_.get());
}

std::vector<StartHost::Row> AppShell::startRows() {
    std::vector<StartHost::Row> rows;
    startWins_.clear();
    for (const WinRef& w : allWins()) {
        if (w.p == start_.get()) continue;
        std::string name;
        bool exists = false;
        switch (w.kind) {
#ifdef SCIFI_HAS_CEF
            case kWinBrowser: name = "Web"; exists = static_cast<WebBrowser*>(w.p)->started(); break;
#endif
#ifdef _WIN32
            case kWinApp:
                for (size_t i = 0; i < apps_.size(); ++i)
                    if (apps_[i].get() == w.p && i < catalog_.size()) name = catalog_[i].name;
                exists = static_cast<AppWindow*>(w.p)->running();
                break;
#endif
            case kWinPanel: {
                PanelApp* p = static_cast<PanelApp*>(w.p);
                const std::string n = p->name();
                name = n.empty() ? n : n.substr(0, 1) + [&] { std::string r = n.substr(1); for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return r; }();
                exists = p->exists();
                break;
            }
            case kWinSettings: name = "Settings"; exists = static_cast<SettingsHub*>(w.p)->exists(); break;
            default: break;
        }
        if (!exists || name.empty()) continue;
        rows.push_back({name, winOpen(w) ? (focus_ == w ? 2 : 0) : 1});
        startWins_.push_back(w);
    }
    return rows;
}

void AppShell::startFocusRow(int i) {
    if (i < 0 || i >= static_cast<int>(startWins_.size())) return;
    const WinRef w = startWins_[i];
    if (!winOpen(w)) {
        switch (w.kind) {
#ifdef SCIFI_HAS_CEF
            case kWinBrowser: static_cast<WebBrowser*>(w.p)->open("", head_.eye, head_.forward, vrOn_); break;
#endif
#ifdef _WIN32
            case kWinApp:
                for (size_t k = 0; k < apps_.size(); ++k)
                    if (apps_[k].get() == w.p) launchApp(static_cast<int>(k));
                break;
#endif
            case kWinPanel: openPanel(static_cast<PanelApp*>(w.p)); break;
            case kWinSettings: openSettingsWin(); break;
            default: break;
        }
    }
    flyToWin(w);
}

void AppShell::startAction(const std::string& what) {
    if (what == "arc") arrangeArc();
    else if (what == "tile") arrangeTile();
    else if (what == "min") minimiseAll();
    else if (what == "exit") running_ = false;
    else if (what == "restart") os::powerOff(true);
    else if (what == "shutdown") os::powerOff(false);
    else if (what == "lock") os::lockPc();
    else if (what == "sleep") os::sleepPc();
}

void AppShell::runTool(Tool tool) {
    switch (tool) {
        case Tool::Browser:
#ifdef SCIFI_HAS_CEF
            if (browser_) {
                if (browser_->active()) raiseFocus({kWinBrowser, browser_.get()});
                else browser_->open("", head_.eye, head_.forward, vrOn_);
            }
#else
            menu_->setStatus("Browser not built in");
#endif
            break;
        case Tool::Editor: openPanel(editor2d_.get()); break;
        case Tool::Image: openPanel(imageEd_.get()); break;
        case Tool::Ftp: openPanel(ftp_.get()); break;
        case Tool::Calc: openPanel(calc_.get()); break;
        case Tool::Monitor: openPanel(monitor_.get()); break;
        case Tool::FileBrowser: openPanel(files2d_.get()); break;
        case Tool::Terminal: openPanel(terminal_.get()); break;
        case Tool::Media: openPanel(media_.get()); break;
        case Tool::Settings:
            if (!settingsPanel_->isOpen()) buildSettingsHub();
            openSettingsWin();
            break;
        case Tool::Theme: cycleTheme(); break;
    }
}

// ---- Scripting -----------------------------------------------------------------------------------------------
// One command per line, # starts a comment. Durations [s] are optional (default 0 = instant).
//   wait S | goto X Y Z [S] | walk M [S] | strafe M [S] | turn DEG [S] | look YAW PITCH [S] | fly on|off
//   city | leave | enter PATH | room N | tool start|startpower|files|settings|theme
//   shot FILE | where | log TEXT | quit
// Output (where / log / errors) is appended to control.log next to the control file.
void AppShell::scriptLog(const std::string& s) {
    const char* la = std::getenv("LOCALAPPDATA");
    std::filesystem::path dir = std::filesystem::path(la ? la : ".") / "SciFiShell";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (FILE* f = std::fopen((dir / "control.log").string().c_str(), "a")) {
        std::fprintf(f, "%s\n", s.c_str());
        std::fclose(f);
    }
}

bool AppShell::scriptExec(const std::string& line) {
    std::istringstream is(line);
    std::string cmd;
    is >> cmd;
    for (auto& c : cmd) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto num = [&](float def) { float v = def; if (!(is >> v)) { is.clear(); v = def; } return v; };
    const float deg = kPi / 180.0f;
    auto startMove = [&](bool pos, bool ang, bool hasY, Vec3 to, float yaw, float pitch, double dur) {
        mv_ = ScriptMove{};
        mv_.on = true; mv_.pos = pos; mv_.ang = ang; mv_.hasY = hasY;
        mv_.from = camera_.position; mv_.to = to;
        mv_.yaw0 = camera_.yaw; mv_.yaw1 = yaw; mv_.pitch0 = camera_.pitch; mv_.pitch1 = pitch;
        mv_.dur = dur;
    };
    const Vec3 fwd{-std::sin(camera_.yaw), 0, -std::cos(camera_.yaw)}, right{std::cos(camera_.yaw), 0, -std::sin(camera_.yaw)};
    if (cmd.empty() || cmd[0] == '#') return true;
    if (cmd == "wait") scriptWait_ = num(1);
    else if (cmd == "waitcity") waitCity_ = 120.0;
    else if (cmd == "goto") {
        const float x = num(0), y = num(camera_.position.y), z = num(0), s = num(0);
        startMove(true, false, true, {x, y, z}, 0, 0, s);
    } else if (cmd == "walk" || cmd == "strafe") {
        const float m = num(1), s = num(0);
        const Vec3 d = (cmd == "walk" ? fwd : right) * m;
        startMove(true, false, false, camera_.position + d, 0, 0, s);
    } else if (cmd == "turn") {
        const float a = num(0), s = num(0);
        startMove(false, true, false, {}, camera_.yaw + a * deg, camera_.pitch, s);
    } else if (cmd == "look") {
        const float yw = num(0), p = num(0), s = num(0);
        startMove(false, true, false, {}, yw * deg, p * deg, s);
    } else if (cmd == "cam") {
        const float x = num(0), y = num(0), z = num(0), yw = num(0) * deg, p = num(0) * deg, s = num(0);
        float dy = std::fmod(yw - camera_.yaw, 2.0f * kPi);
        if (dy > kPi) dy -= 2.0f * kPi;
        if (dy < -kPi) dy += 2.0f * kPi;
        startMove(true, true, true, {x, y, z}, camera_.yaw + dy, p, s);
    } else if (cmd == "sp") {
        const float x = num(0), y = num(0), z = num(0), yw = num(0) * deg, p = num(0) * deg, s = num(1);
        if (spPts_.empty()) spPts_.push_back({{camera_.position.x, camera_.position.y, camera_.position.z, camera_.yaw, camera_.pitch}, 0.0});
        float dy = std::fmod(yw - spPts_.back().v[3], 2.0f * kPi);
        if (dy > kPi) dy -= 2.0f * kPi;
        if (dy < -kPi) dy += 2.0f * kPi;
        spPts_.push_back({{x, y, z, spPts_.back().v[3] + dy, p}, spPts_.back().t + s});
    } else if (cmd == "go") {
        if (spPts_.size() >= 2) { spOn_ = true; spT_ = 0; flying_ = true; }
        else spPts_.clear();
    } else if (cmd == "ride") {
        std::vector<std::vector<Vec3>> rs;
        if (city_) city_->routes(rs);
        const size_t i = static_cast<size_t>(num(0));
        if (cityMode_ && i < rs.size()) startRide(rs[i]);
        else scriptLog("no such route");
    } else if (cmd == "fly") {
        std::string v;
        is >> v;
        flying_ = (v != "off");
        vy_ = 0;
    } else if (cmd == "give") {
        if (city_) { city_->giveItem((int)num(0)); if (city_->holding()) sfx_.play(SfxId::Pickup); }
    } else if (cmd == "car") {
        toggleCar();
    } else if (cmd == "track") {
        if (!driving_) toggleCar();
        if (driving_ && city_ && city_->trackReady()) {
            const float s = num(0);
            City::TrackFrame f0;
            city_->trackSample(s, f0);
            carPos_ = f0.p;
            carYaw_ = chaseYaw_ = railYaw_ = std::atan2(-f0.flat.x, -f0.flat.z);
            carVel_ = f0.flat * 30.0f;
            carVy_ = 0.0f;
            if (s > 14.0f) {
                onTrack_ = true;
                trackS_ = s;
                trackD_ = 0.0f;
                trackV_ = 40.0f;
                camera_.position = f0.p - f0.flat * 8.5f + Vec3{0, 3.4f, 0};
            }
        }
    } else if (cmd == "carcam") {
        carFirst_ = !carFirst_;
    } else if (cmd == "drive") {
        scriptThrottle_ = num(0);
        scriptSteer_ = num(1);
    } else if (cmd == "throw") {
        if (city_) city_->throwHeld(head_.eye);
    } else if (cmd == "pond") {
        Vec3 pp;
        if (city_ && city_->debugPond(pp)) {
            camera_.position = {pp.x, pp.y + 2.2f, pp.z + 11.0f};
            camera_.yaw = 0;
            camera_.pitch = -0.2f;
        }
    } else if (cmd == "pluginstatus") {
        std::string status = "plugins";
        for (const auto& p : plugins_.plugins()) status += " " + p.id + "=" + (p.running ? "on" : "off");
        Vec3 pond;
        status += " driving=" + std::to_string(driving_) + " track=" + std::to_string(onTrack_) +
                  " trackready=" + std::to_string(city_ && city_->trackReady()) +
                  " pond=" + std::to_string(city_ && city_->debugPond(pond)) +
                  " holding=" + std::to_string(city_ && city_->holding()) + " theme=" + currentTheme().name;
        scriptLog(status);
    } else if (cmd == "plugininstall") {
        std::string path;
        std::getline(is, path);
        while (!path.empty() && path.front() == ' ') path.erase(path.begin());
        const bool ok = pluginPackages_->install(std::filesystem::u8path(path));
        scriptLog(ok ? "plugin install ok" : "plugin install failed: " + pluginPackages_->error());
        buildSettingsHub();
    } else if (cmd == "pluginmessage") {
        std::string id;
        is >> id;
        const auto it = pluginMessages_.find(id);
        scriptLog(it == pluginMessages_.end() ? "plugin message unavailable" : "plugin message " + it->second);
    } else if (cmd == "pluginerror") {
        scriptLog("plugin error " + plugins_.error());
    } else if (cmd == "pluginnotice") {
        scriptLog("plugin notice " + settingsPanel_->notice());
    } else if (cmd == "pluginclick") {
        std::string title;
        std::getline(is, title);
        while (!title.empty() && title.front() == ' ') title.erase(title.begin());
        if (!settingsPanel_->isOpen()) { buildSettingsHub(); openSettingsWin(); }
        if (!settingsPanel_->scriptActivate("PLUGINS", title)) scriptLog("plugin settings item not found: " + title);
    } else if (cmd == "settingsclose") {
        settingsPanel_->close();
    } else if (cmd == "city") {
        if (!cityMode_) toggleCity(true);
    } else if (cmd == "leave") {
        if (room_->onExitBuilding) room_->onExitBuilding();
    } else if (cmd == "enter") {
        std::string path;
        std::getline(is, path);
        while (!path.empty() && path.front() == ' ') path.erase(path.begin());
        if (cityMode_) toggleCity(false);
        room_->load(path);
        scriptWait_ = 1.5;
    } else if (cmd == "room") {
        room_->debugEnter(static_cast<int>(num(0)));
        scriptWait_ = 1.0;
    } else if (cmd == "switch") {
        int id = 0;
        is >> id;
        switchTo(id);
    } else if (cmd == "win") {
        std::string v;
        is >> v;
        scriptWin(v);
    } else if (cmd == "tool") {
        std::string v;
        is >> v;
        if (v == "settings") runTool(Tool::Settings);
        else if (v == "settingspage") { std::string s; is >> s; runTool(Tool::Settings); settingsPanel_->openPage(s); }
        else if (v == "harvi") {
            std::string sub; is >> sub;
            std::string rest; std::getline(is, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            const Vec3 e = vrOn_ ? head_.eye : camera_.position;
            const Vec3 f = vrOn_ ? head_.forward : camera_.forward();
            if (sub == "on") harvi_->summon(e, f, vrOn_);
            else if (sub == "off") harvi_->dismiss();
            else if (sub == "say") harvi_->say(rest, "neutral");
            else if (sub == "emote") harvi_->emote(rest, 6.0f);
            else if (sub == "gesture") harvi_->gesture(rest);
            else if (sub == "node") harvi_->showNode(rest);
            else if (sub == "ctx") harvi_->setContext(rest);
        }
        else if (v == "theme") runTool(Tool::Theme);
        else if (v == "monitor") runTool(Tool::Monitor);
        else if (v == "terminal") runTool(Tool::Terminal);
        else if (v == "files2d") runTool(Tool::FileBrowser);
        else if (v == "editor") runTool(Tool::Editor);
        else if (v == "files2dpath") { std::string s; std::getline(is >> std::ws, s); files2d_->navigate(s); }
        else if (v == "image") { runTool(Tool::Image); std::string s; std::getline(is >> std::ws, s); if (!s.empty()) imageEd_->openPath(s); }
        else if (v == "imgact") { int id = 0; is >> id; imageEd_->command(id); }
        else if (v == "ftp") runTool(Tool::Ftp);
        else if (v == "calc") runTool(Tool::Calc);
        else if (v == "calcmode") { int m = 0; is >> m; calc_->setMode(static_cast<Calculator::Mode>(m)); }
        else if (v == "calcbase") { int b = 10; is >> b; calc_->setBase(b); }
        else if (v == "calctype") { std::string s; std::getline(is >> std::ws, s); calc_->feed(s); }
        else if (v == "edtype") { std::string s; std::getline(is >> std::ws, s); editor2d_->text(s); }
        else if (v == "edkey") { editor2d_->key(EditKey::Enter); }
        else if (v == "edsave") { editor2d_->key(EditKey::Save); }
        else if (v == "fs") { if (focus_.p) winToggleFull(focus_); else if (!winOrder_.empty()) winToggleFull(winOrder_.back()); }
        else if (v == "osk") toggleKeyboard();
#ifdef _WIN32
        else if (v == "appclick") { int x = 0, y = 0; is >> x >> y; if (app_) app_->scriptClick(x, y); }
        else if (v == "apptype") { std::string s; std::getline(is >> std::ws, s); if (app_) app_->text(s); }
#endif
        else if (v == "mediafs") { if (media_) media_->toggleFullscreen(); }
        else if (v == "termtype") { std::string s; std::getline(is >> std::ws, s); terminal_->text(s + "\r"); }
        else if (v == "media") runTool(Tool::Media);
        else if (v == "page") { std::string pg; std::getline(is >> std::ws, pg); settingsPanel_->openPage(pg); }
        else if (v == "mediafolder") { std::string p; std::getline(is >> std::ws, p); mediaCategories(); mediaLib_->addFolder(p); }
        else if (v == "mediaplay") {
            mediaCategories();
            std::vector<std::string> all;
            std::string one;
            std::getline(is >> std::ws, one);
            if (!one.empty()) mediaOpenFile(one);
            else {
                for (const MediaTrack& t : mediaLib_->tracks()) all.push_back(t.path);
                mediaQueueSet(all, 0);
            }
        }
        else if (v == "mediaclick") {
            int id = 0, arg = 0;
            is >> id >> arg;
            if (media_) media_->scriptClick(id, arg);
        }
        else if (v == "cinema") {
            if (media_) media_->enterCinema();
        }
        else if (v == "start" || v == "startpower") { if (!start_->isOpen()) toggleStart(); if (v == "startpower") start_->scriptPower(); }
        else scriptLog("unknown tool: " + v);
    } else if (cmd == "shot") {
        std::string p;
        std::getline(is, p);
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        shotPath_ = p;
        shotPending_ = true;
        scriptWait_ = 0.3;
    } else if (cmd == "where") {
        char b[200];
        std::snprintf(b, sizeof b, "pos %.2f %.2f %.2f yaw %.1f pitch %.1f city=%d building=%d flying=%d", camera_.position.x, camera_.position.y,
                      camera_.position.z, camera_.yaw / deg, camera_.pitch / deg, cityMode_ ? 1 : 0, room_->building() ? 1 : 0, flying_ ? 1 : 0);
        scriptLog(b);
    } else if (cmd == "log") {
        std::string t;
        std::getline(is, t);
        scriptLog(t);
    } else if (cmd == "quit") running_ = false;
    else {
        scriptLog("unknown command: " + line);
        return false;
    }
    return true;
}

void AppShell::scriptTick(double dt) {
    static const bool enabled = !std::getenv("SCIFI_NO_SCRIPT");
    if (!enabled) return;
    if (script_.empty() && scriptPc_ == 0) {
        static bool first = true;
        if (first) {
            first = false;
            if (const char* sf = std::getenv("SCIFI_SCRIPT")) {
                std::ifstream in(sf);
                for (std::string l; std::getline(in, l);) script_.push_back(l);
            }
        }
    }
    if (spOn_) {
        spT_ += dt;
        const int n = static_cast<int>(spPts_.size());
        const double t = std::min(spT_, spPts_.back().t);
        int i = 0;
        while (i < n - 2 && t > spPts_[i + 1].t) ++i;
        auto pt = [&](int k, float* v, double& kt) {
            if (k >= 0 && k < n) { std::copy(spPts_[k].v, spPts_[k].v + 5, v); kt = spPts_[k].t; return; }
            const int a = k < 0 ? 0 : n - 1, b = k < 0 ? 1 : n - 2;  // mirror the end points
            for (int c = 0; c < 5; ++c) v[c] = 2 * spPts_[a].v[c] - spPts_[b].v[c];
            kt = 2 * spPts_[a].t - spPts_[b].t;
        };
        float P[4][5];
        double k[4];
        for (int j = 0; j < 4; ++j) pt(i - 1 + j, P[j], k[j]);
        float out[5];
        for (int c = 0; c < 5; ++c) {
            auto lerp = [](float a, float b, double u) { return static_cast<float>(a + (b - a) * u); };
            const float A1 = lerp(P[0][c], P[1][c], (t - k[0]) / (k[1] - k[0]));
            const float A2 = lerp(P[1][c], P[2][c], (t - k[1]) / (k[2] - k[1]));
            const float A3 = lerp(P[2][c], P[3][c], (t - k[2]) / (k[3] - k[2]));
            const float B1 = lerp(A1, A2, (t - k[0]) / (k[2] - k[0]));
            const float B2 = lerp(A2, A3, (t - k[1]) / (k[3] - k[1]));
            out[c] = lerp(B1, B2, (t - k[1]) / (k[2] - k[1]));
        }
        camera_.position = {out[0], out[1], out[2]};
        camera_.yaw = out[3];
        camera_.pitch = out[4];
        vy_ = 0;
        if (spT_ >= spPts_.back().t) { spOn_ = false; spPts_.clear(); }
        return;
    }
    if (mv_.on) {  // eased interpolation of the current scripted motion
        mv_.t += dt;
        const float k = mv_.dur <= 0 ? 1.0f : static_cast<float>(std::min(1.0, mv_.t / mv_.dur));
        const float e = k * k * (3.0f - 2.0f * k);
        if (mv_.pos) {
            camera_.position.x = mv_.from.x + (mv_.to.x - mv_.from.x) * e;
            camera_.position.z = mv_.from.z + (mv_.to.z - mv_.from.z) * e;
            if (mv_.hasY) { camera_.position.y = mv_.from.y + (mv_.to.y - mv_.from.y) * e; vy_ = 0; }
        }
        if (mv_.ang) {
            camera_.yaw = mv_.yaw0 + (mv_.yaw1 - mv_.yaw0) * e;
            camera_.pitch = mv_.pitch0 + (mv_.pitch1 - mv_.pitch0) * e;
        }
        if (k >= 1.0f) mv_.on = false;
        return;
    }
    if (scriptWait_ > 0) { scriptWait_ -= dt; return; }
    if (waitCity_ > 0) {  // "waitcity": hold the script until the city has been built (or the cap runs out)
        waitCity_ -= dt;
        if (!city_ || city_->settled()) waitCity_ = 0;
        else return;
    }
    if (shotPending_) return;
    if (scriptPc_ >= script_.size()) {  // idle: look for a fresh control file twice a second
        script_.clear();
        scriptPc_ = 0;
        scriptPoll_ -= dt;
        if (scriptPoll_ > 0) return;
        scriptPoll_ = 0.5;
        const char* la = std::getenv("LOCALAPPDATA");
        const std::filesystem::path cf = std::filesystem::path(la ? la : ".") / "SciFiShell" / "control.txt";
        std::error_code ec;
        if (!std::filesystem::exists(cf, ec)) return;
        {
            std::ifstream in(cf);
            for (std::string l; std::getline(in, l);) script_.push_back(l);
        }
        std::filesystem::remove(cf, ec);
        return;
    }
    // Run commands until one needs time.
    for (int guard = 0; guard < 64 && scriptPc_ < script_.size() && scriptWait_ <= 0 && waitCity_ <= 0 && !mv_.on && !spOn_ && !shotPending_; ++guard)
        scriptExec(script_[scriptPc_++]);
}

void AppShell::toggleCity(bool on) {
    if (!city_ || on == cityMode_) return;
    if (on && !settings_.showCity) {
        menu_->setStatus("CITY IS OFF - press F9 or use Settings > Graphics to turn it on");
        return;
    }
    cityMode_ = on;
    room_->setCardsVisible(!on);
    if (on) clearRoomPending_ = true;  // drop the building's walls: they sit at the map's centre, on top of the hub
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

void AppShell::setCityEnabled(bool on) {
    settings_.showCity = on;
    settings_.save();
    if (on) {
        if (!cityStarted_ && city_) {
            city_->start();
            cityStarted_ = true;
        }
        toggleCity(true);
    } else {
        toggleCity(false);
        menu_->setStatus("CITY OFF - press F9 to bring it back");
    }
}

void AppShell::startRide(const std::vector<Vec3>& path) {
    if (path.size() < 2) return;
    ridePath_ = path;
    rideLen_ = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const float dx = path[i + 1].x - path[i].x, dz = path[i + 1].z - path[i].z;
        rideLen_ += std::sqrt(dx * dx + dz * dz);
    }
    rideDist_ = 0;
    rideT_ = 0;
    rideY0_ = camera_.position.y;
    riding_ = true;
    flying_ = true;
    vy_ = 0.0f;
    menu_->setStatus("RIDING THE DATA LANE - move or click to step off");
}

// Carry the camera along the route: quick acceleration, a long cruise, then a gentle slow-down at the end.
void AppShell::rideStep(double dt) {
    rideT_ += static_cast<float>(dt);
    const float remaining = rideLen_ - rideDist_;
    const float v = std::min({80.0f, 10.0f + rideT_ * 75.0f, 5.0f + remaining * 2.4f});
    rideDist_ += v * static_cast<float>(dt);
    float d = rideDist_;
    Vec3 pos = ridePath_.back(), dir{0, 0, -1};
    bool done = rideDist_ >= rideLen_ - 0.3f;
    for (size_t i = 0; i + 1 < ridePath_.size(); ++i) {
        const Vec3 a = ridePath_[i], b = ridePath_[i + 1];
        const float dx = b.x - a.x, dz = b.z - a.z, sl = std::sqrt(dx * dx + dz * dz);
        if (sl < 1e-3f) continue;
        dir = {dx / sl, 0, dz / sl};
        if (d <= sl || i + 2 == ridePath_.size()) {
            const float u = std::min(1.0f, d / sl);
            pos = {a.x + dx * u, a.y + (b.y - a.y) * u, a.z + dz * u};
            break;
        }
        d -= sl;
    }
    const float k = std::min(1.0f, static_cast<float>(dt) * 4.0f);
    float dy = std::fmod(std::atan2(-dir.x, -dir.z) - camera_.yaw, 2.0f * kPi);
    if (dy > kPi) dy -= 2.0f * kPi;
    if (dy < -kPi) dy += 2.0f * kPi;
    camera_.yaw += dy * k;
    camera_.pitch += (-0.2f - camera_.pitch) * k * 0.5f;
    // Chase camera: behind and above the light cycle.
    const float e = std::min(1.0f, rideT_ * 2.0f);
    const Vec3 fwd{-std::sin(camera_.yaw), 0.0f, -std::cos(camera_.yaw)};
    const float back = 4.5f * e;
    ridePos_ = pos;
    rideYaw_ = camera_.yaw;
    camera_.position = {pos.x - fwd.x * back, rideY0_ + (pos.y + 2.6f - rideY0_) * e, pos.z - fwd.z * back};
    vy_ = 0.0f;
    flying_ = true;
    if (done) {
        riding_ = false;
        flying_ = false;
        menu_->setStatus("");
    }
}

// Idle fly-through of the city: one continuous spline through the waypoints at a steady speed, with no stops. The altitude is
// raised wherever a building is ahead so the camera dives between towers but never clips them. Any control input stops it.
void AppShell::startTour() {
    std::vector<Vec3> pts;
    city_->tourPoints(pts);
    if (pts.size() < 3) return;
    stopTour();
    tourPts_.clear();
    tourPts_.push_back(camera_.position);
    for (const Vec3& p : pts) tourPts_.push_back(p);
    tourSeg_ = 0;
    tourU_ = 0;
    tourY_ = camera_.position.y;
    tourLog_ = 0;
    flying_ = true;
    vy_ = 0;
    tourActive_ = true;
}

void AppShell::tourStep(double dt) {
    const int n = (int)tourPts_.size();
    auto cr = [&](int i, float u, Vec3& pos, Vec3& vel) {
        const Vec3 p0 = tourPts_[std::max(0, i - 1)], p1 = tourPts_[i], p2 = tourPts_[std::min(n - 1, i + 1)], p3 = tourPts_[std::min(n - 1, i + 2)];
        const float u2 = u * u, u3 = u2 * u;
        auto f = [&](float a, float b, float c, float d, float& p, float& v) {
            p = 0.5f * (2 * b + (-a + c) * u + (2 * a - 5 * b + 4 * c - d) * u2 + (-a + 3 * b - 3 * c + d) * u3);
            v = 0.5f * ((-a + c) + 2 * (2 * a - 5 * b + 4 * c - d) * u + 3 * (-a + 3 * b - 3 * c + d) * u2);
        };
        f(p0.x, p1.x, p2.x, p3.x, pos.x, vel.x);
        f(p0.y, p1.y, p2.y, p3.y, pos.y, vel.y);
        f(p0.z, p1.z, p2.z, p3.z, pos.z, vel.z);
    };
    Vec3 pos, vel;
    cr(tourSeg_, tourU_, pos, vel);
    const float speed = 27.0f + 0.36f * std::max(0.0f, tourY_);
    const float ds = std::max(1.0f, std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z));
    tourU_ += speed * static_cast<float>(dt) / ds;
    while (tourU_ >= 1.0f) {
        tourU_ -= 1.0f;
        if (++tourSeg_ >= n - 1) { tourActive_ = false; return; }
    }
    cr(tourSeg_, tourU_, pos, vel);
    const float hl = std::max(0.01f, std::sqrt(vel.x * vel.x + vel.z * vel.z));
    const float ux = vel.x / hl, uz = vel.z / hl;
    // Look ahead for buildings and rise early enough to clear them.
    float need = 0;
    for (float s : {0.0f, 10.0f, 20.0f, 32.0f, 48.0f}) need = std::max(need, city_->clearHeight(pos.x + ux * s, pos.z + uz * s, 5.5f));
    const float want = std::max(pos.y, need + 3.0f);
    const float k = 1.0f - std::exp(-static_cast<float>(dt) * (want > tourY_ ? 2.6f : 1.0f));
    tourY_ += (want - tourY_) * k;
    camera_.position = {pos.x, tourY_, pos.z};
    const float tyaw = std::atan2(-ux, -uz);
    float dy = std::fmod(tyaw - camera_.yaw, 2.0f * kPi);
    if (dy > kPi) dy -= 2.0f * kPi;
    if (dy < -kPi) dy += 2.0f * kPi;
    camera_.yaw += dy * (1.0f - std::exp(-static_cast<float>(dt) * 0.9f));
    const float tp = clampf(std::atan2(vel.y, hl) * 0.8f - 0.08f, -0.6f, 0.4f);
    camera_.pitch += (tp - camera_.pitch) * (1.0f - std::exp(-static_cast<float>(dt) * 1.6f));
    vy_ = 0;
    flying_ = true;
    if (std::getenv("SCIFI_TOURLOG")) {
        tourLog_ += dt;
        if (tourLog_ > 0.5) {
            tourLog_ = 0;
            if (FILE* f = std::fopen("tour.log", "a")) { std::fprintf(f, "%.1f %.1f %.1f speed %.1f\n", camera_.position.x, camera_.position.y, camera_.position.z, speed); std::fclose(f); }
        }
    }
}
void AppShell::stopTour() {
    script_.clear();
    scriptPc_ = 0;
    scriptWait_ = 0;
    mv_.on = false;
    tourActive_ = false;
}
void AppShell::toggleKeyboard() {
    osk_->toggle(vrOn_ ? head_.eye : camera_.position, vrOn_ ? head_.forward : camera_.forward());
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
            vrHeadY_ = 0.0f;
            vrGaze_ = true;
            vrAnchor_ = {0, 0, 0};
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
    const bool inVR = vrOn_ && head_.vr;
    if (inVR && lookX != 0.0f) {
        // Turn the tracking space about the head, not its own origin, so the view pivots in place.
        Vec3 off = camera_.position - head_.eye;
        off.y = 0.0f;
        const Vec3 turned = Mat4::rotateY(-lookX).transformDir(off);
        camera_.position.x += turned.x - off.x;
        camera_.position.z += turned.z - off.z;
    }
    camera_.yaw -= lookX;
    camera_.pitch = clampf(camera_.pitch - lookY, -1.4f, 1.4f);
    const float speed = 3.0f * speedMul * (!cityMode_ && room_->building() ? room_->speedBoost() : 1.0f);
    // In VR you walk the way your head faces.
    float moveYaw = camera_.yaw;
    if (inVR && std::hypot(head_.forward.x, head_.forward.z) > 0.2f) moveYaw = std::atan2(-head_.forward.x, -head_.forward.z);
    Vec3 fwd{-std::sin(moveYaw), 0, -std::cos(moveYaw)};
    Vec3 right{std::cos(moveYaw), 0, -std::sin(moveYaw)};
    Vec3 p = camera_.position + fwd * (moveY * speed * static_cast<float>(dt)) +
             right * (moveX * speed * static_cast<float>(dt));
    p.x += knock_.x * static_cast<float>(dt);
    p.z += knock_.z * static_cast<float>(dt);
    knock_ = knock_ * std::exp(-3.0f * static_cast<float>(dt));
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
    app_ = a;
    a->setView(renderer_.aspect(), std::tan(kFovY * 0.5f));
    if (!a->active()) a->open(vrOn_ ? head_.eye : camera_.position, winForward(), vrOn_);
    raiseFocus({kWinApp, a});
}

#endif

void AppShell::switchTo(int id) {
    // ids: 0 room, 2 browser, 3 Windows desktop, 10+ programs, 30+ panels, 40 settings
    if (id == 3) {
        SDL_MinimizeWindow(window_);
        return;
    }
    if (id == 0) {
        setFocus({});
        return;
    }
#ifdef _WIN32
    if (id >= 100 && id < 100 + static_cast<int>(apps_.size())) {
        launchApp(id - 100);
        flyToWin(focus_);
        return;
    }
#endif
    if (id == 40) {
        if (!settingsPanel_->isOpen()) buildSettingsHub();
        openSettingsWin();
        flyToWin(focus_);
        return;
    }
    PanelApp* const panels[] = {monitor_.get(), terminal_.get(), files2d_.get(), editor2d_.get(), media_.get(), imageEd_.get(), ftp_.get(), calc_.get()};
    if (id >= 30 && id < 38) {
        openPanel(panels[id - 30]);
        flyToWin(focus_);
        return;
    }
#ifdef SCIFI_HAS_CEF
    if (id == 2 && browser_) {
        if (!browser_->active()) browser_->open("", head_.eye, head_.forward, vrOn_);
        raiseFocus({kWinBrowser, browser_.get()});
        flyToWin(focus_);
    }
#endif
}
// Alt+Tab / Ctrl+Tab task switcher. While it is open it swallows the input that would otherwise reach the scene.
// The flat fullscreen mouse pointer, drawn in view space at the virtual cursor (the tip is the active point).
void AppShell::drawArrow2D() {
    const float t = std::tan(kFovY * 0.5f), dist = 1.0f, h = 0.04f * 2.0f * t * dist;
    const Vec3 tip{cursorX_ * t * renderer_.aspect() * dist, cursorY_ * t * dist, -dist};
    const float uv[4] = {0, 0, 1, 1}, white[3] = {1, 1, 1};
    renderer_.drawWebRegion(*arrow_, Mat4::translate({tip.x + h * 0.5f, tip.y - h * 0.5f, tip.z}) * Mat4::scale({h, h, 1}), uv, 1.0f, 0.0f, -1.0f, 1.0f, white);
}

void AppShell::handleSwitcher(FrameInput& in, PadState& pad) {
    if (state_ != State::FileRoom) return;
    if (!switcher_->isOpen()) {
        if (in.tabStep == 0) return;
        int current = 0;
        std::vector<TaskSwitcher::Item> items;
        {
            PanelApp* const panels[] = {monitor_.get(), terminal_.get(), files2d_.get(), editor2d_.get(), media_.get(), imageEd_.get(), ftp_.get(), calc_.get()};
            const char* names[] = {"MONITOR", "TERMINAL", "FILES", "EDITOR", "MEDIA", "IMAGE", "FTP", "CALC"};
            const bool mediaPlaying = mediaNow().active;  // keeps its place in the switcher while it plays on with the window closed
            for (int i = 0; i < 8; ++i)
                if (panels[i]->exists() || (i == 4 && mediaPlaying)) {
                    items.push_back({30 + i, names[i], panels[i]->exists() ? (panels[i]->hidden() ? "Minimised" : "Window") : "Now playing", false});
                    if (focus_ == WinRef{kWinPanel, panels[i]}) current = 30 + i;
                }
            if (settingsPanel_->exists()) {
                items.push_back({40, "SETTINGS", settingsPanel_->hidden() ? "Minimised" : "Window", false});
                if (focus_.kind == kWinSettings && focus_.p) current = 40;
            }
        }
#ifdef _WIN32
        for (size_t i = 0; i < apps_.size(); ++i)
            if (apps_[i]->running()) {
                items.push_back({100 + static_cast<int>(i), catalog_[i].name, "Program", false});
                if (focus_ == WinRef{kWinApp, apps_[i].get()}) current = 100 + static_cast<int>(i);
            }
#endif
#ifdef SCIFI_HAS_CEF
        if (browser_ && browser_->started()) {
            items.push_back({2, "BROWSER", "Web", false});
            if (focus_ == WinRef{kWinBrowser, browser_.get()}) current = 2;
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
    if (in.modReleased || in.select || in.space || pad.aPressed || in.click || vrInput_.selectPressed) {
        const int id = switcher_->selectedId();
        switcher_->close();
        switchTo(id);
    } else if (in.escape || in.back || pad.bPressed || vrInput_.backPressed) {
        switcher_->close();
    }
    vrInput_.selectPressed = vrInput_.backPressed = false;
    mediaSeek_.reset();
    in.keys.clear();
    in.text.clear();
    in.click = false;
    in.escape = in.back = in.select = in.space = in.left = in.right = in.up = in.down = false;
    in.mediaSeekLeft = in.mediaSeekRight = false;
    in.mediaSeekLeftReleased = in.mediaSeekRightReleased = false;
    in.mediaVolumeUp = in.mediaVolumeDown = in.mediaPlayPause = false;
    in.wheel = 0.0f;
    pad.aPressed = pad.bPressed = pad.leftPressed = pad.rightPressed = pad.upPressed = pad.downPressed = false;
}

void AppShell::tick(double dt) {
    syncWins();
    winTick(static_cast<float>(dt));
#ifdef SCIFI_HAS_CEF
    WebBrowser::pump();
    const bool browserOpen = browser_ && browser_->active();
#else
    const bool browserOpen = false;
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
#endif
    FrameInput in = input_.poll();
    if (in.keyCaptured) {
        settings_.arrangeKey = in.keyCaptured;
        settings_.save();
    }
    input_.arrangeKey = settings_.arrangeKey ? settings_.arrangeKey : SDLK_F8;
    if (in.harvi && !(focus_.p && winTyping(focus_)))
        harvi_->toggle(vrOn_ ? head_.eye : camera_.position, vrOn_ ? head_.forward : camera_.forward(), vrOn_);
    if (in.escape && harvi_->active() && !(focus_.p && winTyping(focus_))) {
        harvi_->dismiss();
        in.escape = false;
    }
    if (in.cityToggle && !(focus_.p && winTyping(focus_))) setCityEnabled(!settings_.showCity);
    if (in.arrange && !((focus_.p && winTyping(focus_)) && input_.arrangeKey < 256)) arrangeCycle();
    // Only the focused window can take typed text; otherwise the keyboard walks the camera.
    const bool winTypingNow = focus_.p && winTyping(focus_);
    PadState pad = pad_.poll();
    if (in.quit) running_ = false;
    {
        // What the on-screen keyboard typed arrives like real key presses.
        OnScreenKeyboard::Typed typed = osk_->take();
        in.keys.insert(in.keys.end(), typed.keys.begin(), typed.keys.end());
        in.text += typed.text;
        if (typed.escape) in.escape = true;
        const bool kbm = in.click || in.lookX != 0 || in.lookY != 0 || in.moveX != 0 || in.moveY != 0 || in.moveZ != 0 || !in.keys.empty() ||
                         !in.text.empty() || in.select || in.back || in.space;
        const bool padUsed = pad.aPressed || pad.bPressed || pad.xPressed || pad.yPressed || pad.lbPressed || pad.rbPressed ||
                             std::fabs(pad.lx) + std::fabs(pad.ly) + std::fabs(pad.rx) + std::fabs(pad.ry) + pad.lt + pad.rt > 0.3f;
        if (kbm) lastSource_ = 0;
        else if (padUsed) lastSource_ = 1;
        else if (vrInput_.active()) lastSource_ = 2;
        if (vrOn_ && vrInput_.valid) {
            // The headset aims until a controller is used; an idle controller lying on the desk is ignored.
            if (length(vrAnchor_) < 0.5f || ((kbm || padUsed) && vrGaze_)) vrAnchor_ = vrInput_.pointerDir;
            if (kbm || padUsed) vrGaze_ = true;
            else if (vrGaze_ && (vrInput_.active() || dot(vrInput_.pointerDir, vrAnchor_) < 0.9976f)) vrGaze_ = false;
        }
        if (state_ == State::FileRoom && (in.osk || pad.xPressed || vrInput_.kbdPressed)) toggleKeyboard();
        // A text field taking focus brings the keyboard up for gamepad and VR controller users.
        if (winTypingNow && !wasTyping_ && lastSource_ != 0 && !osk_->isOpen()) toggleKeyboard();
        wasTyping_ = winTypingNow;
        if ((!driving_ && pad.rbPressed) || vrInput_.switchPressed) in.tabStep += 1;
        if (!driving_ && pad.lbPressed) in.tabStep -= 1;
    }
    {
        std::vector<std::string> picked = std::move(in.dropped);

        if (!picked.empty()) addMediaFolders(picked);
    }
    if (in.resized) renderer_.resize();
    {
        const float th = std::tan(kFovY * 0.5f), asp = renderer_.aspect();
#ifdef _WIN32
        for (auto& a : apps_) a->setView(asp, th);
#endif
#ifdef SCIFI_HAS_CEF
        if (browser_) browser_->setView(asp, th);
#endif
        for (PanelApp* p : {static_cast<PanelApp*>(monitor_.get()), static_cast<PanelApp*>(terminal_.get()), static_cast<PanelApp*>(start_.get()),
                            static_cast<PanelApp*>(files2d_.get()), static_cast<PanelApp*>(editor2d_.get()), static_cast<PanelApp*>(imageEd_.get()), static_cast<PanelApp*>(ftp_.get()),
                            static_cast<PanelApp*>(media_.get())})
            p->setView(th, asp);
        settingsPanel_->setView(th, asp);
    }
    handleSwitcher(in, pad);
    if (deferred_ && --deferDelay_ <= 0) {  // the loading ring has been on screen: do the heavy work now
        auto job = std::move(deferred_);
        deferred_ = nullptr;
        job();
        loadingT_ = 0.5f;
    }
    if (renaming_ && !settingsPanel_->isOpen()) renaming_ = false;
    const bool vEditing = viewer_->editing();
    if (in.toggleVR && !vEditing && !winTypingNow) toggleVR();
    input_.setTextInput(vEditing || winTypingNow || renaming_);
    if (renaming_) in.fullscreen = in.browser = in.toggleVR = in.space = in.up = in.down = in.left = in.right = in.back = false;

    const float fdt = static_cast<float>(dt);
    // Typing only captures the keyboard when a text field (page or address bar) has focus, so WASD walks otherwise.
    const bool typing = vEditing || winTypingNow || renaming_;
    // While typing, WASD belong to the text field; hold Alt to walk with the keyboard anyway (pad / VR sticks always walk).
    const bool kbMove = !typing || (SDL_GetModState() & KMOD_ALT) != 0;
    float kbX = kbMove ? in.moveX : 0.0f, kbY = kbMove ? in.moveY : 0.0f, kbZ = kbMove ? in.moveZ : 0.0f;
    if (viewer_->active() && !viewer_->isText()) kbZ = std::min(kbZ, 0.0f);  // Space is play/pause in the media viewer
    const WinRef mediaFullWin = fullWin();
    if (state_ == State::FileRoom && !switcher_->isOpen() && !appsPanel_->isOpen() && !viewer_->active() &&
        !renaming_ && !winTypingNow && focus_.kind == kWinPanel && focus_.p == media_.get() &&
        (!mediaFullWin.p || mediaFullWin == focus_) &&
        !(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT | KMOD_GUI | KMOD_SHIFT)))
        kbZ = std::min(kbZ, 0.0f);
    kbZ = clampf(kbZ + pad.rt - pad.lt + vrInput_.lookY, -1, 1);  // right trigger = up, left trigger = down; VR right stick up / down climbs
    float moveX = clampf(kbX + pad.lx + vrInput_.moveX, -1, 1);
    float moveY = clampf(kbY + pad.ly + vrInput_.moveY, -1, 1);
    const float freeSpeed = (in.shift ? (cityMode_ ? 4.05f : 1.69f) : 1.0f) * settings_.speedScale() * (cityMode_ ? 4.0f : 1.0f);
    float lookX = in.lookX * 0.0025f * settings_.lookScale() + (vrOn_ ? 0.0f : pad.rx * 2.2f * fdt * settings_.lookScale());
    if (vrOn_) {
        // Snap turning (30 degrees per flick of the right stick / pad) is far kinder in a headset than a smooth spin.
        const float turn = vrInput_.lookX + pad.rx;
        if (std::fabs(turn) > 0.6f && !snapLatch_) {
            lookX += turn > 0 ? 0.5236f : -0.5236f;
            snapLatch_ = true;
        } else if (std::fabs(turn) < 0.3f) {
            snapLatch_ = false;
        }
    }
    float lookY = (in.lookY * 0.0025f - pad.ry * 2.2f * fdt) * settings_.lookScale() * (settings_.invertY ? -1.0f : 1.0f);
    bool up = in.up || pad.upPressed, down = in.down || pad.downPressed;
    bool keySelect = in.select || pad.aPressed || (state_ == State::Menu && in.space);
    bool pointerSelect = in.click || vrInput_.selectPressed;
    bool back = in.back || pad.bPressed || vrInput_.backPressed;
    bool vrPointer = vrOn_ && vrInput_.valid && !vrGaze_;
    if (driving_ && onTrack_ && (in.escape || pad.yPressed) && !typing && !switcher_->isOpen() &&
        !appsPanel_->isOpen() && !viewer_->active() && !fullWin().p) {
        leaveTrack();
        in.escape = pad.yPressed = false;
    }
    if ((in.carToggle || (driving_ && pad.yPressed)) && !(focus_.p && winTyping(focus_))) toggleCar();
    if (in.carCam && driving_ && !typing) carFirst_ = !carFirst_;
    if (driving_) pointerSelect = keySelect = false;
    const bool ringToggle = !driving_ && (in.ring || pad.yPressed || vrInput_.menuPressed);
    if (in.themeCycle) cycleTheme();
    if (in.screenshot) shotPending_ = true;    
    {
        const bool anyInput = in.moveX != 0 || in.moveY != 0 || in.moveZ != 0 || in.lookX != 0 || in.lookY != 0 || in.click || in.clickUp ||
                              in.wheel != 0 || in.escape || in.select || in.back || in.up || in.down || in.left || in.right || in.space ||
                              in.ring || in.browser || in.tabStep != 0 || !in.keys.empty() || !in.text.empty() || in.toggleVR ||
                              std::fabs(pad.lx) + std::fabs(pad.ly) + std::fabs(pad.rx) + std::fabs(pad.ry) + pad.lt + pad.rt > 0.2f ||
                              pad.aPressed || pad.bPressed || pad.yPressed || vrInput_.selectPressed || vrInput_.menuPressed ||
                              vrInput_.backPressed || vrInput_.moveX != 0 || vrInput_.moveY != 0;
        static const bool tourAllowed = !std::getenv("SCIFI_SCRIPT") || std::getenv("SCIFI_TOUR");
        if (anyInput) {
            idleT_ = 0;
            if (tourActive_) stopTour();
        } else {
            idleT_ += dt;
        }
        const bool busy = !winOrder_.empty() || appsPanel_->isOpen() || viewer_->active() ||
                          deferred_ || riding_ || vrOn_;
        if (tourActive_ && (!cityMode_ || busy)) stopTour();
        if (tourAllowed && !tourActive_ && idleT_ > 10.0 && cityMode_ && city_ && city_->built() && !busy && script_.empty() && !mv_.on) startTour();
        
        if (riding_ && (pointerSelect || in.escape || back || ringToggle || std::fabs(moveX) + std::fabs(moveY) + std::fabs(kbZ) > 0.1f)) riding_ = false;
        if (riding_) {
            moveX = moveY = kbZ = 0.0f;
            if (rideT_ < 0.7f) lookX = lookY = 0.0f;
        }
    }
    if (state_ == State::FileRoom && !viewer_->editing() && !appsPanel_->isOpen() && !viewer_->overlay() && ringToggle) {
        const bool wasOpen = start_->isOpen();
        toggleStart();
        if (!wasOpen && start_->isOpen() && lastSource_ == 0) start_->focusSearch();  // typing goes straight to the search
    }

    if (state_ == State::Menu) {
        Vec3 o = camera_.position, d = mouseRayDirection(in.mouseX, in.mouseY);
        if (vrPointer) {
            o = vrInput_.pointerOrigin;
            d = vrInput_.pointerDir;
        } else if (vrOn_) {
            o = head_.eye;
            d = head_.forward;
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
        } else if (vrOn_) {
            o = head_.eye;
            d = head_.forward;
        }
        aimDir_ = d;
        // Windows are persistent and stacked: the frontmost window under the aim takes the pointer, clicking raises and
        // focuses it, and anything else (the world, a door, a bus lane) still works around them. The camera stays free;
        // right click pins the view to a window (camera frozen, mouse drives a cursor) and releases it again.
        const bool overlayBusy = appsPanel_->isOpen() || viewer_->overlay();
        const float wth = std::tan(kFovY * 0.5f), wasp = renderer_.aspect();
        int sw = 1, sh = 1;
        SDL_GetWindowSize(window_, &sw, &sh);
        const WinRef fsw = fullWin();
        if (vrOn_)
            for (const WinRef& w : winOrder_)
                if (winFull(w)) winToggleFull(w);
        if (overlayBusy || winOrder_.empty() || vrOn_) {
            winFocus_ = false;
            recT_ = -1.0f;
        }
        WinRef top;
        bool winModal = false;
        if (fsw.p && !overlayBusy) {
            // A fullscreen window is modal: the world is frozen and the mouse / right stick drive a cursor over it.
            winFocus_ = false;
            recT_ = -1.0f;
            updateCamera(dt, 0, 0, 0, 0, 0);
            cursorX_ = clampf(cursorX_ + in.lookX * 2.0f / std::max(sw, 1) + pad.rx * 1.5f * fdt, -1.0f, 1.0f);
            cursorY_ = clampf(cursorY_ - in.lookY * 2.0f / std::max(sh, 1) - pad.ry * 1.5f * fdt, -1.0f, 1.0f);
            o = {0, 0, 0};
            d = normalize({cursorX_ * wth * wasp, cursorY_ * wth, -1.0f});
            winModal = true;
            top = fsw;
        } else {
            if (in.pin && !overlayBusy && !vrOn_ && recT_ < 0.0f) {
                if (winFocus_) {
                    winFocus_ = false;
                } else {
                    const WinRef t = topWinAt(o, d);
                    if (t.p) flyToWin(t);
                }
            }
            if (recT_ >= 0.0f) {
                recT_ += fdt / 0.7f;
                const float rt = std::min(recT_, 1.0f), ez = rt * rt * (3.0f - 2.0f * rt);
                camera_.position = {lerpf(recFromPos_.x, recToPos_.x, ez), lerpf(recFromPos_.y, recToPos_.y, ez),
                                    lerpf(recFromPos_.z, recToPos_.z, ez)};
                float dy = std::fmod(recToYaw_ - recFromYaw_ + 3.14159265f, 6.2831853f);
                if (dy < 0) dy += 6.2831853f;
                camera_.yaw = recFromYaw_ + (dy - 3.14159265f) * ez;
                camera_.pitch = lerpf(recFromPitch_, recToPitch_, ez);
                cursorX_ = cursorY_ = 0.0f;
                lookX = lookY = moveX = moveY = kbZ = 0.0f;
                if (recT_ >= 1.0f) {
                    recT_ = -1.0f;
                    winFocus_ = true;
                }
            } else if (winFocus_) {
                cursorX_ = clampf(cursorX_ + in.lookX * 2.0f / std::max(sw, 1) + pad.rx * 1.5f * fdt, -1.0f, 1.0f);
                cursorY_ = clampf(cursorY_ - in.lookY * 2.0f / std::max(sh, 1) - pad.ry * 1.5f * fdt, -1.0f, 1.0f);
                lookX = lookY = moveX = moveY = kbZ = 0.0f;
            } else if (!viewer_->overlay() && (cursorX_ != 0.0f || cursorY_ != 0.0f)) {
                // Ease the camera round to the cursor so the aim does not jump when the pin is released.
                const float k = std::min(1.0f, fdt * 12.0f);
                camera_.yaw -= std::atan(cursorX_ * wth * wasp) * k;
                camera_.pitch = clampf(camera_.pitch + std::atan(cursorY_ * wth) * k, -1.4f, 1.4f);
                cursorX_ *= 1.0f - k;
                cursorY_ *= 1.0f - k;
                if (std::fabs(cursorX_) < 0.002f && std::fabs(cursorY_) < 0.002f) cursorX_ = cursorY_ = 0.0f;
            }
            if (!overlayBusy && !vrOn_) d = normalize(camera_.world().transformDir({cursorX_ * wth * wasp, cursorY_ * wth, -1.0f}));
            aimDir_ = d;
            if (!overlayBusy) top = topWinAt(o, d);
        }
        bool oskHit = false;
        if (osk_->isOpen() && !overlayBusy && !winModal) {
            // The keyboard floats in front of the windows: it takes the pointer where it is nearer than the window behind it.
            const float od = osk_->hitDistance(o, d), wd = top.p ? winHit(top, o, d) : -1.0f;
            oskHit = osk_->busy() || (od >= 0.0f && (wd < 0.0f || od <= wd));
            if (oskHit) {
                osk_->pointer(o, d);
                top = {};
            } else {
                osk_->leave();
            }
        }
        bool hvHit = false;
        harvi_->hold(harvi_->panelOpen() && (harvi_->panel().busy() || harvi_->panel().hitDistance(o, d) >= 0.0f));
        if (harvi_->panelOpen() && !overlayBusy && !winModal) {
            const float hd = harvi_->panel().hitDistance(o, d), wd = top.p ? winHit(top, o, d) : -1.0f;
            hvHit = harvi_->panel().busy() || (hd >= 0.0f && (wd < 0.0f || hd <= wd));
            if (hvHit) {
                harvi_->panel().pointer(o, d);
                top = {};
                oskHit = true;
            } else {
                harvi_->panel().leave();
            }
        }
        if (!overlayBusy) {
            for (const WinRef& w : winOrder_) {
                if (fsw.p && w != fsw) winLeave(w, o, d);
                else if (w == top || winBusy(w)) winPointer(w, o, d);
                else winLeave(w, o, d);
            }
            if (top.p) {
                room_->setHover(nullptr);
                if (city_) city_->clearHover();
            }
            for (const WinRef& w : winOrder_)
                if (winMoving(w)) snapDragged(w);
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

            // Escape and back act inside the focused window (leave the address bar or fullscreen, go up a page); a window is
            // never closed by them, so a stray key cannot lose your layout. Back on a pad / VR controller minimises it.
            const WinRef f = focus_;
            if (in.escape && (f.p || winFocus_)) {
                if (!(f.p && winEscape(f))) {
                    if (winFocus_) winFocus_ = false;
                    else setFocus({});
                }
                in.escape = false;
            }
            if ((pad.bPressed || vrInput_.backPressed) && f.p) {
                winBack(f);
                pad.bPressed = back = false;
                vrInput_.backPressed = false;
            }
            if (f.p && !renaming_) {
                if (winTypingNow) {
                    for (EditKey k : in.keys) winKey(f, k);
                    if (!in.text.empty()) winText(f, in.text);
                    in.keys.clear();
                    in.text.clear();
                } else if (f.kind == kWinPanel && f.p == media_.get() && !viewer_->active() && (!fsw.p || fsw == f)) {
                    if (in.mediaVolumeUp) { winKey(f, EditKey::Up); up = pad.upPressed; }
                    if (in.mediaVolumeDown) { winKey(f, EditKey::Down); down = pad.downPressed; }
                    if (in.mediaPlayPause) { winKey(f, EditKey::MediaPlayPause); in.space = false; }
                } else if (f.kind == kWinBrowser) {
                    for (EditKey k : in.keys)
                        if (k == EditKey::Up || k == EditKey::Down || k == EditKey::PageUp || k == EditKey::PageDown || k == EditKey::Home || k == EditKey::End)
                            winKey(f, k);
                } else if (f.kind == kWinSettings) {
                    if (up) settingsPanel_->move(0, -1);
                    if (down) settingsPanel_->move(0, 1);
                    if (in.left || pad.leftPressed) { if (!settingsPanel_->nudge(-1)) settingsPanel_->move(-1, 0); }
                    if (in.right || pad.rightPressed) { if (!settingsPanel_->nudge(1)) settingsPanel_->move(1, 0); }
                    if (in.select) settingsPanel_->activateSelected();
                    up = down = in.left = in.right = in.select = pad.leftPressed = pad.rightPressed = false;
                }
            }
            bool clickEv = in.click || pad.aPressed || vrInput_.selectPressed;
            if (clickEv && oskHit) {
                if (hvHit) {
                    harvi_->panel().click(o, d);
                    if (!in.click) harvi_->panel().releaseButton();
                } else {
                    osk_->click(o, d);
                    if (!in.click) osk_->releaseButton();
                }
                in.click = pointerSelect = keySelect = false;
                pad.aPressed = false;
                vrInput_.selectPressed = false;
                clickEv = false;
            }
            if (resizeInput(o, d, clickEv, in.click, in.clickUp, winModal || oskHit)) {
                in.click = pointerSelect = keySelect = false;
                pad.aPressed = false;
                vrInput_.selectPressed = false;
                clickEv = false;
            }
            if (clickEv) {
                if (top.p) {
                    raiseFocus(top);
                    winPress(top, o, d, !in.click);
                } else if (focus_.kind == kWinSettings && !in.click && pad.aPressed) {
                    settingsPanel_->activateSelected();
                } else if (!fsw.p) {
                    winFocus_ = false;
                    setFocus({});
                }
                if (top.p || (focus_.kind == kWinSettings && !in.click && pad.aPressed)) {
                    in.click = pointerSelect = keySelect = false;
                    pad.aPressed = false;
                    vrInput_.selectPressed = false;
                }
            }
            if (in.clickUp) {
                osk_->releaseButton();
                harvi_->panel().releaseButton();
                for (const WinRef& w : winOrder_) winRelease(w);
            }
            for (const WinRef& w : winOrder_)
                if (in.wheel != 0.0f && winMoving(w)) {
                    winPushPull(w, in.wheel > 0 ? 0.25f : -0.25f);
                    in.wheel = 0.0f;
                }
            if (in.wheel != 0.0f && top.p) {
                winWheel(top, in.wheel);
                in.wheel = 0.0f;
            }
            if (top.kind == kWinBrowser) {
                if (pad.upPressed) winWheel(top, 1.0f);
                if (pad.downPressed) winWheel(top, -1.0f);
            }
            if (in.appFullscreen && !vrOn_) {
                const WinRef t = fsw.p ? fsw : (focus_.kind == kWinApp ? focus_ : top);
                if (t.kind == kWinApp) winToggleFull(t);
            }
#ifdef SCIFI_HAS_CEF
            if (browser_ && browser_->active()) {
                if (in.focusAddress) {
                    raiseFocus({kWinBrowser, browser_.get()});
                    browser_->focusAddress();
                }
                if (in.depthCycle) browser_->cycleDepth();
                if (in.depthSite) browser_->toggleSite();
                if (in.reload) browser_->reload();
                if (in.navBack) browser_->goBack();
                if (in.navForward) browser_->goForward();
            }
#endif
        }
        const bool overWin = (top.p && !winModal) || oskHit;
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
        } else if (winModal) {
            room_->setHover(nullptr);
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
                for (EditKey k : in.keys) viewer_->editKey(k);
                if (!in.text.empty()) viewer_->typeText(in.text);
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
                if (in.left || pad.leftPressed) { if (image) viewer_->step(-1); else viewer_->seekBy(-5.0); }
                if (in.right || pad.rightPressed) { if (image) viewer_->step(1); else viewer_->seekBy(5.0); }
                if (up) viewer_->changeVolume(0.1f);
                if (down) viewer_->changeVolume(-0.1f);
                if (in.fullscreen) viewer_->toggleFullscreen(head_);
                if (back || in.escape) {
                    if (viewer_->fullscreen()) viewer_->toggleFullscreen(head_);
                    else viewer_->close();
                }
                if (viewer_->wantsClose()) viewer_->close();
            }
        } else {
            if (driving_) {
                const Uint8* ks = SDL_GetKeyboardState(nullptr);
                updateCar(dt, clampf(kbY + pad.rt - pad.lt + vrInput_.moveY + scriptThrottle_, -1, 1),
                          clampf(moveX + scriptSteer_, -1, 1), ks[SDL_SCANCODE_SPACE] != 0 || pad.lbHeld,
                          in.shift || pad.rbHeld, lookX, lookY);
            } else {
                updateCamera(dt, moveX, moveY, kbZ, lookX, lookY, freeSpeed);
            }
            static bool dbgPlaced = std::getenv("SCIFI_CITY_STREET") == nullptr;
            if (cityMode_ && !dbgPlaced && city_ && city_->debugStreet(camera_.position)) {
                dbgPlaced = true;
                flying_ = false;
                camera_.yaw = kPi;
                camera_.pitch = 0.0f;
                camera_.yaw = 0.0f;
            }
            hudTarget_.clear();
            if (driving_) hudTarget_ = onTrack_ ? "ESC / Y EXIT TRACK   RT/LT DRIVE   RB/LB BOOST / DRIFT"
                                               : "F10 / Y EXIT   C CAMERA   RT/LT DRIVE   RB/LB BOOST / DRIFT";
            if (cityMode_ && !driving_) {
                room_->setHover(nullptr);
                std::string path, name;
                std::vector<Vec3> routePath;
                int standKind = -1;
                if (!overWin && city_ && !riding_ && city_->holding() && pointerSelect) {
                    city_->throwHeld(head_.eye);
                    hudTarget_.clear();
                } else if (!overWin && city_ && !riding_ && !city_->holding() && city_->pickStand(o, d, standKind)) {
                    hudTarget_ = standKind == 0 ? "HOT DOG" : "ICE CREAM";
                    if (pointerSelect || keySelect) { city_->giveItem(standKind); sfx_.play(SfxId::Pickup); }
                } else if (!overWin && city_ && city_->pick(o, d, path, name)) {
                    hudTarget_ = name;
                    if ((pointerSelect || keySelect) && !deferred_) {
                        city_->markEntered();
                        city_->clearHover();
                        deferred_ = [this, path, name] {
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
                        };
                        deferDelay_ = 3;
                    }
                } else if (!overWin && city_ && !riding_ && city_->pickMonument(o, d) >= 0 && !(pointerSelect || keySelect)) {
                    static const char* const kMon[] = {"CPU", "RAM", "GPU"};
                    hudTarget_ = kMon[std::clamp(city_->pickMonument(o, d), 0, 2)];
                } else if (!overWin && city_ && (pointerSelect || keySelect) && !riding_ && city_->pickMonument(o, d) >= 0) {
                    monitor_->setFocus(city_->pickMonument(o, d));
                    openPanel(monitor_.get());
                } else if (!overWin && pointerSelect && city_ && !riding_ && city_->pickRoute(o, d, routePath)) {
                    startRide(routePath);
                }
            } else {
            if (city_) city_->clearHover();
            Node* hit = overWin ? nullptr : pickNode(room_->root(), o, d);
            room_->setHover(hit);
            if ((pointerSelect || keySelect) && hit && !deferred_) {
                if (room_->opensRoom(hit)) {
                    deferred_ = [this, hit] { menu_->setStatus(room_->activate(hit)); };
                    deferDelay_ = 3;
                } else {
                    menu_->setStatus(room_->activate(hit));
                }
            }
            if (back || (in.escape && room_->inRoom())) menu_->setStatus(room_->goUp());
            else if (in.escape) toggleCity(true);
            }
#ifdef SCIFI_HAS_CEF
            if (in.browser && browser_ && !winTypingNow) {
                if (browser_->active()) raiseFocus({kWinBrowser, browser_.get()});
                else browser_->open("", head_.eye, head_.forward, vrOn_);
            }
#endif
        }
    }

    {
        const WinRef fullscreen = fullWin();
        const bool focused = state_ == State::FileRoom && !switcher_->isOpen() && !appsPanel_->isOpen() &&
                             !viewer_->active() && !renaming_ && !input_.capturing && focus_.kind == kWinPanel &&
                             focus_.p == media_.get() && !media_->wantsText() &&
                             (!fullscreen.p || fullscreen == focus_) &&
                             (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) &&
                             !(SDL_GetModState() & (KMOD_CTRL | KMOD_ALT | KMOD_GUI | KMOD_SHIFT));
        const NowInfo now = mediaNow();
        if (now.path != mediaSeekPath_) {
            mediaSeek_.reset();
            mediaSeekPath_ = now.path;
        }
        double target = 0;
        if (mediaSeek_.update(focused && now.active && !now.live && !now.loading,
                              in.mediaSeekLeft, in.mediaSeekRight, in.mediaSeekLeftHeld, in.mediaSeekRightHeld,
                              in.mediaSeekLeftReleased, in.mediaSeekRightReleased,
                              static_cast<double>(SDL_GetTicks64()) / 1000.0, now.pos, now.dur, target))
            mediaPlayer_.seek(target);
    }
    if (riding_) rideStep(dt);
    if (tourActive_) tourStep(dt);
    {
        const bool want = deferred_ || loadingT_ > 0.0f || (cityMode_ && city_ && !city_->built());
        if (loadingT_ > 0.0f) loadingT_ -= static_cast<float>(dt);
        loaderA_ += ((want ? 1.0f : 0.0f) - loaderA_) * std::min(1.0f, static_cast<float>(dt) * 8.0f);
        if (!want && loaderA_ < 0.01f) loaderA_ = 0.0f;
    }
    time_ += dt;
    {
        const std::time_t nowT = std::time(nullptr);
        std::tm lt{};
        localtime_s(&lt, &nowT);
        char tb[16], db[64];
        std::strftime(tb, sizeof tb, "%H:%M", &lt);
        std::strftime(db, sizeof db, "%A %d %B %Y", &lt);
        const std::string full = std::string(db) + "  " + tb;
        if (!hudTimeLabel_ || hudTimeText_ != full) {
            hudTimeText_ = full;
            hudTimeLabel_ = renderer_.createLabel(makeLabel(full, 40.0f, FontFace::UI, Color8{}, 64, 4));
        }
        const std::string want = cityMode_ ? hudTarget_ : std::string();
        if (want != hudTargetShown_) {
            hudTargetShown_ = want;
            hudTargetLabel_ = want.empty() ? nullptr : renderer_.createLabel(makeLabel(want, 54.0f, FontFace::UI, Color8{}, 96, 4));
        }
    }
    fpsAccum_ += dt;
    ++fpsFrames_;
    if (fpsAccum_ >= 0.5) {
        const int fps = static_cast<int>(fpsFrames_ / fpsAccum_ + 0.5);
        fpsAccum_ = 0;
        fpsFrames_ = 0;
        if (settings_.showFps)
            fpsLabel_ = renderer_.createLabel(makeLabel(std::to_string(fps) + " FPS", 40.0f, FontFace::UI, Color8{}, 96, 4));
    }
    {
        const Theme& th = currentTheme();
        renderer_.setTheme(th.accent, th.alt);
        renderer_.setTime(static_cast<float>(time_));
        room_->setAccent(th.accent);
        start_->update(dt);
        settingsPanel_->update(dt, time_);
        monitor_->update(dt);
        osk_->update(dt);
        terminal_->update(dt);
        files2d_->update(dt);
        editor2d_->update(dt);
        imageEd_->update(dt);
        ftp_->update(dt);
        calc_->update(dt);
        media_->update(dt);
        mediaStep();
        appsPanel_->update(dt, time_);
    }
    scriptTick(dt);
    {
        const Vec3 hEye = vrOn_ ? head_.eye : camera_.position;
        const Vec3 hFwd = vrOn_ ? head_.forward : camera_.forward();
        harvi_->setContext(driving_ ? "car" : (focus_.p && focus_.kind == kWinSettings ? "settings" : ""));
        harvi_->update(dt, hEye, hFwd, vrOn_);
        const std::string act = harvi_->takeAction();
        if (act == "tool:settings") runTool(Tool::Settings);
        else if (act == "tool:media") runTool(Tool::Media);
        else if (act == "key:arrange") arrangeCycle();
        else if (act.rfind("page:", 0) == 0) {
            runTool(Tool::Settings);
            pendingMediaPage_ = act.substr(5);
        }
    }
    if (state_ == State::FileRoom) {
        room_->pump();
        if (cityMode_) renderer_.clearLights();
        { Vec3 sp; float sy; if (room_->takeSpawn(sp, sy)) { camera_.position = sp; camera_.yaw = sy; camera_.pitch = 0; room_->setEye(sp); } }
        viewer_->update(dt, head_);
        if (!pendingMediaPage_.empty()) {
            const std::string page = pendingMediaPage_;
            pendingMediaPage_.clear();
            buildSettingsHub();
            if (settingsPanel_->isOpen()) settingsPanel_->openPage(page);
        }
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
    if (city_ && cityStarted_) {
        carSpawn_ = std::min(1.0f, carSpawn_ + float(dt) * 2.5f);
        City::CarView cv;
        cv.show = carOn_;
        cv.hideBody = driving_ && carFirst_;
        cv.solid = !driving_;
        cv.pos = carPos_;
        cv.yaw = carYaw_;
        cv.pitch = carPitch_;
        cv.steer = carSteer_;
        cv.speed = carSpeed_;
        cv.spin = carSpin_;
        cv.rail = onTrack_;
        cv.railT = railT_;
        cv.railU = railU_;
        trackFade_ = clampf(trackFade_ + float(dt) * (driving_ ? 0.5f : -0.85f), 0.0f, 1.0f);
        city_->setTrackFade(trackFade_);
        cv.spawn = carSpawn_;
        city_->setCar(cv);
        sfx_.setCar(driving_ && cityMode_, carSpeed_, carBoostOn_);
        City::SfxEvent se;
        while (city_->takeSfx(se)) {
            const float d = length(se.p - head_.eye);
            const float vol = clampf(1.0f - d / (se.id == 2 ? 140.0f : 60.0f), 0.0f, 1.0f);
            sfx_.play(se.id == 1 ? SfxId::Throw : se.id == 2 ? SfxId::Boom : SfxId::DuckPop, se.id == 1 ? 0.8f : 0.15f + 0.85f * vol);
        }
        city_->setLook(head_.forward);
        city_->update(dt, state_ == State::FileRoom, head_.eye);
        int pts = 0, tot = 0;
        scoreT_ = std::max(0.0f, scoreT_ - float(dt));
        if (city_->takeScore(pts, tot)) {
            scoreLabel_ = renderer_.createLabel(makeLabel("+" + std::to_string(pts) + "   SCORE " + std::to_string(tot), 54.0f, FontFace::UI, Color8{}, 96, 4));
            scoreT_ = 3.5f;
        }
        Vec3 imp;
        if (cityMode_ && city_->takeBlast(imp)) {
            knock_ = {imp.x, 0.0f, imp.z};
            vy_ += imp.y;
            flying_ = false;
        }
    }
    if (clearRoomPending_) {
        clearRoomPending_ = false;
        if (cityMode_ && state_ == State::FileRoom) room_->loadEmpty();
    }
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
        if (viewer_->controlsShown() && arrow_) {
            drawArrow2D();
                    }
                    return;
                }
    // Planar reflection: draw the room flipped under the floor first; the translucent floor then lets it show through.
    if (state_ == State::FileRoom && desktopPass && !vrOn_) {
        // A fullscreen window fills the view; the pointer is a virtual cursor over it.
        const WinRef fw = fullWin();
        if (fw.p) {
            renderer_.setView(Mat4::identity(), proj);
            renderer_.clearDepth();
            winRender(fw);
            if (arrow_ && !switcher_->isOpen()) drawArrow2D();
            switcher_->render(Mat4::identity(), renderer_.aspect(), std::tan(kFovY * 0.5f), false);
            return;
        }
    }    static const bool dbgNoMirror = std::getenv("SCIFI_NOMIRROR") != nullptr, dbgNoPost = std::getenv("SCIFI_NOPOST") != nullptr;
    renderer_.setPostFx(settings_.postFx && !dbgNoPost);
    renderer_.setFogTune(settings_.fogDensity, settings_.fogHeight, settings_.fogBright, settings_.fogTint, settings_.fogStart, settings_.fogMist, settings_.fogClarity, settings_.fogGlow);
    renderer_.setLightTune(settings_.lightSpot, settings_.lightCap, settings_.lightBase, settings_.lightAmbient, settings_.lightFalloff);
    if (city_) city_->setDrawScale(settings_.distScale());
    if (state_ == State::FileRoom && !dbgNoMirror && true) {
        renderer_.beginReflection();
        renderer_.setMirror(true);
        if (settings_.reflections) {  // when off the target is just cleared: a plain dark floor
            activeRoot().renderTree(renderer_);
            if (cityMode_ && city_) {
                city_->render(renderer_, realEye);
                if (riding_ && rideT_ > 0.3f) city_->renderPacket(renderer_, ridePos_, ridePath_, rideDist_);
            }
        }
        renderer_.setMirror(false);
        renderer_.endReflection();
    }
    activeRoot().renderTree(renderer_);
    if (cityMode_ && city_) {
            city_->render(renderer_, realEye);
            if (riding_ && rideT_ > 0.3f) city_->renderPacket(renderer_, ridePos_, ridePath_, rideDist_);
        }
    if (cityMode_ && city_ && !dbgNoPost) renderer_.postWorld();  // effects are for the city; inside buildings and rooms text stays sharp
    if (state_ == State::FileRoom && viewer_->active()) {
        renderer_.clearDepth();
        viewer_->root().renderTree(renderer_);
    }
    if (state_ == State::FileRoom)
        for (const WinRef& w : winOrder_) {
            if (winFull(w)) continue;
            renderer_.clearDepth();
            winRender(w);
            renderHandle(w);
        }
    if (state_ == State::FileRoom && osk_->isOpen()) {
        renderer_.clearDepth();
        osk_->render();
    }
    if (harvi_->active()) harvi_->render(*crosshair_);
    if (state_ == State::FileRoom && appsPanel_->isOpen()) {
        renderer_.clearDepth();
        appsPanel_->root().renderTree(renderer_);
    }
    if (switcher_->isOpen()) switcher_->render(view, renderer_.aspect(), std::tan(kFovY * 0.5f), vrOn_);
    if (loaderA_ > 0.01f && loadLabel_) {
        // White loading ring, modelled on the command ring: segments chase round it so it is clear the shell is working.
        renderer_.clearDepth();
        const float dist = 1.2f, dia = 0.34f;
        const Vec3 at = vrOn_ ? Vec3{0, 0, -dist} : Vec3{0, 0, -dist};
        const float t = static_cast<float>(time_);
        renderer_.setTheme({1, 1, 1}, {1, 1, 1});
        renderer_.drawRing(*loadLabel_, view.rigidInverse() * Mat4::translate(at) * Mat4::scale({dia, dia, 1.0f}), 12,
                           std::floor(std::fmod(t * 9.0f, 12.0f)), t * 1.2f, loaderA_, 0.5f + 0.5f * std::sin(t * 6.0f));
        renderer_.setTheme(currentTheme().accent, currentTheme().alt);
    }
    if (hudTimeLabel_) {
        renderer_.clearDepth();
        const float dist = 1.2f, halfH = std::tan(kFovY * 0.5f) * dist, halfW = halfH * renderer_.aspect();
        auto put = [&](const GpuLabel& lab, float h, Vec3 at) {
            renderer_.drawHudText(lab, view.rigidInverse() * Mat4::translate(at) * Mat4::scale({h * lab.aspect, h, 1.0f}));
        };
        const float th = 0.03f;
        const float tw = th * hudTimeLabel_->aspect;
        const float rx = vrOn_ ? 0.3f : halfW - 0.008f, ty = vrOn_ ? 0.2f : halfH - 0.008f - th * 0.5f;
        put(*hudTimeLabel_, th, {rx - tw * 0.5f, ty, -dist});
        if (driving_) {
            const int kmh = int(std::fabs(carSpeed_) * 3.6f / 2.0f + 0.5f) * 2;
            auto& sl = speedLabels_[kmh];
            if (!sl) sl = renderer_.createLabel(makeLabel(std::to_string(kmh), 72.0f, FontFace::UI, Color8{}, 96, 4));
            if (!kmhLabel_) kmhLabel_ = renderer_.createLabel(makeLabel("KM/H", 40.0f, FontFace::UI, Color8{}, 96, 4));
            const float by = vrOn_ ? -0.2f : -halfH * 0.8f, bx = halfW * 0.72f;
            put(*sl, 0.1f, {bx, by + 0.06f, -dist});
            put(*kmhLabel_, 0.035f, {bx, by - 0.03f, -dist});
            const int segs = 24, lit = std::min(segs, int(std::fabs(carSpeed_) / 100.0f * segs + 0.5f));
            for (int i = 0; i < segs; ++i) {
                const bool on = i < lit;
                const Vec3 col = on ? (i > segs * 0.75f ? Vec3{1.0f, 0.2f, 0.3f} : Vec3{0.1f, 0.9f, 1.0f}) : Vec3{0.05f, 0.14f, 0.18f};
                const float sx = bx - 0.3f + i * 0.0262f;
                renderer_.drawCube(*crosshair_, view.rigidInverse() * Mat4::translate({sx, by - 0.075f, -dist}) * Mat4::scale({0.009f, 0.012f + 0.0004f * i, 0.0005f}), col);
            }
        }        if (scoreLabel_ && scoreT_ > 0.0f) put(*scoreLabel_, 0.07f, {0.0f, vrOn_ ? 0.18f : halfH * 0.55f, -dist});
        if (hudTargetLabel_) {
            const float gh = 0.06f;
            put(*hudTargetLabel_, gh, {0.0f, vrOn_ ? -0.22f : -halfH * 0.62f, -dist});
        }
    }
    if (settings_.showFps && fpsLabel_) {
        // Head-locked readout in the top-left of the view.
        renderer_.clearDepth();
        const float h = 0.03f, w = h * fpsLabel_->aspect;
        const float dist = 1.2f, halfH = std::tan(kFovY * 0.5f) * dist;
        Vec3 at = vrOn_ ? Vec3{-0.28f, 0.2f, -dist} : Vec3{-halfH * renderer_.aspect() + w * 0.5f + 0.008f, halfH - h * 0.5f - 0.008f, -dist};
        renderer_.drawHudText(*fpsLabel_, view.rigidInverse() * Mat4::translate(at) * Mat4::scale({w, h, 1.0f}));
    }
    if (vrOn_ && vrInput_.valid && !vrGaze_) {
        renderer_.drawCube(*crosshair_, alignZ(vrInput_.pointerOrigin + vrInput_.pointerDir * 1.5f, vrInput_.pointerDir,
                                                {0.006f, 0.006f, 3.0f}),
                           {1.0f, 0.7f, 0.2f});
    } else if (state_ == State::FileRoom && (settings_.crosshair || vrOn_)) {
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
            // In VR the rig is the tracking-space origin: at the player's position and heading, and at the flown altitude
            // minus the head's own height, so the headset follows the same height the desktop view shows.
            const float rigY = std::max(0.0f, camera_.position.y - vrHeadY_);
            Mat4 rig = Mat4::translate({camera_.position.x, rigY, camera_.position.z}) * Mat4::rotateY(camera_.yaw);
            vr_.frame(
                renderer_, rig,
                [&](const EyeView& ev) {
                    Vec3 eyePos = ev.view.rigidInverse().transformPoint({0, 0, 0});
                    if (vrHeadY_ < 0.3f) vrHeadY_ = std::clamp(eyePos.y - rigY, 0.0f, 2.5f);
                    renderScene(ev.view, ev.proj, eyePos, false);
                },
                vrInput_);
        }
        renderer_.beginDesktopFrame();
        renderScene(camera_.world().rigidInverse(), Mat4::perspective(kFovY, renderer_.aspect(), 0.05f, 2500.0f),
                    camera_.position, true);
        renderer_.present(!vrOn_ && settings_.vsync);
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
                         vrOn_ ? head_.forward : camera_.forward());
    settingsPanel_->openPage("FILE");
}

}  // namespace sf
