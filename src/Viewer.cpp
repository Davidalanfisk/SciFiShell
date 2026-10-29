#include "Viewer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <filesystem>

#include "TextRender.h"
#include "stb_image.h"

namespace sf {

namespace {
namespace fs = std::filesystem;

// One shared volume for all audio/video, remembered across files and sessions.
fs::path volumeFile() {
    const char* base = std::getenv("LOCALAPPDATA");
    if (!base || !*base) return {};
    return fs::path(base) / "SciFiShell" / "volume.ini";
}
float loadVolume() {
    auto p = volumeFile();
    float v = 0.8f;
    if (!p.empty()) {
        std::ifstream f(p);
        if (!(f >> v)) v = 0.8f;
    }
    // A remembered "muted" volume made every later file silent with no hint why, so a near-zero value restarts at the default.
    if (v < 0.05f) v = 0.8f;
    return std::min(1.0f, std::max(0.0f, v));
}
void saveVolume(float v) {
    auto p = volumeFile();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p) << v;
}

constexpr int kBarPxW = 1024, kBarPxH = 96;
constexpr int kPlayX0 = 0, kPlayX1 = 96, kSeekX0 = 116, kSeekX1 = 470, kTimeX0 = 484, kVolX0 = 690, kPrevX0 = 776,
              kNextX0 = 820, kFsX0 = 864, kFsX1 = 944, kCloseX0 = 944, kCloseX1 = 1024;
constexpr int kDetPxW = 600, kDetPxH = 760;

struct Rgb {
    float r, g, b;
};
constexpr Rgb kBarBg{0.07f, 0.09f, 0.15f}, kAccent{0.42f, 0.62f, 1.0f}, kText{0.93f, 0.95f, 0.98f},
    kDim{0.55f, 0.60f, 0.72f}, kTrack{0.20f, 0.24f, 0.36f};

Color8 toColor8(Rgb c, float a = 1.0f) {
    return Color8{static_cast<uint8_t>(c.r * 255.0f + 0.5f), static_cast<uint8_t>(c.g * 255.0f + 0.5f),
                  static_cast<uint8_t>(c.b * 255.0f + 0.5f), static_cast<uint8_t>(a * 255.0f + 0.5f)};
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

void fillBitmap(Bitmap& bm, int w, int h, Rgb c) {
    bm.w = w;
    bm.h = h;
    bm.rgba.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < bm.rgba.size(); i += 4) {
        bm.rgba[i] = static_cast<uint8_t>(c.r * 255.0f + 0.5f);
        bm.rgba[i + 1] = static_cast<uint8_t>(c.g * 255.0f + 0.5f);
        bm.rgba[i + 2] = static_cast<uint8_t>(c.b * 255.0f + 0.5f);
        bm.rgba[i + 3] = 255;
    }
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

void fillCircle(Bitmap& bm, float cx, float cy, float r, Rgb c) { fillRoundRect(bm, cx - r, cy - r, cx + r, cy + r, r, c); }

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

void fillTriangle(Bitmap& bm, float ax, float ay, float bx, float by, float cx, float cy, Rgb c) {
    auto edge = [](float x0, float y0, float x1, float y1, float px, float py) {
        return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    };
    for (int y = static_cast<int>(std::min({ay, by, cy})) - 1; y <= static_cast<int>(std::max({ay, by, cy})) + 1; ++y)
        for (int x = static_cast<int>(std::min({ax, bx, cx})) - 1; x <= static_cast<int>(std::max({ax, bx, cx})) + 1; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float px = x + (sx + 0.5f) / 4.0f, py = y + (sy + 0.5f) / 4.0f;
                    float e0 = edge(ax, ay, bx, by, px, py), e1 = edge(bx, by, cx, cy, px, py), e2 = edge(cx, cy, ax, ay, px, py);
                    if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) ++hits;
                }
            putPixel(bm, x, y, c, hits / 16.0f);
        }
}

