#include "ToolRing.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace sf {

namespace {
const char* kNames[ToolRing::kCount] = {"WEB", "FILES", "EDITOR", "MEDIA", "SETTINGS", "THEME", "APPS", "POWER"};
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
}

void ToolRing::cycle(int delta) {
    if (appsMode_ && !appIcons_.empty()) {
        const int n = static_cast<int>(appIcons_.size());
        appSel_ = ((appSel_ + delta) % n + n) % n;
        appHot_ = true;
        return;
    }
    sel_ = ((sel_ + delta) % kCount + kCount) % kCount;
}

void ToolRing::setApps(const std::vector<std::string>& names, const std::vector<Bitmap>& icons) {
    appIcons_.clear();
    appNames_.clear();
    for (size_t i = 0; i < names.size(); ++i) {
        appIcons_.push_back(renderer_.createLabel(icons[i], true));
        appNames_.push_back(std::make_unique<PanelNode>(renderer_, names[i], Vec3{0.1f, 0.85f, 1.0f}, 0.4f, 0.085f, 0));
    }
    appSel_ = 0;
}

void ToolRing::showApps(bool on) {
    appsMode_ = on && !appIcons_.empty();
    appHot_ = appsMode_;
    appSel_ = 0;
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
    if (appsMode_ && !appIcons_.empty()) {  // the outer ring is twice as wide
        const float ro = r / kOuter;
        if (ro >= 0.5f && ro <= 0.97f) {
            float a = std::atan2(u, v);
            if (a < 0) a += kTau;
            const int n = static_cast<int>(appIcons_.size());
            appSel_ = std::min(n - 1, static_cast<int>(a / kTau * n));
            appHot_ = true;
            return;
        }
    }
    appHot_ = false;
    if (r < 0.5f || r > 0.95f) return;
    float ang = std::atan2(u, v);  // clockwise from the top
    if (ang < 0) ang += kTau;
    sel_ = std::min(kCount - 1, static_cast<int>(ang / kTau * kCount));
}

void ToolRing::update(double dt, double time) {
    time_ = static_cast<float>(time);
    const float k = std::min(1.0f, static_cast<float>(dt) * 9.0f);
    anim_ += ((open_ ? 1.0f : 0.0f) - anim_) * k;
    if (!open_ && anim_ < 0.01f) anim_ = 0;
    pulse_ = std::max(0.0f, pulse_ - static_cast<float>(dt) * 2.5f);
    appsAnim_ += ((appsMode_ ? 1.0f : 0.0f) - appsAnim_) * k;
    if (appsMode_ && appSel_ < static_cast<int>(appNames_.size())) appNames_[appSel_]->update(dt, time);
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
    if (appsAnim_ > 0.02f && !appIcons_.empty()) {
        const int n = static_cast<int>(appIcons_.size());
        const float od = dia * kOuter, oa = std::min(anim_, appsAnim_);
        const Mat4 ow = Mat4::translate(center_ + Vec3{std::sin(yaw_), 0, std::cos(yaw_)} * -0.01f) * Mat4::rotateY(yaw_) * Mat4::scale({od, od, 1.0f});
        renderer.drawRing(*dummy_, ow, n, static_cast<float>(appSel_), -spin, oa, appHot_ ? pulse_ : 0.0f);
        const Vec3 rt{std::cos(yaw_), 0.0f, -std::sin(yaw_)};
        const Vec3 tw{std::sin(yaw_), 0.0f, std::cos(yaw_)};
        const float rad = od * 0.5f * 0.75f;
        const float arc = kTau * rad / n;
        const float isz = std::min(0.26f * sc * 1.6f, arc * 0.8f) * oa;
        if (oa > 0.6f)
            for (int i = 0; i < n; ++i) {
                const float a = (i + 0.5f) / n * kTau;
                const Vec3 pos = center_ + rt * (std::sin(a) * rad) + Vec3{0, std::cos(a) * rad, 0} + tw * 0.012f;
                const float s = isz * (i == appSel_ ? 1.18f : 1.0f);
                renderer.drawCube(*appIcons_[i], Mat4::translate(pos) * Mat4::rotateY(yaw_) * Mat4::scale({s, s, 0.01f}), {1, 1, 1}, false, true, -1.0f);
            }
        if (oa > 0.6f && appSel_ < static_cast<int>(appNames_.size())) {
            const float a = (appSel_ + 0.5f) / n * kTau;
            auto& p = *appNames_[appSel_];
            p.position = center_ + rt * (std::sin(a) * rad) + Vec3{0, std::cos(a) * rad - isz * 0.8f, 0} + tw * 0.03f;
            p.yaw = yaw_;
            p.scale = {0.4f * sc * 1.2f, 0.085f * sc * 1.2f, 1.0f};
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
        p.scale = {0.30f * anim_ * sc, 0.085f * anim_ * sc, 1.0f};
        if (anim_ > 0.5f) p.render(renderer);
    }
}

}  // namespace sf
