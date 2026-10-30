#include "SettingsHub.h"

#include <algorithm>
#include <cmath>

#include <cstdio>

#include "TextRender.h"
#include "UiKit.h"
#include "Theme.h"

namespace sf {

namespace {
constexpr int kW = 1832, kH = 960, kPad = 40, kGap = 24;
constexpr int kCols = 4, kHomeTop = 140, kHomeCardH = 240;
constexpr int kListTop = 236, kRowH = 96, kRowGap = 16, kMaxItems = 12;
constexpr float kWorldW = 2.0f, kDist = 1.7f;

void fillRoundRect(Bitmap& bm, float x0, float y0, float x1, float y1, float r, Color8 c) {
    const int ix0 = std::max(0, static_cast<int>(x0) - 1), ix1 = std::min(bm.w, static_cast<int>(x1) + 2);
    const int iy0 = std::max(0, static_cast<int>(y0) - 1), iy1 = std::min(bm.h, static_cast<int>(y1) + 2);
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
    for (int y = iy0; y < iy1; ++y)
        for (int x = ix0; x < ix1; ++x) {
            const float dx = std::max(std::fabs(x + 0.5f - cx) - hx, 0.0f), dy = std::max(std::fabs(y + 0.5f - cy) - hy, 0.0f);
            const float cover = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (cover <= 0) continue;
            uint8_t* p = &bm.rgba[(static_cast<size_t>(y) * bm.w + x) * 4];
            const float a = cover * c.a / 255.0f, pa = p[3] / 255.0f, oa = a + pa * (1 - a);
            if (oa <= 0) continue;
            p[0] = static_cast<uint8_t>((c.r * a + p[0] * pa * (1 - a)) / oa);
            p[1] = static_cast<uint8_t>((c.g * a + p[1] * pa * (1 - a)) / oa);
            p[2] = static_cast<uint8_t>((c.b * a + p[2] * pa * (1 - a)) / oa);
            p[3] = static_cast<uint8_t>(oa * 255);
        }
}

Color8 toColor8(Vec3 v, uint8_t a = 255) {
    auto q = [](float f) { return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255 + 0.5f); };
    return Color8{q(v.x), q(v.y), q(v.z), a};
}
}  // namespace

void SettingsHub::open(Vec3 eye, Vec3 forward) {
    if (open_) {
        hidden_ = false;
        return;
    }
    Vec3 f = {forward.x, 0.0f, forward.z};
    if (length(f) < 0.25f) f = fallbackFlat_;
    f = normalize(f);
    fallbackFlat_ = f;
    Vec3 c = length(forward) < 0.5f ? eye + f * kDist : eye + normalize(forward) * kDist;
    c.y = std::max(c.y, eye.y - 1.0f);
    base_ = Mat4::translate(c) * Mat4::rotateY(std::atan2(-f.x, -f.z));
    if (!hidden_) {
        page_ = -1;
        sel_ = homeSel_ = 0;
    }
    hidden_ = false;
    open_ = true;
    dirty_ = true;
}

void SettingsHub::refreshValues() {
    vals_.clear();
    if (page_ < 0 || page_ >= static_cast<int>(cats_.size())) return;
    for (const SettingsItem& it : cats_[static_cast<size_t>(page_)].items) vals_.push_back(it.value ? it.value() : std::string());
}

std::string SettingsHub::signature() const {
    std::string s = std::to_string(page_) + ":" + std::to_string(sel_) + ":" + currentTheme().name + ":" +
                    std::to_string(hoverBtn_) + (fullscreen_ ? "F" : "W");
    for (const std::string& v : vals_) s += "|" + v;
    return s;
}

