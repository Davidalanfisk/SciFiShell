#include "AppCatalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shlobj.h>
#endif

namespace sf {

std::vector<AppEntry> loadTaskbarApps() {
    namespace fs = std::filesystem;
    std::vector<AppEntry> out;
    const char* appData = std::getenv("APPDATA");
    if (!appData) return out;
    std::error_code ec;
    const fs::path dir = fs::path(appData) / "Microsoft" / "Internet Explorer" / "Quick Launch" / "User Pinned" / "TaskBar";
    std::set<std::string> seen;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        std::string ext = p.extension().u8string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext != ".lnk") continue;
        std::string name = p.stem().u8string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c < 128 ? std::toupper(c) : c; });
        if (!seen.insert(name).second) continue;
        out.push_back({name, p.u8string()});
    }
    std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) { return a.name < b.name; });
    return out;
}

bool loadAppIcon(const std::string& path, int px, Bitmap& out, bool transparent) {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int wl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wp(static_cast<size_t>(std::max(wl, 1)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), wl);
    IShellItemImageFactory* fac = nullptr;
    if (FAILED(SHCreateItemFromParsingName(wp.c_str(), nullptr, IID_PPV_ARGS(&fac))) || !fac) return false;
    HBITMAP hb = nullptr;
    const HRESULT hr = fac->GetImage({px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb);
    fac->Release();
    if (FAILED(hr) || !hb) return false;
    BITMAP bm{};
    GetObject(hb, sizeof bm, &bm);
    const int w = bm.bmWidth, h = bm.bmHeight;
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    const int got = GetDIBits(dc, hb, 0, h, bgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(hb);
    if (!got) return false;
    out.w = w;
    out.h = h;
    out.rgba.resize(bgra.size());
    const float tile[3] = {10.0f, 16.0f, 26.0f};
    for (size_t i = 0; i < bgra.size(); i += 4) {
        const float a = bgra[i + 3] / 255.0f;  // shell icons carry premultiplied alpha
        if (transparent) {  // keep the alpha, undo the premultiply
            const float inv = a > 0.001f ? 1.0f / a : 0.0f;
            for (int k = 0; k < 3; ++k) out.rgba[i + k] = static_cast<uint8_t>(std::min(255.0f, bgra[i + 2 - k] * inv));
            out.rgba[i + 3] = bgra[i + 3];
            continue;
        }
        out.rgba[i + 0] = static_cast<uint8_t>(std::min(255.0f, bgra[i + 2] + tile[0] * (1 - a)));
        out.rgba[i + 1] = static_cast<uint8_t>(std::min(255.0f, bgra[i + 1] + tile[1] * (1 - a)));
        out.rgba[i + 2] = static_cast<uint8_t>(std::min(255.0f, bgra[i + 0] + tile[2] * (1 - a)));
        out.rgba[i + 3] = 255;
    }
    return true;
#else
    (void)path; (void)px; (void)out;
    return false;
#endif
}
}  // namespace sf
