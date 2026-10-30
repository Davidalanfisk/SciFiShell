#define NOMINMAX
#include "WebBrowser.h"

#include <windows.h>

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "TextRender.h"
#include "WebDepthCompositor.h"
#include "WebLayerAnalyzer.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/wrapper/cef_helpers.h"

namespace sf {

namespace {
constexpr int kBarW = 1920, kBarH = 80;
// The page is laid out at 1280x720 CSS px but rasterised at 1.5x, so text is 50% larger in the world and stays sharp.
constexpr int kViewW = 1280, kViewH = 720;
const char* kHomeUrl = "https://duckduckgo.com";

bool g_cefReady = false;

// ---- toolbar painting helpers (same approach as the media viewer's bar) --------------------------------------------
struct Rgb {
    float r, g, b;
};
constexpr Rgb kBarBg{0.07f, 0.09f, 0.15f}, kAccent{0.42f, 0.62f, 1.0f}, kText{0.93f, 0.95f, 0.98f},
    kDim{0.55f, 0.60f, 0.72f}, kField{0.12f, 0.15f, 0.24f}, kHot{0.20f, 0.26f, 0.42f};

Color8 toColor8(Rgb c) {
    return Color8{static_cast<uint8_t>(c.r * 255.0f + 0.5f), static_cast<uint8_t>(c.g * 255.0f + 0.5f),
                  static_cast<uint8_t>(c.b * 255.0f + 0.5f), 255};
}

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

// Toolbar regions (pixels in the 1920x80 bar).
struct Region {
    float x0, x1;
};
constexpr Region kBack{14, 70}, kForward{78, 134}, kReload{142, 198}, kAddress{214, 1674}, kFull{1690, 1790}, kClose{1806, 1906};

std::string hostOf(const std::string& url) {
    size_t s = url.find("://");
    s = s == std::string::npos ? 0 : s + 3;
    size_t e = url.find_first_of("/?#:", s);
    std::string h = url.substr(s, e == std::string::npos ? std::string::npos : e - s);
    if (h.compare(0, 4, "www.") == 0) h.erase(0, 4);
    return h;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += static_cast<char>(c);
        else if (c == ' ') o += '+';
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// Turns whatever was typed in the address bar into a URL (or a search).
std::string normalizeInput(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.empty()) return kHomeUrl;
    if (s.find("://") != std::string::npos || s.compare(0, 6, "about:") == 0) return s;
    bool looksLikeHost = s.find(' ') == std::string::npos && (s.find('.') != std::string::npos || s.compare(0, 9, "localhost") == 0);
    if (looksLikeHost) return "https://" + s;
    return "https://duckduckgo.com/?q=" + urlEncode(s);
}

std::u16string utf8ToUtf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        int extra = 0;
        if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
        else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
        else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}
}  // namespace

struct WebState {
    CefRefPtr<CefClient> client;  // a BrowserClient (kept as CefClient because that class is defined below)
    CefRefPtr<CefBrowser> browser;
    bool created = false;

    int vw = kViewW, vh = kViewH;    // CSS size of the page view (fullscreen matches the screen shape)
    int pageW = 1920, pageH = 1080;  // backing resolution = view * scale
    float scale = 1.5f;
    std::vector<uint32_t> pixels;
    bool dirty = false;
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
    bool paintedSince = false;

    std::shared_ptr<GpuLabel> page, toolbar;
    std::string url, title, pendingUrl;
    bool loading = false, canBack = false, canForward = false;

    WebLayerAnalyzer analyzer;
    WebDepthCompositor depth;
    WebLayout emptyLayout;
    double analysisAt = -1, lastAnalysis = -1;
    bool stale = true;
    int inputSeq = 0, requestSeq = 0;

    Mat4 base = Mat4::identity();
    float worldW = 2.4f, worldH = 2.4f * kViewH / kViewW, barH = 2.4f * kBarH / kBarW;
    int mouseX = 0, mouseY = 0;
    float pointerU = 0, pointerV = 0;
    double time = 0;
    std::string status;
};

