#include "GameplayHudPanel.h"

#include "TextRender.h"
#include "UiKit.h"

#include <algorithm>
#include <cmath>

namespace sf {
bool gameplayHudPanelRect(const PluginScriptRuntime::HudPanel& panel, GameplayHudRect& out) {
    if (!panel.id || !panel.visible || panel.anchor < 0 || panel.anchor > 8) return false;
    const float w = std::clamp(static_cast<float>(panel.width), 0.0f, 1.0f);
    const float h = std::clamp(static_cast<float>(panel.height), 0.0f, 1.0f);
    if (w <= 0 || h <= 0) return false;
    const float ax = (panel.anchor % 3) * 0.5f, ay = (panel.anchor / 3) * 0.5f;
    out.width = w;
    out.height = h;
    out.left = std::clamp(ax + static_cast<float>(panel.x) - ax * w, 0.0f, 1.0f - w);
    out.top = std::clamp(ay + static_cast<float>(panel.y) - ay * h, 0.0f, 1.0f - h);
    return true;
}

int gameplayHudRasterSide(int logicalSide, float scale) {
    return std::clamp(static_cast<int>(std::floor(std::clamp(logicalSide, 1, 4096) * scale)), 1, 4096);
}

std::array<float, 4> gameplayHudRasterScales(const std::vector<PluginScriptRuntime::HudPanel>& panels,
                                          int areaWidth, int areaHeight, size_t pixelBudget) {
    std::array<float, 4> scales{};
    double pixels = 0;
    for (const auto& panel : panels) {
        GameplayHudRect rect;
        if (panel.id < 1 || panel.id > 4 || !gameplayHudPanelRect(panel, rect)) continue;
        const int w = std::clamp(panel.canvasWidth, 1, 4096), h = std::clamp(panel.canvasHeight, 1, 4096);
        // A scrolling panel shows only view_width canvas pixels across its on-screen rectangle.
        const int shown = panel.scrollSpeed > 0 && panel.viewWidth > 0 ? std::min(panel.viewWidth, w) : w;
        const float requested = std::max(rect.width * std::max(areaWidth, 1) / shown,
                                         rect.height * std::max(areaHeight, 1) / h);
        const float scale = std::clamp(requested, 1.0f / 64, std::min(8.0f, 4096.0f / std::max(w, h)));
        scales[panel.id - 1] = scale;
        pixels += static_cast<double>(w) * h * scale * scale;
    }
    const float reduction = pixels > pixelBudget ? static_cast<float>(std::sqrt(pixelBudget / pixels)) : 1.0f;
    for (auto& scale : scales)
        scale = std::floor(scale * reduction * 64) / 64;
    return scales;
}

Bitmap rasterizeGameplayHudPanel(const PluginScriptRuntime::HudPanel& panel, float scale) {
    Bitmap bm;
    scale = std::clamp(scale, 1.0f / 64, std::min(8.0f, 4096.0f /
        std::max(std::clamp(panel.canvasWidth, 1, 4096), std::clamp(panel.canvasHeight, 1, 4096))));
    bm.w = gameplayHudRasterSide(panel.canvasWidth, scale);
    bm.h = gameplayHudRasterSide(panel.canvasHeight, scale);
    bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
    auto color = [](const std::array<std::uint8_t, 4>& c) { return Color8{c[0], c[1], c[2], c[3]}; };
    if (panel.background[3]) ui::fillRect(bm, 0, 0, static_cast<float>(bm.w), static_cast<float>(bm.h), color(panel.background));
    using Kind = PluginScriptRuntime::HudCommand::Kind;
    for (auto command : panel.commands) {
        command.x0 *= scale; command.y0 *= scale;
        command.x1 *= scale; command.y1 *= scale;
        command.size *= scale;
        const Color8 c = color(command.color);
        switch (command.kind) {
        case Kind::Rect:
        case Kind::Frame: {
            const float x0 = std::min(command.x0, command.x0 + command.x1), x1 = std::max(command.x0, command.x0 + command.x1);
            const float y0 = std::min(command.y0, command.y0 + command.y1), y1 = std::max(command.y0, command.y0 + command.y1);
            if (command.kind == Kind::Rect)
                ui::fillRound(bm, x0, y0, x1, y1, std::min(command.size, std::min(x1 - x0, y1 - y0) * 0.5f), c);
            else
                ui::strokeRect(bm, x0, y0, x1, y1, std::min(command.size, std::min(x1 - x0, y1 - y0) * 0.5f), c);
            break;
        }
        case Kind::Line: ui::line(bm, command.x0, command.y0, command.x1, command.y1, command.size, c); break;
        case Kind::Circle: ui::dot(bm, command.x0, command.y0, command.size, c); break;
        case Kind::Text: {
            const float width = measureText(command.text, command.size, FontFace::UI);
            const float x = command.align == 1 ? command.x0 - width * 0.5f :
                            command.align == 2 ? command.x0 - width : command.x0;
            drawText(bm, x, command.y0 + textAscent(command.size, FontFace::UI), command.text, command.size,
                     FontFace::UI, c);
            break;
        }
        }
    }
    return bm;
}
}  // namespace sf
