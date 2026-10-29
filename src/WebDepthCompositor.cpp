#include "WebDepthCompositor.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace sf {

namespace {
// Depth of each band in front of the page plane (metres at "Subtle"). Everything stays well inside +-10 cm even at
// Cinematic so the effect is comfortable in a headset.
constexpr float kBandZ[kWebBandCount] = {0.0f, 0.018f, 0.034f, 0.055f};
constexpr float kMaxZ = 0.095f;

std::filesystem::path settingsPath() {
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base || !*base) base = std::getenv("HOME");
    std::filesystem::path dir = std::filesystem::path(base ? base : ".") / "SciFiShell";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "webdepth.ini";
}
}  // namespace

const char* depthModeName(DepthMode m) {
    switch (m) {
        case DepthMode::Off: return "OFF";
        case DepthMode::Subtle: return "SUBTLE";
        case DepthMode::Cinematic: return "CINEMATIC";
    }
    return "OFF";
}

WebDepthCompositor::WebDepthCompositor() { loadSettings(); }

void WebDepthCompositor::loadSettings() {
    std::ifstream in(settingsPath());
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 5, "mode=") == 0) {
            std::string v = line.substr(5);
            mode_ = v == "off" ? DepthMode::Off : (v == "cinematic" ? DepthMode::Cinematic : DepthMode::Subtle);
        } else if (line.compare(0, 8, "disable=") == 0) {
            std::string h = line.substr(8);
            while (!h.empty() && (h.back() == '\r' || h.back() == ' ')) h.pop_back();
            if (!h.empty()) disabledHosts_.push_back(h);
        }
    }
}

void WebDepthCompositor::saveSettings() const {
    std::ofstream out(settingsPath(), std::ios::trunc);
    out << "mode=" << (mode_ == DepthMode::Off ? "off" : (mode_ == DepthMode::Cinematic ? "cinematic" : "subtle")) << "\n";
    for (const auto& h : disabledHosts_) out << "disable=" << h << "\n";
}

void WebDepthCompositor::setMode(DepthMode m) {
    mode_ = m;
    saveSettings();
}

void WebDepthCompositor::cycleMode() {
    setMode(static_cast<DepthMode>((static_cast<int>(mode_) + 1) % 3));
}

void WebDepthCompositor::setHost(const std::string& host) {
    host_ = host;
    hostDisabled_ = !host_.empty() && std::find(disabledHosts_.begin(), disabledHosts_.end(), host_) != disabledHosts_.end();
}

bool WebDepthCompositor::toggleHostDisabled() {
    if (host_.empty()) return hostDisabled_;
    auto it = std::find(disabledHosts_.begin(), disabledHosts_.end(), host_);
    if (it == disabledHosts_.end()) {
        disabledHosts_.push_back(host_);
        hostDisabled_ = true;
    } else {
        disabledHosts_.erase(it);
        hostDisabled_ = false;
    }
    saveSettings();
    return hostDisabled_;
}

std::string WebDepthCompositor::statusText() const {
    std::string s = std::string("DEPTH ") + depthModeName(mode_);
    if (hostDisabled_) s += " (SITE OFF)";
    else if (fallback_ && mode_ != DepthMode::Off) s += " (SIMPLE)";
    else if (vrReduced_) s += " (REDUCED)";
    return s;
}

void WebDepthCompositor::update(double dt, const WebLayout& layout, float pointerU, float pointerV, bool vr, float frameMs,
                                bool fresh) {
    const float fdt = static_cast<float>(dt);
    vr_ = vr;
    // Fallback: analysis was too slow or the page too complex -> just the single page plane.
    fallback_ = layout.tooComplex;

    // VR performance guard: a sustained slow frame rate halves the depth and drops the extra cards.
    avgFrameMs_ = lerpf(avgFrameMs_, frameMs, clampf(fdt * 2.0f, 0.0f, 1.0f));
    if (vr) {
        if (avgFrameMs_ > 14.0f) {
            vrReduced_ = true;
            calmTime_ = 0;
        } else if (vrReduced_ && avgFrameMs_ < 11.0f) {
            calmTime_ += fdt;
            if (calmTime_ > 5.0f) vrReduced_ = false;
        }
    } else {
        vrReduced_ = false;
    }

    float target = 0;
    if (mode_ != DepthMode::Off && !hostDisabled_) target = mode_ == DepthMode::Cinematic ? 2.0f : 1.0f;
    if (vrReduced_) target *= 0.5f;
    depthScale_ = lerpf(depthScale_, target, clampf(fdt * 6.0f, 0.0f, 1.0f));
    freshness_ = lerpf(freshness_, fresh && !layout.elements.empty() ? 1.0f : 0.0f, clampf(fdt * (fresh ? 4.0f : 14.0f), 0.0f, 1.0f));
    eff_ = depthScale_ * freshness_;
    px_ = lerpf(px_, pointerU, clampf(fdt * 6.0f, 0.0f, 1.0f));
    py_ = lerpf(py_, pointerV, clampf(fdt * 6.0f, 0.0f, 1.0f));

    maxCards_ = mode_ == DepthMode::Cinematic ? 16 : 8;
    maxMedia_ = mode_ == DepthMode::Cinematic ? 20 : 12;
}

