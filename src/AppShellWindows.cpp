// Window layout: gliding windows around, arranging them (arc / tiles), carrying them between the city, lobbies and rooms,
// snapping a dragged window to its neighbours, and resizing from a corner handle.
#include <algorithm>
#include <cmath>

#include "AppShell.h"
#include "UiKit.h"

namespace sf {

namespace {
constexpr float kPi = 3.14159265f;

float wrapAngle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
Vec3 rightOf(float yaw) { return {std::cos(yaw), 0.0f, -std::sin(yaw)}; }
constexpr float kHandle = 0.18f;
}  // namespace

template <class F>
void AppShell::winVisit(WinRef w, F&& f) {
    switch (w.kind) {
#ifdef SCIFI_HAS_CEF
        case kWinBrowser: f(*static_cast<WebBrowser*>(w.p)); break;
#endif
#ifdef _WIN32
        case kWinApp: f(*static_cast<AppWindow*>(w.p)); break;
#endif
        case kWinPanel: f(*static_cast<PanelApp*>(w.p)); break;
        case kWinSettings: f(*static_cast<SettingsHub*>(w.p)); break;
        default: break;
    }
}

Vec3 AppShell::viewFlat() const {
    Vec3 f = vrOn_ ? head_.forward : Vec3{-std::sin(camera_.yaw), 0.0f, -std::cos(camera_.yaw)};
    f.y = 0;
    return length(f) < 1e-4f ? Vec3{0, 0, -1} : normalize(f);
}

float AppShell::winWidthOf(WinRef w) {
    float v = 0;
    winVisit(w, [&](auto& x) { v = x.worldWidth(); });
    return v;
}

bool AppShell::winMoving(WinRef w) {
    bool v = false;
    winVisit(w, [&](auto& x) { v = x.moving(); });
    return v;
}

void AppShell::winPushPull(WinRef w, float d) {
    winVisit(w, [&](auto& x) { x.pushPull(d); });
}

bool AppShell::winFrameOf(WinRef w, WinFrame& f) {
    Vec3 c, n;
    float bw = 0, bh = 0;
    if (!w.p || !winBoundsOf(w, c, n, bw, bh)) return false;
    f.c = c;
    f.n = n;
    f.yaw = std::atan2(n.x, n.z);
    f.w = bw;
    f.h = bh;
    f.ww = winWidthOf(w);
    return true;
}

// Puts the window's frame centre at `centre`, facing `yaw`, with the given content width.
void AppShell::winMoveTo(WinRef w, Vec3 centre, float yaw, float width) {
    WinFrame f;
    if (!winFrameOf(w, f)) return;
    Mat4 P;
    winVisit(w, [&](auto& x) { P = x.placement(); });
    const Vec3 local = P.rigidInverse().transformPoint(f.c);
    const float k = f.ww > 1e-4f ? width / f.ww : 1.0f;
    winVisit(w, [&](auto& x) { x.setWorldWidth(width); });
    const Mat4 R = Mat4::rotateY(yaw);
    Vec3 p = centre - R.transformDir(local * k);
    winVisit(w, [&](auto& x) { x.setPlacement(Mat4::translate(p) * R); });
    WinFrame g;
    if (winFrameOf(w, g)) {
        const Vec3 err = centre - g.c;
        if (length(err) > 1e-4f) winVisit(w, [&](auto& x) { x.setPlacement(Mat4::translate(p + err) * R); });
    }
}

void AppShell::winGlideTo(WinRef w, Vec3 centre, float yaw, float width) {
    WinFrame f;
    if (!winFrameOf(w, f)) return;
    winAnims_.erase(std::remove_if(winAnims_.begin(), winAnims_.end(), [&](const WinAnim& a) { return a.w == w; }), winAnims_.end());
    winAnims_.push_back({w, f.c, centre, f.yaw, yaw, f.ww, width, 0.0f});
}

void AppShell::winAnimStep(float dt) {
    for (size_t i = 0; i < winAnims_.size();) {
        WinAnim& a = winAnims_[i];
        WinFrame f;
        if (!winOpen(a.w) || winMoving(a.w) || !winFrameOf(a.w, f)) {
            winAnims_.erase(winAnims_.begin() + static_cast<long>(i));
            continue;
        }
        a.t += dt / 0.45f;
        const float t = std::min(a.t, 1.0f), s = t * t * (3 - 2 * t);
        const WinAnim cur = a;
        winMoveTo(cur.w, cur.c0 + (cur.c1 - cur.c0) * s, cur.yaw0 + wrapAngle(cur.yaw1 - cur.yaw0) * s, cur.w0 + (cur.w1 - cur.w0) * s);
        if (a.t >= 1.0f) winAnims_.erase(winAnims_.begin() + static_cast<long>(i));
        else ++i;
    }
}

std::vector<AppShell::WinRef> AppShell::arrangeable() {
    std::vector<WinRef> v;
    for (const WinRef& w : winOrder_) {
        WinFrame f;
        if (winOpen(w) && winFrameOf(w, f)) v.push_back(w);
    }
    return v;
}

namespace {
struct Item {
    AppShell::WinRef w;
    AppShell::WinFrame f;
    float a = 0;
};
}  // namespace

// Windows on a gentle arc around the viewer, ordered by where they already are so nothing swaps sides.
void AppShell::arrangeArc() {
    const Vec3 eye = viewEye(), fl = viewFlat(), rt{-fl.z, 0, fl.x};
    std::vector<Item> its;
    for (const WinRef& w : arrangeable()) {
        Item i{w, {}, 0};
        if (!winFrameOf(w, i.f)) continue;
        const Vec3 d = i.f.c - eye;
        i.a = std::atan2(dot(d, rt), dot(d, fl));
        its.push_back(i);
    }
    if (its.empty()) return;
    std::sort(its.begin(), its.end(), [](const Item& a, const Item& b) { return a.a < b.a; });
    const float gap = 0.18f;
    float sum = 0;
    for (const Item& i : its) sum += i.f.w + gap;
    const float R = clampf(sum / 2.4f, 1.9f, 6.0f);
    float cur = -sum / R * 0.5f;
    for (const Item& i : its) {
        const float span = (i.f.w + gap) / R, th = cur + span * 0.5f;
        cur += span;
        Vec3 c = eye + (fl * std::cos(th) + rt * std::sin(th)) * R;
        c.y = eye.y - 0.05f;
        const Vec3 to = eye - c;
        winGlideTo(i.w, c, std::atan2(to.x, to.z), i.f.ww);
    }
}

// A grid of equally sized cells on a cylinder round the viewer; each window is scaled to fit its cell.
void AppShell::arrangeTile() {
    const Vec3 eye = viewEye(), fl = viewFlat(), rt{-fl.z, 0, fl.x};
    std::vector<Item> its;
    for (const WinRef& w : arrangeable()) {
        Item i{w, {}, 0};
        if (!winFrameOf(w, i.f)) continue;
        const Vec3 d = i.f.c - eye;
        i.a = std::atan2(dot(d, rt), dot(d, fl));
        its.push_back(i);
    }
    const int n = static_cast<int>(its.size());
    if (n == 0) return;
    std::sort(its.begin(), its.end(), [](const Item& a, const Item& b) { return a.a < b.a; });
    const int cols = n <= 3 ? n : (n == 4 ? 2 : (n <= 9 ? 3 : static_cast<int>(std::ceil(std::sqrt(static_cast<float>(n))))));
    const int rows = (n + cols - 1) / cols;
    const float cellW = 1.9f, cellH = 1.25f, g = 0.22f;
    const float R = std::max(2.3f, cols * (cellW + g) / 2.6f);
    for (int idx = 0; idx < n; ++idx) {
        const int r = idx / cols, c = idx % cols, inRow = std::min(cols, n - r * cols);
        const float off = static_cast<float>(c) - (inRow - 1) * 0.5f;
        const float th = off * (cellW + g) / R;
        Vec3 p = eye + (fl * std::cos(th) + rt * std::sin(th)) * R;
        p.y = eye.y - 0.05f + ((rows - 1) * 0.5f - r) * (cellH + g);
        const Item& it = its[idx];
        const float s = std::min(cellW / it.f.w, cellH / it.f.h);
        const Vec3 to = Vec3{eye.x, p.y, eye.z} - p;
        winGlideTo(it.w, p, std::atan2(to.x, to.z), clampf(it.f.ww * s, 0.7f, 5.0f));
    }
}

void AppShell::arrangeCycle() {
    if (arrangeNext_ == 0) arrangeArc();
    else arrangeTile();
    arrangeNext_ ^= 1;
}

void AppShell::minimiseAll() {
    const std::vector<WinRef> v = winOrder_;
    for (const WinRef& w : v) winHide(w);
}

void AppShell::scriptWin(const std::string& arg) {
    if (arg == "arc") arrangeArc();
    else if (arg == "tile") arrangeTile();
    else if (arg == "min") minimiseAll();
}

// Remembers where every window sits relative to the viewer; when the viewer is moved somewhere else (city <-> lobby <-> room)
// the windows are glided to the same place relative to them, so they are always within reach.
void AppShell::winTick(float dt) {
    winAnimStep(dt);
    const Vec3 eye = viewEye(), fl = viewFlat(), rt{-fl.z, 0, fl.x};
    const std::string env = std::string(cityMode_ ? "c" : "r") + (room_->inRoom() ? "i" : "o") + (room_->building() ? "b" : "-") +
                            room_->currentPath();
    const bool jumped = envKnown_ && length(eye - lastEye_) > 25.0f && !riding_ && !tourActive_;
    const bool changed = envKnown_ && env != lastEnv_;
    lastEnv_ = env;
    lastEye_ = eye;
    envKnown_ = true;
    if (!settings_.carryWins) {
        carryTimer_ = -1.0f;
        return;
    }
    if (changed || jumped) {
        carryTimer_ = 0.35f;  // the camera may land a moment after the environment changes
        return;
    }
    if (carryTimer_ >= 0.0f) {
        carryTimer_ -= dt;
        if (carryTimer_ < 0.0f) {
            carryTimer_ = -1.0f;
            winCarryApply(eye, fl);
        }
        return;
    }
    if (!winAnims_.empty()) return;
    winRel_.clear();
    relEye_ = eye;
    for (const WinRef& w : winOrder_) {
        WinFrame f;
        if (!winOpen(w) || !winFrameOf(w, f)) continue;
        const Vec3 d = f.c - eye;
        WinRel r;
        r.w = w;
        r.bearing = std::atan2(dot(d, rt), dot(d, fl));
        r.dist = std::sqrt(d.x * d.x + d.z * d.z);
        r.dy = d.y;
        winRel_.push_back(r);
    }
}

void AppShell::winCarryApply(const Vec3& eye, const Vec3& fl) {
    if (length(eye - relEye_) < 3.0f) return;  // the viewer did not really go anywhere
    const Vec3 rt{-fl.z, 0, fl.x};
    struct C {
        WinRef w;
        float w0, b, d, dy;
    };
    std::vector<C> cs;
    for (const WinRel& r : winRel_) {
        WinFrame f;
        if (!winOpen(r.w) || !winFrameOf(r.w, f)) continue;
        cs.push_back({r.w, f.ww, clampf(r.bearing, -1.75f, 1.75f), clampf(r.dist, 1.6f, 3.0f), clampf(r.dy, -0.7f, 0.7f)});
    }
    std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.b < b.b; });
    for (size_t i = 1; i < cs.size(); ++i) {
        const float sep = ((cs[i - 1].w0 + cs[i].w0) * 0.5f + 0.12f) / ((cs[i - 1].d + cs[i].d) * 0.5f);
        if (cs[i].b < cs[i - 1].b + sep) cs[i].b = cs[i - 1].b + sep;
    }
    if (!cs.empty()) {
        const float shift = (cs.front().b + cs.back().b) * 0.5f;
        for (C& c : cs) c.b -= shift * 0.5f;
    }
    for (const C& c : cs) {
        Vec3 p = eye + (fl * std::cos(c.b) + rt * std::sin(c.b)) * c.d;
        p.y = eye.y + c.dy;
        const Vec3 to = eye - p;
        winGlideTo(c.w, p, std::atan2(to.x, to.z), c.w0);
    }
}

