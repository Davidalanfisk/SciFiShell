#include "TextEditor.h"

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace sf {

namespace fs = std::filesystem;

TextEditor::TextEditor(Renderer& renderer) : PanelApp(renderer, 1480, 1250, 1.9f) { newDocument(); }

void TextEditor::newDocument() {
    ed_.clear();
    saveAs_ = false;
    field_.clear();
    armed_ = 0;
    status_ = "New blank document";
    drawnVersion_ = 0;
    invalidate();
}

bool TextEditor::openPath(const std::string& path) {
    armed_ = 0;
    saveAs_ = false;
    drawnVersion_ = 0;
    const bool ok = ed_.load(path);
    status_ = !ok ? "Could not open the file" : ed_.truncated() ? "Large file - opened read only" : ed_.readOnly() ? "Binary file - opened read only" : "Opened " + fs::u8path(path).filename().u8string();
    invalidate();
    return ok;
}

std::string TextEditor::defaultSavePath() const {
    if (!ed_.path().empty()) return ed_.path();
    const char* up = std::getenv("USERPROFILE");
    std::string dir = up ? std::string(up) + "\\Documents" : "C:";
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(dir), ec)) dir = up ? up : "C:";
    return dir + "\\Untitled.txt";
}

void TextEditor::save() {
    if (ed_.readOnly()) { status_ = "This file is read only"; return; }
    if (ed_.path().empty()) {
        saveAs_ = true;
        field_ = defaultSavePath();
        status_ = "Type a file path and press Enter";
        return;
    }
    status_ = ed_.save() ? "Saved" : "Save failed";
}

void TextEditor::commitSaveAs() {
    const std::string p = field_;
    saveAs_ = false;
    if (p.empty()) return;
    std::error_code ec;
    if (fs::is_directory(fs::u8path(p), ec)) { status_ = "That is a folder - add a file name"; saveAs_ = true; return; }
    const fs::path parent = fs::u8path(p).parent_path();
    if (!parent.empty() && !fs::is_directory(parent, ec)) { status_ = "That folder does not exist"; saveAs_ = true; return; }
    status_ = ed_.saveAs(p) ? "Saved " + fs::u8path(p).filename().u8string() : "Save failed";
}

void TextEditor::act(int id) {
    if (id != 1 && id != 2) armed_ = 0;
    if (id == 1 || id == 2) {  // new / open
        if (ed_.dirty() && armed_ != id) {
            armed_ = id;
            status_ = "Unsaved changes - click again to discard them";
            return;
        }
        armed_ = 0;
        if (id == 1) newDocument();
        else if (requestOpen) requestOpen();
    } else if (id == 3) save();
    else if (id == 4) {
        saveAs_ = true;
        field_ = defaultSavePath();
        status_ = "Type a file path and press Enter";
    }
}

bool TextEditor::onHover(float x, float y) {
    int h = -1;
    for (size_t i = 0; i < hits_.size(); ++i)
        if (hits_[i].r.contains(x, y)) h = static_cast<int>(i);
    if (h == hover_) return false;
    hover_ = h;
    return true;
}

void TextEditor::onClick(float x, float y, bool) {
    for (const Hit& h : hits_)
        if (h.r.contains(x, y)) {
            act(h.id);
            return;
        }
    if (editRect_.contains(x, y)) {
        saveAs_ = false;
        armed_ = 0;
        ed_.click(x - editRect_.x0, y - editRect_.y0);
    }
}

void TextEditor::onWheel(float x, float y, float notches) {
    if (editRect_.contains(x, y)) ed_.scrollBy(-notches * 3.0f);
}

void TextEditor::onCtrl(const std::string& s) {
    if (s != "esc") return;
    if (saveAs_) { saveAs_ = false; status_ = "Save cancelled"; }
    else close();
}

void TextEditor::onText(const std::string& s) {
    if (saveAs_) field_ += s;
    else ed_.insertText(s);
}

void TextEditor::onKey(EditKey k) {
    if (k == EditKey::Save) { save(); return; }
    if (saveAs_) {
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
        return;
    }
    if (ed_.readOnly() && k != EditKey::Up && k != EditKey::Down && k != EditKey::PageUp && k != EditKey::PageDown) return;
    switch (k) {
        case EditKey::Backspace: ed_.backspace(); break;
        case EditKey::Delete: ed_.del(); break;
        case EditKey::Enter: ed_.newline(); break;
        case EditKey::Tab: ed_.tab(); break;
        case EditKey::Left: ed_.moveLeft(); break;
        case EditKey::Right: ed_.moveRight(); break;
        case EditKey::Up: ed_.moveUp(); break;
        case EditKey::Down: ed_.moveDown(); break;
        case EditKey::Home: ed_.home(); break;
        case EditKey::End: ed_.end(); break;
        case EditKey::PageUp: ed_.page(-1); break;
        case EditKey::PageDown: ed_.page(1); break;
        case EditKey::DocStart: ed_.docStart(); break;
        case EditKey::DocEnd: ed_.docEnd(); break;
        case EditKey::Paste: ed_.paste(); break;
        default: break;
    }
}

