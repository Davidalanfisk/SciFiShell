#pragma once
#ifdef _WIN32
#include <windows.h>
#include <string>

namespace sf {
inline bool systemDriveRoot(std::wstring& root,std::string& error) {
    wchar_t windows[MAX_PATH+1]{},volume[MAX_PATH+1]{};
    const UINT length=GetWindowsDirectoryW(windows,MAX_PATH+1);
    if(!length || length>=MAX_PATH+1 || !GetVolumePathNameW(windows,volume,MAX_PATH+1)) {
        error="CANNOT FIND WINDOWS SYSTEM DRIVE: "+std::to_string(GetLastError());
        return false;
    }
    root=volume;
    return true;
}
}
#endif
