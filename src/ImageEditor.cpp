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
    status_ = "New blank canvas - open a picture or paint";
}

void ImageEditor::newBlank(int w, int h) {
    w_ = w;
    h_ = h;
    px_.assign(static_cast<size_t>(w) * h * 4, 255);
    undo_.clear();
    path_.clear();
    dirty_ = false;
    tool_ = Tool::None;
    cropN_ = 0;
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
    h_ = h;
    px_.assign(p, p + static_cast<size_t>(w) * h * 4);
    stbi_image_free(p);
    undo_.clear();
    path_ = path;
    dirty_ = false;
    tool_ = Tool::None;
    cropN_ = 0;
    saveAs_ = false;
    status_ = "Opened " + fs::u8path(path).filename().u8string();
    viewDirty_ = true;
    invalidate();
    return true;
}

void ImageEditor::pushUndo() {
    undo_ = px_;
    undoW_ = w_;
    undoH_ = h_;
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
    h_ = nh;
    dirty_ = true;
    viewDirty_ = true;
    status_ = "Resized to " + std::to_string(nw) + " x " + std::to_string(nh);
}

void ImageEditor::stamp(float ix, float iy) {
    const float r = static_cast<float>(kBrush[size_ - 1]) * std::max(1.0f, std::max(w_, h_) / 800.0f);
    const Color8 c = kPalette[colour_];
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
    if (lastX_ >= 0) {
        const float dx = ix - lastX_, dy = iy - lastY_, d = std::sqrt(dx * dx + dy * dy);
        const int n = std::max(1, static_cast<int>(d / 2.0f));
        for (int i = 1; i <= n; ++i) stamp(lastX_ + dx * i / n, lastY_ + dy * i / n);
    } else stamp(ix, iy);
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
        px_.swap(undo_);
        std::swap(w_, undoW_);
        std::swap(h_, undoH_);
        viewDirty_ = true;
        dirty_ = true;
        status_ = "Undone";
    } else if (id == 10) rotate(false);
    else if (id == 11) rotate(true);
    else if (id == 12) flip();
    else if (id >= 13 && id <= 16) {
        tool_ = id == 13 ? Tool::Crop : id == 14 ? Tool::Resize : id == 15 ? Tool::Brush : Tool::Shot;
        cropN_ = 0;
        penDown_ = false;
        lastX_ = -1;
        status_ = tool_ == Tool::Crop ? "Click two opposite corners of the area to keep"
                : tool_ == Tool::Brush ? "Click to start painting, move, click again to lift the brush"
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
    float ix, iy;
    if (tool_ == Tool::Brush && penDown_ && toImage(x, y, ix, iy)) {
        stroke(ix, iy);
        return true;
    }
    if ((tool_ == Tool::Crop && cropN_ == 1) || tool_ == Tool::Brush) changed = true;
    return changed;
}

void ImageEditor::onClick(float x, float y, bool) {
    for (const Hit& h : hits_)
        if (h.r.contains(x, y)) {
            act(h.id);
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
    } else if (tool_ == Tool::Brush) {
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
    } else if (penDown_) {
        penDown_ = false;
        lastX_ = -1;
    } else if (cropN_) cropN_ = 0;
    else close();
}

void ImageEditor::onText(const std::string& s) {
    if (saveAs_) field_ += s;
}

