#include "ImageEditor.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "stb_image.h"
#include "stb_image_write.h"

namespace sf {

namespace fs = std::filesystem;

namespace {
const Color8 kPalette[8] = {{255, 255, 255, 255}, {20, 20, 24, 255},  {235, 64, 52, 255},  {245, 166, 35, 255},
                            {250, 224, 60, 255},  {64, 200, 110, 255}, {40, 190, 235, 255}, {170, 90, 235, 255}};
const int kPct[5] = {25, 50, 75, 150, 200};
const int kDelay[3] = {3, 5, 10};
const int kBrush[3] = {2, 6, 14};

std::string lowerExt(const std::string& p) {
    std::string e = fs::u8path(p).extension().u8string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}
}  // namespace

ImageEditor::ImageEditor(Renderer& renderer) : PanelApp(renderer, 1480, 1250, 1.9f) {
    newBlank(1280, 800);
}

void ImageEditor::newBlank(int w, int h) {
    w_ = w;
    zoom_ = 1;
    panX_ = panY_ = 0;
    h_ = h;
    px_.assign(static_cast<size_t>(w) * h * 4, 255);
    undo_.clear();
    redo_.clear();
    path_.clear();
    dirty_ = false;
    tool_ = Tool::None;
    cropN_ = shapeN_ = 0;
    textOn_ = false;
    penDown_ = false;
    status_.clear();
    viewDirty_ = true;
    invalidate();
}

bool ImageEditor::openPath(const std::string& path) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, n = 0;
    unsigned char* p = data.empty() ? nullptr : stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n, 4);
    if (!p) {
        status_ = "Could not open that picture";
        invalidate();
        return false;
    }
    w_ = w;
    zoom_ = 1;
    panX_ = panY_ = 0;
    h_ = h;
    px_.assign(p, p + static_cast<size_t>(w) * h * 4);
    stbi_image_free(p);
    undo_.clear();
    redo_.clear();
    path_ = path;
    dirty_ = false;
    tool_ = Tool::None;
    cropN_ = shapeN_ = 0;
    textOn_ = false;
    penDown_ = false;
    saveAs_ = false;
    status_ = "Opened " + fs::u8path(path).filename().u8string();
    viewDirty_ = true;
    invalidate();
    return true;
}

void ImageEditor::pushUndo() {
    undo_.push_back({px_, w_, h_});
    if (undo_.size() > 20) undo_.erase(undo_.begin());
    redo_.clear();
}

void ImageEditor::redo() {
    if (redo_.empty()) {
        status_ = "Nothing to redo";
        return;
    }
    undo_.push_back({px_, w_, h_});
    Snap s = std::move(redo_.back());
    redo_.pop_back();
    px_.swap(s.px);
    w_ = s.w;
    h_ = s.h;
    viewDirty_ = dirty_ = true;
    status_ = "Redone";
}

void ImageEditor::flipV() {
    pushUndo();
    for (int y = 0; y < h_ / 2; ++y)
        std::swap_ranges(px_.begin() + static_cast<size_t>(y) * w_ * 4, px_.begin() + static_cast<size_t>(y + 1) * w_ * 4, px_.begin() + static_cast<size_t>(h_ - 1 - y) * w_ * 4);
    dirty_ = viewDirty_ = true;
    status_ = "Flipped vertically";
}

float ImageEditor::brushRadius() const {
    const float base = static_cast<float>(kBrush[size_ - 1]) * std::max(1.0f, std::max(w_, h_) / 800.0f);
    return tool_ == Tool::Eraser ? base * 1.6f : base;
}

void ImageEditor::drawLinePx(float x0, float y0, float x1, float y1, Color8 c, float r) {
    const float dx = x1 - x0, dy = y1 - y0, d = std::sqrt(dx * dx + dy * dy);
    const int n = std::max(1, static_cast<int>(d / std::max(1.0f, r * 0.5f)));
    for (int i = 0; i <= n; ++i) stamp(x0 + dx * i / n, y0 + dy * i / n, c, r);
}

void ImageEditor::drawShape(Tool t, float x0, float y0, float x1, float y1) {
    const Color8 c = kPalette[colour_];
    const float r = std::max(1.0f, brushRadius() * 0.5f);
    if (t == Tool::Line) {
        drawLinePx(x0, y0, x1, y1, c, r);
    } else if (t == Tool::Rect) {
        if (filled_) {
            const int ax = std::max(0, static_cast<int>(std::min(x0, x1))), bx = std::min(w_ - 1, static_cast<int>(std::max(x0, x1)));
            const int ay = std::max(0, static_cast<int>(std::min(y0, y1))), by = std::min(h_ - 1, static_cast<int>(std::max(y0, y1)));
            for (int y = ay; y <= by; ++y)
                for (int x = ax; x <= bx; ++x) {
                    uint8_t* p = &px_[(static_cast<size_t>(y) * w_ + x) * 4];
                    p[0] = c.r;
                    p[1] = c.g;
                    p[2] = c.b;
                    p[3] = 255;
                }
        } else {
            drawLinePx(x0, y0, x1, y0, c, r);
            drawLinePx(x1, y0, x1, y1, c, r);
            drawLinePx(x1, y1, x0, y1, c, r);
            drawLinePx(x0, y1, x0, y0, c, r);
        }
    } else if (t == Tool::Oval) {
        const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, rx = std::fabs(x1 - x0) * 0.5f, ry = std::fabs(y1 - y0) * 0.5f;
        if (rx < 1 || ry < 1) return;
        if (filled_) {
            const int ay = std::max(0, static_cast<int>(cy - ry)), by = std::min(h_ - 1, static_cast<int>(cy + ry));
            for (int y = ay; y <= by; ++y) {
                const float k = (y - cy) / ry, hw = rx * std::sqrt(std::max(0.0f, 1 - k * k));
                const int ax = std::max(0, static_cast<int>(cx - hw)), bx = std::min(w_ - 1, static_cast<int>(cx + hw));
                for (int x = ax; x <= bx; ++x) {
                    uint8_t* p = &px_[(static_cast<size_t>(y) * w_ + x) * 4];
                    p[0] = c.r;
                    p[1] = c.g;
                    p[2] = c.b;
                    p[3] = 255;
                }
            }
        } else {
            const int n = std::max(24, static_cast<int>((rx + ry) * 1.5f));
            float px = cx + rx, py = cy;
            for (int i = 1; i <= n; ++i) {
                const float a = 6.2831853f * i / n, nx = cx + rx * std::cos(a), ny = cy + ry * std::sin(a);
                drawLinePx(px, py, nx, ny, c, r);
                px = nx;
                py = ny;
            }
        }
    }
    dirty_ = viewDirty_ = true;
}

