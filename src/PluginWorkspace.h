#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sf {

// Editable copy of an installed gameplay ZIP. Extraction reuses the strict package reader (only plugin.json,
// scripts/main.lua and assets/<safe-name> entries, bounded sizes) and never overwrites an existing workspace.
// Returns true with created=false when the workspace already exists (the user's edits are preserved).
bool extractGameplayWorkspace(const std::filesystem::path& zip, const std::filesystem::path& workspace,
                              bool& created, std::string& error);

// Packs a workspace into a new ZIP (target must not exist). Symlinks, junctions and other non-regular entries,
// unexpected files, and entries over the package limits are rejected; the result must still be validated as a
// gameplay package before installation.
bool packGameplayWorkspace(const std::filesystem::path& workspace, const std::filesystem::path& zip,
                           std::string& error);

// Plugin-scoped "key=value" settings file (<= 4 KiB, <= 8 entries). Saving writes a sibling temp file and
// atomically replaces the target (MoveFileExW REPLACE_EXISTING|WRITE_THROUGH on Windows), so a failed save
// keeps the previous file intact.
bool savePluginSettings(const std::filesystem::path& file,
                        const std::vector<std::pair<std::string, std::string>>& values, std::string& error);
std::map<std::string, std::string> loadPluginSettings(const std::filesystem::path& file);

}  // namespace sf
