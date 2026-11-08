#include "GameplayPermissions.h"
#include "SettingsNoticeLayout.h"
#include "TextRender.h"

#include <iostream>
#include <string>

int main() {
    if (!sf::loadFonts(SCIFI_TEST_FONT_DIR)) {
        std::cerr << "Cannot load application fonts\n";
        return 1;
    }
    for (char character : std::string("abcdefghijklmnopqrstuvwxyz0123456789-")) {
        const std::string id = "m." + std::string(78, character);
        for (int mask = 0; mask < 256; ++mask) {
            const auto text = sf::gameplayConsentNotice(id, mask & 1, mask & 2, mask & 4, mask & 8,
                                                        mask & 16, mask & 32, mask & 64, mask & 128);
            const auto lines = sf::wrapText(text, sf::kSettingsNoticeFontSize, sf::FontFace::UI,
                1000 - sf::kSettingsNoticeRightPadding - sf::kSettingsNoticeLeft, sf::kSettingsNoticeLines);
            std::string actual, expected;
            for (const auto& line : lines) for (char c : line) if (c != ' ') actual += c;
            for (char c : text) if (c != ' ') expected += c;
            if (actual != expected || lines.size() > sf::kSettingsNoticeLines) {
                std::cerr << "Permission notice is truncated for ID character " << character << '\n';
                return 1;
            }
        }
    }
    return 0;
}
