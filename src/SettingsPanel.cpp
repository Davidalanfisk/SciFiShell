#include "SettingsPanel.h"

#include <cmath>

#include "Theme.h"

namespace sf {

namespace {
constexpr float kRowW = 1.7f, kRowH = 0.15f, kPitch = 0.19f;
}

SettingsPanel::SettingsPanel(Renderer& renderer, const std::vector<std::string>& rows, const std::string& panelTitle) {
    rootHolder_ = std::make_unique<Node>();
    root_ = rootHolder_.get();
    root_->name = "Settings";
    const int n = static_cast<int>(rows.size());
    const float top = (n + 1) * kPitch * 0.5f;
    auto title = std::make_unique<PanelNode>(renderer, panelTitle, Vec3{0.1f, 0.85f, 1.0f}, kRowW, 0.2f, 4);
    title->position = {0, top, 0};
    root_->add(std::move(title));
    for (int i = 0; i < n; ++i) {
        auto row = std::make_unique<PanelNode>(renderer, rows[i], Vec3{0.1f, 0.85f, 1.0f}, kRowW, kRowH, 0);
        row->position = {0, top - (i + 1) * kPitch - 0.06f, 0};
        row->pickable = true;
        row->onActivate = [this, i] {
            if (onRow) onRow(i);
        };
        rows_.push_back(static_cast<PanelNode*>(root_->add(std::move(row))));
    }
    root_->visible = false;
}

void SettingsPanel::open(Vec3 eye, Vec3 forward) {
    Vec3 f = {forward.x, 0.0f, forward.z};
    if (length(f) < 0.25f) f = fallbackFlat_;
    f = normalize(f);
    fallbackFlat_ = f;
    Vec3 c = eye + f * 1.6f;
    c.y = eye.y;
    root_->position = c;
    Vec3 d = eye - c;
    root_->yaw = std::atan2(d.x, d.z);
    open_ = true;
    root_->visible = true;
    select(0);
}

void SettingsPanel::setRowText(int i, const std::string& text) {
    if (i >= 0 && i < static_cast<int>(rows_.size()) && rows_[i]->name != text) rows_[i]->setText(text);
}

void SettingsPanel::select(int i) {
    const int n = static_cast<int>(rows_.size());
    sel_ = ((i % n) + n) % n;
    for (int k = 0; k < n; ++k) rows_[k]->setSelected(k == sel_);
}

void SettingsPanel::moveSelection(int delta) { select(sel_ + delta); }

void SettingsPanel::pointer(Vec3 o, Vec3 d) {
    Node* hit = pickNode(*root_, o, d);
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i] == hit) select(static_cast<int>(i));
}

void SettingsPanel::activateSelected() {
    if (rows_[sel_]->onActivate) rows_[sel_]->onActivate();
}

bool SettingsPanel::activateHit(Vec3 o, Vec3 d) {
    Node* hit = pickNode(*root_, o, d);
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i] == hit) {
            select(static_cast<int>(i));
            activateSelected();
            return true;
        }
    return false;
}

void SettingsPanel::update(double dt, double time) {
    if (!open_) {
        root_->visible = false;
        return;
    }
    const Theme& th = currentTheme();
    for (size_t i = 0; i < rows_.size(); ++i) rows_[i]->setColor(static_cast<int>(i) == sel_ ? th.alt : th.accent);
    if (!root_->children().empty()) static_cast<PanelNode*>(root_->children()[0].get())->setColor(th.accent);
    root_->updateTree(dt, time);
}

}  // namespace sf