void TextEditor::paint(Bitmap& bm) {
    using namespace ui;
    const Tokens t = tokens();
    std::string name = ed_.path().empty() ? "UNTITLED" : fs::u8path(ed_.path()).filename().u8string();
    if (ed_.dirty()) name += " *";
    const Rect c = panel(bm, "EDITOR", name);
    hits_.clear();
    auto hot = [&](const Rect& r, int id) {
        hits_.push_back({r, id});
        return hover_ == static_cast<int>(hits_.size()) - 1 ? State::Hover : State::Normal;
    };

    const float ty = c.y0 + 4, th = 48;
    float x = c.x0 + 4;
    struct B { const char* l; int id; float w; };
    for (B b : {B{"NEW", 1, 100}, B{"OPEN", 2, 110}, B{"SAVE", 3, 110}, B{"SAVE AS", 4, 140}}) {
        const Rect r{x, ty, x + b.w, ty + th};
        State s = hot(r, b.id);
        if (armed_ == b.id) s = State::Pressed;
        button(bm, r, b.l, s, b.id == 3, 22);
        x += b.w + 10;
    }
    const Rect fr{x + 10, ty, c.x1 - 4, ty + th};
    card(bm, fr, saveAs_ ? State::Selected : State::Normal);
    if (saveAs_) {
        const bool caret = (SDL_GetTicks() / 500) % 2 == 0;
        label(bm, fr.x0 + 14, fr.y0 + 32, "SAVE TO:", 20, FontFace::UIBold, t.accent);
        label(bm, fr.x0 + 120, fr.y0 + 32, ellipsize(field_, 22, FontFace::Mono, fr.w() - 150) + (caret ? "|" : ""), 22, FontFace::Mono, t.text);
    } else {
        const std::string p = ed_.path().empty() ? "Not saved yet  -  Ctrl+S to save" : ed_.path();
        label(bm, fr.x0 + 14, fr.y0 + 32, ellipsize(p, 20, FontFace::UI, fr.w() - 28), 20, FontFace::UI, t.dim);
    }

    editRect_ = Rect{c.x0 + 4, c.y0 + 62, c.x0 + 4 + CodeEditor::kPxW, c.y0 + 62 + CodeEditor::kPxH};
    const bool caretOn = !saveAs_ && (SDL_GetTicks() / 500) % 2 == 0;
    if (ed_.version() != drawnVersion_ || caretOn != drawnCaret_ || surf_.rgba.empty()) {
        ed_.render(surf_, !saveAs_, caretOn);
        drawnVersion_ = ed_.version();
        drawnCaret_ = caretOn;
    }
    const int ex = static_cast<int>(editRect_.x0), ey = static_cast<int>(editRect_.y0);
    for (int row = 0; row < surf_.h; ++row) {
        const int dy = ey + row;
        if (dy < 0 || dy >= bm.h) continue;
        const int w = std::min(surf_.w, bm.w - ex);
        if (w > 0) std::memcpy(&bm.rgba[(static_cast<size_t>(dy) * bm.w + ex) * 4], &surf_.rgba[static_cast<size_t>(row) * surf_.w * 4], static_cast<size_t>(w) * 4);
    }
    strokeRect(bm, editRect_.x0 - 1, editRect_.y0 - 1, editRect_.x1 + 1, editRect_.y1 + 1, 2, t.border);

    const float sy = editRect_.y1 + 34;
    label(bm, c.x0 + 8, sy, ellipsize(status_, 20, FontFace::UI, 700), 20, FontFace::UI, armed_ ? t.warn : t.dim);
    labelRight(bm, c.x1 - 8, sy,
               "Ln " + std::to_string(ed_.cursorLine() + 1) + ", Col " + std::to_string(ed_.cursorColumn() + 1) + "   |   " + std::to_string(ed_.lineCount()) +
                   " lines   |   " + ed_.language() + (ed_.readOnly() ? "   |   READ ONLY" : ""),
               20, FontFace::UI, t.muted);
}

}  // namespace sf
