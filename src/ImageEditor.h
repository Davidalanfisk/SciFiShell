#pragma once
// Image editor window: opens a picture and offers rotate, flip, crop, resize and a simple paintbrush, saves the result,
// and has a screenshot timer that grabs the shell after a countdown (the window steps aside first).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "PanelApp.h"

namespace sf {

class ImageEditor : public PanelApp {
public:
    explicit ImageEditor(Renderer& renderer);
    const char* name() const override { return "IMAGE"; }
    bool wantsText() const override { return saveAs_ || textOn_; }
    bool keepAlive() const override { return timerOn_ || shotWait_ > 0; }
    bool openPath(const std::string& path);
    void newBlank(int w, int h);
    void command(int id) { act(id); invalidate(); }

    std::function<void()> requestOpen;                      // the shell shows the file browser in pick mode
    std::function<void(const std::string& out)> takeShot;   // the shell saves a screenshot to this path
    std::function<void()> restore;                          // the shell brings this window back after a timed shot

protected:
    void paint(Bitmap& bm) override;
    void tick(double dt) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    void onWheel(float x, float y, float notches) override;
    void onRelease() override { panning_ = false; }
    void onKey(EditKey k) override;
    void onText(const std::string& s) override;
    void onCtrl(const std::string& s) override;
    double repaintInterval() const override { return timerOn_ ? 0.25 : 0.0; }
    bool fsResizable() const override { return true; }
    void onResize(int, int) override { viewDirty_ = true; }

private:
    enum class Tool { None, Crop, Resize, Brush, Eraser, Fill, Pick, Line, Rect, Oval, Text, Shot, Pan };
    struct Hit {
        ui::Rect r;
        int id;
    };
    void act(int id);
    void pushUndo();
    void rotate(bool right);
    void flip();
    void cropTo(int x0, int y0, int x1, int y1);
    void resizePct(int pct);
    void stamp(float ix, float iy, Color8 c, float r);
    float brushRadius() const;
    void drawLinePx(float x0, float y0, float x1, float y1, Color8 c, float r);
    void drawShape(Tool t, float x0, float y0, float x1, float y1);
    void floodFill(int x, int y, Color8 c);
    void adjust(int id);
    void flipV();
    void redo();
    void commitText();
    bool isDrawTool() const { return tool_ >= Tool::Brush && tool_ <= Tool::Text; }
    void stroke(float ix, float iy);
    bool toImage(float x, float y, float& ix, float& iy) const;
    void save();
    void commitSaveAs();
    void renderView(int vw, int vh);

    int w_ = 0, h_ = 0;
    struct Snap {
        std::vector<uint8_t> px;
        int w = 0, h = 0;
    };
    std::vector<uint8_t> px_;
    std::vector<Snap> undo_, redo_;
    // two-click shapes
    int shapeN_ = 0;
    float sx_ = 0, sy_ = 0;
    bool filled_ = false;
    // text tool
    bool textOn_ = false;
    std::string textBuf_;
    float tx_ = 0, ty_ = 0;
    std::string path_, field_, status_;
    bool dirty_ = false, saveAs_ = false;
    Tool tool_ = Tool::None;
    // crop: two corners in image pixels
    int cropN_ = 0;
    float cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;
    // brush
    int colour_ = 0, size_ = 1;
    bool penDown_ = false;
    float lastX_ = -1, lastY_ = -1;
    // screenshot timer
    int delay_ = 5;
    bool timerOn_ = false;
    double timerLeft_ = 0, shotWait_ = 0;
    int stage_ = 0;
    std::string shotPath_;
    // view
    Bitmap surf_;
    bool viewDirty_ = true;
    ui::Rect view_;
    float scale_ = 1, ox_ = 0, oy_ = 0;
    bool panning_ = false;
    float panLastX_ = 0, panLastY_ = 0;
    float fit_ = 1, zoom_ = 1, panX_ = 0, panY_ = 0;  // zoom relative to fit-to-view; pan in view pixels
    std::vector<Hit> hits_;
    int hover_ = -1;
    float hoverX_ = -1, hoverY_ = -1;
};

}  // namespace sf