class BrowserClient : public CefClient,
                      public CefRenderHandler,
                      public CefLifeSpanHandler,
                      public CefLoadHandler,
                      public CefDisplayHandler,
                      public CefContextMenuHandler {
public:
    explicit BrowserClient(WebState* s) : s_(s) {}
    void detach() { s_ = nullptr; }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }

    // Native context menus make no sense in off-screen mode.
    void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>,
                             CefRefPtr<CefMenuModel> model) override {
        model->Clear();
    }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, s_ ? s_->vw : kViewW, s_ ? s_->vh : kViewH); }
    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& info) override {
        info.device_scale_factor = s_ ? s_->scale : 1.5f;
        info.rect = CefRect(0, 0, s_ ? s_->vw : kViewW, s_ ? s_->vh : kViewH);
        info.available_rect = info.rect;
        return true;
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width,
                 int height) override {
        // Drop-down popups are not composited; only the main view is shown.
        if (!s_ || type != PET_VIEW || width != s_->pageW || height != s_->pageH) return;
        const uint32_t* src = static_cast<const uint32_t*>(buffer);
        for (const CefRect& r : dirtyRects) {
            int x0 = std::max(r.x, 0), y0 = std::max(r.y, 0), x1 = std::min(r.x + r.width, s_->pageW),
                y1 = std::min(r.y + r.height, s_->pageH);
            if (x1 <= x0 || y1 <= y0) continue;
            for (int y = y0; y < y1; ++y) {
                const uint32_t* sp = src + static_cast<size_t>(y) * s_->pageW;
                uint32_t* dp = s_->pixels.data() + static_cast<size_t>(y) * s_->pageW;
                for (int x = x0; x < x1; ++x) {
                    uint32_t p = sp[x];  // BGRA -> RGBA, forced opaque
                    dp[x] = (p & 0x0000FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16) | 0xFF000000u;
                }
            }
            if (!s_->dirty) {
                s_->dx0 = x0; s_->dy0 = y0; s_->dx1 = x1; s_->dy1 = y1;
                s_->dirty = true;
            } else {
                s_->dx0 = std::min(s_->dx0, x0); s_->dy0 = std::min(s_->dy0, y0);
                s_->dx1 = std::max(s_->dx1, x1); s_->dy1 = std::max(s_->dy1, y1);
            }
        }
        s_->paintedSince = true;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        if (!s_) return;
        s_->browser = browser;
        if (!s_->pendingUrl.empty()) {
            browser->GetMainFrame()->LoadURL(s_->pendingUrl);
            s_->pendingUrl.clear();
        }
    }
    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        if (s_) s_->browser = nullptr;
    }
    // Pop-ups (target=_blank, window.open) navigate the single page instead of opening a native window.
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int, const CefString& target_url,
                       const CefString&, WindowOpenDisposition, bool, const CefPopupFeatures&, CefWindowInfo&,
                       CefRefPtr<CefClient>&, CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) override {
        if (!target_url.empty()) browser->GetMainFrame()->LoadURL(target_url);
        return true;
    }

    void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool isLoading, bool canGoBack, bool canGoForward) override {
        if (!s_) return;
        s_->loading = isLoading;
        s_->canBack = canGoBack;
        s_->canForward = canGoForward;
    }
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override {
        if (s_ && frame->IsMain()) s_->analysisAt = s_->time + 0.5;
    }
    void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, const CefString& url) override {
        if (!s_ || !frame->IsMain()) return;
        s_->url = url.ToString();
        s_->depth.setHost(hostOf(s_->url));
        s_->analyzer.clear();
        s_->stale = true;
        s_->analysisAt = s_->time + 1.0;
    }
    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
        if (s_) s_->title = title.ToString();
    }
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message, const CefString&, int) override {
        if (!s_) return false;
        const uint64_t before = s_->analyzer.layout().version;
        if (!s_->analyzer.onConsole(message.ToString())) return false;
        if (s_->analyzer.layout().version != before && s_->requestSeq == s_->inputSeq) s_->stale = false;
        return true;  // hide analyzer traffic from the log
    }

private:
    WebState* s_;
    IMPLEMENT_REFCOUNTING(BrowserClient);
};

namespace {
class ShellApp : public CefApp, public CefBrowserProcessHandler {
public:
    void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> cmd) override {
        cmd->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");
        cmd->AppendSwitchWithValue("disable-features", "Translate");
    }
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

private:
    IMPLEMENT_REFCOUNTING(ShellApp);
};
CefRefPtr<ShellApp> g_app;
}  // namespace

// ---- process level -------------------------------------------------------------------------------------------------

int WebBrowser::executeSubprocess() {
    g_app = new ShellApp();
    CefMainArgs args(GetModuleHandle(nullptr));
    return CefExecuteProcess(args, g_app, nullptr);
}

bool WebBrowser::ensureCef() {
    if (g_cefReady) return true;
    if (!g_app) g_app = new ShellApp();
    CefMainArgs args(GetModuleHandle(nullptr));
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_WARNING;
    const char* local = std::getenv("LOCALAPPDATA");
    std::filesystem::path dir = std::filesystem::path(local ? local : ".") / "SciFiShell" / "cef-cache";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    CefString(&settings.root_cache_path) = dir.wstring();
    CefString(&settings.cache_path) = dir.wstring();
    CefString(&settings.log_file) = (dir / "cef.log").wstring();
    g_cefReady = CefInitialize(args, settings, g_app, nullptr);
    return g_cefReady;
}

