#include "StartMenu.h"

#include <algorithm>
#include <cctype>

#include "TextRender.h"

namespace sf {
using namespace ui;

namespace {
struct ToolDef {
    const char* name;
    Tool tool;
    char glyph;
};
const ToolDef kTools[] = {
    {"Web", Tool::Browser, 'W'},          {"Media", Tool::Media, 'M'},        {"System Monitor", Tool::Monitor, 'S'},
    {"File Browser", Tool::FileBrowser, 'F'}, {"Editor", Tool::Editor, 'E'}, {"Terminal", Tool::Terminal, '>'},
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Draws an icon scaled to size x size with a small box filter so downscaled icons stay smooth.
void blitIcon(Bitmap& dst, const Bitmap& src, int x0, int y0, int size) {
    if (src.w <= 0 || src.h <= 0) return;
    const float step = static_cast<float>(src.w) / size;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx) {
                    const int px = std::min(src.w - 1, static_cast<int>((x + (sx + 0.5f) / 3) * step));
                    const int py = std::min(src.h - 1, static_cast<int>((y + (sy + 0.5f) / 3) * (static_cast<float>(src.h) / size)));
                    const uint8_t* p = &src.rgba[(static_cast<size_t>(py) * src.w + px) * 4];
                    const float a = p[3] / 255.0f;
                    acc[0] += p[0] * a; acc[1] += p[1] * a; acc[2] += p[2] * a; acc[3] += a;
                    ++n;
                }
            const float a = acc[3] / n;
            if (a <= 0.001f) continue;
            const int dx = x0 + x, dy = y0 + y;
            if (dx < 0 || dy < 0 || dx >= dst.w || dy >= dst.h) continue;
            uint8_t* d = &dst.rgba[(static_cast<size_t>(dy) * dst.w + dx) * 4];
            for (int k = 0; k < 3; ++k) {
                const float c = acc[k] / n / a;
                d[k] = static_cast<uint8_t>(c * a + d[k] * (1 - a));
            }
            d[3] = static_cast<uint8_t>(std::max<float>(d[3], a * 255.0f));
        }
}
}  // namespace

StartMenu::StartMenu(Renderer& renderer, StartHost host) : PanelApp(renderer, 1280, 800, 1.5f), host_(std::move(host)) {
    title_ = "START";
}

void StartMenu::setApps(std::vector<std::string> names, std::vector<Bitmap> icons) {
    appNames_ = std::move(names);
    appIcons_ = std::move(icons);
    appsLoaded_ = true;
    invalidate();
}

void StartMenu::onOpen() {
    query_.clear();
    searchFocus_ = false;
    powerOpen_ = false;
    armed_ = 0;
    hover_ = -1;
}

bool StartMenu::back() {
    close();
    return true;
}

std::vector<StartMenu::Result> StartMenu::results() const {
    std::vector<Result> r;
    const std::string q = lower(query_);
    for (const ToolDef& t : kTools)
        if (lower(t.name).find(q) != std::string::npos) r.push_back({t.name, kTool, static_cast<int>(t.tool)});
    for (size_t i = 0; i < appNames_.size(); ++i)
        if (lower(appNames_[i]).find(q) != std::string::npos) r.push_back({appNames_[i], kApp, static_cast<int>(i)});
    return r;
}

int StartMenu::hitAt(float x, float y) const {
    for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i)
        if (hits_[i].r.contains(x, y)) return i;
    return -1;
}

bool StartMenu::onHover(float x, float y) {
    const int h = x < 0 ? -1 : hitAt(x, y);
    if (h == hover_) return false;
    hover_ = h;
    return true;
}

void StartMenu::onClick(float x, float y, bool) {
    const int h = hitAt(x, y);
    if (h < 0) {
        searchFocus_ = false;
        return;
    }
    const Hit hit = hits_[h];
    if (hit.type != kSearch) searchFocus_ = false;
    activate(hit);
}

