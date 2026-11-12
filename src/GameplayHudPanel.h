#pragma once

#include "PluginScript.h"
#include "Utils.h"

namespace sf {
// Normalized view rectangle (0..1, y down) for a plugin HUD panel.
struct GameplayHudRect { float left = 0, top = 0, width = 0, height = 0; };

// Places the panel's anchor point at the matching view anchor plus its offset, then keeps the whole
// panel inside the view. Returns false for hidden or empty panels.
bool gameplayHudPanelRect(const PluginScriptRuntime::HudPanel& panel, GameplayHudRect& out);

inline constexpr size_t kGameplayHudPluginPixels = 4 * 1024 * 1024;
inline constexpr size_t kGameplayHudGlobalPixels = 16 * 1024 * 1024;

// Uniform raster scales preserve logical coordinates while covering the projected display pixels.
// The shared pixel budget, 4096-pixel sides and 8x scale bound CPU/GPU and glyph resources.
std::array<float, 4> gameplayHudRasterScales(const std::vector<PluginScriptRuntime::HudPanel>& panels,
                                          int areaWidth, int areaHeight, size_t pixelBudget);
int gameplayHudRasterSide(int logicalSide, float scale);

// Rasterizes directly at the selected resolution, including font glyphs (not an enlarged bitmap).
// Rect and frame use x, y, width, height; line uses endpoints; text uses x (by alignment) and the top y.
Bitmap rasterizeGameplayHudPanel(const PluginScriptRuntime::HudPanel& panel, float scale = 1.0f);
}  // namespace sf