void WebBrowser::pump() {
    if (g_cefReady) CefDoMessageLoopWork();
}

// ---- panel ---------------------------------------------------------------------------------------------------------

WebBrowser::WebBrowser(Renderer& renderer) : renderer_(renderer), s_(new WebState) {
    s_->scale = scaleForQuality(0);
    s_->page = renderer_.createDynamicLabel(s_->pageW, s_->pageH);
    s_->toolbar = renderer_.createDynamicLabel(kBarW, kBarH);
    s_->pixels.assign(static_cast<size_t>(s_->pageW) * s_->pageH, 0xFF1C140E);
    renderer_.updateLabel(*s_->page, reinterpret_cast<const uint8_t*>(s_->pixels.data()), s_->pageW, s_->pageH);
}

float WebBrowser::scaleForQuality(int q) {
    static const float kScales[kQualityCount] = {1.5f, 2.0f, 3.0f, 4.0f};
    return kScales[std::clamp(q, 0, kQualityCount - 1)];
}

static void resizeView(Renderer& renderer, WebState& s, int w, int h) {
    if (s.vw == w && s.vh == h) return;
    s.vw = w;
    s.vh = h;
    s.pageW = static_cast<int>(w * s.scale);
    s.pageH = static_cast<int>(h * s.scale);
    s.pixels.assign(static_cast<size_t>(s.pageW) * s.pageH, 0xFFFFFFFF);
    s.dirty = false;
    s.page = renderer.createDynamicLabel(s.pageW, s.pageH);
    renderer.updateLabel(*s.page, reinterpret_cast<const uint8_t*>(s.pixels.data()), s.pageW, s.pageH);
    if (s.browser) {
        auto host = s.browser->GetHost();
        host->NotifyScreenInfoChanged();
        host->WasResized();
        host->Invalidate(PET_VIEW);
    }
}

void WebBrowser::setQuality(int q) {    q = std::clamp(q, 0, kQualityCount - 1);
    const float sc = scaleForQuality(q);
    if (sc == s_->scale && !s_->pixels.empty()) return;
    s_->scale = sc;
    s_->pageW = static_cast<int>(s_->vw * sc);
    s_->pageH = static_cast<int>(s_->vh * sc);
    s_->pixels.assign(static_cast<size_t>(s_->pageW) * s_->pageH, 0xFFFFFFFF);
    s_->dirty = false;
    s_->page = renderer_.createDynamicLabel(s_->pageW, s_->pageH);
    renderer_.updateLabel(*s_->page, reinterpret_cast<const uint8_t*>(s_->pixels.data()), s_->pageW, s_->pageH);
    if (s_->browser) {
        auto host = s_->browser->GetHost();
        host->NotifyScreenInfoChanged();
        host->WasResized();
        host->Invalidate(PET_VIEW);
    }
}

std::string WebBrowser::qualityText(int q) {
    const float sc = scaleForQuality(q);
    return std::to_string(static_cast<int>(kViewW * sc)) + " x " + std::to_string(static_cast<int>(kViewH * sc));
}

WebBrowser::~WebBrowser() {
    if (s_->client) static_cast<BrowserClient*>(s_->client.get())->detach();
    if (s_->browser) {
        s_->browser->GetHost()->CloseBrowser(true);
        for (int i = 0; i < 20 && g_cefReady; ++i) {  // let the close complete; CefShutdown is skipped on purpose
            CefDoMessageLoopWork();
            SDL_Delay(5);
        }
    }
}

bool WebBrowser::started() const { return s_ && s_->created; }