void SettingsHub::rebuild() {
    Bitmap bm;
    bm.w = W_;
    bm.h = H_;
    bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
    rects_.clear();
    const ui::Tokens tk = ui::tokens();
    const bool home = page_ < 0 || page_ >= static_cast<int>(cats_.size());
    const std::string title = home ? "SETTINGS" : cats_[static_cast<size_t>(page_)].name;
    const std::string sub = home ? "SHELL PREFERENCES  +  WINDOWS CONTROL PANEL" : cats_[static_cast<size_t>(page_)].subtitle;
    ui::panel(bm, title, "");
    drawText(bm, W_ - kPad - 8 - measureText(sub, 24, FontFace::UI) - 240, 56, sub, 24, FontFace::UI, tk.dim);
    {
        ui::Rect fs, cl, hd;
        ui::windowButtons(bm, W_, hoverBtn_, fs, cl, &hd);
        fsRect_ = {fs.x0, fs.y0, fs.x1, fs.y1};
        closeRect_ = {cl.x0, cl.y0, cl.x1, cl.y1};
        hideRect_ = {hd.x0, hd.y0, hd.x1, hd.y1};
    }
    auto card = [&](const Rect& r, bool sel) {
        ui::card(bm, ui::Rect{r.x0, r.y0, r.x1, r.y1}, sel ? ui::State::Selected : ui::State::Normal);
    };

    if (home) {
        const int n = static_cast<int>(cats_.size());
        const float cw = static_cast<float>(W_ - 2 * kPad - (kCols - 1) * kGap) / kCols;
        for (int i = 0; i < n; ++i) {
            const SettingsCategory& c = cats_[static_cast<size_t>(i)];
            const float x = kPad + (i % kCols) * (cw + kGap), y = static_cast<float>(kHomeTop + (i / kCols) * (kHomeCardH + kGap));
            const Rect r{x, y, x + cw, y + kHomeCardH};
            rects_.push_back(r);
            const bool sel = i == sel_;
            card(r, sel);
            char num[8];
            std::snprintf(num, sizeof num, "%02d", i + 1);
            const Color8 tagCol = c.tag == "SHELL" ? tk.accent : tk.alt;
            drawText(bm, x + 28, y + 56, num, 34, FontFace::Mono, ui::withAlpha(tagCol, 200));
            ui::labelRight(bm, x + cw - 28, y + 52, c.tag, 20, FontFace::UIBold, tagCol);
            drawText(bm, x + 28, y + 128, ellipsize(c.name, 38, FontFace::UIBold, cw - 56), 38, FontFace::UIBold, tk.text);
            float ty = y + 166;
            for (const std::string& line : wrapText(c.subtitle, 22, FontFace::UI, cw - 56, 2)) {
                drawText(bm, x + 28, ty, line, 22, FontFace::UI, tk.dim);
                ty += 28;
            }
            ui::line(bm, x + 20, y + kHomeCardH - 38, x + cw - 20, y + kHomeCardH - 38, 1, ui::withAlpha(tk.border, 160));
            drawText(bm, x + 28, y + kHomeCardH - 12, "OPEN MODULE", 18, FontFace::UIBold, sel ? tk.accent : tk.muted);
            drawText(bm, x + cw - 44, y + kHomeCardH - 12, ">", 20, FontFace::UIBold, sel ? tk.accent : tk.muted);
        }
    } else {
        const SettingsCategory& c = cats_[static_cast<size_t>(page_)];
        const Rect br{kPad, 132, kPad + 260, 212};
        rects_.push_back(br);
        ui::button(bm, ui::Rect{br.x0, br.y0, br.x1, br.y1}, "< BACK", sel_ == 0 ? ui::State::Selected : ui::State::Normal, false, 30);
        const float cw = static_cast<float>(W_ - 2 * kPad - kGap) / 2;
        const int n = std::min(static_cast<int>(c.items.size()), kMaxItems);
        for (int i = 0; i < n; ++i) {
            const SettingsItem& it = c.items[static_cast<size_t>(i)];
            const float x = kPad + (i % 2) * (cw + kGap), y = static_cast<float>(kListTop + (i / 2) * (kRowH + kRowGap));
            const Rect r{x, y, x + cw, y + kRowH};
            rects_.push_back(r);
            card(r, sel_ == i + 1);
            const std::string val = static_cast<size_t>(i) < vals_.size() ? vals_[static_cast<size_t>(i)] : std::string();
            const float vw = val.empty() ? 0.0f : std::min(measureText(val, 30, FontFace::UIBold), cw * 0.45f);
            drawText(bm, x + 28, y + 42, ellipsize(it.title, 30, FontFace::UIBold, cw - 70 - vw), 30, FontFace::UIBold, tk.text);
            if (!it.hint.empty()) drawText(bm, x + 28, y + 76, ellipsize(it.hint, 20, FontFace::UI, cw - 70 - vw), 20, FontFace::UI, tk.dim);
            if (!val.empty()) {
                const std::string v = ellipsize(val, 30, FontFace::UIBold, vw);
                drawText(bm, x + cw - 28 - measureText(v, 30, FontFace::UIBold), y + 60, v, 30, FontFace::UIBold, tk.accent);
            }
        }
    }
    if (!rects_.empty()) sel_ = std::clamp(sel_, 0, static_cast<int>(rects_.size()) - 1);
    aspect_ = static_cast<float>(bm.w) / bm.h;
    label_ = renderer_.createLabel(bm);
}

