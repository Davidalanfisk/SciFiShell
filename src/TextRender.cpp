#include "TextRender.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "stb_truetype.h"

namespace sf {

namespace {

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

std::mutex g_mutex;
Font g_fonts[6];  // 0 SpaceGrotesk-Medium, 1 SpaceGrotesk-Bold, 2 Inter-Medium, 3 Inter-Regular, 4 JetBrainsMono, 5 spare
std::vector<int> g_chain[3];

struct GlyphKey {
    int face, glyphFont, glyph, px4;
    bool operator<(const GlyphKey& o) const {
        if (face != o.face) return face < o.face;
        if (glyphFont != o.glyphFont) return glyphFont < o.glyphFont;
        if (glyph != o.glyph) return glyph < o.glyph;
        return px4 < o.px4;
    }
};
struct CachedGlyph {
    std::vector<uint8_t> cov;
    int w = 0, h = 0, xoff = 0, yoff = 0;
};
std::map<GlyphKey, CachedGlyph> g_cache;

bool readFile(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(n));
    bool ok = n > 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

bool loadOne(int slot, const std::string& path) {
    Font& f = g_fonts[slot];
    if (!readFile(path, f.data) || f.data.size() < 12) return false;
    const unsigned char* m = f.data.data();
    bool sfnt = (m[0] == 0 && m[1] == 1 && m[2] == 0 && m[3] == 0) || std::memcmp(m, "OTTO", 4) == 0 ||
                std::memcmp(m, "true", 4) == 0 || std::memcmp(m, "ttcf", 4) == 0;
    if (!sfnt) return false;
    int off = stbtt_GetFontOffsetForIndex(f.data.data(), 0);
    f.ok = stbtt_InitFont(&f.info, f.data.data(), off) != 0;
    return f.ok;
}

// Decodes the next code point, advancing i. Invalid bytes become U+FFFD.
uint32_t nextCodepoint(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
    if (extra < 0 || i + extra > s.size()) return 0xFFFD;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i]);
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
        ++i;
    }
    return cp;
}

struct Resolved {
    int font = -1, glyph = 0;
};
Resolved resolve(FontFace face, uint32_t cp) {
    for (int slot : g_chain[static_cast<int>(face)]) {
        if (!g_fonts[slot].ok) continue;
        int g = stbtt_FindGlyphIndex(&g_fonts[slot].info, static_cast<int>(cp));
        if (g > 0) return {slot, g};
    }
    return {};
}

float advanceOf(FontFace face, uint32_t cp, float px, Resolved* resolvedOut = nullptr) {
    if (cp == '\t') {
        Resolved sp = resolve(face, ' ');
        if (sp.font < 0) return px * 2.0f;
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&g_fonts[sp.font].info, sp.glyph, &adv, &lsb);
        return 4.0f * adv * stbtt_ScaleForMappingEmToPixels(&g_fonts[sp.font].info, px);
    }
    Resolved r = resolve(face, cp);
    if (resolvedOut) *resolvedOut = r;
    if (r.font < 0) return px * 0.5f;
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&g_fonts[r.font].info, r.glyph, &adv, &lsb);
    return adv * stbtt_ScaleForMappingEmToPixels(&g_fonts[r.font].info, px);
}

const CachedGlyph& glyphBitmap(FontFace face, const Resolved& r, float px) {
    GlyphKey key{static_cast<int>(face), r.font, r.glyph, static_cast<int>(px * 4.0f + 0.5f)};
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    CachedGlyph cg;
    const stbtt_fontinfo* info = &g_fonts[r.font].info;
    float scale = stbtt_ScaleForMappingEmToPixels(info, px);
    unsigned char* bmp = stbtt_GetGlyphBitmap(info, scale, scale, r.glyph, &cg.w, &cg.h, &cg.xoff, &cg.yoff);
    if (bmp) {
        cg.cov.assign(bmp, bmp + static_cast<size_t>(cg.w) * cg.h);
        stbtt_FreeBitmap(bmp, nullptr);
    }
    if (g_cache.size() > 20000) g_cache.clear();
    return g_cache.emplace(key, std::move(cg)).first->second;
}

}  // namespace

bool loadFonts(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    bool sgM = loadOne(0, dir + "/SpaceGrotesk-Medium.ttf");
    bool sgB = loadOne(1, dir + "/SpaceGrotesk-Bold.ttf");
    bool inM = loadOne(2, dir + "/Inter-Medium.ttf");
    bool inR = loadOne(3, dir + "/Inter-Regular.ttf");
    bool jb = loadOne(4, dir + "/JetBrainsMono-Regular.ttf");
    g_chain[0] = {0, 2, 3};
    g_chain[1] = {1, 2, 3};
    g_chain[2] = {4, 3, 2};
    (void)sgM; (void)sgB; (void)jb;
    return sgM || sgB || inM || inR || jb;
}

float measureText(const std::string& s, float px, FontFace face) {
    std::lock_guard<std::mutex> lock(g_mutex);
    float w = 0;
    for (size_t i = 0; i < s.size();) w += advanceOf(face, nextCodepoint(s, i), px);
    return w;
}