void ImageEditor::floodFill(int x, int y, Color8 c) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    pushUndo();
    uint8_t seed[4];
    std::memcpy(seed, &px_[(static_cast<size_t>(y) * w_ + x) * 4], 4);
    if (seed[0] == c.r && seed[1] == c.g && seed[2] == c.b && seed[3] == 255) return;
    auto match = [&](const uint8_t* p) {
        return std::abs(p[0] - seed[0]) + std::abs(p[1] - seed[1]) + std::abs(p[2] - seed[2]) + std::abs(p[3] - seed[3]) <= 48;
    };
    std::vector<uint8_t> done(static_cast<size_t>(w_) * h_, 0);
    std::vector<int> stack{y * w_ + x};
    done[static_cast<size_t>(y) * w_ + x] = 1;
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        uint8_t* p = &px_[static_cast<size_t>(i) * 4];
        p[0] = c.r;
        p[1] = c.g;
        p[2] = c.b;
        p[3] = 255;
        const int cx = i % w_, cy = i / w_;
        const int nb[4][2] = {{cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1}};
        for (const auto& n : nb) {
            if (n[0] < 0 || n[1] < 0 || n[0] >= w_ || n[1] >= h_) continue;
            const size_t j = static_cast<size_t>(n[1]) * w_ + n[0];
            if (done[j] || !match(&px_[j * 4])) continue;
            done[j] = 1;
            stack.push_back(static_cast<int>(j));
        }
    }
    dirty_ = viewDirty_ = true;
    status_ = "Filled";
}

void ImageEditor::adjust(int id) {
    pushUndo();
    auto each = [&](auto fn) {
        for (size_t i = 0; i < px_.size(); i += 4) fn(&px_[i]);
    };
    auto cl = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f)); };
    if (id == 70) {
        each([&](uint8_t* p) { p[0] = p[1] = p[2] = cl(p[0] * 0.299f + p[1] * 0.587f + p[2] * 0.114f); });
        status_ = "Grayscale";
    } else if (id == 71) {
        each([&](uint8_t* p) { p[0] = 255 - p[0]; p[1] = 255 - p[1]; p[2] = 255 - p[2]; });
        status_ = "Inverted";
    } else if (id == 72) {
        each([&](uint8_t* p) {
            const float r = p[0], g = p[1], b = p[2];
            p[0] = cl(r * 0.393f + g * 0.769f + b * 0.189f);
            p[1] = cl(r * 0.349f + g * 0.686f + b * 0.168f);
            p[2] = cl(r * 0.272f + g * 0.534f + b * 0.131f);
        });
        status_ = "Sepia";
    } else if (id == 73 || id == 74) {
        const float d = id == 73 ? 24.0f : -24.0f;
        each([&](uint8_t* p) { p[0] = cl(p[0] + d); p[1] = cl(p[1] + d); p[2] = cl(p[2] + d); });
        status_ = id == 73 ? "Lighter" : "Darker";
    } else if (id == 75 || id == 76) {
        const float k = id == 75 ? 1.2f : 0.83f;
        each([&](uint8_t* p) { p[0] = cl((p[0] - 128) * k + 128); p[1] = cl((p[1] - 128) * k + 128); p[2] = cl((p[2] - 128) * k + 128); });
        status_ = id == 75 ? "More contrast" : "Less contrast";
    } else if (id == 77) {
        std::vector<uint8_t> o = px_;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                for (int c = 0; c < 3; ++c) {
                    int sum = 0, n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= w_ || yy >= h_) continue;
                            sum += px_[(static_cast<size_t>(yy) * w_ + xx) * 4 + c];
                            ++n;
                        }
                    o[(static_cast<size_t>(y) * w_ + x) * 4 + c] = static_cast<uint8_t>(sum / n);
                }
        px_.swap(o);
        status_ = "Blurred";
    }
    dirty_ = viewDirty_ = true;
}

void ImageEditor::commitText() {
    if (!textOn_) return;
    textOn_ = false;
    if (textBuf_.empty()) return;
    pushUndo();
    Bitmap tmp;
    tmp.w = w_;
    tmp.h = h_;
    tmp.rgba.swap(px_);
    const float px = 28.0f * static_cast<float>(1 << (size_ - 1)) * std::max(1.0f, std::max(w_, h_) / 800.0f);
    drawText(tmp, tx_, ty_ + px * 0.8f, textBuf_, px, FontFace::UIBold, kPalette[colour_]);
    px_.swap(tmp.rgba);
    textBuf_.clear();
    dirty_ = viewDirty_ = true;
    status_ = "Text added";
}

void ImageEditor::rotate(bool right) {
    pushUndo();
    std::vector<uint8_t> o(px_.size());
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const int nx = right ? h_ - 1 - y : y, ny = right ? x : w_ - 1 - x;
            std::memcpy(&o[(static_cast<size_t>(ny) * h_ + nx) * 4], &px_[(static_cast<size_t>(y) * w_ + x) * 4], 4);
        }
    px_.swap(o);
    std::swap(w_, h_);
    dirty_ = true;
    viewDirty_ = true;
    status_ = right ? "Rotated right" : "Rotated left";
}

void ImageEditor::flip() {
    pushUndo();
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_ / 2; ++x)
            for (int c = 0; c < 4; ++c) std::swap(px_[(static_cast<size_t>(y) * w_ + x) * 4 + c], px_[(static_cast<size_t>(y) * w_ + (w_ - 1 - x)) * 4 + c]);
    dirty_ = true;
    viewDirty_ = true;
    status_ = "Flipped";
}