int SettingsHub::hitIndex(Vec3 o, Vec3 d) const {
    float px, py, t;
    if (!toPanel(o, d, px, py, t)) return -1;
    for (size_t i = 0; i < rects_.size(); ++i)
        if (px >= rects_[i].x0 && px <= rects_[i].x1 && py >= rects_[i].y0 && py <= rects_[i].y1) return static_cast<int>(i);
    return -1;
}

bool SettingsHub::toPanel(Vec3 o, Vec3 d, float& px, float& py, float& t) const {
    const Mat4 inv = base_.rigidInverse();
    const Vec3 lo = inv.transformPoint(o), ld = inv.transformDir(d);
    if (std::fabs(ld.z) < 1e-5f) return false;
    t = -lo.z / ld.z;
    if (t < 0) return false;
    const float x = lo.x + ld.x * t, y = lo.y + ld.y * t, worldH = worldW_ / aspect_;
    px = (x / worldW_ + 0.5f) * W_;
    py = (0.5f - y / worldH) * H_;
    return true;
}

int SettingsHub::hitButton(Vec3 o, Vec3 d) const {
    float px, py, t;
    if (!toPanel(o, d, px, py, t)) return 0;
    auto in = [&](const Rect& r) { return px >= r.x0 && px <= r.x1 && py >= r.y0 && py <= r.y1; };
    if (in(closeRect_)) return 2;
    if (in(fsRect_)) return 1;
    if (in(hideRect_)) return 3;
    return 0;
}

void SettingsHub::pointer(Vec3 o, Vec3 d) {
    if (fullscreen_) applyFullscreen();
    if (dragging_) {
        const Vec3 p = o + d * grabDist_;
        Vec3 f{p.x - o.x, 0, p.z - o.z};
        if (length(f) > 0.05f) {
            f = normalize(f);
            base_ = Mat4::translate(p) * Mat4::rotateY(std::atan2(-f.x, -f.z)) * Mat4::translate({-grabX_, -grabY_, 0});
        }
        return;
    }
    const int h = hitIndex(o, d);
    if (h >= 0 && h != sel_) sel_ = h;
    hoverBtn_ = hitButton(o, d);
    float px, py, t;
    hoverTitle_ = toPanel(o, d, px, py, t) && px >= 0 && px <= W_ && py >= 0 && py < 86 && hoverBtn_ == 0;
}

void SettingsHub::applyFullscreen() {
    const float viewH = 2.0f * viewTan_, viewW = viewH * viewAspect_;
    const int h = kH, w = std::max(1000, static_cast<int>(h * viewAspect_ + 0.5f));
    if (w != W_ || h != H_) {
        W_ = w;
        H_ = h;
        aspect_ = static_cast<float>(W_) / H_;
        dirty_ = true;
    }
    worldW_ = viewW;
    base_ = Mat4::translate({0, 0, -1.0f});
}

void SettingsHub::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        savedBase_ = base_;
        applyFullscreen();
    } else {
        base_ = savedBase_;
        worldW_ = kWorldW;
        W_ = kW;
        H_ = kH;
        aspect_ = static_cast<float>(W_) / H_;
    }
    dirty_ = true;
}

void SettingsHub::close() {
    if (fullscreen_) toggleFullscreen();
    dragging_ = false;
    open_ = false;
    hidden_ = false;
}

void SettingsHub::hide() {
    if (!open_) return;
    if (fullscreen_) toggleFullscreen();
    dragging_ = false;
    open_ = false;
    hidden_ = true;
}

void SettingsHub::step(int delta) {
    const int n = static_cast<int>(rects_.size());
    if (n > 0) sel_ = ((sel_ + delta) % n + n) % n;
}