// Pulls the window being dragged onto the edges, height and plane of its neighbours (stateless: the drag re-aims each frame).
void AppShell::snapDragged(WinRef w) {
    if (!settings_.snapWins) return;
    WinFrame f;
    if (!winFrameOf(w, f)) return;
    float yaw = f.yaw;
    Vec3 c = f.c;
    for (const WinRef& o : winOrder_) {
        WinFrame g;
        if (o == w || !winOpen(o) || !winFrameOf(o, g)) continue;
        if (length(g.c - f.c) < 8.0f && std::fabs(wrapAngle(g.yaw - yaw)) < 0.14f) yaw = g.yaw;
    }
    const Vec3 right = rightOf(yaw), n{std::sin(yaw), 0, std::cos(yaw)};
    float bestX = 0.15f, bestY = 0.15f, bestZ = 0.25f, sx = 0, sy = 0, sz = 0;
    bool hx = false, hy = false, hz = false;
    for (const WinRef& o : winOrder_) {
        WinFrame g;
        if (o == w || !winOpen(o) || !winFrameOf(o, g)) continue;
        if (std::fabs(wrapAngle(g.yaw - yaw)) > 0.02f) continue;
        const Vec3 d = g.c - c;
        const float lx = dot(d, right), ly = d.y, lz = dot(d, n);
        if (std::fabs(lz) > 1.0f) continue;
        const bool overlapY = std::fabs(ly) < (g.h + f.h) * 0.5f, overlapX = std::fabs(lx) < (g.w + f.w) * 0.5f + 0.1f;
        if (std::fabs(lz) < bestZ) { bestZ = std::fabs(lz); sz = lz; hz = true; }
        if (overlapY || std::fabs(lx) < (g.w + f.w) * 0.5f + 0.3f) {
            const float e = (g.w + f.w) * 0.5f + 0.02f;
            for (float t : {e, -e, 0.0f}) {
                const float s = lx - t;
                if (std::fabs(s) < bestX) { bestX = std::fabs(s); sx = s; hx = true; }
            }
        }
        if (overlapX) {
            const float e = (g.h + f.h) * 0.5f + 0.02f, top = (g.h - f.h) * 0.5f;
            for (float t : {e, -e, 0.0f, top, -top}) {
                const float s = ly - t;
                if (std::fabs(s) < bestY) { bestY = std::fabs(s); sy = s; hy = true; }
            }
        }
    }
    if (hx) c = c + right * sx;
    if (hy) c.y += sy;
    if (hz) c = c + n * sz;
    if (!hy) {
        const float eyeY = viewEye().y - 0.05f;
        if (std::fabs(c.y - eyeY) < 0.12f) c.y = eyeY;
    }
    if (length(c - f.c) > 1e-4f || std::fabs(yaw - f.yaw) > 1e-4f) winMoveTo(w, c, yaw, f.ww);
}

