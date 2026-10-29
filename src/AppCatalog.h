#pragma once
// The list of programs the shell can host: Notepad plus whatever is pinned to the Windows taskbar.
#include <string>
#include <vector>

#include "Utils.h"

namespace sf {

struct AppEntry {
    std::string name;    // display name, upper case
    std::string launch;  // path handed to ShellExecute (a program or a .lnk shortcut)
};

// Reads the shortcuts in %APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar.
std::vector<AppEntry> loadTaskbarApps();

// The shell icon of a program or shortcut, rendered onto a dark tile (opaque RGBA, px x px). False if unavailable.
bool loadAppIcon(const std::string& path, int px, Bitmap& out);

}  // namespace sf