void WebBrowser::open(const std::string& url, Vec3 eye, Vec3 forward, bool vr) {
    if (!ensureCef()) {
        s_->status = "Browser failed to start";
        return;
    }
    Vec3 f{forward.x, 0, forward.z};
    f = length(f) < 0.1f ? Vec3{0, 0, -1} : normalize(f);
    fullscreen_ = false;
    resizeView(renderer_, *s_, kViewW, kViewH);
    s_->depth.setMode(DepthMode::Off);
    s_->worldW = vr ? 2.0f : 2.4f;
    s_->worldH = s_->worldW * s_->vh / s_->vw;
    s_->barH = s_->worldW * kBarH / kBarW;
    const float totalH = s_->worldH + s_->barH + 0.015f;
    // Desktop: sit at the distance where the page (with its toolbar) fills 85% of the screen.
    const float dist = vr ? 1.8f : fitDistance(s_->worldW, totalH, viewAspect_, viewTan_);
    Vec3 c = eye + f * dist;
    c.y = eye.y - (vr ? 0.1f : (s_->barH + 0.015f) * 0.5f);
    s_->base = Mat4::translate(c) * Mat4::rotateY(std::atan2(-f.x, -f.z));

    if (!s_->created) {
        s_->created = true;
        s_->client = new BrowserClient(s_.get());
        CefWindowInfo info;
        info.SetAsWindowless(nullptr);
        CefBrowserSettings bs;
        bs.windowless_frame_rate = 60;
        bs.background_color = 0xFFFFFFFF;
        s_->pendingUrl = url.empty() ? kHomeUrl : normalizeInput(url);
        CefBrowserHost::CreateBrowser(info, s_->client, s_->pendingUrl, bs, nullptr, nullptr);
    } else if (!url.empty()) {
        navigate(url);
    }
    if (s_->browser) {
        auto host = s_->browser->GetHost();
        host->WasHidden(false);
        host->SetAudioMuted(false);
        host->SetFocus(true);
        host->Invalidate(PET_VIEW);
    }
    visible_ = true;
    addressFocused_ = false;
    toolbarDirty_ = true;
    s_->stale = true;
    s_->analysisAt = s_->time + 0.3;
}

void WebBrowser::close() {
    if (!visible_) return;
    if (fullscreen_) toggleFullscreen();
    dragging_ = false;
    if (mouseDown_) sendMouse(false, true, s_->mouseX, s_->mouseY);
    releasePending_ = false;
    if (s_->browser) {
        auto host = s_->browser->GetHost();
        host->SetAudioMuted(true);
        host->SetFocus(false);
        host->WasHidden(true);
    }
    visible_ = false;
    addressFocused_ = false;
}

void WebBrowser::scheduleAnalysis(double delay) {
    double at = s_->time + delay;
    if (s_->analysisAt < s_->time || at < s_->analysisAt) s_->analysisAt = at;
}

void WebBrowser::markInput() {
    ++s_->inputSeq;
    s_->stale = true;  // page may have scrolled/changed: hide extra layers until re-analysed
    scheduleAnalysis(0.45);
}

void WebBrowser::update(double dt, Vec3, bool vr) {
    if (!visible_) return;
    clock_ += dt;
    s_->time = clock_;
    if (fullscreen_) applyFullscreen();

    if (releasePending_) {
        releasePending_ = false;
        release();
    }

    if (s_->dirty) {
        const int x = s_->dx0, y = s_->dy0, w = s_->dx1 - s_->dx0, h = s_->dy1 - s_->dy0;
        renderer_.updateLabelRect(*s_->page, x, y, w, h,
                                  reinterpret_cast<const uint8_t*>(s_->pixels.data() + static_cast<size_t>(y) * s_->pageW + x),
                                  static_cast<size_t>(s_->pageW) * 4);
        s_->dirty = false;
    }

    // Layout analysis: on load/navigation, after input settles, and as a slow refresh while the page keeps painting.
    if (s_->browser && s_->analysisAt >= 0 && clock_ >= s_->analysisAt && clock_ - s_->lastAnalysis > 0.4) {
        s_->requestSeq = s_->inputSeq;
        s_->analyzer.request(s_->browser->GetMainFrame(), s_->vw, s_->vh);
        s_->lastAnalysis = clock_;
        s_->analysisAt = -1;
        s_->paintedSince = false;
    } else if (s_->analysisAt < 0 && s_->paintedSince && clock_ - s_->lastAnalysis > 4.0) {
        scheduleAnalysis(0.0);
    }

    s_->depth.update(dt, s_->analyzer.layout(), s_->pointerU, s_->pointerV, vr, static_cast<float>(dt * 1000.0), !s_->stale);

    if (toolbarDirty_ || addressFocused_ || s_->loading) drawToolbar();
}

bool WebBrowser::wantsKeyboard() const { return visible_ && (addressFocused_ || s_->analyzer.editableFocused()); }