void StartMenu::activate(const Hit& h) {
    switch (h.type) {
        case kTool: close(); if (host_.tool) host_.tool(static_cast<Tool>(h.arg)); break;
        case kApp: close(); if (host_.app) host_.app(h.arg); break;
        case kRow: close(); if (host_.focusRow) host_.focusRow(h.arg); break;
        case kAct: doAct(h.arg); break;
        case kSearch: searchFocus_ = true; break;
        case kPowerToggle: powerOpen_ = !powerOpen_; armed_ = 0; break;
        default: break;
    }
    invalidate();
}

void StartMenu::doAct(int a) {
    auto run = [&](const char* what) {
        close();
        if (host_.action) host_.action(what);
    };
    switch (a) {
        case kArc: run("arc"); break;
        case kTile: run("tile"); break;
        case kMin: run("min"); break;
        case kSettings: close(); if (host_.tool) host_.tool(Tool::Settings); break;
        case kTheme: if (host_.theme) host_.theme(); break;
        case kExit: run("exit"); break;
        case kRestart: if (armed_ == 1) run("restart"); else armed_ = 1; break;
        case kShut: if (armed_ == 2) run("shutdown"); else armed_ = 2; break;
        case kLock: run("lock"); break;
        case kSleep: run("sleep"); break;
        case kClear: query_.clear(); break;
        default: break;
    }
}

void StartMenu::onKey(EditKey k) {
    if (!searchFocus_) return;
    if (k == EditKey::Backspace) {
        while (!query_.empty() && (static_cast<unsigned char>(query_.back()) & 0xC0) == 0x80) query_.pop_back();
        if (!query_.empty()) query_.pop_back();
    } else if (k == EditKey::Enter) {
        const std::vector<Result> r = results();
        if (!r.empty() && !query_.empty()) activate({{}, r[0].type, r[0].arg});
    }
}

void StartMenu::onText(const std::string& s) {
    for (char c : s)
        if (static_cast<unsigned char>(c) >= 32 && c != 127) query_ += c;
}

void StartMenu::onCtrl(const std::string& s) {
    if (s == "esc") {
        query_.clear();
        searchFocus_ = false;
    }
}

