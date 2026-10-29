#pragma once
// 2D file browser window: breadcrumbs, places and drives, sortable list, search, multi select and the usual file
// operations (new folder, rename, copy, cut, paste, delete to the recycle bin). Files open in the 3D viewer / default
// program, and any folder can be entered as a 3D room.
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "PanelApp.h"

namespace sf {

class FileBrowser : public PanelApp {
public:
    explicit FileBrowser(Renderer& renderer);
    const char* name() const override { return "FILES"; }
    bool wantsText() const override { return edit_ != Edit::None; }
    bool back() override;
    void navigate(const std::string& path, bool remember = true);

    std::function<void(const std::string& path)> openFile;    // viewer / default program
    std::function<void(const std::string& folder)> openRoom;
    std::function<void(const std::string& path)> pickHandler;  // set: the next double-clicked file goes here instead  // enter the folder as a 3D room

protected:
    void onOpen() override;
    void onClose() override { pickHandler = nullptr; }
    void paint(Bitmap& bm) override;
    bool onHover(float x, float y) override;
    void onClick(float x, float y, bool dbl) override;
    void onWheel(float x, float y, float notches) override;
    void onKey(EditKey k) override;
    void onText(const std::string& s) override;
    void onCtrl(const std::string& s) override;
    double repaintInterval() const override { return 0.5; }  // caret blink

private:
    struct Entry {
        std::string name, path, date;
        bool dir = false;
        unsigned long long size = 0, stamp = 0;
        int kind = 0;
    };
    enum class Edit { None, Search, Rename, NewFolder };
    struct Hit {
        ui::Rect r;
        int id;
    };

    void reload();
    void applyFilter();
    void sortEntries();
    void commitEdit();
    void startEdit(Edit e);
    void fileOp(int op, const std::vector<std::string>& from, const std::string& to);
    std::vector<std::string> selectedPaths() const;
    void act(int id);
    int rowAt(float y) const;

    std::string cwd_;
    std::vector<Entry> all_, view_;
    std::vector<std::pair<std::string, std::string>> places_;
    std::vector<std::string> history_;
    std::set<int> sel_;
    int anchor_ = -1, hoverRow_ = -1, sortCol_ = 0;
    bool sortAsc_ = true, showHidden_ = false;
    float scroll_ = 0;
    Edit edit_ = Edit::None;
    std::string buf_, search_, status_, clipOp_;
    std::vector<std::string> clip_;
    int deleteArmed_ = 0;
    std::vector<Hit> hits_;
    ui::Rect list_;
    float rowH_ = 34;
    int hoverHit_ = -1;
    double blink_ = 0;
};

}  // namespace sf
