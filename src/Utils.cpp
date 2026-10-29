#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace sf {

Mat4 Mat4::identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
}
Mat4 Mat4::translate(Vec3 t) {
    Mat4 r = identity();
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}
Mat4 Mat4::scale(Vec3 s) {
    Mat4 r = identity();
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}
Mat4 Mat4::rotateX(float a) {
    Mat4 r = identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[5] = c;
    r.m[9] = -s;
    r.m[6] = s;
    r.m[10] = c;
    return r;
}
Mat4 Mat4::rotateY(float a) {
    Mat4 r = identity();
    float c = std::cos(a), s = std::sin(a);
    r.m[0] = c;
    r.m[8] = s;
    r.m[2] = -s;
    r.m[10] = c;
    return r;
}
Mat4 Mat4::perspectiveTan(float tl, float tr, float tu, float td, float zn, float zf) {
    Mat4 r{};
    r.m[0] = 2.0f / (tr - tl);
    r.m[5] = 2.0f / (tu - td);
    r.m[8] = (tr + tl) / (tr - tl);   // row 0, col 2
    r.m[9] = (tu + td) / (tu - td);   // row 1, col 2
    r.m[10] = zf / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = zn * zf / (zn - zf);
    return r;
}
Mat4 Mat4::perspective(float fovY, float aspect, float zn, float zf) {
    float t = std::tan(fovY * 0.5f);
    return perspectiveTan(-t * aspect, t * aspect, t, -t, zn, zf);
}
Mat4 Mat4::fromPose(float x, float y, float z, float w, Vec3 p) {
    Mat4 r = identity();
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + z * w);
    r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);
    r.m[9] = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[12] = p.x;
    r.m[13] = p.y;
    r.m[14] = p.z;
    return r;
}
Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
Vec3 Mat4::transformPoint(Vec3 p) const {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}
Vec3 Mat4::transformDir(Vec3 d) const {
    return {m[0] * d.x + m[4] * d.y + m[8] * d.z, m[1] * d.x + m[5] * d.y + m[9] * d.z,
            m[2] * d.x + m[6] * d.y + m[10] * d.z};
}
Mat4 Mat4::rigidInverse() const {
    Mat4 r = identity();
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) r.m[c * 4 + row] = m[row * 4 + c];
    Vec3 t = {m[12], m[13], m[14]};
    r.m[12] = -(r.m[0] * t.x + r.m[4] * t.y + r.m[8] * t.z);
    r.m[13] = -(r.m[1] * t.x + r.m[5] * t.y + r.m[9] * t.z);
    r.m[14] = -(r.m[2] * t.x + r.m[6] * t.y + r.m[10] * t.z);
    return r;
}

namespace {
struct Glyph {
    char c;
    uint8_t col[5];  // column bytes, bit 0 = top row
};
const Glyph kFont[] = {
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}}, {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}}, {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'_', {0x40, 0x40, 0x40, 0x40, 0x40}}, {':', {0x00, 0x36, 0x36, 0x00, 0x00}},
    {'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
};
const uint8_t* findGlyph(char c) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const Glyph& g : kFont)
        if (g.c == c) return g.col;
    return nullptr;  // unknown characters render as blank space
}
}  // namespace

Bitmap rasterizeLines(const std::vector<std::string>& lines, int scale, int minW, int minH) {
    const int pad = 2 * scale, advance = 6 * scale, lineH = 9 * scale;
    size_t maxChars = 1;
    for (auto& l : lines) maxChars = std::max(maxChars, l.size());
    Bitmap bm;
    bm.w = std::max(minW, static_cast<int>(maxChars) * advance + 2 * pad);
    bm.h = std::max(minH, static_cast<int>(lines.size()) * lineH + 2 * pad);
    bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
    int totalH = static_cast<int>(lines.size()) * lineH;
    int y0 = (minH > 0) ? (bm.h - totalH) / 2 : pad;
    for (size_t li = 0; li < lines.size(); ++li) {
        const std::string& s = lines[li];
        int textW = static_cast<int>(s.size()) * advance;
        int x0 = (minW > 0) ? (bm.w - textW) / 2 : pad;
        for (size_t ci = 0; ci < s.size(); ++ci) {
            const uint8_t* g = findGlyph(s[ci]);
            if (!g) continue;
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row) {
                    if (!(g[col] & (1 << row))) continue;
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx) {
                            int px = x0 + static_cast<int>(ci) * advance + col * scale + sx;
                            int py = y0 + static_cast<int>(li) * lineH + row * scale + sy;
                            if (px < 0 || py < 0 || px >= bm.w || py >= bm.h) continue;
                            uint8_t* p = &bm.rgba[(static_cast<size_t>(py) * bm.w + px) * 4];
                            p[0] = p[1] = p[2] = p[3] = 255;
                        }
                }
        }
    }
    return bm;
}

std::string homeDirectory() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    if (h && *h) return h;
    std::error_code ec;
    return std::filesystem::current_path(ec).string();
}

std::string parentDirectory(const std::string& path) {
    std::filesystem::path p = std::filesystem::u8path(path);
    return p.has_parent_path() && p.parent_path() != p ? p.parent_path().u8string() : path;
}

std::string parentOrDrives(const std::string& path) {
    if (path.empty()) return homeDirectory();
    std::string parent = parentDirectory(path);
#ifdef _WIN32
    if (parent == path) return "";
#endif
    return parent;
}

std::vector<DirEntry> listDrives() {
    std::vector<DirEntry> out;
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char root[4] = {static_cast<char>('A' + i), ':', '\\', 0};
        wchar_t wroot[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
        UINT type = GetDriveTypeW(wroot);
        const char* kind = type == DRIVE_REMOVABLE ? "Removable" : type == DRIVE_CDROM ? "Disc" :
                           type == DRIVE_REMOTE ? "Network" : "Drive";
        DirEntry e;
        e.name = std::string(kind) + " " + static_cast<char>('A' + i) + ":";
        e.path = root;
        e.isDir = true;
        out.push_back(std::move(e));
    }
#else
    DirEntry e;
    e.name = "Root /";
    e.path = "/";
    e.isDir = true;
    out.push_back(std::move(e));
#endif
    DirEntry home;
    home.name = "Home";
    home.path = homeDirectory();
    home.isDir = true;
    out.push_back(std::move(home));
    return out;
}

std::vector<DirEntry> listDirectory(const std::string& path, size_t maxEntries) {
    namespace fs = std::filesystem;
    std::vector<DirEntry> out;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(path), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && out.size() < maxEntries; it.increment(ec)) {
        DirEntry e;
        e.name = it->path().filename().u8string();
        e.path = it->path().u8string();
        std::error_code ec2;
        e.isDir = it->is_directory(ec2);
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        return a.name < b.name;
    });
    return out;
}

}  // namespace sf