void ImageEditor::onKey(EditKey k) {
    if (k == EditKey::Save) {
        save();
        return;
    }
    if (!saveAs_) return;
    if (k == EditKey::Enter) commitSaveAs();
    else if (k == EditKey::Backspace) {
        while (!field_.empty() && (field_.back() & 0xC0) == 0x80) field_.pop_back();
        if (!field_.empty()) field_.pop_back();
    } else if (k == EditKey::Paste) {
        if (char* t = SDL_GetClipboardText()) {
            for (const char* c = t; *c; ++c)
                if (static_cast<unsigned char>(*c) >= 0x20) field_ += *c;
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
    scale_ = std::min(8.0f, std::min(static_cast<float>(vw - 8) / w_, static_cast<float>(vh - 8) / h_));
    const float dw = w_ * scale_, dh = h_ * scale_;
    const float lx = (vw - dw) * 0.5f, ly = (vh - dh) * 0.5f;
    ox_ = view_.x0 + lx;
    oy_ = view_.y0 + ly;
    for (int y = std::max(0, static_cast<int>(ly)); y < std::min(vh, static_cast<int>(ly + dh)); ++y) {
        const int sy = std::min(h_ - 1, static_cast<int>((y - ly) / scale_));
        for (int x = std::max(0, static_cast<int>(lx)); x < std::min(vw, static_cast<int>(lx + dw)); ++x) {
            const int sx = std::min(w_ - 1, static_cast<int>((x - lx) / scale_));
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
        float w;
    };
    auto row = [&](float y, std::initializer_list<B> items, int sel) {
        float x = c.x0 + 4;
        for (const B& b : items) {
            const Rect r{x, y, x + b.w, y + 46};
            State s = hot(r, b.id);
            if (b.id == sel) s = State::Selected;
            button(bm, r, b.l, s, false, 20);
            x += b.w + 8;
        }
    };

    float y = c.y0 + 4;
    row(y, {{"OPEN", 1, 100}, {"SAVE", 2, 100}, {"SAVE AS", 3, 130}, {"UNDO", 4, 100}}, -1);
    const Rect fr{c.x0 + 460, y, c.x1 - 4, y + 46};
    card(bm, fr, saveAs_ ? State::Selected : State::Normal);
    if (saveAs_) {
        const bool caret = (SDL_GetTicks() / 500) % 2 == 0;
        label(bm, fr.x0 + 14, fr.y0 + 31, "SAVE TO:", 20, FontFace::UIBold, t.accent);
        label(bm, fr.x0 + 120, fr.y0 + 31, ellipsize(field_, 22, FontFace::Mono, fr.w() - 150) + (caret ? "|" : ""), 22, FontFace::Mono, t.text);
    } else {
        label(bm, fr.x0 + 14, fr.y0 + 31, ellipsize(path_.empty() ? "Not saved yet  -  Ctrl+S to save" : path_, 20, FontFace::UI, fr.w() - 28), 20, FontFace::UI, t.dim);
    }
    y += 54;
    const int toolId = tool_ == Tool::Crop ? 13 : tool_ == Tool::Resize ? 14 : tool_ == Tool::Brush ? 15 : tool_ == Tool::Shot ? 16 : -1;
    row(y, {{"ROTATE L", 10, 140}, {"ROTATE R", 11, 140}, {"FLIP", 12, 100}, {"CROP", 13, 100}, {"RESIZE", 14, 120}, {"BRUSH", 15, 120}, {"SHOT TIMER", 16, 170}}, toolId);
    y += 54;
    float x = c.x0 + 4;
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
    } else if (tool_ == Tool::Brush) {
        for (int i = 0; i < 8; ++i) {
            const Rect r{x, y, x + 46, y + 46};
            const State s = hot(r, 40 + i);
            fillRound(bm, r.x0 + 4, r.y0 + 4, r.x1 - 4, r.y1 - 4, 8, kPalette[i]);
            strokeRect(bm, r.x0 + 1, r.y0 + 1, r.x1 - 1, r.y1 - 1, i == colour_ ? 3.0f : 1.0f, i == colour_ ? t.accent : (s == State::Hover ? t.text : t.border));
            x += 54;
        }
        x += 16;
        int i = 1;
        for (const char* l : {"S", "M", "L"}) {
            const Rect r{x, y, x + 60, y + 46};
            State s = hot(r, 49 + i);
            if (i == size_) s = State::Selected;
            button(bm, r, l, s, false, 20);
            x += 68;
            ++i;
        }
        label(bm, x + 10, y + 31, penDown_ ? "PAINTING - click to lift" : "Click to start a stroke", 20, FontFace::UI, penDown_ ? t.accent : t.dim);
    } else if (tool_ == Tool::Shot) {
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

    view_ = Rect{c.x0 + 4, y + 56, c.x1 - 4, c.y1 - 44};
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
    if (tool_ == Tool::Brush) {
        float ix, iy;
        if (toImage(hoverX_, hoverY_, ix, iy)) {
            const float r = kBrush[size_ - 1] * std::max(1.0f, std::max(w_, h_) / 800.0f) * scale_;
            strokeRect(bm, hoverX_ - r, hoverY_ - r, hoverX_ + r, hoverY_ + r, 1.5f, t.accent);
        }
    }

    const float sy = c.y1 - 12;
    label(bm, c.x0 + 8, sy, ellipsize(status_, 20, FontFace::UI, 900), 20, FontFace::UI, t.dim);
    labelRight(bm, c.x1 - 8, sy, std::to_string(w_) + " x " + std::to_string(h_), 20, FontFace::UI, t.muted);
}

}  // namespace sf