void WebBrowser::render(Renderer& renderer) {
    if (!visible_) return;
    const WebLayout& layout = s_->analyzer.layout();
    const float gap = 0.015f, pad = 0.03f;
    if (!fullscreen_) {
        // One glass card behind the toolbar (title bar) and the page.
        const float top = s_->worldH * 0.5f + gap + s_->barH + pad, bottom = -s_->worldH * 0.5f - pad;
        const float fw = s_->worldW + pad * 2, fh = top - bottom;
        renderer.drawGlass(s_->base * Mat4::translate({0, (top + bottom) * 0.5f, -0.012f}) * Mat4::scale({fw, fh, 1}), fw / fh,
                           0.022f / fh);
    }
    s_->depth.render(renderer, *s_->page, s_->base, s_->worldW, s_->worldH, s_->vw, s_->vh, layout, clock_);
    const float y = s_->worldH * 0.5f + s_->barH * 0.5f + gap;
    const float uvAll[4] = {0, 0, 1, 1}, white[3] = {1, 1, 1};
    renderer.drawWebRegion(*s_->toolbar, s_->base * Mat4::translate({0, y, 0}) * Mat4::scale({s_->worldW, s_->barH, 1}), uvAll,
                           s_->worldW / s_->barH, 0.004f / s_->barH, 0.0f, 0.94f, white);
}

bool WebBrowser::bounds(Vec3& centre, Vec3& normal, float& w, float& h) const {
    if (!visible_ || fullscreen_) return false;
    const float top = s_->worldH * 0.5f + 0.015f + s_->barH, bottom = -s_->worldH * 0.5f;
    centre = s_->base.transformPoint({0, (top + bottom) * 0.5f, 0});
    normal = normalize(s_->base.transformDir({0, 0, 1}));
    w = s_->worldW;
    h = top - bottom;
    return true;
}

bool WebBrowser::rayToPlane(Vec3 origin, Vec3 dir, float& hx, float& hy) const {
    Mat4 inv = s_->base.rigidInverse();
    Vec3 o = inv.transformPoint(origin), d = inv.transformDir(dir);
    if (std::fabs(d.z) < 1e-5f) return false;
    float t = -o.z / d.z;
    if (t <= 0) return false;
    hx = o.x + d.x * t;
    hy = o.y + d.y * t;
    return true;
}

float WebBrowser::hitDistance(Vec3 origin, Vec3 dir) const {
    if (!visible_) return -1.0f;
    float hx = 0, hy = 0;
    if (!rayToPlane(origin, dir, hx, hy)) return -1.0f;
    const float pad = 0.03f;
    const float top = s_->worldH * 0.5f + 0.015f + s_->barH + pad;
    if (std::fabs(hx) > s_->worldW * 0.5f + pad || hy > top || hy < -s_->worldH * 0.5f - pad) return -1.0f;
    return length(s_->base.transformPoint({hx, hy, 0}) - origin);
}

WebBrowser::Hit WebBrowser::toolbarHit(float px, float py) const {
    if (py < 0 || py > kBarH) return Hit::None;
    if (py < 12 || py > 68) return Hit::Grip;
    auto in = [&](Region r) { return px >= r.x0 && px <= r.x1; };
    if (in(kBack)) return Hit::Back;
    if (in(kForward)) return Hit::Forward;
    if (in(kReload)) return Hit::Reload;
    if (in(kAddress)) return Hit::Address;
    if (in(kFull)) return Hit::Full;
    if (in(kClose)) return Hit::Close;
    return Hit::Grip;
}

void WebBrowser::sendMouse(bool move, bool up, int px, int py) {
    if (!s_->browser) return;
    CefMouseEvent ev;
    ev.x = px;
    ev.y = py;
    ev.modifiers = mouseDown_ && move ? EVENTFLAG_LEFT_MOUSE_BUTTON : 0;
    auto host = s_->browser->GetHost();
    if (move) {
        host->SendMouseMoveEvent(ev, false);
    } else {
        host->SendMouseClickEvent(ev, MBT_LEFT, up, 1);
        mouseDown_ = !up;
    }
}

