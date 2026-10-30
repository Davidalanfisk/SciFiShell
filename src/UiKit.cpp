#include "UiKit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Theme.h"

namespace sf::ui {

Color8 withAlpha(Color8 c, uint8_t a) {
    c.a = a;
    return c;
}

Color8 mix(Color8 a, Color8 b, float t) {
    auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t + 0.5f); };
    return Color8{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

Color8 toColor8(Vec3 v, uint8_t a) {
    auto q = [](float f) { return static_cast<uint8_t>(std::clamp(f, 0.0f, 1.0f) * 255 + 0.5f); };
    return Color8{q(v.x), q(v.y), q(v.z), a};
}

Tokens tokens() {
    const Theme& th = currentTheme();
    Tokens t;
    t.bg = {8, 13, 20, 242};
    t.panel = {16, 24, 33, 255};
    t.panelHi = {22, 36, 50, 255};
    t.accent = toColor8(th.accent);
    t.alt = toColor8(th.alt);
    t.border = withAlpha(t.accent, 110);
    t.text = {216, 250, 255, 255};
    t.dim = {122, 168, 181, 255};
    t.muted = {75, 106, 118, 255};
    t.success = {51, 255, 153, 255};
    t.warn = {255, 204, 51, 255};
    t.danger = {255, 51, 102, 255};
    return t;
}

namespace {
inline void blend(uint8_t* p, Color8 c, float cover) {
    const float a = cover * c.a / 255.0f;
    if (a <= 0) return;
    const float pa = p[3] / 255.0f, oa = a + pa * (1 - a);
    if (oa <= 0) return;
    p[0] = static_cast<uint8_t>((c.r * a + p[0] * pa * (1 - a)) / oa);
    p[1] = static_cast<uint8_t>((c.g * a + p[1] * pa * (1 - a)) / oa);
    p[2] = static_cast<uint8_t>((c.b * a + p[2] * pa * (1 - a)) / oa);
    p[3] = static_cast<uint8_t>(oa * 255);
}
}  // namespace

void fillRect(Bitmap& bm, float x0, float y0, float x1, float y1, Color8 c) {
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0))), ix1 = std::min(bm.w, static_cast<int>(std::ceil(x1)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0))), iy1 = std::min(bm.h, static_cast<int>(std::ceil(y1)));
    for (int y = iy0; y < iy1; ++y) {
        uint8_t* row = &bm.rgba[static_cast<size_t>(y) * bm.w * 4];
        for (int x = ix0; x < ix1; ++x) {
            if (c.a == 255) {
                uint8_t* p = row + x * 4;
                p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
            } else {
                blend(row + x * 4, c, 1.0f);
            }
        }
    }
}