void ImageEditor::cropTo(int x0, int y0, int x1, int y1) {
    x0 = std::clamp(x0, 0, w_ - 1);
    y0 = std::clamp(y0, 0, h_ - 1);
    x1 = std::clamp(x1, x0 + 1, w_);
    y1 = std::clamp(y1, y0 + 1, h_);
    pushUndo();
    const int nw = x1 - x0, nh = y1 - y0;
    std::vector<uint8_t> o(static_cast<size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; ++y) std::memcpy(&o[static_cast<size_t>(y) * nw * 4], &px_[(static_cast<size_t>(y0 + y) * w_ + x0) * 4], static_cast<size_t>(nw) * 4);
    px_.swap(o);
    w_ = nw;
    zoom_ = 1;
    panX_ = panY_ = 0;
    h_ = nh;
    dirty_ = true;
    viewDirty_ = true;
    status_ = "Cropped to " + std::to_string(nw) + " x " + std::to_string(nh);
}

void ImageEditor::resizePct(int pct) {
    const int nw = std::max(1, w_ * pct / 100), nh = std::max(1, h_ * pct / 100);
    if (nw > 12000 || nh > 12000) {
        status_ = "That would be too large";
        return;
    }
    pushUndo();
    std::vector<uint8_t> o(static_cast<size_t>(nw) * nh * 4);
    for (int y = 0; y < nh; ++y) {
        const float fy = std::max(0.0f, (y + 0.5f) * h_ / nh - 0.5f);
        const int y0 = std::min(h_ - 1, static_cast<int>(fy)), y1 = std::min(h_ - 1, y0 + 1);
        const float ty = fy - y0;
        for (int x = 0; x < nw; ++x) {
            const float fx = std::max(0.0f, (x + 0.5f) * w_ / nw - 0.5f);
            const int x0 = std::min(w_ - 1, static_cast<int>(fx)), x1 = std::min(w_ - 1, x0 + 1);
            const float tx = fx - x0;
            for (int c = 0; c < 4; ++c) {
                const float a = px_[(static_cast<size_t>(y0) * w_ + x0) * 4 + c] * (1 - tx) + px_[(static_cast<size_t>(y0) * w_ + x1) * 4 + c] * tx;
                const float b = px_[(static_cast<size_t>(y1) * w_ + x0) * 4 + c] * (1 - tx) + px_[(static_cast<size_t>(y1) * w_ + x1) * 4 + c] * tx;
                o[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>(a * (1 - ty) + b * ty + 0.5f);
            }
        }
    }
    px_.swap(o);
    w_ = nw;
    zoom_ = 1;
    panX_ = panY_ = 0;
    h_ = nh;
    dirty_ = true;
    viewDirty_ = true;
    status_ = "Resized to " + std::to_string(nw) + " x " + std::to_string(nh);
}

void ImageEditor::stamp(float ix, float iy, Color8 c, float r) {
    const int x0 = std::max(0, static_cast<int>(ix - r)), x1 = std::min(w_ - 1, static_cast<int>(ix + r));
    const int y0 = std::max(0, static_cast<int>(iy - r)), y1 = std::min(h_ - 1, static_cast<int>(iy + r));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = x - ix, dy = y - iy;
            if (dx * dx + dy * dy > r * r) continue;
            uint8_t* p = &px_[(static_cast<size_t>(y) * w_ + x) * 4];
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = 255;
        }
}

void ImageEditor::stroke(float ix, float iy) {
    const Color8 c = tool_ == Tool::Eraser ? Color8{255, 255, 255, 255} : kPalette[colour_];
    const float r = brushRadius();
    if (lastX_ >= 0) drawLinePx(lastX_, lastY_, ix, iy, c, r);
    else stamp(ix, iy, c, r);
    lastX_ = ix;
    lastY_ = iy;
    dirty_ = true;
    viewDirty_ = true;
}

bool ImageEditor::toImage(float x, float y, float& ix, float& iy) const {
    if (!view_.contains(x, y) || scale_ <= 0) return false;
    ix = (x - ox_) / scale_;
    iy = (y - oy_) / scale_;
    return ix >= 0 && iy >= 0 && ix < w_ && iy < h_;
}

void ImageEditor::onWheel(float x, float y, float notches) {
    if (!view_.contains(x, y) || w_ <= 0 || fit_ <= 0) return;
    const float old = scale_;
    zoom_ = std::clamp(zoom_ * std::pow(1.2f, notches), 0.25f, 40.0f / fit_);
    if (zoom_ < 1.05f && zoom_ > 0.95f) zoom_ = 1;
    scale_ = fit_ * zoom_;
    // Keep the pixel under the cursor fixed.
    const float cx = (view_.x0 + view_.x1) * 0.5f, cy = (view_.y0 + view_.y1) * 0.5f;
    const float k = scale_ / old;
    panX_ = (x - cx) - ((x - cx) - panX_) * k;
    panY_ = (y - cy) - ((y - cy) - panY_) * k;
    if (zoom_ == 1) panX_ = panY_ = 0;
    viewDirty_ = true;
}

void ImageEditor::save() {
    if (path_.empty()) {
        saveAs_ = true;
        const char* up = std::getenv("USERPROFILE");
        field_ = std::string(up ? up : "C:") + "\\Pictures\\Edited.png";
        status_ = "Type a file path (.png .jpg .bmp) and press Enter";
        return;
    }
    field_ = path_;
    commitSaveAs();
}

void ImageEditor::commitSaveAs() {
    const std::string p = field_;
    saveAs_ = false;
    if (p.empty()) return;
    std::error_code ec;
    const fs::path parent = fs::u8path(p).parent_path();
    if (!parent.empty() && !fs::is_directory(parent, ec)) {
        status_ = "That folder does not exist";
        saveAs_ = true;
        return;
    }
    const std::string e = lowerExt(p);
    const std::string native = fs::u8path(p).string();
    int ok = 0;
    if (e == ".jpg" || e == ".jpeg") ok = stbi_write_jpg(native.c_str(), w_, h_, 4, px_.data(), 92);
    else if (e == ".bmp") ok = stbi_write_bmp(native.c_str(), w_, h_, 4, px_.data());
    else ok = stbi_write_png(native.c_str(), w_, h_, 4, px_.data(), w_ * 4);
    if (ok) {
        path_ = p;
        dirty_ = false;
        status_ = "Saved " + fs::u8path(p).filename().u8string();
    } else status_ = "Save failed";
}

