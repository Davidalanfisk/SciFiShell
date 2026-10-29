#pragma once
// Antialiased vector text (stb_truetype) with per-glyph fallback across several fonts.
// All functions are thread-safe (thumbnails are composed on a worker thread).
#include <string>
#include <vector>

#include "Utils.h"

namespace sf {

enum class FontFace { UI, UIBold, Mono };

struct Color8 {
    uint8_t r = 255, g = 255, b = 255, a = 255;
};

// Loads the TTF files from `dir`. Returns false when no usable font was found.
bool loadFonts(const std::string& dir);

float measureText(const std::string& utf8, float px, FontFace face);
float textAscent(float px, FontFace face);
float textLineHeight(float px, FontFace face);

// Composites text (straight alpha "over") into bm. `baseline` is the y of the text baseline in pixels.
void drawText(Bitmap& bm, float x, float baseline, const std::string& utf8, float px, FontFace face, Color8 color);

std::string ellipsize(const std::string& utf8, float px, FontFace face, float maxWidth);
// Breaks anywhere (file names have no spaces to wrap on); the last line is ellipsized.
std::vector<std::string> wrapText(const std::string& utf8, float px, FontFace face, float maxWidth, int maxLines);

// Tight label bitmap: fixed height, width follows the text. Used by panels.
Bitmap makeLabel(const std::string& utf8, float px, FontFace face, Color8 color, int height, int padX);

// Fills a bitmap with a thin white door outline (round top, square bottom) and a soft neon halo in the alpha channel.
void drawDoorOutline(Bitmap& bm);
}  // namespace sf