void fillRound(Bitmap& bm, float x0, float y0, float x1, float y1, float r, Color8 c) {
    if (r <= 0.5f) {
        fillRect(bm, x0, y0, x1, y1, c);
        return;
    }
    const int ix0 = std::max(0, static_cast<int>(x0) - 1), ix1 = std::min(bm.w, static_cast<int>(x1) + 2);
    const int iy0 = std::max(0, static_cast<int>(y0) - 1), iy1 = std::min(bm.h, static_cast<int>(y1) + 2);
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f - r, hy = (y1 - y0) * 0.5f - r;
    for (int y = iy0; y < iy1; ++y)
        for (int x = ix0; x < ix1; ++x) {
            const float dx = std::max(std::fabs(x + 0.5f - cx) - hx, 0.0f), dy = std::max(std::fabs(y + 0.5f - cy) - hy, 0.0f);
            const float cover = std::clamp(r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            if (cover > 0) blend(&bm.rgba[(static_cast<size_t>(y) * bm.w + x) * 4], c, cover);
        }
}

void strokeRect(Bitmap& bm, float x0, float y0, float x1, float y1, float t, Color8 c) {
    fillRect(bm, x0, y0, x1, y0 + t, c);
    fillRect(bm, x0, y1 - t, x1, y1, c);
    fillRect(bm, x0, y0 + t, x0 + t, y1 - t, c);
    fillRect(bm, x1 - t, y0 + t, x1, y1 - t, c);
}

void line(Bitmap& bm, float x0, float y0, float x1, float y1, float t, Color8 c) {
    const float len = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
    const float h = t * 0.5f;
    if (len < 0.5f) {
        fillRound(bm, x0 - h, y0 - h, x0 + h, y0 + h, h, c);
        return;
    }
    for (float s = 0; s <= len; s += 0.5f) {
        const float x = x0 + (x1 - x0) * s / len, y = y0 + (y1 - y0) * s / len;
        fillRound(bm, x - h, y - h, x + h, y + h, h, c);
    }
}

void brackets(Bitmap& bm, float x0, float y0, float x1, float y1, float len, float t, Color8 c) {
    fillRect(bm, x0, y0, x0 + len, y0 + t, c);
    fillRect(bm, x0, y0, x0 + t, y0 + len, c);
    fillRect(bm, x1 - len, y0, x1, y0 + t, c);
    fillRect(bm, x1 - t, y0, x1, y0 + len, c);
    fillRect(bm, x0, y1 - t, x0 + len, y1, c);
    fillRect(bm, x0, y1 - len, x0 + t, y1, c);
    fillRect(bm, x1 - len, y1 - t, x1, y1, c);
    fillRect(bm, x1 - t, y1 - len, x1, y1, c);
}

void label(Bitmap& bm, float x, float baseline, const std::string& s, float px, FontFace f, Color8 c) {
    drawText(bm, x, baseline, s, px, f, c);
}

void labelRight(Bitmap& bm, float xRight, float baseline, const std::string& s, float px, FontFace f, Color8 c) {
    drawText(bm, xRight - measureText(s, px, f), baseline, s, px, f, c);
}

Rect panel(Bitmap& bm, const std::string& title, const std::string& subtitle) {
    const Tokens t = tokens();
    fillRect(bm, 0, 0, static_cast<float>(bm.w), static_cast<float>(bm.h), t.bg);
    fillRect(bm, 0, 0, static_cast<float>(bm.w), 84, withAlpha(t.panelHi, 200));
    fillRect(bm, 0, 84, static_cast<float>(bm.w), 86, withAlpha(t.accent, 150));
    drawText(bm, 36, 58, title, 46, FontFace::UIBold, t.accent);
    if (!subtitle.empty()) {
        const float tw = measureText(title, 46, FontFace::UIBold);
        drawText(bm, 36 + tw + 28, 56, subtitle, 22, FontFace::UI, t.dim);
    }
    brackets(bm, 3, 3, static_cast<float>(bm.w) - 3, static_cast<float>(bm.h) - 3, 34, 4, t.accent);
    return Rect{36, 108, static_cast<float>(bm.w) - 36, static_cast<float>(bm.h) - 30};
}

void card(Bitmap& bm, const Rect& r, State s) {
    const Tokens t = tokens();
    const bool hot = s == State::Selected || s == State::Hover;
    fillRect(bm, r.x0, r.y0, r.x1, r.y1, hot ? t.panelHi : t.panel);
    strokeRect(bm, r.x0, r.y0, r.x1, r.y1, hot ? 3.0f : 2.0f, hot ? t.accent : t.border);
    brackets(bm, r.x0, r.y0, r.x1, r.y1, 14, hot ? 4.0f : 3.0f, hot ? t.accent : withAlpha(t.accent, 170));
}

void button(Bitmap& bm, const Rect& r, const std::string& text, State s, bool primary, float px) {
    const Tokens t = tokens();
    const bool dis = s == State::Disabled, hot = s == State::Hover, down = s == State::Pressed || s == State::Selected;
    Color8 fill = primary ? mix(t.panel, t.accent, down ? 0.34f : (hot ? 0.26f : 0.16f)) : (hot || down ? t.panelHi : t.panel);
    Color8 edge = dis ? t.muted : t.accent;
    const float inset = down ? 2.0f : 0.0f;
    fillRect(bm, r.x0 + inset, r.y0 + inset, r.x1 - inset, r.y1 - inset, fill);
    strokeRect(bm, r.x0, r.y0, r.x1, r.y1, hot || down ? 3.0f : 2.0f, edge);
    if (hot) strokeRect(bm, r.x0 - 4, r.y0 - 4, r.x1 + 4, r.y1 + 4, 1.5f, withAlpha(t.accent, 110));
    const Color8 tc = dis ? t.muted : t.text;
    const float tw = measureText(text, px, FontFace::UIBold);
    drawText(bm, r.x0 + (r.w() - tw) * 0.5f, r.y0 + (r.h() + px * 0.7f) * 0.5f, text, px, FontFace::UIBold, tc);
    if (dis) line(bm, r.x0 + 6, r.y1 - 6, r.x1 - 6, r.y0 + 6, 1.5f, withAlpha(t.muted, 160));
}

void toggle(Bitmap& bm, const Rect& r, bool on, State s) {
    const Tokens t = tokens();
    const float h = r.h();
    fillRound(bm, r.x0, r.y0, r.x1, r.y1, h * 0.5f, on ? mix(t.panel, t.accent, 0.35f) : t.panel);
    fillRound(bm, r.x0, r.y0, r.x1, r.y1, h * 0.5f, withAlpha(on ? t.accent : t.muted, 0));
    strokeRect(bm, r.x0, r.y0, r.x1, r.y1, s == State::Hover ? 3.0f : 2.0f, on ? t.accent : t.muted);
    const float kx = on ? r.x1 - h * 0.5f : r.x0 + h * 0.5f;
    fillRound(bm, kx - h * 0.34f, r.y0 + h * 0.16f, kx + h * 0.34f, r.y1 - h * 0.16f, h * 0.34f, on ? t.accent : t.dim);
}

void slider(Bitmap& bm, const Rect& r, float v, State s) {
    const Tokens t = tokens();
    v = std::clamp(v, 0.0f, 1.0f);
    const float cy = (r.y0 + r.y1) * 0.5f, x = r.x0 + r.w() * v;
    fillRect(bm, r.x0, cy - 3, r.x1, cy + 3, t.muted);
    fillRect(bm, r.x0, cy - 3, x, cy + 3, t.accent);
    const float k = s == State::Hover || s == State::Pressed ? 14.0f : 11.0f;
    fillRound(bm, x - k, cy - k, x + k, cy + k, k, t.accent);
    fillRound(bm, x - k + 5, cy - k + 5, x + k - 5, cy + k - 5, k - 5, t.bg);
}

void bar(Bitmap& bm, const Rect& r, float v, Color8 fill) {
    const Tokens t = tokens();
    v = std::clamp(v, 0.0f, 1.0f);
    fillRect(bm, r.x0, r.y0, r.x1, r.y1, withAlpha(t.panel, 255));
    strokeRect(bm, r.x0, r.y0, r.x1, r.y1, 1.5f, withAlpha(t.accent, 90));
    const float seg = 10.0f, gap = 3.0f, wid = (r.w() - 6) * v;
    for (float x = r.x0 + 3; x + 1 < r.x0 + 3 + wid; x += seg + gap)
        fillRect(bm, x, r.y0 + 3, std::min(x + seg, r.x0 + 3 + wid), r.y1 - 3, fill);
}

void dot(Bitmap& bm, float cx, float cy, float radius, Color8 c) {
    fillRound(bm, cx - radius, cy - radius, cx + radius, cy + radius, radius, c);
}

void graph(Bitmap& bm, const Rect& r, const std::vector<float>& v, Color8 c) {
    const Tokens t = tokens();
    fillRect(bm, r.x0, r.y0, r.x1, r.y1, withAlpha(t.panel, 255));
    for (int i = 1; i < 4; ++i) {
        const float y = r.y0 + r.h() * i / 4.0f;
        fillRect(bm, r.x0, y, r.x1, y + 1, withAlpha(t.accent, 36));
    }
    strokeRect(bm, r.x0, r.y0, r.x1, r.y1, 1.5f, withAlpha(t.accent, 90));
    if (v.size() < 2) return;
    const float w = r.w() - 4, h = r.h() - 6;
    const float stepX = w / static_cast<float>(v.size() - 1);
    float px = 0, py = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const float x = r.x0 + 2 + stepX * i, y = r.y1 - 3 - h * std::clamp(v[i], 0.0f, 1.0f);
        fillRect(bm, x, y, x + std::max(1.0f, stepX), r.y1 - 2, withAlpha(c, 34));
        if (i) line(bm, px, py, x, y, 2.5f, c);
        px = x;
        py = y;
    }
}

