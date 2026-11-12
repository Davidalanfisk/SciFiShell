#include "GameplayPermissions.h"
#include "SettingsNoticeLayout.h"
#include "TextRender.h"

#include <iostream>
#include <string>
#include <vector>

int main() {
    if (!sf::loadFonts(SCIFI_TEST_FONT_DIR)) {
        std::cerr << "Cannot load application fonts\n";
        return 1;
    }
    for (char character : std::string("abcdefghijklmnopqrstuvwxyz0123456789-")) {
        const std::string id = "m." + std::string(78, character);
        // Worst-case network_hosts: two 63-character names totalling 100 characters, or four 25-character names.
        const char h = character == '-' ? 'a' : character;
        const std::vector<std::vector<std::string>> hostLists{
            {}, {std::string(59, h) + ".com", std::string(33, h) + ".com"},
            {std::string(21, 'h') + ".com", std::string(21, 'i') + ".com", std::string(21, 'j') + ".com",
             std::string(21, 'k') + ".com"}};
        for (int mask = 0; mask < 2048 * 3; ++mask) {
            const auto text = sf::gameplayConsentNotice(id, mask & 1, mask & 2, mask & 4, mask & 8,
                                                        mask & 16, mask & 32, mask & 64, mask & 128, mask & 256, mask & 512, mask & 1024,
                                                        hostLists[mask / 2048]);
            const auto layout = sf::layoutSettingsNotice(text, 1000 - sf::kSettingsNoticeRightPadding - sf::kSettingsNoticeLeft);
            const auto& lines = layout.lines;
            const int maxLines = layout.fontSize == sf::kSettingsNoticeFontSize ? sf::kSettingsNoticeLines : sf::kSettingsNoticeCompactLines;
            if (!hostLists[mask / 2048].size() && layout.fontSize != sf::kSettingsNoticeFontSize) {
                std::cerr << "Notice without network hosts must keep the standard layout\n";
                return 1;
            }
            std::string actual, expected;
            for (const auto& line : lines) for (char c : line) if (c != ' ') actual += c;
            for (char c : text) if (c != ' ') expected += c;
            if (actual != expected || static_cast<int>(lines.size()) > maxLines) {
                std::cerr << "Permission notice is truncated for ID character " << character << " mask " << mask << " lines " << lines.size() << '\n';
                for (const auto& line : lines) std::cerr << "[" << line << "]\n";
                return 1;
            }
        }
    }
    {
        // Every permission at once, with four 25-character network hosts and four link hosts, still fits untruncated.
        const std::string id = "m." + std::string(78, 'w');
        const std::vector<std::string> hosts{std::string(21, 'h') + ".com", std::string(21, 'i') + ".com",
                                             std::string(21, 'j') + ".com", std::string(21, 'k') + ".com"};
        const std::vector<std::string> links{std::string(21, 'l') + ".com", std::string(21, 'm') + ".com",
                                             std::string(21, 'n') + ".com", std::string(21, 'o') + ".com"};
        const auto text = sf::gameplayConsentNotice(id, true, true, true, true, true, true, true, true, true, true, true,
                                                    hosts, true, "nhjk", links);
        if (text.find(", HOTKEYS N H J K,") == std::string::npos) {
            std::cerr << "Hotkey letters must be shown as typed letters: " << text << '\n';
            return 1;
        }
        const auto layout = sf::layoutSettingsNotice(text, 1000 - sf::kSettingsNoticeRightPadding - sf::kSettingsNoticeLeft);
        std::string actual, expected;
        for (const auto& line : layout.lines) for (char c : line) if (c != ' ') actual += c;
        for (char c : text) if (c != ' ') expected += c;
        const int maxLines = layout.fontSize == sf::kSettingsNoticeFontSize ? sf::kSettingsNoticeLines : sf::kSettingsNoticeCompactLines;
        if (actual != expected || static_cast<int>(layout.lines.size()) > maxLines) {
            std::cerr << "Full permission notice with link hosts is truncated (" << layout.lines.size() << " lines)\n";
            return 1;
        }
    }
    return 0;
}
