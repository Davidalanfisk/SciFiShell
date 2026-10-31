#pragma once
// On-screen keyboard: a floating panel that types into whichever window has the keyboard. It is not a window itself (it never
// takes focus); the shell collects what was pressed with take() and delivers it. Made for the headset and the gamepad.
#include <string>
#include <vector>

#include "PanelApp.h"

namespace sf {

class OnScreenKeyboard : public PanelApp {
public:
    struct Typed {
        std::vector<EditKey> keys;
        std::string text;
        bool escape = false;
    };

    explicit OnScreenKeyboard(Renderer& renderer);
    const char* name() const override { return "KEYBOARD"; }
    // Opens in front of the viewer, a little below the line of sight, or closes when already open.
    void toggle(Vec3 eye, Vec3 forward);
    Typed take();

protected:
    void paint(Bitmap& bm) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    double repaintInterval() const override { return 0.0; }

private:
    enum Act { kChar, kBackspace, kDelete, kEnter, kTab, kEsc, kShift, kCaps, kCtrl, kSpace, kLeft, kRight, kUp, kDown, kClose };
    struct Key {
        std::string lo, hi;
        Act act = kChar;
        float units = 1.0f;
        ui::Rect r;
    };
    void layout();
    void press(const Key& k);

    std::vector<std::vector<Key>> rows_;
    bool laidOut_ = false;
    bool shift_ = false, caps_ = false, ctrl_ = false;
    int hover_ = -1;
    Typed out_;
};

}  // namespace sf