void windowButtons(Bitmap& bm, int panelW, int hover, Rect& fs, Rect& close, Rect* hide) {
    const Tokens t = tokens();
    close = {panelW - 112.0f, 14, panelW - 24.0f, 70};
    fs = {panelW - 214.0f, 14, panelW - 126.0f, 70};
    const bool hf = hover == 1, hc = hover == 2;
    fillRect(bm, fs.x0, fs.y0, fs.x1, fs.y1, hf ? t.panelHi : t.panel);
    strokeRect(bm, fs.x0, fs.y0, fs.x1, fs.y1, hf ? 3.0f : 2.0f, hf ? t.accent : t.border);
    fillRect(bm, close.x0, close.y0, close.x1, close.y1, hc ? mix(t.panel, t.danger, 0.55f) : t.panel);
    strokeRect(bm, close.x0, close.y0, close.x1, close.y1, hc ? 3.0f : 2.0f, hc ? t.danger : t.border);
    const float fx = (fs.x0 + fs.x1) * 0.5f, cy = 42, mx = (close.x0 + close.x1) * 0.5f;
    strokeRect(bm, fx - 16, cy - 11, fx + 16, cy + 11, 3.0f, hf ? t.text : t.dim);
    line(bm, mx - 12, cy - 12, mx + 12, cy + 12, 3.0f, hc ? t.text : t.dim);
    line(bm, mx - 12, cy + 12, mx + 12, cy - 12, 3.0f, hc ? t.text : t.dim);
    if (hide) {
        *hide = {panelW - 316.0f, 14, panelW - 228.0f, 70};
        const bool hh = hover == 3;
        fillRect(bm, hide->x0, hide->y0, hide->x1, hide->y1, hh ? t.panelHi : t.panel);
        strokeRect(bm, hide->x0, hide->y0, hide->x1, hide->y1, hh ? 3.0f : 2.0f, hh ? t.accent : t.border);
        const float hx = (hide->x0 + hide->x1) * 0.5f;
        line(bm, hx - 16, cy + 8, hx + 16, cy + 8, 4.0f, hh ? t.text : t.dim);
    }
}

std::string bytesText(unsigned long long b) {
    char buf[32];
    const double g = 1073741824.0;
    if (b >= static_cast<unsigned long long>(g * 1024)) std::snprintf(buf, sizeof buf, "%.1f TB", b / (g * 1024));
    else if (b >= static_cast<unsigned long long>(g)) std::snprintf(buf, sizeof buf, "%.1f GB", b / g);
    else if (b >= 1048576ull) std::snprintf(buf, sizeof buf, "%.1f MB", b / 1048576.0);
    else if (b >= 1024ull) std::snprintf(buf, sizeof buf, "%.0f KB", b / 1024.0);
    else std::snprintf(buf, sizeof buf, "%llu B", b);
    return buf;
}

}  // namespace sf::ui
