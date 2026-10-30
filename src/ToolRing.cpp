#include "ToolRing.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace sf {

namespace {
const char* kNames[ToolRing::kCount] = {"FILES", "SETTINGS", "THEME", "APPS", "POWER"};
constexpr float kTau = 2.0f * kPi;
}  // namespace

ToolRing::ToolRing(Renderer& renderer) : renderer_(renderer) {
    Bitmap b;
    b.w = b.h = 1;
    b.rgba = {255, 255, 255, 255};
    dummy_ = renderer.createLabel(b);
    for (int i = 0; i < kCount; ++i) {
        pills_.push_back(std::make_unique<PanelNode>(renderer, kNames[i], Vec3{0.1f, 0.85f, 1.0f}, 0.30f, 0.085f, 0));
    }
}

void ToolRing::open(Vec3 eye, Vec3 forward) {
    Vec3 f = {forward.x, 0.0f, forward.z};
    if (length(f) < 0.25f) f = fallbackFlat_;  // looking almost straight up/down
    f = normalize(f);
    fallbackFlat_ = f;
    center_ = eye + f * 1.5f;
    center_.y = eye.y;
    Vec3 d = eye - center_;
    yaw_ = std::atan2(d.x, d.z);
    open_ = true;
    pulse_ = 0;
}

void ToolRing::close() {
    open_ = false;
    appsMode_ = false;
    level_ = 0;
    intent_ = 0;
}

void ToolRing::cycle(int delta) {
    if (appsMode_ && !sets_[level_].icons.empty()) {
        const int n = static_cast<int>(sets_[level_].icons.size());
        appSel_ = ((appSel_ + delta) % n + n) % n;
        appHot_ = true;
        return;
    }
    sel_ = ((sel_ + delta) % kCount + kCount) % kCount;
}

void ToolRing::setLevel(int level, const std::vector<std::string>& names, const std::vector<Bitmap>& icons, bool transparent) {
    ItemSet& s = sets_[level];
    s.icons.clear();
    s.names.clear();
    s.transparent = transparent;
    for (size_t i = 0; i < names.size(); ++i) {
        s.icons.push_back(renderer_.createLabel(icons[i], true));
        s.names.push_back(std::make_shared<PanelNode>(renderer_, names[i], Vec3{0.1f, 0.85f, 1.0f}, 0.4f, 0.085f, 0));
    }
    appSel_ = 0;
}

void ToolRing::showApps(bool on) {
    level_ = 0;
    appsMode_ = on && !sets_[0].icons.empty();
    appHot_ = appsMode_;
    appSel_ = 0;
}

void ToolRing::enterLevel(int level) {
    if (sets_[level].icons.empty()) return;
    level_ = level;
    appSel_ = 0;
    appHot_ = true;
}
void ToolRing::pointer(Vec3 o, Vec3 d) {
    if (!open_) return;
    const Vec3 n{std::sin(yaw_), 0.0f, std::cos(yaw_)};
    const float denom = dot(n, d);
    if (std::fabs(denom) < 1e-4f) return;
    const float t = dot(center_ - o, n) / denom;
    if (t <= 0) return;
    const Vec3 hit = o + d * t - center_;
    const Vec3 right{std::cos(yaw_), 0.0f, -std::sin(yaw_)};
    const float sc = 1.0f - 0.4f * appsAnim_;
    const float half = kDiameter * 0.5f * sc;
    const float u = dot(hit, right) / half, v = hit.y / half;
    const float r = std::sqrt(u * u + v * v);
    if (appsMode_ && !sets_[level_].icons.empty()) {  // the outer ring is twice as wide
        const float ro = r / kOuter;
        if (ro >= 0.5f && ro <= 0.97f) {
            float a = std::atan2(u, v);
            if (a < 0) a += kTau;
            const int n = static_cast<int>(sets_[level_].icons.size());
            appSel_ = std::min(n - 1, static_cast<int>(a / kTau * n));
            appHot_ = true;
            hoverIntent(level_ == 0 && appSel_ == n - 1 && !sets_[1].icons.empty() ? 3 : 0);
            return;
        }
    }
    appHot_ = false;
    const bool inner = r >= 0.5f && r <= 0.95f;
    bool overApps = false;
    if (inner) {
        float ang = std::atan2(u, v);  // clockwise from the top
        if (ang < 0) ang += kTau;
        sel_ = std::min(kCount - 1, static_cast<int>(ang / kTau * kCount));
        overApps = kMap[sel_] == Tool::Apps;
    }
    // Sub-rings follow the pointer: hovering Apps opens it, leaving both the parent item and the sub-ring closes it.
    const bool away = inner || r / kOuter > 1.0f;
    if (!appsMode_) hoverIntent(overApps ? 1 : 0);
    else if (overApps) hoverIntent(level_ == 1 ? 4 : 0);
    else hoverIntent(away ? 2 : 0);
}