namespace {
void ring(sf::Bitmap& bm, float cx, float cy, float rx, float ry, float th, sf::Color8 col) {
    float px = cx + rx, py = cy;
    for (int i = 1; i <= 24; ++i) {
        const float a = 6.2831853f * i / 24, nx = cx + rx * std::cos(a), ny = cy + ry * std::sin(a);
        sf::ui::line(bm, px, py, nx, ny, th, col);
        px = nx;
        py = ny;
    }
}

// Simple line-art icons for the toolbox, drawn around (cx, cy) inside a 64 px button.
void drawToolIcon(sf::Bitmap& bm, int id, float cx, float cy, sf::Color8 c) {
    using namespace sf::ui;
    switch (id) {
    case 15:  // brush
        line(bm, cx - 14, cy + 14, cx + 10, cy - 10, 5, c);
        line(bm, cx + 10, cy - 10, cx + 16, cy - 16, 8, c);
        dot(bm, cx - 17, cy + 17, 4, c);
        break;
    case 17:  // eraser
        line(bm, cx - 16, cy + 6, cx - 4, cy - 12, 3, c);
        line(bm, cx - 4, cy - 12, cx + 16, cy + 2, 3, c);
        line(bm, cx + 16, cy + 2, cx + 4, cy + 18, 3, c);
        line(bm, cx + 4, cy + 18, cx - 16, cy + 6, 3, c);
        line(bm, cx - 8, cy + 12, cx + 10, cy - 8, 3, c);
        break;
    case 18:  // fill bucket
        strokeRect(bm, cx - 14, cy - 6, cx + 8, cy + 16, 3, c);
        line(bm, cx - 14, cy - 6, cx - 4, cy - 16, 3, c);
        line(bm, cx - 4, cy - 16, cx + 8, cy - 6, 3, c);
        dot(bm, cx + 15, cy + 8, 4, c);
        line(bm, cx + 15, cy, cx + 15, cy + 6, 3, c);
        break;
    case 19:  // picker
        line(bm, cx - 16, cy + 16, cx + 4, cy - 4, 4, c);
        line(bm, cx + 2, cy - 14, cx + 14, cy - 2, 7, c);
        line(bm, cx + 12, cy - 18, cx + 18, cy - 12, 4, c);
        break;
    case 22:  // line
        line(bm, cx - 16, cy + 16, cx + 16, cy - 16, 3.5f, c);
        dot(bm, cx - 16, cy + 16, 4, c);
        dot(bm, cx + 16, cy - 16, 4, c);
        break;
    case 23: strokeRect(bm, cx - 17, cy - 12, cx + 17, cy + 12, 3.5f, c); break;  // rectangle
    case 24: ring(bm, cx, cy, 18, 12, 3.5f, c); break;                              // oval
    case 25:  // text
        line(bm, cx - 14, cy - 14, cx + 14, cy - 14, 5, c);
        line(bm, cx, cy - 14, cx, cy + 16, 5, c);
        break;
    case 13:  // crop
        line(bm, cx - 8, cy - 18, cx - 8, cy + 8, 3.5f, c);
        line(bm, cx - 18, cy - 8, cx + 18, cy - 8, 0.01f, c);
        line(bm, cx - 18, cy + 8, cx + 8, cy + 8, 3.5f, c);
        line(bm, cx + 8, cy - 8, cx + 8, cy + 18, 3.5f, c);
        line(bm, cx - 18, cy - 8, cx + 8, cy - 8, 3.5f, c);
        break;
    case 14:  // resize
        strokeRect(bm, cx - 17, cy - 3, cx + 3, cy + 17, 3, c);
        strokeRect(bm, cx - 17, cy - 17, cx + 17, cy + 17, 1.5f, c);
        line(bm, cx - 2, cy + 2, cx + 14, cy - 14, 3.5f, c);
        line(bm, cx + 14, cy - 14, cx + 6, cy - 14, 3.5f, c);
        line(bm, cx + 14, cy - 14, cx + 14, cy - 6, 3.5f, c);
        break;
    case 26:  // pan: four-way arrow
        line(bm, cx - 17, cy, cx + 17, cy, 3.5f, c);
        line(bm, cx, cy - 17, cx, cy + 17, 3.5f, c);
        dot(bm, cx - 17, cy, 4, c);
        dot(bm, cx + 17, cy, 4, c);
        dot(bm, cx, cy - 17, 4, c);
        dot(bm, cx, cy + 17, 4, c);
        break;
    case 16:  // camera
        strokeRect(bm, cx - 18, cy - 10, cx + 18, cy + 14, 3.5f, c);
        line(bm, cx - 8, cy - 14, cx + 6, cy - 14, 5, c);
        ring(bm, cx, cy + 2, 8, 8, 3.5f, c);
        break;
    default: break;
    }
}
}  // namespace
void ImageEditor::act(int id) {
    if (id == 1) {
        if (requestOpen) requestOpen();
    } else if (id == 2) save();
    else if (id == 3) {
        saveAs_ = true;
        const char* up = std::getenv("USERPROFILE");
        field_ = path_.empty() ? std::string(up ? up : "C:") + "\\Pictures\\Edited.png" : path_;
        status_ = "Type a file path (.png .jpg .bmp) and press Enter";
    } else if (id == 4) {
        if (undo_.empty()) {
            status_ = "Nothing to undo";
            return;
        }
        redo_.push_back({px_, w_, h_});
        Snap s = std::move(undo_.back());
        undo_.pop_back();
        px_.swap(s.px);
        w_ = s.w;
        h_ = s.h;
        viewDirty_ = true;
        dirty_ = true;
        status_ = "Undone";
    } else if (id == 5) {
        newBlank(1280, 800);
        status_ = "New blank canvas";
    } else if (id == 6) redo();
    else if (id == 7) flipV();
    else if (id == 10) rotate(false);
    else if (id == 11) rotate(true);
    else if (id == 12) flip();
    else if ((id >= 13 && id <= 19) || (id >= 22 && id <= 26)) {
        commitText();
        tool_ = id == 13 ? Tool::Crop : id == 14 ? Tool::Resize : id == 15 ? Tool::Brush : id == 16 ? Tool::Shot : id == 17 ? Tool::Eraser
              : id == 18 ? Tool::Fill : id == 19 ? Tool::Pick : id == 22 ? Tool::Line : id == 23 ? Tool::Rect : id == 24 ? Tool::Oval : id == 26 ? Tool::Pan : Tool::Text;
        cropN_ = shapeN_ = 0;
        penDown_ = false;
        lastX_ = -1;
        status_ = tool_ == Tool::Crop ? "Click two opposite corners of the area to keep"
                : tool_ == Tool::Brush || tool_ == Tool::Eraser ? "Click to start, move, click again to lift"
                : tool_ == Tool::Fill ? "Click an area to fill it with the chosen colour"
                : tool_ == Tool::Pick ? "Click the picture to pick up a colour"
                : tool_ == Tool::Line || tool_ == Tool::Rect || tool_ == Tool::Oval ? "Click the start point, then the end point"
                : tool_ == Tool::Text ? "Click where the text goes, type, then press Enter"
                : tool_ == Tool::Pan ? "Drag the picture to move it; scroll to zoom"
                : tool_ == Tool::Resize ? "Pick a size"
                                        : "Pick a delay, then Start - this window steps aside before the shot";
    } else if (id == 20) {
        if (cropN_ == 2)
            cropTo(static_cast<int>(std::min(cx0_, cx1_)), static_cast<int>(std::min(cy0_, cy1_)), static_cast<int>(std::max(cx0_, cx1_)),
                   static_cast<int>(std::max(cy0_, cy1_)));
        cropN_ = 0;
    } else if (id == 21) cropN_ = 0;
    else if (id >= 30 && id < 35) resizePct(kPct[id - 30]);
    else if (id >= 40 && id < 48) colour_ = id - 40;
    else if (id >= 50 && id < 53) size_ = id - 49;
    else if (id >= 70 && id <= 77) adjust(id);
    else if (id == 80) filled_ = !filled_;
    else if (id >= 60 && id < 63) delay_ = kDelay[id - 60];
    else if (id == 63) {
        timerOn_ = true;
        timerLeft_ = delay_;
        status_ = "Timer running";
    } else if (id == 64) {
        timerOn_ = false;
        status_ = "Timer cancelled";
    }
}

