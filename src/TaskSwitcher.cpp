#include "TaskSwitcher.h"

#include <algorithm>
#include <cmath>

#include "TextRender.h"

namespace sf {

namespace {
constexpr int kCardW = 340, kCardH = 170, kGap = 20, kPad = 26;

// Rounded rectangle composited "over" the bitmap with a one pixel antialiased edge.
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

void TaskSwitcher::open(std::vector<Item> items, Vec3 accent) {
    items_ = std::move(items);
    accent_ = accent;
    open_ = !items_.empty();
    selected_ = 0;
    const int n = static_cast<int>(items_.size());
    for (int i = 0; i < n; ++i)
        if (items_[static_cast<size_t>(i)].current) selected_ = (i + 1) % n;
    rebuild();
}

void TaskSwitcher::step(int dir) {
    if (!open_ || items_.empty()) return;
    const int n = static_cast<int>(items_.size());
    selected_ = ((selected_ + dir) % n + n) % n;
    rebuild();
}

void TaskSwitcher::rebuild() {
    const int n = static_cast<int>(items_.size());
    Bitmap bm;
    bm.w = kPad * 2 + n * kCardW + (n - 1) * kGap;
    bm.h = kPad * 2 + kCardH;
    bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
    fillRoundRect(bm, 0, 0, static_cast<float>(bm.w), static_cast<float>(bm.h), 30, Color8{10, 14, 26, 238});
    fillRoundRect(bm, 0, 0, static_cast<float>(bm.w), static_cast<float>(bm.h), 30, Color8{0, 0, 0, 0});
    for (int i = 0; i < n; ++i) {
        const Item& it = items_[static_cast<size_t>(i)];
        const float x0 = static_cast<float>(kPad + i * (kCardW + kGap)), y0 = static_cast<float>(kPad);
        const bool sel = i == selected_;
        if (sel) fillRoundRect(bm, x0 - 4, y0 - 4, x0 + kCardW + 4, y0 + kCardH + 4, 24, toColor8(accent_));
        fillRoundRect(bm, x0, y0, x0 + kCardW, y0 + kCardH, 20, sel ? Color8{34, 48, 78, 255} : Color8{22, 30, 50, 255});
        const std::string name = ellipsize(it.name, 40, FontFace::UIBold, kCardW - 40);
        drawText(bm, x0 + (kCardW - measureText(name, 40, FontFace::UIBold)) * 0.5f, y0 + 84, name, 40, FontFace::UIBold,
                 Color8{238, 242, 250, 255});
        const std::string detail = ellipsize(it.current ? "CURRENT" : it.detail, 24, FontFace::UI, kCardW - 40);
        drawText(bm, x0 + (kCardW - measureText(detail, 24, FontFace::UI)) * 0.5f, y0 + 124, detail, 24, FontFace::UI,
                 it.current ? toColor8(accent_) : Color8{140, 153, 184, 255});
    }
    aspect_ = static_cast<float>(bm.w) / bm.h;
    label_ = renderer_.createLabel(bm);
}

void TaskSwitcher::render(const Mat4& view, float aspect, float tanHalfFov, bool vr) {
    if (!open_ || !label_) return;
    renderer_.clearDepth();
    const float dist = vr ? 1.2f : 1.0f;
    const float maxW = std::min(vr ? 1.1f : 1.7f, 2.0f * tanHalfFov * dist * aspect * 0.9f);
    const float h = std::min(0.3f, maxW / aspect_), w = h * aspect_;
    const float uv[4] = {0, 0, 1, 1};
    const float white[3] = {1, 1, 1};
    renderer_.drawWebRegion(*label_, view.rigidInverse() * Mat4::translate({0, 0, -dist}) *     Mat4::scale({w, h, 1}), uv, aspect_, 30.0f / (kPad * 2 + kCardH),
                            0.0f, 1.0f, white);
}

}  // namespace sf
