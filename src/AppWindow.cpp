#define NOMINMAX
#include "AppWindow.h"

#include <windows.h>

#include <UIAutomation.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <map>
#include <thread>
#include <vector>

#include "TextRender.h"

#pragma comment(lib, "dwmapi.lib")

namespace sf {

namespace {
constexpr int kTexW = 3840, kTexH = 2160;       // GPU texture; the program's window is copied into its top-left corner
constexpr int kWinW = 1600, kWinH = 960;        // size the program's window is given
constexpr int kOffX = -24000, kOffY = 200;      // parked off-screen position
constexpr int kBarW = 1920, kBarH = 80;
constexpr float kBarRatio = static_cast<float>(kBarH) / kBarW;

struct Rgb {
    float r, g, b;
};
constexpr Rgb kBarBg{0.07f, 0.09f, 0.15f}, kText{0.93f, 0.95f, 0.98f}, kDim{0.55f, 0.60f, 0.72f}, kHot{0.20f, 0.26f, 0.42f},
    kDanger{0.55f, 0.16f, 0.20f};

void putPixel(Bitmap& bm, int x, int y, Rgb c, float cov) {
    if (x < 0 || y < 0 || x >= bm.w || y >= bm.h || cov <= 0.0f) return;
    cov = std::min(cov, 1.0f);
    uint8_t* d = &bm.rgba[(static_cast<size_t>(y) * bm.w + x) * 4];
    d[0] = static_cast<uint8_t>(d[0] + (c.r * 255.0f - d[0]) * cov + 0.5f);
    d[1] = static_cast<uint8_t>(d[1] + (c.g * 255.0f - d[1]) * cov + 0.5f);
    d[2] = static_cast<uint8_t>(d[2] + (c.b * 255.0f - d[2]) * cov + 0.5f);
    d[3] = 255;
}

void fillRoundRect(Bitmap& bm, float x0, float y0, float x1, float y1, float r, Rgb c) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    r = std::min(r, std::min(hx, hy));
    for (int y = static_cast<int>(y0) - 1; y <= static_cast<int>(y1) + 1; ++y)
        for (int x = static_cast<int>(x0) - 1; x <= static_cast<int>(x1) + 1; ++x) {
            float qx = std::fabs(x + 0.5f - cx) - hx + r, qy = std::fabs(y + 0.5f - cy) - hy + r;
            float d = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f)) +
                      std::min(std::max(qx, qy), 0.0f) - r;
            putPixel(bm, x, y, c, clampf(0.5f - d, 0.0f, 1.0f));
        }
}

void strokeLine(Bitmap& bm, float x0, float y0, float x1, float y1, float w, Rgb c) {
    float minX = std::min(x0, x1) - w, maxX = std::max(x0, x1) + w, minY = std::min(y0, y1) - w, maxY = std::max(y0, y1) + w;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = static_cast<int>(minY); y <= static_cast<int>(maxY); ++y)
        for (int x = static_cast<int>(minX); x <= static_cast<int>(maxX); ++x) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? clampf((px * dx + py * dy) / len2, 0.0f, 1.0f) : 0.0f;
            float ex = px - dx * t, ey = py - dy * t;
            float d = std::sqrt(ex * ex + ey * ey) - w * 0.5f;
            putPixel(bm, x, y, c, clampf(0.5f - d, 0.0f, 1.0f));
        }
}

constexpr float kHideX0 = 1574, kHideX1 = 1674, kFullX0 = 1690, kFullX1 = 1790, kCloseX0 = 1806, kCloseX1 = 1906;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// A window worth hosting: a visible, un-owned, titled top-level window that is not cloaked.
bool isCandidate(HWND h, DWORD ownPid) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return false;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == ownPid) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    RECT r;
    GetWindowRect(h, &r);
    return r.right - r.left >= 160 && r.bottom - r.top >= 100;
}