// Handle geometry: a small bracket just outside the bottom right corner of the window frame.
static void handleGeometry(const AppShell::WinFrame& f, Vec3& centre, Vec3& right) {
    right = rightOf(f.yaw);
    const Vec3 corner = f.c + right * (f.w * 0.5f + 0.03f) + Vec3{0, -(f.h * 0.5f + 0.03f), 0};
    centre = corner + right * (-kHandle * 0.5f + 0.04f) + Vec3{0, kHandle * 0.5f - 0.04f, 0};
}

bool AppShell::handleAt(WinRef w, const Vec3& o, const Vec3& d, float& t) {
    WinFrame f;
    if (!winFrameOf(w, f)) return false;
    Vec3 hc, right;
    handleGeometry(f, hc, right);
    const float dn = dot(d, f.n);
    if (std::fabs(dn) < 1e-4f) return false;
    t = dot(hc - o, f.n) / dn;
    if (t <= 0) return false;
    const Vec3 p = o + d * t - hc;
    const float lim = kHandle * 0.7f;
    return std::fabs(dot(p, right)) <= lim && std::fabs(p.y) <= lim;
}

void AppShell::resizeStep(const Vec3& o, const Vec3& d) {
    WinFrame f;
    if (!winFrameOf(sizing_, f)) {
        sizing_ = {};
        return;
    }
    const Vec3 right = rightOf(sizeYaw_), n{std::sin(sizeYaw_), 0, std::cos(sizeYaw_)};
    const float dn = dot(d, n);
    if (std::fabs(dn) < 1e-4f) return;
    const float t = dot(sizeAnchor_ - o, n) / dn;
    if (t <= 0) return;
    const Vec3 p = o + d * t - sizeAnchor_;
    const float s = (dot(p, right) * sizeFw0_ - p.y * sizeFh0_) / (sizeFw0_ * sizeFw0_ + sizeFh0_ * sizeFh0_) / sizeK_;
    const float W = clampf(sizeW0_ * s, 0.7f, 5.0f), k = W / sizeW0_;
    const Vec3 c = sizeAnchor_ + right * (sizeFw0_ * k * 0.5f) + Vec3{0, -sizeFh0_ * k * 0.5f, 0};
    winMoveTo(sizing_, c, sizeYaw_, W);
}