void WebBrowser::pointer(Vec3 origin, Vec3 dir) {
    if (!visible_) return;
    if (dragging_) {
        Vec3 p = origin + dir * grabDist_;
        Vec3 f{p.x - origin.x, 0, p.z - origin.z};
        if (length(f) > 0.05f) {
            f = normalize(f);
            s_->base = Mat4::translate(p) * Mat4::rotateY(std::atan2(-f.x, -f.z)) * Mat4::translate({-grabX_, -grabY_, 0});
        }
        return;
    }
    float hx = 0, hy = 0;
    Hit hover = Hit::None;
    if (rayToPlane(origin, dir, hx, hy)) {
        const float u = hx / s_->worldW, v = hy / s_->worldH;
        s_->pointerU = clampf(u, -0.5f, 0.5f);
        s_->pointerV = clampf(v, -0.5f, 0.5f);
        if (std::fabs(u) <= 0.5f && std::fabs(v) <= 0.5f) {
            int px = clampf((u + 0.5f) * s_->vw, 0, s_->vw - 1), py = clampf((0.5f - v) * s_->vh, 0, s_->vh - 1);
            // A raised layer in front of the page: hit-test what the user actually sees, not the plane behind it.
            const Mat4 inv = s_->base.rigidInverse();
            float vx = 0, vy = 0;
            if (s_->depth.pick(inv.transformPoint(origin), inv.transformDir(dir), vx, vy)) {
                px = clampf(vx, 0, s_->vw - 1);
                py = clampf(vy, 0, s_->vh - 1);
            }
            if (px != s_->mouseX || py != s_->mouseY) {
                s_->mouseX = px;
                s_->mouseY = py;
                sendMouse(true, false, px, py);
            }
        } else {
            const float by = hy - (s_->worldH * 0.5f + s_->barH * 0.5f + 0.015f);
            if (std::fabs(u) <= 0.5f && std::fabs(by) <= s_->barH * 0.5f) {
                hover = toolbarHit((u + 0.5f) * kBarW, (0.5f - by / s_->barH) * kBarH);
                hoverX_ = hx;
                hoverY_ = hy;
                hoverDist_ = length(s_->base.transformPoint({hx, hy, 0}) - origin);
            }
            s_->mouseX = -1;  // pointer left the page: the next re-entry always sends a move
        }
    }
    if (hover != hover_) {
        hover_ = hover;
        toolbarDirty_ = true;
    }
}

void WebBrowser::press(bool autoRelease) {
    if (!visible_) return;
    if (hover_ != Hit::None) {
        switch (hover_) {
            case Hit::Back: goBack(); break;
            case Hit::Forward: goForward(); break;
            case Hit::Reload: reload(); break;
            case Hit::Address: focusAddress(); break;
            case Hit::Full: toggleFullscreen(); break;
            case Hit::Close: close(); break;
            case Hit::Grip:
                if (!autoRelease && !fullscreen_) {
                    dragging_ = true;
                    grabX_ = hoverX_;
                    grabY_ = hoverY_;
                    grabDist_ = clampf(hoverDist_, 0.8f, 8.0f);
                }
                break;
            default: break;
        }
        return;
    }
    if (s_->mouseX < 0) return;  // not over the page
    if (addressFocused_) {
        addressFocused_ = false;
        address_ = s_->url;
        toolbarDirty_ = true;
    }
    if (s_->browser) s_->browser->GetHost()->SetFocus(true);
    sendMouse(false, false, s_->mouseX, s_->mouseY);
    // Keep the layers up for a click so the pointer doesn't jump between layer and base-plane coordinates mid-click.
    ++s_->inputSeq;
    scheduleAnalysis(0.6);
    releasePending_ = autoRelease;
}

void WebBrowser::release() {
    dragging_ = false;
    if (mouseDown_) sendMouse(false, true, s_->mouseX < 0 ? 0 : s_->mouseX, s_->mouseY);
}

void WebBrowser::scrollNotches(float notches) { scrollPixels(-notches * 120.0f); }

void WebBrowser::scrollPixels(float pixels) {
    if (!visible_ || !s_->browser || s_->mouseX < 0 || pixels == 0.0f) return;
    CefMouseEvent ev;
    ev.x = s_->mouseX;
    ev.y = s_->mouseY;
    ev.modifiers = 0;
    s_->browser->GetHost()->SendMouseWheelEvent(ev, 0, static_cast<int>(-pixels));
    markInput();
}