float textAscent(float px, FontFace face) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int slot : g_chain[static_cast<int>(face)]) {
        if (!g_fonts[slot].ok) continue;
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&g_fonts[slot].info, &asc, &desc, &gap);
        return asc * stbtt_ScaleForMappingEmToPixels(&g_fonts[slot].info, px);
    }
    return px * 0.8f;
}

float textLineHeight(float px, FontFace) { return px * 1.3f; }

void drawText(Bitmap& bm, float x, float baseline, const std::string& s, float px, FontFace face, Color8 col) {
    std::lock_guard<std::mutex> lock(g_mutex);
    float pen = x;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = nextCodepoint(s, i);
        Resolved r;
        float adv = advanceOf(face, cp, px, &r);
        if (r.font >= 0 && cp > ' ') {
            const CachedGlyph& g = glyphBitmap(face, r, px);
            int gx = static_cast<int>(std::floor(pen + 0.5f)) + g.xoff;
            int gy = static_cast<int>(std::floor(baseline + 0.5f)) + g.yoff;
            for (int row = 0; row < g.h; ++row) {
                int py = gy + row;
                if (py < 0 || py >= bm.h) continue;
                for (int c = 0; c < g.w; ++c) {
                    int pxl = gx + c;
                    if (pxl < 0 || pxl >= bm.w) continue;
                    float sa = g.cov[static_cast<size_t>(row) * g.w + c] / 255.0f * (col.a / 255.0f);
                    if (sa <= 0.0f) continue;
                    uint8_t* d = &bm.rgba[(static_cast<size_t>(py) * bm.w + pxl) * 4];
                    float da = d[3] / 255.0f;
                    float oa = sa + da * (1.0f - sa);
                    d[0] = static_cast<uint8_t>((col.r * sa + d[0] * da * (1.0f - sa)) / oa + 0.5f);
                    d[1] = static_cast<uint8_t>((col.g * sa + d[1] * da * (1.0f - sa)) / oa + 0.5f);
                    d[2] = static_cast<uint8_t>((col.b * sa + d[2] * da * (1.0f - sa)) / oa + 0.5f);
                    d[3] = static_cast<uint8_t>(oa * 255.0f + 0.5f);
                }
            }
        }
        pen += adv;
    }
}

std::string ellipsize(const std::string& s, float px, FontFace face, float maxWidth) {
    if (measureText(s, px, face) <= maxWidth) return s;
    const std::string dots = "\xE2\x80\xA6";  // U+2026
    float limit = maxWidth - measureText(dots, px, face);
    std::string out;
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        uint32_t cp = nextCodepoint(s, i);
        float a;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            a = advanceOf(face, cp, px);
        }
        if (w + a > limit) break;
        w += a;
        out.append(s, start, i - start);
    }
    return out + dots;
}

std::vector<std::string> wrapText(const std::string& s, float px, FontFace face, float maxWidth, int maxLines) {
    std::vector<std::string> lines;
    std::string cur;
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t start = i;
        uint32_t cp = nextCodepoint(s, i);
        float a;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            a = advanceOf(face, cp, px);
        }
        if (w + a > maxWidth && !cur.empty()) {
            lines.push_back(cur);
            cur.clear();
            w = 0;
            if (static_cast<int>(lines.size()) == maxLines) {
                lines.back() = ellipsize(s.substr(0, 0) + lines.back() + s.substr(start), px, face, maxWidth);
                return lines;
            }
        }
        cur.append(s, start, i - start);
        w += a;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

Bitmap makeLabel(const std::string& s, float px, FontFace face, Color8 color, int height, int padX) {
    Bitmap bm;
    bm.h = height;
    bm.w = static_cast<int>(std::ceil(measureText(s, px, face))) + 2 * padX;
    bm.w = std::max(bm.w, 8);
    bm.rgba.assign(static_cast<size_t>(bm.w) * bm.h * 4, 0);
    float asc = textAscent(px, face);
    float baseline = (height - px * 1.0f) * 0.5f + asc * 0.98f;
    drawText(bm, static_cast<float>(padX), baseline, s, px, face, color);
    return bm;
}

void drawDoorOutline(Bitmap& bm) {
    const float inset = 26.0f, th = 6.0f, rt = 110.0f;
    const float hx = bm.w * 0.5f - inset, hy = bm.h * 0.5f - inset;
    for (int y = 0; y < bm.h; ++y)
        for (int x = 0; x < bm.w; ++x) {
            const float px = x + 0.5f - bm.w * 0.5f, py = y + 0.5f - bm.h * 0.5f;
            const float rr = py < 0.0f ? rt : 0.0f;
            const float qx = std::fabs(px) - (hx - rr), qy = std::fabs(py) - (hy - rr);
            const float dd = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f)) +
                             std::min(std::max(qx, qy), 0.0f) - rr;
            const float d = std::fabs(dd + th * 0.5f);
            const float core = std::clamp(th * 0.5f + 0.5f - d, 0.0f, 1.0f);
            const float halo = std::exp(-d / 7.0f) * 0.5f;
            const float a = std::max(core, halo * (1.0f - core));
            uint8_t* p = &bm.rgba[(static_cast<size_t>(y) * bm.w + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = static_cast<uint8_t>(std::min(a, 1.0f) * 255.0f);
        }
}
}  // namespace sf
