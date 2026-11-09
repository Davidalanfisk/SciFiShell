#pragma once
// Card based settings hub: a home page of category cards, each opening a page of option cards.
// Shell preferences and Windows (control panel replacement) options live side by side. Rendered as one
// floating glass panel; hover by ray, click / select to activate.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Renderer.h"
#include "Utils.h"
#include "SettingsModel.h"

namespace sf {

class SettingsHub {
public:
    explicit SettingsHub(Renderer& renderer) : renderer_(renderer) {}
    const std::vector<SettingsCategory>& categories() const { return cats_; }

    void setCategories(std::vector<SettingsCategory> cats) {
        cats_ = std::move(cats);
        dirty_ = true;
    }
    void setNotice(std::string text) { notice_ = std::move(text); dirty_ = true; }
    const std::string& notice() const { return notice_; }

    void open(Vec3 eye, Vec3 forward);
    void close();
    void openPage(const std::string& name) {
        for (size_t i = 0; i < cats_.size(); ++i)
            if (cats_[i].name == name) { page_ = static_cast<int>(i); sel_ = 0; scrollRow_ = 0; notice_.clear(); dirty_ = true; }
    }
    bool isOpen() const { return open_; }
    void hide();  // minimise: keeps the current page; open() brings it back
    bool hidden() const { return hidden_; }
    bool exists() const { return open_ || hidden_; }
    void leave() { hoverBtn_ = 0; }
    void releaseButton() { dragging_ = false; sliderDrag_ = -1; }
    bool busy() const { return dragging_ || sliderDrag_ >= 0; }
    bool nudge(int dir);                       // left/right on a selected slider; false if not a slider
    void setSliderFromPx(int item, float px);
    bool overTitle() const { return open_ && !fullscreen_ && hoverTitle_; }
    bool fullscreen() const { return fullscreen_; }
    Mat4 placement() const { return base_; }
    void setPlacement(const Mat4& m) { base_ = m; savedBase_ = m; }
    float worldWidth() const { return worldW_; }
    void setWorldWidth(float w) { worldW_ = userW_ = w; }
    bool moving() const { return dragging_; }
    void pushPull(float d) { grabDist_ = clampf(grabDist_ + d, 0.8f, 8.0f); }
    void toggleFullscreen();
    void setView(float tanHalf, float aspect) { viewTan_ = tanHalf; viewAspect_ = aspect; }

    void pointer(Vec3 origin, Vec3 dir);       // hover by ray
    void move(int dx, int dy);                 // directional navigation (keys / pad)
    void step(int delta);                      // linear navigation (wheel)
    void activateSelected();
    bool scriptActivate(const std::string& category, const std::string& title);
    bool activateHit(Vec3 origin, Vec3 dir);   // click: activates the card under the ray, if any
    bool back();                               // category page -> home; false when already home

    void update(double dt, double time);
    void render();
    float hitDistance(Vec3 origin, Vec3 dir) const;  // distance to the panel (margin included), or -1
    bool bounds(Vec3& centre, Vec3& normal, float& w, float& h) const;
    struct Rect {
        float x0, y0, x1, y1;
    };
    std::vector<std::string> vals_;  // live values of the current page, refreshed a couple of times a second
    double valueTimer_ = 0;
    void refreshValues();
    void rebuild();
    std::string signature() const;
    int hitIndex(Vec3 o, Vec3 d) const;
    bool toPanel(Vec3 o, Vec3 d, float& px, float& py, float& t) const;
    int hitButton(Vec3 o, Vec3 d) const;  // 1 fullscreen, 2 close, 0 none
    void applyFullscreen();
    void ensureSelectionVisible();
    int W_ = 1832, H_ = 960;  // bitmap size: fullscreen widens it to the screen's shape

    Rect fsRect_{0, 0, 0, 0}, closeRect_{0, 0, 0, 0}, hideRect_{0, 0, 0, 0};
    bool hidden_ = false, dragging_ = false, hoverTitle_ = false;
    float grabX_ = 0, grabY_ = 0, grabDist_ = 2.0f;
    int hoverBtn_ = 0;
    int sliderDrag_ = -1;
    int drawnHover_ = 0;
    bool fullscreen_ = false;
    Mat4 savedBase_;
    float worldW_ = 2.0f, userW_ = 2.0f;
    float viewTan_ = 0.577f, viewAspect_ = 1.78f;

    Renderer& renderer_;
    std::vector<SettingsCategory> cats_;
    std::vector<Rect> rects_;
    std::shared_ptr<GpuLabel> label_;
    Mat4 base_;
    std::string sig_;
    std::string notice_;
    float aspect_ = 1.9f;
    int page_ = -1;  // -1 home, otherwise a category index
    int sel_ = 0;
    int homeSel_ = 0;
    int scrollRow_ = 0;
    bool open_ = false;
    bool dirty_ = true;
    Vec3 fallbackFlat_{0, 0, -1};
};

}  // namespace sf
