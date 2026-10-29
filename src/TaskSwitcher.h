#pragma once
// Alt+Tab style task switcher: a head-locked strip of cards, one per running task, with the selection highlighted.
#include <memory>
#include <string>
#include <vector>

#include "Renderer.h"
#include "Utils.h"

namespace sf {

class TaskSwitcher {
public:
    struct Item {
        int id = 0;            // caller-defined identifier returned by selectedId()
        std::string name;      // card title
        std::string detail;    // small line under the title
        bool current = false;  // the task the user is in right now
    };

    explicit TaskSwitcher(Renderer& renderer) : renderer_(renderer) {}

    // Opens with the first task after the current one selected.
    void open(std::vector<Item> items, Vec3 accent);
    void step(int dir);
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    int selectedId() const { return items_.empty() ? -1 : items_[static_cast<size_t>(selected_)].id; }

    // view: the matrix the scene is drawn with (the strip is locked to it).
    void render(const Mat4& view, float aspect, float tanHalfFov, bool vr);

private:
    void rebuild();

    Renderer& renderer_;
    std::vector<Item> items_;
    Vec3 accent_{0, 1, 1};
    int selected_ = 0;
    bool open_ = false;
    std::shared_ptr<GpuLabel> label_;
    float aspect_ = 1;
};

}  // namespace sf