std::set<HWND> snapshotWindows(DWORD ownPid) {
    std::set<HWND> out;
    struct Ctx {
        std::set<HWND>* s;
        DWORD pid;
    } ctx{&out, ownPid};
    EnumWindows(
        [](HWND h, LPARAM l) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(l);
            if (isCandidate(h, c->pid)) c->s->insert(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return out;
}

bool isHostClass(HWND h) {
    wchar_t cls[96] = {};
    GetClassNameW(h, cls, 96);
    std::wstring c = cls;
    return c.rfind(L"Microsoft.UI.Content", 0) == 0 || c.rfind(L"InputSite", 0) == 0 || c.rfind(L"InputNonClient", 0) == 0 ||
           c.rfind(L"Windows.UI", 0) == 0;
}

// Deepest real child control (an edit box, say) that covers the most area; the default keyboard target.
HWND largestControl(HWND top) {
    struct Ctx {
        HWND best = nullptr;
        long long area = 0;
    } ctx;
    EnumChildWindows(
        top,
        [](HWND c, LPARAM l) -> BOOL {
            auto* x = reinterpret_cast<Ctx*>(l);
            if (!IsWindowVisible(c) || isHostClass(c)) return TRUE;
            RECT r;
            GetWindowRect(c, &r);
            long long a = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
            if (a >= x->area) {
                x->area = a;
                x->best = c;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    ~Dib() { reset(); }
    void reset() {
        if (dc) {
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
        }
        dc = nullptr;
        bmp = nullptr;
        bits = nullptr;
        w = h = 0;
    }
    bool ensure(int nw, int nh) {
        if (dc && nw == w && nh == h) return true;
        reset();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = nw;
        bi.bmiHeader.biHeight = -nh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) {
            reset();
            return false;
        }
        old = static_cast<HBITMAP>(SelectObject(dc, bmp));
        w = nw;
        h = nh;
        return true;
    }
};

bool grab(Dib& dib, HWND h, int w, int hh) {
    if (!dib.ensure(w, hh)) return false;
    std::memset(dib.bits, 0, static_cast<size_t>(w) * hh * 4);
    return PrintWindow(h, dib.dc, 2 /* PW_RENDERFULLCONTENT */) != 0;
}

inline uint32_t bgraToRgba(uint32_t c) { return (c & 0x0000FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu) | 0xFF000000u; }

// Windows already hosted by some AppWindow, so a second launch never grabs the first one's window.
std::mutex g_claimMutex;
std::set<HWND> g_claimed;
bool isClaimed(HWND h) {
    std::lock_guard<std::mutex> lock(g_claimMutex);
    return g_claimed.count(h) != 0;
}
void claim(HWND h) {
    std::lock_guard<std::mutex> lock(g_claimMutex);
    g_claimed.insert(h);
}
void unclaim(HWND h) {
    std::lock_guard<std::mutex> lock(g_claimMutex);
    g_claimed.erase(h);
}
}  // namespace

struct AppWindowState {
    std::shared_ptr<GpuLabel> tex, bar;
    Mat4 base = Mat4::identity();
    float worldW = 2.4f, worldH = 1.5f, barH = 2.4f * kBarRatio;
    HWND owner = nullptr;
    DWORD ownPid = 0;

    std::thread worker;
    std::atomic<bool> stop{false}, capturing{false}, exited{false}, failed{false};
    std::atomic<uintptr_t> hwnd{0};
    std::set<HWND> before;

    std::mutex m;  // guards everything below
    std::vector<uint32_t> front;
    int fw = 0, fh = 0;
    bool fresh = false;
    RECT mainRect{}, popupRect{};
    HWND popup = nullptr;
    std::string windowTitle;

    int contentW = 0, contentH = 0;  // main thread only: last uploaded size
    int mouseX = -1, mouseY = -1;
    HWND lastTarget = nullptr;
    std::string status;
    std::string shownTitle;

    // XAML flyouts (menus, drop-downs) are separate windows; each is composited into the panel at (cx, cy).
    struct PopupInfo {
        HWND h;
        int cx, cy, w, hgt;
        RECT sr;  // its real screen rect, for mapping panel clicks into it
    };
    std::vector<PopupInfo> popups;  // guarded by m
    std::vector<POINT> clicks;      // panel clicks on XAML chrome, handled by the UI Automation thread; guarded by m
    POINT pendingPos{0, 0};         // panel position for the next flyout to appear; guarded by m
    std::atomic<bool> uiaStop{false};
    std::atomic<int> wantW{kWinW}, wantH{kWinH};  // size the program's window should have (changes in fullscreen)
};

// Modern XAML chrome (tabs, menus, buttons) ignores posted mouse messages, so clicks on it are performed through
// UI Automation (Invoke / Expand / Select / Toggle) by the SciFiUia helper process. That works on the off-screen
// window with no focus change.

// A child process (SciFiUia) performs the UI Automation calls: Notepad dismisses flyouts opened by an in-process client.
struct UiaProcess {
    HANDLE in = nullptr, out = nullptr, proc = nullptr;  // our write end of its stdin, our read end of its stdout

    bool start() {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring exe(path);
        exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"SciFiUia.exe";
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE inR = nullptr, outW = nullptr;
        if (!CreatePipe(&inR, &in, &sa, 0) || !CreatePipe(&out, &outW, &sa, 0)) return false;
        SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inR;
        si.hStdOutput = outW;
        si.hStdError = outW;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, TRUE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi);
        CloseHandle(inR);
        CloseHandle(outW);
        if (!ok) return false;
        CloseHandle(pi.hThread);
        proc = pi.hProcess;
        return true;
    }

    bool send(const char* text) {
        DWORD n = 0;
        return in && WriteFile(in, text, static_cast<DWORD>(std::strlen(text)), &n, nullptr);
    }

    // Reads the helper's one-line reply: ok, opened, element rect.
    bool reply(int& ok, int& opened, RECT& r) {
        char line[160];
        size_t len = 0;
        while (len < sizeof(line) - 1) {
            DWORD n = 0;
            char c;
            if (!ReadFile(out, &c, 1, &n, nullptr) || n == 0) return false;
            if (c == '\n') break;
            line[len++] = c;
        }
        line[len] = 0;
        return std::sscanf(line, "R %d %d %ld %ld %ld %ld", &ok, &opened, &r.left, &r.top, &r.right, &r.bottom) == 6;
    }

    void stop() {
        if (in) CloseHandle(in);  // the helper exits when its stdin closes
        if (proc) {
            if (WaitForSingleObject(proc, 1000) == WAIT_TIMEOUT) TerminateProcess(proc, 0);
            CloseHandle(proc);
        }
        if (out) CloseHandle(out);
        in = out = proc = nullptr;
    }
};

// Performs panel clicks on the XAML chrome and on flyouts through the UI Automation helper.
static void uiaMain(AppWindowState* s) {
    UiaProcess helper;
    const bool up = helper.start();
    while (!s->uiaStop) {
        Sleep(8);
        HWND h = reinterpret_cast<HWND>(s->hwnd.load());
        std::vector<POINT> clicks;
        std::vector<AppWindowState::PopupInfo> snapshot;
        {
            std::lock_guard<std::mutex> lock(s->m);
            if (!h || !up) {
                s->clicks.clear();
                continue;
            }
            clicks.swap(s->clicks);
            snapshot = s->popups;
        }
        for (const POINT& c : clicks) {
            RECT mr{};
            GetWindowRect(h, &mr);
            // Which flyout (if any) is under the click? The newest is on top.
            const AppWindowState::PopupInfo* hit = nullptr;
            for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it)
                if (c.x >= it->cx && c.x < it->cx + it->w && c.y >= it->cy && c.y < it->cy + it->hgt) {
                    hit = &*it;
                    break;
                }
            if (!hit && !snapshot.empty()) {  // clicking away from an open menu dismisses it
                helper.send("X\n");
                continue;
            }
            char cmd[96];
            if (hit)
                std::snprintf(cmd, sizeof(cmd), "F %llx %ld %ld\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hit->h)),
                              hit->sr.left + (c.x - hit->cx), hit->sr.top + (c.y - hit->cy));
            else
                std::snprintf(cmd, sizeof(cmd), "C %ld %ld\n", mr.left + c.x, mr.top + c.y);
            int ok = 0, opened = 0;
            RECT er{};
            if (!helper.send(cmd) || !helper.reply(ok, opened, er)) break;
            if (ok && opened) {
                std::lock_guard<std::mutex> lock(s->m);
                if (hit)
                    s->pendingPos = {hit->cx + hit->w - 8, hit->cy + (er.top - hit->sr.top) - 6};
                else
                    s->pendingPos = {er.left - mr.left, er.bottom - mr.top};
            }
        }
    }
    helper.stop();
}

// Finds the program's new window, parks it off-screen, then keeps copying it into `front`.
static void workerMain(AppWindowState* s) {
    Dib mainDib, popDib;
    int appliedW = kWinW, appliedH = kWinH;
    std::vector<uint32_t> canvas;
    const auto started = std::chrono::steady_clock::now();
    auto lastFrame = started;
    std::map<HWND, POINT> popPos;  // where each flyout is drawn on the panel
    std::vector<HWND> popList;
    auto lastEnum = started;
    // UI Automation calls run on their own thread: issuing them from the capture thread made Notepad dismiss flyouts.
    struct UiaThread {
        AppWindowState* s;
        std::thread t;
        explicit UiaThread(AppWindowState* st) : s(st) {
            s->uiaStop = false;
            t = std::thread(uiaMain, st);
        }
        ~UiaThread() {
            s->uiaStop = true;
            t.join();
        }
    } uiaThread(s);
    while (!s->stop) {
        HWND h = reinterpret_cast<HWND>(s->hwnd.load());
        if (!h) {
            struct Ctx {
                AppWindowState* s;
                HWND found;
            } ctx{s, nullptr};
            EnumWindows(
                [](HWND w, LPARAM l) -> BOOL {
                    auto* c = reinterpret_cast<Ctx*>(l);
                    if (isCandidate(w, c->s->ownPid) && !c->s->before.count(w) && !isClaimed(w)) {
                        c->found = w;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&ctx));
            if (ctx.found) {
                // Park the window off-screen at a known size and hand focus straight back to the shell.
                if (IsIconic(ctx.found) || IsZoomed(ctx.found)) ShowWindow(ctx.found, SW_RESTORE);
                SetWindowPos(ctx.found, HWND_BOTTOM, kOffX, kOffY, kWinW, kWinH, SWP_NOACTIVATE);
                // A non-activating window can't pull the foreground (and the shell's mouse capture) away on a click.
                SetWindowLongPtrW(ctx.found, GWL_EXSTYLE, GetWindowLongPtrW(ctx.found, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
                if (s->owner) SetForegroundWindow(s->owner);
                claim(ctx.found);
                s->hwnd = reinterpret_cast<uintptr_t>(ctx.found);
                continue;
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(20)) {
                s->failed = true;
                return;
            }
            Sleep(6);
            continue;
        }
        if (!IsWindow(h)) {
            s->exited = true;
            return;
        }
        if (!s->capturing) {
            Sleep(40);
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFrame < std::chrono::milliseconds(30)) {
            Sleep(4);
            continue;
        }
        lastFrame = now;

        RECT r{};
        GetWindowRect(h, &r);
        int w = r.right - r.left, hh = r.bottom - r.top;
        if (s->wantW != appliedW || s->wantH != appliedH) {
            appliedW = s->wantW;
            appliedH = s->wantH;
            SetWindowPos(h, nullptr, 0, 0, appliedW, appliedH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            continue;
        }
        if (w > kTexW || hh > kTexH) {  // programs that grow past the texture are pulled back to size
            SetWindowPos(h, nullptr, 0, 0, std::min(w, kWinW), std::min(hh, kWinH), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            continue;
        }
        if (w <= 0 || hh <= 0 || !grab(mainDib, h, w, hh)) {
            Sleep(30);
            continue;
        }
        canvas.resize(static_cast<size_t>(w) * hh);
        const uint32_t* src = static_cast<const uint32_t*>(mainDib.bits);
        for (size_t i = 0; i < canvas.size(); ++i) canvas[i] = bgraToRgba(src[i]);

        // A modal dialog (save prompt, file picker) is a separate owned window: composite it over the main one.
        RECT pr{};
        HWND pop = GetLastActivePopup(h);
        if (pop && pop != h && IsWindowVisible(pop) && GetWindow(pop, GW_OWNER) == h && !isHostClass(pop)) {
            GetWindowRect(pop, &pr);
            RECT inter;
            if (!IntersectRect(&inter, &pr, &r)) {  // opened somewhere visible: park it over the hosted window
                int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
                SetWindowPos(pop, nullptr, r.left + (w - pw) / 2, r.top + (hh - ph) / 2, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                GetWindowRect(pop, &pr);
            }
            int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
            if (pw > 0 && ph > 0 && grab(popDib, pop, pw, ph)) {
                const uint32_t* ps = static_cast<const uint32_t*>(popDib.bits);
                for (int y = 0; y < ph; ++y) {
                    int cy = pr.top - r.top + y;
                    if (cy < 0 || cy >= hh) continue;
                    for (int x = 0; x < pw; ++x) {
                        int cx = pr.left - r.left + x;
                        if (cx < 0 || cx >= w) continue;
                        canvas[static_cast<size_t>(cy) * w + cx] = bgraToRgba(ps[static_cast<size_t>(y) * pw + x]);
                    }
                }
            } else {
                pop = nullptr;
            }
        } else {
            pop = nullptr;
        }

        // XAML flyouts (menus, drop-downs) are separate top-level windows of the same process: composite them too.
        std::vector<AppWindowState::PopupInfo> shown;
        {
            if (now - lastEnum > std::chrono::milliseconds(80)) {
                lastEnum = now;
                struct Ctx {
                    DWORD pid;
                    std::vector<HWND> list;
                } bc{0, {}};
                GetWindowThreadProcessId(h, &bc.pid);
                EnumWindows(
                    [](HWND w2, LPARAM l) -> BOOL {
                        auto* c = reinterpret_cast<Ctx*>(l);
                        if (!IsWindowVisible(w2)) return TRUE;
                        wchar_t cls[64] = {};
                        GetClassNameW(w2, cls, 64);
                        if (std::wcscmp(cls, L"Microsoft.UI.Content.PopupWindowSiteBridge") != 0) return TRUE;
                        DWORD p = 0;
                        GetWindowThreadProcessId(w2, &p);
                        if (p == c->pid) c->list.push_back(w2);
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&bc));
                popList.swap(bc.list);
            }
            for (HWND b : popList) {
                RECT br;
                if (!IsWindow(b) || !IsWindowVisible(b) || !GetWindowRect(b, &br)) continue;
                const int bw = br.right - br.left, bh = br.bottom - br.top;
                if (bw < 8 || bh < 8 || bw > w || bh > hh) continue;
                if (!grab(popDib, b, bw, bh)) continue;
                {
                    // Composition-rendered popups can print as black; fall back to the live screen pixels.
                    const uint32_t* q = static_cast<const uint32_t*>(popDib.bits);
                    bool blank = true;
                    for (size_t i = 0; i < static_cast<size_t>(bw) * bh && blank; i += 7) blank = (q[i] & 0xFFFFFFu) == 0;
                    if (blank) {
                        HDC sdc = GetDC(nullptr);
                        BitBlt(popDib.dc, 0, 0, bw, bh, sdc, br.left, br.top, SRCCOPY);
                        ReleaseDC(nullptr, sdc);
                    }
                }
                auto pos = popPos.find(b);
                if (pos == popPos.end()) {
                    POINT pp;
                    {
                        std::lock_guard<std::mutex> lock(s->m);
                        pp = s->pendingPos;
                    }
                    pos = popPos.emplace(b, pp).first;
                }
                const int ox = std::max(0, std::min(static_cast<int>(pos->second.x), w - bw));
                const int oy = std::max(0, std::min(static_cast<int>(pos->second.y), hh - bh));
                const uint32_t* ps = static_cast<const uint32_t*>(popDib.bits);
                for (int y = 0; y < bh; ++y) {
                    uint32_t* dst = &canvas[static_cast<size_t>(oy + y) * w + ox];
                    const uint32_t* srow = ps + static_cast<size_t>(y) * bw;
                    for (int x = 0; x < bw; ++x) dst[x] = bgraToRgba(srow[x]);
                }
                shown.push_back({b, ox, oy, bw, bh, br});
            }
            for (auto it = popPos.begin(); it != popPos.end();)
                it = (IsWindow(it->first) && IsWindowVisible(it->first)) ? std::next(it) : popPos.erase(it);
        }

        // Stop the hosted program (and the UIA helper) from stealing the foreground: that would drop the shell's mouse capture.
        wchar_t title[256] = {};
        GetWindowTextW(h, title, 256);
        std::lock_guard<std::mutex> lock(s->m);
        s->popups.swap(shown);
        s->front.swap(canvas);
        s->fw = w;
        s->fh = hh;
        s->fresh = true;
        s->mainRect = r;
        s->popup = pop;
        s->popupRect = pr;
        s->windowTitle = narrow(title);
    }
}

AppWindow::AppWindow(Renderer& renderer, std::string title, std::string exe, void* ownerWindow)
    : renderer_(renderer), title_(std::move(title)), exe_(std::move(exe)), s_(new AppWindowState) {
    s_->owner = static_cast<HWND>(ownerWindow);
    s_->ownPid = GetCurrentProcessId();
    s_->tex = renderer_.createDynamicLabel(kTexW, kTexH);
    s_->bar = renderer_.createDynamicLabel(kBarW, kBarH);
}

AppWindow::~AppWindow() { release(); }

bool AppWindow::running() const { return s_->hwnd.load() != 0 && !s_->exited; }

void AppWindow::release() {
    s_->stop = true;
    if (s_->worker.joinable()) s_->worker.join();
    HWND h = reinterpret_cast<HWND>(s_->hwnd.load());
    if (h) unclaim(h);
    if (h && IsWindow(h)) {
        SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
        SetWindowPos(h, HWND_TOP, 160, 120, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }
    s_->hwnd = 0;
    visible_ = false;
}

void AppWindow::open(Vec3 eye, Vec3 forward, bool vr) {
    Vec3 f{forward.x, 0, forward.z};
    f = length(f) < 0.1f ? Vec3{0, 0, -1} : normalize(f);
    s_->worldW = vr ? 2.2f : 2.6f;
    s_->barH = s_->worldW * kBarRatio;
    const float totalH = s_->worldW * kWinH / kWinW + s_->barH + 0.015f;
    // Desktop: sit at the distance where the window (with its title bar) fills 85% of the screen.
    const float dist = vr ? 1.9f : fitDistance(s_->worldW, totalH, viewAspect_, viewTan_);
    Vec3 c = eye + f * dist;
    c.y = eye.y - (vr ? 0.1f : (s_->barH + 0.015f) * 0.5f);
    s_->base = Mat4::translate(c) * Mat4::rotateY(std::atan2(-f.x, -f.z));
    savedBase_ = s_->base;
    savedW_ = s_->worldW;

    if (s_->exited || s_->failed) {  // the previous instance is gone: start fresh
        if (HWND old = reinterpret_cast<HWND>(s_->hwnd.load())) unclaim(old);
        s_->stop = true;
        if (s_->worker.joinable()) s_->worker.join();
        s_->hwnd = 0;
    }
    if (s_->hwnd.load() == 0 && !s_->worker.joinable()) {
        s_->stop = false;
        s_->exited = false;
        s_->failed = false;
        s_->contentW = s_->contentH = 0;
        s_->shownTitle.clear();
        s_->before = snapshotWindows(s_->ownPid);
        std::wstring exe = widen(exe_);
        std::wstring params;
        if (const size_t bar = exe.find(L'|'); bar != std::wstring::npos) {  // "program|arguments"
            params = exe.substr(bar + 1);
            exe.resize(bar);
        }
        if (params.empty()) {
            // The pinned File Explorer shortcut just re-activates an existing window; force a new one on the home folder.
            std::wstring low = exe;
            for (wchar_t& c : low) c = static_cast<wchar_t>(towlower(c));
            if (low.size() >= 17 && low.compare(low.size() - 17, 17, L"file explorer.lnk") == 0) {
                wchar_t home[MAX_PATH] = L"";
                GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
                exe = L"explorer.exe";
                params = L"/n,\"" + std::wstring(home) + L"\"";
            }
        }
        HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), params.empty() ? nullptr : params.c_str(), nullptr, SW_SHOWNOACTIVATE);
        if (reinterpret_cast<intptr_t>(r) <= 32) {
            s_->status = "Could not launch " + exe_;
            s_->failed = true;
        } else {
            s_->status = "Launching...";
            s_->worker = std::thread(workerMain, s_.get());
        }
    }
    s_->capturing = true;
    visible_ = true;
    focused_ = false;
    barDirty_ = true;
}

void AppWindow::hide() {
    dragging_ = false;
    if (fullscreen_) toggleFullscreen();
    visible_ = false;
    focused_ = false;
    s_->capturing = false;
    if (mouseDown_) {
        sendMouse(WM_LBUTTONUP, 0);
        mouseDown_ = false;
    }
}

void AppWindow::escape() {
    if (fullscreen_) toggleFullscreen();
    else hide();
}

void AppWindow::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        // Match the window to the screen's shape so the program fills it without letterboxing.
        int w = kTexW, h = kTexH;
        if (viewAspect_ >= static_cast<float>(kTexW) / kTexH) h = std::max(480, static_cast<int>(kTexW / viewAspect_));
        else w = std::max(640, static_cast<int>(kTexH * viewAspect_));
        s_->wantW = w;
        s_->wantH = h;
    } else {
        s_->base = savedBase_;
        s_->worldW = savedW_;
        s_->wantW = kWinW;
        s_->wantH = kWinH;
    }
    s_->mouseX = s_->mouseY = -1;
    hover_ = Hit::None;
    barDirty_ = true;
}

// Fullscreen: a view-space panel one unit in front of the eye, as large as fits the screen.
void AppWindow::applyFullscreenLayout() {
    const float viewH = 2.0f * viewTan_, viewW = viewH * viewAspect_;
    const float cw = s_->contentW > 0 ? static_cast<float>(s_->contentW) : static_cast<float>(kWinW);
    const float ch = s_->contentH > 0 ? static_cast<float>(s_->contentH) : static_cast<float>(kWinH);
    s_->worldW = std::min(viewW, viewH * cw / ch);
    s_->worldH = s_->worldW * ch / cw;
    s_->base = Mat4::translate({0, 0, -1.0f});
}

void AppWindow::closeProgram() {
    HWND h = reinterpret_cast<HWND>(s_->hwnd.load());
    if (h) PostMessageW(h, WM_CLOSE, 0, 0);
    else hide();
}

void AppWindow::update(double dt) {
    clock_ += dt;
    if (!visible_) return;
    if (releasePending_) {
        releasePending_ = false;
        releaseButton();
    }
    if (s_->exited) {
        hide();  // the program quit (or was closed from the panel)
        return;
    }
    if (s_->failed && s_->status == "Launching...") {
        s_->status = "The program did not open a window";
        barDirty_ = true;
    }
    std::string title;
    {
        std::lock_guard<std::mutex> lock(s_->m);
        if (s_->fresh && s_->fw > 0) {
            renderer_.updateLabelRect(*s_->tex, 0, 0, s_->fw, s_->fh, reinterpret_cast<const uint8_t*>(s_->front.data()),
                                      static_cast<size_t>(s_->fw) * 4);
            s_->contentW = s_->fw;
            s_->contentH = s_->fh;
            s_->fresh = false;
        }
        title = s_->windowTitle;
    }
    if (title != s_->shownTitle) {
        s_->shownTitle = title;
        barDirty_ = true;
    }
    if (barDirty_) drawBar();
}

void AppWindow::render(Renderer& renderer) {
    if (!visible_) return;
    if (fullscreen_) applyFullscreenLayout();
    if (s_->contentW <= 0 && !fullscreen_) s_->worldH = s_->worldW * kWinH / kWinW;
    if (s_->contentW > 0 && !fullscreen_) s_->worldH = s_->worldW * s_->contentH / s_->contentW;
    const float gap = 0.015f, pad = 0.03f, round = 0.004f;
    if (!fullscreen_) {
        // One glass card behind the title bar and the program.
        const float top = s_->worldH * 0.5f + gap + s_->barH + pad, bottom = -s_->worldH * 0.5f - pad;
        const float fw = s_->worldW + pad * 2, fh = top - bottom;
        renderer.drawGlass(s_->base * Mat4::translate({0, (top + bottom) * 0.5f, -0.004f}) * Mat4::scale({fw, fh, 1}), fw / fh,
                           0.022f / fh);
    }
    if (s_->contentW > 0) {
        const float uv[4] = {0, 0, static_cast<float>(s_->contentW) / kTexW, static_cast<float>(s_->contentH) / kTexH};
        const float white[3] = {1, 1, 1};
        renderer.drawWebRegion(*s_->tex, s_->base * Mat4::scale({s_->worldW, s_->worldH, 1}), uv, s_->worldW / s_->worldH,
                               fullscreen_ ? 0.0f : round / s_->worldH, 0.0f, 1.0f, white);
    }
    if (fullscreen_) return;
    const float y = s_->worldH * 0.5f + s_->barH * 0.5f + gap;
    const float uvAll[4] = {0, 0, 1, 1}, white[3] = {1, 1, 1};
    renderer.drawWebRegion(*s_->bar, s_->base * Mat4::translate({0, y, 0}) * Mat4::scale({s_->worldW, s_->barH, 1}), uvAll,
                           s_->worldW / s_->barH, round / s_->barH, 0.0f, 0.94f, white);
}

bool AppWindow::rayToPlane(Vec3 origin, Vec3 dir, float& hx, float& hy) const {
    Mat4 inv = s_->base.rigidInverse();
    Vec3 o = inv.transformPoint(origin), d = inv.transformDir(dir);
    if (std::fabs(d.z) < 1e-5f) return false;
    float t = -o.z / d.z;
    if (t <= 0) return false;
    hx = o.x + d.x * t;
    hy = o.y + d.y * t;
    return true;
}

float AppWindow::hitDistance(Vec3 origin, Vec3 dir) const {
    if (!visible_) return -1.0f;
    float hx = 0, hy = 0;
    if (!rayToPlane(origin, dir, hx, hy)) return -1.0f;
    // The whole glass card (title bar, gap, program and padding) counts as the window.
    const float pad = fullscreen_ ? 0.0f : 0.03f;
    const float top = s_->worldH * 0.5f + (fullscreen_ ? 0.0f : 0.015f + s_->barH) + pad;
    if (std::fabs(hx) > s_->worldW * 0.5f + pad || hy > top || hy < -s_->worldH * 0.5f - pad) return -1.0f;
    return length(s_->base.transformPoint({hx, hy, 0}) - origin);
}

bool AppWindow::bounds(Vec3& centre, Vec3& normal, float& w, float& h) const {
    if (!visible_ || fullscreen_) return false;
    const float top = s_->worldH * 0.5f + 0.015f + s_->barH, bottom = -s_->worldH * 0.5f;
    centre = s_->base.transformPoint({0, (top + bottom) * 0.5f, 0});
    normal = normalize(s_->base.transformDir({0, 0, 1}));
    w = s_->worldW;
    h = top - bottom;
    return true;
}

float AppWindow::distanceTo(Vec3 eye) const { return length(s_->base.transformPoint({0, 0, 0}) - eye); }

void AppWindow::blur() {
    focused_ = false;
    hover_ = Hit::None;
    barDirty_ = true;
    if (mouseDown_) {
        sendMouse(WM_LBUTTONUP, 0);
        mouseDown_ = false;
    }
    dragging_ = false;
    s_->mouseX = s_->mouseY = -1;
}

AppWindow::Hit AppWindow::barHit(float px, float py) const {
    if (py < 0 || py > kBarH) return Hit::None;
    if (px >= kFullX0 && px <= kFullX1) return Hit::Full;
    if (px >= kHideX0 && px <= kHideX1) return Hit::Hide;
    if (px >= kCloseX0 && px <= kCloseX1) return Hit::Close;
    return Hit::Title;
}

// Posts a mouse message for the current pointer position to the deepest child window under it.
void AppWindow::sendMouse(int msg, int wparam, bool wheelDelta, int delta) {
    HWND top = reinterpret_cast<HWND>(s_->hwnd.load());
    if (!top || s_->mouseX < 0) return;
    RECT mr, pr;
    HWND pop;
    {
        std::lock_guard<std::mutex> lock(s_->m);
        mr = s_->mainRect;
        pr = s_->popupRect;
        pop = s_->popup;
    }
    POINT sp{mr.left + s_->mouseX, mr.top + s_->mouseY};
    HWND target = (pop && PtInRect(&pr, sp)) ? pop : top;
    for (int depth = 0; depth < 12; ++depth) {
        POINT cp = sp;
        ScreenToClient(target, &cp);
        HWND child = ChildWindowFromPointEx(target, cp, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
        if (!child || child == target) break;
        target = child;
    }
    s_->lastTarget = target;
    if (isHostClass(target)) {
        std::lock_guard<std::mutex> lock(s_->m);
        if (!s_->popups.empty()) return;  // a posted message to the chrome would light-dismiss the open flyout
    }
    if (wheelDelta) {
        PostMessageW(target, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<short>(delta)),
                     MAKELPARAM(static_cast<short>(sp.x), static_cast<short>(sp.y)));
        return;
    }
    POINT cp = sp;
    ScreenToClient(target, &cp);
    PostMessageW(target, static_cast<UINT>(msg), static_cast<WPARAM>(wparam),
                 MAKELPARAM(static_cast<short>(cp.x), static_cast<short>(cp.y)));
}

void AppWindow::pointer(Vec3 origin, Vec3 dir) {
    if (!visible_) return;
    if (fullscreen_) applyFullscreenLayout();
    if (dragging_ && !fullscreen_) {
        Vec3 p = origin + dir * grabDist_;
        Vec3 f{p.x - origin.x, 0, p.z - origin.z};
        if (length(f) > 0.05f) {
            f = normalize(f);
            s_->base = Mat4::translate(p) * Mat4::rotateY(std::atan2(-f.x, -f.z)) * Mat4::translate({-grabX_, -grabY_, 0});
            savedBase_ = s_->base;
        }
        return;
    }
    float hx = 0, hy = 0;
    Hit hover = Hit::None;
    if (rayToPlane(origin, dir, hx, hy) && s_->contentW > 0) {
        const float u = hx / s_->worldW, v = hy / s_->worldH;
        if (std::fabs(u) <= 0.5f && std::fabs(v) <= 0.5f) {
            const int px = static_cast<int>(clampf((u + 0.5f) * s_->contentW, 0, static_cast<float>(s_->contentW - 1)));
            const int py = static_cast<int>(clampf((0.5f - v) * s_->contentH, 0, static_cast<float>(s_->contentH - 1)));
            if (px != s_->mouseX || py != s_->mouseY) {
                s_->mouseX = px;
                s_->mouseY = py;
                sendMouse(WM_MOUSEMOVE, mouseDown_ ? MK_LBUTTON : 0);
            }
        } else {
            if (!fullscreen_) {
                const float by = hy - (s_->worldH * 0.5f + s_->barH * 0.5f + 0.015f);
                if (std::fabs(u) <= 0.5f && std::fabs(by) <= s_->barH * 0.5f) {
                    hover = barHit((u + 0.5f) * kBarW, (0.5f - by / s_->barH) * kBarH);
                    hoverX_ = hx;
                    hoverY_ = hy;
                    hoverDist_ = length(s_->base.transformPoint({hx, hy, 0}) - origin);
                }
            }
            if (s_->mouseX >= 0 && !mouseDown_) {
                if (s_->lastTarget && IsWindow(s_->lastTarget)) PostMessageW(s_->lastTarget, WM_MOUSELEAVE, 0, 0);
                s_->mouseX = -1;
            }
        }
    }
    if (hover != hover_) {
        hover_ = hover;
        barDirty_ = true;
    }
}

void AppWindow::press(bool autoRelease) {
    if (!visible_) return;
    if (hover_ == Hit::Full) {
        toggleFullscreen();
        return;
    }
    if (hover_ == Hit::Hide) {
        hide();
        return;
    }
    if (hover_ == Hit::Close) {
        closeProgram();
        return;
    }
    if (hover_ == Hit::Title) {
        if (!autoRelease) {
            dragging_ = true;
            grabX_ = hoverX_;
            grabY_ = hoverY_;
            grabDist_ = clampf(hoverDist_, 0.8f, 8.0f);
        }
        return;
    }
    if (s_->mouseX < 0) {
        focused_ = false;  // clicked outside the window: the keyboard goes back to flying
        return;
    }
    focused_ = true;
    HWND top = reinterpret_cast<HWND>(s_->hwnd.load());
    bool inFlyout = false;
    {
        std::lock_guard<std::mutex> lock(s_->m);
        for (const auto& p : s_->popups)
            if (s_->mouseX >= p.cx && s_->mouseX < p.cx + p.w && s_->mouseY >= p.cy && s_->mouseY < p.cy + p.hgt) inFlyout = true;
        if (top && (inFlyout || (s_->lastTarget && IsWindow(s_->lastTarget) && isHostClass(s_->lastTarget)) || !s_->popups.empty())) {
            s_->clicks.push_back(POINT{s_->mouseX, s_->mouseY});
            return;
        }
    }
    sendMouse(WM_LBUTTONDOWN, MK_LBUTTON);
    mouseDown_ = true;
    releasePending_ = autoRelease;
}

void AppWindow::releaseButton() {
    dragging_ = false;
    if (!mouseDown_) return;
    mouseDown_ = false;
    if (s_->mouseX >= 0) sendMouse(WM_LBUTTONUP, 0);
    else if (s_->lastTarget && IsWindow(s_->lastTarget)) PostMessageW(s_->lastTarget, WM_LBUTTONUP, 0, 0);
}

void AppWindow::scrollNotches(float notches) {
    if (!visible_ || s_->mouseX < 0) return;
    sendMouse(0, 0, true, static_cast<int>(notches * WHEEL_DELTA));
}

namespace {
HWND keyTarget(HWND top, HWND lastTarget) {
    GUITHREADINFO gi{};
    gi.cbSize = sizeof(gi);
    if (GetGUIThreadInfo(GetWindowThreadProcessId(top, nullptr), &gi) && gi.hwndFocus && !isHostClass(gi.hwndFocus))
        return gi.hwndFocus;
    if (lastTarget && IsWindow(lastTarget) && !isHostClass(lastTarget)) return lastTarget;
    if (HWND c = largestControl(top)) return c;
    return top;
}

void postKey(HWND t, UINT vk, bool extended) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM down = 1 | (static_cast<LPARAM>(scan) << 16) | (extended ? (1 << 24) : 0);
    PostMessageW(t, WM_KEYDOWN, vk, down);
    PostMessageW(t, WM_KEYUP, vk, down | static_cast<LPARAM>(0xC0000000));
}
}  // namespace

void AppWindow::key(EditKey k) {
    HWND top = reinterpret_cast<HWND>(s_->hwnd.load());
    if (!top || !focused_) return;
    HWND t = keyTarget(top, s_->lastTarget);
    switch (k) {
        case EditKey::Paste: PostMessageW(t, WM_PASTE, 0, 0); break;
        case EditKey::Copy: PostMessageW(t, WM_COPY, 0, 0); break;
        case EditKey::Cut: PostMessageW(t, WM_CUT, 0, 0); break;
        case EditKey::Undo: PostMessageW(t, WM_UNDO, 0, 0); break;
        case EditKey::SelectAll: PostMessageW(t, EM_SETSEL, 0, -1); break;
        case EditKey::DocStart:
            PostMessageW(t, EM_SETSEL, 0, 0);
            PostMessageW(t, EM_SCROLLCARET, 0, 0);
            break;
        case EditKey::DocEnd:
            PostMessageW(t, EM_SETSEL, 0x7FFFFFFF, 0x7FFFFFFF);
            PostMessageW(t, EM_SCROLLCARET, 0, 0);
            break;
        case EditKey::Backspace:
            postKey(t, VK_BACK, false);
            PostMessageW(t, WM_CHAR, 8, 1);
            break;
        case EditKey::Enter:
            postKey(t, VK_RETURN, false);
            PostMessageW(t, WM_CHAR, 13, 1);
            break;
        case EditKey::Tab:
            postKey(t, VK_TAB, false);
            PostMessageW(t, WM_CHAR, 9, 1);
            break;
        case EditKey::Delete: postKey(t, VK_DELETE, true); break;
        case EditKey::Left: postKey(t, VK_LEFT, true); break;
        case EditKey::Right: postKey(t, VK_RIGHT, true); break;
        case EditKey::Up: postKey(t, VK_UP, true); break;
        case EditKey::Down: postKey(t, VK_DOWN, true); break;
        case EditKey::Home: postKey(t, VK_HOME, true); break;
        case EditKey::End: postKey(t, VK_END, true); break;
        case EditKey::PageUp: postKey(t, VK_PRIOR, true); break;
        case EditKey::PageDown: postKey(t, VK_NEXT, true); break;
        default: break;
    }
}

void AppWindow::text(const std::string& utf8) {
    HWND top = reinterpret_cast<HWND>(s_->hwnd.load());
    if (!top || !focused_) return;
    HWND t = keyTarget(top, s_->lastTarget);
    for (wchar_t ch : widen(utf8)) PostMessageW(t, WM_CHAR, ch, 1);
}

void AppWindow::drawBar() {
    barDirty_ = false;
    Bitmap bm;
    bm.w = kBarW;
    bm.h = kBarH;
    bm.rgba.resize(static_cast<size_t>(kBarW) * kBarH * 4);
    for (size_t i = 0; i < bm.rgba.size(); i += 4) {
        bm.rgba[i] = static_cast<uint8_t>(kBarBg.r * 255);
        bm.rgba[i + 1] = static_cast<uint8_t>(kBarBg.g * 255);
        bm.rgba[i + 2] = static_cast<uint8_t>(kBarBg.b * 255);
        bm.rgba[i + 3] = 255;
    }
    auto c8 = [](Rgb c) {
        return Color8{static_cast<uint8_t>(c.r * 255 + 0.5f), static_cast<uint8_t>(c.g * 255 + 0.5f),
                      static_cast<uint8_t>(c.b * 255 + 0.5f), 255};
    };
    std::string label = title_;
    if (!s_->shownTitle.empty() && s_->shownTitle != title_) label += "   -   " + s_->shownTitle;
    else if (s_->contentW == 0 && !s_->status.empty()) label += "   -   " + s_->status;
    label = ellipsize(label, 30, FontFace::UIBold, 1480);
    drawText(bm, 36, 50, label, 30, FontFace::UIBold, c8(kText));

    const float cy = kBarH * 0.5f;
    if (hover_ == Hit::Full) fillRoundRect(bm, kFullX0, 12, kFullX1, 68, 16, kHot);
    {
        const Rgb fc = hover_ == Hit::Full ? kText : kDim;
        const float fx = (kFullX0 + kFullX1) * 0.5f, a = fullscreen_ ? 9.0f : 16.0f, b = fullscreen_ ? 14.0f : 11.0f;
        strokeLine(bm, fx - a, cy - b, fx + a, cy - b, 4, fc);
        strokeLine(bm, fx - a, cy + b, fx + a, cy + b, 4, fc);
        strokeLine(bm, fx - a, cy - b, fx - a, cy + b, 4, fc);
        strokeLine(bm, fx + a, cy - b, fx + a, cy + b, 4, fc);
    }
    if (hover_ == Hit::Hide) fillRoundRect(bm, kHideX0, 12, kHideX1, 68, 16, kHot);
    strokeLine(bm, kHideX0 + 32, cy + 8, kHideX1 - 32, cy + 8, 4, hover_ == Hit::Hide ? kText : kDim);
    if (hover_ == Hit::Close) fillRoundRect(bm, kCloseX0, 12, kCloseX1, 68, 16, kDanger);
    const Rgb xc = hover_ == Hit::Close ? kText : kDim;
    const float mx = (kCloseX0 + kCloseX1) * 0.5f;
    strokeLine(bm, mx - 14, cy - 14, mx + 14, cy + 14, 4, xc);
    strokeLine(bm, mx - 14, cy + 14, mx + 14, cy - 14, 4, xc);
    renderer_.updateLabel(*s_->bar, bm.rgba.data(), kBarW, kBarH);
}

}  // namespace sf