std::string formatTime(double s) {
    if (s < 0) s = 0;
    int total = static_cast<int>(s);
    char buf[32];
    if (total >= 3600) std::snprintf(buf, sizeof buf, "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    else std::snprintf(buf, sizeof buf, "%d:%02d", total / 60, total % 60);
    return buf;
}

std::string formatSize(uintmax_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[48];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

std::string formatModified(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return "Unknown";
    auto sys = std::chrono::system_clock::now() +
               std::chrono::duration_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, "%d %b %Y, %H:%M", &tmv);
    return buf;
}

// Full-resolution image decode, box-downscaled so the longest side is at most maxSide.
bool decodeImage(const std::string& path, Bitmap& out, int maxSide) {
    std::error_code ec;
    auto size = fs::file_size(fs::u8path(path), ec);
    if (ec || size > 256u * 1024 * 1024) return false;
    FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(fs::u8path(path).wstring().c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    std::vector<unsigned char> data(static_cast<size_t>(size));
    size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) return false;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &comp, 4);
    if (!px) return false;
    int factor = std::max(1, (std::max(w, h) + maxSide - 1) / maxSide);
    int nw = std::max(1, w / factor), nh = std::max(1, h / factor);
    out.w = nw;
    out.h = nh;
    out.rgba.resize(static_cast<size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            int acc[3] = {0, 0, 0}, n = 0;
            for (int dy = 0; dy < factor; ++dy)
                for (int dx = 0; dx < factor; ++dx) {
                    int sx = std::min(x * factor + dx, w - 1), sy = std::min(y * factor + dy, h - 1);
                    const unsigned char* p = px + (static_cast<size_t>(sy) * w + sx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
                    ++n;
                }
            uint8_t* o = &out.rgba[(static_cast<size_t>(y) * nw + x) * 4];
            o[0] = static_cast<uint8_t>(acc[0] / n);
            o[1] = static_cast<uint8_t>(acc[1] / n);
            o[2] = static_cast<uint8_t>(acc[2] / n);
            o[3] = 255;
        }
    stbi_image_free(px);
    return true;
}

float smooth(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float wrapAngle(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
}  // namespace

class Viewer::LabelNode : public Node {
public:
    void render(Renderer& r) override {
        const Mat4 w = worldMatrix();
        if (glass) {
            r.drawGlass(w, scale.x / scale.y, 0.022f / scale.y);
            return;
        }
        if (!label) return;
        const float white[3] = {1, 1, 1};
        r.drawWebRegion(*label, w, uv, scale.x / scale.y, radius > 0 ? radius / scale.y : 0.0f, 0.0f, brightness, white);
    }
    std::shared_ptr<GpuLabel> label;
    float brightness = 1.0f;
    bool glass = false;
    float radius = 0.014f;
    float uv[4] = {0, 0, 1, 1};
};

Viewer::Viewer(Renderer& renderer) : renderer_(renderer) {
    rootHolder_ = std::make_unique<Node>();
    root_ = rootHolder_.get();
    auto fr = std::make_unique<LabelNode>();
    fr->glass = true;
    frame_ = static_cast<LabelNode*>(root_->add(std::move(fr)));
    title_ = static_cast<LabelNode*>(root_->add(std::make_unique<LabelNode>()));
    auto s = std::make_unique<LabelNode>();
    s->pickable = true;
    surface_ = static_cast<LabelNode*>(root_->add(std::move(s)));
    auto b = std::make_unique<LabelNode>();
    b->pickable = true;
    bar_ = static_cast<LabelNode*>(root_->add(std::move(b)));
    details_ = static_cast<LabelNode*>(root_->add(std::make_unique<LabelNode>()));
    root_->visible = false;
}

Viewer::~Viewer() = default;

bool Viewer::open(const OpenRequest& req, const HeadPose& head) {
    if (active_) {
        // Opening another file while one is showing swaps it in place.
        if (closing_ || !prepareSwitch()) return false;
    }
    const bool keepFs = active_ == false && fullscreen_ && switching_;  // next/previous keeps fullscreen
    switching_ = false;
    req_ = req;
    wantsClose_ = closing_ = false;
    fullscreen_ = keepFs;
    zoom_ = 1.0f;
    uvOx_ = uvOy_ = 0;
    uvS_ = 1.0f;
    panX_ = panY_ = 0;
    idle_ = 0;
    haveVideoFrame_ = false;
    videoLabel_.reset();
    imageLabel_.reset();
    drawnPos_ = -1;
    barTimer_ = 0;

    poster_ = req.poster;
    if (!poster_) {
        Bitmap pb;
        fillBitmap(pb, 384, 248, {0.08f, 0.10f, 0.17f});
        poster_ = renderer_.createLabel(pb, true);
    }
    mediaAspect_ = poster_->aspect;
    isMedia_ = false;
    isText_ = editing_ = discardArmed_ = false;
    textLabel_.reset();
    status_.clear();
    drawnSig_.clear();
    caretTimer_ = 0;

    if (req.kind == FileKind::Text) {
        if (editor_.load(req.path)) {
            isText_ = true;
            textLabel_ = renderer_.createDynamicLabel(CodeEditor::kPxW, CodeEditor::kPxH);
            mediaAspect_ = static_cast<float>(CodeEditor::kPxW) / CodeEditor::kPxH;
            if (editor_.truncated()) setStatus("Large file - opened read only", 1e9);
            else if (editor_.readOnly()) setStatus("Binary file - opened read only", 1e9);
            redrawText(false);
        }
    }

    if (req.kind == FileKind::Image) {
        Bitmap bm;
        if (decodeImage(req.path, bm, 4096)) {
            imageLabel_ = renderer_.createLabel(bm, true);
            mediaAspect_ = static_cast<float>(bm.w) / static_cast<float>(bm.h);
        }
    } else if (req.kind == FileKind::Video || req.kind == FileKind::Audio) {
        if (player_.open(req.path)) {
            isMedia_ = true;
            if (player_.hasVideo() && player_.videoWidth() > 0 && player_.videoHeight() > 0) {
                videoLabel_ = renderer_.createDynamicLabel(player_.videoWidth(), player_.videoHeight());
                mediaAspect_ = static_cast<float>(player_.videoWidth()) / static_cast<float>(player_.videoHeight());
            }
            player_.setVolume(loadVolume());
            player_.play();
        }
    }
    mediaAspect_ = clampf(mediaAspect_, 0.4f, 3.0f);

    surface_->label = textLabel_ ? textLabel_ : imageLabel_ ? imageLabel_ : poster_;
    barLabel_ = renderer_.createDynamicLabel(kBarPxW, kBarPxH);
    bar_->label = barLabel_;
    buildDetails();
    details_->label = detailsLabel_;

    lastHead_ = head;
    startPos_ = req.cardPos;
    startYaw_ = req.cardYaw;
    layout(head);
    startScale_ = std::max(0.05f, req.cardScale.x / surfW_);
    t_ = keepFs ? 1.0f : 0.0f;
    active_ = true;
    root_->visible = true;
    drawBar();
    applyTransform(keepFs ? 1.0f : 0.0f);
    return true;
}

void Viewer::close() {
    if (!active_ || closing_) return;
    wantsClose_ = false;
    if (isText_ && editor_.dirty() && !discardArmed_) {
        discardArmed_ = true;
        setStatus("Unsaved changes - close again to discard", 6.0);
        return;
    }
    closing_ = true;
    fullscreen_ = false;
    editing_ = false;
    if (isMedia_) player_.pause();
    layout(lastHead_);
}

void Viewer::buildDetails() {
    Bitmap bm;
    fillBitmap(bm, kDetPxW, kDetPxH, {0.07f, 0.09f, 0.15f});
    fillRoundRect(bm, 0, 0, kDetPxW, 6, 0, kAccent);
    float y = 40;
    auto line = [&](const std::string& text, float px, FontFace face, Rgb c, float x = 36.0f) {
        y += textAscent(px, face);
        drawText(bm, x, y, text, px, face, toColor8(c));
        y += textLineHeight(px, face) - textAscent(px, face);
    };
    std::vector<std::string> nameLines = wrapText(req_.name, 42.0f, FontFace::UIBold, kDetPxW - 72.0f, 3);
    for (const auto& l : nameLines) line(l, 42.0f, FontFace::UIBold, kText);
    y += 10;

    std::string ext = fileExtensionUpper(req_.path);
    const char* kindName = req_.kind == FileKind::Video ? "Video" : req_.kind == FileKind::Audio ? "Audio"
                           : req_.kind == FileKind::Text ? "Text" : "Image";
    line(std::string(kindName) + (ext.empty() ? "" : "  -  " + ext), 28.0f, FontFace::UI, kAccent);
    y += 24;

    std::error_code ec;
    fs::path p = fs::u8path(req_.path);
    auto size = fs::file_size(p, ec);
    auto row = [&](const char* k, const std::string& v) {
        line(k, 22.0f, FontFace::UI, kDim);
        line(ellipsize(v, 30.0f, FontFace::UI, kDetPxW - 72.0f), 30.0f, FontFace::UI, kText);
        y += 14;
    };
    row("SIZE", ec ? "Unknown" : formatSize(size));
    row("MODIFIED", formatModified(p));
    if (req_.kind == FileKind::Image) {
        row("DIMENSIONS", imageLabel_ ? std::to_string(imageLabel_->texture->GetDesc().Width) + " x " +
                                            std::to_string(imageLabel_->texture->GetDesc().Height)
                                      : "Unavailable");
    } else if (isMedia_) {
        if (player_.hasVideo()) row("RESOLUTION", std::to_string(player_.videoWidth()) + " x " + std::to_string(player_.videoHeight()));
        row("DURATION", player_.duration() > 0 ? formatTime(player_.duration()) : "Unknown");
        row("TRACKS", std::string(player_.hasVideo() ? "Video" : "") + (player_.hasVideo() && player_.hasAudio() ? " + " : "") +
                          (player_.hasAudio() ? "Audio" : ""));
    } else if (isText_) {
        row("LINES", std::to_string(editor_.lineCount()));
        row("LANGUAGE", editor_.language());
    } else {
        row("PLAYBACK", "Unavailable (no decoder)");
    }
    line("LOCATION", 22.0f, FontFace::UI, kDim);
    std::string dir = p.parent_path().u8string();
    for (const auto& l : wrapText(dir, 22.0f, FontFace::Mono, kDetPxW - 72.0f, 3)) line(l, 22.0f, FontFace::Mono, kText);
    detailsLabel_ = renderer_.createLabel(bm, true);
}

void Viewer::drawBar() {
    Bitmap& bm = barBitmap_;
    fillBitmap(bm, kBarPxW, kBarPxH, kBarBg);
    bool playing = isMedia_ && player_.playing();
    double pos = isMedia_ ? player_.position() : 0, dur = isMedia_ ? player_.duration() : 0;

    if (isMedia_) {
        if (playing) {
            fillRoundRect(bm, 33, 28, 45, 68, 3, kText);
            fillRoundRect(bm, 53, 28, 65, 68, 3, kText);
        } else {
            fillTriangle(bm, 34, 26, 34, 70, 70, 48, kText);
        }
        float tx0 = kSeekX0, tx1 = kSeekX1, cy = kBarPxH * 0.5f;
        fillRoundRect(bm, tx0, cy - 5, tx1, cy + 5, 5, kTrack);
        float frac = dur > 0 ? clampf(static_cast<float>(pos / dur), 0.0f, 1.0f) : 0.0f;
        fillRoundRect(bm, tx0, cy - 5, tx0 + (tx1 - tx0) * frac, cy + 5, 5, kAccent);
        fillCircle(bm, tx0 + (tx1 - tx0) * frac, cy, 13, kText);
        std::string time = formatTime(pos) + " / " + (dur > 0 ? formatTime(dur) : "--:--");
        drawText(bm, kTimeX0, 58, time, 30.0f, FontFace::UI, toColor8(kText));
        int vol = static_cast<int>(player_.volume() * 100.0f + 0.5f);
        drawText(bm, kVolX0, 58, "VOL " + std::to_string(vol), 24.0f, FontFace::UI, toColor8(kDim));
    } else if (isText_) {
        drawTextBar(bm);
    } else {
        drawText(bm, 36, 58, "Press F for fullscreen", 28.0f, FontFace::UI, toColor8(kDim));
    }

    // Fullscreen glyph: four corner brackets (inward when already fullscreen). Windowed, these live in the title strip.
    float cx = (kFsX0 + kFsX1) * 0.5f, cy = kBarPxH * 0.5f, a = 18, l = 11, w = 4;
    for (int sx = -1; fullscreen_ && sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            float px = cx + sx * a, py = cy + sy * a;
            float dir = fullscreen_ ? -1.0f : 1.0f;
            strokeLine(bm, px, py, px - sx * l * dir, py, w, kText);
            strokeLine(bm, px, py, px, py - sy * l * dir, w, kText);
        }
    // Previous / next file in the folder.
    if (onStep) {
        for (int k = 0; k < 2; ++k) {
            float bx = (k == 0 ? kPrevX0 : kNextX0) + 22.0f, d = k == 0 ? -1.0f : 1.0f;
            strokeLine(bm, bx - 6 * d, cy - 16, bx + 6 * d, cy, 5, kText);
            strokeLine(bm, bx + 6 * d, cy, bx - 6 * d, cy + 16, 5, kText);
        }
    }
    // Close glyph.
    float xx = (kCloseX0 + kCloseX1) * 0.5f;
    if (fullscreen_) {
        strokeLine(bm, xx - 14, cy - 14, xx + 14, cy + 14, 5, {0.98f, 0.55f, 0.58f});
        strokeLine(bm, xx - 14, cy + 14, xx + 14, cy - 14, 5, {0.98f, 0.55f, 0.58f});
    }

    renderer_.updateLabel(*barLabel_, bm.rgba.data(), bm.w, bm.h);
    drawnPos_ = pos;
    drawnPlaying_ = playing;
    drawnFs_ = fullscreen_;
    drawnVol_ = isMedia_ ? player_.volume() : 0;
    barTimer_ = 0;
}

// Sizes the panels for normal or fullscreen mode and decides where the viewer sits in the world.
void Viewer::layout(const HeadPose& head) {
    lastHead_ = head;
    Vec3 flat = normalize({head.forward.x, 0, head.forward.z});
    if (fullscreen_) {
        float d, boxW, boxH;
        HeadPose h = head;
        if (head.vr) {
            d = 3.0f;
            boxW = 5.4f;
            boxH = 3.0f;
        } else {
            // Desktop fullscreen is drawn in view space: the screen is a quad straight ahead of an identity camera.
            h.eye = {0, 0, 0};
            h.forward = {0, 0, -1};
            d = 1.6f;
            boxH = 2.0f * d * std::tan(0.6f);
            boxW = boxH * head.aspect;
        }
        boxW_ = boxW;
        boxH_ = boxH;
        surfH_ = std::min(boxH, boxW / mediaAspect_);
        surfW_ = surfH_ * mediaAspect_;
        barW_ = std::max(1.6f, std::min(surfW_ * 0.8f, 4.0f));
        barH_ = barW_ * kBarPxH / kBarPxW;
        detW_ = 0;
        float zoom = (!head.vr && imageLabel_) ? zoom_ : 1.0f;
        float limX = std::max(0.0f, (surfW_ * zoom - boxW) * 0.5f), limY = std::max(0.0f, (surfH_ * zoom - boxH) * 0.5f);
        panX_ = clampf(panX_, -limX, limX);
        panY_ = clampf(panY_, -limY, limY);
        surface_->scale = {surfW_ * zoom, surfH_ * zoom, 0.02f};
        surface_->position = {panX_, panY_, 0};
        bar_->scale = {barW_, barH_, 0.02f};
        bar_->position = {0, (head.vr ? -surfH_ * 0.5f : -boxH * 0.5f) + barH_ * 0.5f + 0.05f, 0.03f};
        barBase_ = bar_->position;
        if (head.vr) {
            Vec3 c = h.eye + flat * d;
            finalPos_ = {c.x, std::max(h.eye.y, 1.55f), c.z};
            finalYaw_ = std::atan2(h.eye.x - c.x, h.eye.z - c.z);
        } else {
            finalPos_ = h.eye + h.forward * d;
            finalYaw_ = 0;
        }
        return;
    }
    surface_->position = {0, 0, 0};
    zoom_ = 1.0f;
    panX_ = panY_ = 0;
    const float boxW = isText_ ? 3.0f : 2.8f, boxH = isText_ ? 2.1f : 1.7f;
    surfH_ = std::min(boxH, boxW / mediaAspect_);
    surfW_ = surfH_ * mediaAspect_;
    barW_ = std::max(surfW_, 2.2f);
    barH_ = barW_ * kBarPxH / kBarPxW;
    const float gap = 0.04f, pad = 0.05f, titleH = 0.13f, detGap = 0.08f;
    float totalH = surfH_ + gap + barH_;
    detW_ = totalH * kDetPxW / kDetPxH;
    surface_->scale = {surfW_, surfH_, 0.02f};
    bar_->scale = {barW_, barH_, 0.02f};
    bar_->position = {0, -(surfH_ * 0.5f + gap + barH_ * 0.5f), 0};
    barBase_ = bar_->position;
    details_->scale = {detW_, totalH, 0.02f};
    details_->position = {surfW_ * 0.5f + detGap + detW_ * 0.5f, -(gap + barH_) * 0.5f, 0};

    // One glass card: title strip on top, picture + controls on the left, details on the right.
    const float contentL = -std::max(surfW_, barW_) * 0.5f, contentR = surfW_ * 0.5f + detGap + detW_;
    const float contentT = surfH_ * 0.5f + gap + titleH, contentB = -(surfH_ * 0.5f + gap + barH_);
    const float cw = contentR - contentL, ch = contentT - contentB;
    frameW_ = cw + pad * 2;
    frameH_ = ch + pad * 2;
    frame_->scale = {frameW_, frameH_, 0.02f};
    frame_->position = {(contentL + contentR) * 0.5f, (contentT + contentB) * 0.5f, -0.03f};
    title_->scale = {cw, titleH, 0.02f};
    title_->position = {(contentL + contentR) * 0.5f, surfH_ * 0.5f + gap + titleH * 0.5f, 0};
    drawTitle(static_cast<int>(cw / titleH * 64.0f));

    float dist = head.vr ? 2.4f : fitDistance(frameW_, frameH_, head.aspect, head.tanHalf);
    // Spawn the preview straight ahead at the user's own eye height (so it stays usable however high they have flown);
    // only a gentle share of the view pitch is applied. Desktop: it fills 85% of the screen.
    Vec3 c = head.eye + flat * dist;
    c.y = std::max(0.8f, head.eye.y + head.forward.y * dist * 0.5f);
    finalYaw_ = std::atan2(head.eye.x - c.x, head.eye.z - c.z);
    if (!head.vr) c = c - Mat4::rotateY(finalYaw_).transformDir({1, 0, 0}) * ((contentL + contentR) * 0.5f);
    finalPos_ = c;
    finalPos_.y = c.y - (contentT + contentB) * 0.5f;
}

bool Viewer::bounds(Vec3& centre, Vec3& normal, float& w, float& h) const {
    if (!active_ || closing_ || fullscreen_ || t_ < 1.0f) return false;
    const Mat4 m = root_->worldMatrix();
    centre = m.transformPoint(frame_->position);
    normal = normalize(m.transformDir({0, 0, 1}));
    w = frameW_ * root_->scale.x;
    h = frameH_ * root_->scale.y;
    return true;
}

bool Viewer::zoomAt(Vec3 origin, Vec3 dir, float factor) {
    if (!active_ || closing_ || fullscreen_ || isMedia_ || isText_ || t_ < 1.0f) return false;
    float t, u, v;
    if (!surface_->rayHitUV(origin, normalize(dir), t, u, v)) return false;
    const float s0 = uvS_;
    const float tx = uvOx_ + (u + 0.5f) * s0, ty = uvOy_ + (0.5f - v) * s0;
    const float s1 = clampf(s0 / factor, 1.0f / 16.0f, 1.0f);
    uvS_ = s1;
    uvOx_ = clampf(tx - (u + 0.5f) * s1, 0.0f, 1.0f - s1);
    uvOy_ = clampf(ty - (0.5f - v) * s1, 0.0f, 1.0f - s1);
    if (s1 >= 0.999f) uvOx_ = uvOy_ = 0;
    return true;
}

void Viewer::drawTitle(int pxW) {
    pxW = std::max(256, pxW);
    titlePx_ = pxW;
    const int pxH = 64;
    Bitmap bm;
    fillBitmap(bm, pxW, pxH, {0.09f, 0.12f, 0.2f});
    fillRoundRect(bm, 0, pxH - 3, static_cast<float>(pxW), pxH, 0, kAccent);
    drawText(bm, 24, 42, ellipsize(req_.name, 30.0f, FontFace::UIBold, pxW - 48.0f - 2 * kTitleBtn), 30.0f, FontFace::UIBold, toColor8(kText));
    // Window buttons, top right: fullscreen then close (same order and glyphs as every other window).
    const float cy = pxH * 0.5f - 1, fx = pxW - kTitleBtn * 1.5f, cx = pxW - kTitleBtn * 0.5f, w = 3.5f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            const float px = fx + sx * 11, py = cy + sy * 9;
            strokeLine(bm, px, py, px - sx * 8, py, w, kText);
            strokeLine(bm, px, py, px, py - sy * 7, w, kText);
        }
    strokeLine(bm, cx - 10, cy - 10, cx + 10, cy + 10, w, kText);
    strokeLine(bm, cx - 10, cy + 10, cx + 10, cy - 10, w, kText);
    titleLabel_ = renderer_.createLabel(bm, true);
    title_->label = titleLabel_;
}

int Viewer::titleButton(Vec3 origin, Vec3 dir) const {
    if (!title_->visible) return 0;
    float t, u, v;
    if (!title_->rayHitUV(origin, normalize(dir), t, u, v)) return 0;
    const float px = (u + 0.5f) * titlePx_;
    if (px >= titlePx_ - kTitleBtn) return 2;
    if (px >= titlePx_ - 2 * kTitleBtn) return 1;
    return 0;
}

bool Viewer::hits(Vec3 origin, Vec3 dir) const {
    if (!active_ || closing_ || fullscreen_) return false;
    dir = normalize(dir);
    float t, u, v;
    return frame_->rayHitUV(origin, dir, t, u, v) || surface_->rayHitUV(origin, dir, t, u, v) ||
           (bar_->visible && bar_->rayHitUV(origin, dir, t, u, v));
}

// Anywhere on the card that is not the picture or the controls (title, details, glass margin) is the grab handle:
// dragging it moves the whole panel, keeping its distance from the viewer.
bool Viewer::dragStart(Vec3 origin, Vec3 dir) {
    if (!active_ || closing_ || fullscreen_ || t_ < 1.0f || !details_->visible) return false;
    dir = normalize(dir);
    float t, u, v;
    if (!frame_->rayHitUV(origin, dir, t, u, v)) return false;
    if (titleButton(origin, dir) != 0) return false;
    float t2;
    if (surface_->rayHitUV(origin, dir, t2, u, v) || (bar_->visible && bar_->rayHitUV(origin, dir, t2, u, v))) return false;
    grabDist_ = clampf(t, 0.8f, 8.0f);
    grabOff_ = Mat4::rotateY(-finalYaw_).transformDir(origin + dir * t - finalPos_);
    dragging_ = true;
    return true;
}

void Viewer::dragTo(Vec3 origin, Vec3 dir) {
    if (!dragging_) return;
    Vec3 p = origin + normalize(dir) * grabDist_;
    Vec3 f{origin.x - p.x, 0, origin.z - p.z};
    if (length(f) < 0.05f) return;
    finalYaw_ = std::atan2(f.x, f.z);
    finalPos_ = p - Mat4::rotateY(finalYaw_).transformDir(grabOff_);
}

void Viewer::applyTransform(float e) {
    root_->position = {lerpf(startPos_.x, finalPos_.x, e), lerpf(startPos_.y, finalPos_.y, e),
                       lerpf(startPos_.z, finalPos_.z, e)};
    root_->yaw = startYaw_ + wrapAngle(finalYaw_ - startYaw_) * e;
    float s = lerpf(startScale_, 1.0f, e);
    root_->scale = {s, s, s};
    bool settled = t_ >= 1.0f && !closing_;
    const bool hide = fullscreen_ && !lastHead_.vr;  // desktop fullscreen: controls fade away when idle
    float f = hide ? smooth(barFade_) : 1.0f;
    bar_->visible = settled && f > 0.01f;
    bar_->brightness = f;
    bar_->position = barBase_ + Vec3{0, -(1.0f - f) * (barH_ + 0.15f), 0};
    details_->visible = settled && !fullscreen_;
    frame_->visible = title_->visible = details_->visible;
    const float rr = fullscreen_ ? 0.0f : 0.014f;
    surface_->radius = bar_->radius = rr;
    details_->radius = title_->radius = 0.014f;
    surface_->uv[0] = uvOx_;
    surface_->uv[1] = uvOy_;
    surface_->uv[2] = surface_->uv[3] = (fullscreen_ || isMedia_ || isText_) ? 1.0f : uvS_;
    if (fullscreen_ || isMedia_ || isText_) surface_->uv[0] = surface_->uv[1] = 0;
}

void Viewer::update(double dt, const HeadPose& head) {
    if (!active_) return;
    lastHead_ = head;
    if (fullscreen_ && !head.vr) layout(head);  // screen-filling view-space panel on the desktop

    {
        const bool hide = fullscreen_ && !head.vr;
        idle_ += dt;
        const bool keep = !hide || idle_ < 3.0 || (isMedia_ && !player_.playing()) || (isText_ && editing_);
        const float target = keep ? 1.0f : 0.0f;
        const float step = static_cast<float>(dt) / 0.45f;
        barFade_ = barFade_ < target ? std::min(target, barFade_ + step * 2.0f) : std::max(target, barFade_ - step);
    }
    if (closing_) {
        t_ -= static_cast<float>(dt) / 0.3f;
        if (t_ <= 0.0f) {
            t_ = 0;
            active_ = false;
            closing_ = false;
            root_->visible = false;
            player_.close();
            videoLabel_.reset();
            imageLabel_.reset();
            textLabel_.reset();
            isText_ = editing_ = false;
            return;
        }
    } else if (t_ < 1.0f) {
        t_ = std::min(1.0f, t_ + static_cast<float>(dt) / 0.35f);
    }
    applyTransform(smooth(t_));

    if (isText_) {
        caretTimer_ += dt;
        if (!status_.empty() && (statusTimer_ -= dt) <= 0) status_.clear();
        bool caret = editing_ && std::fmod(caretTimer_, 1.0) < 0.6;
        if (editor_.version() != drawnVersion_ || caret != drawnCaret_ || editing_ != drawnEditing_) redrawText(caret);
        std::string sig = std::string(editing_ ? "e" : "v") + (editor_.dirty() ? "d" : "c") + (fullscreen_ ? "f" : "n") + status_ + "|" +
                          std::to_string(editor_.cursorLine()) + ":" + std::to_string(editor_.cursorColumn());
        if (sig != drawnSig_) {
            drawBar();
            drawnSig_ = sig;
        }
    } else if (isMedia_) {
        if (videoLabel_ && player_.takeFrame(frameBuf_)) {
            renderer_.updateLabel(*videoLabel_, frameBuf_.data(), player_.videoWidth(), player_.videoHeight());
            if (!haveVideoFrame_) {
                haveVideoFrame_ = true;
                surface_->label = videoLabel_;
            }
        }
        if (player_.playing() && player_.ended()) player_.pause();
        barTimer_ += dt;
        bool changed = player_.playing() != drawnPlaying_ || fullscreen_ != drawnFs_ || player_.volume() != drawnVol_;
        if (changed || (player_.playing() && barTimer_ > 0.1) || (!player_.playing() && std::fabs(player_.position() - drawnPos_) > 0.05))
            drawBar();
    } else if (fullscreen_ != drawnFs_) {
        drawBar();
    }
}

void Viewer::togglePause() {
    if (!isMedia_) return;
    if (player_.playing()) {
        player_.pause();
    } else {
        if (player_.ended() || (player_.duration() > 0 && player_.position() >= player_.duration() - 0.05)) player_.seek(0);
        player_.play();
    }
}

void Viewer::seekBy(double seconds) {
    if (isMedia_) player_.seek(player_.position() + seconds);
}

void Viewer::changeVolume(float delta) {
    if (!isMedia_) return;
    player_.setVolume(player_.volume() + delta);
    saveVolume(player_.volume());
}

void Viewer::toggleFullscreen(const HeadPose& head) {
    if (!active_ || closing_ || t_ < 1.0f) return;
    fullscreen_ = !fullscreen_;
    idle_ = 0;
    barFade_ = 1.0f;
    layout(head);
    drawBar();
}

void Viewer::zoomBy(float factor) {
    if (!overlay() || !imageLabel_) return;
    float old = zoom_;
    zoom_ = clampf(zoom_ * factor, 1.0f, 16.0f);
    float k = zoom_ / old;
    panX_ *= k;  // keep the view centred on the same spot
    panY_ *= k;
    layout(lastHead_);
}

void Viewer::panBy(float ndcX, float ndcY) {
    if (!overlay() || !imageLabel_ || zoom_ <= 1.0f) return;
    panX_ += ndcX * boxW_ * 0.5f;
    panY_ += ndcY * boxH_ * 0.5f;
    layout(lastHead_);
}

bool Viewer::pointerPress(Vec3 origin, Vec3 dir) {
    if (!active_ || closing_ || t_ < 1.0f) return false;
    dir = normalize(dir);
    float t, u, v;
    if (!fullscreen_) {
        const int tb = titleButton(origin, dir);
        if (tb == 2) { wantsClose_ = true; return true; }
        if (tb == 1) { toggleFullscreen(lastHead_); return true; }
    }
    if (bar_->visible && bar_->rayHitUV(origin, dir, t, u, v)) {
        float px = (u + 0.5f) * kBarPxW;
        if (fullscreen_ && px >= kCloseX0) {
            wantsClose_ = true;
        } else if (fullscreen_ && px >= kFsX0) {
            toggleFullscreen(lastHead_);
        } else if (px >= kPrevX0 && px < kFsX0 && onStep) {
            step(px < kNextX0 ? -1 : 1);
        } else if (isText_) {
            if (px < kPlayX1) {
                if (editing_) stopEditing(); else startEditing();
            } else if (px >= 116 && px < 266) {
                saveText();
            }
        } else if (isMedia_ && px < kPlayX1) {
            togglePause();
        } else if (isMedia_ && px >= kSeekX0 - 12 && px <= kSeekX1 + 12) {
            double dur = player_.duration();
            if (dur > 0) player_.seek(clampf((px - kSeekX0) / static_cast<float>(kSeekX1 - kSeekX0), 0.0f, 1.0f) * dur);
        }
        return true;
    }
    if (surface_->rayHitUV(origin, dir, t, u, v)) {
        if (isText_) {
            if (!editing_) startEditing();
            if (editing_) editor_.click((u + 0.5f) * CodeEditor::kPxW, (0.5f - v) * CodeEditor::kPxH);
            return true;
        }
        togglePause();
        return true;
    }
    if (frame_->visible && frame_->rayHitUV(origin, dir, t, u, v)) return true;
    return false;
}

void Viewer::step(int dir) {
    if (!active_ || closing_ || t_ < 1.0f || !onStep) return;
    onStep(dir);
}

bool Viewer::prepareSwitch() {
    if (isText_ && editor_.dirty() && !discardArmed_) {
        discardArmed_ = true;
        setStatus("Unsaved changes - switch again to discard", 6.0);
        return false;
    }
    if (isMedia_) player_.pause();
    active_ = false;
    switching_ = true;
    return true;
}

void Viewer::setStatus(const std::string& s, double seconds) {
    status_ = s;
    statusTimer_ = seconds;
}

void Viewer::redrawText(bool caretOn) {
    if (!textLabel_) return;
    editor_.render(textBitmap_, editing_, caretOn);
    renderer_.updateLabel(*textLabel_, textBitmap_.rgba.data(), textBitmap_.w, textBitmap_.h);
    drawnVersion_ = editor_.version();
    drawnCaret_ = caretOn;
    drawnEditing_ = editing_;
}

void Viewer::startEditing() {
    if (!isText_ || editing_) return;
    if (editor_.readOnly()) {
        setStatus("This file is read only", 3.0);
        return;
    }
    editing_ = true;
    caretTimer_ = 0;
}

void Viewer::stopEditing() { editing_ = false; }

void Viewer::scrollText(float lines) {
    if (isText_) editor_.scrollBy(lines);
}

bool Viewer::saveText() {
    if (!isText_) return false;
    discardArmed_ = false;
    if (editor_.readOnly()) {
        setStatus("This file is read only", 3.0);
        return false;
    }
    if (!editor_.save()) {
        setStatus("Save failed", 4.0);
        return false;
    }
    setStatus("Saved", 2.5);
    buildDetails();
    details_->label = detailsLabel_;
    return true;
}

void Viewer::typeText(const std::string& utf8) {
    if (isText_ && editing_) {
        discardArmed_ = false;
        editor_.insertText(utf8);
        caretTimer_ = 0;
    }
}

void Viewer::editKey(EditKey key) {
    if (!isText_ || closing_) return;
    if (key == EditKey::Save) {
        saveText();
        return;
    }
    if (!editing_) {
        switch (key) {
            case EditKey::Edit: startEditing(); break;
            case EditKey::Up: editor_.scrollBy(-1); break;
            case EditKey::Down: editor_.scrollBy(1); break;
            case EditKey::PageUp: editor_.scrollBy(-20); break;
            case EditKey::PageDown: editor_.scrollBy(20); break;
            case EditKey::DocStart: editor_.docStart(); break;
            case EditKey::DocEnd: editor_.docEnd(); break;
            default: break;
        }
        return;
    }
    discardArmed_ = false;
    caretTimer_ = 0;
    switch (key) {
        case EditKey::Backspace: editor_.backspace(); break;
        case EditKey::Delete: editor_.del(); break;
        case EditKey::Enter: editor_.newline(); break;
        case EditKey::Tab: editor_.tab(); break;
        case EditKey::Left: editor_.moveLeft(); break;
        case EditKey::Right: editor_.moveRight(); break;
        case EditKey::Up: editor_.moveUp(); break;
        case EditKey::Down: editor_.moveDown(); break;
        case EditKey::Home: editor_.home(); break;
        case EditKey::End: editor_.end(); break;
        case EditKey::PageUp: editor_.page(-1); break;
        case EditKey::PageDown: editor_.page(1); break;
        case EditKey::DocStart: editor_.docStart(); break;
        case EditKey::DocEnd: editor_.docEnd(); break;
        case EditKey::Paste: editor_.paste(); break;
        default: break;
    }
}

void Viewer::drawTextBar(Bitmap& bm) {
    const float cy = kBarPxH * 0.5f;
    // Pencil button, lit while editing.
    if (editing_) fillRoundRect(bm, 12, 12, 84, 84, 16, kAccent);
    Rgb ink = editing_ ? kBarBg : kText;
    strokeLine(bm, 32, 64, 58, 36, 11, ink);
    fillTriangle(bm, 26, 70, 30, 56, 40, 66, ink);
    strokeLine(bm, 60, 34, 66, 28, 8, ink);

    bool dirty = editor_.dirty();
    fillRoundRect(bm, 116, 20, 262, 76, 14, dirty ? kAccent : kTrack);
    float sw = measureText("SAVE", 26.0f, FontFace::UIBold);
    drawText(bm, 189 - sw * 0.5f, cy + 9, "SAVE", 26.0f, FontFace::UIBold, toColor8(dirty ? kBarBg : kDim));

    std::string info;
    if (!status_.empty()) info = status_;
    else if (editing_) info = "Editing  Ln " + std::to_string(editor_.cursorLine() + 1) + ", Col " + std::to_string(editor_.cursorColumn() + 1) + (dirty ? "  *" : "");
    else if (editor_.readOnly()) info = "Read only";
    else info = "E to edit - " + editor_.language();
    drawText(bm, 286, cy + 9, ellipsize(info, 26.0f, FontFace::UI, 470.0f), 26.0f, FontFace::UI, toColor8(status_.empty() ? kDim : kText));
}

}  // namespace sf