bool ImageEditor::onHover(float x, float y) {
    int h = -1;
    for (size_t i = 0; i < hits_.size(); ++i)
        if (hits_[i].r.contains(x, y)) h = static_cast<int>(i);
    bool changed = h != hover_;
    hover_ = h;
    hoverX_ = x;
    hoverY_ = y;
    if (panning_) {
        panX_ += x - panLastX_;
        panY_ += y - panLastY_;
        panLastX_ = x;
        panLastY_ = y;
        viewDirty_ = true;
        return true;
    }
    float ix, iy;
    if (tool_ == Tool::Brush && penDown_ && toImage(x, y, ix, iy)) {
        stroke(ix, iy);
        return true;
    }
    if (tool_ == Tool::Eraser && penDown_ && toImage(x, y, ix, iy)) {
        stroke(ix, iy);
        return true;
    }
    if ((tool_ == Tool::Crop && cropN_ == 1) || (shapeN_ == 1) || tool_ == Tool::Brush || tool_ == Tool::Eraser) changed = true;
    return changed;
}

void ImageEditor::onClick(float x, float y, bool) {
    for (const Hit& h : hits_)
        if (h.r.contains(x, y)) {
            act(h.id);
            return;
        }
    if (tool_ == Tool::Pan && view_.contains(x, y)) {
        panning_ = true;
        panLastX_ = x;
        panLastY_ = y;
        return;
    }
    float ix, iy;
    if (!toImage(x, y, ix, iy)) return;
    if (tool_ == Tool::Crop) {
        if (cropN_ == 0 || cropN_ == 2) {
            cx0_ = cx1_ = ix;
            cy0_ = cy1_ = iy;
            cropN_ = 1;
        } else {
            cx1_ = ix;
            cy1_ = iy;
            cropN_ = 2;
            status_ = "Press APPLY CROP to keep this area";
        }
    } else if (tool_ == Tool::Fill) {
        floodFill(static_cast<int>(ix), static_cast<int>(iy), kPalette[colour_]);
    } else if (tool_ == Tool::Pick) {
        const uint8_t* p = &px_[(static_cast<size_t>(static_cast<int>(iy)) * w_ + static_cast<int>(ix)) * 4];
        int best = 0, bd = 1 << 30;
        for (int i = 0; i < 8; ++i) {
            const int d = std::abs(p[0] - kPalette[i].r) + std::abs(p[1] - kPalette[i].g) + std::abs(p[2] - kPalette[i].b);
            if (d < bd) { bd = d; best = i; }
        }
        colour_ = best;
        status_ = "Picked the nearest palette colour";
    } else if (tool_ == Tool::Line || tool_ == Tool::Rect || tool_ == Tool::Oval) {
        if (shapeN_ == 0) {
            sx_ = ix;
            sy_ = iy;
            shapeN_ = 1;
        } else {
            pushUndo();
            drawShape(tool_, sx_, sy_, ix, iy);
            shapeN_ = 0;
        }
    } else if (tool_ == Tool::Text) {
        commitText();
        textOn_ = true;
        textBuf_.clear();
        tx_ = ix;
        ty_ = iy;
    } else if (tool_ == Tool::Brush || tool_ == Tool::Eraser) {
        if (!penDown_) {
            pushUndo();
            penDown_ = true;
            lastX_ = -1;
            stroke(ix, iy);
        } else {
            stroke(ix, iy);
            penDown_ = false;
            lastX_ = -1;
        }
    }
}

