#include "GameplayHudPanel.h"
#include "TextRender.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
const std::uint8_t* pixel(const sf::Bitmap& bm, int x, int y) { return &bm.rgba[(static_cast<size_t>(y) * bm.w + x) * 4]; }
}  // namespace

int main() {
    if (!sf::loadFonts(SCIFI_TEST_FONT_DIR)) { std::cerr << "Cannot load application fonts\n"; return 1; }
    using Panel = sf::PluginScriptRuntime::HudPanel;
    using Command = sf::PluginScriptRuntime::HudCommand;
    Panel panel;
    panel.id = 1;
    panel.anchor = 8;  // bottom_right
    panel.x = -0.01; panel.y = -0.01; panel.width = 0.25; panel.height = 0.2;
    sf::GameplayHudRect rect;
    check(sf::gameplayHudPanelRect(panel, rect) && std::abs(rect.left - 0.74f) < 1e-5f &&
          std::abs(rect.top - 0.79f) < 1e-5f, "bottom_right anchor places the panel's corner at the offset");
    panel.anchor = 4; panel.x = 0; panel.y = 0;
    check(sf::gameplayHudPanelRect(panel, rect) && std::abs(rect.left - 0.375f) < 1e-5f &&
          std::abs(rect.top - 0.4f) < 1e-5f, "center anchor centres the panel");
    panel.anchor = 0; panel.x = 0.9; panel.y = -0.5;
    check(sf::gameplayHudPanelRect(panel, rect) && rect.left == 0.75f && rect.top == 0.0f,
          "offsets cannot push a panel outside the view");
    panel.visible = false;
    check(!sf::gameplayHudPanelRect(panel, rect), "hidden panels are not placed");

    panel.visible = true;
    panel.canvasWidth = 64; panel.canvasHeight = 32;
    panel.background = {0, 0, 0, 128};
    Command fill;
    fill.kind = Command::Kind::Rect;
    fill.x0 = 4; fill.y0 = 4; fill.x1 = 8; fill.y1 = 8;
    fill.color = {255, 0, 0, 255};
    Command dot;
    dot.kind = Command::Kind::Circle;
    dot.x0 = 40; dot.y0 = 16; dot.size = 5;
    dot.color = {0, 0, 255, 255};
    Command text;
    text.kind = Command::Kind::Text;
    text.x0 = 63; text.y0 = 0; text.size = 16; text.align = 2;
    text.color = {255, 255, 255, 255};
    text.text = "Hi";
    panel.commands = {fill, dot, text};
    const auto bm = sf::rasterizeGameplayHudPanel(panel);
    check(bm.w == 64 && bm.h == 32 && bm.rgba.size() == 64u * 32u * 4u, "bitmap matches the canvas");
    check(pixel(bm, 0, 31)[3] == 128 && pixel(bm, 0, 31)[0] == 0, "translucent background fills the canvas");
    check(pixel(bm, 6, 6)[0] == 255 && pixel(bm, 6, 6)[1] == 0 && pixel(bm, 6, 6)[3] == 255 &&
          pixel(bm, 13, 13)[0] == 0, "rect uses x, y, width, height");
    check(pixel(bm, 40, 16)[2] == 255 && pixel(bm, 40, 16)[0] == 0, "circle is centred on its point");
    bool textRight = false, textLeft = false;
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 64; ++x)
            if (pixel(bm, x, y)[0] > 200 && pixel(bm, x, y)[1] > 200) (x >= 40 ? textRight : textLeft) = true;
    check(textRight && !textLeft, "right-aligned text ends at its x coordinate");
    Command outside = fill;
    outside.x0 = -4000; outside.y0 = 4000;
    panel.commands = {outside};
    check(sf::rasterizeGameplayHudPanel(panel).w == 64, "out-of-canvas commands are clipped safely");

    Panel weather = panel;
    weather.anchor = 0; weather.width = 0.28; weather.height = 0.18;
    weather.canvasWidth = 360; weather.canvasHeight = 130;
    weather.commands = {fill, dot, text};
    const auto scales = sf::gameplayHudRasterScales({weather}, 3440, 1440, sf::kGameplayHudPluginPixels);
    const auto sharp = sf::rasterizeGameplayHudPanel(weather, scales[0]);
    check(sharp.w >= 960 && sharp.w <= 964 && sharp.h >= 346,
          "ultrawide panels rasterize at their physical display resolution, not their logical canvas");
    const int sx = static_cast<int>(6 * scales[0]), sy = static_cast<int>(6 * scales[0]);
    check(pixel(sharp, sx, sy)[0] == 255 && pixel(sharp, sx, sy)[1] == 0,
          "logical drawing coordinates scale with the raster");
    const auto small = sf::gameplayHudRasterScales({weather}, 1280, 720, sf::kGameplayHudPluginPixels);
    check(small[0] < scales[0] &&
          sf::gameplayHudRasterScales({weather}, 3440, 1440, sf::kGameplayHudPluginPixels) == scales,
          "resize/DPI changes raster density; unchanged views select identical scales");
    weather.width = 0.9;
    check(sf::gameplayHudRasterScales({weather}, 3440, 1440, sf::kGameplayHudPluginPixels)[0] > scales[0],
          "changing a panel's display size rebuilds its raster without changing logical coordinates");
    std::vector<Panel> crowded(4, weather);
    for (size_t i = 0; i < crowded.size(); ++i) {
        crowded[i].id = static_cast<int>(i + 1);
        crowded[i].width = crowded[i].height = 1;
        crowded[i].canvasWidth = crowded[i].canvasHeight = 512;
    }
    const size_t fairShare = sf::kGameplayHudGlobalPixels / 64;
    const auto bounded = sf::gameplayHudRasterScales(crowded, 16384, 16384, fairShare);
    size_t total = 0;
    for (const auto& p : crowded) {
        const int side = sf::gameplayHudRasterSide(p.canvasWidth, bounded[p.id - 1]);
        total += static_cast<size_t>(side) * side;
        check(side <= 4096, "raster sides remain bounded at extreme display sizes");
    }
    check(total <= fairShare && total * 64 <= sf::kGameplayHudGlobalPixels,
          "four full-screen panels per plugin share the global budget fairly across 64 plugins");
    const auto solo = sf::gameplayHudRasterScales(crowded, 16384, 16384, sf::kGameplayHudPluginPixels);
    total = 0;
    for (const auto& p : crowded) {
        const int side = sf::gameplayHudRasterSide(p.canvasWidth, solo[p.id - 1]);
        total += static_cast<size_t>(side) * side;
    }
    check(total <= sf::kGameplayHudPluginPixels, "a single plugin cannot exceed its physical raster allocation");
    weather.visible = false;
    check(sf::gameplayHudRasterScales({weather}, 3440, 1440, sf::kGameplayHudPluginPixels)[0] == 0,
          "hidden panels consume no raster budget");

    Panel glyph = panel;
    glyph.canvasWidth = glyph.canvasHeight = 64;
    glyph.background = {};
    text.x0 = 0; text.y0 = 0; text.size = 12; text.align = 0; text.text = "Hi";
    glyph.commands = {text};
    const auto low = sf::rasterizeGameplayHudPanel(glyph);
    const auto high = sf::rasterizeGameplayHudPanel(glyph, 3);
    bool freshGlyphDetail = false;
    for (int y = 0; y < 28; ++y)
        for (int x = 0; x < 36; ++x)
            if (pixel(high, x, y)[3] != pixel(high, x / 3 * 3, y / 3 * 3)[3]) freshGlyphDetail = true;
    check(low.w == 64 && high.w == 192 && freshGlyphDetail,
          "high-resolution text has new glyph coverage, rather than enlarged low-resolution pixels");
    return failures ? 1 : 0;
}
