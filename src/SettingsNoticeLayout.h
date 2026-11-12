#pragma once

#include "TextRender.h"

#include <string>
#include <vector>

namespace sf {
inline constexpr float kSettingsNoticeFontSize = 16.0f;
inline constexpr float kSettingsNoticePitch = 19.0f;
inline constexpr int kSettingsNoticeLines = 5;
// Long consent notices (for example with network hosts) fall back to a denser layout in the same vertical space:
// the last compact baseline (143 + 6 * 14.5) stays above the settings list at y = 236.
inline constexpr float kSettingsNoticeCompactFontSize = 12.0f;
inline constexpr float kSettingsNoticeCompactPitch = 14.5f;
inline constexpr int kSettingsNoticeCompactLines = 7;
inline constexpr int kSettingsNoticeLeft = 320;
inline constexpr int kSettingsNoticeRightPadding = 40;

struct SettingsNoticeLayout {
    std::vector<std::string> lines;
    float fontSize = kSettingsNoticeFontSize;
    float pitch = kSettingsNoticePitch;
};

inline SettingsNoticeLayout layoutSettingsNotice(const std::string& text, float width) {
    SettingsNoticeLayout out;
    out.lines = wrapText(text, kSettingsNoticeFontSize, FontFace::UI, width, kSettingsNoticeLines + 1);
    if (static_cast<int>(out.lines.size()) <= kSettingsNoticeLines) return out;
    out.fontSize = kSettingsNoticeCompactFontSize;
    out.pitch = kSettingsNoticeCompactPitch;
    out.lines = wrapText(text, kSettingsNoticeCompactFontSize, FontFace::UI, width, kSettingsNoticeCompactLines);
    return out;
}
}  // namespace sf