void WebBrowser::key(EditKey k) {
    if (!visible_) return;
    if (addressFocused_) {
        switch (k) {
            case EditKey::Backspace:
                if (addressFresh_) address_.clear();
                while (!address_.empty() && (static_cast<unsigned char>(address_.back()) & 0xC0) == 0x80) address_.pop_back();
                if (!address_.empty()) address_.pop_back();
                break;
            case EditKey::Enter: navigate(address_); break;
            case EditKey::Paste:
                if (char* clip = SDL_GetClipboardText()) {
                    if (addressFresh_) address_.clear();
                    address_ += clip;
                    SDL_free(clip);
                }
                break;
            default: return;
        }
        addressFresh_ = false;
        toolbarDirty_ = true;
        return;
    }
    if (!s_->browser) return;
    int vk = 0, mods = 0;
    switch (k) {
        case EditKey::Backspace: vk = VK_BACK; break;
        case EditKey::Delete: vk = VK_DELETE; break;
        case EditKey::Enter: vk = VK_RETURN; break;
        case EditKey::Tab: vk = VK_TAB; break;
        case EditKey::Left: vk = VK_LEFT; break;
        case EditKey::Right: vk = VK_RIGHT; break;
        case EditKey::Up: vk = VK_UP; break;
        case EditKey::Down: vk = VK_DOWN; break;
        case EditKey::Home: vk = VK_HOME; break;
        case EditKey::End: vk = VK_END; break;
        case EditKey::PageUp: vk = VK_PRIOR; break;
        case EditKey::PageDown: vk = VK_NEXT; break;
        case EditKey::DocStart: vk = VK_HOME; mods = EVENTFLAG_CONTROL_DOWN; break;
        case EditKey::DocEnd: vk = VK_END; mods = EVENTFLAG_CONTROL_DOWN; break;
        case EditKey::Paste:
            if (auto frame = s_->browser->GetFocusedFrame()) frame->Paste();
            markInput();
            return;
        default: return;  // Save / Edit have no browser meaning ("e" arrives as text)
    }
    CefKeyEvent ev;
    ev.windows_key_code = vk;
    ev.native_key_code = static_cast<int>((MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16) | 1);
    ev.modifiers = mods;
    ev.is_system_key = false;
    ev.focus_on_editable_field = false;
    auto host = s_->browser->GetHost();
    ev.type = KEYEVENT_RAWKEYDOWN;
    host->SendKeyEvent(ev);
    if (vk == VK_RETURN) {
        ev.type = KEYEVENT_CHAR;
        ev.character = ev.unmodified_character = '\r';
        host->SendKeyEvent(ev);
    }
    ev.type = KEYEVENT_KEYUP;
    ev.character = ev.unmodified_character = 0;
    host->SendKeyEvent(ev);
    markInput();
}

void WebBrowser::text(const std::string& utf8) {
    if (!visible_ || utf8.empty()) return;
    if (addressFocused_) {
        if (addressFresh_) {
            address_.clear();
            addressFresh_ = false;
        }
        address_ += utf8;
        toolbarDirty_ = true;
        return;
    }
    if (!s_->browser) return;
    auto host = s_->browser->GetHost();
    for (char16_t ch : utf8ToUtf16(utf8)) {
        CefKeyEvent ev;
        ev.type = KEYEVENT_CHAR;
        ev.windows_key_code = ch;
        ev.native_key_code = 0;
        ev.modifiers = 0;
        ev.character = ev.unmodified_character = ch;
        ev.is_system_key = false;
        ev.focus_on_editable_field = false;
        host->SendKeyEvent(ev);
    }
    markInput();
}

void WebBrowser::escape() {
    if (addressFocused_) {
        addressFocused_ = false;
        address_ = s_->url;
        toolbarDirty_ = true;
    } else if (fullscreen_) {
        toggleFullscreen();
    } else {
        close();
    }
}

void WebBrowser::applyFullscreen() {
    const float gap = 0.015f, viewH = 2.0f * viewTan_, viewW = viewH * viewAspect_;
    s_->worldW = viewW;
    s_->barH = s_->worldW * kBarH / kBarW;
    s_->worldH = viewH - gap - s_->barH;
    const int vh = kViewH, vw = std::max(640, static_cast<int>(vh * s_->worldW / s_->worldH + 0.5f));
    resizeView(renderer_, *s_, vw, vh);
    s_->base = Mat4::translate({0, -(s_->barH + gap) * 0.5f, -1.0f});
}
void WebBrowser::toggleFullscreen() {
    if (!visible_) return;
    dragging_ = false;
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        savedBase_ = s_->base;
        savedWorldW_ = s_->worldW;
        applyFullscreen();
    } else {
        resizeView(renderer_, *s_, kViewW, kViewH);
        s_->base = savedBase_;
        s_->worldW = savedWorldW_;
        s_->worldH = s_->worldW * s_->vh / s_->vw;
        s_->barH = s_->worldW * kBarH / kBarW;
    }
    toolbarDirty_ = true;
}

void WebBrowser::focusAddress() {
    addressFocused_ = true;
    addressFresh_ = true;  // first keystroke replaces the whole address
    address_ = s_->url;
    toolbarDirty_ = true;
}

void WebBrowser::navigate(const std::string& input) {
    std::string url = normalizeInput(input);
    addressFocused_ = false;
    toolbarDirty_ = true;
    s_->stale = true;
    if (s_->browser) s_->browser->GetMainFrame()->LoadURL(url);
    else s_->pendingUrl = url;
    s_->url = url;
    address_ = url;
}

void WebBrowser::goBack() {
    if (s_->browser && s_->canBack) {
        s_->browser->GoBack();
        markInput();
    }
}
void WebBrowser::goForward() {
    if (s_->browser && s_->canForward) {
        s_->browser->GoForward();
        markInput();
    }
}
void WebBrowser::reload() {
    if (s_->browser) {
        s_->browser->Reload();
        markInput();
    }
}
void WebBrowser::cycleDepth() {}
void WebBrowser::toggleSite() {}
std::string WebBrowser::statusText() const { return s_->status; }