void ToolRing::update(double dt, double time) {
    time_ = static_cast<float>(time);
    dwell_ += static_cast<float>(dt);
    if (open_ && dwell_ > 0.12f) {
        if (intent_ == 2 && appsMode_) showApps(false);
        else if (intent_ == 3 && level_ == 0) enterLevel(1);
        else if (intent_ == 4 && level_ == 1) {
            level_ = 0;
            appSel_ = std::max(0, static_cast<int>(sets_[0].names.size()) - 1);
        }
    }
    const float k = std::min(1.0f, static_cast<float>(dt) * 9.0f);
    anim_ += ((open_ ? 1.0f : 0.0f) - anim_) * k;
    if (!open_ && anim_ < 0.01f) anim_ = 0;
    pulse_ = std::max(0.0f, pulse_ - static_cast<float>(dt) * 2.5f);
    appsAnim_ += ((appsMode_ ? 1.0f : 0.0f) - appsAnim_) * k;
    if (appsMode_ && appSel_ < static_cast<int>(sets_[level_].names.size())) sets_[level_].names[appSel_]->update(dt, time);
    const Theme& th = currentTheme();
    for (int i = 0; i < kCount; ++i) {
        pills_[i]->setSelected(i == sel_);
        pills_[i]->setColor(i == sel_ ? th.alt : th.accent);
        pills_[i]->update(dt, time);
    }
}

void ToolRing::render(Renderer& renderer) {
    if (anim_ <= 0.01f) return;
    const float spin = (1.0f - anim_) * -1.2f;
    const float sc = 1.0f - 0.4f * appsAnim_;
    const float dia = kDiameter * sc;
    const Mat4 world = Mat4::translate(center_) * Mat4::rotateY(yaw_) * Mat4::scale({dia, dia, 1.0f});
    renderer.drawRing(*dummy_, world, kCount, static_cast<float>(sel_), spin, anim_, pulse_);
    if (appsAnim_ > 0.02f && !sets_[level_].icons.empty()) {
        const int n = static_cast<int>(sets_[level_].icons.size());
        const float od = dia * kOuter, oa = std::min(anim_, appsAnim_);
        const Mat4 ow = Mat4::translate(center_ + Vec3{std::sin(yaw_), 0, std::cos(yaw_)} * -0.01f) * Mat4::rotateY(yaw_) * Mat4::scale({od, od, 1.0f});
        renderer.drawRing(*dummy_, ow, n, static_cast<float>(appSel_), -spin, oa, appHot_ ? pulse_ : 0.0f);
        const Vec3 rt{std::cos(yaw_), 0.0f, -std::sin(yaw_)};
        const Vec3 tw{std::sin(yaw_), 0.0f, std::cos(yaw_)};
        const float rad = od * 0.5f * 0.75f;
        const float arc = kTau * rad / n;
        const float isz = std::min(0.13f * sc, arc * 0.6f) * oa;
        const bool clear = sets_[level_].transparent;
        if (oa > 0.6f)
            for (int i = 0; i < n; ++i) {
                const float a = (i + 0.5f) / n * kTau;
                const Vec3 pos = center_ + rt * (std::sin(a) * rad) + Vec3{0, std::cos(a) * rad, 0} + tw * 0.012f;
                const float s = isz * (i == appSel_ ? 1.18f : 1.0f);
                const float uv[4] = {0, 0, 1, 1}, tint[3] = {0, 0, 0};
                renderer.drawWebRegion(*sets_[level_].icons[i], Mat4::translate(pos) * Mat4::rotateY(yaw_) * Mat4::scale({s, s, 1.0f}), uv, 1.0f,
                                       clear ? 0.0f : 0.1f, clear ? -1.0f : 0.0f, i == appSel_ ? 1.0f : (clear ? 0.62f : 1.0f), tint);
            }
        if (oa > 0.6f && appSel_ < static_cast<int>(sets_[level_].names.size())) {
            const float a = (appSel_ + 0.5f) / n * kTau;
            auto& p = *sets_[level_].names[appSel_];
            p.position = center_ + rt * (std::sin(a) * rad) + Vec3{0, std::cos(a) * rad - isz * 0.8f, 0} + tw * 0.03f;
            p.yaw = yaw_;
            p.scale = {0.24f * sc, 0.051f * sc, 1.0f};
            p.render(renderer);
        }
    }

    const Vec3 right{std::cos(yaw_), 0.0f, -std::sin(yaw_)};
    const Vec3 toward{std::sin(yaw_), 0.0f, std::cos(yaw_)};
    const float radius = dia * 0.5f * 0.75f;
    for (int i = 0; i < kCount; ++i) {
        const float a = (i + 0.5f) / kCount * kTau;
        const float dx = std::sin(a), dy = std::cos(a);
        auto& p = *pills_[i];
        p.position = center_ + right * (dx * radius) + Vec3{0, dy * radius, 0} + toward * 0.02f;
        p.yaw = yaw_;
        p.scale = {0.15f * anim_ * sc, 0.0425f * anim_ * sc, 1.0f};
        if (anim_ > 0.5f) p.render(renderer);
    }
}

}  // namespace sf
