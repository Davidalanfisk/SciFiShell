#pragma once
// A floating glass settings panel: a title plus a column of pickable rows. Each row's text is set by the
// owner; activating a row calls onRow(index).
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "LCARSUI.h"

namespace sf {

class SettingsPanel {
public:
    SettingsPanel(Renderer& renderer, const std::vector<std::string>& rows, const std::string& title = "SYSTEM SETTINGS");

    void open(Vec3 eye, Vec3 forward);
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    void setRowText(int i, const std::string& text);
    void pointer(Vec3 origin, Vec3 dir);  // hover by ray
    void moveSelection(int delta);
    void activateSelected();
    bool activateHit(Vec3 origin, Vec3 dir);  // click: activates the row under the ray, if any

    void update(double dt, double time);
    Node& root() { return *root_; }

    std::function<void(int)> onRow;

private:
    void select(int i);
    Node* root_ = nullptr;
    std::unique_ptr<Node> rootHolder_;
    std::vector<PanelNode*> rows_;
    int sel_ = 0;
    bool open_ = false;
    Vec3 fallbackFlat_{0, 0, -1};
};

}  // namespace sf