void WebBrowser::drawToolbar() {
    toolbarDirty_ = false;
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
    auto button = [&](Region r, Hit h, bool enabled) {
        if (hover_ == h && enabled) fillRoundRect(bm, r.x0, 12, r.x1, 68, 16, kHot);
        return enabled ? kText : kDim;
    };
    const float cy = kBarH * 0.5f;

    Rgb c = button(kBack, Hit::Back, s_->canBack);
    strokeLine(bm, 52, cy - 14, 34, cy, 4, c);
    strokeLine(bm, 34, cy, 52, cy + 14, 4, c);
    c = button(kForward, Hit::Forward, s_->canForward);
    strokeLine(bm, 96, cy - 14, 114, cy, 4, c);
    strokeLine(bm, 114, cy, 96, cy + 14, 4, c);
    c = button(kReload, Hit::Reload, true);
    for (int i = 0; i < 16; ++i) {  // open circle with an arrow head
        float a0 = 0.6f + i * 0.34f, a1 = a0 + 0.34f;
        strokeLine(bm, 170 + 13 * std::cos(a0), cy + 13 * std::sin(a0), 170 + 13 * std::cos(a1), cy + 13 * std::sin(a1), 3.5f, c);
    }
    strokeLine(bm, 183, cy - 15, 183, cy - 4, 3.5f, c);
    strokeLine(bm, 183, cy - 4, 172, cy - 4, 3.5f, c);

    fillRoundRect(bm, kAddress.x0, 12, kAddress.x1, 68, 28, addressFocused_ ? kHot : kField);
    if (addressFocused_) fillRoundRect(bm, kAddress.x0, 64, kAddress.x1, 67, 1.5f, kAccent);
    std::string shown = addressFocused_ ? address_ : (s_->url.empty() ? std::string("about:blank") : s_->url);
    if (!addressFocused_ && !s_->title.empty()) shown = s_->title + "   -   " + shown;
    const float maxW = kAddress.x1 - kAddress.x0 - 56;
    if (addressFocused_) {
        std::string clipped = shown;
        while (measureText(clipped, 28, FontFace::UI) > maxW && !clipped.empty()) clipped.erase(0, 1);  // keep the caret end visible
        drawText(bm, kAddress.x0 + 28, 47, clipped, 28, FontFace::UI, toColor8(kText));
        if (static_cast<int>(clock_ * 2) % 2 == 0) {
            float cx = kAddress.x0 + 28 + measureText(clipped, 28, FontFace::UI) + 2;
            fillRoundRect(bm, cx, 22, cx + 3, 58, 1, kAccent);
        }
    } else {
        drawText(bm, kAddress.x0 + 28, 47, ellipsize(shown, 28, FontFace::UI, maxW), 28, FontFace::UI, toColor8(kText));
    }

    if (hover_ == Hit::Full) fillRoundRect(bm, kFull.x0, 12, kFull.x1, 68, 16, kHot);
    {
        const Rgb fc = hover_ == Hit::Full ? kText : kDim;
        const float fx = (kFull.x0 + kFull.x1) * 0.5f, a = fullscreen_ ? 9.0f : 16.0f, b = fullscreen_ ? 14.0f : 11.0f;
        strokeLine(bm, fx - a, cy - b, fx + a, cy - b, 4, fc);
        strokeLine(bm, fx - a, cy + b, fx + a, cy + b, 4, fc);
        strokeLine(bm, fx - a, cy - b, fx - a, cy + b, 4, fc);
        strokeLine(bm, fx + a, cy - b, fx + a, cy + b, 4, fc);
    }
    if (hover_ == Hit::Close) fillRoundRect(bm, kClose.x0, 12, kClose.x1, 68, 16, Rgb{0.55f, 0.18f, 0.22f});
    {
        const Rgb xc = hover_ == Hit::Close ? kText : kDim;
        const float mx = (kClose.x0 + kClose.x1) * 0.5f;
        strokeLine(bm, mx - 14, cy - 14, mx + 14, cy + 14, 4, xc);
        strokeLine(bm, mx - 14, cy + 14, mx + 14, cy - 14, 4, xc);
    }
    if (s_->loading) fillRoundRect(bm, 0, kBarH - 4, kBarW * (0.3f + 0.4f * std::fabs(std::sin(static_cast<float>(clock_)))), kBarH, 0, kAccent);
    renderer_.updateLabel(*s_->toolbar, bm.rgba.data(), bm.w, bm.h);
}

}  // namespace sf
