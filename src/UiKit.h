#pragma once
// Shared drawing kit for the bitmap based panels (settings, system monitor, file browser, terminal ...).
// Everything is theme aware: the accent colours come from the current Theme, the neutral tones are fixed.
#include <string>
#include <vector>

#include "TextRender.h"
#include "Utils.h"

namespace sf::ui {

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

struct Tokens {
    Color8 bg, panel, panelHi, border, accent, alt, text, dim, muted, success, warn, danger;
};
Tokens tokens();  // follows the current theme

Color8 withAlpha(Color8 c, uint8_t a);
Color8 mix(Color8 a, Color8 b, float t);
Color8 toColor8(Vec3 v, uint8_t a = 255);

void fillRect(Bitmap& bm, float x0, float y0, float x1, float y1, Color8 c);
void fillRound(Bitmap& bm, float x0, float y0, float x1, float y1, float r, Color8 c);
void strokeRect(Bitmap& bm, float x0, float y0, float x1, float y1, float t, Color8 c);
void line(Bitmap& bm, float x0, float y0, float x1, float y1, float t, Color8 c);
// Sharp L shaped corner accents, the signature of the look.
void brackets(Bitmap& bm, float x0, float y0, float x1, float y1, float len, float t, Color8 c);

enum class State { Normal, Hover, Pressed, Disabled, Selected };

// Window background with the corner brackets and an optional title strip. Returns the content area under the strip.
Rect panel(Bitmap& bm, const std::string& title, const std::string& subtitle);
// A card / frame of any size. Selected and hovered ones glow in the accent colour.
void card(Bitmap& bm, const Rect& r, State s);
// Text button; `primary` fills it with the accent tint. Disabled and pressed read without relying on colour alone.
void button(Bitmap& bm, const Rect& r, const std::string& label, State s, bool primary = false, float px = 26);
void toggle(Bitmap& bm, const Rect& r, bool on, State s);
void slider(Bitmap& bm, const Rect& r, float value01, State s);
void bar(Bitmap& bm, const Rect& r, float value01, Color8 fill);
void dot(Bitmap& bm, float cx, float cy, float radius, Color8 c);
// Line chart of values in 0..1 (oldest first) with a soft fill under the curve.
void graph(Bitmap& bm, const Rect& r, const std::vector<float>& v, Color8 c);
void label(Bitmap& bm, float x, float baseline, const std::string& s, float px, FontFace f, Color8 c);
// Right aligned text ending at x.
void labelRight(Bitmap& bm, float xRight, float baseline, const std::string& s, float px, FontFace f, Color8 c);
// Close / fullscreen buttons used in every panel header; returns the two rects through the out parameters.
void windowButtons(Bitmap& bm, int panelW, int hover, Rect& fs, Rect& close);

std::string bytesText(unsigned long long b);

}  // namespace sf::ui
