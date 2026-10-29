#include "LCARSUI.h"

#include <algorithm>

#include "TextRender.h"

namespace sf {

namespace {
Bitmap labelBitmap(const std::string& text) { return makeLabel(text, 54.0f, FontFace::UI, Color8{}, 96, 4); }
}  // namespace

PanelNode::PanelNode(Renderer& renderer, const std::string& text, Vec3 color, float width, float height, int segments)
    : renderer_(renderer), color_(color), segments_(segments) {
    scale = {width, height, 1};
    name = text;
    label_ = renderer_.createLabel(labelBitmap(text));
}

void PanelNode::setText(const std::string& text) {
    name = text;
    label_ = renderer_.createLabel(labelBitmap(text));
}

void PanelNode::update(double dt, double time) {
    time_ = static_cast<float>(time);
    float target = selected_ ? 1.0f : 0.0f;
    selectBlend_ += (target - selectBlend_) * std::min(1.0f, static_cast<float>(dt) * 10.0f);
}

void PanelNode::render(Renderer& renderer) {
    PanelParams p;
    p.time = time_;
    p.aspect = scale.x / scale.y;
    p.radius = 0.35f;
    p.selected = selectBlend_;
    p.segments = static_cast<float>(segments_);
    renderer.drawPanel(*label_, worldMatrix(), color_, p);
}

LCARSMenu::LCARSMenu(Renderer& renderer, Callbacks cb) {
    rootHolder_ = std::make_unique<Node>();
    root_ = rootHolder_.get();
    root_->name = "LCARS menu";
    root_->position = {0, 0, -3.2f};

    auto header = std::make_unique<PanelNode>(renderer, "SciFi Shell", Vec3{0.45f, 0.6f, 1.0f}, 4.2f, 0.55f, 0);
    header->position = {0, 2.5f, 0};
    root_->add(std::move(header));

    struct Def {
        const char* text;
        Vec3 color;
        std::function<void()> fn;
    };
    Def defs[] = {{"Enter file room", {0.45f, 0.6f, 1.0f}, cb.enterFileRoom},
                  {"VR mode: off", {0.5f, 0.85f, 0.8f}, cb.toggleVR},
                  {"Exit shell", {0.95f, 0.5f, 0.55f}, cb.exitShell}};
    float y = 1.7f;
    for (auto& d : defs) {
        auto item = std::make_unique<PanelNode>(renderer, d.text, d.color, 4.2f, 0.65f, 0);
        item->position = {0, y, 0};
        item->pickable = true;
        item->onActivate = d.fn;
        items_.push_back(static_cast<PanelNode*>(root_->add(std::move(item))));
        y -= 0.78f;
    }
    vrItem_ = items_[1];

    auto status = std::make_unique<PanelNode>(renderer, "Ready", Vec3{0.5f, 0.6f, 0.75f}, 4.2f, 0.4f, 0);
    status->position = {0, y - 0.1f, 0};
    status_ = static_cast<PanelNode*>(root_->add(std::move(status)));
    items_[0]->setSelected(true);
}

void LCARSMenu::moveSelection(int delta) {
    items_[selected_]->setSelected(false);
    int n = static_cast<int>(items_.size());
    selected_ = ((selected_ + delta) % n + n) % n;
    items_[selected_]->setSelected(true);
}

void LCARSMenu::select(Node* node) {
    for (size_t i = 0; i < items_.size(); ++i)
        if (items_[i] == node && static_cast<int>(i) != selected_) {
            items_[selected_]->setSelected(false);
            selected_ = static_cast<int>(i);
            items_[i]->setSelected(true);
        }
}

void LCARSMenu::activateSelected() {
    if (items_[selected_]->onActivate) items_[selected_]->onActivate();
}

void LCARSMenu::activate(Node* node) {
    select(node);
    activateSelected();
}

void LCARSMenu::setVRState(bool on) { vrItem_->setText(on ? "VR mode: on" : "VR mode: off"); }

void LCARSMenu::setStatus(const std::string& text) { status_->setText(text); }

}  // namespace sf