void ImageEditor::onCtrl(const std::string& s) {
    if (s != "esc") return;
    if (saveAs_) {
        saveAs_ = false;
        status_ = "Save cancelled";
    } else if (textOn_) {
        textOn_ = false;
        textBuf_.clear();
    } else if (shapeN_) shapeN_ = 0;
    else if (penDown_) {
        penDown_ = false;
        lastX_ = -1;
    } else if (cropN_) cropN_ = 0;
    else close();
}

void ImageEditor::onText(const std::string& s) {
    if (saveAs_) field_ += s;
    else if (textOn_) textBuf_ += s;
}

void ImageEditor::onKey(EditKey k) {
    if (k == EditKey::Save) {
        save();
        return;
    }
    if (!saveAs_ && !textOn_) return;
    std::string& field = saveAs_ ? field_ : textBuf_;
    if (k == EditKey::Enter) {
        if (saveAs_) commitSaveAs();
        else commitText();
    } else if (k == EditKey::Backspace) {
        while (!field.empty() && (field.back() & 0xC0) == 0x80) field.pop_back();
        if (!field.empty()) field.pop_back();
    } else if (k == EditKey::Paste) {
        if (char* t = SDL_GetClipboardText()) {
            for (const char* c = t; *c; ++c)
                if (static_cast<unsigned char>(*c) >= 0x20) field += *c;
            SDL_free(t);
        }
    }
}

void ImageEditor::tick(double dt) {
    if (timerOn_) {
        timerLeft_ -= dt;
        invalidate();
        if (timerLeft_ <= 0) {
            timerOn_ = false;
            const char* up = std::getenv("USERPROFILE");
            fs::path dir = fs::path(up ? up : ".") / "Pictures" / "SciFiShell";
            std::error_code ec;
            fs::create_directories(dir, ec);
            shotPath_ = (dir / ("Timed-" + std::to_string(SDL_GetTicks()) + ".png")).string();
            hide();
            stage_ = 1;
            shotWait_ = 0.8;
        }
    } else if (shotWait_ > 0) {
        shotWait_ -= dt;
        if (shotWait_ <= 0) {
            if (stage_ == 1) {
                if (takeShot) takeShot(shotPath_);
                stage_ = 2;
                shotWait_ = 1.6;
            } else {
                stage_ = 0;
                shotWait_ = 0;
                if (restore) restore();
                openPath(shotPath_);
                status_ = "Timed screenshot saved - " + fs::u8path(shotPath_).filename().u8string();
            }
        }
    }
}

void ImageEditor::renderView(int vw, int vh) {
    surf_.w = vw;
    surf_.h = vh;
    surf_.rgba.assign(static_cast<size_t>(vw) * vh * 4, 0);
    const ui::Tokens t = ui::tokens();
    for (size_t i = 0; i < surf_.rgba.size(); i += 4) {
        surf_.rgba[i] = t.bg.r;
        surf_.rgba[i + 1] = t.bg.g;
        surf_.rgba[i + 2] = t.bg.b;
        surf_.rgba[i + 3] = 255;
    }
    fit_ = std::min(8.0f, std::min(static_cast<float>(vw - 8) / w_, static_cast<float>(vh - 8) / h_));
    scale_ = fit_ * zoom_;
    const float dw = w_ * scale_, dh = h_ * scale_;
    const float lx = (vw - dw) * 0.5f + panX_, ly = (vh - dh) * 0.5f + panY_;
    ox_ = view_.x0 + lx;
    oy_ = view_.y0 + ly;
    for (int y = std::max(0, static_cast<int>(std::floor(ly))); y < std::min(vh, static_cast<int>(ly + dh)); ++y) {
        const int sy = std::clamp(static_cast<int>((y - ly) / scale_), 0, h_ - 1);
        for (int x = std::max(0, static_cast<int>(std::floor(lx))); x < std::min(vw, static_cast<int>(lx + dw)); ++x) {
            const int sx = std::clamp(static_cast<int>((x - lx) / scale_), 0, w_ - 1);
            const uint8_t* s = &px_[(static_cast<size_t>(sy) * w_ + sx) * 4];
            uint8_t* d = &surf_.rgba[(static_cast<size_t>(y) * vw + x) * 4];
            const bool chk = ((x / 16) + (y / 16)) & 1;
            const int bg = chk ? 60 : 44, a = s[3];
            d[0] = static_cast<uint8_t>((s[0] * a + bg * (255 - a)) / 255);
            d[1] = static_cast<uint8_t>((s[1] * a + bg * (255 - a)) / 255);
            d[2] = static_cast<uint8_t>((s[2] * a + bg * (255 - a)) / 255);
        }
    }
    viewDirty_ = false;
}