// Returns true when the click was taken by a resize handle.
bool AppShell::resizeInput(const Vec3& o, const Vec3& d, bool clickEv, bool mouseClick, bool clickUp, bool blocked) {
    if (sizing_.p) {
        if (!winOpen(sizing_) || blocked) {
            sizing_ = {};
            return false;
        }
        resizeStep(o, d);
        if (sizingHold_) {
            if (clickUp) sizing_ = {};
            return clickEv;
        }
        if (clickEv) {
            sizing_ = {};
            return true;
        }
        return false;
    }
    handleHover_ = {};
    if (blocked) return false;
    // The handle of the front-most window under the aim, unless another window covers it.
    WinRef top;
    float topD = -1;
    for (size_t i = winOrder_.size(); i-- > 0;) {
        const float hd = winHit(winOrder_[i], o, d);
        if (hd >= 0) { top = winOrder_[i]; topD = hd; break; }
    }
    for (size_t i = winOrder_.size(); i-- > 0;) {
        float t = 0;
        if (!handleAt(winOrder_[i], o, d, t)) continue;
        if (top.p && !(top == winOrder_[i]) && topD >= 0 && topD < t - 0.05f) continue;
        handleHover_ = winOrder_[i];
        break;
    }
    if (handleHover_.p && clickEv) {
        WinFrame f;
        if (!winFrameOf(handleHover_, f)) return false;
        sizing_ = handleHover_;
        raiseFocus(sizing_);
        sizingHold_ = mouseClick;
        sizeYaw_ = f.yaw;
        sizeW0_ = f.ww;
        sizeFw0_ = f.w;
        sizeFh0_ = f.h;
        sizeAnchor_ = f.c + rightOf(f.yaw) * (-f.w * 0.5f) + Vec3{0, f.h * 0.5f, 0};
        // Where the pointer sits relative to the corner now, so the window does not jump when the grab starts.
        sizeK_ = 1.0f;
        const Vec3 n = f.n;
        const float dn = dot(d, n);
        if (std::fabs(dn) > 1e-4f) {
            const float t = dot(sizeAnchor_ - o, n) / dn;
            if (t > 0) {
                const Vec3 p = o + d * t - sizeAnchor_;
                const float k = (dot(p, rightOf(f.yaw)) * f.w - p.y * f.h) / (f.w * f.w + f.h * f.h);
                if (k > 0.3f) sizeK_ = k;
            }
        }
        return true;
    }
    return false;
}

void AppShell::renderHandle(WinRef w) {
    if (!(w == focus_ || w == handleHover_ || w == sizing_)) return;
    WinFrame f;
    if (!winFrameOf(w, f)) return;
    if (!handleLabel_) {
        Bitmap bm;
        bm.w = bm.h = 96;
        bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
        const Color8 c = ui::tokens().accent;
        ui::line(bm, 88, 22, 88, 88, 9, c);
        ui::line(bm, 22, 88, 88, 88, 9, c);
        ui::line(bm, 88, 50, 50, 88, 6, c);
        handleLabel_ = renderer_.createLabel(bm);
    }
    Vec3 hc, right;
    handleGeometry(f, hc, right);
    const bool hot = w == handleHover_ || w == sizing_;
    const float uv[4] = {0, 0, 1, 1}, white[3] = {1, 1, 1};
    renderer_.drawWebRegion(*handleLabel_, Mat4::translate(hc + f.n * 0.01f) * Mat4::rotateY(f.yaw) * Mat4::scale({kHandle, kHandle, 1}),
                            uv, 1.0f, 0.0f, 0.0f, hot ? 1.0f : 0.4f, white);
}

}  // namespace sf