void StartMenu::paint(Bitmap& bm) {
    const Tokens t = tokens();
    hits_.clear();
    rows_ = host_.rows ? host_.rows() : std::vector<StartHost::Row>{};
    const Rect c = panel(bm, "START", "");
    const float pad = 20, railW = 200, nowW = 310;
    const Rect rail{c.x0 + pad, c.y0 + pad, c.x0 + pad + railW, c.y1 - pad};
    const Rect now{c.x1 - pad - nowW, c.y0 + pad, c.x1 - pad, c.y1 - pad};
    const Rect mid{rail.x1 + pad, c.y0 + pad, now.x0 - pad, c.y1 - pad};

    auto add = [&](const Rect& r, int type, int arg) {
        hits_.push_back({r, type, arg});
        return static_cast<int>(hits_.size()) - 1 == hover_ ? State::Hover : State::Normal;
    };

    // Rail: settings and theme at the top, power (with its popover) at the bottom.
    float y = rail.y0;
    const float bh = 58;
    {
        Rect r{rail.x0, y, rail.x1, y + bh};
        button(bm, r, "Settings", add(r, kAct, kSettings), false, 26);
        y += bh + 10;
        r = {rail.x0, y, rail.x1, y + bh};
        button(bm, r, "Theme", add(r, kAct, kTheme), false, 26);
    }
    Rect pr{rail.x0, rail.y1 - bh, rail.x1, rail.y1};
    button(bm, pr, "Power", powerOpen_ ? State::Selected : add(pr, kPowerToggle, 0), false, 26);
    if (powerOpen_) {
        hits_.push_back({pr, kPowerToggle, 0});
        struct P { const char* name; int act; bool danger; };
        const P items[5] = {{"Exit shell", kExit, false}, {"Restart", kRestart, true}, {"Shut down", kShut, true}, {"Lock", kLock, false}, {"Sleep", kSleep, false}};
        float py = pr.y0 - 10;
        for (int i = 4; i >= 0; --i) {
            py -= 50;
            Rect r{rail.x0, py, rail.x1, py + 44};
            const bool armed = (items[i].act == kRestart && armed_ == 1) || (items[i].act == kShut && armed_ == 2);
            State s = add(r, kAct, items[i].act);
            button(bm, r, armed ? "Confirm" : items[i].name, armed ? State::Pressed : s, armed, 24);
            py -= 6;
        }
    }

    // Search
    Rect sr{mid.x0, mid.y0, mid.x1, mid.y0 + 58};
    card(bm, sr, searchFocus_ ? State::Selected : add(sr, kSearch, 0));
    if (searchFocus_) hits_.push_back({sr, kSearch, 0});
    const std::string shown = query_.empty() ? "Search apps and tools" : query_ + (searchFocus_ ? "|" : "");
    label(bm, sr.x0 + 20, sr.y0 + 38, ellipsize(shown, 28, FontFace::UI, sr.w() - 100), 28, FontFace::UI, query_.empty() ? t.muted : t.text);
    if (!query_.empty()) {
        Rect xr{sr.x1 - 54, sr.y0 + 8, sr.x1 - 8, sr.y1 - 8};
        button(bm, xr, "x", add(xr, kAct, kClear), false, 24);
    }

    float cy = sr.y1 + 26;
    if (!query_.empty()) {
        const std::vector<Result> res = results();
        if (res.empty()) label(bm, mid.x0 + 4, cy + 24, "Nothing matches", 26, FontFace::UI, t.dim);
        for (size_t i = 0; i < res.size() && cy + 56 <= mid.y1; ++i) {
            Rect r{mid.x0, cy, mid.x1, cy + 56};
            card(bm, r, i == 0 ? State::Selected : add(r, res[i].type, res[i].arg));
            if (i == 0) hits_.push_back({r, res[i].type, res[i].arg});
            float tx = r.x0 + 20;
            if (res[i].type == kApp && res[i].arg < static_cast<int>(appIcons_.size())) {
                blitIcon(bm, appIcons_[res[i].arg], static_cast<int>(r.x0 + 12), static_cast<int>(r.y0 + 8), 40);
                tx = r.x0 + 66;
            }
            label(bm, tx, r.y0 + 37, ellipsize(res[i].name, 26, FontFace::UI, r.x1 - tx - 20), 26, FontFace::UI, t.text);
            labelRight(bm, r.x1 - 18, r.y0 + 36, res[i].type == kApp ? "APP" : "TOOL", 20, FontFace::UI, t.muted);
            cy += 64;
        }
    } else {
        label(bm, mid.x0 + 4, cy, "TOOLS", 22, FontFace::UIBold, t.dim);
        cy += 14;
        const int cols = 3;
        const float gap = 12, tw = (mid.w() - gap * (cols - 1)) / cols, th = 90;
        for (int i = 0; i < 6; ++i) {
            const int col = i % cols, row = i / cols;
            Rect r{mid.x0 + col * (tw + gap), cy + row * (th + gap), mid.x0 + col * (tw + gap) + tw, cy + row * (th + gap) + th};
            card(bm, r, add(r, kTool, static_cast<int>(kTools[i].tool)));
            Rect g{r.x0 + 12, r.y0 + 21, r.x0 + 60, r.y0 + 69};
            fillRound(bm, g.x0, g.y0, g.x1, g.y1, 12, withAlpha(t.accent, 60));
            const std::string gl(1, kTools[i].glyph);
            label(bm, g.x0 + (48 - measureText(gl, 30, FontFace::UIBold)) * 0.5f, g.y0 + 35, gl, 30, FontFace::UIBold, t.accent);
            label(bm, r.x0 + 70, r.y0 + 52, ellipsize(kTools[i].name, 21, FontFace::UI, tw - 78), 21, FontFace::UI, t.text);
        }
        cy += 2 * (th + gap) + 14;
        label(bm, mid.x0 + 4, cy, "WINDOWS APPS", 22, FontFace::UIBold, t.dim);
        cy += 14;
        if (appNames_.empty()) {
            label(bm, mid.x0 + 4, cy + 28, "No pinned apps found", 24, FontFace::UI, t.muted);
        } else {
            const float ah = 66;
            const int maxRows = std::max(1, static_cast<int>((mid.y1 - cy + gap) / (ah + gap)));
            const int shownN = std::min(static_cast<int>(appNames_.size()), maxRows * cols);
            for (int i = 0; i < shownN; ++i) {
                const int col = i % cols, row = i / cols;
                Rect r{mid.x0 + col * (tw + gap), cy + row * (ah + gap), mid.x0 + col * (tw + gap) + tw, cy + row * (ah + gap) + ah};
                card(bm, r, add(r, kApp, i));
                float tx = r.x0 + 16;
                if (i < static_cast<int>(appIcons_.size())) {
                    blitIcon(bm, appIcons_[i], static_cast<int>(r.x0 + 12), static_cast<int>(r.y0 + 13), 40);
                    tx = r.x0 + 64;
                }
                label(bm, tx, r.y0 + 42, ellipsize(appNames_[i], 22, FontFace::UI, r.x1 - tx - 12), 22, FontFace::UI, t.text);
            }
            if (shownN < static_cast<int>(appNames_.size()))
                labelRight(bm, mid.x1, mid.y1 - 2, "+" + std::to_string(appNames_.size() - shownN) + " more: search", 20, FontFace::UI, t.muted);
        }
    }

    // Now column: the open windows, and the arrange buttons.
    card(bm, now, State::Normal);
    label(bm, now.x0 + 18, now.y0 + 36, "NOW", 22, FontFace::UIBold, t.dim);
    const float btnH = 50, btnArea = btnH * 2 + 10 + 28;
    float ry = now.y0 + 56;
    const float rh = 50;
    if (rows_.empty()) label(bm, now.x0 + 18, ry + 30, "No windows open", 24, FontFace::UI, t.muted);
    for (size_t i = 0; i < rows_.size() && ry + rh <= now.y1 - btnArea; ++i) {
        Rect r{now.x0 + 10, ry, now.x1 - 10, ry + rh};
        const State s = add(r, kRow, static_cast<int>(i));
        if (s == State::Hover) fillRound(bm, r.x0, r.y0, r.x1, r.y1, 8, t.panelHi);
        const StartHost::Row& w = rows_[i];
        dot(bm, r.x0 + 16, r.y0 + 26, 6, w.state == 2 ? t.accent : (w.state == 1 ? t.muted : t.success));
        label(bm, r.x0 + 34, r.y0 + 32, ellipsize(w.name, 24, FontFace::UI, r.w() - 50 - (w.state == 1 ? 100 : 0)), 24, FontFace::UI, w.state == 1 ? t.dim : t.text);
        if (w.state == 1) labelRight(bm, r.x1 - 10, r.y0 + 31, "MIN", 18, FontFace::UI, t.muted);
        ry += rh + 4;
    }
    const float bw = (now.w() - 20 - 10) / 2;
    Rect b1{now.x0 + 10, now.y1 - 10 - btnH * 2 - 10, now.x0 + 10 + bw, now.y1 - 10 - btnH - 10};
    Rect b2{b1.x1 + 10, b1.y0, now.x1 - 10, b1.y1};
    Rect b3{now.x0 + 10, now.y1 - 10 - btnH, now.x1 - 10, now.y1 - 10};
    button(bm, b1, "Arc", add(b1, kAct, kArc), false, 24);
    button(bm, b2, "Tile", add(b2, kAct, kTile), false, 24);
    button(bm, b3, "Minimise all", add(b3, kAct, kMin), false, 24);
}

}  // namespace sf