void WebDepthCompositor::render(Renderer& r, const GpuLabel& page, const Mat4& pageBase, float worldW, float worldH,
                                int texW, int texH, const WebLayout& layout, double time) const {
    // Base plane: the full page. Everything below is extra geometry layered around it.
    r.drawImage(page, pageBase * Mat4::scale({worldW, worldH, 0.002f}), 1.0f);
    pickList_.clear();
    if (eff_ < 0.02f || fallback_ || hostDisabled_ || layout.elements.empty()) return;

    struct Item {
        const WebElement* e;
        float z;
        int index;
    };
    std::vector<Item> items;
    int cards = 0, media = 0;
    const int cardBudget = vrReduced_ ? 0 : std::min(maxCards_, WebLayerAnalyzer::kMaxCards);
    const int mediaBudget = vrReduced_ ? maxMedia_ / 2 : maxMedia_;
    int index = 0;
    for (const WebElement& e : layout.elements) {
        ++index;
        if (e.kind == WebElementKind::Card) {
            if (cards++ >= cardBudget) continue;
        } else if (media++ >= mediaBudget) {
            continue;
        }
        float extra = e.kind == WebElementKind::Video ? 0.012f : (e.kind == WebElementKind::Image ? 0.006f : 0.0f);
        if (vr_ && e.kind != WebElementKind::Card)  // tiny "float" in VR
            extra += 0.003f * std::sin(static_cast<float>(time) * 0.8f + static_cast<float>(index));
        // A flat monitor needs a much bigger offset than a headset to read as depth, so desktop gets 4x.
        const float boost = vr_ ? 1.0f : 4.0f;
        float z = std::min((kBandZ[e.band] + extra) * eff_ * boost, kMaxZ * boost);
        items.push_back({&e, std::max(z, 0.004f * eff_ * boost), index});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.z < b.z; });
    // Wrappers that are (nearly) the same box as a raised child (e.g. <a>/<figure> around an <img>) would show twice.
    items.erase(std::remove_if(items.begin(), items.end(),
                               [&](const Item& a) {
                                   for (const Item& b : items) {
                                       if (&a == &b || b.z <= a.z) continue;
                                       const WebElement &p = *a.e, &c = *b.e;
                                       if (c.x < p.x - 6 || c.y < p.y - 6 || c.x + c.w > p.x + p.w + 6 || c.y + c.h > p.y + p.h + 6) continue;
                                       if (p.w * p.h < c.w * c.h * 1.35f && p.kind != WebElementKind::Card) return true;
                                   }
                                   return false;
                               }),
                items.end());

    const float shadowCol[3] = {0.0f, 0.0f, 0.02f};
    const float glowCol[3] = {0.35f, 0.62f, 1.0f};
    const float rimCol[3] = {0.8f, 0.9f, 1.0f};
    const float strength = clampf(eff_, 0.0f, 2.0f);

    // Hide the original content under each raised layer by painting page-background over its footprint on the base
    // plane, so the page shows "holes" where the floating copies are instead of a doubled-up duplicate.
    {
        const float bgUv[4] = {0.003f, 0.6f, 0.001f, 0.001f};
        const float fill = clampf(eff_ * 2.0f, 0.0f, 1.0f);
        for (const Item& it : items) {
            const WebElement& e = *it.e;
            const float cx = ((e.x + e.w * 0.5f) / texW - 0.5f) * worldW;
            const float cy = (0.5f - (e.y + e.h * 0.5f) / texH) * worldH;
            const float sw = e.w / texW * worldW, sh = e.h / texH * worldH;
            r.drawWebRegion(page, pageBase * Mat4::translate({cx, cy, 0.0025f}) * Mat4::scale({sw, sh, 1}), bgUv, sw / sh,
                            0.0f, 0.0f, fill, rimCol);
        }
    }

    // Shadows first (behind everything extra), then the raised regions, both back to front.
    for (int pass = 0; pass < 2; ++pass) {
        for (const Item& it : items) {
            const WebElement& e = *it.e;
            const float sw = e.w / texW * worldW, sh = e.h / texH * worldH;
            // Desktop pointer parallax: higher bands slide a little further against the pointer.
            float sx = 0, sy = 0;
            if (!vr_) {
                sx = -px_ * 0.030f * strength * static_cast<float>(e.band + 1) * 0.5f;
                sy = -py_ * 0.030f * strength * static_cast<float>(e.band + 1) * 0.5f;
            }
            const float cx = ((e.x + e.w * 0.5f) / texW - 0.5f) * worldW + sx;
            const float cy = (0.5f - (e.y + e.h * 0.5f) / texH) * worldH + sy;
            if (pass == 0) {
                const float m = std::min(sw, sh) * (e.kind == WebElementKind::Video ? 0.22f : 0.16f) * (0.6f + 0.4f * strength);
                const float qw = sw + 2 * m, qh = sh + 2 * m;
                const bool glow = e.kind == WebElementKind::Video;
                const float intensity = (glow ? 0.45f : (e.kind == WebElementKind::Card ? 0.32f : 0.26f)) * std::min(strength, 1.5f);
                r.drawWebShadow(page, pageBase * Mat4::translate({cx, cy - (glow ? 0.0f : m * 0.25f), it.z * 0.35f}) *
                                          Mat4::scale({qw, qh, 1}),
                                glow ? glowCol : shadowCol, intensity, sw / qw, sh / qh);
            } else {
                pickList_.push_back({cx, cy, it.z, sw, sh, e.x, e.y, e.w, e.h});
                // Nested layers (an image inside a card): the parent's copy already contains this content, so blank it
                // out there; otherwise the same picture shows twice.
                for (const Item& o : items) {
                    const WebElement& p = *o.e;
                    if (&p == &e || o.z >= it.z || p.w * p.h <= e.w * e.h) continue;
                    if (e.x < p.x - 2 || e.y < p.y - 2 || e.x + e.w > p.x + p.w + 2 || e.y + e.h > p.y + p.h + 2) continue;
                    const float bgUv[4] = {0.003f, 0.6f, 0.001f, 0.001f};
                    // Place the patch where the parent's copy (with the parent's parallax) shows this child.
                    const float pk = static_cast<float>(p.band + 1) * 0.5f * 0.030f * strength;
                    const float pcx = ((e.x + e.w * 0.5f) / texW - 0.5f) * worldW - (vr_ ? 0.0f : px_ * pk);
                    const float pcy = (0.5f - (e.y + e.h * 0.5f) / texH) * worldH - (vr_ ? 0.0f : py_ * pk);
                    r.drawWebRegion(page, pageBase * Mat4::translate({pcx, pcy, std::min(o.z + 0.0008f, (o.z + it.z) * 0.5f)}) * Mat4::scale({sw, sh, 1}), bgUv,
                                    sw / sh, 0.0f, 0.0f, clampf(eff_ * 2.0f, 0.0f, 1.0f), rimCol);
                    break;
                }
                const float uv[4] = {e.x / texW, e.y / texH, e.w / texW, e.h / texH};
                const float radius = std::min(std::max(e.radius / std::max(e.h, 1.0f), e.kind == WebElementKind::Card ? 0.0f : 0.012f), 0.5f);
                const float rim = e.kind == WebElementKind::Card ? 0.25f : (e.kind == WebElementKind::Video ? 0.7f : 0.5f);
                r.drawWebRegion(page, pageBase * Mat4::translate({cx, cy, it.z}) * Mat4::scale({sw, sh, 1}), uv, sw / sh,
                                radius, rim * std::min(strength, 1.5f), 1.0f,
                                e.kind == WebElementKind::Video ? glowCol : rimCol);
            }
        }
    }
}

bool WebDepthCompositor::pick(Vec3 o, Vec3 d, float& viewX, float& viewY) const {
    if (std::fabs(d.z) < 1e-5f) return false;
    if (freshness_ < 0.95f || eff_ < 0.3f) return false;  // layout may not match the page (scrolling): use the base plane
    for (size_t i = pickList_.size(); i-- > 0;) {
        const PickItem& p = pickList_[i];
        const float t = (p.z - o.z) / d.z;
        if (t <= 0) continue;
        const float x = o.x + d.x * t, y = o.y + d.y * t;
        if (std::fabs(x - p.cx) > p.sw * 0.5f || std::fabs(y - p.cy) > p.sh * 0.5f) continue;
        viewX = p.ex + ((x - p.cx) / p.sw + 0.5f) * p.ew;
        viewY = p.ey + (0.5f - (y - p.cy) / p.sh) * p.eh;
        return true;
    }
    return false;
}

}  // namespace sf