void ImageEditor::paint(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    std::string name = path_.empty() ? "UNTITLED" : fs::u8path(path_).filename().u8string();
    if (dirty_) name += " *";
    const Rect c = panel(bm, "IMAGE", name);
    hits_.clear();
    auto hot = [&](const Rect& r, int id) {
        hits_.push_back({r, id});
        return hover_ == static_cast<int>(hits_.size()) - 1 ? State::Hover : State::Normal;
    };
    struct B {
        const char* l;
        int id;
    };
    float y = c.y0 + 4;
    const int toolId = tool_ == Tool::Crop ? 13 : tool_ == Tool::Resize ? 14 : tool_ == Tool::Brush ? 15 : tool_ == Tool::Shot ? 16 : tool_ == Tool::Eraser ? 17
                     : tool_ == Tool::Fill ? 18 : tool_ == Tool::Pick ? 19 : tool_ == Tool::Line ? 22 : tool_ == Tool::Rect ? 23 : tool_ == Tool::Oval ? 24
                     : tool_ == Tool::Text ? 25 : tool_ == Tool::Pan ? 26 : -1;
    // Top row: file and history buttons plus the path / save-as field.
    float fx = c.x0 + 4;
    {
        const B top[] = {{"OPEN", 1}, {"NEW", 5}, {"SAVE", 2}, {"SAVE AS", 3}, {"UNDO", 4}, {"REDO", 6}};
        for (const B& b : top) {
            const float bw = std::max(84.0f, measureText(b.l, 20, FontFace::UIBold) + 40);
            const Rect r{fx, y, fx + bw, y + 46};
            button(bm, r, b.l, hot(r, b.id), false, 20);
            fx += bw + 8;
        }
    }
    // Right-hand sidebar: transform and adjustment buttons in two columns.
    const float sideW = 2 * 128 + 8, sideX = c.x1 - 4 - sideW;
    {
        const B side[][2] = {{{"ROTATE L", 10}, {"ROTATE R", 11}}, {{"FLIP H", 12}, {"FLIP V", 7}},   {{"GRAYSCALE", 70}, {"INVERT", 71}},
                             {{"SEPIA", 72}, {"BLUR", 77}},        {{"LIGHTER", 73}, {"DARKER", 74}}, {{"CONTRAST +", 75}, {"CONTRAST -", 76}}};
        float sy2 = y + 54;
        int gap = 0;
        for (const auto& pr : side) {
            if (gap == 2) sy2 += 14;
            for (int k = 0; k < 2; ++k) {
                const Rect r{sideX + k * 136, sy2, sideX + k * 136 + 128, sy2 + 46};
                button(bm, r, pr[k].l, hot(r, pr[k].id), false, 18);
            }
            sy2 += 54;
            ++gap;
        }
    }
    const Rect fr{fx + 4, y, c.x1 - 4, y + 46};
    if (saveAs_ || !path_.empty()) {
        card(bm, fr, saveAs_ ? State::Selected : State::Normal);
        if (saveAs_) {
            const bool caret = (SDL_GetTicks() / 500) % 2 == 0;
            label(bm, fr.x0 + 14, fr.y0 + 31, "SAVE TO:", 20, FontFace::UIBold, t.accent);
            label(bm, fr.x0 + 120, fr.y0 + 31, ellipsize(field_, 22, FontFace::Mono, fr.w() - 150) + (caret ? "|" : ""), 22, FontFace::Mono, t.text);
        } else {
            label(bm, fr.x0 + 14, fr.y0 + 31, ellipsize(path_, 20, FontFace::UI, fr.w() - 28), 20, FontFace::UI, t.dim);
        }
    }    y += 54;    const float tbW = 140;
    float x = c.x0 + 4 + tbW + 12;
    if (tool_ == Tool::Crop) {
        const Rect a{x, y, x + 190, y + 46}, b{x + 198, y, x + 338, y + 46};
        button(bm, a, "APPLY CROP", cropN_ == 2 ? hot(a, 20) : State::Disabled, true, 20);
        button(bm, b, "CLEAR", hot(b, 21), false, 20);
        label(bm, b.x1 + 18, y + 31, cropN_ == 0 ? "Click the first corner" : cropN_ == 1 ? "Click the opposite corner" : "Selection ready", 20, FontFace::UI, t.dim);
    } else if (tool_ == Tool::Resize) {
        int i = 0;
        for (const char* l : {"25%", "50%", "75%", "150%", "200%"}) {
            const Rect r{x, y, x + 110, y + 46};
            button(bm, r, l, hot(r, 30 + i++), false, 20);
            x += 118;
        }
        label(bm, x + 10, y + 31, std::to_string(w_) + " x " + std::to_string(h_), 20, FontFace::Mono, t.dim);
    } else if (isDrawTool()) {
        if (tool_ != Tool::Eraser) {
            for (int i = 0; i < 8; ++i) {
                const Rect r{x, y, x + 46, y + 46};
                const State s = hot(r, 40 + i);
                fillRound(bm, r.x0 + 4, r.y0 + 4, r.x1 - 4, r.y1 - 4, 8, kPalette[i]);
                strokeRect(bm, r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1, i == colour_ ? 3.0f : 1.0f, i == colour_ ? t.accent : (s == State::Hover ? t.text : t.border));
                x += 54;
            }
            x += 16;
        }
        if (tool_ != Tool::Fill && tool_ != Tool::Pick) {
            int i = 1;
            for (const char* l : {"S", "M", "L"}) {
                const Rect r{x, y, x + 60, y + 46};
                State s = hot(r, 49 + i);
                if (i == size_) s = State::Selected;
                button(bm, r, l, s, false, 20);
                x += 68;
                ++i;
            }
        }
        if (tool_ == Tool::Rect || tool_ == Tool::Oval) {
            const Rect r{x + 8, y, x + 158, y + 46};
            const State fs = hot(r, 80);
            button(bm, r, "FILLED", filled_ ? State::Selected : fs, false, 20);
            x += 166;
        }
        const char* hint = (tool_ == Tool::Brush || tool_ == Tool::Eraser) ? (penDown_ ? "PAINTING - click to lift" : "Click to start a stroke")
                         : (tool_ == Tool::Line || tool_ == Tool::Rect || tool_ == Tool::Oval) ? (shapeN_ ? "Click the end point" : "Click the start point")
                         : tool_ == Tool::Text ? (textOn_ ? "Type, then press Enter" : "Click where the text goes")
                         : tool_ == Tool::Fill ? "Click an area to fill" : "Click to pick a colour";
        label(bm, x + 10, y + 31, ellipsize(hint, 20, FontFace::UI, std::max(40.0f, sideX - 16 - x - 10)), 20, FontFace::UI, (penDown_ || shapeN_ || textOn_) ? t.accent : t.dim);    } else if (tool_ == Tool::Shot) {
        int i = 0;
        for (const char* l : {"3 SEC", "5 SEC", "10 SEC"}) {
            const Rect r{x, y, x + 120, y + 46};
            State s = hot(r, 60 + i);
            if (kDelay[i] == delay_) s = State::Selected;
            button(bm, r, l, s, false, 20);
            x += 128;
            ++i;
        }
        x += 10;
        const Rect r{x, y, x + 150, y + 46};
        if (timerOn_) {
            button(bm, r, "CANCEL", hot(r, 64), false, 20);
            label(bm, r.x1 + 18, y + 31, "Shot in " + std::to_string(static_cast<int>(std::ceil(std::max(0.0, timerLeft_)))) + " ...", 24, FontFace::UIBold, t.accent);
        } else {
            button(bm, r, "START", hot(r, 63), true, 20);
            label(bm, r.x1 + 18, y + 31, "Saves to Pictures\\SciFiShell", 20, FontFace::UI, t.dim);
        }
    }

    // Toolbox down the left side: icon buttons with tooltips.
    std::vector<std::pair<int, std::string>> tips;
    {
        struct T {
            int id;
            const char* tip;
        };
        const T tools[] = {{15, "Brush - paint freehand strokes"}, {17, "Eraser - rub out to transparent"}, {18, "Fill - flood an area with the chosen colour"},
                           {19, "Picker - pick up a colour from the picture"}, {22, "Line - click the start, then the end"}, {23, "Rectangle - outline or filled"},
                           {24, "Oval - outline or filled"}, {25, "Text - click, type, press Enter"}, {13, "Crop - keep a selected area"},
                           {14, "Resize - scale the whole picture"}, {26, "Pan - drag to move the picture, scroll to zoom"}, {16, "Screenshot timer - grab the shell after a countdown"}};
        int i = 0;
        for (const T& tl : tools) {
            const float bx = c.x0 + 4 + (i % 2) * 70, by = y + (i / 2) * 70;
            const Rect r{bx, by, bx + 64, by + 64};
            State s = hot(r, tl.id);
            tips.push_back({static_cast<int>(hits_.size()) - 1, tl.tip});
            if (tl.id == toolId) s = State::Selected;
            card(bm, r, s);
            drawToolIcon(bm, tl.id, (r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f, (tl.id == toolId || s == State::Hover) ? t.accent : t.text);
            ++i;
        }
    }
    view_ = Rect{c.x0 + 4 + tbW + 12, y + 56, sideX - 8, c.y1 - 44};
    const int vw = static_cast<int>(view_.w()), vh = static_cast<int>(view_.h());
    if (viewDirty_ || surf_.w != vw || surf_.h != vh || surf_.rgba.empty()) renderView(vw, vh);
    const int vx = static_cast<int>(view_.x0), vy = static_cast<int>(view_.y0);
    for (int r = 0; r < surf_.h; ++r) {
        const int dy = vy + r;
        if (dy < 0 || dy >= bm.h) continue;
        const int w = std::min(surf_.w, bm.w - vx);
        if (w > 0) std::memcpy(&bm.rgba[(static_cast<size_t>(dy) * bm.w + vx) * 4], &surf_.rgba[static_cast<size_t>(r) * surf_.w * 4], static_cast<size_t>(w) * 4);
    }
    strokeRect(bm, view_.x0 - 1, view_.y0 - 1, view_.x1 + 1, view_.y1 + 1, 2, t.border);

    if (tool_ == Tool::Crop && cropN_ > 0) {
        float bx = cx1_, by = cy1_;
        float ix, iy;
        if (cropN_ == 1 && toImage(hoverX_, hoverY_, ix, iy)) {
            bx = ix;
            by = iy;
        }
        strokeRect(bm, ox_ + std::min(cx0_, bx) * scale_, oy_ + std::min(cy0_, by) * scale_, ox_ + std::max(cx0_, bx) * scale_, oy_ + std::max(cy0_, by) * scale_, 3, t.accent);
    }
    if (tool_ == Tool::Brush || tool_ == Tool::Eraser) {
        float ix, iy;
        if (toImage(hoverX_, hoverY_, ix, iy)) {
            const float r = brushRadius() * scale_;
            strokeRect(bm, hoverX_ - r, hoverY_ - r, hoverX_ + r, hoverY_ + r, 1.5f, t.accent);
        }
    }
    if (shapeN_ == 1) {
        float ix, iy;
        if (toImage(hoverX_, hoverY_, ix, iy)) {
            const float ax = ox_ + sx_ * scale_, ay = oy_ + sy_ * scale_;
            if (tool_ == Tool::Line) line(bm, ax, ay, hoverX_, hoverY_, 2.5f, t.accent);
            else if (tool_ == Tool::Rect) strokeRect(bm, std::min(ax, hoverX_), std::min(ay, hoverY_), std::max(ax, hoverX_), std::max(ay, hoverY_), 2.5f, t.accent);
            else if (tool_ == Tool::Oval) {
                const float cx = (ax + hoverX_) * 0.5f, cy = (ay + hoverY_) * 0.5f, rx = std::fabs(hoverX_ - ax) * 0.5f, ry = std::fabs(hoverY_ - ay) * 0.5f;
                float px = cx + rx, py = cy;
                for (int i = 1; i <= 48; ++i) {
                    const float a = 6.2831853f * i / 48, nx = cx + rx * std::cos(a), ny = cy + ry * std::sin(a);
                    line(bm, px, py, nx, ny, 2.5f, t.accent);
                    px = nx;
                    py = ny;
                }
            }
        }
    }
    if (textOn_) {
        const float px = 28.0f * static_cast<float>(1 << (size_ - 1)) * std::max(1.0f, std::max(w_, h_) / 800.0f) * scale_;
        const float bx = ox_ + tx_ * scale_, by = oy_ + ty_ * scale_;
        const bool caret = (SDL_GetTicks() / 500) % 2 == 0;
        drawText(bm, bx, by + px * 0.8f, textBuf_ + (caret ? "|" : ""), px, FontFace::UIBold, kPalette[colour_]);
    }

    const float sy = c.y1 - 12;
    label(bm, c.x0 + 8, sy, ellipsize(status_, 20, FontFace::UI, 900), 20, FontFace::UI, t.dim);
    labelRight(bm, c.x1 - 8, sy, std::to_string(w_) + " x " + std::to_string(h_), 20, FontFace::UI, t.muted);
    for (const auto& tp : tips) {
        if (tp.first != hover_) continue;
        const float tw = measureText(tp.second, 20, FontFace::UI) + 28;
        float tx = std::min(hoverX_ + 14, static_cast<float>(bm.w) - tw - 6), ty = hoverY_ + 22;
        if (ty + 40 > bm.h) ty = hoverY_ - 46;
        fillRound(bm, tx, ty, tx + tw, ty + 38, 6, Color8{8, 14, 20, 245});
        strokeRect(bm, tx, ty, tx + tw, ty + 38, 1.5f, t.accent);
        label(bm, tx + 14, ty + 26, tp.second, 20, FontFace::UI, t.text);
    }
}

}  // namespace sf
