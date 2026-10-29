#pragma once
// Colour themes for the Tron / LCARS hybrid look. Cyan = active, amber = alerts, white = neutral.
#include "Utils.h"

namespace sf {

struct Theme {
    const char* name;
    Vec3 accent;  // active glow: grid lines, card edges, tool ring
    Vec3 alt;     // alert / secondary glow
};

const Theme& currentTheme();
void cycleTheme();  // next theme; the choice is remembered in %LOCALAPPDATA%\SciFiShell\theme.ini

}  // namespace sf
