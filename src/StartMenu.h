#pragma once
// The Start menu: a normal floating window (so it carries, moves, resizes and arranges like every other window) that
// launches the built-in tools, lists the running windows, arranges them and holds the power options.
#include <functional>
#include <string>
#include <vector>

#include "PanelApp.h"

namespace sf {

enum class Tool { Browser = 0, Editor, Media, Settings, Theme, Monitor, FileBrowser, Terminal, Image, Ftp, Calc };

struct StartHost {
    struct Row {
        std::string name;
        int state = 0;  // 0 open, 1 minimised, 2 has focus
    };
    std::function<void(Tool)> tool;
    std::function<void(int)> focusRow;   // index into rows()
    std::function<void(const std::string&)> action;  // arc | tile | min | exit | restart | shutdown | lock | sleep
    std::function<std::vector<Row>()> rows;
};

class StartMenu : public PanelApp {
public:
    StartMenu(Renderer& renderer, StartHost host);
    const char* name() const override { return "START"; }
    void focusSearch() { searchFocus_ = true; invalidate(); }
    bool wantsText() const override { return searchFocus_; }
    bool back() override;
    void scriptPower() { powerOpen_ = true; invalidate(); }

protected:
    void onOpen() override;
    void paint(Bitmap& bm) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    void onKey(EditKey k) override;
    void onText(const std::string& s) override;
    void onCtrl(const std::string& s) override;
    double repaintInterval() const override { return 1.0; }

private:
    enum HitType { kTool = 1, kRow, kAct, kSearch, kPowerToggle };
    enum Act { kArc = 1, kTile, kMin, kExit, kRestart, kShut, kLock, kSleep, kClear };
    struct Hit {
        ui::Rect r;
        int type, arg;
    };
    struct Result {
        std::string name;
        int type, arg;
    };
    std::vector<Result> results() const;
    void activate(const Hit& h);
    void doAct(int a);
    int hitAt(float x, float y) const;

    StartHost host_;
    std::vector<Hit> hits_;
    std::vector<StartHost::Row> rows_;
    std::string query_;
    bool searchFocus_ = false, powerOpen_ = false;
    int armed_ = 0;  // 1 restart / 2 shut down waiting for a second click
    int hover_ = -1;
};

}  // namespace sf
