#include "OnScreenKeyboard.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace sf {

namespace {
constexpr int kW = 1500, kH = 640;
}

OnScreenKeyboard::OnScreenKeyboard(Renderer& renderer) : PanelApp(renderer, kW, kH, 1.35f) { title_ = "KEYBOARD"; }

void OnScreenKeyboard::toggle(Vec3 eye, Vec3 forward) {
    if (isOpen()) {
        close();
        return;
    }
    shift_ = ctrl_ = false;
    Vec3 f = normalize(forward);
    // Sit below the line of sight, like a keyboard on a desk.
    open(eye, normalize(Vec3{f.x, f.y - 0.38f, f.z}));
}

OnScreenKeyboard::Typed OnScreenKeyboard::take() {
    Typed t = std::move(out_);
    out_ = Typed{};
    return t;
}

void OnScreenKeyboard::layout() {
    if (laidOut_) return;
    laidOut_ = true;
    auto sp = [](const char* label, Act a, float u) {
        Key k;
        k.lo = k.hi = label;
        k.act = a;
        k.units = u;
        return k;
    };
    auto run = [&](std::vector<Key>& row, const std::string& lo, const std::string& hi) {
        for (size_t i = 0; i < lo.size(); ++i) {
            Key k;
            k.lo = std::string(1, lo[i]);
            k.hi = std::string(1, hi[i]);
            row.push_back(k);
        }
    };
    std::vector<Key> r;
    run(r, "`1234567890-=", "~!@#$%^&*()_+");
    r.push_back(sp("BACKSPACE", kBackspace, 2.0f));
    rows_.push_back(r);
    r.clear();
    r.push_back(sp("TAB", kTab, 1.5f));
    run(r, "qwertyuiop[]", "QWERTYUIOP{}");
    Key bs;
    bs.lo = "\\";
    bs.hi = "|";
    bs.units = 1.5f;
    r.push_back(bs);
    rows_.push_back(r);
    r.clear();
    r.push_back(sp("CAPS", kCaps, 1.75f));
    run(r, "asdfghjkl;'", "ASDFGHJKL:\"");
    r.push_back(sp("ENTER", kEnter, 2.25f));
    rows_.push_back(r);
    r.clear();
    r.push_back(sp("SHIFT", kShift, 2.25f));
    run(r, "zxcvbnm,./", "ZXCVBNM<>?");
    r.push_back(sp("UP", kUp, 1.25f));
    r.push_back(sp("DEL", kDelete, 1.5f));
    rows_.push_back(r);
    r.clear();
    r.push_back(sp("CTRL", kCtrl, 1.5f));
    r.push_back(sp("ESC", kEsc, 1.5f));
    r.push_back(sp("SPACE", kSpace, 6.5f));
    r.push_back(sp("LEFT", kLeft, 1.25f));
    r.push_back(sp("DOWN", kDown, 1.25f));
    r.push_back(sp("RIGHT", kRight, 1.25f));
    r.push_back(sp("DONE", kClose, 1.75f));
    rows_.push_back(r);
}

void OnScreenKeyboard::paint(Bitmap& bm) {
    using namespace ui;
    layout();
    const Rect c = panel(bm, "KEYBOARD", ctrl_ ? "CTRL + ..." : (caps_ ? "CAPS LOCK" : "POINT AND CLICK A KEY"));
    const float gap = 8.0f, pad = 22.0f;
    const float areaW = c.w() - pad * 2, areaH = c.h() - pad * 2;
    const float rowH = (areaH - gap * (rows_.size() - 1)) / rows_.size();
    int idx = 0;
    for (size_t ri = 0; ri < rows_.size(); ++ri) {
        auto& row = rows_[ri];
        float units = 0;
        for (const Key& k : row) units += k.units;
        const float u = (areaW - gap * (row.size() - 1)) / units;
        float x = c.x0 + pad;
        const float y = c.y0 + pad + ri * (rowH + gap);
        for (Key& k : row) {
            const float w = u * k.units;
            k.r = {x, y, x + w, y + rowH};
            const bool upper = shift_ != caps_;
            const bool on = (k.act == kShift && shift_) || (k.act == kCaps && caps_) || (k.act == kCtrl && ctrl_);
            const State s = on ? State::Selected : (idx == hover_ ? State::Hover : State::Normal);
            std::string label = (k.act == kChar && upper) ? k.hi : k.lo;
            if (k.act == kLeft) label = "<";
            else if (k.act == kRight) label = ">";
            else if (k.act == kUp) label = "^";
            else if (k.act == kDown) label = "v";
            button(bm, k.r, label, s, k.act == kEnter || k.act == kClose, k.act == kChar ? 34.0f : 20.0f);
            x += w + gap;
            ++idx;
        }
    }
}

bool OnScreenKeyboard::onHover(float x, float y) {
    int h = -1, idx = 0;
    for (auto& row : rows_)
        for (Key& k : row) {
            if (k.r.contains(x, y)) h = idx;
            ++idx;
        }
    if (h == hover_) return false;
    hover_ = h;
    return true;
}

void OnScreenKeyboard::onClick(float x, float y, bool) {
    for (auto& row : rows_)
        for (const Key& k : row)
            if (k.r.contains(x, y)) {
                press(k);
                return;
            }
}

void OnScreenKeyboard::press(const Key& k) {
    switch (k.act) {
        case kChar: {
            const bool upper = shift_ != caps_;
            if (ctrl_ && k.lo.size() == 1 && std::isalpha(static_cast<unsigned char>(k.lo[0]))) {
                const char l = k.lo[0];
                if (l == 'c') out_.keys.push_back(EditKey::Copy);
                else if (l == 'x') out_.keys.push_back(EditKey::Cut);
                else if (l == 'v') out_.keys.push_back(EditKey::Paste);
                else if (l == 'a') out_.keys.push_back(EditKey::SelectAll);
                else if (l == 'z') out_.keys.push_back(EditKey::Undo);
                else if (l == 's') out_.keys.push_back(EditKey::Save);
            } else {
                out_.text += upper ? k.hi : k.lo;
            }
            shift_ = ctrl_ = false;
            break;
        }
        case kSpace: out_.text += " "; shift_ = false; break;
        case kBackspace: out_.keys.push_back(EditKey::Backspace); break;
        case kDelete: out_.keys.push_back(EditKey::Delete); break;
        case kEnter: out_.keys.push_back(EditKey::Enter); break;
        case kTab: out_.keys.push_back(EditKey::Tab); break;
        case kLeft: out_.keys.push_back(EditKey::Left); break;
        case kRight: out_.keys.push_back(EditKey::Right); break;
        case kUp: out_.keys.push_back(EditKey::Up); break;
        case kDown: out_.keys.push_back(EditKey::Down); break;
        case kEsc: out_.escape = true; break;
        case kShift: shift_ = !shift_; break;
        case kCaps: caps_ = !caps_; break;
        case kCtrl: ctrl_ = !ctrl_; break;
        case kClose: close(); break;
    }
}

}  // namespace sf