void SettingsHub::move(int dx, int dy) {
    if (rects_.empty()) return;
    const Rect& cur = rects_[static_cast<size_t>(sel_)];
    const float cx = (cur.x0 + cur.x1) * 0.5f, cy = (cur.y0 + cur.y1) * 0.5f;
    int best = -1;
    float bestScore = 1e9f;
    for (size_t i = 0; i < rects_.size(); ++i) {
        if (static_cast<int>(i) == sel_) continue;
        const float ox = (rects_[i].x0 + rects_[i].x1) * 0.5f - cx, oy = (rects_[i].y0 + rects_[i].y1) * 0.5f - cy;
        const float along = dx != 0 ? ox * dx : oy * dy, perp = dx != 0 ? std::fabs(oy) : std::fabs(ox);
        if (along < 20.0f) continue;
        const float score = along + 3.0f * perp;
        if (score < bestScore) { bestScore = score; best = static_cast<int>(i); }
    }
    if (best >= 0) sel_ = best;
}

void SettingsHub::activateSelected() {
    if (rects_.empty()) return;
    if (page_ < 0 || page_ >= static_cast<int>(cats_.size())) {
        if (sel_ < static_cast<int>(cats_.size())) {
            homeSel_ = sel_;
            page_ = sel_;
            sel_ = 1;
            dirty_ = true;
        }
        return;
    }
    if (sel_ == 0) {
        back();
        return;
    }
    const auto& items = cats_[static_cast<size_t>(page_)].items;
    if (sel_ - 1 < static_cast<int>(items.size()) && items[static_cast<size_t>(sel_ - 1)].act) items[static_cast<size_t>(sel_ - 1)].act();
    dirty_ = true;
}

bool SettingsHub::activateHit(Vec3 o, Vec3 d) {
    const int b = hitButton(o, d);
    if (b == 1) {
        toggleFullscreen();
        return true;
    }
    if (b == 2) {
        close();
        return true;
    }
    if (b == 3) {
        hide();
        return true;
    }
    float px, py, t;
    if (!fullscreen_ && toPanel(o, d, px, py, t) && py < 86) {
        dragging_ = true;
        grabX_ = (px / W_ - 0.5f) * worldW_;
        grabY_ = (0.5f - py / H_) * worldW_ / aspect_;
        grabDist_ = std::clamp(t, 0.8f, 8.0f);
        return true;
    }
    const int h = hitIndex(o, d);
    if (h < 0) return false;
    sel_ = h;
    activateSelected();
    return true;
}

bool SettingsHub::back() {
    if (fullscreen_) {
        toggleFullscreen();
        return true;
    }
    if (page_ < 0) return false;
    page_ = -1;
    sel_ = homeSel_;
    dirty_ = true;
    return true;
}

void SettingsHub::update(double dt, double) {
    if (!open_) return;
    if (fullscreen_) applyFullscreen();
    valueTimer_ += dt;
    if (dirty_ || valueTimer_ > 0.5) {
        refreshValues();
        valueTimer_ = 0;
    }
    const std::string s = signature();
    if (dirty_ || s != sig_ || !label_) {
        rebuild();
        sig_ = signature();
        dirty_ = false;
    }
}

void SettingsHub::render() {
    if (!open_ || !label_) return;
    if (fullscreen_) applyFullscreen();
    else renderer_.clearDepth();
    const float h = worldW_ / aspect_;
    const float uv[4] = {0, 0, 1, 1};
    const float white[3] = {1, 1, 1};
    if (!fullscreen_) {
        const float pad = 0.03f;
        const float fw = worldW_ + pad * 2, fh = h + pad * 2;
        renderer_.drawGlass(base_ * Mat4::translate({0, 0, -0.01f}) * Mat4::scale({fw, fh, 1}), fw / fh, 0.03f / fh);
    }
    renderer_.drawWebRegion(*label_, base_ * Mat4::scale({worldW_, h, 1}), uv, aspect_, fullscreen_ ? 0.0f : 0.004f / h, 0.0f,
                            fullscreen_ ? 1.0f : 0.94f, white);
}

float SettingsHub::hitDistance(Vec3 o, Vec3 d) const {
    if (!open_) return -1.0f;
    float px, py, t;
    if (!toPanel(o, d, px, py, t)) return -1.0f;
    const float m = fullscreen_ ? 0.0f : 0.03f / worldW_ * W_;
    if (px < -m || px > W_ + m || py < -m || py > H_ + m) return -1.0f;
    return t * length(d);
}

bool SettingsHub::bounds(Vec3& centre, Vec3& normal, float& w, float& h) const {
    if (!open_ || fullscreen_) return false;
    centre = base_.transformPoint({0, 0, 0});
    normal = normalize(base_.transformDir({0, 0, 1}));
    w = worldW_ + 0.06f;
    h = worldW_ / aspect_ + 0.06f;
    return true;
}

}  // namespace sf
