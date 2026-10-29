#pragma once
// LCARS-style UI: animated rounded panels rendered as quads in 3D space, plus the main menu.
#include <functional>
#include <memory>
#include <string>

#include "SceneGraph.h"

namespace sf {

class PanelNode : public Node {
public:
    PanelNode(Renderer& renderer, const std::string& text, Vec3 color, float width, float height, int segments);
    void setText(const std::string& text);
    void setColor(Vec3 c) { color_ = c; }
    void setSelected(bool s) { selected_ = s; }
    void update(double dt, double time) override;
    void render(Renderer& renderer) override;
    bool isTransparent() const override { return true; }
    std::function<void()> onActivate;

private:
    Renderer& renderer_;
    std::shared_ptr<GpuLabel> label_;
    Vec3 color_;
    int segments_;
    bool selected_ = false;
    float selectBlend_ = 0;
    float time_ = 0;
};

class LCARSMenu {
public:
    struct Callbacks {
        std::function<void()> enterFileRoom, toggleVR, exitShell;
    };
    LCARSMenu(Renderer& renderer, Callbacks callbacks);

    Node& root() { return *root_; }
    void moveSelection(int delta);
    void select(Node* node);       // hover; ignored for non-item nodes
    void activateSelected();
    void activate(Node* node);
    void setVRState(bool on);
    void setStatus(const std::string& text);

private:
    std::unique_ptr<Node> rootHolder_;
    Node* root_;
    PanelNode* status_ = nullptr;
    PanelNode* vrItem_ = nullptr;
    std::vector<PanelNode*> items_;
    int selected_ = 0;
};

}  // namespace sf
