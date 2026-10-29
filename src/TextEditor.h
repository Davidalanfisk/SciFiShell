#pragma once
// Text editor window: a blank document by default, with New / Open / Save / Save As. The document model, syntax
// highlighting and rasteriser are the CodeEditor; this panel adds the window chrome, toolbar and file handling.
#include <functional>
#include <string>
#include <vector>

#include "CodeEditor.h"
#include "PanelApp.h"

namespace sf {

class TextEditor : public PanelApp {
public:
    explicit TextEditor(Renderer& renderer);
    const char* name() const override { return "EDITOR"; }
    bool wantsText() const override { return true; }
    bool keepAlive() const override { return false; }
    void newDocument();
    bool openPath(const std::string& path);

    std::function<void()> requestOpen;  // the shell shows the file browser in pick mode

protected:
    void paint(Bitmap& bm) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    void onWheel(float x, float y, float notches) override;
    void onKey(EditKey k) override;
    void onText(const std::string& s) override;
    void onCtrl(const std::string& s) override;
    double repaintInterval() const override { return 0.5; }

private:
    struct Hit {
        ui::Rect r;
        int id;
    };
    void act(int id);
    void save();
    void commitSaveAs();
    std::string defaultSavePath() const;

    CodeEditor ed_;
    Bitmap surf_;
    uint64_t drawnVersion_ = 0;
    bool drawnCaret_ = false;
    bool saveAs_ = false;
    std::string field_, status_;
    int armed_ = 0;  // a destructive action (new / open) waiting for a second click while there are unsaved changes
    std::vector<Hit> hits_;
    int hover_ = -1;
    ui::Rect editRect_;
};

}  // namespace sf
