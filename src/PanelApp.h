#pragma once
// Base class for the floating bitmap panels that behave like desktop windows in the 3D world (system monitor, 2D file
// browser, terminal). It owns the glass frame, placement, ray -> pixel mapping, window buttons and the texture upload;
// a subclass only paints its content and reacts to clicks, keys and text.
#include <memory>
#include <string>

#include "InputSystem.h"
#include "Renderer.h"
#include "UiKit.h"
#include "Utils.h"

namespace sf {

class PanelApp {
public:
    PanelApp(Renderer& renderer, int w, int h, float worldW = 2.0f);
    virtual ~PanelApp() = default;

    virtual const char* name() const = 0;

    void open(Vec3 eye, Vec3 forward);
    void close();  // closes for good (the app is reset next time)
    void hide();   // minimise: the window disappears but keeps its state; open() brings it back
    bool isOpen() const { return open_; }
    bool hidden() const { return hidden_; }
    bool exists() const { return open_ || hidden_; }
    void leave();                      // the pointer is on another window
    void releaseButton() { dragging_ = false; }
    bool busy() const { return dragging_; }
    bool overTitle() const { return open_ && !fullscreen_ && mx_ >= 0 && my_ < 86 && hoverBtn_ == 0; }
    bool fullscreen() const { return fullscreen_; }
    void toggleFullscreen();
    void setView(float tanHalf, float aspect) { viewTan_ = tanHalf; viewAspect_ = aspect; }

    void pointer(Vec3 origin, Vec3 dir);                  // hover
    bool click(Vec3 origin, Vec3 dir);                    // true when the click landed on the panel
    void wheel(float notches);                            // + up
    void key(EditKey k) { onKey(k); invalidate(); }
    void text(const std::string& s) { onText(s); invalidate(); }
    void ctrl(const std::string& s) { onCtrl(s); invalidate(); }
    virtual bool wantsText() const { return false; }      // a text field has focus: the keyboard belongs to the panel
    virtual bool back() { return false; }                 // Esc / back: true when handled inside the panel

    void update(double dt);   // runs even while the panel is closed if keepAlive() (e.g. a terminal session)
    void render();
    virtual bool keepAlive() const { return false; }
    float hitDistance(Vec3 origin, Vec3 dir) const;       // distance to the panel (margin included), or -1
    bool bounds(Vec3& centre, Vec3& normal, float& w, float& h) const;
    void invalidate() { dirty_ = true; }

protected:
    virtual void onOpen() {}
    virtual void onResize(int /*w*/, int /*h*/) {}  // the bitmap changed size (fullscreen enter / exit)
    virtual bool fsResizable() const { return false; }  // true: fullscreen re-lays the bitmap out at screen resolution
    virtual void tick(double /*dt*/) {}
    virtual void paint(Bitmap& bm) = 0;
    virtual bool onHover(float /*x*/, float /*y*/) { return false; }  // true when the look changed
    virtual void onClick(float /*x*/, float /*y*/, bool /*dbl*/) {}
    virtual void onWheel(float /*x*/, float /*y*/, float /*notches*/) {}
    virtual void onKey(EditKey) {}
    virtual void onText(const std::string&) {}
    virtual void onCtrl(const std::string&) {}
    virtual void onClose() {}
    virtual bool chromeless() const { return false; }  // fullscreen without the window buttons (video cinema)
    virtual double repaintInterval() const { return 0.0; }  // > 0: also repaint on a timer (animated content)

    int pw() const { return w_; }
    int ph() const { return h_; }
    float mx() const { return mx_; }
    float my() const { return my_; }
    std::string subtitle_;
    std::string title_;

    Renderer& renderer_;

private:
    bool toPanel(Vec3 o, Vec3 d, float& px, float& py, float& t) const;
    void applyFullscreen();
    void repaint();

    int w_, h_;
    int savedW_ = 0, savedH_ = 0;
    float worldW_, baseWorldW_;
    bool open_ = false, hidden_ = false, fullscreen_ = false, dirty_ = true;
    bool dragging_ = false;
    float grabX_ = 0, grabY_ = 0, grabDist_ = 1.7f;
    float mx_ = -1, my_ = -1;
    int hoverBtn_ = 0;
    double sincePaint_ = 0, lastClick_ = -10, clock_ = 0;
    float lastClickX_ = 0, lastClickY_ = 0;
    ui::Rect fs_, close_, hide_;
    Mat4 base_, savedBase_;
    Vec3 fallbackFlat_{0, 0, -1};
    float viewTan_ = 0.577f, viewAspect_ = 1.78f;
    Bitmap bm_;
    std::shared_ptr<GpuLabel> label_;
    std::string themeSig_;
};

}  // namespace sf
