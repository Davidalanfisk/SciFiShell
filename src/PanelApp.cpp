#include "PanelApp.h"

#include <algorithm>
#include <cmath>

#include "Theme.h"

namespace sf {

namespace {
constexpr float kDist = 1.7f;
}

PanelApp::PanelApp(Renderer& renderer, int w, int h, float worldW)
    : renderer_(renderer), w_(w), h_(h), worldW_(worldW), baseWorldW_(worldW) {
    bm_.w = w;
    bm_.h = h;
    bm_.rgba.assign(static_cast<size_t>(w) * h * 4, 0);
}

void PanelApp::open(Vec3 eye, Vec3 forward) {
    if (!open_) {
        Vec3 f = {forward.x, 0.0f, forward.z};
        if (length(f) < 0.25f) f = fallbackFlat_;
        f = normalize(f);
        fallbackFlat_ = f;
        // Centre on where the viewer is actually looking (pitch included) so the panel lands in the middle of the view.
        Vec3 c = eye + normalize(forward) * kDist;
        if (length(forward) < 0.5f) c = eye + f * kDist;
        c.y = std::max(c.y, eye.y - 1.0f);
        base_ = Mat4::translate(c) * Mat4::rotateY(std::atan2(-f.x, -f.z));
        open_ = true;
        onOpen();
    }
    dirty_ = true;
}

void PanelApp::close() {
    if (fullscreen_) toggleFullscreen();
    if (open_) onClose();
    open_ = false;
}

void PanelApp::applyFullscreen() {
    const float viewH = 2.0f * viewTan_, viewW = viewH * viewAspect_;
    worldW_ = std::min(viewW, viewH * static_cast<float>(w_) / h_);
    base_ = Mat4::translate({0, 0, -1.0f});
}

void PanelApp::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        savedBase_ = base_;
        applyFullscreen();
    } else {
        base_ = savedBase_;
        worldW_ = baseWorldW_;
    }
    dirty_ = true;
}

bool PanelApp::toPanel(Vec3 o, Vec3 d, float& px, float& py, float& t) const {
    const Mat4 inv = base_.rigidInverse();
    const Vec3 lo = inv.transformPoint(o), ld = inv.transformDir(d);
    if (std::fabs(ld.z) < 1e-5f) return false;
    t = -lo.z / ld.z;
    if (t < 0) return false;
    const float x = lo.x + ld.x * t, y = lo.y + ld.y * t, worldH = worldW_ * h_ / w_;
    px = (x / worldW_ + 0.5f) * w_;
    py = (0.5f - y / worldH) * h_;
    return true;
}

void PanelApp::pointer(Vec3 o, Vec3 d) {
    if (fullscreen_) applyFullscreen();
    float px, py, t;
    int hb = 0;
    if (toPanel(o, d, px, py, t) && px >= 0 && px <= w_ && py >= 0 && py <= h_) {
        mx_ = px;
        my_ = py;
        if (close_.contains(px, py)) hb = 2;
        else if (fs_.contains(px, py)) hb = 1;
        if (onHover(px, py)) dirty_ = true;
    } else {
        if (mx_ >= 0 && onHover(-1, -1)) dirty_ = true;
        mx_ = my_ = -1;
    }
    if (hb != hoverBtn_) {
        hoverBtn_ = hb;
        dirty_ = true;
    }
}

bool PanelApp::click(Vec3 o, Vec3 d) {
    float px, py, t;
    if (!toPanel(o, d, px, py, t) || px < 0 || px > w_ || py < 0 || py > h_) return false;
    if (close_.contains(px, py)) {
        close();
        return true;
    }
    if (fs_.contains(px, py)) {
        toggleFullscreen();
        return true;
    }
    const bool dbl = clock_ - lastClick_ < 0.4 && std::fabs(px - lastClickX_) < 14 && std::fabs(py - lastClickY_) < 14;
    lastClick_ = dbl ? -10 : clock_;
    lastClickX_ = px;
    lastClickY_ = py;
    onClick(px, py, dbl);
    dirty_ = true;
    return true;
}

void PanelApp::wheel(float notches) {
    if (mx_ < 0) return;
    onWheel(mx_, my_, notches);
    dirty_ = true;
}

void PanelApp::repaint() {
    std::fill(bm_.rgba.begin(), bm_.rgba.end(), uint8_t(0));
    paint(bm_);
    ui::windowButtons(bm_, w_, hoverBtn_, fs_, close_);
    if (!label_) label_ = renderer_.createDynamicLabel(w_, h_);
    renderer_.updateLabel(*label_, bm_.rgba.data(), w_, h_);
    dirty_ = false;
    sincePaint_ = 0;
}

void PanelApp::update(double dt) {
    clock_ += dt;
    if (!open_ && !keepAlive()) return;
    tick(dt);
    if (!open_) return;
    sincePaint_ += dt;
    const std::string sig = currentTheme().name;
    if (sig != themeSig_) {
        themeSig_ = sig;
        dirty_ = true;
    }
    const double iv = repaintInterval();
    if (iv > 0 && sincePaint_ >= iv) dirty_ = true;
    if (dirty_ || !label_) repaint();
    if (fullscreen_) applyFullscreen();
}

void PanelApp::render() {
    if (!open_ || !label_) return;
    if (fullscreen_) applyFullscreen();
    else renderer_.clearDepth();
    const float h = worldW_ * h_ / w_;
    const float uv[4] = {0, 0, 1, 1};
    const float white[3] = {1, 1, 1};
    if (!fullscreen_) {
        const float pad = 0.03f;
        const float fw = worldW_ + pad * 2, fh = h + pad * 2;
        renderer_.drawGlass(base_ * Mat4::translate({0, 0, -0.01f}) * Mat4::scale({fw, fh, 1}), fw / fh, 0.03f / fh);
    }
    renderer_.drawWebRegion(*label_, base_ * Mat4::scale({worldW_, h, 1}), uv, static_cast<float>(w_) / h_, fullscreen_ ? 0.0f : 0.004f / h,
                            0.0f, fullscreen_ ? 1.0f : 0.94f, white);
}

float PanelApp::hitDistance(Vec3 o, Vec3 d) const {
    if (!open_) return -1.0f;
    float px, py, t;
    if (!toPanel(o, d, px, py, t)) return -1.0f;
    const float m = fullscreen_ ? 0.0f : 0.03f / worldW_ * w_;
    if (px < -m || px > w_ + m || py < -m || py > h_ + m) return -1.0f;
    return t * length(d);
}

bool PanelApp::bounds(Vec3& centre, Vec3& normal, float& w, float& h) const {
    if (!open_ || fullscreen_) return false;
    centre = base_.transformPoint({0, 0, 0});
    normal = normalize(base_.transformDir({0, 0, 1}));
    w = worldW_ + 0.06f;
    h = worldW_ * h_ / w_ + 0.06f;
    return true;
}

}  // namespace sf
